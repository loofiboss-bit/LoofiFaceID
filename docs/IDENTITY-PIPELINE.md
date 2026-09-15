# Identity pipeline contract

This is the authoritative Milestone 4 detector, alignment, embedding, and
comparison contract. It produces an experimental in-session result, never an
operating-system authorization decision.

## Versioned identities

- Detector: `yunet-2023mar-v1`
- Embedding model: `sface-2021dec-fp32-v1`
- SFace SHA-256:
  `0ba9fbfa01b5270c96627c4ef784da859931e02f04419c829e83484087c34e79`
- Embedding format: `sface-f32-le-128-l2-v1`
- Normalization version: `1`
- Dimension: `128`

A profile is invalid for the active runtime if any identity, hash, dimension,
format, or normalization version differs.

## Processing sequence

1. Rust validates the bounded RGB8, RGBA8, or Gray8 frame and converts it into
   a fresh packed BGR buffer.
2. The YuNet provider runs full-resolution detection first and uses the same
   320×320 letterbox/tracking path as live guidance when no face is returned.
   Both paths return original-frame coordinates and bounded raw `1 x 15` FP32
   rows.
3. Rust validates every rectangle, score, and landmark and requires exactly
   one face for enrollment and verification.
4. Quality flags must be clear. The face rectangle must be at least 80×80
   pixels and remain at least four pixels inside the image.
5. The selected row supplies, in OpenCV YuNet order: right eye, left eye, nose
   tip, right mouth corner, and left mouth corner.
6. The C++ bridge copies the frame and row into owned OpenCV matrices.
   `FaceRecognizerSF::alignCrop` applies OpenCV's SFace reference similarity
   transformation and must return exactly 112×112 BGR `CV_8UC3`.
7. `FaceRecognizerSF::feature` must return one continuous `1 x 128`
   `CV_32FC1` feature. The C ABI copies exactly those 128 values into a
   caller-owned fixed array.
8. Rust treats the array as hostile: every value must be finite and within
   `[-32, 32]`; its squared L2 norm must be finite and greater than `1e-12`.
9. Rust accumulates the norm in FP64, divides each element by it, stores FP32,
   and verifies the resulting L2 norm within `1e-5` of one.
10. Cosine is the FP64 dot product of two validated unit vectors, clamped to
    `[-1, 1]`. Native OpenCV cosine exists only as a qualification cross-check.

No OpenCV object, pointer, matrix, or unbounded vector crosses the safe Rust
API. C++ exceptions are caught and converted to stable bridge errors.

## Matching policy

The vault owns one profile with 3–8 normalized samples. Verification compares
one candidate with every sample, sorts the finite cosine values, and uses the
median (the mean of the two central values for an even count).

- median `>= 0.45`: `Match`
- median `>= 0.41` and `< 0.45`: `Ambiguous`
- median `< 0.41`: `No match`

The `0.45` threshold and `0.04` ambiguity margin are provisional,
centrally-owned experimental policy. They are not configurable and are not an
authentication threshold. No similarity value leaves the Rust identity/vault
boundary.

## Fail-closed conditions

Extraction returns no embedding for:

- zero or multiple faces;
- quality, face-size, edge, landmark, rectangle, or detector-score failure;
- non-finite or malformed native output;
- unexpected crop/feature dimensions or element type;
- zero-length, zero-norm, non-finite, or out-of-bound embeddings;
- model-inventory or detector/embedding identity mismatch;
- cancellation or deadline expiry before or after native inference;
- OpenCV exception, process failure, or protocol violation.

Frames, landmarks, raw features, normalized embeddings, and scores are cleared
where practical and never enter QML, normal logs, CLI output, support reports,
or temporary files.

## Evidence boundary

The exact alignment and numeric shape are executable contracts. Accuracy,
bias, FAR, FRR, liveness, and spoof resistance are not. A real labelled and
permissioned evaluation set is required before any error-rate claim.
