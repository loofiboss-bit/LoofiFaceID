# KFaceAuth 5.0.0 release checklist

> The current end-user release is the experimental Fedora 44/KDE local
> profile/comparison KCM. PAM, daemon, PAD, accelerator, and physical
> qualification items below are historical or future gates unless independently
> rerun; they must not be presented as current capabilities.

This checklist describes the v5.0.0 release candidate boundary and publication
closure. Checked items inherited from an earlier candidate do not qualify the
current working tree; rerun them and record direct evidence before release.

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
- [ ] Historical/future gate: verify system daemon (kfaceauthd) and PAM module
      (pam_kfaceauth) security boundaries, drop_privileges, and least-privilege
      DAC modes before any authentication release.
- [ ] Historical/future gate: reproduce Presentation Attack Detection (PAD)
      evidence against ISO/IEC 30107-3; `docs/QUALIFICATION-V5.md` is archival
      and not current release evidence.

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

- [ ] Future authentication gate: dedicated system daemon `kfaceauthd` with
      Landlock and Seccomp sandboxing, independently reproduced for the target
      release.
- [ ] Future authentication gate: PAM module `pam_kfaceauth.so` with strict
      2-second fail-closed timeout, independently reproduced for the target
      release.
- [ ] Future authentication gate: Level 1 and Level 2 Presentation Attack
      Detection (PAD) qualification under ISO/IEC 30107-3.
- [ ] Historical report retained in `docs/QUALIFICATION-V5.md`; its APCER,
      BPCER, and physical results are not current evidence.

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
