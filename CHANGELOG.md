# Changelog

## Unreleased (v5.3 series)

- Replace global, password-triggered face PAM with independent root-owned
  per-user SDDM and Plasma modes: Off, on activity, or button only. Ordinary
  password PAM remains unchanged, and the experimental default stays Off.
- Add dedicated face-auth PAM services and a recoverable migration away from
  LoofiFaceID's former global `auth sufficient` rules. System-profile sync,
  revoke, replacement, and deletion now update per-user policy transactionally.
- Bound each face request to two seconds and stop/reap its isolated worker when
  the greeter disconnects or cancels. Fixed progress messages never authorize;
  only the current successful PAM transaction can do so.
- Add version-pinned opt-in integration patch series and build instructions for
  SDDM 0.21.0 and KScreenLocker 6.7.5. Theme/API handshakes keep unsupported
  themes on the password path.
- Automated integration checks do not qualify physical camera, SELinux,
  accessibility, SDDM login, or Plasma unlock behavior.
- Load an optional administrator-owned camera selection file for the
  experimental authentication daemon, so systems with multiple compatible
  V4L2 nodes can select one stable device path without changing PAM rules.

## 5.3.0 — Reliable local enrollment and release delivery

- Give explicit enrollment one shared five-minute camera/session deadline while
  retaining the one-minute ordinary preview limit and cancellation cleanup.
- Retry failed framing guidance without discarding accepted session samples;
  confirm replacement at save, improve narrow layouts, and unify experimental
  authentication status across Setup, Diagnostics, and support reports.
- Isolate experimental authentication in a killable, confined worker process;
  bound the daemon protocol and zeroize owned camera buffers.
- Separate local profile extraction from unqualified spoof heuristics; keep
  conservative heuristic rejection in the authentication experiment.
- Select only a unique compatible camera automatically and remove generic
  vendor emitter controls.
- Evaluate actual multi-sample profile decisions using separated enrollment
  and probe data, with aggregate-only reporting and bounded biometric memory.
- Build archives from an explicit source manifest; check version consistency,
  release lineage, and immutable published artifact readback.
- No camera, accessibility, PAM login, presentation-attack, or biometric
  suitability qualification is claimed by this release.

## 5.2.1 — Fedora packaging correction

- Declare `systemd-devel` as a standard build requirement so clean Fedora
  buildroots provide the `libudev` development metadata used by CMake.
- No runtime or authentication behavior changes. PAM remains opt-in,
  unsupported, and unqualified for login.

## 5.2.0 — Stable local-session release; PAM remains experimental

- Publish the Fedora 44/KDE local-profile workflow as the current stable
  package release. Stable release status does not qualify face comparison as
  an authentication or presentation-attack defense.
- Restrict the privileged vault helper to fixed production paths and the
  verified `PKEXEC_UID`; protect the system vault from user-owned replay.
- Enforce an absolute request deadline and bounded daemon ingress while keeping
  camera capture and inference serialized.
- Keep helper success output static and free of user identifiers or profile
  metadata.
- Run the standard and explicit experimental-auth CI and RPM paths separately;
  the standard package remains free of authentication components.
- Keep PAM login, SELinux runtime, physical attack, and hardware qualification
  outside the stable package claim until their documented gates pass.

The 5.2.0 package changelog entries below record the implementation history of
the separately opt-in authentication experiment. That path remains disabled,
unsupported, and unqualified for login.

- Use Fedora 44's `v4l_device_t` camera label and keep the socket type compatible
  with the policy by using the `file_type` attribute only.
- Package SELinux file contexts as concrete contexts so Fedora can parse and
  activate the opt-in modules without a macro expansion step.
- Repair activation when an older unmanaged PAM rule is already present: the KCM
  now reports only a valid managed rule as enabled, and the explicit
  administrator operation safely adopts the exact rule only when the later
  `password-auth` fallback is intact.
- Refuse to replace an existing pre-session profile when its separate key is
  missing.
- Make the existing SDDM and KDE Plasma lock-screen PAM path available only
  through the explicit `kfaceauth-experimental-auth` RPM subpackage and the
  `KFACEAUTH_BUILD_EXPERIMENTAL_AUTH_COMPONENTS=ON` CMake option.
- Keep the default build and COPR package free of the PAM module, daemon,
  authentication units, privileged setup helper, and SELinux policy.
- Separate the pre-session system profile and randomly generated key from the
  logged-in user's KWallet profile and key.
- Configure either SDDM or Plasma lock-screen PAM independently and preserve
  the system password stack as the fallback.
- Scope the SELinux file-context rule to the socket, leaving
  `/run/kfaceauth` at its platform default label for systemd sandbox setup.
- Keep the feature experimental and unqualified. IR camera selection is not
  liveness or presentation-attack evidence; physical testing and independent
  security review remain open.

## 5.1.0 (local registration stabilization)

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
evidence. The opt-in v5.2.0 authentication components remain experimental;
see `docs/RELEASE-QUALIFICATION-V5.2.md`.

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
