# Troubleshooting

## Unsupported system

LoofiFace-ID is qualified only on Fedora 44 with KDE Plasma. If Diagnostics
shows **This system is not qualified**, use the displayed distribution and
version when reporting the issue. The KCM does not change the host or enable
system authentication on another platform.

## Local identity unavailable

Verify every installed package-owned file, including the exact model
inventory:

```bash
rpm -V kfaceauth
```

No output means RPM verification found no difference. Then open KFaceAuth,
choose **Diagnostics**, refresh status, and inspect or export the bounded
redacted report. Do not copy, rename, or download an ONNX file manually. The
worker requires the exact closed YuNet/SFace inventory and never substitutes a
model.

## KWallet locked, cancelled, or unavailable

Unlock your normal KDE wallet and retry. Cancelling access safely leaves the
profile unchanged. KFaceAuth never falls back to a key file. If the wallet key
is permanently lost, use **Reset unreadable data** and re-enroll; recovery or
export is not implemented.

## Profile unreadable or model mismatch

KFaceAuth preserves unreadable data and will not overwrite it automatically.
First verify/reinstall the RPM. If the profile cannot be recovered with the
original KWallet key and exact model version, explicitly reset it and enroll
again.

## Enrollment sample rejected

Follow the one instruction shown above the camera: keep exactly one face
visible, use even lighting, center the face away from the edge, move closer or
farther as requested, and hold still. Automatic capture needs three fresh
observations over at least 600 ms; **Capture manually** remains available. A
rejected sample does not end enrollment. Quality guidance is not liveness
evidence.

## Preview stops or verification is rate-limited

Ordinary preview stops after 60 seconds; explicit registration receives one
shared five-minute deadline. Both stop on Setup/Test page hide, app deactivation,
failure, or teardown. Restart it explicitly. Verification intentionally
permits no faster than one request every two seconds.

## Build dependencies

`opencv-devel` must provide OpenCV >=4.8, `openssl-devel` OpenSSL 3, and
`kf6-kwallet-devel` KF6 Wallet. KFaceAuth rejects another OpenCV minor until
reviewed.

## v5.3.0 framing-guide recovery

If framing guidance fails while the camera and enrollment remain active, choose
**Retry guidance** or capture manually. Retry clears stale observations without
extending the deadline or discarding accepted samples. If the camera itself
stops, transient samples are cleared and registration must be restarted.

## Experimental authentication camera selection

The opt-in auth worker automatically selects a camera only when exactly one
V4L2 streaming capture node supports GREY or YUYV. With multiple compatible
nodes, an administrator must configure `KFACEAUTH_CAMERA_DEVICE` in the service
configuration. The KCM preview selection is separate. Formats or names alone
are not liveness evidence. Generic vendor emitter controls are not sent.

A timed-out native attempt is terminated in a separate confined process. A
kernel-stuck process may remain busy until it actually exits; password fallback
must remain available. Runtime and physical qualification are still unverified.
