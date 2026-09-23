# Changelog

## 5.1.0 (unreleased; local registration stabilization)

- Distinguish clipped face geometry, invalid detector output, and vision runtime failures in the worker protocol.
- Keep guided vision sessions alive after a recoverable frame error, with a bounded stop after repeated failures.
- Replace unsupported OpenCV reinstall advice with error-specific recovery guidance.
- Remove key export and broad verify/delete operations from the experimental daemon protocol.
- Make key lookup read-only: status and authentication requests cannot create key material.
- Keep daemon, PAM, and system service artifacts out of the default build and Fedora base package.
- KDE lock-screen integration, fresh unlock-profile enrollment, and physical attack/user qualification remain unimplemented or unverified. Face unlock is not shipped.

## 5.0.0 historical claims correction

The earlier 5.0.0 changelog overstated system authentication, active PAD,
ISO/IEC qualification, acceleration, and physical test results. Those claims
are not supported by the shipped user workflow or current qualification
evidence. The supported product remains an experimental current-user profile
and local comparison utility; see `docs/RELEASE-QUALIFICATION-V5.1.md`.

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
