# Experimental Plasma Desktop face control

This companion patch targets Plasma Desktop 6.7.5's
`desktoppackage/contents/lockscreen/LockScreenUi.qml`. KScreenLocker loads
this Plasma/Shell surface; LookAndFeel lock-screen files are not the active
integration surface on this baseline.

Apply the patch only alongside the opt-in KScreenLocker face factor. Its
Loader remains inactive without the API. The shared component registers
interface v1 and becomes visible only after a valid enabled per-user policy
resolves. Showing the view does not start a camera. Manual and on-activity
behavior remains controlled by the existing factor.

Password editing, password submission, Escape, user switching, and hiding the
unlock controls cancel the face attempt. A success signal requests Qt.quit;
the greeter's C++ authentication check remains the unlock authority. The
ordinary password UI and PAM service remain intact.

Validate on a disposable Plasma Desktop v6.7.5 source tree:

```bash
integrations/plasma-desktop/check_patch.sh /path/to/clean/plasma-desktop-v6.7.5
```

A standalone `kscreenlocker_greet --testing --shell /path/to/disposable/shell`
can verify component registration using the real opt-in greeter without locking
the session. Physical unlock still requires a deliberate test with password
fallback available. The local package-owned QML repair is overwritten by a
Plasma Desktop package update and must be reapplied or packaged downstream.

The checker pins commit `43f55fff3480c6eb5f60133f045bfc74b9c67d84`,
applies the patch in a disposable clone, and checks idle capture, API guards,
password cancellation and the existing greeter authority. These source checks
do not replace runtime or physical qualification.
