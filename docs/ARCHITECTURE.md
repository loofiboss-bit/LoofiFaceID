# Architecture

Milestone 4 separates capture, neutral analysis, identity extraction, key
access, and encrypted persistence:

```text
KDE System Settings / KFaceAuthKcm
  +-- RefreshCoordinator -> NativeFaceAuthBackend
  +-- SystemProbe
  +-- CameraPreviewSession -> kfaceauth-camera-preview-worker
  +-- VisionAnalysisSession -> kfaceauth-vision-worker
  +-- EnrollmentSession --------+
  +-- LocalVerificationSession --+-> IdentityWorkerClient
  |                                  -> kfaceauth-identity-worker
  +-- KWalletKeyProvider

Rust identity worker
  -> vision -> vision-opencv-sys -> OpenCV >=4.8 (Fedora baseline: 4.13)
  -> identity-types
  -> templates -> crypto-openssl-sys -> Fedora OpenSSL 3
```

All workers run as the ordinary user and communicate only over inherited
private pipes. Preview is a bounded session worker. Vision and identity are
short-lived one-request workers. There is no listener, daemon activation,
shell, network, privileged process, or authentication interface.

## Ownership

The shared `kfaceauth-protocol` crate owns bounded worker framing, clean EOF and
truncated-input handling, and the worker protocol version. Rust owns closed
operations, bounds, model inventory and identity, frame validation,
cancellation/deadlines, native-output validation,
normalization, matching policy, vault format, filesystem safety, and
zeroization wrappers.

The project-owned C++ OpenCV bridge owns only `FaceDetectorYN` and
`FaceRecognizerSF` construction, packed BGR copies, five-landmark
`alignCrop`, feature extraction, and a qualification-only cosine call. It
catches every exception. OpenCV objects and matrices never cross the C ABI.

The KCM backend owns manual action boundaries, latest-generation-wins worker
lifecycle, rate limiting, transient enrollment embeddings, KWallet access,
and high-level UI states. QML receives no frame bytes, embeddings, landmarks,
keys, paths, or scores.

## UI flow and semantic state

The KCM exposes one shared `CameraPreviewSession` to four destinations:

```text
Home -> Setup -> Test
  \-> Diagnostics
```

Home derives `NeedsCamera`, `NeedsProfile`, `ReadyToTest`, or
`NeedsAttention` from typed backend properties. Setup owns the explicit camera,
one-frame framing check, enrollment, and separated destructive profile
management. Test owns one explicit comparison and its clear-result action.
Diagnostics is read-only and exposes aggregate capability state plus a bounded
redacted report. QML uses semantic properties such as `canStartPreview`,
`previewActive`, `canAnalyze`, `profileReady`, and `recommendedAction`; it does
not interpret numeric backend state values.

The reusable `CameraPreviewCard`, `PrimaryStatusCard`, `PrivacySummary`, and
`ActionableIssue` components keep camera controls, the privacy boundary, and
recovery messaging consistent across pages. The preview guide is a static
framing aid only. There is no continuous analysis or background recognition.

## Enrollment and verification

Enrollment starts explicitly, captures exactly one current frame per click,
keeps 3–8 accepted embeddings only in memory (five recommended), and commits
them atomically at Finish. Recoverable one-frame quality/face errors keep the
bounded session available for an explicit retry; fatal failure, the 120-second
timeout, page hide, app deactivation, preview stop, replacement, cancel, or
teardown clears transient material.

Verification also requires a preview and a separate one-frame action. The
worker opens the current user's encrypted profile, extracts one candidate,
applies the central median/threshold policy, and returns only a typed result.
The result updates only Test. Page changes, application deactivation, preview
stop, cancellation, timeouts, and teardown clear active frames/results and
terminate the relevant worker while preserving the last valid committed
profile.

## Vault and key

The fixed XDG user-data vault is AES-256-GCM encrypted and bound to numeric
UID, schema, YuNet/SFace identities, exact SFace hash, embedding format,
dimension, and normalization version. KWallet stores only the random master
key. Atomic writes, metadata checks, bounded locking, authenticated rotation,
corruption preservation, and explicit deletion/reset are defined in
[TEMPLATE-VAULT.md](TEMPLATE-VAULT.md).

## Status

The backend reports aggregate engine/runtime, verified model availability,
KWallet availability/lock state, vault/profile state, and bounded sample
count. It exposes separate detector, embedding, enrollment, encrypted
persistence, local verification, and deletion capabilities. PAM, authselect,
system authentication, liveness, security tiers, and privileged services
remain explicitly unsupported.

Production refresh first verifies the installed worker and both model hashes,
then executes the identity worker's bounded `status` operation. It reads a key
only when KWallet is already open; a locked wallet is reported without
prompting or falling back. Test-only availability probes bypass this runtime
path only in unit tests.

## Supply chain

`models/manifest.kfaceauth` is a closed offline allow-list. Python and Rust
both reject missing, renamed, modified, duplicate, malformed, or unlisted
artifacts before inference. Configure, build, test, install, and runtime never
download a model.
