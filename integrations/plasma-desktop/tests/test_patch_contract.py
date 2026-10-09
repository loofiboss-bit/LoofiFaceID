# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify the pinned active Plasma/Shell adapter without locking a session."""
from pathlib import Path
import sys
import unittest

ROOT = Path(sys.argv.pop(1)).resolve()
TEXT = (ROOT / 'desktoppackage/contents/lockscreen/LockScreenUi.qml').read_text()

class PlasmaShellContract(unittest.TestCase):
    def test_control_requires_runtime_api_and_never_starts_capture(self):
        control = TEXT.split('id: faceAuthenticationControl', 1)[1].split('Loader {', 1)[0]
        self.assertIn('typeof faceAuthenticator.registerThemeComponent === "function"', control)
        self.assertIn('qrc:/fallbacktheme/FaceAuthenticationControl.qml', control)
        self.assertNotIn('faceAuthenticator.start', TEXT)
        self.assertIn('item.usePasswordRequested.connect', control)
        self.assertIn('mainBlock.mainPasswordBox.forceActiveFocus()', control)

    def test_password_and_session_controls_cancel_face_attempt(self):
        for event in ('function onTextEdited()', 'onPasswordResult:', 'Keys.onEscapePressed:', 'onUiVisibleChanged:'):
            handler = TEXT.split(event, 1)[1].split('\n        }', 1)[0]
            self.assertRegex(handler, r'(faceAuthenticator\.cancel\(\)|cancelFaceAuthentication\(\))')
        self.assertIn('authenticator.respond(password)', TEXT)
        switch = TEXT.split('sessionManagement.switchUser()', 1)[0]
        self.assertIn('lockScreenUi.cancelFaceAuthentication()', switch[-200:])

    def test_success_uses_existing_greeter_authority(self):
        handler = TEXT.split('function onSucceeded()', 1)[1].split('}', 1)[0]
        self.assertIn('Qt.quit()', handler)
        self.assertNotIn('authenticator.succeeded', handler)

if __name__ == '__main__':
    unittest.main()
