// SPDX-License-Identifier: GPL-3.0-or-later
// qmllint disable unqualified

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import "components" as Components

Kirigami.ScrollablePage {
    id: root

    property var systemState: null
    property var cameraPreviewSession: null
    property var enrollmentSession: null
    property bool backendReady: true
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

    QQC2.Dialog {
        id: homeResetConfirmation
        objectName: "homeResetProfileConfirmation"
        parent: QQC2.Overlay.overlay
        modal: true
        title: i18n("Reset corrupt profile?")
        standardButtons: QQC2.Dialog.Ok | QQC2.Dialog.Cancel
        onAccepted: {
            if (root.enrollmentSession !== null)
                root.enrollmentSession.resetUnreadable()
        }
        onClosed: homeResetButton.forceActiveFocus()

        QQC2.Label {
            width: Math.min(Kirigami.Units.gridUnit * 28, root.width)
            text: i18n("The unreadable vault and its KWallet key will be removed. This cannot recover the profile; you must enroll again.")
            wrapMode: Text.Wrap
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }
    }

    ColumnLayout {
        width: root.availableWidth
        spacing: Kirigami.Units.largeSpacing

        QQC2.Label {
            Layout.fillWidth: true
            text: i18n("LoofiFace-ID %1", root.productVersion)
            color: Kirigami.Theme.disabledTextColor
            font.weight: Font.DemiBold
        }

        Components.PrimaryStatusCard {
            Layout.fillWidth: true
            eyebrow: root.flowStateLabel
            title: root.needsAttention
                ? i18n("Fix the current problem")
                : (root.readyToTest
                    ? i18n("Test your profile")
                    : (root.needsCamera ? i18n("Get started") : i18n("Continue registration")))
            description: root.needsAttention
                ? i18n("Open Diagnostics for the current issue and a bounded recovery step.")
                : (root.readyToTest
                    ? i18n("Start a private preview in Test and compare one deliberate current frame locally.")
                    : (root.needsCamera
                        ? i18n("Choose the only usable camera, start the private preview, and follow the guided registration.")
                        : i18n("Continue the guided registration. Five samples are recommended; saving always requires your click.")))
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

            QQC2.Button {
                id: homeResetButton
                objectName: "homeResetProfileButton"
                text: i18n("Reset corrupt profile")
                icon.name: "edit-clear-all"
                visible: root.enrollmentSession !== null && root.enrollmentSession.profileNeedsAttention
                enabled: root.enrollmentSession !== null && !root.enrollmentSession.busy
                activeFocusOnTab: true
                Accessible.name: text
                onClicked: homeResetConfirmation.open()
            }
        }

        Components.PrivacySummary {
            Layout.fillWidth: true
        }

        Kirigami.InlineMessage {
            objectName: "backendInitializationMessage"
            Layout.fillWidth: true
            visible: !root.backendReady
            type: Kirigami.MessageType.Warning
            text: i18n("LoofiFace-ID is still initializing. If this message remains, close and reopen System Settings.")
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            type: Kirigami.MessageType.Information
            text: i18n("LoofiFace-ID is experimental. It can compare one current frame in this logged-in session, but it cannot unlock, authenticate, authorize, or change system settings.")
        }
    }
}
