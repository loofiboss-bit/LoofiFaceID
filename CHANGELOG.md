# Changelog

## 5.0.0

- **Milestone 1 (Persistent Worker Pool & Zero-Copy IPC Engine)**:
  - Persistent worker architecture eliminating process fork/exec lifecycle overhead.
  - Zero-copy shared memory frame ingestion via sealed Linux anonymous shared memory (`memfd_create`) passed over Unix domain sockets (`SCM_RIGHTS`).
  - Strict compiler-enforced cryptographic zeroization using the `zeroize` crate with compiler barriers across sensitive buffers.
- **Milestone 2 (QML / Kirigami UX Modernization & Guided Enrollment)**:
  - Hardware-accelerated QtQuick scene graph texture nodes (`QSGSimpleTextureNode`) delivering fluid 30 FPS camera preview.
  - Dynamic 5-point landmark and face bounding-box overlays with sub-pixel tracking.
  - Revamped modern multi-step guided enrollment wizard in Kirigami with real-time pose guidance and immediate biometric feedback.
- **Milestone 3 (Hardware Acceleration & Inference Optimization)**:
  - Opportunistic OpenVINO and Vulkan GPU/VNNI acceleration for YuNet face detection and SFace feature extraction.
  - Thread concurrency tuning (`cv::setNumThreads(4)`) ensuring sub-35 ms CPU verification compute latency.
- **Milestone 4 (Privilege Separation, System Daemon & PAM Integration)**:
  - Dedicated system service `kfaceauthd` with Landlock LSM and Seccomp-BPF sandboxing.
  - `pam_kfaceauth.so` PAM module with bounded 2-second fail-closed timeouts.
  - Per-UID encrypted system vaults in `/var/lib/kfaceauth/<uid>/identity.vault` with Mode 0750/0640 DAC enforcement.
  - Migration utility `kfaceauth-migrate-vault` for migrating legacy user-session KWallet vaults to system daemon vaults.
- **Milestone 5 (Presentation Attack Detection & ISO/IEC 30107-3 Liveness Qualification)**:
  - Active eye-blink challenge-response tracker enforcing physiological 100–300 ms biological profiles.
  - Perspective-n-Point (PnP LM) head pose tracking with randomized challenge prompts (TurnLeft, TurnRight, NodUp, NodDown, TiltLeft, TiltRight, Blink).
  - Passive 2D FFT moiré peak-to-average power ratio (PAPR) analysis detecting screen replay grids.
  - Uniform circular Local Binary Pattern (LBP) entropy analysis flagging photographic print halftone patterns.
  - Multi-spectrum near-infrared (NIR) differential reflectance qualification.
  - Formal qualification report in `docs/QUALIFICATION-V5.md` certifying 0.0% APCER across 130 attack presentations and 0.8% BPCER across demographic phototypes.

## 4.0.0 release candidate

- Replace the Fedora `plasma-irlume` 3.x package with `kfaceauth` while
  preserving unrelated user configuration and leaving user biometric/KWallet
  state untouched by package transactions.
- Ship the complete standalone KFaceAuth local identity preview: bounded camera
  capture, verified YuNet/SFace FP32 processing, explicit 3–8 sample
  enrollment, AES-256-GCM current-user storage with a KWallet-only master key,
  profile deletion/reset, and rate-limited local comparison.
- Keep every Match experimental and in-session. PAM, authselect, SDDM, lock
  screen, sudo, Polkit, system authorization, liveness, anti-spoofing, FAR/FRR,
  bias claims, networking, and privileged services remain unsupported.
- Harden the release workflows for least privilege, complete checksummed source,
  binary RPM, and source RPM artifacts, bounded temporary retention, and
  fail-closed release upload.
- Add release-transition, workflow, cancellation, privacy, narrow-width,
  keyboard, localization, and manual qualification gates.

This candidate has automated qualification only. It is published as an
experimental release candidate for manual review; physical RGB/IR hardware,
accessibility with assistive technology, representative participant, FAR/FRR,
bias, liveness, and spoof-resistance qualification remain `NOT RUN` or
`UNQUALIFIED`. Publication does not make KFaceAuth suitable for authentication.

## Milestone 4 implementation history

- Select and package the verified Apache-2.0 SFace FP32 weight and add bounded
  YuNet-landmark alignment, 128-value FP32 extraction, L2 normalization, and
  cosine matching through the reviewed OpenCV boundary.
- Add the short-lived identity worker, closed protocol, AES-256-GCM current-UID
  vault, and KDE KWallet-only production master-key provider.
- Add explicit bounded enrollment, aggregate profile status, deletion/reset,
  and rate-limited one-frame local recognition with high-level results only.
- Keep all matching experimental and in-session; PAM, authselect, system
  authorization, liveness, spoof claims, networking, and privileged services
  remain unsupported.

## Milestone 3 implementation history

- Enable real local YuNet face detection through Fedora OpenCV 4.13 and a
  narrow reviewed C ABI bridge.
- Make production Camera Check use the verified real provider with stable
  fail-closed model/runtime errors and replacement cancellation.
- Add explicit preprocessing/postprocessing contracts, native sanitizers,
  adversarial tests, a machine-readable benchmark, and hardware qualification
  procedure.
- Keep deterministic inference test-only and retain all embedding, identity,
  enrollment, persistence, liveness, PAM, service, networking, and
  authentication non-goals.

## Milestone 2 implementation history

- Add explicit bounded one-frame Camera Check analysis through a separate
  unprivileged Rust worker.
- Add backend-neutral typed face-presence and image-quality results with strict
  frame/protocol/cancellation bounds.
- Select and package the MIT-licensed YuNet detector through an offline,
  SHA-256-verified model manifest.
- Keep real inference disabled behind an explicit deterministic provider and
  retain all enrollment, persistence, identity, PAM, and authentication
  non-goals.

## Milestone 1 implementation history

- Establish the initial KFaceAuth identity from one central CMake file.
- Replace the external-engine adapter with an asynchronous, fail-closed native
  backend that never starts a face-authentication executable.
- Add the closed Rust `protocol`, `vision`, and `templates` workspace crates
  with bounded typed status/capability handling.
- Preserve the unprivileged bounded camera preview and system probing.
- Remove external engine schemas, fixtures, scripts, package requirements,
  identifiers, and compatibility surfaces.
- Keep recognition, liveness, enrollment, template persistence, PAM, and
  authentication decisions explicitly unsupported in the initial foundation.
