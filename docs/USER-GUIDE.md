# User guide — LoofiFace-ID

LoofiFace-ID (KFaceAuth) is an experimental local profile and explicit
comparison utility for a logged-in Fedora 44/KDE Plasma session. It does not
enable PAM, SDDM, sudo, Polkit, desktop unlock, or login authentication.

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
