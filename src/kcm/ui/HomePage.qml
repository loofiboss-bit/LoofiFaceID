// SPDX-License-Identifier: GPL-3.0-or-later
// qmllint disable unqualified

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import "components" as Components

Kirigami.ScrollablePage {
    id: root

    required property QtObject systemState
    required property QtObject cameraPreviewSession
    required property QtObject enrollmentSession
    required property string productVersion
    required property string flowStateLabel
    required property string recommendedAction
    required property bool needsCamera
    required property bool needsProfile
    required property bool readyToTest
    required property bool needsAttention
    required property bool refreshActive
    property var openSetup: () => {}
    property var openTest: () => {}
    property var openDiagnostics: () => {}
    property var refresh: () => {}

    title: i18n("Home")
    padding: Kirigami.Units.largeSpacing

    function primaryAction() {
        if (root.needsAttention)
            root.openDiagnostics()
        else if (root.readyToTest)
            root.openTest()
        else
            root.openSetup()
    }

    ColumnLayout {
        width: root.availableWidth
        spacing: Kirigami.Units.largeSpacing

        QQC2.Label {
            Layout.fillWidth: true
            text: i18n("KFaceAuth %1", root.productVersion)
            color: Kirigami.Theme.disabledTextColor
            font.weight: Font.DemiBold
        }

        Components.PrimaryStatusCard {
            Layout.fillWidth: true
            eyebrow: root.flowStateLabel
            title: root.needsAttention
                ? i18n("A local check needs attention")
                : (root.readyToTest
                    ? i18n("Your profile is ready to test")
                    : (root.needsCamera ? i18n("Start with a camera check") : i18n("Create your encrypted profile")))
            description: root.needsAttention
                ? i18n("Open Diagnostics for the current issue and a bounded recovery step.")
                : (root.readyToTest
                    ? i18n("Start a preview in Test, then test exactly one current frame.")
                    : (root.needsCamera
                        ? i18n("Setup guides you through camera discovery, a frame check, and profile creation.")
                        : i18n("Setup keeps the camera, frame check, and one-click enrollment in one guided path.")))
            actionText: root.recommendedAction
            actionIcon: root.needsAttention ? "tools-report-bug" : (root.readyToTest ? "view-preview" : "go-next")
            onPrimaryAction: root.primaryAction()
        }

        Flow {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            QQC2.Button {
                objectName: "homeRefreshButton"
                text: root.refreshActive ? i18n("Updating…") : i18n("Refresh status")
                icon.name: "view-refresh"
                enabled: !root.refreshActive
                activeFocusOnTab: true
                Accessible.name: text
                onClicked: root.refresh()
            }

            QQC2.Button {
                objectName: "homeSetupButton"
                text: i18n("Open Setup")
                icon.name: "camera-photo"
                flat: true
                activeFocusOnTab: true
                Accessible.name: text
                onClicked: root.openSetup()
            }
        }

        Components.PrivacySummary {
            Layout.fillWidth: true
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            type: Kirigami.MessageType.Information
            text: i18n("KFaceAuth is experimental. It can compare one current frame in this logged-in session, but it cannot unlock, authenticate, authorize, or change system settings.")
        }
    }
}
