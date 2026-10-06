# LoofiFaceID Improvement Plan

**Updated:** 2026-10-06
**Review baseline:** initial plan `v5.2.1`; published baseline `v5.3.0`
**Delivery:** v5.3.0 is published on GitHub and available from the Fedora 44 COPR repository; release artifacts and repository metadata were read back. PAM remains an unqualified experiment.

The repository follow-up below targets a future v5.3-series build. It does not
change the already-published v5.3.0 tag or artifacts.

## 2026-10-06 repository follow-up

The experimental daemon now reads an optional root-owned
`/etc/kfaceauth/kfaceauth.conf` through its packaged systemd unit. Administrators
can set `KFACEAUTH_CAMERA_DEVICE` to a stable V4L2 device path when automatic
selection correctly refuses multiple compatible cameras. The standard package
still excludes all authentication components, and no device-specific path is
shipped.

Verification passed: all 75 Python tests, systemd unit validation, canonical
project identity, and staged standard/experimental package-boundary checks. No
new camera capture or lock-screen authentication attempt was run for this
source-only change; hardware behavior remains unverified.

## 2026-10-06 login and unlock integration

The experimental source now separates installed integration from each user's
SDDM sign-in and Plasma-unlock choice. A root-owned policy stores Off, On
activity, or Button only per UID and target. Normal `sddm` and `kde` password
PAM services remain unchanged; dedicated face services are prepared only by
the administrative transaction. The former exact LoofiFaceID global PAM block
is removed with rollback if the migration cannot complete safely.

Profile replacement revokes both targets before swapping the shared system
copy, then restores the independent per-target choices after integrity checks.
Profile deletion fails if system revocation fails. The KCM has a dedicated
Login and unlock page showing component/theme availability, activation, and
system-profile freshness as separate states. Freshness stays Unknown whenever
the installed copy cannot be compared safely. Face access remains Off by
default and only an experimental package build contains the PAM components.

Daemon requests are target-bound, use a two-second absolute deadline, expose
only fixed progress states, and cancel/reap the camera worker when its PAM
client disconnects. Late worker success is discarded after cancellation or
when the accepted policy mode changes. Version-pinned downstream patches and build/check instructions
are kept in `integrations/sddm` (SDDM 0.21.0) and `integrations/kscreenlocker`
(KScreenLocker 6.7.5). Both integrations keep the standard password path and
require a theme API handshake before face UI can run.

Source verification passes the standard and experimental local build/test
paths. No physical camera capture, SDDM login, Plasma unlock, SELinux Enforcing
interaction, assistive-technology session, or package installation was run for
this source-only implementation; those checks remain unverified. On this
Fedora host, the currently configured SDDM theme is NoxForge and does not
declare the required face-authentication API, so the KCM should keep that path
unavailable until a compatible theme is selected.

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
  and GREY/YUYV support. Auto-selection requires one compatible node; the
  experimental service now reads an optional root-owned config file so an
  administrator can select a stable path when multiple compatible nodes exist.
  Generic vendor emitter writes have been removed.
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
  and the 72-test Python suite. Fedora 44 tag CI passed for both configurations.
  Recovery workflow [37349441222](https://github.com/loofiboss-bit/LoofiFaceID/actions/runs/37349441222)
  rebuilt the tagged standard RPM and SRPM, checked payloads and lifecycle, then
  verified tag/commit/archive lineage and byte-for-byte release readback. The
  public GitHub release contains the source archive, standard RPM, source RPM,
  and `SHA256SUMS`; the opt-in authentication RPM remains CI-only. SRPM identity
  uses SOURCEPACKAGE=1 independently of build architecture, covered by a
  regression test.
- COPR build [11080464](https://copr.fedorainfracloud.org/coprs/build/11080464/)
  succeeded for Fedora 44 x86_64. Public primary repository metadata lists
  `kfaceauth-5.3.0-1.fc44`.

Automated tests establish source behavior and packaging boundaries, not real
camera, accessibility, SELinux login-path behavior, password fallback, or
biometric suitability. The public asset and COPR readbacks establish delivery,
not authentication qualification.

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
