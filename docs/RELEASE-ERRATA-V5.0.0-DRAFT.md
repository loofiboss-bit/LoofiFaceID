# Draft correction for the v5.0.0 GitHub release

**Status:** Draft prepared 2026-10-04. This document has not been published as
an edit to the GitHub release.

## Proposed correction

The original v5.0.0 release notes overstate the authentication and biometric
qualification delivered by this project. The release text advertises
production PAM support for SDDM, the lock screen, sudo and Polkit; ISO/IEC
30107-3 presentation-attack detection; and figures including 0.0% APCER across
130 attack presentations, 0.8% BPCER, and 55–75 ms end-to-end authentication
latency. The current source and reproducible qualification evidence do not
substantiate those claims.

Do not use the v5.0.0 release as an authentication mechanism. The current
standard product is an experimental logged-in-session enrollment and local
comparison utility. The v5.2 PAM path is a separate, explicit opt-in experiment
for SDDM and the Plasma lock screen; it remains unsupported and unqualified.
The standard build and ordinary COPR package omit authentication components,
and package installation does not enable PAM.

The earlier PAD, authentication, and performance figures are withdrawn as
product qualification claims. No biometric images or embeddings should be
retained or inferred from the aggregate numbers in the original release notes.

## Review before publication

- Verify the exact release text, assets, tag target, and checksums again at the
  time of any proposed correction.
- Confirm that the correction matches the source and qualification state being
  described, including any later evidence.
- Publish only after explicit release authorization. This draft makes no change
  to the existing GitHub release or COPR.
