# Embedding model selection

Status: Milestone 4 production decision, reviewed 2026-07-29.

## Decision

KFaceAuth selects the FP32 OpenCV Zoo SFace weight
`face_recognition_sface_2021dec.onnx`.

| Property | Selected value |
|---|---|
| Repository | `https://github.com/opencv/opencv_zoo` |
| Immutable revision | `47534e27c9851bb1128ccc0102f1145e27f23f98` |
| Upstream path | `models/face_recognition_sface/face_recognition_sface_2021dec.onnx` |
| Actual weight size | 38,696,353 bytes |
| Actual weight SHA-256 | `0ba9fbfa01b5270c96627c4ef784da859931e02f04419c829e83484087c34e79` |
| Weight license | Apache-2.0, from the immutable model-directory `LICENSE` |
| OpenCV/bridge code license | Apache-2.0 / GPL-3.0-or-later |
| Output | `1 x 128`, `CV_32FC1` |
| Stored representation | 128 little-endian FP32 values after deterministic L2 normalization |

The reviewed artifact is the actual Git LFS object, not the 133-byte pointer.
The model, the directory license, and immutable provenance are in the closed
inventory and are verified offline before OpenCV is initialized.

## FP32 and quantized variants

The immutable upstream directory also contains INT8 variants. During this
review their actual artifacts were inspected separately:

| Variant | Size | SHA-256 | Decision |
|---|---:|---|---|
| FP32 | 38,696,353 | `0ba9fbfa01b5270c96627c4ef784da859931e02f04419c829e83484087c34e79` | Selected |
| INT8 | 9,896,933 | `2b0e941e...d78a` | Rejected |
| INT8 block-quantized | 10,667,852 | `fb143eea...74ee` | Rejected |

The quantized files reduce package size substantially, but no representative
Fedora 44 parity set was available to measure embedding consistency, error
rates, demographic behavior, or threshold movement. Storage savings alone do
not justify changing identity output. FP32 is therefore the conservative,
reproducible baseline. The abbreviated rejected hashes are evaluation notes,
not package inputs; only the selected artifact is shipped.

## Alignment and preprocessing

SFace receives the five landmarks from the single selected YuNet row in this
order: right eye, left eye, nose tip, right mouth corner, left mouth corner.
OpenCV `FaceRecognizerSF::alignCrop` applies the SFace reference similarity
transformation and produces a 112×112 BGR `CV_8UC3` crop. The reviewed Fedora
The supported OpenCV `feature` operation (Fedora baseline 4.13) then produces
exactly 128 finite FP32 values.
Rust rejects an unexpected shape, type, range, or zero norm and performs the
only stored normalization.

The complete contract is in [IDENTITY-PIPELINE.md](IDENTITY-PIPELINE.md).

## Accuracy evidence and limitations

OpenCV Zoo publishes benchmark results for the upstream model and example
comparison thresholds. Those results are useful provenance, but they do not
qualify this product. They do not represent KFaceAuth's camera pipeline,
enrollment aggregation, Fedora CPU implementation, user population, lighting,
pose, accessibility needs, or attack model.

KFaceAuth therefore uses a centrally owned provisional cosine threshold of
`0.45`, a `0.04` ambiguity margin, and median aggregation across enrolled
samples. These values are engineering defaults for an experimental local
comparison only. They were not derived from representative KFaceAuth FAR/FRR
evidence and are not user-adjustable.

FAR, FRR, demographic parity, bias behavior, spoof resistance, infrared
behavior, and presentation-attack resistance remain unqualified. Upstream
example thresholds are not accepted as authentication or release evidence.

## CPU latency and memory

The repository includes a non-installed aggregate evaluation tool and a manual
hardware procedure. Build-environment measurements are recorded separately
from hardware qualification. No representative positive fixture or labelled
participant dataset is committed, so detector-plus-alignment-plus-embedding
latency and FAR/FRR remain unmeasured until an explicitly supplied,
permissioned dataset is used.

The FP32 weight adds 38,696,353 bytes of model input before OpenCV runtime
allocations. Peak resident memory and cold/warm latency must be reported from
the target Fedora 44 system; package size is not a proxy for runtime memory.

A 2026-07-29 build-host run on Fedora 44 x86_64, OpenCV 4.13.0, and an
Intel Core i5-1145G7 supplied four generated black PPM frames. All four
correctly ended at `no-face`, so these numbers cover model loading and the
detector rejection path, not positive alignment/SFace extraction:

| Aggregate | Measurement |
| --- | ---: |
| model initialization | 708.703 ms |
| fresh-provider pipeline median / p95 / worst | 660.400 / 702.520 / 702.520 ms |
| reused-provider pipeline median / p95 / worst | 12.238 / 24.198 / 24.198 ms |
| first evaluator worker process | 800.521 ms |
| subsequent fresh worker median / p95 / worst | 749.550 / 784.795 / 784.795 ms |
| parent / worker peak resident memory | 274,488 / 207,496 KiB |

The first process is one observed cold-start approximation; subsequent
processes benefit from the operating-system page cache. These measurements
are not camera, positive-identity, responsiveness, demographic, or release
qualification.

## License separation

The model weight is redistributed under the Apache-2.0 license applied by the
immutable SFace model directory. OpenCV and OpenCV Zoo reference code are also
Apache-2.0. KFaceAuth project code remains GPL-3.0-or-later. The package ships
the complete SFace Apache-2.0 text and immutable provenance beside the model.
