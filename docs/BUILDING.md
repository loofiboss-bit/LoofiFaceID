# Building and testing

## Fedora 44 dependencies

```bash
sudo dnf install \
  cargo clang-tools-extra cmake extra-cmake-modules gcc-c++ ninja-build rust \
  kf6-kcmutils-devel kf6-kcoreaddons-devel kf6-ki18n-devel \
  kf6-kirigami-devel kf6-kwallet-devel \
  qt6-qtbase-devel qt6-qtdeclarative-devel qt6-qtmultimedia-devel \
  opencv-devel openssl-devel systemd-devel
```

KFaceAuth builds against OpenCV 4.8 or newer and is release-baselined on
Fedora OpenCV 4.13, OpenSSL 3, and KWallet. OpenVINO is optional: when the
OpenCV build exposes its inference-engine backend, the worker probes it at
runtime and falls back to CPU if it is unavailable. Vulkan is also optional
and is enabled only after the worker sandbox is applied. Neither accelerator
runtime is bundled. `systemd-devel` supplies libudev headers only; no systemd
unit or runtime service is added.

## Full local gates

```bash
cmake --fresh -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
python3 -m unittest discover -s tests -p 'test_*.py'

cargo fmt --manifest-path engine/Cargo.toml --all -- --check
cargo clippy --manifest-path engine/Cargo.toml \
  --workspace --all-targets --locked --offline -- -D warnings
cargo test --manifest-path engine/Cargo.toml \
  --workspace --all-targets --locked --offline

/usr/lib64/qt6/bin/qmllint src/kcm/ui/*.qml src/kcm/ui/components/*.qml
msgfmt --check -o /dev/null po/sv/kcm_kfaceauth.po
find src tests/unit engine/vision-opencv-sys/native \
  engine/crypto-openssl-sys/native \
  -type f \( -name '*.cpp' -o -name '*.h' \) -print0 \
  | xargs -0 clang-format --dry-run --Werror
python3 tools/verify_models.py --root models
python3 tools/audit_quantization.py --help
git diff --check
```

Cargo has no registry dependencies. Model weights are present in the complete
prepared source set. All Cargo operations use `--locked --offline`; no
configure, build, test, install, or runtime step downloads data.

## Milestone 3 benchmarking and sandbox controls

The benchmark emits schema `kfaceauth-yunet-benchmark-v2`, including the
selected backend, OpenCV thread count, and optional 1/2/4/8/16 thread sweep.
Use an absolute model path:

```bash
cargo run --manifest-path engine/Cargo.toml --offline \
  --bin kfaceauth-yunet-benchmark -- \
  --model-root "$PWD/models" --width 320 --height 320 \
  --warm-up 3 --iterations 20 --thread-sweep
```

The worker defaults to two OpenCV threads on systems with at most four CPUs
and four otherwise. `--threads N` or `KFACEAUTH_OPENCV_THREADS=N` may select a
reviewed value from 1 through 16. `KFACEAUTH_INFERENCE_BACKEND=cpu|openvino|vulkan|auto`
controls selection; the default is `auto`. For the vision worker,
`KFACEAUTH_ENABLE_SECCOMP_SANDBOX=1` enables the opt-in syscall filter after
provider initialization, while
`KFACEAUTH_REQUIRE_MEMLOCK=1` turns best-effort `mlock2(MLOCK_ONFAULT)` into a
hardening requirement.

The INT8 audit is offline and aggregate-only. It requires permissioned JSONL
embeddings and pair labels when data-dependent qualification is authorized;
without those inputs it reports `not-run`:

```bash
python3 tools/audit_quantization.py \
  --fp32-model /path/to/sface-fp32.onnx \
  --int8-model /path/to/sface-int8.onnx \
  --pairs /path/to/lfw-ijbc-pairs.jsonl \
  --fp32-embeddings /path/to/fp32-embeddings.jsonl \
  --int8-embeddings /path/to/int8-embeddings.jsonl
```

For native ASan/UBSan coverage:

```bash
cmake --fresh -S . -B build-sanitized -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DKFACEAUTH_ENABLE_NATIVE_SANITIZERS=ON
cmake --build build-sanitized --parallel
ASAN_OPTIONS=detect_leaks=1 \
  ctest --test-dir build-sanitized --output-on-failure \
  -R yunet_bridge_native
```

## Aggregate evaluation

The non-installed evaluator requires an explicitly supplied, permissioned PPM
dataset manifest. It runs the same one-request identity service path in a
short-lived evaluator subprocess for worker latency and memory measurements.
It never captures a camera and outputs aggregate JSON only. See
[HARDWARE-QUALIFICATION.md](HARDWARE-QUALIFICATION.md).

## Staged installation

```bash
DESTDIR="$PWD/stage" cmake --install build
find stage -type f -o -type l
```

The payload includes the KCM, translation, camera/vision/identity workers,
YuNet/SFace models, licenses, provenance, and manifest. It contains no
evaluator, fake provider, PAM module, service, privileged helper, enrolled
profile, or key.
