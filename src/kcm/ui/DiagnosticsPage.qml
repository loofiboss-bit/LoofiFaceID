// SPDX-License-Identifier: GPL-3.0-or-later
// qmllint disable unqualified

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import "components" as Components

Kirigami.ScrollablePage {
    id: root

    required property var systemState
    required property var supportReport
    required property var cameraPreviewSession
    required property bool refreshActive
    property var refresh: () => {}

    title: i18n("Diagnostics")
    padding: Kirigami.Units.largeSpacing

    ColumnLayout {
        width: root.availableWidth
        spacing: Kirigami.Units.largeSpacing

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: true
            type: Kirigami.MessageType.Information
            text: i18n("Diagnostics are read-only. Refreshing runs bounded local probes and never performs biometric or PAM operations.")
        }

        Components.ActionableIssue {
            issueTitle: supportReport.hasIssue ? supportReport.issueTitle : ""
            recoveryText: supportReport.hasIssue ? supportReport.recommendedAction : ""
        }

        Kirigami.AbstractCard {
            Layout.fillWidth: true
            Accessible.role: Accessible.Grouping
            Accessible.name: i18n("Current capability status")

            contentItem: ColumnLayout {
                Components.DetailRow {
                    Layout.fillWidth: true
                    label: i18n("Worker and runtime")
                    value: systemState.engineStatusLabel
                    tone: systemState.engineReady ? 1 : 2
                }

                Kirigami.Separator { Layout.fillWidth: true }

                Components.DetailRow {
                    Layout.fillWidth: true
                    label: i18n("Verified models")
                    value: systemState.modelStatusLabel
                    tone: systemState.modelsVerified ? 1 : 3
                }

                Kirigami.Separator { Layout.fillWidth: true }

                Components.DetailRow {
                    Layout.fillWidth: true
                    label: i18n("Camera availability")
                    value: i18np("%1 camera found", "%1 cameras found", root.cameraPreviewSession.deviceCount)
                    tone: root.cameraPreviewSession.deviceCount > 0 ? 1 : 0
                }

                Kirigami.Separator { Layout.fillWidth: true }

                Components.DetailRow {
                    Layout.fillWidth: true
                    label: i18n("KWallet")
                    value: systemState.keyProviderStatusLabel
                    tone: systemState.keyAvailable ? 1 : 2
                }

                Kirigami.Separator { Layout.fillWidth: true }

                Components.DetailRow {
                    Layout.fillWidth: true
                    label: i18n("Encrypted profile")
                    value: systemState.vaultStatusLabel
                    tone: systemState.vaultReady ? 1 : 2
                }

                Components.DetailRow {
                    Layout.fillWidth: true
                    label: i18n("Profile samples")
                    value: systemState.profileEnrolled
                        ? i18np("%1 encrypted sample", "%1 encrypted samples", systemState.profileSampleCount)
                        : i18n("No profile")
                    tone: systemState.profileEnrolled ? 1 : 0
                }

                QQC2.Button {
                    objectName: "diagnosticsRefreshButton"
                    Layout.alignment: Qt.AlignRight
                    text: root.refreshActive ? i18n("Updating…") : i18n("Refresh diagnostics")
                    icon.name: "view-refresh"
                    enabled: !root.refreshActive
                    Accessible.name: text
                    onClicked: root.refresh()
                }
            }
        }

        Kirigami.AbstractCard {
            Layout.fillWidth: true
            Accessible.role: Accessible.Grouping
            Accessible.name: i18n("Environment details")

            contentItem: ColumnLayout {
                QQC2.Button {
                    id: environmentToggle
                    objectName: "environmentDetailsButton"
                    text: environmentDetails.visible ? i18n("Hide environment details") : i18n("Show environment details")
                    icon.name: environmentDetails.visible ? "arrow-up" : "arrow-down"
                    checkable: true
                    checked: environmentDetails.visible
                    activeFocusOnTab: true
                    Accessible.name: text
                    onClicked: environmentDetails.visible = checked
                }

                ColumnLayout {
                    id: environmentDetails
                    Layout.fillWidth: true
                    visible: false
                    spacing: Kirigami.Units.smallSpacing

                    Components.DetailRow {
                        Layout.fillWidth: true
                        label: i18n("Secure Boot")
                        value: systemState.secureBootStatusLabel
                        tone: 0
                    }

                    Components.DetailRow {
                        Layout.fillWidth: true
                        label: i18n("Display manager")
                        value: systemState.activeDisplayManager
                        tone: 0
                    }

                    Components.DetailRow {
                        Layout.fillWidth: true
                        label: i18n("System authentication")
                        value: i18n("Not implemented")
                        tone: 2
                    }
                }
            }
        }

        Kirigami.AbstractCard {
            Layout.fillWidth: true
            Accessible.role: Accessible.Grouping
            Accessible.name: i18n("Redacted support report preview")

            contentItem: ColumnLayout {
                QQC2.TextArea {
                    Layout.fillWidth: true
                    Layout.minimumHeight: Kirigami.Units.gridUnit * 10
                    text: supportReport.report
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.Wrap
                    Accessible.name: i18n("Redacted support report")
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing

                    QQC2.Button {
                        objectName: "copyReportButton"
                        text: i18n("Copy report")
                        icon.name: "edit-copy"
                        Accessible.name: text
                        onClicked: supportReport.copyReport()
                    }

                    QQC2.Button {
                        objectName: "exportReportButton"
                        text: i18n("Export report")
                        icon.name: "document-save"
                        Accessible.name: text
                        Accessible.description: i18n("Saves a redacted Markdown report in Documents")
                        onClicked: supportReport.exportReport()
                    }
                }

                QQC2.Label {
                    Layout.fillWidth: true
                    visible: supportReport.statusText.length > 0
                    text: supportReport.statusText
                    color: Kirigami.Theme.disabledTextColor
                    wrapMode: Text.Wrap
                }
            }
        }
    }
}
