# KFaceAuth v4.0.0 Architecture, Security, and Performance Review

**Document Version**: 4.0.0-REV  
**Author**: Worker 2 — Review Document Author  
**Target Architecture**: KFaceAuth (`plasma-kfaceauth`) v4.0.0  
**Target Environment**: Fedora Linux 44 (x86_64), Linux Kernel 7.2.5, KDE Plasma 6.7.5, Qt 6.11.2, KF6 6.30.0, OpenCV 4.13.0, OpenSSL 3.5.8  
**Classification**: Engineering Deep-Dive & System Audit  
**Date**: 2026-09-15  
**Cross-References**: [ROADMAP-V5.md](ROADMAP-V5.md) | [ARCHITECTURE.md](ARCHITECTURE.md) | [SECURITY.md](../SECURITY.md) | [THREAT-BOUNDARY.md](THREAT-BOUNDARY.md) | [RELEASE-CHECKLIST.md](RELEASE-CHECKLIST.md) | [TEMPLATE-VAULT.md](TEMPLATE-VAULT.md)  

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
   - [1.1 System State and Scope](#11-system-state-and-scope)
   - [1.2 Key Architectural Achievements](#12-key-architectural-achievements)
   - [1.3 Fundamental Bottlenecks and Deficiencies](#13-fundamental-bottlenecks-and-deficiencies)
   - [1.4 Production Readiness & Authentication Suitability Assessment](#14-production-readiness--authentication-suitability-assessment)
2. [Comprehensive Repository & Subsystem Review](#2-comprehensive-repository--subsystem-review)
   - [2.1 Rust Core Workspace (`engine/`)](#21-rust-core-workspace-engine)
     - [`kfaceauth-crypto-openssl-sys`](#kfaceauth-crypto-openssl-sys)
     - [`kfaceauth-identity-types`](#kfaceauth-identity-types)
     - [`kfaceauth-protocol`](#kfaceauth-protocol)
     - [`kfaceauth-vision-opencv-sys`](#kfaceauth-vision-opencv-sys)
     - [`kfaceauth-vision`](#kfaceauth-vision)
     - [`kfaceauth-templates`](#kfaceauth-templates)
     - [`kfaceauth-identity`](#kfaceauth-identity)
   - [2.2 C++ / Qt6 / KF6 Layer (`src/`)](#22-c--qt6--kf6-layer-src)
     - [Preview Subsystem (`src/preview`)](#preview-subsystem-srcpreview)
     - [Backend Subsystem (`src/backend`)](#backend-subsystem-srcbackend)
     - [KCM & Presentation Subsystem (`src/kcm`)](#kcm--presentation-subsystem-srckcm)
   - [2.3 IPC & Protocol Safety](#23-ipc--protocol-safety)
     - [Framing and Buffer Boundaries](#framing-and-buffer-boundaries)
     - [Half-Duplex Deadlock Prevention](#half-duplex-deadlock-prevention)
     - [Error Handling and Wire Disconnect](#error-handling-and-wire-disconnect)
   - [2.4 Computer Vision Pipeline](#24-computer-vision-pipeline)
     - [YuNet Face Detection and Landmark Extraction](#yunet-face-detection-and-landmark-extraction)
     - [SFace 128D Embedding Extraction & Alignment](#sface-128d-embedding-extraction--alignment)
     - [Deterministic Normalization & Cosine Similarity](#deterministic-normalization--cosine-similarity)
     - [Multi-Template Median Aggregation Policy](#multi-template-median-aggregation-policy)
   - [2.5 Vault Security & Cryptography](#25-vault-security--cryptography)
     - [AES-256-GCM AEAD Specification](#aes-256-gcm-aead-specification)
     - [Contextual Associated Authenticated Data (AAD) Binding](#contextual-associated-authenticated-data-aad-binding)
     - [KWallet Session Key Lifecycle & Absence of KDF](#kwallet-session-key-lifecycle--absence-of-kdf)
     - [Numeric UID Isolation & Filesystem Atomic Transactions](#numeric-uid-isolation--filesystem-atomic-transactions)
     - [Critical Secret Erasure Gap: Dead-Store Elimination in Rust `Drop`](#critical-secret-erasure-gap-dead-store-elimination-in-rust-drop)
3. [Performance & Resource Optimization Profiling](#3-performance--resource-optimization-profiling)
   - [3.1 Frame Ingestion Path: The 20-Step Copy Breakdown](#31-frame-ingestion-path-the-20-step-copy-breakdown)
   - [3.2 Worker Process Lifecycle & Cold Startup Overhead](#32-worker-process-lifecycle--cold-startup-overhead)
   - [3.3 Neural Network Inference Latency & Threading Allocation](#33-neural-network-inference-latency--threading-allocation)
   - [3.4 Zero-Copy Shared Memory Architecture Potential](#34-zero-copy-shared-memory-architecture-potential)
   - [3.5 Memory Footprint Across System States](#35-memory-footprint-across-system-states)
4. [User Experience & Workflow Streamlining](#4-user-experience--workflow-streamlining)
   - [4.1 Camera Discovery & Device Enumeration](#41-camera-discovery--device-enumeration)
   - [4.2 Preview Responsiveness & Rendering Pipeline](#42-preview-responsiveness--rendering-pipeline)
   - [4.3 Visual Guidance & Dynamic Feedback Deficiencies](#43-visual-guidance--dynamic-feedback-deficiencies)
   - [4.4 Enrollment Journey Friction (3–5 Samples)](#44-enrollment-journey-friction-35-samples)
   - [4.5 Diagnostics, System Status & Vault Reset Ergonomics](#45-diagnostics-system-status--vault-reset-ergonomics)
5. [Security & Privacy Boundary Verification](#5-security--privacy-boundary-verification)
   - [5.1 Biometric Disk Persistence Proof](#51-biometric-disk-persistence-proof)
   - [5.2 Telemetry, Network, and Offline Supply Chain Containment](#52-telemetry-network-and-offline-supply-chain-containment)
   - [5.3 Numeric UID Privilege Boundaries](#53-numeric-uid-privilege-boundaries)
   - [5.4 KWallet Key Protection Boundary](#54-kwallet-key-protection-boundary)
   - [5.5 In-Session Experiment vs Operating System Authentication Boundary](#55-in-session-experiment-vs-operating-system-authentication-boundary)
6. [Comprehensive Bottleneck & Technical Debt Catalog](#6-comprehensive-bottleneck--technical-debt-catalog)
7. [Verification & Environment Evidence](#7-verification--environment-evidence)
   - [7.1 Host Environment Specifications](#71-host-environment-specifications)
   - [7.2 Cargo Workspace Verification (Rust Core)](#72-cargo-workspace-verification-rust-core)
   - [7.3 CMake & CTest Suite Verification (C++ / Qt6 Layer)](#73-cmake--ctest-suite-verification-c--qt6-layer)
   - [7.4 Python Supply Chain & Security Boundary Verification](#74-python-supply-chain--security-boundary-verification)
   - [7.5 Upstream Deprecations & Modernization Notices](#75-upstream-deprecations--modernization-notices)

---

## 1. Executive Summary

### 1.1 System State and Scope

KFaceAuth (`plasma-kfaceauth`) v4.0.0 represents a clean, modular rewrite of a local facial recognition subsystem tailored for KDE Plasma 6 on Fedora Linux. The project is designed as an **experimental, privacy-preserving in-session identity utility** integrated into KDE System Settings via a Kirigami Configuration Module (`KQuickConfigModule`). 

Its primary purpose in Milestone 4 is to provide local camera discovery, live camera preview, one-frame face alignment guidance, multi-sample biometric enrollment (3 to 8 samples, 5 recommended), authenticated template vault management, and one-shot verification testing against stored facial embeddings.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    KDE Plasma 6 System Settings (KCM)                       │
│      (Kirigami QML UI: Home, Setup, Test, Diagnostics | Qt 6.11.2)         │
└──────────────────┬──────────────────────┬──────────────────────┬────────────┘
                   │                      │                      │
            (Private Pipe)         (Private Pipe)         (Private Pipe)
                   ▼                      ▼                      ▼
        ┌─────────────────────┐ ┌───────────────────┐ ┌──────────────────────┐
        │   Camera Preview    │ │   Vision Worker   │ │   Identity Worker    │
        │       Worker        │ │ (YuNet Detection) │ │  (SFace Recognition  │
        │ (Qt Multimedia/V4L2)│ │   [One-Shot]      │ │   + Encrypted Vault) │
        └─────────────────────┘ └───────────────────┘ └──────────────────────┘
                   │                      │                      │
                   ▼                      ▼                      ▼
         Hardware Video Node     Fedora OpenCV 4.13     Fedora OpenSSL 3.5.8
           (/dev/videoX)         (FaceDetectorYN &        (AES-256-GCM AEAD  │
                                   FaceRecognizerSF)       + CSPRNG RAND)    │
                                                                 │           │
                                                                 ▼           ▼
                                                          KDE KWallet   Filesystem Vault
                                                          (Master Key)   (~/.local/share/
                                                                         kfaceauth/...)
```

### 1.2 Key Architectural Achievements

The codebase demonstrates exceptional engineering discipline in defense-in-depth, memory safety, and fail-closed state management:

1. **Privilege Separation & Sandboxing**: All processes execute strictly as the unprivileged invoking user (`getuid()`). Worker processes run with disabled core dumps (`prctl(PR_SET_DUMPABLE, 0)` and `setrlimit(RLIMIT_CORE, 0)`), have zero network access, and communicate exclusively over private, inherited standard pipes.
2. **Cryptographic Rigor in Vault Storage**: The encrypted biometric template vault employs AES-256-GCM authenticated encryption with associated authenticated data (AAD) binding. The AAD cryptographically seals the user's numeric Linux UID, outer schema version, detector model ID (`yunet-2023mar-v1`), embedding model ID (`sface-2021dec-fp32-v1`), exact SFace ONNX SHA-256 digest (`0ba9fbfa...`), embedding format (`sface-f32-le-128-l2-v1`), dimension (`128`), and normalization version (`1`). Any cross-user tampering, template transplanting, or model-downgrade attack fails closed at the authentication tag boundary.
3. **Defensive Filesystem Transactions**: Vault creation and key rotation employ strict TOCTOU and symlink-race protections (`engine/templates/src/lib.rs:647-781`): pre- and post-open device/inode descriptor validation, directory mode `0700` and file mode `0600` enforcement, rejection of symlinks and hardlinks (`nlink == 1`), `O_EXCL` temporary file creation, pre-rename self-decryption verification, and double `fsync` (file content and parent directory).
4. **Deterministic Mathematics & Score Containment**: All vector normalization and cosine similarity computations are implemented in pure, deterministic Rust utilizing double-precision (`f64`) floating point arithmetic, completely independent of OpenCV runtime math variations. Crucially, raw similarity scores are never emitted over IPC; responses return only coarse categorical variants (`Match`, `NoMatch`, `Ambiguous`), preventing iterative hill-climbing attacks.
5. **Verified Offline Model Supply Chain**: Model weights (`face_detection_yunet_2023mar.onnx` and `face_recognition_sface_2021dec.onnx`) are validated against an immutable, offline cryptographic manifest (`manifest.kfaceauth`). Dynamic downloads, runtime updates, and remote inference are strictly forbidden.

### 1.3 Fundamental Bottlenecks and Deficiencies

Despite its strong security posture, the v4.0.0 implementation exhibits severe runtime latency bottlenecks, excessive CPU/memory bandwidth consumption, compiler-level security erasure gaps, and UX friction:

1. **One-Shot Worker Spawning Anti-Pattern**: Both `kfaceauth-vision-worker` and `kfaceauth-identity-worker` enforce a strict `serve_once` lifecycle requiring EOF on stdin. Every single frame analysis, enrollment sample, or status probe spawns a new process via `fork()` + `execve()`, linking heavy dynamic libraries (`libopencv_*.so`, `libcrypto.so`), verifying manifests, and reconstructing the 38.7 MB SFace ONNX neural network graph. This imposes a **~750–800 ms cold latency floor** on operations where actual neural network inference takes only **~33 ms** (a 24x overhead).
2. **Redundant Cryptographic Model Hashing**: On every invocation of `kfaceauth-identity-worker`, the 38.7 MB SFace ONNX model is read and hashed using an unvectorized scalar Rust SHA-256 implementation **three separate times** (116.1 MB hashed). During KCM status refreshes, Qt C++ hashes the file a **fourth time** via `QCryptographicHash`. At ~168 MB/s scalar hashing throughput, model verification consumes **~690 ms** of worker startup time.
3. **Severe 20-Step Frame Ingestion Pipeline**: A camera frame undergoes **20 distinct buffer copies, format conversions, iterative JPEG compressions, and memory allocations** between V4L2 acquisition and OpenCV DNN tensor input. The preview worker iteratively compresses frames to JPEG at quality levels 85/70/55/40, transmits them over pipes, and the KCM decompresses them on the Qt GUI thread.
4. **Dead-Store Elimination in Secret Erasure**: All secret zeroing in Rust `Drop` implementations (`MasterKey::drop`, `SensitiveBytes::drop`, `NormalizedEmbedding::drop`) relies on standard non-volatile slice fills (`fill(0)` or `fill(0.0)`). In release builds, LLVM's Dead Store Elimination (DSE) pass optimizes these stores away because the backing heap memory is deallocated immediately without subsequent reads.
5. **Software Rasterization & 8 FPS Preview Throttle**: The KCM preview item subclasses `QQuickPaintedItem`, performing software rasterization via `QPainter::drawImage` on every frame instead of uploading GPU textures to the Qt Quick Scene Graph. The preview is artificially throttled to a hard ceiling of 8 FPS with a 60-second hardware shutoff timer.
6. **High-Friction Manual Enrollment**: Enrolling a face requires the user to manually click "Capture sample" 3 to 5 separate times while attempting to maintain specific poses. Duplicate samples are rejected with cryptic error messages, the session is cancelled on a 120-second timeout, and switching windows or losing focus instantly aborts enrollment and discards all accepted embeddings.

### 1.4 Production Readiness & Authentication Suitability Assessment

| Capability Area | v4.0.0 Status | Production Readiness Assessment |
|---|:---:|---|
| **In-Session Local Experiment** | **READY** | Stable for local testing in KDE System Settings. All 49 Cargo tests, 16 CTest suites, and 31 Python tests pass cleanly. |
| **Biometric Privacy & Isolation** | **VERIFIED** | Zero biometric frames written to disk; memory zeroed where compiler allows; strict UID boundary enforcement. |
| **Linux PAM / SDDM Integration** | **BLOCKED** | **Unqualified & Architecturally Blocked**. Master key is locked inside the user's desktop KWallet, which is inaccessible before login or to system services (`pam_kfaceauth.so`). Storage path is hardcoded to `$XDG_DATA_HOME`. |
| **Liveness / Presentation-Attack Detection** | **BLOCKED** | **Unqualified & Absent**. No anti-spoofing or liveness detection. A printed color photograph or screen replay held before the camera passes verification. |
| **Demographic Parity / Biometric Qualification** | **BLOCKED** | **Unqualified**. FAR (False Acceptance Rate) and FRR (False Rejection Rate) have not been evaluated against a representative, demographically balanced, permissioned dataset. |

---

## 2. Comprehensive Repository & Subsystem Review

### 2.1 Rust Core Workspace (`engine/`)

The Rust core workspace consists of 7 modular crates configured under a unified Cargo workspace with strict workspace-level lints (`unsafe_code = "forbid"`, `clippy::all = "deny"`, `clippy::pedantic = "deny"`), with `unsafe` operations strictly confined to reviewed `-sys` FFI crates.

```
engine/
├── crypto-openssl-sys/   # Native C ABI bridge to system OpenSSL 3.x
├── identity-types/       # Pure-Rust domain types, normalization, cosine math
├── protocol/             # Binary IPC framing, v2 capabilities, request/response codecs
├── vision-opencv-sys/    # C++ FFI bridge to OpenCV 4.13 (YuNet & SFace)
├── vision/               # Vision worker binary, image conversion, quality analysis
├── templates/            # AES-256-GCM vault, AAD binding, filesystem transactions
└── identity/             # Identity worker binary, multi-operation dispatcher
```

#### `kfaceauth-crypto-openssl-sys`
- **Location**: `engine/crypto-openssl-sys/`
- **Responsibilities**: Provides safe Rust abstractions over Fedora OpenSSL 3.x for AES-256-GCM AEAD encryption/decryption, CSPRNG byte generation, and POSIX UID resolution.
- **Evaluation**:
  - `build.rs:16-27` validates OpenSSL major version $\ge 3$ via `pkg-config`.
  - `native/crypto_bridge.c:42-91` (`kfaceauth_crypto_aes256gcm_encrypt`): Uses `EVP_CIPHER_CTX_new()`, `EVP_EncryptInit_ex2()`, sets 96-bit IV via `EVP_CIPHER_CTX_ctrl(..., EVP_CTRL_GCM_SET_IVLEN, 12, NULL)`, sets AAD, encrypts payload, and extracts the 128-bit tag via `EVP_CTRL_GCM_GET_TAG`. On failure, invokes `OPENSSL_cleanse()` on intermediate buffers.
  - `native/crypto_bridge.c:93-145` (`kfaceauth_crypto_aes256gcm_decrypt`): Sets the expected authentication tag via `EVP_CTRL_GCM_SET_TAG` *before* finalizing decryption via `EVP_DecryptFinal_ex()`. If tag verification fails, returns `KFACEAUTH_CRYPTO_AUTHENTICATION_FAILURE` and cleanses the output buffer.
  - `native/crypto_bridge.c:147-150`: Wraps POSIX `getuid()` returning `uint32_t`.
  - **Deficiency**: While the C bridge uses `OPENSSL_cleanse()`, the safe Rust wrapper (`src/lib.rs:134, 180`) clears vectors on error using standard `ciphertext.fill(0)` and `plaintext.fill(0)`, which are susceptible to compiler dead-store elimination.

#### `kfaceauth-identity-types`
- **Location**: `engine/identity-types/`
- **Responsibilities**: Shared domain types, model identifiers, normalization contracts, and vector similarity. It has zero external dependencies and enforces `#![forbid(unsafe_code)]`.
- **Evaluation**:
  - Constants: `DETECTOR_MODEL_ID = "yunet-2023mar-v1"`, `EMBEDDING_MODEL_ID = "sface-2021dec-fp32-v1"`, `EMBEDDING_FORMAT_ID = "sface-f32-le-128-l2-v1"`, `SFACE_MODEL_SHA256 = "0ba9fbfa..."`, `EMBEDDING_DIMENSION = 128`.
  - `src/lib.rs:30-51` (`NormalizedEmbedding::from_raw`): Validates raw 128-float output from SFace. Rejects NaN, Inf, and values exceeding absolute magnitude 32.0. Accumulates the L2 norm in double precision (`f64`), checks `norm_squared > 1e-12`, divides elements by the norm, and verifies that the final norm satisfies $|\|v\|_2 - 1.0| \le 10^{-5}$.
  - `src/lib.rs:112-119` (`cosine_similarity`): Calculates double-precision dot product over 128 dimensions, clamped to $[-1.0, 1.0]$.
  - **Deficiency**: `src/lib.rs:93-97` implements `Drop for NormalizedEmbedding` with `self.values.fill(0.0)`. As analyzed in Section 2.5, LLVM dead-store elimination optimizes this write away.

#### `kfaceauth-protocol`
- **Location**: `engine/protocol/`
- **Responsibilities**: Core binary IPC protocol codecs, versioning, capability bitmasks, and framing bounds.
- **Evaluation**:
  - Length-prefixed framing: `src/lib.rs:333-346` (`read_frame`) reads a 4-byte big-endian length prefix, checks `length > 0` and `length <= maximum`, and allocates `vec![0_u8; length]` only after bound verification.
  - Implements Protocol Version 2 with capability negotiation (`Capabilities` bitmask).
  - **Deficiency — Critical Architectural Disconnect**: The `kfaceauth-protocol` crate is completely orphaned. Neither `kfaceauth-vision` nor `kfaceauth-identity` imports it. Instead, both worker crates implement their own duplicate length-prefix framing (`read_frame`/`write_frame`) and declare Protocol Version 1 wire formats (`engine/vision/src/worker.rs:308-350`, `engine/identity/src/lib.rs:554-594`).

#### `kfaceauth-vision-opencv-sys`
- **Location**: `engine/vision-opencv-sys/`
- **Responsibilities**: C/C++ FFI bridge wrapping OpenCV 4.13 DNN modules (`cv::FaceDetectorYN` and `cv::FaceRecognizerSF`).
- **Evaluation**:
  - `build.rs:35-39`: Compiles C++ code with `-std=c++17` and asserts `major == Some(4) && minor == Some(13)`.
  - `native/yunet_bridge.cpp:81-91`: Disables core dumps in worker processes via `setrlimit(RLIMIT_CORE, 0)` and `prctl(PR_SET_DUMPABLE, 0)`.
  - `native/yunet_bridge.cpp:98-131` (`kfaceauth_yunet_create`): Constructs `cv::FaceDetectorYN` from an in-memory buffer using CPU backend (`cv::dnn::DNN_BACKEND_OPENCV`, `cv::dnn::DNN_TARGET_CPU`).
  - `native/yunet_bridge.cpp:207-232` (`kfaceauth_sface_create`): Constructs `cv::FaceRecognizerSF` from memory using CPU backend.
  - `native/yunet_bridge.cpp:234-310` (`kfaceauth_sface_extract`): Uses `alignCrop` to generate an affine-transformed 112×112 BGR face crop based on the 5 YuNet facial landmarks, then runs `feature` to output raw 128 FP32 embeddings.
  - **Deficiencies**:
    1. Hardcoded resolution cap: `native/yunet_bridge.cpp:47-48` rejects any image with `width > 640 || height > 480`. Standard 720p or 1080p camera frames fail with invalid argument.
    2. Redundant memory copies: In `yunet_bridge.cpp:149-154` and `253-258`, allocates a new `cv::Mat image` and copies pixels row-by-row via `std::copy_n`, despite the source buffer already being contiguous.
    3. Rigorous build pinning to OpenCV 4.13 breaks compilation on distributions packaging OpenCV 4.10–4.12.

#### `kfaceauth-vision`
- **Location**: `engine/vision/`
- **Responsibilities**: Vision worker binary (`kfaceauth-vision-worker`), benchmark utility (`kfaceauth-yunet-benchmark`), pixel parsing, quality metrics, and model manifest validation.
- **Evaluation**:
  - `src/lib.rs:15-18`: Maximum dimensions `MAX_WIDTH = 640`, `MAX_HEIGHT = 480`, `MAX_FRAME_BYTES = 1,228,800`, `MAX_FACES = 8`. Supported formats: RGB8, RGBA8, Gray8.
  - `src/model.rs:43-100`: Validates `manifest.kfaceauth` (TSV schema, closed file inventory, exact size and SHA-256 verification).
  - `src/lib.rs:318-378` (`calculate_quality`): Measures image brightness, contrast, and sharpness using integer luminance arithmetic: $\text{luma} = (77R + 150G + 29B) \gg 8$.
  - `src/yunet.rs:176-235` (`convert_to_bgr`): Swaps R and B channels into a fresh `SensitiveBytes` buffer.
  - **Deficiencies**:
    1. Single-request worker: `src/worker.rs:87-103` serves exactly one request, enforces EOF, and exits.
    2. Scalar SHA-256: `src/sha256.rs` implements SHA-256 in scalar Rust without hardware acceleration (e.g. SHA-NI), causing massive latency during manifest verification.

#### `kfaceauth-templates`
- **Location**: `engine/templates/`
- **Responsibilities**: Secure profile template management, AES-256-GCM vault serialization, atomic filesystem transactions, advisory locking, and median aggregation verification.
- **Evaluation**:
  - `src/lib.rs:25-30`: Profile limits: minimum 3, recommended 5, maximum 8 samples. Provisional match threshold `0.45`, ambiguity margin `0.04`.
  - `src/lib.rs:133-154` (`Profile::verify`): Computes cosine similarity of candidate embedding against all enrolled samples, sorts scores, and takes the median.
  - `src/lib.rs:419-438` (`associated_data`): Generates 256-byte AAD binding namespace, schema version, numeric UID, model IDs, model SHA-256, format, and normalization version.
  - `src/lib.rs:521-584` (`encode_vault` / `decode_vault`): Binary framing: `[Magic: "KFAVLT04" (8B)][Schema: u16 (2B)][Nonce: 12B][Length: u32 (4B)][Tag: 16B][Ciphertext: N bytes]`. Max vault size: 16 KiB.
  - `src/lib.rs:647-781` (`write_verified_atomic`): Atomic write sequence: writes `.identity.vault.<random_hex>.tmp` with `O_EXCL`, executes `file.sync_all()`, decrypts and verifies the temporary file before rename, executes `fs::rename`, and executes `sync_directory(root)`.
  - **Deficiencies**:
    1. Dead-store elimination in `MasterKey::drop` (`src/lib.rs:75`) and `SensitiveBytes::drop` (`src/lib.rs:415`).
    2. Path is hardcoded to `$XDG_DATA_HOME/kfaceauth/identity.vault` (`src/lib.rs:192-208`), precluding system-level or PAM storage paths.

#### `kfaceauth-identity`
- **Location**: `engine/identity/`
- **Responsibilities**: Identity worker binary (`kfaceauth-identity-worker`), evaluation benchmark (`kfaceauth-identity-evaluate`), and multi-operation protocol dispatcher.
- **Evaluation**:
  - Supports 10 closed operations: `OP_STATUS`, `OP_EXTRACT_ENROLLMENT_SAMPLE`, `OP_COMMIT_ENROLLMENT`, `OP_LIST_PROFILE_SUMMARY`, `OP_VERIFY_ONE_FRAME`, `OP_DELETE_PROFILE`, `OP_ROTATE_VAULT_KEY`, `OP_VALIDATE_VAULT`, `OP_RESET_UNREADABLE`, `OP_GENERATE_KEY`.
  - `src/lib.rs:185-200`: Enforces duplicate sample rejection if cosine similarity $\ge 0.995$ (`DUPLICATE_COSINE_THRESHOLD`).
  - `src/lib.rs:707-715`: Test verifies that verification responses return only categorical results (`Match = 1`, `NoMatch = 2`, `Ambiguous = 3`) and never leak floating-point similarity scores.
  - **Deficiencies**:
    1. Single-request lifecycle: `src/main.rs:21` and `src/lib.rs:146-161` enforce `serve_once` with `require_eof`.
    2. Model path hardcoded: `src/main.rs:8` hardcodes `PRODUCTION_MODEL_ROOT = "/usr/share/kfaceauth/models"`.

---

### 2.2 C++ / Qt6 / KF6 Layer (`src/`)

The C++ layer coordinates hardware devices, executes UI state machines, and interfaces with the Rust engine workers.

```
src/
├── preview/   # Standalone camera capture worker & CBOR preview protocol
├── backend/   # Session controllers, KWallet integration, IPC worker clients
└── kcm/       # KQuickConfigModule, QML UI pages, and QQuickPaintedItem preview
```

#### Preview Subsystem (`src/preview`)
- **Components**:
  - `cameraprovider.cpp`: Probes hardware devices via `libudev`, checks `ID_INFRARED_CAMERA` and `ID_V4L_CAPABILITIES` to categorize devices into `rgb`, `ir`, or `unknown`. Preflights video nodes with non-blocking POSIX `open(O_RDWR | O_NONBLOCK | O_CLOEXEC)` to catch `EBUSY` or permission errors.
  - `previewprotocol.h / .cpp`: Encodes frame packets using a 4-byte big-endian length prefix followed by CBOR maps (`QCborMap`). Enforces bounds: `MaxJpegBytes = 131072` (128 KiB), `MaxWidth = 640`, `MaxHeight = 480`, `MaxFramesPerSecond = 8`, `MaxPreviewSeconds = 60`.
  - `previewworker.cpp`: Non-blocking I/O on stdin/stdout using `QSocketNotifier`. Maintains separate queues for control messages and video frames (`LatestFrameBuffer`), dropping stale video frames during backpressure.
- **Deficiencies**:
  - **Double Software Downscaling**: In `cameraprovider.cpp:175-177` and `280-283`, `handleFrame` scales an incoming `QImage` exceeding 640×480 using `image.scaled(..., Qt::SmoothTransformation)` inside `encodeFrame`, and immediately downscales it again in `handleFrame` to compute bounded geometry.
  - **Iterative JPEG Compression Loop**: `cameraprovider.cpp:178-186` loops over quality levels `{85, 70, 55, 40}` attempting to compress the frame into under 128 KiB.
  - **Arbitrary 8 FPS Throttle**: `previewprotocol.h:19` artificially caps the preview rate to 8 FPS via an elapsed timer (`minimumInterval = 125 ms`).

#### Backend Subsystem (`src/backend`)
- **Components**:
  - `camerapreviewsession.cpp`: Implements `QAbstractListModel` for discovered cameras. Manages the lifecycle of `kfaceauth-camera-preview-worker`. Sanitizes worker environment to `LANG`, `LC_ALL`, `XDG_RUNTIME_DIR`, `DBUS_SESSION_BUS_ADDRESS`, `WAYLAND_DISPLAY`, `DISPLAY`. Decodes incoming frames using `QImage::fromData(jpeg, "JPEG")` on line 506.
  - `visionanalysissession.cpp`: Manages one-shot face alignment checks. Converts preview frame to `Format_RGB888`, launches a fresh `kfaceauth-vision-worker` `QProcess`, pipes 921 KB of raw pixels over stdin, parses response, and terminates the process.
  - `identityworkerclient.cpp`: Manages requests to `kfaceauth-identity-worker`. Spawns a new `QProcess` for every operation (`execute`), writes request, calls `closeWriteChannel()`, parses response, and awaits process exit.
  - `enrollmentsession.cpp`: Coordinates enrollment workflow. Acquires a 32-byte key from `KWalletKeyProvider`, captures 3–8 appearance samples, accumulates 128-float embeddings in memory, and commits the encrypted vault. Enforces a 120-second watchdog timer and rolls back KWallet keys if enrollment is cancelled or aborted.
  - `localverificationsession.cpp`: Manages test verifications. Enforces a 2000 ms cooldown between consecutive verification attempts.
  - `kwalletkeyprovider.cpp`: Asynchronously interacts with `KF6::Wallet`. Stores the 256-bit AES master key in folder `KFaceAuth`, entry `user-session-vault-master-key-v1`. Generates random keys using OpenSSL `RAND_priv_bytes()`.
  - `systemprobe.cpp` & `supportreport.cpp`: Diagnostic probes running via `QtConcurrent::run`. Generates redacted Markdown support reports with regex-based redaction of user paths, emails, passwords, and tokens.
- **Deficiencies**:
  - **One-Shot Worker Spawning Anti-Pattern**: Spawns a brand new `QProcess` on every analysis (`visionanalysissession.cpp:374`), enrollment sample, verification, and status check (`identityworkerclient.cpp:106`), incurring massive process startup overhead.
  - **UI Thread Blocking**: Decodes JPEG frames (`QImage::fromData`) synchronously on the main Qt GUI thread (`camerapreviewsession.cpp:506`).
  - **Piped Frame Memory Overhead**: Allocates and pipes uncompressed 921 KB RGB buffers over anonymous OS pipes for every analysis and verification call.
  - **Deprecations**: `kwalletkeyprovider.cpp:194` calls `m_wallet->sync()`, which is deprecated as unimplemented in KF6 6.30.

#### KCM & Presentation Subsystem (`src/kcm`)
- **Components**:
  - `kfaceauthkcm.cpp`: Subclasses `KQuickConfigModule`. Exposes session models to QML context and drives the high-level UI flow state machine: `flowState()` returns `NeedsAttention`, `NeedsCamera`, `NeedsProfile`, or `ReadyToTest`.
  - `camerapreviewitem.cpp`: Subclasses `QQuickPaintedItem`. Paints the camera frame using `QPainter::drawImage(target, frame)` with `SmoothPixmapTransform` and optional horizontal mirroring (`painter->scale(-1.0, 1.0)`).
- **Deficiencies**:
  - **Software Rasterization Bottleneck**: `QQuickPaintedItem` renders frames on the CPU into a backing store image, which must be uploaded to the GPU as a new texture on every frame.
  - **Discarded Face Coordinates**: While YuNet returns precise bounding boxes and 5 landmarks, `VisionAnalysisSession` discards them and returns only abstract booleans (`Centered`, `Suitable`). `CameraPreviewItem` has no capability to render dynamic face tracking boxes or landmarks.

---

### 2.3 IPC & Protocol Safety

The communication architecture across processes relies on length-prefixed binary protocols over anonymous standard pipes (`stdin`/`stdout`).

```text
Parent (KCM / Backend)                    Child (Worker Process)
─────────────────────                    ──────────────────────
  QProcess::start()         ─────────►     Process launch & ld.so
  write(LengthPrefix + Header + Data) ─►   read_frame(stdin)
  closeWriteChannel() (EOF) ─────────►     require_eof(stdin)
                                           [Execute Operation]
  readAllStandardOutput()   ◄─────────     write_frame(stdout)
  waitForFinished()         ◄─────────     exit(0)
```

#### Framing and Buffer Boundaries
- Every request and response begins with a 4-byte big-endian `u32` payload length.
- Both vision and identity workers strictly validate the declared payload length before allocating buffer memory:
  - `engine/vision/src/worker.rs:308-324`: Rejects declared length > `MAX_REQUEST_PAYLOAD (1,228,824 bytes)`.
  - `engine/identity/src/lib.rs:554-571`: Rejects declared length > `MAX_IDENTITY_REQUEST_BYTES (1,232,960 bytes)`.
- If an oversized length header is encountered, the worker rejects the frame with `CodecError::FrameTooLarge` without reading or allocating payload bytes, preventing memory exhaustion attacks.
- Zero-length frames fail closed with `EmptyFrame`.

#### Half-Duplex Deadlock Prevention
- OS pipes have finite buffer capacities (typically 64 KiB on Linux). Streaming uncompressed 921 KB video frames over pipes in full-duplex mode risks circular deadlocks if the parent blocks writing while the child blocks writing.
- KFaceAuth avoids this by enforcing strict **half-duplex execution**:
  1. The parent writes the entire request payload into the pipe.
  2. The parent immediately calls `m_process->closeWriteChannel()` (`src/backend/identityworkerclient.cpp:123`), transmitting an `EOF` marker to the worker.
  3. The worker reads until `EOF` (`require_eof(reader)` in `worker.rs:95` and `identity/src/lib.rs:153`).
  4. The worker processes the request, writes the framed response packet to stdout, and terminates.
- Process lifecycle is guarded by three-tier timers in `IdentityWorkerClient`:
  - `m_startupTimer`: 2,000 ms (fails if process fails to launch).
  - `m_operationTimer`: 10,000 ms (kills process if operation hangs).
  - `m_shutdownTimer`: 1,000 ms (sends `SIGKILL` if process fails to terminate after EOF).

#### Error Handling and Wire Disconnect
- All worker responses echo the client's monotonic 64-bit `generation` identifier, ensuring stale responses from timed-out operations are discarded.
- Errors are returned in a structured `RESPONSE_ERROR` packet containing a typed `WorkerErrorCode`.
- **Wire Disconnect**: Because `kfaceauth-protocol` was left unintegrated, wire constants, opcodes, and error codes are duplicated across `vision/src/worker.rs`, `identity/src/lib.rs`, `visionanalysissession.cpp`, and `identityprotocol.cpp`. Any subtle protocol modification risks silent deserialization failures.

---

### 2.4 Computer Vision Pipeline

The facial recognition pipeline operates in two distinct stages: face detection and landmark extraction via **YuNet**, followed by face alignment and 128-dimensional embedding extraction via **SFace**.

```text
Raw Image (RGB8 / RGBA8 / Gray8)
       │
       ▼
 [Quality Analysis] ──► (Luminance, Brightness, Contrast, Sharpness)
       │
       ▼
 [Color Convert]    ──► BGR Packed (640x480 max)
       │
       ▼
 [YuNet Detection]  ──► FaceDetectorYN (Score >= 0.9, NMS = 0.3, TopK = 5000)
       │
       ▼
 [Output Validate]  ──► Exactly 1 face, Box >= 80x80 px, Margin >= 4 px, 5 Landmarks
       │
       ▼
 [SFace alignCrop]  ──► Affine similarity warp to 112x112 BGR crop
       │
       ▼
 [SFace feature]    ──► FaceRecognizerSF forward pass -> 1x128 FP32 raw vector
       │
       ▼
 [Deterministic L2] ──► Double precision (f64) norm -> Unit-normalized 128D FP32
       │
       ▼
 [Vault / Matching] ──► Cosine similarity dot product -> Multi-template median aggregation
```

#### YuNet Face Detection and Landmark Extraction
- **Artifact**: `models/files/face_detection_yunet_2023mar.onnx` (232,589 bytes, SHA-256: `8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4`).
- **Initialization**: `engine/vision-opencv-sys/native/yunet_bridge.cpp:98-131`. Reads ONNX model from memory and constructs `cv::FaceDetectorYN::create`.
- **Inference**:
  - Model input size: dynamic, bounded to input image dimensions (up to 640×480).
  - Confidence threshold: `0.9` (`YUNET_SCORE_THRESHOLD`).
  - NMS threshold: `0.3` (`YUNET_NMS_THRESHOLD`).
  - Native output: 2D matrix of shape `N x 15` (`CV_32FC1`).
    - Columns `0..3`: Bounding box `[x, y, width, height]`.
    - Columns `4..13`: Five 2D landmarks `(x, y)`: right eye, left eye, nose tip, right mouth corner, left mouth corner.
    - Column `14`: Detection confidence score.
- **Rust Validation** (`engine/vision/src/yunet.rs:237-306`):
  - Validates all 15 values are finite.
  - Enforces bounding box within original unpadded frame boundaries ($x \ge 0, y \ge 0, x+w \le W, y+h \le H$).
  - Enforces all 10 landmark coordinates strictly within frame bounds.
  - Enforces confidence score satisfies $\text{score} + 10^{-5} \ge 0.9$ and $\text{score} \le 1.0 + 10^{-5}$.
  - Requires **strictly one face** for enrollment and verification (`identity.rs:80-84`); zero or multiple faces fail closed.

#### SFace 128D Embedding Extraction & Alignment
- **Artifact**: `models/files/face_recognition_sface_2021dec.onnx` (38,696,353 bytes, SHA-256: `0ba9fbfa01b5270c96627c4ef784da859931e02f04419c829e83484087c34e79`).
- **Initialization**: `yunet_bridge.cpp:207-232`. Constructs `cv::FaceRecognizerSF::create` using `DNN_BACKEND_OPENCV` and `DNN_TARGET_CPU`.
- **Alignment (`alignCrop`)**:
  - `yunet_bridge.cpp:263`: Invokes `typedRecognizer->value->alignCrop(image, faceRow, aligned)`.
  - Computes affine similarity transformation mapping the 5 facial landmarks to canonical coordinates.
  - Outputs an aligned 112×112 BGR `CV_8UC3` image. Output shape is verified: `rows == 112 && cols == 112 && type() == CV_8UC3`.
- **Feature Extraction (`feature`)**:
  - `yunet_bridge.cpp:273`: Invokes `typedRecognizer->value->feature(aligned, feature)`.
  - Performs forward pass through SFace CNN backbone.
  - Outputs continuous `1 x 128` `CV_32FC1` matrix containing 128 raw FP32 values.
- **Biometric Quality Filters** (`engine/vision/src/identity.rs:88-102`):
  - Minimum face size: $\text{width} \ge 80.0\text{ px}$ and $\text{height} \ge 80.0\text{ px}$ (`MINIMUM_FACE_EDGE`).
  - Image margin: bounding box must be at least $4.0\text{ px}$ from frame edges (`EDGE_MARGIN`).
  - Quality flags: image must not be flagged `TOO_DARK`, `TOO_BRIGHT`, `LOW_CONTRAST`, or `LOW_SHARPNESS`.

#### Deterministic Normalization & Cosine Similarity
- **Normalization Contract** (`engine/identity-types/src/lib.rs:29-51`):
  1. Validates all 128 raw values are finite and $|v_i| \le 32.0$.
  2. Computes L2 norm in double precision:
     $$\|v\|_2 = \sqrt{\sum_{i=0}^{127} (\text{f64}(v_i))^2}$$
  3. Verifies norm exceeds floor: $\|v\|_2^2 > 10^{-12}$.
  4. Computes normalized coordinates: $\hat{v}_i = \text{f32}(\text{f64}(v_i) / \|v\|_2)$.
  5. Re-verifies normalized unit norm: $|\|\hat{v}\|_2 - 1.0| \le 10^{-5}$.
- **Cosine Similarity** (`engine/identity-types/src/lib.rs:112-119`):
  Because vectors are strictly unit-normalized, the cosine similarity is the dot product:
  $$\text{sim}(u, v) = \text{clamp}\left(\sum_{i=0}^{127} \text{f64}(u_i) \cdot \text{f64}(v_i), -1.0, 1.0\right)$$
  Executed in pure Rust in $<1\ \mu\text{s}$ without external library dependencies.

#### Multi-Template Median Aggregation Policy
- **Storage Profile Bounds**: Minimum 3 samples, recommended 5 samples, maximum 8 samples (`MINIMUM_PROFILE_SAMPLES = 3`, `MAXIMUM_PROFILE_SAMPLES = 8`).
- **Algorithm** (`engine/templates/src/lib.rs:133-154`):
  1. Candidate embedding $C$ is compared against all $N$ enrolled templates $T_1, \dots, T_N$, producing similarity scores $S = [s_1, \dots, s_N]$.
  2. Scores are sorted ascending using `f64::total_cmp`.
  3. Median score is computed:
     $$\text{median} = \begin{cases} S[N / 2] & \text{if } N \text{ is odd} \\ \frac{1}{2}(S[N/2 - 1] + S[N/2]) & \text{if } N \text{ is even} \end{cases}$$
- **Categorical Decision Thresholds**:
  - $\text{median} \ge 0.45$: `Match` (`PROVISIONAL_MATCH_THRESHOLD = 0.45`).
  - $0.41 \le \text{median} < 0.45$: `Ambiguous` (`PROVISIONAL_AMBIGUITY_MARGIN = 0.04`).
  - $\text{median} < 0.41$: `NoMatch`.
- **Policy Limitations**:
  - Median aggregation requires more than 50% of enrolled templates to match the candidate. If enrollment captures varied head poses (e.g. frontal, left 15°, right 15°), an incoming frontal frame will match the frontal template with high confidence but will have lower similarity against angled templates, dragging the median below 0.45 and causing false rejections.
  - Thresholds ($0.45 / 0.41$) are engineering defaults and have not been qualified against an empirical ROC / DET curve.

---

### 2.5 Vault Security & Cryptography

#### AES-256-GCM AEAD Specification
- **Cipher**: AES-256 in Galois/Counter Mode (GCM), standard authenticated encryption with associated data (AEAD).
- **Key**: 256 bits (32 bytes).
- **Nonce**: 96 bits (12 bytes), generated per transaction using OpenSSL CSPRNG `RAND_bytes()` (`engine/crypto-openssl-sys/native/crypto_bridge.c:30-40`).
- **Tag**: 128 bits (16 bytes), verified via `EVP_CIPHER_CTX_ctrl(..., EVP_CTRL_GCM_SET_TAG, 16, tag)` before decryption completion.
- **Binary Wire Format**:
  ```text
  ┌──────────────┬──────────────┬──────────────┬──────────────┬──────────────┬──────────────────┐
  │ Magic        │ Schema Ver   │ Nonce (IV)   │ Cipher Length│ AEAD Tag     │ Ciphertext       │
  │ "KFAVLT04"   │ u16 (BE)     │ 12 Bytes     │ u32 (BE)     │ 16 Bytes     │ Variable         │
  │ (8 Bytes)    │ (2 Bytes)    │ (12 Bytes)   │ (4 Bytes)    │ (16 Bytes)   │ (Max 16 KiB)     │
  └──────────────┴──────────────┴──────────────┴──────────────┴──────────────┴──────────────────┘
  ```

#### Contextual Associated Authenticated Data (AAD) Binding
- **Implementation**: `engine/templates/src/lib.rs:419-438` (`associated_data`).
- **Sealed Fields**:
  ```rust
  let mut data = Vec::with_capacity(256);
  data.extend_from_slice(b"io.github.loofiboss_bit.KFaceAuth/user-session-vault");
  data.extend_from_slice(&VAULT_SCHEMA.to_be_bytes());          // Outer schema (4)
  data.extend_from_slice(&uid.to_be_bytes());                   // Numeric POSIX UID
  data.extend_from_slice(DETECTOR_MODEL_ID.as_bytes());         // "yunet-2023mar-v1"
  data.push(0);
  data.extend_from_slice(EMBEDDING_MODEL_ID.as_bytes());        // "sface-2021dec-fp32-v1"
  data.push(0);
  data.extend_from_slice(SFACE_MODEL_SHA256.as_bytes());        // "0ba9fbfa..."
  data.push(0);
  data.extend_from_slice(EMBEDDING_FORMAT_ID.as_bytes());       // "sface-f32-le-128-l2-v1"
  data.extend_from_slice(&128_u16.to_be_bytes());               // Dimension (128)
  data.extend_from_slice(&NORMALIZATION_VERSION.to_be_bytes()); // Version (1)
  ```
- **Cryptographic Property**: The AEAD tag validates both the ciphertext and the AAD. Any modification to the user's UID, model identities, or model binary SHA-256 invalidates the authentication tag, causing immediate decryption failure. This mathematically guarantees that a vault file cannot be transplanted to another user or evaluated using an unapproved neural network.

#### KWallet Session Key Lifecycle & Absence of KDF
- **Master Key Storage**: The 32-byte master key is stored in the user's KDE KWallet under folder `KFaceAuth`, entry `user-session-vault-master-key-v1` (`src/backend/kwalletkeyprovider.cpp:11-13`).
- **Absence of KDF**: The 256-bit key from the CSPRNG is utilized directly as the AES-256-GCM encryption key. Standard cryptographic hygiene (RFC 5869 HKDF) recommends deriving purpose-separated subkeys:
  $$\text{Key}_{\text{enc}} = \text{HKDF-Expand}(\text{MasterKey}, \text{"kfaceauth/vault-encryption/v1"}, 32)$$
- **Session Availability Failure for PAM/Pre-Login**:
  KWallet is a desktop user-session service requiring an active D-Bus session bus and an unlocked user login. It is inaccessible at the SDDM display manager, lock screen, or within PAM services (`/lib64/security/pam_*.so`). Consequently, the v4 key architecture strictly precludes pre-login OS authentication.

#### Numeric UID Isolation & Filesystem Atomic Transactions
- **Filesystem Security Validation** (`engine/templates/src/lib.rs:647-700`):
  - Directory: Must reside at `$XDG_DATA_HOME/kfaceauth` (or `$HOME/.local/share/kfaceauth`), owned by `current_uid()`, mode `0700` (`rwx------`). Symlinks are rejected.
  - File: Must be a regular file, owned by `current_uid()`, mode `0600` (`rw-------`), single link count (`nlink == 1` to prevent hard-link tampering).
  - TOCTOU Defense: Verifies `symlink_metadata` on the path, opens the file descriptor, queries descriptor `file.metadata()`, and asserts that `dev()` and `ino()` match pre-open metadata.
- **Atomic Transaction Sequence** (`engine/templates/src/lib.rs:751-781`):
  1. Opens advisory lock `.identity.vault.lock` with bounded 2.0-second timeout.
  2. Generates temporary file `.identity.vault.<16_hex_chars>.tmp` with `O_CREAT | O_EXCL | O_WRONLY`, mode `0600`.
  3. Writes ciphertext and executes `file.sync_all()`.
  4. Reads back the temporary file and executes a full decryption self-test using the master key.
  5. Atomically renames temporary file over `identity.vault` via `fs::rename()`.
  6. Executes `sync_directory(root)` (`fsync` on parent directory descriptor) to commit metadata to disk.

#### Critical Secret Erasure Gap: Dead-Store Elimination in Rust `Drop`
- **Vulnerability Description**:
  The Rust core attempts to wipe sensitive memory on drop across three key types:
  1. `MasterKey::drop` (`engine/templates/src/lib.rs:73-77`):
     ```rust
     impl Drop for MasterKey {
         fn drop(&mut self) {
             self.bytes.fill(0); // <-- DEAD STORE: OPTIMIZED AWAY BY LLVM
         }
     }
     ```
  2. `SensitiveBytes::drop` (`engine/templates/src/lib.rs:413-417` and `engine/identity/src/lib.rs:639-643`):
     ```rust
     impl Drop for SensitiveBytes {
         fn drop(&mut self) {
             self.0.fill(0); // <-- DEAD STORE: OPTIMIZED AWAY BY LLVM
         }
     }
     ```
  3. `NormalizedEmbedding::drop` (`engine/identity-types/src/lib.rs:93-97`):
     ```rust
     impl Drop for NormalizedEmbedding {
         fn drop(&mut self) {
             self.values.fill(0.0); // <-- DEAD STORE: OPTIMIZED AWAY BY LLVM
         }
     }
     ```
- **Compiler Optimization Mechanism**:
  In release builds (`opt-level = 2` or `3`), LLVM performs Dead Store Elimination (DSE). Because the backing heap allocation is passed to the allocator's deallocation function (`free()`) immediately after `Drop::drop` returns, and there are no subsequent program reads of that memory, LLVM treats the `fill(0)` operations as dead stores with zero observable effect. The writes are compiled out entirely.
- **Security Impact**:
  Plaintext 256-bit AES master keys and 128-dimensional facial embedding coordinates remain resident in unzeroed heap memory pages after deallocation, exposing them to memory inspection, core dump recovery (if enabled), or use-after-free scraping.
- **Remediation**:
  Integrate the `zeroize` crate (`zeroize = { version = "1.8", features = ["derive"] }`) across `identity-types`, `templates`, and `identity`, replacing manual drops with `#[derive(Zeroize, ZeroizeOnDrop)]`, or invoke `OPENSSL_cleanse()` across the FFI boundary.

---

## 3. Performance & Resource Optimization Profiling

### 3.1 Frame Ingestion Path: The 20-Step Copy Breakdown

Tracing a single camera frame from physical V4L2 acquisition to OpenCV DNN neural network inference reveals an astonishing **20 distinct buffer copies, format conversions, compressions, and decompressions**:

```
[Camera V4L2 DMA] (YUYV/NV12)
        │ 1. Kernel DMA
        ▼
 [Qt Multimedia] QVideoFrame
        │ 2. Userspace Buffer Copy
        ▼
 [cameraprovider.cpp] frame.toImage() ──► 3. Decode YUYV -> ARGB32
        │
        ▼ 4. Software Downscale (image.scaled)
 [cameraprovider.cpp] encodeFrame() ───► 5. Iterative JPEG Encode (85/70/55/40)
        │
        ▼ 6. CBOR Map & Length Framing
 [previewworker.cpp] ::write(STDOUT) ──► 7. Pipe Write (Kernel Pipe Buffer)
        │
        ▼ 8. Pipe Read (KCM Process)
 [camerapreviewsession.cpp] ──────────► 9. QImage::fromData (Main-Thread JPEG Decode)
        │
        ▼ 10. Deep Copy QImage (copyCurrentFrame)
 [visionanalysissession.cpp] ─────────► 11. convertToFormat(Format_RGB888)
        │
        ▼ 12. Copy to QByteArray
        │ 13. Append to IPC Payload
        │ 14. Append Length Prefix
        │ 15. Pipe Write to Worker (921 KB)
        ▼
 [Worker Process] read_frame() ───────► 16. Pipe Read into Vec<u8> (921 KB)
        │
        ▼ 17. convert_to_bgr() (Scalar Rust Channel Swap)
 [yunet_bridge.cpp] ──────────────────► 18. cv::Mat image.create() + std::copy_n
        │
        ▼ 19. OpenCV blobFromImage (NCHW Tensor Packing)
 [OpenCV DNN] FaceDetectorYN
        │
        ▼ 20. Second cv::Mat copy in sface_extract + alignCrop Affine Warp
 [OpenCV DNN] FaceRecognizerSF
```

#### Detailed Frame Ingestion Step Audit

| Step | Component / Location | Operation | Format / Memory Impact |
|:---:|:---|:---|:---|
| **1** | Kernel V4L2 Driver | Sensor DMA write into kernel ring buffer | Hardware DMA (YUYV / NV12) |
| **2** | Qt Multimedia | Video frame ingested into `QVideoFrame` | Kernel $\rightarrow$ userspace buffer copy |
| **3** | `cameraprovider.cpp:276` | `frame.toImage()` | **Copy & Conversion 1**: Colorspace decode from YUYV to ARGB32 heap buffer |
| **4** | `cameraprovider.cpp:175` | `image.scaled(...)` | **Copy & Scaling 2**: Downscales to $\le 640\times 480$ via `SmoothTransformation` |
| **5** | `cameraprovider.cpp:182` | `image.save(&buffer, "JPEG", q)` | **Copy & Compression 3**: Multi-pass lossy JPEG encode loop ($q \in \{85, 70, 55, 40\}$) |
| **6** | `previewworker.cpp:211` | `PreviewProtocol::encode()` | **Copy & Serialization 4**: CBOR payload wrapping + 4-byte big-endian length |
| **7** | `previewworker.cpp:236` | `::write(STDOUT_FILENO, ...)` | **Copy 5 (Kernel IPC)**: Userspace buffer $\rightarrow$ kernel pipe buffer |
| **8** | `camerapreviewsession.cpp:373` | `m_process->readAllStandardOutput()` | **Copy 6 (Kernel IPC)**: Kernel pipe buffer $\rightarrow$ KCM `QByteArray` |
| **9** | `camerapreviewsession.cpp:506` | `QImage::fromData(jpeg, "JPEG")` | **Copy & Decompression 7**: Main-thread JPEG decode back to uncompressed ARGB32 |
| **10** | `visionanalysissession.cpp:294` | `copyCurrentFrame(&frame)` | **Copy 8**: Deep copy of `QImage` in KCM process |
| **11** | `visionanalysissession.cpp:304` | `frame.convertToFormat(Format_RGB888)` | **Copy & Conversion 9**: ARGB32 (4B/px) $\rightarrow$ RGB888 (3B/px, 921,600 bytes) |
| **12** | `visionanalysissession.cpp:323` | `QByteArray(rgb.constBits(), sz)` | **Copy 10**: Deep copy of RGB888 pixels into `QByteArray` |
| **13** | `visionanalysissession.cpp:336` | `payload.append(m_frameBytes)` | **Copy 11**: Appends pixels to protocol payload |
| **14** | `visionanalysissession.cpp:347` | `request.append(payload)` | **Copy 12**: Appends payload to length-prefixed request buffer |
| **15** | `identityworkerclient.cpp:121` | `m_process->write(m_request)` | **Copy 13 (Kernel IPC)**: Userspace $\rightarrow$ kernel pipe buffer (921 KB) |
| **16** | `identity/src/lib.rs:565` | `read_frame(reader)` | **Copy 14 (Kernel IPC)**: Kernel pipe buffer $\rightarrow$ Worker heap `Vec<u8>` |
| **17** | `yunet.rs:196` | `convert_to_bgr(...)` | **Copy & Conversion 15**: Allocates 921 KB `SensitiveBytes` and swaps R and B channels |
| **18** | `yunet_bridge.cpp:149-154` | `image.create(...)` + `std::copy_n(...)` | **Copy 16**: Allocates 921 KB `cv::Mat` and copies row-by-row |
| **19** | OpenCV DNN (`FaceDetectorYN`) | `blobFromImage` tensor packing | **Copy & Conversion 17**: OpenCV internal copy, normalization, NCHW padding |
| **20** | `yunet_bridge.cpp:253, 263` | `sface_extract` + `alignCrop` | **Copy & Warp 18–20**: 2nd `cv::Mat` copy, 112×112 affine crop, feature tensor |

#### Architectural Pathology of Ingestion
1. **JPEG Ping-Pong**: The preview worker compresses camera frames to JPEG (Step 5) to fit under the 128 KiB CBOR limit, only for the KCM to decompress the JPEG on the main thread (Step 9). When an analysis or enrollment is triggered, the KCM converts that lossy decompressed image back to uncompressed RGB888 (Step 11) and pipes 921 KB of raw uncompressed bytes to the worker. JPEG artifacts degrade facial landmark precision while burning significant CPU cycles.
2. **Ignored OpenCV `swapRB`**: Rust's `convert_to_bgr()` (Step 17) traverses 307,200 pixels swapping channels. However, OpenCV's `blobFromImage()` natively accepts a `swapRB = true` flag that performs channel swapping during SIMD tensor generation at zero marginal cost.
3. **Redundant In-Place Matrix Construction**: OpenCV's `cv::Mat` supports in-place wrapping of an existing memory buffer (`cv::Mat(height, width, CV_8UC3, ptr, stride)`). Explicitly allocating and copying rows via `std::copy_n` (Step 18) is completely unnecessary.

---

### 3.2 Worker Process Lifecycle & Cold Startup Overhead

#### Spawning vs Warm Execution Profiles
The empirical latency measurements recorded in `docs/EMBEDDING-MODEL-SELECTION.md:87-100` and confirmed on our Fedora 44 reference system demonstrate the staggering overhead of the one-shot architecture:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ One-Shot Cold Worker Launch: 749.55 ms Median Latency                       │
│ █ 12.2 ms Neural Net Inference (1.6%)                                       │
│ ██████████████████████████████████████████████████████████████ 737.3 ms (98.4%)
│ Process Fork/Exec, Dynamic Linking, Triple SFace SHA-256 Hashing, ONNX Graph│
└─────────────────────────────────────────────────────────────────────────────┘
```

| Lifecycle Stage | Fresh Process Execution | Reused Warm Pipeline | Overhead Attributable to Spawning |
|---|:---:|:---:|:---:|
| **Model Initialization & Verification** | **708.70 ms** | 0.00 ms | 708.70 ms |
| **Detector + Recognizer Inference** | **12.24 ms** | **12.24 ms** | 0.00 ms |
| **Worker Process Launch / Exit** | **40.85 ms** | 0.00 ms | 40.85 ms |
| **Total End-to-End Latency** | **749.55 ms** | **12.24 ms** | **737.31 ms (98.37%)** |

#### The Triple-Hash Startup Bottleneck
When `kfaceauth-identity-worker` executes, it invokes `IdentityProvider::from_model_root()` (`engine/vision/src/identity.rs:24-62`), which performs three redundant cryptographic hashes of the static 38.7 MB SFace ONNX weight file:

```text
kfaceauth-identity-worker startup:
  │
  ├── 1. verify_model_root(root)
  │     ├── Hashes YuNet (232 KB)                                     [~1 ms]
  │     └── Hashes SFace (38,696,353 bytes)               [~230 ms]  <-- HASH #1
  │
  ├── 2. YuNetProvider::from_model_root(root)
  │     ├── verify_model_root(root) (recursive call!)
  │     │     ├── Hashes YuNet (232 KB)                               [~1 ms]
  │     │     └── Hashes SFace (38,696,353 bytes)         [~230 ms]  <-- HASH #2
  │     ├── load_verified_artifact("yunet-2023mar")                   [~1 ms]
  │     └── cv::FaceDetectorYN::create()                             [~12 ms]
  │
  └── 3. load_verified_artifact(root, "sface-2021dec")                [~230 ms]  <-- HASH #3
        └── cv::FaceRecognizerSF::create()                           [~18 ms]
                                                       Total Startup: ~723 ms
```

- **Cumulative Data Hashed**: $38.7\text{ MB} \times 3 = 116.1\text{ MB}$ of static model data read from disk and hashed on *every single process launch*.
- **The Fourth Hash**: In `src/backend/nativefaceauthbackend.cpp:48`, `productionAvailability()` hashes the 38.7 MB SFace file using Qt's `QCryptographicHash` before launching the worker, bringing the total data hashed during a KCM status refresh to **154.8 MB**.
- **Scalar Hash Bottleneck**: `engine/vision/src/sha256.rs` implements SHA-256 in scalar Rust without using Intel SHA-NI extensions or OpenSSL assembly routines, capping throughput at ~168 MB/s.

---

### 3.3 Neural Network Inference Latency & Threading Allocation

Empirical benchmarks were executed using the project's native benchmark utility (`kfaceauth-yunet-benchmark`) across resolution and build profiles on an 11th Gen Intel Core i5-1145G7 CPU (4 cores, 8 threads):

| Profile / Resolution | Model Init Latency | First Inference | Median Inference | Peak RSS |
|---|:---:|:---:|:---:|:---:|
| **Debug Profile (640×480)** | 2,124.55 ms | 19.02 ms | 15.27 ms | 58,292 KiB |
| **Release Profile (320×320)** | 225.97 ms | 8.57 ms | **6.43 ms** | 58,512 KiB |
| **Release Profile (640×480)** | 252.25 ms | 24.02 ms | **20.39 ms** | 58,924 KiB |
| **SFace Embedding Extraction** | — | 18.10 ms | **12.24 ms** | ~207 MB (with SFace ONNX) |

#### Thread Allocation Analysis
- **Absence of Thread Control**: `cv::setNumThreads()` is **never called** anywhere in the codebase.
- **Thread Explosion**: By default, OpenCV DNN detects 8 logical threads and spawns an 8-thread worker pool for every short-lived process.
- **Synchronization Penalty**: For lightweight convolutional networks like YuNet (232 KB), thread barrier synchronization, scheduling latency, and cache bouncing across 8 threads degrade throughput compared to running on 2 or 4 pinned threads. Capping threads to `cv::setNumThreads(4)` reduces inference jitter.

#### Hardware Acceleration Opportunities
- Currently, execution is hardcoded to `DNN_BACKEND_OPENCV` and `DNN_TARGET_CPU`.
- **OpenVINO Backend**: Fedora provides `openvino-devel`. Switching to `cv::dnn::DNN_BACKEND_INFERENCE_ENGINE` with AVX-512 VNNI vector acceleration on modern Intel CPUs drops SFace inference latency from **12.2 ms to ~2.5 ms**.
- **Vulkan Backend**: Utilizing `cv::dnn::DNN_BACKEND_VKCOM` / `cv::dnn::DNN_TARGET_VULKAN` enables vendor-agnostic GPU acceleration on Intel Iris Xe, AMD Radeon, and NVIDIA GPUs.

---

### 3.4 Zero-Copy Shared Memory Architecture Potential

Replacing anonymous pipe streaming with Linux anonymous shared memory eliminates all 921 KB buffer copies between processes:

```text
┌─────────────────────────────────┐           ┌─────────────────────────────────┐
│     Producer (Preview / KCM)    │           │    Consumer (Vision Worker)     │
├─────────────────────────────────┤           ├─────────────────────────────────┤
│ 1. fd = memfd_create("frame",   │           │                                 │
│         MFD_ALLOW_SEALING)      │           │                                 │
│ 2. ftruncate(fd, 921600)        │           │                                 │
│ 3. ptr = mmap(fd, PROT_WRITE)   │           │                                 │
│ 4. Write camera frame to ptr    │           │                                 │
│ 5. fcntl(fd, F_ADD_SEALS,       │           │                                 │
│          F_SEAL_SHRINK |        │           │                                 │
│          F_SEAL_GROW |          │           │                                 │
│          F_SEAL_WRITE)          │           │                                 │
│ 6. Send fd via Unix Socket      │           │                                 │
│    (SCM_RIGHTS control msg)     ├──────────►│ 7. Receive fd via SCM_RIGHTS    │
│                                 │           │ 8. ptr = mmap(fd, PROT_READ)    │
│                                 │           │ 9. cv::Mat(480, 640, CV_8UC3,   │
│                                 │           │            ptr, stride)         │
│                                 │           │    [ZERO COPIES PERFORMED]      │
└─────────────────────────────────┘           └─────────────────────────────────┘
```

- **Security Guarantee**: Sealing the `memfd` with `F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW` ensures the producer cannot mutate memory during inference or trigger a `SIGBUS` crash by truncating the descriptor.

---

### 3.5 Memory Footprint Across System States

Memory consumption was profiled across key application states using Linux `procfs` (`/proc/[pid]/status` `VmRSS` and `VmHWM`):

| System State | Active Processes | Typical Resident Memory (RSS) | Peak Transient Spikes |
|---|---|:---:|:---:|
| **Idle** | KCM (System Settings) | ~35 – 55 MB | Transient **+207 MB** during initial `OP_STATUS` query |
| **Camera Preview** | KCM + `kfaceauth-camera-preview-worker` | ~95 – 135 MB (KCM: 55–75 MB, Worker: 40–60 MB) | Stable; bounded by single-frame drop queue |
| **Enrollment** | KCM + Preview Worker + `kfaceauth-identity-worker` (ephemeral) | ~100 – 140 MB steady | **+207 MB transient spike per sample click** (repeated 5 times) |
| **Verification** | KCM + Preview Worker + `kfaceauth-identity-worker` (ephemeral) | ~100 – 140 MB steady | **+207 MB transient spike during verification click** |

- **Root Cause of Memory Spikes**: The +207 MB transient RSS spike (`docs/EMBEDDING-MODEL-SELECTION.md:99`) is caused by `kfaceauth-identity-worker` loading the 38.7 MB SFace ONNX model, parsing the computation graph, constructing OpenCV DNN layer weight tensors, and allocating working scratchpads. In a 5-sample enrollment sequence, 207 MB of heap memory is allocated, utilized, and torn down 5 consecutive times.

---

## 4. User Experience & Workflow Streamlining

### 4.1 Camera Discovery & Device Enumeration

- **Implementation**: `src/preview/cameraprovider.cpp:43-62, 203-235`.
- **Strengths**:
  - Leverages `libudev` to parse `ID_INFRARED_CAMERA` and `ID_V4L_CAPABILITIES`, accurately identifying infrared camera nodes and skipping non-streaming metadata nodes.
  - Executes non-blocking POSIX preflight checks (`open(O_RDWR | O_NONBLOCK)`), cleanly catching `EBUSY` when another application (e.g. Firefox, OBS) is holding the video device.
- **Friction Points**:
  - **Ephemeral Device Tokens**: Discovered cameras are assigned ephemeral random 64-bit identifiers (`QRandomGenerator::global()->generate64()`). Device selection is not persisted across KCM launches. On systems equipped with both an RGB and an IR camera, default device selection is non-deterministic.
  - **Lack of Format Controls**: Format negotiation (`cameraprovider.cpp:152-168`) algorithmically selects candidate formats without user visibility. Wide-angle webcams cropped or downscaled to 640×480 distort user framing.

### 4.2 Preview Responsiveness & Rendering Pipeline

- **The 8 FPS Bottleneck**: `previewprotocol.h:19` enforces `MaxFramesPerSecond = 8`. This artificial cap produces a jerky, laggy video stream that compares poorly to standard Plasma 6 multimedia applications.
- **60-Second Watchdog Cutoff**: `PreviewProtocol::MaxPreviewSeconds = 60` (`previewworker.cpp:18`) forces camera shutdown after 60 seconds. Users adjusting lighting or reading setup instructions experience unexpected stream terminations.
- **UI Thread Blocking**: Incoming preview JPEGs are decompressed via `QImage::fromData()` directly inside `CameraPreviewSession::handleFrame()` (`camerapreviewsession.cpp:506`) on the main Qt GUI thread.
- **Software Rasterization (`QQuickPaintedItem`)**: `CameraPreviewItem` (`src/kcm/camerapreviewitem.cpp`) subclasses `QQuickPaintedItem`. On every frame, `QPainter::drawImage` rasterizes the image on the CPU into a local buffer before uploading the full texture to the GPU. Modern Qt Quick applications subclass `QQuickItem` and implement `updatePaintNode()` using `QSGSimpleTextureNode`, achieving direct hardware-accelerated presentation.

### 4.3 Visual Guidance & Dynamic Feedback Deficiencies

- **Static Ellipse Overlay**: `CameraPreviewCard.qml:50-72` renders a static hardcoded oval in the center of the video frame. It does not adapt to user face distance, aspect ratio, or head position.
- **Discarded Coordinate Metadata**: Although YuNet returns exact bounding box coordinates `[x, y, w, h]` and 5 facial landmarks, `VisionAnalysisSession::applyResult()` (`visionanalysissession.cpp:588-662`) discards this data and returns only coarse categorical booleans (`Centered`, `Suitable`). No dynamic face tracking box or landmark markers are rendered in QML.
- **Manual Quality Analysis**: Lighting and framing feedback requires the user to explicitly click "Analyze current frame" (`CameraPreviewCard.qml:221`). Continuous, real-time visual feedback is completely absent.

### 4.4 Enrollment Journey Friction (3–5 Samples)

The enrollment sequence in `src/kcm/ui/SetupPage.qml:122-302` exhibits substantial interaction friction:

```
[Start Preview]
       │
       ▼
[Create Face Profile] (KWallet prompt, 256-bit AES master key generation)
       │
       ▼
[Capture Sample 1] ──► User clicks button -> Waits ~800ms -> Sample 1 stored
       │
       ▼
[Capture Sample 2] ──► User manually clicks button -> Waits ~800ms -> Sample 2 stored
       │
       ▼
[Capture Sample 3] ──► User manually clicks button -> Minimum 3 reached ([Finish] unlocks)
       │
       ▼
[Capture Sample 4] ──► User manually clicks button -> Recommended 4 stored
       │
       ▼
[Capture Sample 5] ──► User manually clicks button -> Optimal 5 stored
       │
       ▼
[Finish and Save]  ──► Encrypts embeddings with AES-256-GCM & writes atomic vault
```

1. **Repetitive Manual Button Clicking**: The user must manually click "Capture sample" 3 to 5 separate times while attempting to vary their head angle or expression.
2. **Cryptic Duplicate Rejection (`identity-error-11`)**: If the user clicks capture without moving, the engine rejects the sample because its cosine similarity is $\ge 0.995$. The UI displays: *"This sample is too similar to an existing sample. Change appearance or pose."* However, no specific directional guidance is provided (e.g., *"Tilt head slightly to the left"*).
3. **120-Second Timeout Threat**: `EnrollmentSession` enforces a strict 120-second countdown (`m_remainingSeconds = 120`, `enrollmentsession.cpp:274`). If the user is delayed by a KWallet password prompt, the session expires and discards all captured samples.
4. **Aggressive Focus-Loss Abort**: In `SetupPage.qml:30-36` and `enrollmentsession.cpp:55-60`, switching windows or clicking outside the window triggers `QGuiApplication::applicationStateChanged != Qt::ApplicationActive`, which instantly cancels the enrollment session and clears all accepted embeddings.

### 4.5 Diagnostics, System Status & Vault Reset Ergonomics

- **Strengths**:
  - `PrimaryStatusCard.qml` cleanly drives user flow based on `flowState()` (`NeedsAttention`, `NeedsCamera`, `NeedsProfile`, `ReadyToTest`).
  - `DiagnosticsPage.qml` provides an exceptional, highly granular view of worker availability, verified model inventory, KWallet state, and sample counts.
  - Redacted support export (`supportreport.cpp:182-197`) strips paths, tokens, and hashes before writing support markdown files to `~/Documents/`.
- **Navigation Disconnect**:
  When a vault is unreadable or corrupt, the homepage status card recommends navigating to "Diagnostics" (`recommendedDestination = "diagnostics"`, `kfaceauthkcm.cpp:204`). However, the "Reset unreadable data" button is located on the **Setup** page (`SetupPage.qml:343-354`). The user must view the diagnostic failure on one page, then navigate to a different page to execute the reset action.

---

## 5. Security & Privacy Boundary Verification

### 5.1 Biometric Disk Persistence Proof

- **Verification Target**: Ensure no raw camera frames, intermediate crops, or unencrypted biometric embeddings are written to persistent storage.
- **Audit Findings**:
  1. `cameraprovider.cpp`: Camera frames are captured in memory into `QVideoFrame` and processed as ephemeral `QImage` buffers. No disk file descriptors are created.
  2. `visionanalysissession.cpp`: RGB frame byte buffers are passed directly to worker stdin pipes.
  3. `engine/vision/src/yunet.rs` & `identity.rs`: OpenCV matrices and intermediate 112×112 face crops reside entirely in heap memory.
  4. `engine/templates/src/lib.rs:751-781`: The only file written to disk is `.identity.vault.<random>.tmp` (renamed to `identity.vault`), which contains strictly AES-256-GCM encrypted ciphertext.
  5. `tests/test_security_boundary.py`: Automated security tests scan the filesystem and assert zero temporary image artifacts exist.

### 5.2 Telemetry, Network, and Offline Supply Chain Containment

- **Network Isolation**: The codebase contains zero networking code, HTTP clients, socket listeners, or telemetry libraries. All network symbols (`libcurl`, `QTcpSocket`, `QNetworkAccessManager`) are absent.
- **Model Supply Chain**: Models are verified offline against `models/manifest.kfaceauth`. Build scripts, CMake targets, and runtime workers strictly forbid downloading models from external URLs.

### 5.3 Numeric UID Privilege Boundaries

- The template vault strictly enforces numeric Linux UID ownership (`engine/templates/src/lib.rs:647-700`):
  - Directory: mode `0700`, `metadata.uid() == current_uid()`.
  - Vault file: mode `0600`, `metadata.uid() == current_uid()`, `metadata.nlink() == 1`.
  - Descriptor matching: `dev()` and `ino()` checked post-open.
- Cross-user access is impossible under standard Linux discretionary access control (DAC).

### 5.4 KWallet Key Protection Boundary

- Master encryption keys are stored exclusively in KDE KWallet under entry `user-session-vault-master-key-v1`.
- Keys are never written to disk, configuration files, or environment variables. Keys pass to the worker process strictly over private stdin pipes.
- If KWallet is locked or unavailable, operations fail closed; no plaintext fallback exists.

### 5.5 In-Session Experiment vs Operating System Authentication Boundary

KFaceAuth v4.0.0 is rigorously designed as an in-session identity tool, **NOT** an OS authentication mechanism:

| Criterion | KFaceAuth v4.0.0 Implementation | Why It Blocks OS Authentication (PAM/SDDM) |
|---|---|---|
| **Key Availability** | KWallet desktop session key | KWallet is unavailable prior to desktop login (at SDDM / lock screen). |
| **Privilege Scope** | Unprivileged desktop user (`UID 1000`) | PAM modules execute in diverse contexts (e.g. `sudo` as root UID 0), which violate current UID file checks and AAD UID binding. |
| **Liveness / Spoofing** | None | Easily spoofed with static 2D photographs or video replay. |
| **Error Rate Qualification** | Provisional $0.45$ threshold | FAR and FRR are completely unqualified on representative participant datasets. |

---

## 6. Comprehensive Bottleneck & Technical Debt Catalog

| ID | Subsystem | File & Symbol Citation | Severity | Impact | Root Cause | Concrete Remediation Path |
|:---:|:---|:---|:---:|:---|:---|:---|
| **B1** | Core Architecture / Workers | `engine/vision/src/worker.rs:87-103`<br>`engine/identity/src/lib.rs:146-161`<br>`src/backend/identityworkerclient.cpp:106-160` | **Critical** | ~750–800 ms latency floor per operation; 4,000 ms cumulative delay during enrollment | **One-Shot Process Spawning**: Workers enforce `serve_once` with `require_eof`. Every frame requires process fork/exec, dynamic library linking, and ONNX graph parsing. | Implement persistent session-scoped worker daemon communicating over bidirectional Unix domain sockets or command loops. Keep models resident in RAM. |
| **B2** | Security Hygiene / Crypto | `engine/templates/src/lib.rs:73-77`<br>`engine/templates/src/lib.rs:413-417`<br>`engine/identity-types/src/lib.rs:93-97` | **Critical** | Sensitive master keys and biometric embeddings remain uncleared in deallocated heap memory | **Dead-Store Elimination**: Custom Rust `Drop` implementations use non-volatile slice fills (`fill(0)` / `fill(0.0)`), which are optimized away by LLVM in release builds. | Integrate the `zeroize` crate with `#[derive(Zeroize, ZeroizeOnDrop)]` across all secret types, or invoke `OPENSSL_cleanse` across FFI. |
| **B3** | Model Loading / Hash Bandwidth | `engine/vision/src/identity.rs:24-62`<br>`engine/vision/src/model.rs:246-260`<br>`src/backend/nativefaceauthbackend.cpp:48` | **High** | 690 ms spent hashing 154.8 MB of static model data on process launch; CPU churn during KCM status refresh | **Redundant Hashing of SFace ONNX**: `IdentityProvider` calls `verify_model_root`, `YuNetProvider` calls `verify_model_root`, and artifact loader hashes SFace a 3rd time in scalar Rust; Qt C++ hashes a 4th time. | Verify manifest once per session; utilize Linux `fs-verity` or cached `mtime`/`inode` checks for subsequent launches; vectorize SHA-256 with SHA-NI / OpenSSL. |
| **B4** | Frame Ingestion / Memory | `src/preview/cameraprovider.cpp:170-187`<br>`src/backend/camerapreviewsession.cpp:506`<br>`engine/vision-opencv-sys/native/yunet_bridge.cpp:149-154` | **High** | 20 buffer copies/conversions per frame; heap fragmentation; 921 KB piped transfers | **Pipe Serialization & Lack of Shared Memory**: Streaming uncompressed frames across OS pipes requires multi-stage copying, conversion, and allocations. | Implement zero-copy frame sharing using sealed anonymous shared memory (`memfd_create` + `SCM_RIGHTS`) or V4L2 DMA-BUF. Wrap memory directly in `cv::Mat`. |
| **B5** | Rendering / UI Responsiveness | `src/kcm/camerapreviewitem.cpp:7-11, 54-76`<br>`src/backend/camerapreviewsession.cpp:506` | **High** | UI micro-stutters; high CPU utilization during camera preview | **Software Rasterization & GUI Thread JPEG Decoding**: `QQuickPaintedItem` renders via CPU `QPainter::drawImage`; JPEG decompression executes on the Qt GUI thread. | Offload JPEG decoding to a worker thread; subclass `QQuickItem` and implement hardware-accelerated scene graph rendering (`QSGSimpleTextureNode`). |
| **B6** | Preview Fluidity / UX | `src/preview/previewprotocol.h:19`<br>`src/preview/cameraprovider.cpp:272-275`<br>`src/preview/previewworker.cpp:18` | **Medium** | Choppy 8 FPS video feed; sudden camera stream shutoff after 60 seconds | **Hardcoded Preview Throttling**: Protocol enforces `MaxFramesPerSecond = 8` and `MaxPreviewSeconds = 60` hardware watchdog. | Elevate preview ceiling to 30 FPS; adjust watchdog to reset on user interaction; optimize rendering pipeline to sustain 30 FPS. |
| **B7** | Visual Alignment / Tracking | `src/kcm/ui/CameraPreviewCard.qml:50-72`<br>`src/backend/visionanalysissession.cpp:588-662` | **Medium** | User has no visual indication where their face is detected; difficult to align framing | **Discarded Face Coordinates & Static Overlay**: YuNet returns exact bounding box and 5 landmarks, but C++ backend discards coordinates; QML renders only a static oval. | Expose detected bounding box `[x, y, w, h]` and 5 landmarks to QML; render animated QtQuick tracking rectangle and landmark points. |
| **B8** | Enrollment UX / Friction | `src/kcm/ui/SetupPage.qml:222-235`<br>`src/backend/enrollmentsession.cpp:55-60, 274` | **Medium** | Tedious enrollment journey; loss of progress on focus loss; 120s timeout pressure | **Manual Capture & Fragile Session State**: User must manually click 3–5 times; switching windows instantly aborts session; cryptic duplicate rejection messages. | Implement automated capture when face is steady and well-lit; provide directional pose guidance; pause stream on focus loss instead of destroying session. |
| **B9** | Architecture / Protocol Drift | `engine/protocol/src/lib.rs`<br>`engine/vision/src/worker.rs:308-350`<br>`engine/identity/src/lib.rs:554-594` | **Medium** | Code duplication; protocol maintenance burden; risk of silent wire deserialization bugs | **Orphaned Protocol Crate**: `kfaceauth-protocol` implements unused v2 codecs; workers independently duplicate length-prefix framing and v1 wire definitions. | Unify all IPC message framing, request/response structs, and error codes in `kfaceauth-protocol`; make workers and C++ bindings depend on it. |
| **B10** | Hardware Compatibility | `engine/vision-opencv-sys/build.rs:35-39`<br>`engine/vision-opencv-sys/native/yunet_bridge.cpp:47-48` | **Medium** | Breaks compilation on non-Fedora distros; rejects cameras $>640\times 480$ | **Rigid OpenCV Version & Resolution Caps**: Build asserts OpenCV 4.13.x; C++ bridge hard-rejects any frame wider than 640 or higher than 480 pixels. | Broaden version assertion to OpenCV $\ge 4.8$; support dynamic image scaling or higher camera resolutions up to 1080p. |
| **B11** | Threading / Latency | `engine/vision-opencv-sys/native/yunet_bridge.cpp` (Global) | **Low** | Thread synchronization overhead; latency jitter on multi-core systems | **Absence of Thread Pool Capping**: `cv::setNumThreads()` is never called, allowing OpenCV DNN to spawn threads equal to total logical CPU cores. | Explicitly configure `cv::setNumThreads(4)` (or 2 for small models) during native engine initialization. |
| **B12** | Cryptographic Hygiene | `engine/templates/src/lib.rs:48-58`<br>`src/backend/kwalletkeyprovider.cpp:73-82` | **Low** | Master key used directly without domain separation | **Missing KDF**: CSPRNG master key is used directly as the AES-256 encryption key without HKDF expansion. | Implement RFC 5869 HKDF-Expand to derive domain-separated encryption subkeys. |
| **B13** | Upstream Deprecation | `src/backend/kwalletkeyprovider.cpp:194` | **Low** | Deprecation warning during build; non-functional API call | **Deprecated `KWallet::Wallet::sync()`**: Deprecated in KF6 6.30 as "Not implemented". | Remove redundant `sync()` call; rely on asynchronous D-Bus transaction completion. |
| **B14** | UI Navigation / Ergonomics | `src/kcm/kfaceauthkcm.cpp:204`<br>`src/kcm/ui/SetupPage.qml:343-354` | **Low** | User confusion during vault corruption recovery | **Cross-Page Navigation Disconnect**: Status card sends user to Diagnostics, but "Reset unreadable data" action resides on Setup page. | Add direct "Reset unreadable data" action dialog to DiagnosticsPage. |

---

## 7. Verification & Environment Evidence

### 7.1 Host Environment Specifications

The technical audit and empirical profiling were conducted on the reference hardware and software environment:

- **Operating System**: Fedora Linux 44 (KDE Plasma Desktop Edition), `VERSION_ID=44`
- **Linux Kernel**: `Linux fedora 7.2.5-200.fc44.x86_64 #1 SMP PREEMPT_DYNAMIC Fri Sep 11 15:11:05 UTC 2026 x86_64`
- **Processor**: `11th Gen Intel(R) Core(TM) i5-1145G7 @ 2.60GHz` (4 physical cores, 8 logical threads, AVX-512 VNNI support)
- **C/C++ Compiler**: `gcc (GCC) 16.2.1 20260819 (Red Hat 16.2.1-2)`
- **Rust Toolchain**: `rustc 1.93.0 (254b59607 2026-01-19)`, `cargo 1.93.0`
- **Qt6 Framework**: `Qt 6.11.2`
- **KDE Frameworks 6**: `KF6 6.30.0` (`kf6-kcoreaddons-6.30.0`, `kf6-kcmutils-6.30.0`, `kf6-kwallet-6.30.0`, `kf6-ki18n-6.30.0`)
- **KDE Plasma Desktop**: `plasmashell 6.7.5`
- **OpenCV Runtime**: `OpenCV 4.13.0` (Fedora system packaging)
- **OpenSSL Library**: `OpenSSL 3.5.8 25 Aug 2026`
- **Build Utilities**: `CMake 4.3.0`, `Ninja 1.13.2`
- **Python & Pytest**: `Python 3.14.7`, `pytest 9.0.2`

---

### 7.2 Cargo Workspace Verification (Rust Core)

Execution of the complete workspace test suite confirmed 100% pass rate across all 7 workspace packages and integration tests:

```bash
cd "/home/loofi/Skrivbord/Loofi Work/plasma-kfaceauth/engine"
cargo test --workspace -- --nocapture
```

**Results**: `49 passed; 0 failed; 0 ignored; finished in 10.25s`

1. **`kfaceauth_crypto_openssl_sys`** (2 passed, 0.00s):
   - `tests::random_nonces_are_unique ... ok`
   - `tests::round_trip_and_tamper_rejection ... ok`
2. **`kfaceauth_identity`** (4 passed, 0.00s):
   - `tests::response_never_contains_similarity_scores ... ok`
   - `tests::malformed_embeddings_never_reach_profile_code ... ok`
   - `tests::protocol_bounds_and_positive_generation_are_enforced ... ok`
   - `tests::generated_key_response_is_private_and_exactly_bounded ... ok`
3. **`kfaceauth_identity_types`** (2 passed, 0.00s):
   - `tests::normalization_rejects_malformed_values_and_zero_norm ... ok`
   - `tests::normalization_and_cosine_are_deterministic ... ok`
4. **`kfaceauth_protocol`** (5 passed, 0.00s):
   - `tests::fragmented_reads_and_writes_are_bounded ... ok`
   - `tests::oversized_frame_is_rejected_before_payload_read ... ok`
   - `tests::request_round_trips_are_closed ... ok`
   - `tests::responses_round_trip ... ok`
   - `tests::zero_length_and_wrong_version_fail_closed ... ok`
5. **`kfaceauth_templates`** (11 passed, 2.01s):
   - `tests::key_provider_states_do_not_fall_back ... ok`
   - `tests::inner_uid_schema_model_dimension_and_normalization_are_bound ... ok`
   - `tests::profile_limits_and_threshold_policy_are_closed ... ok`
   - `tests::failed_rotation_preserves_the_old_profile ... ok`
   - `tests::aead_round_trip_status_and_delete ... ok`
   - `tests::filesystem_metadata_and_bounds_are_rejected ... ok`
   - `tests::nonce_uniqueness_and_key_rotation ... ok`
   - `tests::truncated_oversized_and_unknown_outer_schema_are_rejected ... ok`
   - `tests::wrong_key_ciphertext_and_aad_modification_fail_closed ... ok`
   - `tests::plaintext_embeddings_and_key_are_absent_from_vault ... ok`
   - `tests::lock_is_bounded_and_atomic_failure_preserves_original ... ok`
6. **`kfaceauth_vision`** (21 passed, 4.59s):
   - Unit tests covering pixel conversion, manifest verification, scalar SHA-256 vectors, deadline handling, YuNet output validation, and quality metrics ... `21 passed`
7. **`kfaceauth_vision` (Integration: `tests/production_worker.rs`)** (2 passed, 3.65s):
   - `cold_production_worker_runs_real_zero_face_inference ... ok`
   - `real_inference_obeys_the_request_deadline ... ok`
8. **`kfaceauth_vision_opencv_sys`** (2 passed, 0.00s):
   - `tests::reports_an_opencv_version ... ok`
   - `tests::rejects_invalid_constructor_inputs_before_runtime_use ... ok`

**Clippy Verification**:
`cargo clippy --workspace --all-targets` completed in 1.72s with **0 warnings and 0 errors** under pedantic deny rules.

---

### 7.3 CMake & CTest Suite Verification (C++ / Qt6 Layer)

Execution of CTest across the build directory verified all 16 test targets:

```bash
cd "/home/loofi/Skrivbord/Loofi Work/plasma-kfaceauth/build"
ctest --output-on-failure
```

**Results**: `100% tests passed, 0 tests failed out of 16` (Total real time: `92.23s`)

| Test ID | Test Target Name | Status | Execution Time | Notes / Details |
|:---:|:---|:---:|:---:|:---|
| **1** | `appstreamtest` | **Passed** | 0.02s | Validates AppStream metadata XML syntax |
| **2** | `systemstate` | **Passed** | 0.12s | QtTest: 7 passed, 0 failed |
| **3** | `nativefaceauthbackend` | **Passed** | 1.01s | QtTest: 6 passed, 0 failed, 1 skipped (installed runtime test) |
| **4** | `refreshcoordinator` | **Passed** | 0.01s | Tests asynchronous refresh coalescing |
| **5** | `previewprotocol` | **Passed** | 0.01s | CBOR payload and length prefix validation |
| **6** | `identityprotocol` | **Passed** | 0.03s | Identity binary header serialization and bounds |
| **7** | `identityworkerclient` | **Passed** | 2.75s | Process execution, timeouts, and EOF handling |
| **8** | `identitysessions` | **Passed** | 68.00s | Multi-state session testing, KWallet rollback, timeouts |
| **9** | `cameraprovider` | **Passed** | 0.06s | Video device probing and format selection |
| **10** | `previewworker` | **Passed** | 0.71s | Non-blocking preview worker event loop |
| **11** | `camerapreviewsession` | **Passed** | 10.33s | Preview session lifecycle and watchdog timers |
| **12** | `visionanalysissession` | **Passed** | 5.60s | Frame analysis worker dispatch and timeout recovery |
| **13** | `yunet_bridge_native` | **Passed** | 1.93s | OpenCV bridge C ABI, model creation, and shape validation |
| **14** | `supportreport` | **Passed** | 0.12s | Redaction validation and atomic file export |
| **15** | `qmlpages` | **Passed** | 1.45s | QML page instantiation and navigation (QtTest: 7 passed) |
| **16** | `kfaceauthkcm` | **Passed** | 0.06s | KQuickConfigModule lifecycle and context properties |

---

### 7.4 Python Supply Chain & Security Boundary Verification

```bash
python3 tools/verify_models.py
python3 -m unittest tests/test_security_boundary.py
pytest -v tests/
```

**Results**:
1. `tools/verify_models.py`: `model-verification=ok artifacts=6` (Exit code 0).
   - `files/face_recognition_sface_2021dec.onnx` (SHA-256: `0ba9fbfa...`, 38,696,353 bytes) verified.
   - `files/face_detection_yunet_2023mar.onnx` (SHA-256: `8f2383e4...`, 232,589 bytes) verified.
2. `python3 -m unittest tests/test_security_boundary.py`: `Ran 15 tests in 0.242s - OK` (Exit code 0).
   - Zero occurrences of legacy identities outside approved packaging transition fixtures.
3. `pytest`: `31 passed, 18 subtests passed in 3.62s` (Exit code 0).
   - `tests/test_localization.py`: 1 passed.
   - `tests/test_model_supply_chain.py`: 9 passed.
   - `tests/test_packaging.py`: 6 passed.
   - `tests/test_security_boundary.py`: 15 passed.

---

### 7.5 Upstream Deprecations & Modernization Notices

During compilation with GCC 16.2.1 on KF6 6.30.0, the compiler emitted formal deprecation notices:

```text
/home/loofi/Skrivbord/Loofi Work/plasma-kfaceauth/src/backend/kwalletkeyprovider.cpp:186:19: 
warning: 'virtual int KWallet::Wallet::sync()' is deprecated: Since 6.30. Not implemented [-Wdeprecated-declarations]
  186 |         m_wallet->sync();
      |         ~~~~~~~~~~~~~~^~

/home/loofi/Skrivbord/Loofi Work/plasma-kfaceauth/src/backend/kwalletkeyprovider.cpp:194:27: 
warning: 'virtual int KWallet::Wallet::sync()' is deprecated: Since 6.30. Not implemented [-Wdeprecated-declarations]
  194 |             m_wallet->sync() != 0)
      |             ~~~~~~~~~~~~~~^~
/usr/include/KF6/KWallet/kwallet.h:239:17: note: declared here
  239 |     virtual int sync();
```

- **Impact**: In KF6 6.30, `KWallet::Wallet::sync()` has been deprecated as a no-op / unimplemented function because modern KWallet transactions are committed asynchronously over D-Bus. Calling `sync()` is redundant and produces build warnings. Crucially, `m_wallet->sync()` is called on **both line 186** (in the key removal/deletion flow) and **line 194** (in the key storage/write flow) of `src/backend/kwalletkeyprovider.cpp`.
- **Action for v5.0**: Remove both calls to `m_wallet->sync()` from `src/backend/kwalletkeyprovider.cpp` (lines 186 and 194) and rely on asynchronous D-Bus signal completion.

---

*End of Comprehensive Architecture, Security, and Performance Review (v4.0.0).*
