# Roadmap

**Reviewed:** 2026-10-05
**Current source version:** 5.3.0
**Release status:** 5.3.0 is the current stable release for local profile
enrollment and explicit comparison. See [release qualification](RELEASE-QUALIFICATION-V5.3.md).
System authentication and biometric suitability remain unqualified.

## Current product

The standard package is an experimental Fedora 44/KDE logged-in-session
utility for guided enrollment and explicit local profile comparison. It does
not authenticate, unlock a session, or authorize system actions.

Version 5.3.0 also contains an opt-in PAM experiment for SDDM and the Plasma
lock screen. It is unqualified and unsupported for login. The default build
and COPR package omit its components, and installation does not enable it.
See [v5.3 qualification](RELEASE-QUALIFICATION-V5.3.md) and the
[threat boundary](THREAT-BOUNDARY.md).

## Current release and remaining work

The v5.3.0 source and Fedora package deliver recoverable enrollment, clearer
status, predictable camera selection, and stronger release integrity for the
logged-in local-profile workflow. The Fedora 44 standard package contains no
authentication components. The opt-in PAM experiment remains unqualified and
unsupported for login.

- Keep the README, release notes, qualification record, package metadata, and
  wiki consistent with the published release. The GitHub release is
  [v5.3.0](https://github.com/loofiboss-bit/LoofiFaceID/releases/tag/v5.3.0),
  and the historical v5.0.0 correction is linked from its release notes.
- Previous v5.2.1 tag [CI](https://github.com/loofiboss-bit/LoofiFaceID/actions/runs/37264564300)
  and [RPM checks](https://github.com/loofiboss-bit/LoofiFaceID/actions/runs/37264564276)
  passed. Release [CI](https://github.com/loofiboss-bit/LoofiFaceID/actions/runs/37265108773)
  and [RPM, lifecycle, and artifact upload](https://github.com/loofiboss-bit/LoofiFaceID/actions/runs/37265108797)
  passed; all four release assets and checksums were verified. The exact
  published SRPM succeeded in [COPR build 11075253](https://copr.fedorainfracloud.org/coprs/build/11075253/),
  and public Fedora 44 repository metadata lists `kfaceauth-5.2.1-1.fc44`.
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
