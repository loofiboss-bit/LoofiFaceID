# v5.1.0 qualification status

**Status: unreleased development target; not release-qualified. Face unlock is not shipped.**

## Supported scope

The supported application remains a logged-in user's local profile and explicit
comparison utility. Registration uses a separate five-pose guide, saves only
after an explicit action, and does not write camera images. The v5.1.0 changes
below improve detector error classification and recovery. They do not add KDE
lock-screen authentication.

The default CMake build and Fedora base RPM omit the experimental system daemon,
PAM module, systemd units, sysusers configuration, and SELinux source files.
The experimental build option is off by default and must stay off in release
packaging. Installation does not modify PAM, SDDM, Polkit, sudo, or the active
NoxForge theme.

## Code-level changes and limits

- YuNet edge clipping, invalid detector output, and runtime failure use distinct
  worker errors. Partly clipped faces are rejected with actionable edge
  guidance; impossible geometry, non-finite values, invalid scores, and invalid
  landmarks remain rejected.
- Continuous guidance retries recoverable worker errors up to three consecutive
  times, clears frame/result data, and then stops with a typed error. Protocol
  corruption, timeout, and worker crashes still fail closed immediately.
- Error code 12 remains a legacy catch-all. It cannot identify whether the old
  worker saw invalid output or a runtime failure; the UI now says this instead
  of recommending an unsupported OpenCV reinstall.
- The experimental daemon no longer exposes key-export, caller-provided frame
  verification, or profile deletion operations. Requests are bound to the exact
  UID reported by `SO_PEERCRED`. Key lookup is read-only; only a separate
  explicit provisioning call can store a key, and the old migration CLI is not
  built.
- The daemon/PAM path still lacks a fresh separate unlock-profile enrollment,
  password-confirmed activation/deactivation state, a robust per-user attempt
  limiter, working camera capture, and active randomized challenge-response.
  Its existing PAD primitives are not physical attack evidence; analysis errors
  fail closed but no unlock profile can be qualified from this state. These
  components remain out of the base package.

## Release gates

| Gate | Requirement | Current status |
|---|---|---|
| Registration recovery | Automated and manual capture, cancel, timeout, rejected sample, then continued registration | Code-level recovery tests are added; physical workflow not yet qualified |
| KDE integration | A compatible KScreenLocker/Plasma lock-screen action and dedicated screen-lock PAM service; password path unchanged | **Open**; current project does not include the required KDE lock-screen change |
| Security boundary | Independent review of identity binding, key handling, rate limits, enrollment/activation, PAM trust, fail-closed behavior | **Open**; known missing features above block unlock |
| Physical functionality | At least 20 physical registration/unlock cycles across at least two RGB cameras and three lighting conditions; busy/missing camera, timeout, resume-after-sleep, multi-user, activation/deactivation and password fallback | **Not run** |
| Print attack resistance | At least 300 consent-based photo presentations and APCER <= 1% | **Not run** |
| Screen replay resistance | At least 300 consent-based screen presentations and APCER <= 1% | **Not run** |
| Bona fide users | BPCER <= 5% | **Not run** |
| Other-person comparisons | At least 3,000 comparisons and false-match rate <= 0.1% | **Not run** |
| Privacy | Report aggregate results and conditions only; retain no face images | **Not run** |

Internal tests do not establish ISO/IEC 30107-3 certification. Qualification
requires separate, consent-based test sets and documented conditions. A future
release must keep face unlock disabled if any KDE, security, or physical gate is
open. Password unlock must remain available throughout testing.

## Result

Do not describe v5.1.0 as supporting face unlock, liveness qualification,
anti-spoofing performance, or system authentication. The tracked source changes
are not a public release, and no tag, RPM publication, or host PAM change has
been made.
