# Security policy

KFaceAuth is experimental software for a bounded local comparison in an
already logged-in KDE session. Version 5.2 also contains a separately
packaged, explicit opt-in PAM experiment for SDDM and the KDE Plasma lock
screen. That path is not supported or qualified for login; do not use it to
protect an account, unlock a device, or authorize an action. Installing the
ordinary or experimental package does not enable PAM authentication.

## Reporting a vulnerability

Use the repository's private security-reporting mechanism when one is
available. If private reporting is unavailable, open a minimal public issue
that asks maintainers for a private contact channel and contains no sensitive
details.

Include only reproducible, non-sensitive facts: affected version or commit,
Fedora/Plasma context, a short impact description, and commands or synthetic
fixtures that reproduce the behavior. Do not attach or paste face images,
frames, embeddings, landmarks, profiles, vaults, KWallet keys, similarity
scores, stable camera identifiers, biometric paths, personal data, or
unredacted support reports. Never upload secrets or a user's home directory.

Do not use an everyday workstation to test PAM, authselect, SDDM, lock-screen,
Polkit, sudo, or other system authentication changes. The v5.2 experiment may
be evaluated only on a dedicated Fedora 44 and KDE Plasma 6 test system with
a verified password recovery path, following the project's qualification
plan. Use the offline fake workers and bounded unit tests for reports about
protocol, cancellation, worker, or UI behavior.

Maintainers will acknowledge a report when practical, reproduce it in an
isolated environment, and document a fix or an explicit non-goal. A local
`Match` result, a camera-quality signal, or synthetic test output is never
treated as authentication or security qualification.
