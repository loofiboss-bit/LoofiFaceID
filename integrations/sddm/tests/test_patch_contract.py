from __future__ import annotations

import sys
import unittest
from pathlib import Path


ROOT = Path(sys.argv[1]).resolve()


def source(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


class SddmFaceAuthenticationContract(unittest.TestCase):
    def test_experimental_build_is_opt_in_and_marker_is_versioned(self) -> None:
        cmake = source("CMakeLists.txt")
        greeter_cmake = source("src/greeter/CMakeLists.txt")
        marker = source("src/greeter/kfaceauth-sddm-api-v1.conf")
        self.assertIn('option(SDDM_ENABLE_EXPERIMENTAL_FACE_AUTH "Build the opt-in KFaceAuth greeter integration." OFF)', cmake)
        self.assertIn("if(SDDM_ENABLE_EXPERIMENTAL_FACE_AUTH)", greeter_cmake)
        self.assertIn('${DATA_INSTALL_DIR}/kfaceauth', greeter_cmake)
        self.assertIn("api=1", marker)
        self.assertIn("sddm_version=0.21.0", marker)
        self.assertIn("upstream_commit=63780fcd79f1dbf81a30eef48c28c699ab15aded", marker)

    def test_active_theme_and_loaded_qml_must_negotiate_api_v1(self) -> None:
        greeter = source("src/daemon/Greeter.cpp")
        display = source("src/daemon/Display.cpp")
        socket_server = source("src/daemon/SocketServer.cpp")
        control = source("src/greeter/theme/FaceAuthenticationControl.qml")
        self.assertIn('QStringLiteral("faceAuthenticationApi")).toInt() == 1', greeter)
        self.assertIn("faceAuthenticationApi=1", source("src/greeter/theme/theme.conf"))
        self.assertIn('"/kfaceauth/sddm-face-auth-api-v1.conf"', display)
        self.assertIn('"/etc/pam.d/sddm-kfaceauth"', display)
        self.assertIn("faceAuthenticationUiReady(socket)", display)
        self.assertIn("FaceAuthenticationApiVersion", socket_server)
        self.assertIn("sddm.registerFaceAuthenticationUi(1)", control)
        self.assertIn("sddm.faceAuthenticationUiReady", control)

    def test_original_password_api_and_service_are_preserved(self) -> None:
        proxy_header = source("src/greeter/GreeterProxy.h")
        proxy = source("src/greeter/GreeterProxy.cpp")
        backend = source("src/helper/backend/PamBackend.cpp")
        self.assertIn("void login(const QString &user, const QString &password, const int sessionIndex) const;", proxy_header)
        self.assertIn("void GreeterProxy::login(const QString &user, const QString &password, const int sessionIndex) const", proxy)
        self.assertIn('QStringLiteral("sddm-kfaceauth")', backend)
        self.assertIn('QString service = QStringLiteral("sddm");', backend)
        self.assertIn("m_pam->authenticate()", source("src/helper/backend/PamBackend.cpp"))
        self.assertIn("m_pam->acctMgmt()", source("src/helper/backend/PamBackend.cpp"))
        self.assertNotIn("password.isEmpty()", proxy)

    def test_face_attempt_requires_explicit_selected_user_action(self) -> None:
        qml = source("src/greeter/theme/Main.qml")
        control = source("src/greeter/theme/FaceAuthenticationControl.qml")
        proxy = source("src/greeter/GreeterProxy.cpp")
        display = source("src/daemon/Display.cpp")
        self.assertIn("selectedUser: listView.currentItem ? listView.currentItem.userName : \"\"", qml)
        self.assertIn("authenticationMode === \"on-activity\"", control)
        self.assertIn("sddm.FaceAuthenticationUserActivity", control)
        self.assertIn("sddm.FaceAuthenticationExplicitButton", control)
        self.assertIn("setFaceAuthenticationSelectedUser(selectedUser)", control)
        self.assertNotIn("Keys.onReturnPressed", control)
        self.assertNotIn("sddm.startFaceAuthentication", qml.split("onCurrentIndexChanged:", 1)[1].split("KeyNavigation", 1)[0])
        self.assertIn("user != d->faceAuthenticationSelectedUser", proxy)
        self.assertIn("d->faceAuthenticationAutoAttempted", proxy)
        self.assertIn("/usr/libexec/kfaceauth-policy", display)
        self.assertIn('mode == QLatin1String("on-activity")', display)
        self.assertIn('mode == QLatin1String("manual")', display)
        self.assertIn("m_faceUserUid = static_cast<quint64>(account->pw_uid)", display)
        self.assertIn("queryFaceAuthenticationMode(m_faceUserUid", display)
        self.assertIn('authenticationMode === "manual" || authenticationMode === "on-activity"', control)
        self.assertIn("suppressFaceAuthenticationActivity()", control)

    def test_per_uid_policy_queries_are_bounded_and_fail_closed(self) -> None:
        display = source("src/daemon/Display.cpp")
        socket_server = source("src/daemon/SocketServer.cpp")
        self.assertIn('QStringLiteral("--get")', display)
        self.assertIn('QStringLiteral("--uid")', display)
        self.assertIn('QStringLiteral("--target"), QStringLiteral("sddm")', display)
        self.assertNotIn("if (uidValue == 0)", display)
        self.assertIn('QByteArrayLiteral("mode=off\\n")', display)
        self.assertIn('QByteArrayLiteral("mode=manual\\n")', display)
        self.assertIn('QByteArrayLiteral("mode=on-activity\\n")', display)
        self.assertIn("timeout->start(400)", display)
        self.assertIn('QStringLiteral("unknown")', display)
        self.assertIn("faceAuthenticationModeRequested", socket_server)
        self.assertIn("FaceAuthenticationTrigger::UserActivity", display)
        self.assertIn("FaceAuthenticationTrigger::ExplicitButton", source("src/daemon/Display.h"))

    def test_password_input_escape_switch_hide_and_disconnect_cancel(self) -> None:
        app = source("src/greeter/GreeterApp.cpp")
        qml = source("src/greeter/theme/Main.qml")
        socket_server = source("src/daemon/SocketServer.cpp")
        proxy = source("src/greeter/GreeterProxy.cpp")
        self.assertIn('indexOfProperty("echoMode")', app)
        self.assertIn("QEvent::InputMethod", app)
        self.assertIn("Qt::Key_Escape", app)
        self.assertIn("sddm.cancelFaceAuthentication()", qml)
        self.assertIn("candidate->isVisible()", app)
        self.assertIn("emit cancelFaceLogin(socket, 0)", socket_server)
        self.assertIn("const_cast<GreeterProxy *>(this)->cancelFaceAuthentication()", proxy)

    def test_deadline_cancel_and_late_status_are_attempt_bound(self) -> None:
        display = source("src/daemon/Display.cpp")
        proxy = source("src/greeter/GreeterProxy.cpp")
        auth = source("src/auth/Auth.cpp")
        self.assertIn("m_faceAuthDeadline.start(2000)", display)
        self.assertIn("attemptId != m_faceAttemptId", display)
        self.assertIn("if (attemptId != d->faceAuthenticationAttemptId)", proxy)
        self.assertIn("++d->faceAuthenticationAttemptId", proxy)
        self.assertIn("d->socket->abort()", auth)
        self.assertIn("d->child->terminate()", auth)
        self.assertIn("QTimer::singleShot(150", auth)
        self.assertIn("d->child->kill()", auth)

    def test_only_fixed_pam_tokens_are_displayed_and_status_is_not_auth(self) -> None:
        display = source("src/daemon/Display.cpp")
        self.assertIn('token == QLatin1String("starting-camera")', display)
        self.assertIn('token == QLatin1String("looking-for-face")', display)
        self.assertIn('token == QLatin1String("use-password")', display)
        info_handler = display.split("void Display::slotFaceAuthInfo", 1)[1].split("void Display::slotFaceAuthError", 1)[0]
        self.assertNotIn("m_faceAuthenticationAccepted = true", info_handler)
        auth_result = display.split("void Display::slotFaceAuthenticationFinished", 1)[1].split("void Display::slotFaceSessionStarted", 1)[0]
        self.assertIn("if (!success)", auth_result)
        self.assertIn("user != m_faceUser", auth_result)
        self.assertIn("pw_uid) != m_faceUserUid", auth_result)

    def test_multiple_views_share_one_attempt_and_retry_is_explicit(self) -> None:
        app = source("src/greeter/GreeterApp.cpp")
        display = source("src/daemon/Display.cpp")
        control = source("src/greeter/theme/FaceAuthenticationControl.qml")
        proxy = source("src/greeter/GreeterProxy.cpp")
        self.assertIn("m_proxy->faceAuthenticationActive()", app)
        self.assertIn("faceActivityGeneration()", app)
        self.assertIn("eventSerial == m_faceActivityEventSerial", app)
        self.assertIn("m_faceAuthBusy", display)
        self.assertIn("m_faceAuthDeadline.start(2000)", display)
        self.assertIn("QDBusConnection::systemBus().asyncCall(message)", display)
        self.assertNotIn("candidate->service()", display)
        self.assertNotIn("candidateSession->state()", display)
        self.assertIn("faceAuthenticationAutoAttempted = false", proxy)
        self.assertIn('qsTr("Try face again")', control)
        self.assertIn("onClicked: button.activate()", control)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
