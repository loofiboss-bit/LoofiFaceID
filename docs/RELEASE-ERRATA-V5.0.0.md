# Correction for the v5.0.0 GitHub release

**Status:** Published 2026-10-05. The original tag and assets are preserved.

The original v5.0.0 release notes overstated the authentication and biometric
qualification delivered by this project. They advertised production PAM
support for SDDM, the lock screen, sudo, and Polkit; ISO/IEC 30107-3
presentation-attack detection; and results including 0.0% APCER across 130
attack presentations, 0.8% BPCER, and 55–75 ms end-to-end authentication
latency. The source and reproducible qualification evidence do not substantiate
those claims. The claims have been withdrawn in the
[published v5.0.0 release notes](https://github.com/loofiboss-bit/LoofiFaceID/releases/tag/v5.0.0).

Do not use the v5.0.0 release as an authentication mechanism or as evidence of
liveness, spoof resistance, PAD, or biometric performance. The current v5.3.0
standard product is an explicit local-profile and comparison utility for an
already logged-in Fedora 44/KDE session. Its separate PAM path remains
opt-in, unsupported, and unqualified for login. Password authentication stays
available as the PAM fallback.

The original v5.0.0 tag and source, RPM, and checksum assets remain unchanged
for provenance. This record corrects the public claims; it does not revise or
reissue those artifacts.
