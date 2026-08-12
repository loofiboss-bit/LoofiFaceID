# Troubleshooting

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

Keep exactly one face visible, use even lighting, center the face away from the
edge, move closer if it is small, and vary ordinary pose or appearance between
samples. Each frame requires an explicit Capture click. Quality guidance is not
liveness evidence.

## Preview stops or verification is rate-limited

Preview stops after 60 seconds and on Setup/Test page hide, app deactivation,
failure, or teardown. Restart it explicitly. Verification intentionally
permits no faster than one request every two seconds.

## Build dependencies

`opencv-devel` must provide OpenCV 4.13, `openssl-devel` OpenSSL 3, and
`kf6-kwallet-devel` KF6 Wallet. KFaceAuth rejects another OpenCV minor until
reviewed.
