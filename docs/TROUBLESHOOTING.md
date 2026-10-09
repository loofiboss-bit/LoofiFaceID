# Troubleshooting

## Upgrade blocked by the experimental-auth package

The opt-in `kfaceauth-experimental-auth` package requires the exact same
version of `kfaceauth`. COPR publishes the standard local-profile package, not
the opt-in authentication package, so DNF cannot upgrade the base while an
older experimental package is installed.

To return to the standard package, first disable every authentication target
that you enabled. These commands restore the normal password-based PAM path;
they do not delete the local face profile or its KWallet key:

```bash
sudo kfaceauth-pam-setup --disable-target sddm
sudo kfaceauth-pam-setup --disable-target plasma-lock
```

Then preview the package removal and check that DNF lists only
`kfaceauth-experimental-auth`:

```bash
sudo dnf5 remove --assumeno kfaceauth-experimental-auth
```

If the preview is as expected, remove that package and upgrade the standard
one:

```bash
sudo dnf5 remove kfaceauth-experimental-auth
sudo dnf5 upgrade kfaceauth
```

Do not continue if either target cannot be disabled or the removal preview
includes another package. The experimental package is unqualified and is not
available as a matching COPR upgrade; keeping it installed pins the base
package to its exact version.

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
nodes, an administrator must select the intended node. The experimental
`kfaceauth.service` reads an optional root-owned environment file at
`/etc/kfaceauth/kfaceauth.conf`; it is not installed by the package and is
ignored when absent. The KCM preview selection is separate.

Inspect the camera identity and supported capture formats without saving
frames, then choose a stable symlink from `/dev/v4l/by-path` or
`/dev/v4l/by-id`. With `v4l-utils` installed, these read-only commands list
device identities, stable paths, and the formats each capture node advertises:

```bash
v4l2-ctl --list-devices
ls -l /dev/v4l/by-path
udevadm info --query=property --name=/dev/videoN | grep '^ID_V4L_PRODUCT='
v4l2-ctl --device=/dev/videoN --list-formats-ext
```

Select the node by its camera identity, not by GREY or YUYV format alone; a
grayscale stream does not establish that a camera is infrared or provides
liveness detection. Add the selected path to the administrator configuration:

```bash
sudo install -d -o root -g root -m 0755 /etc/kfaceauth
sudoedit /etc/kfaceauth/kfaceauth.conf
```

Add one line, replacing `REPLACE_WITH_CAMERA_LINK` with the complete stable
symlink name for the selected capture node:

```ini
KFACEAUTH_CAMERA_DEVICE=/dev/v4l/by-path/REPLACE_WITH_CAMERA_LINK
```

Then ensure the file is root-owned and restart the daemon to reload its
environment:

```bash
sudo chown root:root /etc/kfaceauth/kfaceauth.conf
sudo chmod 0644 /etc/kfaceauth/kfaceauth.conf
sudo systemctl daemon-reload
sudo systemctl restart kfaceauth.service
```

If the setting is absent, unique-camera auto-selection remains available; when
multiple compatible nodes remain, authentication fails closed and PAM keeps
the password fallback. Generic vendor emitter controls are not sent.

A timed-out native attempt is terminated in a separate confined process. A
kernel-stuck process may remain busy until it actually exits; password fallback
must remain available. Runtime and physical qualification are still unverified.

## Authentication camera setting (unreleased 5.4.0)

In the opt-in experiment, **Login and unlock → Camera for login and unlock**
can read the administrator setting, choose a compatible camera or restore
automatic selection. Refresh, Apply and Reset use a fixed Polkit-protected
helper. Refresh queries capabilities without capturing frames. The default
package omits this helper and its policy.

Apply/Reset first explain that the setting affects all experiment users and
requires a daemon restart, interrupting active face attempts. The helper
revalidates selection, preserves unrelated root-owned configuration and
verifies service plus configuration after restart. Failure restores the previous
snapshot and active state; a failed rollback is reported explicitly. Password
sign-in remains available. Unsupported multiline/continued EnvironmentFile
values are rejected before editing, so they require administrator review.

An unverified system profile remains Unknown, including with an older daemon.
A verified changed local generation is Stale: use **Sync system profile** to
explicitly approve its new system copy. Successful sync is shown only after
Current readback. Neither status enables authentication or captures an image.
