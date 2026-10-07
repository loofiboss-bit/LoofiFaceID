// SPDX-License-Identifier: GPL-3.0-or-later
// qmllint disable unqualified

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import "components" as Components

Kirigami.ScrollablePage {
    id: root

    property bool backendReady: false
    property var enrollmentSession: null
    property var refresh: () => {}
    property var openSetup: () => {}

    title: i18n("Login and unlock")
    padding: Kirigami.Units.largeSpacing

    function modeIndex(mode) {
        switch (mode) {
        case "off":
            return 0
        case "on-activity":
            return 1
        case "manual":
            return 2
        default:
            return -1
        }
    }

    function restoreModeBindings() {
        sddmMode.currentIndex = Qt.binding(() => root.enrollmentSession !== null
            ? root.modeIndex(root.enrollmentSession.sddmAuthMode) : -1)
        plasmaMode.currentIndex = Qt.binding(() => root.enrollmentSession !== null
            ? root.modeIndex(root.enrollmentSession.plasmaLockAuthMode) : -1)
    }

    function applyMode(target, index) {
        if (root.backendReady && root.enrollmentSession !== null && index >= 0 && index <= 2) {
            const mode = ["off", "on-activity", "manual"][index]
            if (target === "sddm")
                root.enrollmentSession.setSddmAuthMode(mode)
            else
                root.enrollmentSession.setPlasmaLockAuthMode(mode)
        }
        root.restoreModeBindings()
    }

    ColumnLayout {
        width: root.availableWidth
        spacing: Kirigami.Units.largeSpacing

        Kirigami.InlineMessage {
            objectName: "backendInitializationMessage"
            Layout.fillWidth: true
            visible: !root.backendReady
            type: Kirigami.MessageType.Warning
            text: i18n("LoofiFace-ID is still initializing. Authentication controls will appear when the local backend is ready.")
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            type: Kirigami.MessageType.Warning
            text: i18n("Experimental authentication is off by default and requires a version-matched integration build. Face sign-in does not unlock KWallet or encrypted storage; keep your password available.")
        }

        Flow {
            Layout.fillWidth: true
            spacing: Kirigami.Units.mediumSpacing
            QQC2.Button {
                objectName: "refreshAuthStatus"
                width: Math.min(implicitWidth, parent.width)
                text: i18n("Refresh status")
                icon.name: "view-refresh"
                enabled: root.backendReady && root.enrollmentSession !== null && !root.enrollmentSession.systemAuthBusy
                activeFocusOnTab: true
                onClicked: root.refresh()
            }
            QQC2.Button {
                objectName: "openAuthRegistration"
                width: Math.min(implicitWidth, parent.width)
                text: i18n("Register profile")
                enabled: root.backendReady && root.enrollmentSession !== null && !root.enrollmentSession.systemAuthBusy
                activeFocusOnTab: true
                onClicked: root.openSetup()
            }
        }

        QQC2.BusyIndicator {
            Layout.alignment: Qt.AlignHCenter
            running: root.enrollmentSession !== null && root.enrollmentSession.systemAuthBusy
            visible: running
            Accessible.name: i18n("Authentication change in progress")
        }

        Kirigami.InlineMessage {
            objectName: "authOperationFeedback"
            Layout.fillWidth: true
            visible: root.enrollmentSession !== null && root.enrollmentSession.authOperationText.length > 0
            text: root.enrollmentSession !== null ? root.enrollmentSession.authOperationText : ""
            type: root.enrollmentSession !== null && root.enrollmentSession.authOperationErrorCode.length > 0
                ? Kirigami.MessageType.Warning : Kirigami.MessageType.Information
        }

        Kirigami.AbstractCard {
            Layout.fillWidth: true
            Accessible.role: Accessible.Grouping
            Accessible.name: i18n("SDDM sign-in authentication")

            contentItem: ColumnLayout {
                spacing: Kirigami.Units.mediumSpacing

                Kirigami.Heading {
                    Layout.fillWidth: true
                    level: -1
                    text: i18n("SDDM sign-in")
                }

                QQC2.Label {
                    objectName: "sddmIntegrationAvailability"
                    Layout.fillWidth: true
                    text: root.backendReady && root.enrollmentSession !== null
                        ? root.enrollmentSession.sddmIntegrationStatusText
                        : i18n("Checking the installed SDDM integration…")
                    wrapMode: Text.Wrap
                }

                QQC2.Label {
                    Layout.fillWidth: true
                    text: root.backendReady && root.enrollmentSession !== null
                        ? root.enrollmentSession.sddmAuthStatusText
                        : i18n("Authentication status is initializing.")
                    wrapMode: Text.Wrap
                    Accessible.name: text
                }

                Repeater {
                    model: root.enrollmentSession !== null ? root.enrollmentSession.sddmReadiness : []
                    delegate: Components.DetailRow {
                        required property var modelData
                        Layout.fillWidth: true
                        label: modelData.label
                        value: modelData.value
                        tone: modelData.ready ? 1 : 2
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    QQC2.Label {
                        text: i18n("When to try face authentication")
                        Accessible.name: text
                    }
                    QQC2.ComboBox {
                        id: sddmMode
                        objectName: "sddmAuthMode"
                        Layout.fillWidth: true
                        model: [i18n("Off"), i18n("On activity"), i18n("Button only")]
                        currentIndex: root.enrollmentSession !== null
                            ? root.modeIndex(root.enrollmentSession.sddmAuthMode) : -1
                        enabled: root.backendReady && root.enrollmentSession !== null
                            && !root.enrollmentSession.systemAuthBusy
                        displayText: currentIndex >= 0 ? currentText : i18n("Unknown")
                        activeFocusOnTab: true
                        Accessible.name: i18n("SDDM face-authentication mode")
                        onActivated: index => root.applyMode("sddm", index)
                    }
                }
            }
        }

        Kirigami.AbstractCard {
            Layout.fillWidth: true
            Accessible.role: Accessible.Grouping
            Accessible.name: i18n("Plasma unlock authentication")

            contentItem: ColumnLayout {
                spacing: Kirigami.Units.mediumSpacing

                Kirigami.Heading {
                    Layout.fillWidth: true
                    level: -1
                    text: i18n("Plasma screen unlock")
                }

                QQC2.Label {
                    objectName: "plasmaIntegrationAvailability"
                    Layout.fillWidth: true
                    text: root.backendReady && root.enrollmentSession !== null
                        ? root.enrollmentSession.plasmaLockIntegrationStatusText
                        : i18n("Checking the installed KScreenLocker integration…")
                    wrapMode: Text.Wrap
                }

                QQC2.Label {
                    Layout.fillWidth: true
                    text: root.backendReady && root.enrollmentSession !== null
                        ? root.enrollmentSession.plasmaLockAuthStatusText
                        : i18n("Authentication status is initializing.")
                    wrapMode: Text.Wrap
                    Accessible.name: text
                }

                Repeater {
                    model: root.enrollmentSession !== null ? root.enrollmentSession.plasmaLockReadiness : []
                    delegate: Components.DetailRow {
                        required property var modelData
                        Layout.fillWidth: true
                        label: modelData.label
                        value: modelData.value
                        tone: modelData.ready ? 1 : 2
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    QQC2.Label {
                        text: i18n("When to try face authentication")
                        Accessible.name: text
                    }
                    QQC2.ComboBox {
                        id: plasmaMode
                        objectName: "plasmaLockAuthMode"
                        Layout.fillWidth: true
                        model: [i18n("Off"), i18n("On activity"), i18n("Button only")]
                        currentIndex: root.enrollmentSession !== null
                            ? root.modeIndex(root.enrollmentSession.plasmaLockAuthMode) : -1
                        enabled: root.backendReady && root.enrollmentSession !== null
                            && !root.enrollmentSession.systemAuthBusy
                        displayText: currentIndex >= 0 ? currentText : i18n("Unknown")
                        activeFocusOnTab: true
                        Accessible.name: i18n("Plasma unlock face-authentication mode")
                        onActivated: index => root.applyMode("plasma-lock", index)
                    }
                }
            }
        }

        Kirigami.AbstractCard {
            Layout.fillWidth: true
            Accessible.role: Accessible.Grouping
            Accessible.name: i18n("System face-profile status")

            contentItem: ColumnLayout {
                Kirigami.Heading {
                    Layout.fillWidth: true
                    level: 4
                    text: i18n("System profile")
                }
                QQC2.Label {
                    objectName: "systemProfileFreshness"
                    Layout.fillWidth: true
                    text: root.backendReady && root.enrollmentSession !== null
                        ? root.enrollmentSession.systemProfileFreshnessText
                        : i18n("System-profile freshness is initializing.")
                    wrapMode: Text.Wrap
                }
                QQC2.Button {
                    objectName: "syncSystemProfile"
                    Layout.fillWidth: true
                    text: i18n("Sync system profile")
                    icon.name: "view-refresh"
                    enabled: root.backendReady && root.enrollmentSession !== null && root.enrollmentSession.systemProfileCanSync
                    activeFocusOnTab: true
                    onClicked: root.enrollmentSession.syncSystemProfile()
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    text: i18n("Refreshing status never starts the camera. Theme registration must be observed in the real login or unlock process; installed support alone does not confirm it.")
                    wrapMode: Text.Wrap
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    text: i18n("Each setting belongs to this user and this target. Changing or deleting a local profile first revokes its system copy; if revocation fails, the local change is stopped.")
                    wrapMode: Text.Wrap
                }
            }
        }
    }
}
