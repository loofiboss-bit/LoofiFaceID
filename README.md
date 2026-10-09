# LoofiFace-ID (KFaceAuth)

LoofiFace-ID is an experimental local profile and comparison utility for a
logged-in Fedora 44/KDE Plasma 6 session. It keeps camera frames and biometric
features in memory or in the encrypted user-session profile. The normal
workflow never enables PAM, SDDM, sudo, Polkit, or another system
authentication service.

Version 5.3.0 is the current stable package release for this explicit local
workflow. Stable release status describes the published source and package; it
does not qualify face comparison for login or other security decisions. See
the [v5.3.0 GitHub release](https://github.com/loofiboss-bit/LoofiFaceID/releases/tag/v5.3.0)
and the [release qualification record](docs/RELEASE-QUALIFICATION-V5.3.md).

The working source targets an **unreleased 5.4.0 candidate**. It adds recoverable
camera hotplug, explicit comparison recovery, verified informational system-profile
freshness, and an administrator-authorized camera setting in the opt-in experiment.
These changes do not change the published release or qualify system login. See
[candidate validation and remaining qualification](docs/VALIDATION-V5.4-CANDIDATE.md).

The previous v5.2.0 source package could not rebuild in a clean COPR build
because its standard spec omitted `systemd-devel` for `libudev`. v5.2.1 fixed
that requirement and passed COPR build 11075253. v5.3.0 carries forward the
corrected Fedora build dependency and improves enrollment recovery, diagnostics,
and release integrity. Manual qualification gates remain open in the current
qualification record.

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

The ordinary COPR package is not an authentication factor. It does not unlock
the desktop, log in to a display manager, approve `sudo`/Polkit requests, or
provide a PAD or performance qualification claim.

The source also contains a separate, explicit opt-in experimental path for PAM
authentication through SDDM and the KDE Plasma lock screen. The default CMake
build and COPR RPM omit its PAM module, daemon, service files, and SELinux
policy. The opt-in path is unqualified and unsupported; a positive face match
is not evidence of liveness or spoof resistance. Password authentication is
kept as the PAM fallback. See the [current v5.3.0 qualification status](docs/RELEASE-QUALIFICATION-V5.3.md)
before enabling or describing this experiment.

The downstream integration patch series and isolated build checks are documented
for [SDDM](integrations/sddm/README.md) and
[KScreenLocker](integrations/kscreenlocker/README.md), with its
[Plasma Desktop lock-screen surface](integrations/plasma-desktop/README.md). None of these patches is installed
by the standard package, and none has passed physical login/unlock qualification.

The historical [v5.0.0 GitHub release](https://github.com/loofiboss-bit/LoofiFaceID/releases/tag/v5.0.0)
has a published correction withdrawing its unsupported authentication, PAD,
and performance claims. Its original tag and assets remain unchanged for
provenance and are not suitable as an authentication mechanism. See the
[correction record](docs/RELEASE-ERRATA-V5.0.0.md).

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
camera automatically; a camera selector permits explicit reselection after a device disappears or its
worker restarts. Device refresh never starts a camera.

### Update

```bash
sudo dnf upgrade kfaceauth
```

If DNF reports that `kfaceauth-experimental-auth` requires an exact older
`kfaceauth` version, follow the
[experimental-auth upgrade recovery](docs/TROUBLESHOOTING.md#upgrade-blocked-by-the-experimental-auth-package).
That opt-in package is not published in COPR and must be disabled and removed
before the standard package can move to a different version.

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

## What's new in 5.3.0

Explicit registration shares a five-minute deadline with its camera
preview; ordinary preview still ends after one minute. Hiding the page, app
inactivity, camera failure or cancellation clears unsaved samples. A failed
framing guide can be retried within the active deadline without losing accepted
samples. Saving over an existing profile requires a replacement confirmation;
failed saving preserves the previous profile. Local comparison uses face/quality
checks and does not claim or apply qualified spoof detection. The opt-in
authentication experiment now runs its native capture and comparison in a
separate bounded worker process; it remains unqualified and absent from the
standard RPM.

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

Current release notes, qualification status, user guides, and the release
checklist are linked from the [project wiki](https://github.com/loofiboss-bit/LoofiFaceID/wiki).

## License

Project code is GPL-3.0-or-later. YuNet weights are MIT. SFace weights and
Fedora OpenCV are Apache-2.0. Exact model licenses and provenance are shipped
in the closed model inventory.
