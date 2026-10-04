# Roadmap

**Reviewed:** 2026-10-04
**Current source version:** 5.2.0

## Current product

The standard package is an experimental Fedora 44/KDE logged-in-session
utility for guided enrollment and explicit local profile comparison. It does
not authenticate, unlock a session, or authorize system actions.

Version 5.2.0 also contains an opt-in PAM experiment for SDDM and the Plasma
lock screen. It is unqualified and unsupported for login. The default build
and COPR package omit its components, and installation does not enable it.
See [v5.2 qualification](RELEASE-QUALIFICATION-V5.2.md) and the
[threat boundary](THREAT-BOUNDARY.md).

## Near-term work

1. Keep current status, release instructions, and installable documentation
   consistent with the v5.2 product boundary.
2. Build and inspect both the standard and explicit experimental package paths
   in CI while keeping the standard package authentication-free.
3. Execute the PAM setup helper against isolated fixtures and verify rollback
   after injected command failures.
4. Record KCM hardware/accessibility qualification separately from PAM login
   and presentation-attack qualification.

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
