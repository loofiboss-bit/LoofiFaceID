# LoofiFace-ID (KFaceAuth)

LoofiFace-ID (KFaceAuth 4.0.0) is an experimental KDE System Settings utility for one
bounded, local face-comparison flow in the already logged-in user session. It
guides you through camera setup, an encrypted local profile, and one explicit
current-frame test.

`Match` is only an in-session comparison result. LoofiFace-ID cannot unlock,
authenticate, authorize, call PAM or Polkit, change the login stack, or alter
system settings.

## What you get

- Home, Setup, Test, and Diagnostics destinations with one clear next action.
- Explicit RGB/infrared preview and one-frame YuNet framing guidance.
- Three required, five recommended, and eight maximum enrollment samples.
- One AES-256-GCM profile for the current numeric UID, with its random key
  held only by the logged-in KDE KWallet session.
- Explicit profile deletion and unreadable-vault reset with confirmations.
- Short-lived unprivileged workers, private pipes, strict bounds, cancellation,
  deadlines, no telemetry, and no network access.

Captured images are not intentionally stored. Frames, landmarks, embeddings,
keys, similarity scores, biometric paths, and stable camera identifiers are
not exposed to QML, normal logs, support reports, or documentation examples.

## Supported release matrix

The v4.0.0 experimental release candidate is prepared for:

| Component | Supported baseline |
|---|---|
| Distribution | Fedora 44 |
| Desktop | KDE Plasma 6 / System Settings |
| Qt / KDE Frameworks | Qt 6.8 or newer / KF6 6.10 or newer |
| Vision runtime | OpenCV >= 4.8 (Fedora 44 release baseline: 4.13.x) |
| Profile key storage | KDE KWallet in the logged-in session |

Other combinations may build, but are not release-qualified. Physical RGB/IR
coverage, accessibility, latency, memory, and representative identity
qualification remain manual review gates for this experimental candidate.

Milestone 3 selects OpenVINO when the OpenCV build exposes it, then Vulkan
when the worker sandbox permits it, and otherwise uses the verified OpenCV CPU
path. Acceleration is opportunistic; the installed package does not require a
vendor-specific runtime or claim hardware qualification.

## Install a release RPM

From a directory containing the release RPM and its `SHA256SUMS` file:

```bash
sha256sum --check SHA256SUMS
rpm -K ./kfaceauth-4.0.0-1.fc44.x86_64.rpm
dnf install ./kfaceauth-4.0.0-1.fc44.x86_64.rpm
```

Use the exact filename produced for your Fedora architecture if it differs
from the example. Open **System Settings → Security & Privacy → LoofiFace-ID**,
or launch the KCM directly:

```bash
systemsettings kcm_kfaceauth
```

The package does not create a profile or KWallet key during installation.

## Quick start

1. Open **Home** and follow its single recommended action.
2. In **Setup**, refresh and explicitly start the camera preview. The first
   usable camera is selected automatically; a selector is available when more
   than one camera is usable.
3. Run the one-frame **Analyze current frame** check for framing and lighting
   guidance. This is not liveness or authentication evidence.
4. Create a profile with one explicit capture per sample. Three samples are
   required, five are recommended, and eight is the hard maximum. **Finish and
   save** performs the atomic encrypted commit.
5. Open **Test**, start the preview, and choose **Test current frame** once.
   Clear the result before another deliberate test; requests are rate-limited.
6. Use **Diagnostics** for current worker, model, camera, KWallet, profile, and
   redacted support-report status.

## Remove KFaceAuth

```bash
dnf remove kfaceauth
```

Package transactions do not inspect or remove user profiles or KWallet data.
Delete a valid profile explicitly in **Setup → Profile management** before
uninstalling if you want the application data removed through the supported
application action. Physical erasure from SSDs, snapshots, backups, journals,
or copy-on-write storage is not promised.

## Honest limitations

KFaceAuth has no PAM, authselect, SDDM, lock-screen, sudo, su, Polkit, system
authorization, privileged helper, system service, setuid binary, file
capability, network listener, runtime model download, cloud processing,
telemetry, background recognition, configurable threshold, user-selectable
model, liveness check, or presentation-attack defense.

FAR, FRR, bias, demographic behavior, spoof resistance, pre-login key access,
and authentication suitability are `UNQUALIFIED`. A physical camera run,
keyboard/Orca run, and real Fedora 44 screenshots are release follow-ups; no
screenshots are fabricated or included in this repository.

The GitHub repository is `LoofiFaceID`. The remaining `plasma-irlume` names are
limited to Fedora transition compatibility. A future rename to `kfaceauth`
would be a separate optional repository operation.

## Development and verification

Build dependencies, offline Cargo rules, staged installation, RPM checks, and
the complete local gate list are in [docs/BUILDING.md](docs/BUILDING.md).
Architecture and threat boundaries are documented in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and
[docs/THREAT-BOUNDARY.md](docs/THREAT-BOUNDARY.md). The manual boundary is
maintained in [docs/HARDWARE-QUALIFICATION.md](docs/HARDWARE-QUALIFICATION.md)
and [docs/V4-QUALIFICATION-REPORT.md](docs/V4-QUALIFICATION-REPORT.md).

For deep-dive architecture audits and the next-generation specification:
- [docs/REVIEW-V4.md](docs/REVIEW-V4.md): Complete repository review, subsystem architecture audit, and profiling of bottlenecks B1–B14.
- [docs/ROADMAP-V5.md](docs/ROADMAP-V5.md): Technical specification and roadmap for v5.0, latency budgets, persistent worker-pools, and PAM/daemon decoupling.

## License

Project code is GPL-3.0-or-later. YuNet weights are MIT. SFace weights and
Fedora OpenCV are Apache-2.0. Exact model licenses and provenance are shipped
in the closed model inventory.
