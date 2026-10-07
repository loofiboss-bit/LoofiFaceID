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
private pipes. Preview is a bounded session worker. Vision uses a persistent
`--session` worker during guided enrollment and a short-lived one-request mode
for explicit checks; identity remains a one-request worker. There is no
listener, shell, network, privileged process, or automatic authentication
activation in the beginner flow.

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

The KCM backend owns explicit action boundaries, latest-generation-wins worker
lifecycle, one-request-at-a-time vision sessions, four-analyses-per-second
guidance throttling, transient enrollment embeddings, KWallet access, and
high-level typed UI states. QML receives no frame bytes, raw embeddings, raw
landmarks, keys, paths, or detector scores.

Guidance state is backend-owned and typed: `NoFace`, `MultipleFaces`,
`TooFar`, `TooClose`, `OffCenter`, `PoorLighting`, `Blurred`, `WrongPose`, or
`Ready`. QML receives only the state, coarse pose, and a boolean pose-match
decision for the current enrollment step.

## UI flow and semantic state

The KCM exposes one shared `CameraPreviewSession` to four destinations:

```text
Home -> Setup -> Test
  \-> Diagnostics
```

Home derives `NeedsCamera`, `NeedsProfile`, `ReadyToTest`, or
`NeedsAttention` from typed backend properties and presents one action:
`Get started`, `Continue registration`, `Test profile`, or `Fix problem`.
Setup owns the explicit camera consent, shared guidance session, five-pose
enrollment, and separated destructive profile management. Test owns one
explicit comparison and its clear-result action.
Diagnostics is read-only and exposes aggregate capability state plus a bounded
redacted report. QML uses semantic properties such as `canStartPreview`,
`previewActive`, `canAnalyze`, `profileReady`, and `recommendedAction`; it does
not interpret numeric backend state values.

The reusable `CameraPreviewCard`, `PrimaryStatusCard`, `PrivacySummary`, and
`ActionableIssue` components keep camera controls, the privacy boundary, and
recovery messaging consistent across pages. The preview guide is a static
framing aid; guided enrollment adds bounded typed analysis only while the page
and camera are active. There is no background recognition.

## Enrollment and verification

Enrollment starts explicitly, captures exactly one current frame per click,
keeps 3–8 accepted embeddings only in memory (five recommended), and commits
them atomically at Finish. Recoverable one-frame quality/face errors keep the
bounded session available for an explicit retry; fatal failure, the shared 300-second
deadline, page hide, app deactivation, preview stop, replacement, cancel, or
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
persistence, local verification, and deletion capabilities. The standard package has no PAM, authselect, system-authentication service,
liveness or qualified security tier. The optional authentication experiment
reports configuration/readiness separately from qualification.

The opt-in authentication status is queried through the system daemon's
bounded status protocol. The KCM does not inspect the protected system vault
or key paths directly; only the daemon reports whether an enrolled system
profile is available.

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

## v5.3.0 experimental worker isolation

The optional daemon validates bounded socket requests and peer UID, rate-limits
attempts and supervises at most one `/usr/libexec/kfaceauth-auth-worker` child.
The child runs in the same `kfaceauth` account and SELinux domain, reads a fixed
UID/timeout record from private stdin, and returns only a fixed status on its
private output. It owns camera capture, verified model loading and system-vault
comparison. No key or frame is sent through command arguments or environment.
The parent terminates the child at its absolute deadline, ignores late results,
and reaps without blocking the PAM response. An unreaped child remains busy;
there is no overlapping camera operation or claim of recovery from kernel D-state.
Owned frame/key buffers use zeroization wrappers. Standard payloads omit the child.

`ExtractionPurpose::LocalProfile` performs face, geometry and quality checks
without the speculative presentation heuristic. `ExperimentalAuth` keeps
conservative heuristic rejection, including analysis failure, but is not PAD
qualification. The internal extraction API requires a purpose at every call site.

## Experimental authentication recovery state

EnrollmentSession retains the existing profile/privileged-helper boundary and
exposes backend-owned Readiness, AuthOperationState and AuthOperationResult.
Its optional constructor-injected status/runner functions let synthetic tests
exercise readback and failures without touching host authentication. Production
uses fixed installed helpers and the bounded daemon status request. Injection
is not available through QML, configuration or environment variables.

An explicit resync has no target/mode argument: it replaces the shared system
profile while preserving both independent user choices. Helper completion starts
a bounded readback; successful exit alone is insufficient for UI success. Late
probe generations are discarded and last-operation errors are separate from
current readiness. Only fixed readiness/result categories reach reports.

The daemon keeps an open, validated policy descriptor from authorization until
its final positive result check. Supported writers atomically replace the policy;
any changed file identity revokes that attempt even if mode values were restored.
The retained descriptor prevents inode reuse from hiding an intermediate swap.
