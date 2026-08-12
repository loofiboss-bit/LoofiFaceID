// SPDX-License-Identifier: GPL-3.0-or-later
// qmllint disable unqualified

import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kcmutils as KCMUtils

KCMUtils.SimpleKCM {
    id: root

    title: i18n("KFaceAuth (Experimental Local Identity Preview)")
    KCMUtils.ConfigModule.buttons: KCMUtils.ConfigModule.NoAdditionalButton

    header: Kirigami.NavigationTabBar {
        id: tabs
        objectName: "navigationTabs"

        width: parent.width
        Accessible.name: i18n("KFaceAuth sections")
        KeyNavigation.down: stack.children[tabs.currentIndex]

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
            openSetup: () => tabs.currentIndex = 1
            openTest: () => tabs.currentIndex = 2
            openDiagnostics: () => tabs.currentIndex = 3
            refresh: () => kcm.refresh()
        }

        SetupPage {
            id: setup

            Layout.fillWidth: true
            systemState: kcm.systemState
            cameraPreviewSession: kcm.cameraPreviewSession
            visionAnalysisSession: kcm.visionAnalysisSession
            enrollmentSession: kcm.enrollmentSession
            openTest: () => tabs.currentIndex = 2
        }

        TestPage {
            id: test

            Layout.fillWidth: true
            cameraPreviewSession: kcm.cameraPreviewSession
            localVerificationSession: kcm.localVerificationSession
        }

        DiagnosticsPage {
            id: diagnostics

            Layout.fillWidth: true
            systemState: kcm.systemState
            supportReport: kcm.supportReport
            cameraPreviewSession: kcm.cameraPreviewSession
            refreshActive: kcm.refreshing
            refresh: () => kcm.refresh()
        }
    }
}
