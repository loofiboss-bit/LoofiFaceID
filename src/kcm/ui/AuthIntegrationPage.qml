// SPDX-License-Identifier: GPL-3.0-or-later
// qmllint disable unqualified

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: root

    property bool backendReady: false
    property var enrollmentSession: null

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
            return 3
        }
    }

    function applyMode(target, index) {
        if (!root.backendReady || root.enrollmentSession === null || index < 0 || index > 2)
            return
        if (index !== 0 && !(target === "sddm"
                ? root.enrollmentSession.sddmAuthCanEnable
                : root.enrollmentSession.plasmaLockAuthCanEnable))
            return
        const mode = ["off", "on-activity", "manual"][index]
        if (target === "sddm")
            root.enrollmentSession.setSddmAuthMode(mode)
        else
            root.enrollmentSession.setPlasmaLockAuthMode(mode)
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

        Kirigami.AbstractCard {
            Layout.fillWidth: true
            Accessible.role: Accessible.Grouping
            Accessible.name: i18n("SDDM sign-in authentication")

            contentItem: ColumnLayout {
                spacing: Kirigami.Units.mediumSpacing

                Kirigami.Heading {
                    Layout.fillWidth: true
                    level: 3
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
                        model: [i18n("Off"), i18n("On activity"), i18n("Button only"), i18n("Unknown")]
                        currentIndex: root.enrollmentSession !== null
                            ? root.modeIndex(root.enrollmentSession.sddmAuthMode) : 3
                        enabled: root.backendReady && root.enrollmentSession !== null
                            && !root.enrollmentSession.systemAuthBusy
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
                    level: 3
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
                        model: [i18n("Off"), i18n("On activity"), i18n("Button only"), i18n("Unknown")]
                        currentIndex: root.enrollmentSession !== null
                            ? root.modeIndex(root.enrollmentSession.plasmaLockAuthMode) : 3
                        enabled: root.backendReady && root.enrollmentSession !== null
                            && !root.enrollmentSession.systemAuthBusy
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
                QQC2.Label {
                    Layout.fillWidth: true
                    text: i18n("Each setting belongs to this user and this target. Changing or deleting a local profile first revokes its system copy; if revocation fails, the local change is stopped.")
                    wrapMode: Text.Wrap
                }
            }
        }
    }
}
