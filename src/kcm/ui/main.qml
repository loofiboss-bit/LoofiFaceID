// SPDX-License-Identifier: GPL-3.0-or-later
// qmllint disable unqualified

import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kcmutils as KCMUtils

KCMUtils.SimpleKCM {
    id: root

    title: i18n("LoofiFace-ID (Experimental Local Identity Preview)")

    // KCMUtils can create the QML surface before all backend objects have been
    // published into the context. Keep the surface usable during that short
    // window and let the destination pages render their explicit fallback.
    property bool backendReady: typeof kcm !== "undefined"
        && kcm !== null
        && kcm.systemState !== null
        && kcm.cameraPreviewSession !== null
        && kcm.visionAnalysisSession !== null
        && kcm.enrollmentSession !== null
        && kcm.localVerificationSession !== null
        && kcm.supportReport !== null

    header: Kirigami.NavigationTabBar {
        id: tabs
        objectName: "navigationTabs"

        width: parent.width
        Accessible.name: i18n("LoofiFace-ID sections")
        KeyNavigation.down: stack && tabs.currentIndex >= 0 ? stack.children[tabs.currentIndex] : null

        Kirigami.NavigationTabButton {
            objectName: "homeTab"
            text: i18n("Home")
            icon.name: "view-dashboard"
            Accessible.name: text
            activeFocusOnTab: true
        }

        Kirigami.NavigationTabButton {
            objectName: "setupTab"
            text: i18n("Setup")
            icon.name: "camera-photo"
            Accessible.name: text
            activeFocusOnTab: true
        }

        Kirigami.NavigationTabButton {
            objectName: "testTab"
            text: i18n("Test")
            icon.name: "view-preview"
            Accessible.name: text
            activeFocusOnTab: true
        }

        Kirigami.NavigationTabButton {
            objectName: "diagnosticsTab"
            text: i18n("Diagnostics")
            icon.name: "tools-report-bug"
            Accessible.name: text
            activeFocusOnTab: true
        }
    }

    StackLayout {
        id: stack
        width: parent.width
        currentIndex: tabs.currentIndex

        HomePage {
            id: home

            Layout.fillWidth: true
            backendReady: root.backendReady
            systemState: kcm.systemState
            cameraPreviewSession: kcm.cameraPreviewSession
            enrollmentSession: kcm.enrollmentSession
            productVersion: kcm.productVersion
            flowStateLabel: kcm.flowStateLabel
            recommendedAction: kcm.recommendedAction
            needsCamera: kcm.needsCamera
            needsProfile: kcm.needsProfile
            readyToTest: kcm.readyToTest
            needsAttention: kcm.needsAttention
            refreshActive: kcm.refreshing
            startOnboarding: () => {
                tabs.currentIndex = 1
                setup.beginFirstStart()
            }
            openSetup: () => tabs.currentIndex = 1
            openTest: () => tabs.currentIndex = 2
            openDiagnostics: () => tabs.currentIndex = 3
            refresh: () => kcm.refresh()
        }

        SetupPage {
            id: setup

            Layout.fillWidth: true
            backendReady: root.backendReady
            systemState: kcm.systemState
            cameraPreviewSession: kcm.cameraPreviewSession
            visionAnalysisSession: kcm.visionAnalysisSession
            enrollmentSession: kcm.enrollmentSession
            openTest: () => tabs.currentIndex = 2
        }

        TestPage {
            id: test

            Layout.fillWidth: true
            backendReady: root.backendReady
            cameraPreviewSession: kcm.cameraPreviewSession
            localVerificationSession: kcm.localVerificationSession
        }

        DiagnosticsPage {
            id: diagnostics

            Layout.fillWidth: true
            backendReady: root.backendReady
            systemState: kcm.systemState
            supportReport: kcm.supportReport
            cameraPreviewSession: kcm.cameraPreviewSession
            enrollmentSession: kcm.enrollmentSession
            refreshActive: kcm.refreshing
            refresh: () => kcm.refresh()
        }
    }
}
