# Experimental KScreenLocker face factor

This downstream patch adds an explicit LoofiFaceID factor to KScreenLocker
6.7.5. It is pinned to upstream commit
`057b3774d9ad322cfccc2683ea057aed87e0f878` (tag `v6.7.5`) and is kept separate
from the standard LoofiFaceID package.

The face path calls the dedicated `kde-kfaceauth` PAM service from a short-lived
worker process. The existing `kde` password service and its PAM stack are left
unchanged. The build option `KSCREENLOCKER_ENABLE_EXPERIMENTAL_FACE_AUTH` is
OFF by default; only an opt-in build installs the worker and exposes the face
control. An opt-in build also installs
`/usr/share/kfaceauth/integrations/kscreenlocker.json`, which reports only the
component ID, interface version, QML resource, and upstream KScreenLocker
version. It does not report the active theme, camera, user policy, or biometric
qualification. The fallback greeter offers **Use Face ID**, **Cancel face
check**, and **Use password**. The per-user `plasma-lock` mode is read through
the fixed `/usr/libexec/kfaceauth-policy` helper using
`--get --uid UID --target plasma-lock`. The helper must exit successfully and
write exactly one of `mode=off\n`, `mode=manual\n`, or `mode=on-activity\n`;
missing helpers, timeouts, nonzero exit, extra output, and malformed output all
resolve to `unknown`. Lookup runs asynchronously with a 250 ms deadline, so the
password PAM path does not wait for it.

`off` hides and disables the face factor. `manual` starts only when the user
presses **Use Face ID**. `on-activity` permits one attempt after the first
explicit key, mouse, touch, or tablet press while an unlock view is visible.
Creating or showing the greeter when the session is locked does not count as
activity and never starts the camera. Repeated keys and presses do not start
more attempts in the same activity cycle; pointer motion does not start an
attempt. After a non-positive result, a new attempt requires the explicit
**Try Face ID again** button. The activity cycle resets after all lock views are
hidden or the system resumes from sleep. Activity received while policy or
theme support is still resolving is held for at most 250 ms; if the theme does
not register in that window, no later component load can start a stale attempt.
Password selection, password entry, Escape, and other cancellation clear the
pending activity immediately.

Every display shares one face-attempt object. `FaceAuthenticationControl.qml`
registers its interface version when the QML component is instantiated; the
face button and start method remain unavailable until that registration
succeeds. Removing the last such component cancels an in-flight attempt. This
runtime report is what lets a theme advertise actual support; the build marker
alone does not. Password entry, Enter/Return, Escape, choosing password,
switching users, suspend, or hiding all lock-screen views cancels the separate
PAM worker. The worker receives SIGTERM immediately and SIGKILL after 100 ms if
needed, which closes the PAM client's daemon socket. Password input remains
enabled while face PAM runs. A fixed allowlist maps PAM status tokens to three
localized states; no raw PAM text reaches QML. Only a normal worker exit with
PAM success can request unlock, and the greeter rechecks that result before
quitting. Policy state, activity, and typed progress are never authorization;
only successful `kde-kfaceauth` PAM completion can unlock.

The QML control is included in KScreenLocker's fallback theme. Other lock-screen
themes must instantiate
`qrc:/fallbacktheme/FaceAuthenticationControl.qml` and use the shared
`faceAuthenticator` context object. The greeter counts these live component
instances and rejects starts if none has reported interface version 1. Themes
that do not do this continue to use password authentication and do not trigger
face recognition. NoxForge 15's Graphite and Obsidian lock screens load this
component behind the API-v1 guard in the [companion NoxForge PR](https://github.com/loofiboss-bit/NoxForge/pull/43).
That theme change has offscreen and password-fallback checks only; physical
unlock, camera cancellation, and assistive-technology behavior remain
unverified. Breeze and other themes remain password-only until they load the
component and complete their own runtime qualification.

## Apply and verify

Use a clean KScreenLocker `v6.7.5` checkout. The check script verifies the exact
upstream commit, checks that the patch applies, applies it to a temporary copy,
and runs source-contract tests without changing the supplied checkout:

```bash
git clone --depth 1 --branch v6.7.5 https://github.com/KDE/kscreenlocker.git /tmp/kscreenlocker-v6.7.5
./integrations/kscreenlocker/check_patch.sh /tmp/kscreenlocker-v6.7.5
```

To build and run the included Qt tests on a host with KScreenLocker's full KDE
development dependencies installed:

```bash
git -C /tmp/kscreenlocker-v6.7.5 apply "$PWD/integrations/kscreenlocker/patches/0001-face-auth-factor-kscreenlocker-v6.7.5.patch"
cmake -S /tmp/kscreenlocker-v6.7.5 -B /tmp/kscreenlocker-v6.7.5/build-face-auth \
  -DBUILD_TESTING=ON -DKSCREENLOCKER_ENABLE_EXPERIMENTAL_FACE_AUTH=ON
cmake --build /tmp/kscreenlocker-v6.7.5/build-face-auth --parallel
ctest --test-dir /tmp/kscreenlocker-v6.7.5/build-face-auth --output-on-failure \
  -R 'kscreenlocker-facePamAuthenticatorTest'
```

The face PAM service must be installed and separately enabled by the
administrator for the target user's `plasma-lock` policy. This patch does not
edit PAM configuration, install LoofiFaceID, or qualify biometric login.
