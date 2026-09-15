# KFaceAuth 4.0.0 release checklist

This checklist describes the v4.0.0 candidate boundary. It does not authorize
a commit, tag, push, repository rename, release, or public upload by itself.

## Local candidate closure

- [ ] Confirm `cmake/ProjectIdentity.cmake` remains version `4.0.0` and all
      generated metadata uses the KFaceAuth/kfaceauth identity.
- [ ] Confirm the worktree diff contains no secrets, biometric material,
      generated archives, fake production provider, or unrelated changes.
- [ ] Run the CMake build and offscreen CTest suite.
- [ ] Run Python tests, QML lint, Swedish `msgfmt`, C++ formatting, Cargo fmt,
      locked/offline Cargo clippy/tests, and model verification.
- [ ] Build the reproducible source archive and verify its deterministic hash.
- [ ] Build/lint the Fedora 44 RPM and SRPM when the Fedora 44 toolchain is
      available.
- [ ] Run the isolated v3-to-v4 RPM transition and clean install/reinstall/
      remove smoke test; verify unrelated home and PAM sentinels survive.
- [ ] Verify package contents contain only ordinary-user workers and no
      privileged, authentication, network, evaluator, or fake-provider path.

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

## B. Authentication suitability — blocked

- [ ] Do not mark PAM, authselect, SDDM, lock-screen, sudo, Polkit, or system
      authorization complete.
- [ ] Do not mark pre-login key access complete.
- [ ] Do not claim FAR, FRR, demographic/bias qualification, liveness,
      presentation-attack/spoof resistance, or authentication suitability.

Section B remains `UNQUALIFIED` for v4.0.0, even if Section A passes.

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
