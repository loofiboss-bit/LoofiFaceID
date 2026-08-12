# Threat boundary

Milestone 4 is an offline, current-user, in-session identity experiment. It is
not an authentication system and has no liveness or spoof resistance.

## Trusted components

- generation-aware KCM coordination and explicit user actions;
- closed bounded preview, vision, and identity protocols;
- private inherited pipes and finite process deadlines;
- closed model inventory and exact YuNet/SFace SHA-256 verification;
- Rust frame, native-output, embedding, and model-identity validation;
- the narrow exception-catching OpenCV 4.13 bridge;
- Fedora OpenSSL 3 AES-256-GCM and CSPRNG;
- KWallet as the only production master-key provider;
- versioned, UID/model-bound vault validation and atomic filesystem rules.

OpenCV and ONNX parsing remain native-library risk. They are confined to
short-lived ordinary-user workers with disabled core dumps, bounded input and
output, no network, and no privilege.

## Explicitly unsupported

- PAM decisions/configuration, authselect, SDDM, lock screen, sudo, su, Polkit,
  KAuth authentication, and any OS authorization;
- root/setuid helpers, privileged or system services, SELinux policy, TPM
  sealing, pre-login key access, or login-password recovery;
- passive/continuous recognition, background camera use, telemetry, networking,
  remote inference, or runtime downloads;
- liveness, presentation-attack detection, anti-spoof, security-tier, RGB/IR
  security, FAR, FRR, bias, or authentication claims.

## Biometric privacy

Preview and capture are manual and ephemeral. No image is intentionally written
to disk. One-frame copies, landmarks, raw features, uncommitted embeddings,
keys, and plaintext templates stay inside the private native/Rust boundary and
are cleared where practical. QML, normal logs, CLI, and support reports receive
only aggregate capability/state and typed result categories.

Encrypted embeddings remain sensitive biometric data. User-session encryption
limits ordinary at-rest disclosure but does not qualify disk theft, a
compromised logged-in account, process inspection by same-user malware,
snapshots/backups, or physical erasure. KWallet is unavailable before login.

## Fail-closed behavior

- Missing/modified/unlisted models, wrong OpenCV version, malformed native
  output, zero/multiple faces, invalid quality/geometry, cancellation, timeout,
  crash, stale generation, or protocol violation produces no embedding/result.
- Locked/cancelled/unavailable KWallet never falls back to a plaintext key.
- Wrong key, tag/ciphertext/AAD change, unknown schema, wrong UID/model/format,
  unsafe mode/owner/link/type, or oversized/truncated vault is rejected.
- Unreadable vaults are preserved until an explicit destructive reset.
- Atomic commit/rotation verifies the temporary ciphertext before rename; a
  failure preserves the previous valid file.
- A local `Match` can update only Test and has no authorization
  side effect.
- Package install/upgrade/remove has no authentication scriptlet and does not
  create, rewrite, migrate, or delete a user profile.

See [IDENTITY-PIPELINE.md](IDENTITY-PIPELINE.md),
[IDENTITY-PROTOCOL.md](IDENTITY-PROTOCOL.md), and
[TEMPLATE-VAULT.md](TEMPLATE-VAULT.md).
