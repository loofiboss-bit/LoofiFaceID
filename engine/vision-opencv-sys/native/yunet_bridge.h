// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    enum KFaceAuthYuNetStatus
    {
        KFACEAUTH_YUNET_OK = 0,
        KFACEAUTH_YUNET_INVALID_ARGUMENT = 1,
        KFACEAUTH_YUNET_RUNTIME_FAILURE = 2,
        KFACEAUTH_YUNET_OUTPUT_TOO_LARGE = 3,
        KFACEAUTH_YUNET_MALFORMED_OUTPUT = 4,
        KFACEAUTH_YUNET_HARDENING_FAILURE = 5,
    };

    enum KFaceAuthYuNetBackend
    {
        KFACEAUTH_YUNET_BACKEND_CPU = 0,
        KFACEAUTH_YUNET_BACKEND_OPENVINO = 1,
        KFACEAUTH_YUNET_BACKEND_VULKAN = 2,
    };

    enum KFaceAuthYuNetSandboxStatus
    {
        KFACEAUTH_YUNET_SANDBOX_UNAVAILABLE = 0,
        KFACEAUTH_YUNET_SANDBOX_APPLIED = 1,
        KFACEAUTH_YUNET_SANDBOX_ALREADY_APPLIED = 2,
        KFACEAUTH_YUNET_SANDBOX_FAILURE = 3,
    };

    typedef struct KFaceAuthYuNetDetection
    {
        float values[15];
    } KFaceAuthYuNetDetection;

    enum
    {
        KFACEAUTH_SFACE_EMBEDDING_DIMENSION = 128,
        KFACEAUTH_SFACE_ALIGNED_WIDTH = 112,
        KFACEAUTH_SFACE_ALIGNED_HEIGHT = 112,
    };

    int kfaceauth_yunet_disable_core_dumps(void);
    const char *kfaceauth_yunet_opencv_version(void);

    int kfaceauth_yunet_set_thread_count(int32_t thread_count);
    int32_t kfaceauth_yunet_thread_count(void);
    int kfaceauth_yunet_configure_worker_sandbox(const char *model_root, const char *writable_root);
    int kfaceauth_yunet_install_seccomp(int allow_drm_ioctl);
    int kfaceauth_yunet_backend(void *engine);
    const char *kfaceauth_yunet_backend_name(int backend);

    int kfaceauth_yunet_create(const uint8_t *model_bytes, size_t model_size, int32_t width, int32_t height,
                               float score_threshold, float nms_threshold, int32_t top_k, void **detector_out);

    int kfaceauth_yunet_detect(void *detector, const uint8_t *bgr_bytes, size_t bgr_size, int32_t width, int32_t height,
                               size_t stride, KFaceAuthYuNetDetection *detections, size_t detection_capacity,
                               size_t *detection_count);

    int kfaceauth_yunet_detect_scaled(void *detector, const uint8_t *bgr_bytes, size_t bgr_size, int32_t width,
                                      int32_t height, size_t stride, int32_t inference_width, int32_t inference_height,
                                      KFaceAuthYuNetDetection *detections, size_t detection_capacity,
                                      size_t *detection_count);

    void kfaceauth_yunet_destroy(void *detector);

    int kfaceauth_sface_create(const uint8_t *model_bytes, size_t model_size, void **recognizer_out);

    int kfaceauth_sface_extract(void *recognizer, const uint8_t *bgr_bytes, size_t bgr_size, int32_t width,
                                int32_t height, size_t stride, const KFaceAuthYuNetDetection *detection,
                                float *embedding, size_t embedding_capacity, size_t *embedding_count);

    int kfaceauth_sface_cosine(void *recognizer, const float *left, size_t left_count, const float *right,
                               size_t right_count, double *similarity);

    int kfaceauth_sface_backend(void *recognizer);

    void kfaceauth_sface_destroy(void *recognizer);

#ifdef __cplusplus
}
#endif
