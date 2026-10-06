# Experimental SDDM face sign-in

This downstream patch series targets the exact SDDM v0.21.0 commit
`63780fcd79f1dbf81a30eef48c28c699ab15aded`. It adds a separate face-sign-in
path for the currently selected user. The ordinary
`GreeterProxy::login(user, password, sessionIndex) const` method and SDDM's
`sddm` password PAM service remain in place; an empty password never selects
the face service.

The integration is experimental and disabled by default. Build with
`-DSDDM_ENABLE_EXPERIMENTAL_FACE_AUTH=ON` to compile the runtime capability and
install this marker:

```text
${DATA_INSTALL_DIR}/kfaceauth/sddm-face-auth-api-v1.conf
```

On a Fedora build with the default `/usr` prefix, the path is
`/usr/share/sddm/kfaceauth/sddm-face-auth-api-v1.conf`. The marker records API
version 1, SDDM 0.21.0, and the pinned upstream commit. It is installed only by
the opt-in build. It says that this SDDM package contains the greeter API; it
does not prove that the active theme or PAM service is ready.

The reusable control is embedded in the `sddm-greeter` binary at
`qrc:/theme/FaceAuthenticationControl.qml`; it is not installed as a standalone
QML file. The SDDM patch installs no PAM file or LoofiFaceID helper. Those
separate prerequisites are `/etc/pam.d/sddm-kfaceauth` and
`/usr/libexec/kfaceauth-policy`, owned by the LoofiFaceID package/admin setup.

The active theme must declare `faceAuthenticationApi=1` under `[General]` in
the config file named by `[SddmGreeterTheme] ConfigFile` in its
`metadata.desktop` (normally `theme.conf`). The embedded fallback theme
includes the API v1 control. A custom theme can load
the same reusable control with
`Loader { source: "qrc:/theme/FaceAuthenticationControl.qml" }`, set its
`selectedUser` and `sessionIndex` properties, and must use API version 1. The
loaded QML component calls `sddm.registerFaceAuthenticationUi(1)`; the greeter
socket handshake is required before SDDM exposes or accepts a face attempt.
The current custom theme remains password-only when it omits either declaration
or registration.

The SDDM process also requires `/etc/pam.d/sddm-kfaceauth` and the read-only
policy helper `/usr/libexec/kfaceauth-policy`. SDDM does not create or edit
either file. The LoofiFaceID package/admin transaction owns them. SDDM queries
the helper asynchronously with `--get --uid <uid> --target sddm` and accepts
only one exact output line: `mode=off`, `mode=manual`, or `mode=on-activity`.
Unknown output, helper failure, or timeout fails closed. The selected username
is resolved to a UID by SDDM for each query and each face attempt, so a UI mode
response is never the authorization check. The face helper invokes the PAM
service only through a separate `sddm-helper --face-auth` transaction; ordinary
password login, greeter auth, and autologin continue to use their existing
service names.

With `mode=manual`, the fallback control starts only after an explicit click or
Space activation. With `mode=on-activity`, the first settled keyboard, pointer,
or wheel activity after a user is already selected starts one attempt for that
selection; user-selection/navigation events, password-field activity, Escape,
and empty Enter do not trigger it. Both enabled modes retain the explicit
button for retry. Off and unknown modes hide the control, and the daemon rejects
both trigger types for Off. Automatic activity is consumed once per selected
user; later movement or key events cannot create repeated attempts. Password
typing, Escape, selecting another user, choosing **Use password**, hiding every
greeter view, or disconnecting the greeter cancels an active attempt. Input
events continue to their original target so typed password characters are
retained. Multiple screen views share one attempt. The daemon starts a two-
second deadline when it receives the attempt and ignores status from any other
attempt ID.

Cancellation is asynchronous: SDDM closes its helper-control socket, sends
SIGTERM to the isolated `sddm-helper`, then sends SIGKILL after 150 ms if it is
still blocked in PAM. Process exit closes the helper's PAM client connection to
LoofiFaceID. The LoofiFaceID daemon must treat that disconnect as cancellation
and stop its worker; a UI status message is never an authorization result. Only
the current helper's successful PAM authentication and account-management
result can follow SDDM's existing session and login checks. A mismatched PAM
user is rejected.

## Apply and check

Use a clean checkout of the pinned upstream revision. The checker validates the
exact commit, checks patch applicability, applies it to a temporary clone, and
runs source-contract tests without editing the supplied checkout.

```bash
git clone --depth 1 --branch v0.21.0 https://github.com/sddm/sddm.git /tmp/sddm-v0.21.0
./integrations/sddm/check_patch.sh /tmp/sddm-v0.21.0
```

To build and test the patched SDDM without installing it, use a host with the
SDDM v0.21.0 Qt/KDE build dependencies:

```bash
git -C /tmp/sddm-v0.21.0 apply "$PWD/integrations/sddm/patches/0001-face-auth-v0.21.0.patch"
cmake -S /tmp/sddm-v0.21.0 -B /tmp/sddm-v0.21.0/build-face-auth \
  -DBUILD_WITH_QT6=ON -DSDDM_ENABLE_EXPERIMENTAL_FACE_AUTH=ON \
  -DINSTALL_PAM_CONFIGURATION=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build /tmp/sddm-v0.21.0/build-face-auth --parallel
ctest --test-dir /tmp/sddm-v0.21.0/build-face-auth --output-on-failure
```

The patch applies to the pinned upstream commit, its nine source-contract tests
pass, the opt-in Qt 6 build completes, and its three CTest cases pass. Physical
SDDM login, SELinux behavior, password fallback, camera release,
assistive-technology behavior, and the installed marker on Fedora remain
unverified until tested on the target system. This patch does not install SDDM,
edit PAM configuration, or qualify biometric authentication.
