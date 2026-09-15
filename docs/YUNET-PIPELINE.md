# YuNet pipeline

This document defines the Milestone 3 production pipeline. The authoritative
constants are in `engine/vision/src/yunet.rs` and the native detector setup is
in `engine/vision-opencv-sys/native/yunet_bridge.cpp`.

## Input and conversion

The worker accepts exactly one bounded frame in `RGB8`, `RGBA8`, or `Gray8`
format. Width is `1..1920`, height is `1..1080`, and stride may contain padding
but must cover every packed row. Protocol parsing validates the exact payload
length and all multiplication and addition before preprocessing.

Rust converts the original frame into a tightly packed three-channel BGR
buffer:

- `RGB8`: `(R, G, B)` becomes `(B, G, R)`;
- `RGBA8`: alpha is discarded and `(R, G, B, A)` becomes `(B, G, R)`;
- `Gray8`: the value is copied into B, G, and R.

Row padding is ignored. If either dimension is below 64 pixels, Rust
zero-pads only the right or bottom of the BGR buffer to a 64-pixel minimum.
This avoids the degenerate single-feature-cell output observed at smaller
YuNet runtime sizes while preserving the protocol's existing `1x1` minimum.
The original frame is not mutated. There is no normalization, mean
subtraction, scale factor, alpha blending, or color-space heuristic.

## OpenCV inference

The bridge initializes `FaceDetectorYN` directly from the verified FP32 ONNX
bytes. It accepts OpenCV 4.8 or newer. Backend selection probes OpenVINO first,
then Vulkan when the worker sandbox is active, and finally the OpenCV CPU
backend. Any accelerator initialization or inference failure is retried on
CPU. The default OpenCV thread pool is bounded to two threads on small CPUs or
four otherwise.

Tracking analysis letterboxes the source frame into a 320×320 inference image
and maps validated coordinates back to the source dimensions. Identity
extraction first uses the source resolution directly and falls back to the
same 320×320 tracking path when full-resolution detection returns no face. Both
paths preserve the original-frame coordinates for the five landmarks and the
subsequent 112×112 SFace crop.

For full-resolution detection, the detector input size is set to the BGR
buffer size unless the documented 64-pixel minimum padding applies.

OpenCV's YuNet implementation creates its DNN blob with scale `1.0`, zero mean,
no channel swap, and no crop. Internally it pads only the right and bottom
edges to the next multiple of 32 using zeros. It does not resize or letterbox
the image. Consequently KFaceAuth applies no coordinate rescaling. Every
rectangle and landmark is still validated against the original, unpadded
frame; output in either padding region fails closed.

The fixed production parameters are:

| Parameter | Value |
|---|---:|
| score threshold | `0.70` |
| NMS threshold | `0.3` |
| OpenCV `topK` | `5000` |
| emitted face limit | `8` |
| backend/target | OpenVINO, Vulkan, or OpenCV CPU with runtime fallback |

OpenCV performs score filtering and non-maximum suppression. Thresholds are
finite, compiled constants; the benchmark and user configuration cannot
change them.

## Output validation

OpenCV must return a two-dimensional float matrix with exactly 15 columns per
row: rectangle `(x, y, width, height)`, five landmark pairs, and a score.
The bridge rejects another element type, shape, negative row count, or more
than 5,000 rows.

Rust then validates every returned row, including rows beyond the eight that
can be emitted:

- every value must be finite;
- width and height must be positive;
- the rectangle must remain inside the original image;
- all five landmarks must remain inside the original image;
- the score must satisfy the configured threshold within an explicit
  `1e-5` floating-point tolerance and must not exceed one beyond that tolerance.

Validated rectangles are conservatively rounded outward and converted to the
bounded integer result. The five landmarks are carried only in the private v2
vision response so the backend can calculate pose; detector scores never leave
the Rust worker. Neither landmarks nor scores are placed in QML, logs, or
support reports. At most eight faces are returned.

## Quality and result semantics

Brightness, contrast, and sharpness are calculated from the original accepted
pixel format, not from a detector tensor. Quality calculation is independent
of whether detection returns zero, one, or multiple faces. Quality flags are
neutral framing guidance and are checked for contradictory values before the
KCM displays them.

- zero faces means that this frame produced no detector result;
- one face means that one rectangle passed validation;
- multiple faces means that two to eight rectangles passed validation.

No result identifies a person. Detector score is not identity confidence,
liveness confidence, anti-spoof evidence, or authentication confidence.

## Upstream parity

The implementation follows the supported OpenCV `FaceDetectorYN` buffer-loading path,
input-size update, blob parameters, right/bottom multiple-of-32 padding, score
thresholding, and NMS. KFaceAuth's additional below-64 right/bottom padding is
explicitly tested at the smallest accepted frame. Native tests exercise model
loading, no-face inference, input immutability, bounds, and malformed output
shape. Numeric postprocessing tests use the explicit tolerance above rather
than byte equality.

No redistributable positive face photograph is included. Real-provider
automation therefore covers verified loading, preprocessing, invalid input,
postprocessing, and a synthetic black zero-face frame. Positive detections
remain covered through the deterministic test provider until a project-owned,
synthetic, or clearly licensed fixture is reviewed.
