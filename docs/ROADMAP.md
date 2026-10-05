# Roadmap

**Reviewed:** 2026-10-05
**Current source version:** 5.2.1 candidate
**Release status:** v5.2.0 is published; its COPR rebuild failed on a missing
standard build dependency. The v5.2.1 candidate corrects it; PAM remains
unsupported.

## Current product

The standard package is an experimental Fedora 44/KDE logged-in-session
utility for guided enrollment and explicit local profile comparison. It does
not authenticate, unlock a session, or authorize system actions.

Version 5.2.1 also contains an opt-in PAM experiment for SDDM and the Plasma
lock screen. It is unqualified and unsupported for login. The default build
and COPR package omit its components, and installation does not enable it.
See [v5.2 qualification](RELEASE-QUALIFICATION-V5.2.md) and the
[threat boundary](THREAT-BOUNDARY.md).

## Current release and remaining work

The v5.2.0 source and Fedora package were published for the explicit
logged-in local-profile workflow. Its Fedora build and release checks passed,
but the COPR rebuild failed because the standard spec omitted `systemd-devel`
for `libudev`. The v5.2.1 candidate adds that requirement. Stable package
status does not extend to the opt-in PAM experiment, which remains unqualified
and unsupported for login.

- Keep the README, release notes, qualification record, package metadata, and
  wiki consistent with the published release. The GitHub release is
  [v5.2.0](https://github.com/loofiboss-bit/LoofiFaceID/releases/tag/v5.2.0),
  and the historical v5.0.0 correction is linked from its release notes.
- The exact v5.2.0 tag passed [Fedora 44 standard/opt-in CI](https://github.com/loofiboss-bit/LoofiFaceID/actions/runs/37261632933)
  and [RPM build, lint, and payload checks](https://github.com/loofiboss-bit/LoofiFaceID/actions/runs/37261632931).
  The release-triggered [artifact upload](https://github.com/loofiboss-bit/LoofiFaceID/actions/runs/37262206660)
  also passed; see the qualification record for the exact asset manifest.
- Preserve the passing root-only vault ownership and rollback regressions in
  CI; the Fedora test job must assert that its container runs as root.
- Complete KCM hardware/accessibility, PAM login, SELinux, device/session, and
  consent-based physical qualification only on the documented test system.

## Qualification tracks

- **KCM local-session track:** manually qualify camera recovery, enrollment,
  comparison, cancellation, session cycles, keyboard use, screen-reader labels,
  and window scaling on the documented Fedora 44/KDE baseline.
- **PAM experiment track:** require automated transaction tests, independent
  security review, Enforcing-mode SELinux evidence, real SDDM and lock-screen
  cycles, password fallback, device/session coverage, and consent-based
  physical evaluation before any support claim.

Unobserved results remain `unverified`. A local comparison or successful build
does not qualify login or presentation-attack detection. See
[hardware qualification](HARDWARE-QUALIFICATION.md) for the manual matrices.

## Historical engineering documents

- [ROADMAP-V5.md](ROADMAP-V5.md) is the archived v5.0 technical roadmap.
- [REVIEW-V4.md](REVIEW-V4.md) and [V4-QUALIFICATION-REPORT.md](V4-QUALIFICATION-REPORT.md)
  describe earlier source and qualification states.
- Versioned v5.0 and v5.1 qualification files are historical records; the
  v5.2 report is the current PAM experiment status.
