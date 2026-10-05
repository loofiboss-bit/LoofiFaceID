# LoofiFaceID Improvement Plan

**Updated:** 2026-10-05
**Review baseline:** `v5.2.1`
**Delivery:** v5.3.0 release candidate; publication is pending the tag workflow and public readback. PAM remains an unqualified experiment.

## Product boundary

The standard package remains a logged-in Fedora 44/KDE local-profile utility.
PAM, daemon, auth worker, systemd and SELinux artifacts remain exclusive to the
explicit experimental build/subpackage. Authentication is unsupported and
unqualified. Existing password fallback and active-session preservation apply.

## Implemented product improvements

- Enrollment and camera share an explicit, non-renewing deadline of at most
  300 seconds. Ordinary preview stays at 60 seconds. Cancellation, page hide,
  app inactivity and camera failure still clear transient samples and workers.
- Framing guidance can be retried within an active enrollment without losing
  accepted samples; stale observations cannot trigger automatic capture.
- Replacing an existing profile is confirmed at final save. Atomic failed-save
  preservation remains in place; no biometric history or backup was added.
- Setup, Diagnostics and support reports share typed authentication states;
  configured experimental components do not imply qualification.
- Registration instructions use content-driven height and wrapping step cards.
- Local profile extraction and experimental authentication use explicit purposes.
  Local comparison does not use unqualified spoof heuristics; the authentication
  experiment retains conservative rejection. UI errors make no PAD claim.
- A separate confined auth-worker process owns capture/inference/vault comparison.
  The parent retains UID validation, attempt limits, ingress limits, deadline
  enforcement, child ownership and nonblocking reap. A timed-out kernel D-state
  process may delay recovery; no concurrent camera child is started in that state.
- Camera buffers are zeroizing; protocol requests have exact lengths, valid
  UTF-8/reserved fields and a 269-byte maximum.
- Camera discovery considers actual video nodes, capture/streaming capabilities
  and GREY/YUYV support. Auto-selection requires one compatible node. Generic
  vendor emitter writes have been removed.
- Evaluation uses separated enrollment/probe inputs and the production 3–8
  sample median policy, with aggregate results and bounded biometric memory.
- Source archives use a reviewed explicit file list, portable to unpacked SRPMs;
  canonical version, Cargo and RPM metadata must agree. Release upload verifies
  tag/commit/archive lineage and refuses to overwrite different published bytes.

## Verification record

Validated locally on 2026-10-05:

- Fresh standard and experimental CMake builds: PASS. Full CTest results:
  standard 17/17 and experimental 18/18, including native sanitizers and QML
  geometry/focus checks at 320/480/960 pixels with increased text size.
- Python: 72 tests PASS. Rust workspace/all-target tests, Clippy with warnings
  denied, Cargo formatting, C++ formatting, QML lint, translations and six
  model-integrity checks: PASS.
- Current template (16) and vault-helper (6) test binaries executed as root in
  disposable, network-disabled Fedora containers: PASS. No host authentication
  configuration or installed package was changed.
- Staged standard/experimental installation payload checks: PASS. Standard
  installation has no authentication payload; the experimental worker is opt-in.
- Synthetic CPU YuNet benchmark, 320x320, 3 warmups/20 iterations: cold 1851.821
  ms, warm median 60.503 ms, p95 71.371 ms, peak RSS 66636 KiB. These numbers
  describe synthetic inference on the build host, not camera or login latency.
- Exact v5.3.0 source passed fresh standard and experimental CMake/CTest builds
  and the 72-test Python suite. The local Fedora RPM build and artifact checks
  were performed against v5.2.1, not this candidate; the v5.3.0 tag workflow
  must provide the release-version RPM/SRPM and payload evidence before launch.
  SRPM identity uses SOURCEPACKAGE=1 independently of its build architecture,
  covered by a regression test.

Automated tests establish source behavior and packaging boundaries, not real
camera, accessibility, SELinux login-path behavior, password fallback, or biometric
suitability. Public release upload/readback is pending; upload protection was
verified using mocked release endpoints.

## Remaining qualification

- Manual KCM camera recovery, enrollment/comparison, cancellation, suspend/resume,
  repeated sessions, keyboard, large-text and screen-reader qualification.
- Experimental SELinux Enforcing login-path evidence, real SDDM/Plasma cycles,
  negative decisions and password fallback after every failure condition.
- Consent-based product-policy and presentation-attack evaluation at the attempt
  counts required by `RELEASE-QUALIFICATION-V5.3.md`.
- Runtime/physical review of the new confined child-process path. Source review
  and compiled policy alone cannot qualify it.

The user's previously reported successful login/unlock remains limited smoke
evidence in the current qualification documents. Unobserved cases remain
`NOT RUN` or `unverified`. This work adds no automated logout or locking, new
model, accelerator, inference cache, or qualified authentication target. The
standard package continues to support only the already logged-in local workflow.
