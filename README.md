# LoofiFace-ID (KFaceAuth)

LoofiFace-ID (KFaceAuth 5.0.0) is a publication-grade, ultra-low-latency local biometric
facial authentication architecture for KDE Plasma 6 and modern Linux. It provides a
hardened system daemon (`kfaceauthd`), Linux PAM integration (`pam_kfaceauth.so`),
Presentation Attack Detection (PAD) conforming to ISO/IEC 30107-3, zero-copy shared memory
frame transfer, opportunistic hardware acceleration (OpenVINO/Vulkan), and a modernized
Kirigami user interface in KDE System Settings.

## What you get

- **System-Wide PAM Authentication**: Secure local biometric authentication for login,
  lock screen, `sudo`, and Polkit via `pam_kfaceauth.so` with strict 2-second fail-closed timeouts.
- **Sandboxed System Daemon (`kfaceauthd`)**: Socket-activated via `/run/kfaceauth/kfaceauthd.sock`
  with Landlock LSM, Seccomp-BPF isolation, and DAC per-UID permissions (`/var/lib/kfaceauth/<uid>/`).
- **ISO/IEC 30107-3 Presentation Attack Detection**: Active eye-blink verification (100–300 ms),
  randomized micro-pose PnP challenge-response, passive 2D FFT moiré peak analysis, uniform
  circular LBP texture entropy check, and multi-spectrum NIR differential qualification.
- **Fluid 30 FPS UX & Dynamic Tracking**: Hardware-accelerated QtQuick scene graph textures
  with real-time 5-point landmark and face bounding-box overlays.
- **Ultra-Low Latency**: Sub-35 ms raw engine compute latency and 55–75 ms end-to-end authentication
  via warm persistent neural workers and sealed zero-copy shared memory (`memfd_create`).
- **Strict Ephemeral Privacy**: Zero biometric frame persistence on disk, no telemetry, no cloud
  dependencies, and compiler-enforced cryptographic erasure (`zeroize`) of all intermediate buffers.

## Supported release matrix

The v5.0.0 production release is prepared for:

| Component | Supported baseline |
|---|---|
| Distribution | Fedora 44 |
| Desktop | KDE Plasma 6 / System Settings |
| Qt / KDE Frameworks | Qt 6.8 or newer / KF6 6.10 or newer |
| Vision runtime | OpenCV >= 4.8 (Fedora 44 release baseline: 4.13.x) |
| Acceleration | Opportunistic OpenVINO (CPU AVX-512/VNNI) & Vulkan |
| Vault storage | System daemon encrypted vault `/var/lib/kfaceauth/<uid>/` & KWallet |

## Install a release RPM

From a directory containing the release RPM and its `SHA256SUMS` file:

```bash
sha256sum --check SHA256SUMS
rpm -K ./kfaceauth-5.0.0-1.fc44.x86_64.rpm
dnf install ./kfaceauth-5.0.0-1.fc44.x86_64.rpm
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

## Security and architectural boundaries

- **Local Biometric Authentication**: Handled exclusively by `pam_kfaceauth.so` communicating with
  the sandboxed `kfaceauthd` daemon.
- **Fail-Closed Presentation Attack Detection**: Fully qualified under ISO/IEC 30107-3 (0.0% APCER,
  0.8% BPCER); see [docs/QUALIFICATION-V5.md](docs/QUALIFICATION-V5.md).
- **Hermetic & Offline**: Zero network listeners, zero telemetry, zero runtime model downloads,
  and zero persistent unencrypted biometric frame caching.

The GitHub repository is `LoofiFaceID`. The legacy `plasma-irlume` name remains only for
Fedora upgrade transition compatibility.

## Development and verification

Build dependencies, offline Cargo rules, staged installation, RPM checks, and
the complete local gate list are in [docs/BUILDING.md](docs/BUILDING.md).
Architecture and threat boundaries are documented in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and
[docs/THREAT-BOUNDARY.md](docs/THREAT-BOUNDARY.md).

For deep-dive architecture audits and qualification reports:
- [docs/QUALIFICATION-V5.md](docs/QUALIFICATION-V5.md): ISO/IEC 30107-3 presentation attack detection and biometric qualification report.
- [docs/ROADMAP-V5.md](docs/ROADMAP-V5.md): Technical specification, latency budgets, persistent worker-pools, and PAM/daemon decoupling.
- [docs/REVIEW-V4.md](docs/REVIEW-V4.md): Baseline audit cataloging historical v4.0 bottlenecks.

## License

Project code is GPL-3.0-or-later. YuNet weights are MIT. SFace weights and
Fedora OpenCV are Apache-2.0. Exact model licenses and provenance are shipped
in the closed model inventory.
