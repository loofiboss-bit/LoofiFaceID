# Model selection

Status: Milestone 4 detector inventory, reviewed 2026-07-29.

This document treats source-code and model-weight licensing as separate
questions. A repository license is not used as evidence for a weight unless the
upstream project explicitly applies it to the model file or its directory.

## Decision

KFaceAuth selects and packages the OpenCV Zoo **YuNet FP32
`face_detection_yunet_2023mar.onnx`** detector for reproducible evaluation.
The selected artifact is:

- upstream repository: <https://github.com/opencv/opencv_zoo>;
- immutable upstream revision:
  `47534e27c9851bb1128ccc0102f1145e27f23f98`;
- upstream path:
  `models/face_detection_yunet/face_detection_yunet_2023mar.onnx`;
- installed filename: `face_detection_yunet_2023mar.onnx`;
- size: 232,589 bytes;
- SHA-256:
  `8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4`;
- model-weight license: MIT. The immutable upstream
  [model README](https://github.com/opencv/opencv_zoo/blob/47534e27c9851bb1128ccc0102f1145e27f23f98/models/face_detection_yunet/README.md)
  explicitly says that all files in the directory use the
  [directory MIT license](https://github.com/opencv/opencv_zoo/blob/47534e27c9851bb1128ccc0102f1145e27f23f98/models/face_detection_yunet/LICENSE);
- reference implementation/runtime code license: Apache-2.0 for OpenCV and
  OpenCV Zoo code outside the model's separately licensed directory.

MIT permits use, modification, redistribution, sublicensing, and sale provided
the copyright and permission notice are retained. MIT is compatible with this
GPL-3.0-or-later package. The upstream MIT text and immutable provenance record
are shipped beside the weight.

YuNet is a detector only. It is not an embedding model and cannot identify a
person. OpenCV's `FaceDetectorYN` interface accepts an image at a configured
input size and returns an `N x 15` floating-point matrix containing a rectangle,
five landmarks, and a detector score. The selected fixed-shape model is intended
for OpenCV 4.x; input size must be set to the actual bounded frame size. Upstream
reports WIDER Face validation AP and describes detectable face sizes of roughly
10x10 to 300x300 pixels, but supplies no Fedora-specific CPU latency guarantee.

The 227 KiB detector is preferred over the alternatives because it has an
explicit weight license, a small package footprint, a conventional ONNX format,
CPU support in Fedora's OpenCV stack, and output that maps directly to bounded
face-presence results. The FP32 artifact is preferred over the smaller
quantized variant until target-hardware accuracy has been measured.

Milestone 3 enables the verified weight through OpenCV >=4.8 (Fedora baseline:
4.13) and a narrow
reviewed C ABI bridge. There is no silent fallback: production always attempts
the real provider, and any inventory or runtime failure is a stable error.
Deterministic inference is compiled only for Rust tests and injected fake worker
tests.

## Detector candidates

### OpenCV Zoo YuNet FP32 — selected

- Role: face detection, rectangles, five optional landmarks, detector score.
- Format: ONNX FP32, fixed input height and width; OpenCV output is `N x 15`
  float32.
- Size and CPU: 232,589 bytes; OpenCV documents CPU execution, but target
  latency remains unmeasured while the real backend is disabled.
- Modification/redistribution: permitted under MIT with the license notice.
- Limitations: detector score is not identity confidence, liveness, or
  anti-spoof evidence. Performance varies with face scale, pose, occlusion,
  lighting, camera spectrum, and the input domain. Upstream benchmark results do
  not establish authentication suitability or demographic parity.
- Security implication: detector false positives and false negatives must
  produce neutral guidance only. They must never become an authentication
  decision.

### OpenCV Zoo YuNet INT8 — rejected for Milestone 3

- Exact source: the quantized YuNet artifacts in the same immutable model
  directory and revision.
- Weight license: MIT, explicitly applied to every file in the directory.
- Format/role: quantized ONNX detector with the same face-presence role.
- Expected resource use: approximately 100 KiB for the reviewed INT8 artifact,
  with lower storage and potentially lower CPU cost.
- Rejection reason: upstream provides the artifact but no primary
  target-platform accuracy or latency evidence sufficient to justify changing
  numerical behavior. FP32 is small enough and is the less ambiguous baseline.

### MediaPipe BlazeFace short-range v1 — compatible but rejected

- Exact source: Google's
  [BlazeFace short-range model card](https://storage.googleapis.com/mediapipe-assets/MediaPipe%20BlazeFace%20Model%20Card%20%28Short%20Range%29.pdf)
  and
  [versioned TFLite artifact](https://storage.googleapis.com/mediapipe-models/face_detector/blaze_face_short_range/float16/1/blaze_face_short_range.tflite).
- Artifact: version `1`, 229,746 bytes; the object generation recorded during
  review was `1682480001338381`.
- Weight license: Apache-2.0 is stated by the model card for the model.
- Code license: MediaPipe code is Apache-2.0.
- Modification/redistribution: Apache-2.0 permits modification and
  redistribution with its license, notices, change marking, and patent terms.
- Input/output: RGB `128 x 128 x 3` float values in `[-1, 1]`; anchor
  transformations require decoding and non-maximum suppression. The task API
  produces rectangles and six keypoints.
- Size and CPU: about 230 KiB. The model card reports mobile/XNNPACK
  performance, not Fedora desktop qualification.
- Limitations: short-range, prominently displayed faces; degrades with pose,
  scale, lighting, noise, motion, and overlap. Surveillance and identity
  recognition are explicitly out of scope.
- Rejection reason: it adds a separate LiteRT/MediaPipe preprocessing and
  packaging surface without a benefit over YuNet for the current Fedora/OpenCV
  target.

## Embedding model

### OpenCV Zoo SFace FP32 — selected in Milestone 4

- Exact source:
  `face_recognition_sface_2021dec.onnx` at OpenCV Zoo revision
  `47534e27c9851bb1128ccc0102f1145e27f23f98`.
- Actual Git LFS object size: 38,696,353 bytes.
- Actual SHA-256:
  `0ba9fbfa01b5270c96627c4ef784da859931e02f04419c829e83484087c34e79`.
- Weight license: Apache-2.0 is applied explicitly by the
  [model-directory license](https://github.com/opencv/opencv_zoo/blob/47534e27c9851bb1128ccc0102f1145e27f23f98/models/face_recognition_sface/LICENSE).
- Code license: OpenCV `FaceRecognizerSF` code is Apache-2.0.
- Modification/redistribution: permitted under Apache-2.0 with its license and
  notice requirements.
- Input/output: an aligned face crop is converted to an embedding vector;
  OpenCV exposes cosine and L2 comparison helpers.
- Decision: selected as the conservative FP32 baseline. Quantized variants are
  smaller but lack representative KFaceAuth parity evidence.
- Limits: the current threshold is provisional and local-only. FAR, FRR, bias,
  spoofing, liveness, and authentication suitability remain unqualified.

The full FP32/quantized, licensing, alignment, preprocessing, accuracy, and
limitations record is
[EMBEDDING-MODEL-SELECTION.md](EMBEDDING-MODEL-SELECTION.md).

### InsightFace ArcFace/Buffalo weights — rejected

- Upstream source:
  <https://github.com/deepinsight/insightface>.
- Code license: MIT.
- Model-weight terms: upstream states that training data and models trained
  with it, including manually and automatically downloaded models, are for
  non-commercial research only and directs users to request separate licensing.
- Rejection reason: those restrictions are not compatible with unrestricted
  Fedora/COPR redistribution. The MIT code license does not license the model
  weights.

## Remaining qualification

Before local identity can support liveness or authentication work, the project
still needs:

1. a verified, redistributable positive fixture for automated real detections;
2. real-camera latency and error behavior across the supported RGB/IR matrix;
3. pose, scale, occlusion, lighting, glasses, and ordinary appearance
   qualification;
4. representative FAR/FRR and bias evaluation for the documented alignment and
   normalized embedding contract;
5. independent privacy, threshold, template-security, and spoof review.

Milestone 4 local matching is not liveness, anti-spoofing, or authentication
qualification.
