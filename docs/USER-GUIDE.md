# User guide — LoofiFace-ID

LoofiFace-ID (KFaceAuth) is an experimental local profile and explicit
comparison utility for a logged-in Fedora 44/KDE Plasma session. It does not
enable PAM, SDDM, sudo, Polkit, desktop unlock, or login authentication.

Version 5.3.0 is the current stable package release for this local workflow.
This release status does not qualify face comparison as an authentication
factor. See the [release notes](https://github.com/loofiboss-bit/LoofiFaceID/releases/tag/v5.3.0)
and [qualification record](RELEASE-QUALIFICATION-V5.3.md).

## Install and launch

The supported Fedora 44 installation path is COPR:

```bash
sudo dnf copr enable loofitheboss/loofifaceid
sudo dnf install kfaceauth
systemsettings kcm_kfaceauth
```

Open **Home** and follow the one primary action. Choose **Get started** when a
profile is missing and the camera is not running. The only usable camera is
selected automatically; the camera selector appears only when multiple usable
cameras are discovered.

## What's new in 5.3.0

Explicit registration shares a five-minute deadline with its camera
preview; ordinary preview still ends after one minute. Hiding the page, app
inactivity, camera failure or cancellation clears unsaved samples. A failed
framing guide can be retried within the active deadline without losing accepted
samples. Saving over an existing profile requires a replacement confirmation;
failed saving preserves the previous profile. Local comparison uses face/quality
checks and does not claim or apply qualified spoof detection. The opt-in
authentication experiment now performs native work in a separate bounded
worker process, while remaining unqualified and absent from the standard RPM.

## First start and enrollment

1. Click **Get started**. This is the explicit consent to start the private
   camera preview.
2. Follow the same guide: placement, **Frontal**, **Left**, **Right**, **Tilt**,
   and **Natural**.
3. Automatic capture requires exactly one face, suitable framing/image quality,
   and three fresh analyses spanning at least 600 ms. Stability resets as soon
   as the condition breaks; an 800 ms cooldown follows each accepted sample.
4. **Capture manually** is always available as a fallback. After three samples
   **Save now** is offered; five samples are recommended and eight is the hard
   maximum.
5. Choose **Save profile** yourself. The profile is never saved automatically.
   Then choose **Test profile** or open the **Test** tab.

When a sample is rejected, enrollment continues and shows one concrete action:
move closer/farther, center the face, improve lighting, hold still, or show only
one face. Camera frames are not stored.

## Home, Test, and profile management

Home reports **Get started**, **Continue registration**, **Test profile**, or
**Fix problem** according to the current state. **Test** processes one deliberate
current frame against the encrypted local profile; its result has no effect on
the Linux session.

**Delete face profile** and **Reset unreadable data** are separate, confirmed
actions. Neither promises physical erasure from SSDs, snapshots, backups,
journals, or copy-on-write storage.

## Diagnostics and errors

Diagnostics is read-only and never stops other camera services or changes host
configuration. Refresh and follow the typed recovery action for:

- a camera that is busy, disconnected, or unavailable;
- a worker, protocol, or verified-model failure;
- a locked or unavailable KWallet;
- an unreadable or model-incompatible profile;
- an unqualified distribution/version.

The support report is bounded and redacted. Do not attach camera images,
embeddings, keys, or passwords when reporting a problem.

## Privacy boundary

Raw landmarks, detector values, scores, frames, and embeddings stay inside the
private worker/backend boundary. They are not exposed to QML, normal logs, or
support reports. This version has no reproducible PAD, performance, bias, or
authentication qualification.

## Experimental login and unlock recovery

The optional authentication experiment is unsupported and off by default.
The standard package does not contain its PAM module, daemon or privileged
helpers. Face sign-in does not unlock KWallet or encrypted storage.

Open **Login and unlock** to review the separate SDDM and Plasma requirements.
Each selector contains only Off, On activity and Button only. An unreadable
saved policy is shown as Unknown; it is never silently interpreted as Off.
**Refresh status** runs bounded probes without starting camera capture.
An installed API marker or SDDM theme declaration is distinct from a theme
registering its control in the real login or unlock process.

If the local profile is missing, choose **Register profile** and finish the
existing explicit enrollment. If the system copy is missing or needs replacing,
choose **Sync system profile** and approve KWallet/administrative access.
The helper preserves each target's saved choice, and the page waits for policy
and system-profile readback before reporting success. Freshness remains Unknown
across refreshes; this version does not compare persistent profile generations.
A cancelled, failed or unverified change remains visible after refresh.

In a compatible greeter, camera-busy, timeout, unavailable-service and retry-later
messages describe recovery rather than a match score. Use the normal password
path immediately, or choose an explicit face retry when available. Errors never
create an automatic retry loop or turn a progress message into authorization.
Diagnostics exports fixed categories only; it includes no username, UID,
camera path, frame, biometric feature or credential.

Actual camera/login/unlock and failure recovery must be tested only on the
separate qualification system described in `HARDWARE-QUALIFICATION.md`.
