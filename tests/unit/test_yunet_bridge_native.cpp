// SPDX-License-Identifier: GPL-3.0-or-later

#include "yunet_bridge.h"
#include "yunet_output_validation.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <vector>

namespace
{
constexpr int Width = 64;
constexpr int Height = 64;
constexpr size_t Stride = Width * 3;
constexpr size_t MaximumDetections = 5000;

std::vector<uint8_t> readModel(const char *path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
} // namespace

int main()
{
    if (!KFaceAuthYuNet::validOutputShape(2, 5, 5, 1, 15) || KFaceAuthYuNet::validOutputShape(3, 5, 5, 1, 15) ||
        KFaceAuthYuNet::validOutputShape(2, 4, 5, 1, 15) || KFaceAuthYuNet::validOutputShape(2, 5, 5, -1, 15) ||
        KFaceAuthYuNet::validOutputShape(2, 5, 5, 1, 14))
        return 1;

    if (kfaceauth_yunet_disable_core_dumps() != KFACEAUTH_YUNET_OK)
        return 2;
    unsetenv("KFACEAUTH_INFERENCE_BACKEND");
    unsetenv("KFACEAUTH_ACCELERATION");
    if (kfaceauth_yunet_set_thread_count(0) != KFACEAUTH_YUNET_INVALID_ARGUMENT ||
        kfaceauth_yunet_set_thread_count(17) != KFACEAUTH_YUNET_INVALID_ARGUMENT ||
        kfaceauth_yunet_set_thread_count(1) != KFACEAUTH_YUNET_OK || kfaceauth_yunet_thread_count() != 1)
        return 3;

    auto model = readModel(KFACEAUTH_YUNET_MODEL_PATH);
    if (model.size() != 232589)
        return 4;
    void *detector = nullptr;
    setenv("KFACEAUTH_INFERENCE_BACKEND", "openvino", 1);
    setenv("KFACEAUTH_TEST_FAIL_OPENVINO", "1", 1);
    if (kfaceauth_yunet_create(model.data(), model.size(), Width, Height, 0.7F, 0.3F, 5000, &detector) !=
            KFACEAUTH_YUNET_OK ||
        !detector || kfaceauth_yunet_backend(detector) != KFACEAUTH_YUNET_BACKEND_CPU)
    {
        kfaceauth_yunet_destroy(detector);
        return 5;
    }
    kfaceauth_yunet_destroy(detector);
    detector = nullptr;
    unsetenv("KFACEAUTH_INFERENCE_BACKEND");
    unsetenv("KFACEAUTH_TEST_FAIL_OPENVINO");
    setenv("KFACEAUTH_INFERENCE_BACKEND", "vulkan", 1);
    setenv("KFACEAUTH_TEST_FAIL_VULKAN", "1", 1);
    if (kfaceauth_yunet_create(model.data(), model.size(), Width, Height, 0.7F, 0.3F, 5000, &detector) !=
            KFACEAUTH_YUNET_OK ||
        !detector || kfaceauth_yunet_backend(detector) != KFACEAUTH_YUNET_BACKEND_CPU)
    {
        kfaceauth_yunet_destroy(detector);
        return 6;
    }
    kfaceauth_yunet_destroy(detector);
    detector = nullptr;
    unsetenv("KFACEAUTH_INFERENCE_BACKEND");
    unsetenv("KFACEAUTH_TEST_FAIL_VULKAN");
    if (kfaceauth_yunet_create(model.data(), model.size() - 1, Width, Height, 0.7F, 0.3F, 5000, &detector) !=
            KFACEAUTH_YUNET_INVALID_ARGUMENT ||
        detector)
        return 7;
    if (kfaceauth_yunet_create(model.data(), model.size(), Width, Height, 0.7F, 0.3F, 5000, &detector) !=
            KFACEAUTH_YUNET_OK ||
        !detector)
        return 8;

    std::vector<uint8_t> frame(Stride * Height, 0);
    const auto original = frame;
    std::vector<KFaceAuthYuNetDetection> detections(MaximumDetections);
    size_t count = 0;
    if (kfaceauth_yunet_detect(detector, frame.data(), frame.size(), Width, Height, Stride - 1, detections.data(),
                               detections.size(), &count) != KFACEAUTH_YUNET_INVALID_ARGUMENT)
    {
        kfaceauth_yunet_destroy(detector);
        return 9;
    }
    if (kfaceauth_yunet_detect(detector, frame.data(), frame.size(), Width, Height, Stride, detections.data(),
                               detections.size(), &count) != KFACEAUTH_YUNET_OK ||
        count > detections.size() || frame != original)
    {
        kfaceauth_yunet_destroy(detector);
        return 10;
    }

    std::vector<uint8_t> highResolution(1920 * 1080 * 3, 0);
    if (kfaceauth_yunet_detect_scaled(detector, highResolution.data(), highResolution.size(), 1920, 1080, 1920 * 3, 320,
                                      320, detections.data(), detections.size(), &count) != KFACEAUTH_YUNET_OK ||
        kfaceauth_yunet_detect_scaled(detector, frame.data(), frame.size(), 1921, Height, Stride, 320, 320,
                                      detections.data(), detections.size(), &count) != KFACEAUTH_YUNET_INVALID_ARGUMENT)
    {
        kfaceauth_yunet_destroy(detector);
        return 11;
    }

    std::fill(model.begin(), model.end(), uint8_t{0});
    std::fill(frame.begin(), frame.end(), uint8_t{0});
    std::fill(detections.begin(), detections.end(), KFaceAuthYuNetDetection{});
    kfaceauth_yunet_destroy(detector);

    auto sfaceModel = readModel(KFACEAUTH_SFACE_MODEL_PATH);
    if (sfaceModel.size() != 38696353)
        return 12;
    void *recognizer = nullptr;
    if (kfaceauth_sface_create(sfaceModel.data(), sfaceModel.size() - 1, &recognizer) !=
            KFACEAUTH_YUNET_INVALID_ARGUMENT ||
        recognizer)
        return 13;
    if (kfaceauth_sface_create(sfaceModel.data(), sfaceModel.size(), &recognizer) != KFACEAUTH_YUNET_OK || !recognizer)
        return 14;

    constexpr int FaceWidth = 112;
    constexpr int FaceHeight = 112;
    constexpr size_t FaceStride = FaceWidth * 3;
    std::vector<uint8_t> alignedInput(FaceStride * FaceHeight, 0);
    const auto alignedInputOriginal = alignedInput;
    KFaceAuthYuNetDetection face{{0.0F, 0.0F, 112.0F, 112.0F, 38.2946F, 51.6963F, 73.5318F, 51.5014F, 56.0252F,
                                  71.7366F, 41.5493F, 92.3655F, 70.7299F, 92.2041F, 1.0F}};
    std::vector<float> embedding(KFACEAUTH_SFACE_EMBEDDING_DIMENSION, 0.0F);
    size_t embeddingCount = 0;
    if (kfaceauth_sface_extract(recognizer, alignedInput.data(), alignedInput.size(), FaceWidth, FaceHeight, FaceStride,
                                &face, embedding.data(), embedding.size(), &embeddingCount) != KFACEAUTH_YUNET_OK ||
        embeddingCount != KFACEAUTH_SFACE_EMBEDDING_DIMENSION || alignedInput != alignedInputOriginal ||
        !std::all_of(embedding.begin(), embedding.end(), [](float value) { return std::isfinite(value); }))
    {
        kfaceauth_sface_destroy(recognizer);
        return 15;
    }

    double similarity = 0.0;
    if (kfaceauth_sface_cosine(recognizer, embedding.data(), embedding.size(), embedding.data(), embedding.size(),
                               &similarity) != KFACEAUTH_YUNET_OK ||
        !std::isfinite(similarity) || std::abs(similarity - 1.0) > 1.0e-5)
    {
        kfaceauth_sface_destroy(recognizer);
        return 16;
    }

    face.values[4] = std::numeric_limits<float>::quiet_NaN();
    embeddingCount = 99;
    if (kfaceauth_sface_extract(recognizer, alignedInput.data(), alignedInput.size(), FaceWidth, FaceHeight, FaceStride,
                                &face, embedding.data(), embedding.size(),
                                &embeddingCount) != KFACEAUTH_YUNET_INVALID_ARGUMENT)
    {
        kfaceauth_sface_destroy(recognizer);
        return 17;
    }
    std::fill(sfaceModel.begin(), sfaceModel.end(), uint8_t{0});
    std::fill(alignedInput.begin(), alignedInput.end(), uint8_t{0});
    std::fill(embedding.begin(), embedding.end(), 0.0F);
    kfaceauth_sface_destroy(recognizer);
    return 0;
}
