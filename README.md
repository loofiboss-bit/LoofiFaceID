# LoofiFace-ID (KFaceAuth)

LoofiFace-ID is an experimental local profile and comparison utility for a
logged-in Fedora 44/KDE Plasma session. It keeps camera frames and biometric
features in memory or in the encrypted user-session profile. The normal
workflow never enables PAM, SDDM, sudo, Polkit, or another system
authentication service.

## What you get

- A private camera preview with one-camera auto-selection and clear recovery
  messages when a camera is busy or unavailable.
- One shared YuNet detection path for framing guidance and local identity
  extraction, including validated five-point landmarks inside the private
  worker protocol.
- A guided five-pose enrollment flow. Three samples are required, five are
  recommended, and saving always requires an explicit click.
- An encrypted KWallet-backed profile and an explicit one-frame local test.
- No telemetry, network model downloads, or saved camera images.

This is not an authentication factor. It does not unlock the desktop, log in
to a display manager, approve `sudo`/Polkit requests, or provide a PAD or
performance qualification claim.

The v5.1.0 work is an unreleased stabilization target. Its default build and
base RPM omit the experimental daemon, PAM module, and system service files.
KDE lock-screen unlock is not implemented or qualified. See the [v5.1.0
qualification status](docs/RELEASE-QUALIFICATION-V5.1.md) before treating any
security or physical test result as release evidence.

The currently published [v5.0.0 GitHub release](https://github.com/loofiboss-bit/LoofiFaceID/releases/tag/v5.0.0)
describes production PAM authentication and PAD results. The repository's
supported workflow and current qualification evidence do not substantiate
those claims. Do not use that release as an authentication mechanism; this
source correction has not been published.

## Supported release matrix

| Component | Supported baseline |
|---|---|
| Distribution | Fedora 44 |
| Desktop | KDE Plasma 6 / System Settings |
| Qt / KDE Frameworks | Qt 6.8 or newer / KF6 6.10 or newer |
| Vision runtime | Fedora OpenCV (4.13.x on Fedora 44) |
| Profile key | KWallet in the logged-in user session |

On another distribution or version, the KCM reports **This system is not
qualified** and shows the detected values in Diagnostics. It does not try to
repair or reconfigure the host.

## Install from COPR (recommended)

The maintained Fedora channel is [loofitheboss/loofifaceid](https://copr.fedorainfracloud.org/coprs/loofitheboss/loofifaceid/):

```bash
sudo dnf copr enable loofitheboss/loofifaceid
sudo dnf install kfaceauth
systemsettings kcm_kfaceauth
```

Open **Home** and choose **Get started**. The KCM selects the only usable
camera automatically; a camera selector appears only when more than one
usable camera is discovered.

### Update

```bash
sudo dnf upgrade kfaceauth
```

### Uninstall

```bash
sudo dnf remove kfaceauth
```

Removing the package does not delete an encrypted profile or its KWallet key.
Use **Setup → Delete face profile** first if you want the supported application
cleanup action. Physical erasure from SSDs, snapshots, backups, journals, or
copy-on-write storage is not promised.

### Advanced: verified GitHub RPM

For development or offline installation, download a matching RPM and
`SHA256SUMS` file from the [GitHub releases](https://github.com/loofiboss-bit/LoofiFaceID/releases),
then verify and install it:

```bash
sha256sum --check SHA256SUMS
rpm -K ./kfaceauth-*.rpm
sudo dnf install ./kfaceauth-*.rpm
```

The COPR route is preferred for ordinary Fedora users because dependencies and
updates are resolved by Fedora.

## First start

1. **Home → Get started** starts the selected camera after your explicit click.
2. Follow the single registration guide: camera placement, Frontal, Left,
   Right, Tilt, and Natural.
3. Automatic capture waits for one face, suitable framing/quality, and three
   fresh observations spanning at least 600 ms. **Capture manually** remains
   available as a fallback.
4. After three samples, **Save now** is available; five samples remain the
   recommended target. The profile is never saved automatically.
5. Choose **Save profile**, then **Test profile** on Home (or **Test → Test
   current frame**) for an explicit local comparison.

The status text always explains the next action, such as moving closer,
centering the face, adding light, or showing only one face. A failed sample
does not end the guide; correct the instruction and continue.

## Reporting a problem

1. Open **Diagnostics** and choose **Refresh diagnostics**.
2. Read the typed issue: camera busy/unavailable, worker or model failure,
   KWallet state, profile state, or unsupported platform.
3. Follow the displayed recovery action. Export **Copy report** or
   **Export report** only after checking that the redacted report contains no
   private data you do not want to share.

Please include the exact issue code, Fedora/Plasma versions, and the steps that
reproduced it. Do not attach camera frames, embeddings, KWallet files, or
credentials.

## Development and verification

Build dependencies, offline Cargo rules, staged installation, and local gates
are documented in [docs/BUILDING.md](docs/BUILDING.md). The architecture and
privacy boundary are in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and
[docs/THREAT-BOUNDARY.md](docs/THREAT-BOUNDARY.md).

The v5 roadmap and older qualification documents contain historical or
experimental proposals. They are not evidence that system authentication,
PAD, acceleration, or physical hardware qualification is currently supported.

## License

Project code is GPL-3.0-or-later. YuNet weights are MIT. SFace weights and
Fedora OpenCV are Apache-2.0. Exact model licenses and provenance are shipped
in the closed model inventory.
