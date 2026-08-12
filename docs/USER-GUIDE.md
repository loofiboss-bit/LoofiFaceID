# User guide

KFaceAuth is an experimental local comparison utility in KDE System Settings.
It operates only in the already logged-in user session and does not enable
login or authentication.

## Home

Home shows the current next step as one primary action:

- **Set up camera** when no usable camera is available;
- **Create face profile** when the camera path is ready but no profile exists;
- **Test recognition** when a profile is ready;
- **Resolve issue** when a worker, model, KWallet, or vault problem needs
  attention.

The privacy summary is always visible: processing is local to this session,
captured images are not stored, and the profile key is kept only in KWallet.
The experimental warning is part of the normal flow, not a promise of
authentication.

## Setup

Setup combines camera, frame guidance, enrollment, and profile management.

1. Refresh devices and explicitly start the private preview. The first usable
   camera is selected automatically; choose another one when multiple devices
   are available.
2. Choose **Analyze current frame** for one explicit YuNet check. Feedback is
   limited to face count, framing, and image-quality guidance. It is not
   liveness, spoof detection, or authentication.
3. Choose **Create face profile**, then capture exactly one sample per click.
   Three are required, five are recommended, and eight is the hard maximum.
4. Use **Retry sample** to discard the latest transient sample, **Cancel** to
   clear the uncommitted session, or **Finish and save** for the atomic
   encrypted commit.
5. When the profile is complete, choose **Open Test**. Completion does not
   enable login or any system authentication path.

Enrollment expires after 120 seconds and is cancelled when its page, preview,
application, or KCM becomes inactive. No partial profile is saved before
**Finish and save**.

### Profile management

**Delete face profile** removes a valid encrypted profile after confirmation.
**Reset unreadable data** is a separate destructive action for an unreadable
vault and its KWallet key; re-enrollment is required afterwards. Neither action
promises physical erasure from SSDs, snapshots, backups, journals, or
copy-on-write storage.

## Test

Start the private preview and choose **Test current frame**. The current frame
is processed once against the encrypted current-user profile. The page can
show `Match`, `No match`, `Ambiguous`, or a typed unavailable result such as no
profile, locked KWallet, model mismatch, cancellation, or worker failure.

Scores and thresholds are intentionally hidden. Requests are rate-limited and
the result can be cleared explicitly. `Match` changes only this page: it cannot
unlock, authenticate, authorize, call PAM or Polkit, or alter the Linux
session.

## Diagnostics

Diagnostics is read-only. Refreshing runs bounded local probes and reports:

- worker/runtime availability;
- offline model verification;
- camera count;
- KWallet state;
- encrypted vault/profile state and bounded sample count;
- the current typed issue and a plain-language recovery action.

The redacted support report contains aggregate status only. Secure Boot and
display-manager details are secondary environment information; they do not
change the local comparison capability.

## Privacy boundary

Face embeddings are sensitive biometric data. KFaceAuth intentionally stores
no captured image. Frames, landmarks, embeddings, keys, scores, biometric
paths, and stable camera identifiers do not reach QML, normal logs, the CLI,
support reports, or examples. KWallet is the only production key provider.

There is no liveness or presentation-attack detection. FAR, FRR, bias,
demographic behavior, RGB/IR security, and authentication suitability remain
unqualified. KWallet keys are unavailable before login.
