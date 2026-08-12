# Roadmap

## Completed foundations

- Milestone 1: standalone native-v4 identity, asynchronous status, closed Rust
  status protocol, explicit unsupported authentication capabilities.
- Milestone 2: bounded explicit one-frame vision worker and selected,
  hash-pinned YuNet inventory.
- Milestone 3: production YuNet through Fedora OpenCV 4.13 and a narrow
  reviewed C ABI.
- Milestone 4: complete local identity MVP: verified SFace FP32, bounded
  alignment/embedding, KWallet-backed encrypted single-user vault, enrollment,
  profile status/deletion/reset, and explicit local verification.

Milestone 4 remains an experimental user-session comparison. It does not
authorize login, unlock, sudo, Polkit, or any system action.

## v4.0.0 release-candidate boundary

The v4 source, package transition, and automated gates are prepared for an
experimental release candidate. Public publication may proceed only with
explicit release authority and an honest qualification boundary. The
[v4 qualification report](V4-QUALIFICATION-REPORT.md) still records physical
RGB/IR camera coverage, keyboard and assistive-technology behavior,
cancellation and teardown loops, KWallet failure behavior, corrupt-vault
recovery, latency, memory, and remaining untested coverage as `NOT RUN` until
directly observed.

Publication of this experimental candidate does not waive those manual gates.
Authentication suitability remains blocked and `UNQUALIFIED`.

Representative FAR/FRR, bias, liveness, and spoof-resistance evidence is not
available. It must remain `UNQUALIFIED`; no current Match result is an
authentication or publication claim.

## Blockers before liveness work

1. Select a liveness/presentation-attack threat model and representative attack
   corpus with redistribution and participant permission.
2. Qualify RGB and IR capture paths without treating spectrum, brightness, or
   image quality as liveness evidence.
3. Measure false accept/reject behavior, demographic/bias limitations, pose,
   appearance, lighting, camera, latency, memory, and spoof behavior on the
   supported hardware matrix.
4. Add independently reviewed attack tests and privacy/data-retention rules.

## Blockers before PAM or system authentication

1. Complete liveness/spoof qualification; Milestone 4 provides none.
2. Replace user-session KWallet with a separately reviewed pre-login key
   provider, migration, recovery, and disk-theft design.
3. Define a privileged trust boundary, authorization policy, rate limiting,
   lockout, audit, password fallback, recovery, and fail-safe behavior.
4. Perform PAM/authselect/SDDM/lock-screen, SELinux, packaging, upgrade,
   rollback, and threat reviews as a separate milestone.
5. Obtain representative FAR/FRR evidence and independent security review.

No current capability or local `Match` may be reused as evidence that these
blockers are solved.
