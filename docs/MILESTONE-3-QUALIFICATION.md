# Milestone 3 qualification

This document separates implemented code paths from hardware- and
data-dependent qualification. It is intentionally conservative: a fallback
test or a successful build does not prove acceleration latency, numerical
parity, or vendor coverage.

## Implemented paths

| Task | Implementation evidence | Qualification status |
|---|---|---|
| 3.1 concurrency | `cv::setNumThreads`, default 2/4 policy, reviewed 1..16 override, benchmark v2 thread sweep at 1/2/4/8/16 | Code and schema verified; performance result is hardware-dependent |
| 3.2 OpenVINO | optional CMake detection, OpenCV inference-engine candidate, runtime CPU fallback | OpenVINO runtime not present on the current host |
| 3.3 Vulkan/sandbox | VKCOM candidate, Landlock model/ICD/DRM rules, identity-worker XDG data subtree, opt-in seccomp DRM ioctl filter | Current host has Vulkan loader/ICD files but no Landlock securityfs; vendor execution is unverified |
| 3.4 dual resolution | 320×320 letterboxed tracking and source-resolution identity detection, bounded at 1920×1080 | Automated zero-face and bounds coverage verified |
| 3.5 INT8 audit | `tools/audit_quantization.py` consumes aggregate-only JSONL vectors and reports model digests, cosine divergence, FAR/FRR drift | Not run: no INT8 artifact or permissioned LFW/IJB-C vectors are shipped |
| 3.6 portability | OpenCV >=4.8 build check, 720p/1080p bounds, aspect-preserving scaling, accelerator fallback | OpenCV 4.13 build verified; other minor releases remain unverified |
| 3.7 memory hygiene | best-effort `mlock2(MLOCK_ONFAULT)`, strict opt-in mode, native matrix/model zeroization on all return paths | Code path and native tests verified; graph-internal activation reset is not exposed by the reviewed OpenCV API |

## Reproducible checks

Run the benchmark from a built workspace with an absolute model root:

```bash
cargo run --manifest-path engine/Cargo.toml --offline \
  --bin kfaceauth-yunet-benchmark -- \
  --model-root "$PWD/models" --width 320 --height 320 \
  --warm-up 3 --iterations 20 --thread-sweep
```

The JSON output records the selected backend, OpenCV version, thread count,
resolution, cold start, median, p95, worst latency, and optional thread
scaling. It is evidence for the local build environment only.

The quantization audit is aggregate-only and never emits individual pair
scores or identifiers:

```bash
python3 tools/audit_quantization.py \
  --fp32-model /path/to/sface-fp32.onnx \
  --int8-model /path/to/sface-int8.onnx \
  --pairs /path/to/lfw-ijbc-pairs.jsonl \
  --fp32-embeddings /path/to/fp32-embeddings.jsonl \
  --int8-embeddings /path/to/int8-embeddings.jsonl
```

Embedding JSONL rows contain `{"id":"...","embedding":[128 finite
numbers]}`. Pair JSONL rows contain `{"dataset":"LFW|IJB-C","left":"...",
"right":"...","same":true|false}`. Both LFW and IJB-C must be present for
the report status to become `complete`. Without all three benchmark inputs the
tool returns `not-run` or a stable input error.

## Acceptance gates

The following remain open until measured on the named qualification hardware
and vectors:

- SFace accelerated latency at or below 4 ms;
- accelerated-to-FP32 cosine divergence below `1e-4` across all qualification
  vectors;
- Intel Xe, AMD RDNA, and NVIDIA Vulkan shader/memory qualification;
- OpenVINO AVX-512 VNNI qualification;
- LFW/IJB-C false-match and false-reject drift;
- OpenCV 4.8 through 4.12 build/test matrix.

The current implementation test run therefore reports these as `unverified`,
not as passed gates.
