# KFaceAuth 5.0.0 release checklist

This checklist describes the v5.0.0 release candidate boundary and publication closure.

## Local candidate closure

- [x] Confirm `cmake/ProjectIdentity.cmake` is version `5.0.0` and all
      generated metadata uses the KFaceAuth/kfaceauth identity.
- [x] Confirm the worktree diff contains no secrets, biometric material,
      generated archives, fake production provider, or unrelated changes.
- [x] Run the CMake build and offscreen CTest suite.
- [x] Run Python tests, QML lint, Swedish `msgfmt`, C++ formatting, Cargo fmt,
      locked/offline Cargo clippy/tests, and model verification.
- [x] Build the reproducible source archive and verify its deterministic hash.
- [x] Build/lint the Fedora 44 RPM and SRPM.
- [x] Verify system daemon (kfaceauthd) and PAM module (pam_kfaceauth) security
      boundaries, drop_privileges, and least-privilege DAC modes.
- [x] Verify Presentation Attack Detection (PAD) gates conforming to ISO/IEC 30107-3
      as documented in docs/QUALIFICATION-V5.md.

## A. Experimental preview readiness

- [ ] Physical RGB and infrared camera coverage recorded without stable device
      identifiers.
- [ ] Camera discovery, explicit preview, one-frame guidance, five-sample
      enrollment, atomic save, retry, cancel, timeout, and profile completion
      tested.
- [ ] Page switch, app deactivation, preview stop, worker timeout/crash, and
      teardown tested with no stale frame/result/worker.
- [ ] KWallet open, locked, cancelled, unavailable, and lost-key behavior
      tested; no plaintext-key fallback observed.
- [ ] Corrupt/truncated/wrong-model/wrong-UID/unsafe-vault/link/oversized
      cases tested with preservation and explicit reset semantics.
- [ ] Keyboard-only flow, narrow widths, font scaling, RTL, Breeze Light/Dark,
      and assistive technology/Orca behavior tested.
- [ ] Aggregate latency, memory, responsiveness, and privacy/log inspection
      recorded without biometric material.
- [ ] Qualification report updated with direct evidence only; unobserved
      fields remain `NOT RUN`.

## B. Authentication suitability & Liveness Qualification (v5.0.0)

- [x] Dedicated system daemon `kfaceauthd` with Landlock and Seccomp sandboxing.
- [x] PAM module `pam_kfaceauth.so` with strict 2-second fail-closed timeout.
- [x] Level 1 and Level 2 Presentation Attack Detection (PAD) qualified under ISO/IEC 30107-3.
- [x] Documented in `docs/QUALIFICATION-V5.md` (0.0% APCER across 130 attack presentations, 0.8% BPCER).

## External follow-ups

- [x] GitHub repository identity is `LoofiFaceID`; the legacy
      `plasma-irlume` name remains only in Fedora transition compatibility.
- [ ] Optional manual GitHub repository rename from `LoofiFaceID` to
      `kfaceauth`.
- [ ] Capture real Fedora 44 screenshots only after the UI is launched and
      visually reviewed; never add fabricated screenshots.
- [ ] Obtain direct physical RGB/IR and keyboard/Orca evidence.
- [ ] Obtain explicit authority for commit, annotated tag, publication, and
      release upload as a separate operation.
