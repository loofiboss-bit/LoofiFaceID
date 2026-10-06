from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path


ROOT = Path(sys.argv[1]).resolve()


def source(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


class KScreenLockerPatchContract(unittest.TestCase):
    def test_experimental_build_is_off_by_default(self) -> None:
        cmake = source("greeter/CMakeLists.txt")
        self.assertIn(
            'option(KSCREENLOCKER_ENABLE_EXPERIMENTAL_FACE_AUTH "Build the opt-in KFaceAuth lock-screen factor." OFF)',
            cmake,
        )
        self.assertIn('set(KSCREENLOCKER_PAM_FACE_SERVICE "kde-kfaceauth"', cmake)
        self.assertIn('set(KSCREENLOCKER_PAM_PASSWORD_SERVICE "\\"kde\\"")', cmake)
        self.assertIn("${KDE_INSTALL_DATADIR}/kfaceauth/integrations", cmake)

    def test_build_marker_only_reports_component_api_and_upstream_version(self) -> None:
        cmake = source("greeter/CMakeLists.txt")
        experimental_build = cmake.split("if (KSCREENLOCKER_ENABLE_EXPERIMENTAL_FACE_AUTH)", 1)[1].split("endif()", 1)[0]
        self.assertIn("configure_file(", experimental_build)
        self.assertIn("install(FILES ${CMAKE_CURRENT_BINARY_DIR}/kfaceauth-kscreenlocker.json", experimental_build)
        marker = json.loads(source("greeter/kfaceauth-kscreenlocker.json.in"))
        self.assertEqual(marker["schema_version"], 1)
        self.assertEqual(marker["component_id"], "org.loofi.kfaceauth.kscreenlocker")
        self.assertEqual(marker["kscreenlocker_version"], "@PROJECT_VERSION@")
        self.assertEqual(marker["theme_contract"]["interface_version"], 1)
        self.assertEqual(marker["theme_contract"]["component_url"], "qrc:/fallbacktheme/FaceAuthenticationControl.qml")
        self.assertTrue(marker["theme_contract"]["runtime_registration_required"])
        self.assertEqual(set(marker), {
            "schema_version",
            "component_id",
            "kscreenlocker_version",
            "theme_contract",
        })

    def test_attempt_is_gated_until_supported_qml_component_registers(self) -> None:
        authenticator = source("greeter/facepamauthenticator.cpp")
        header = source("greeter/facepamauthenticator.h")
        qml = source("greeter/fallbacktheme/FaceAuthenticationControl.qml")
        self.assertIn("!m_themeComponents.isEmpty()", authenticator)
        self.assertIn("interfaceVersion != InterfaceV1", authenticator)
        self.assertIn("m_themeComponents.isEmpty()", authenticator)
        self.assertIn("registerThemeComponent(root, ScreenLocker.FaceAuthenticator.InterfaceV1)", qml)
        self.assertIn("Q_INVOKABLE bool registerThemeComponent", header)

    def test_one_face_attempt_object_is_shared_by_all_views(self) -> None:
        app = source("greeter/greeterapp.cpp")
        self.assertIn('context->setContextProperty(QStringLiteral("faceAuthenticator"), m_faceAuthenticator);', app)
        self.assertIn("m_faceAuthenticator->cancel();", app)
        self.assertIn("cancelFaceAuthenticationIfNoViewsVisible", app)
        self.assertIn("std::none_of(m_views.cbegin(), m_views.cend()", app)

    def test_view_creation_does_not_start_the_camera(self) -> None:
        app = source("greeter/greeterapp.cpp")
        view_creation = app.split("PlasmaQuick::QuickViewSharedEngine *UnlockApp::createViewForScreen", 1)[1]
        view_creation = view_creation.split("void UnlockApp::markViewsAsVisible", 1)[0]
        self.assertNotIn("startAttempt", view_creation)
        self.assertNotIn("notifyUserActivity", view_creation)

    def test_activity_mode_waits_for_post_lock_input_on_a_visible_view(self) -> None:
        app = source("greeter/greeterapp.cpp")
        event_filter = app.split("bool UnlockApp::eventFilter", 1)[1]
        show_handling = event_filter.split("if (obj != this && event->type() == QEvent::Show)", 1)[1]
        show_handling = show_handling.split("const bool userActivity", 1)[0]
        self.assertNotIn("notifyUserActivity", show_handling)
        self.assertIn("event->type() == QEvent::MouseButtonPress", event_filter)
        self.assertIn("event->type() == QEvent::TouchBegin", event_filter)
        self.assertIn("event->type() == QEvent::TabletPress", event_filter)
        self.assertIn("event->type() == QEvent::KeyPress", event_filter)
        self.assertIn("if (userActivity && unlockViewVisible && !m_authenticators->isBusy())", event_filter)
        self.assertIn("m_faceAuthenticator->notifyUserActivity();", event_filter)
        self.assertIn("m_faceAuthenticator->resetActivityCycle();", app)

    def test_per_user_policy_helper_is_bounded_async_and_fail_closed(self) -> None:
        cmake = source("greeter/CMakeLists.txt")
        self.assertIn('KSCREENLOCKER_FACE_POLICY_HELPER="/usr/libexec/kfaceauth-policy"', cmake)
        authenticator = source("greeter/facepamauthenticator.cpp")
        lookup = authenticator.split("void FacePamAuthenticator::startPolicyLookup()", 1)[1]
        lookup = lookup.split("void FacePamAuthenticator::readPolicyOutput", 1)[0]
        self.assertIn('QStringLiteral("--get")', lookup)
        self.assertIn('QStringLiteral("--uid")', lookup)
        self.assertIn('QStringLiteral("plasma-lock")', lookup)
        self.assertIn("m_policyDeadline.start(policyLookupLimit)", lookup)
        self.assertNotIn("waitForFinished", lookup)
        self.assertIn("constexpr auto policyLookupLimit = 250ms", authenticator)
        self.assertIn("m_activityDeadline.start(policyLookupLimit)", authenticator)
        self.assertIn('QByteArrayLiteral("mode=off\\n")', authenticator)
        self.assertIn('QByteArrayLiteral("mode=manual\\n")', authenticator)
        self.assertIn('QByteArrayLiteral("mode=on-activity\\n")', authenticator)
        self.assertIn("QString mode = QStringLiteral(\"unknown\")", authenticator)
        self.assertIn("m_policyReady && (m_policyMode == QLatin1String(\"manual\") || m_policyMode == QLatin1String(\"on-activity\"))", authenticator)

    def test_off_manual_and_on_activity_have_distinct_start_triggers(self) -> None:
        authenticator = source("greeter/facepamauthenticator.cpp")
        available = authenticator.split("bool FacePamAuthenticator::isAvailable() const", 1)[1]
        available = available.split("bool FacePamAuthenticator::canStart() const", 1)[0]
        self.assertIn('QLatin1String("manual")', available)
        self.assertIn('QLatin1String("on-activity")', available)
        notify = authenticator.split("void FacePamAuthenticator::notifyUserActivity()", 1)[1]
        notify = notify.split("void FacePamAuthenticator::resetActivityCycle()", 1)[0]
        self.assertIn("m_policyMode == QLatin1String(\"on-activity\")", notify)
        self.assertIn("schedulePendingActivityAttempt();", notify)
        self.assertIn("m_activityAttemptConsumed", notify)
        qml = source("greeter/fallbacktheme/FaceAuthenticationControl.qml")
        self.assertIn("visible: faceAuthenticator.available", qml)

    def test_password_input_escape_and_selection_cancel_face_worker(self) -> None:
        qml = source("greeter/fallbacktheme/Greeter.qml")
        self.assertIn("onTextEdited:", qml)
        self.assertIn("faceAuthenticator.cancel();", qml)
        self.assertIn("Keys.onEscapePressed", qml)
        self.assertIn("if (!faceAttemptWasActive)", qml)
        self.assertIn("faceAuthenticator.cancel();\n                    switchUserClicked();", qml)
        self.assertIn("enabled: !authenticator.busy", qml)
        control = source("greeter/fallbacktheme/FaceAuthenticationControl.qml")
        self.assertIn('text: i18nd("kscreenlocker_greet", "Use password")', control)
        self.assertIn('text: faceAuthenticator.busy', control)
        self.assertGreaterEqual(control.count("Keys.onEscapePressed"), 2)

    def test_suspend_cancels_face_worker(self) -> None:
        app = source("greeter/greeterapp.cpp")
        sleep_handler = app.split("prepareForSleep", 1)[1].split("initialize();", 1)[0]
        self.assertIn("m_faceAuthenticator->cancel();", sleep_handler)

    def test_only_allowlisted_typed_statuses_reach_qml(self) -> None:
        worker = source("greeter/facepamworker.cpp")
        authenticator = source("greeter/facepamauthenticator.cpp")
        qml = source("greeter/fallbacktheme/FaceAuthenticationControl.qml")
        self.assertIn("pam_authenticate(handle, 0)", worker)
        for token, state in (
            ("KFACEAUTH_STATUS=starting-camera", "camera-starting"),
            ("KFACEAUTH_STATUS=looking-for-face", "looking-for-face"),
            ("KFACEAUTH_STATUS=use-password", "use-password"),
        ):
            self.assertIn(token, worker)
            self.assertIn(f'QByteArrayLiteral("{state}")', authenticator)
        self.assertIn("PAM text never reaches QML", authenticator)
        self.assertIn("Look at the camera", qml)
        self.assertIn("Use your password or try again", qml)

    def test_status_is_never_an_authorization_result(self) -> None:
        authenticator = source("greeter/facepamauthenticator.cpp")
        app = source("greeter/greeterapp.cpp")
        self.assertIn("exitStatus == QProcess::NormalExit && exitCode == 0", authenticator)
        self.assertIn("m_authenticators->isUnlocked() || m_faceAuthenticator->isUnlocked()", app)
        self.assertNotIn("m_unlocked = true", authenticator.split("void FacePamAuthenticator::readStatus", 1)[1].split("void FacePamAuthenticator::finishProcess", 1)[0])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
