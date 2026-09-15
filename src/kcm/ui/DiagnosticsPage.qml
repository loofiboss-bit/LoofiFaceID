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
    property var supportReport: null
    property var cameraPreviewSession: null
    property bool backendReady: root.systemState !== null
        && root.supportReport !== null
        && root.cameraPreviewSession !== null
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

        Kirigami.InlineMessage {
            objectName: "backendInitializationMessage"
            Layout.fillWidth: true
            visible: !root.backendReady
            type: Kirigami.MessageType.Warning
            text: i18n("KFaceAuth is still initializing. Diagnostic values will appear when the local backend is ready.")
        }

        Components.ActionableIssue {
            issueTitle: root.supportReport !== null && root.supportReport.hasIssue
                ? root.supportReport.issueTitle
                : ""
            recoveryText: root.supportReport !== null && root.supportReport.hasIssue
                ? root.supportReport.recommendedAction
                : ""
        }

        Kirigami.AbstractCard {
            Layout.fillWidth: true
            Accessible.role: Accessible.Grouping
            Accessible.name: i18n("Current capability status")

            contentItem: ColumnLayout {
                Components.DetailRow {
                    Layout.fillWidth: true
                    label: i18n("Worker and runtime")
                    value: root.systemState !== null ? root.systemState.engineStatusLabel : i18n("Initializing…")
                    tone: root.systemState !== null && root.systemState.engineReady ? 1 : 2
                }

                Kirigami.Separator { Layout.fillWidth: true }

                Components.DetailRow {
                    Layout.fillWidth: true
                    label: i18n("Verified models")
                    value: root.systemState !== null ? root.systemState.modelStatusLabel : i18n("Initializing…")
                    tone: root.systemState !== null && root.systemState.modelsVerified ? 1 : 3
                }

                Kirigami.Separator { Layout.fillWidth: true }

                Components.DetailRow {
                    Layout.fillWidth: true
                    label: i18n("Camera availability")
                    value: root.cameraPreviewSession !== null
                        ? i18np("%1 camera found", "%1 cameras found", root.cameraPreviewSession.deviceCount)
                        : i18n("Initializing…")
                    tone: root.cameraPreviewSession !== null && root.cameraPreviewSession.deviceCount > 0 ? 1 : 0
                }

                Kirigami.Separator { Layout.fillWidth: true }

                Components.DetailRow {
                    Layout.fillWidth: true
                    label: i18n("KWallet")
                    value: root.systemState !== null ? root.systemState.keyProviderStatusLabel : i18n("Initializing…")
                    tone: root.systemState !== null && root.systemState.keyAvailable ? 1 : 2
                }

                Kirigami.Separator { Layout.fillWidth: true }

                Components.DetailRow {
                    Layout.fillWidth: true
                    label: i18n("Encrypted profile")
                    value: root.systemState !== null ? root.systemState.vaultStatusLabel : i18n("Initializing…")
                    tone: root.systemState !== null && root.systemState.vaultReady ? 1 : 2
                }

                Components.DetailRow {
                    Layout.fillWidth: true
                    label: i18n("Profile samples")
                    value: root.systemState !== null && root.systemState.profileEnrolled
                        ? i18np("%1 encrypted sample", "%1 encrypted samples", root.systemState.profileSampleCount)
                        : i18n("No profile")
                    tone: root.systemState !== null && root.systemState.profileEnrolled ? 1 : 0
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
                        value: root.systemState !== null ? root.systemState.secureBootStatusLabel : i18n("Initializing…")
                        tone: 0
                    }

                    Components.DetailRow {
                        Layout.fillWidth: true
                        label: i18n("Display manager")
                        value: root.systemState !== null ? root.systemState.activeDisplayManager : i18n("Initializing…")
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
                    text: root.supportReport !== null ? root.supportReport.report : i18n("Support report is initializing…")
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
                        enabled: root.supportReport !== null
                        Accessible.name: text
                        onClicked: {
                            if (root.supportReport !== null)
                                root.supportReport.copyReport()
                        }
                    }

                    QQC2.Button {
                        objectName: "exportReportButton"
                        text: i18n("Export report")
                        icon.name: "document-save"
                        enabled: root.supportReport !== null
                        Accessible.name: text
                        Accessible.description: i18n("Saves a redacted Markdown report in Documents")
                        onClicked: {
                            if (root.supportReport !== null)
                                root.supportReport.exportReport()
                        }
                    }
                }

                QQC2.Label {
                    Layout.fillWidth: true
                    visible: root.supportReport !== null && root.supportReport.statusText.length > 0
                    text: root.supportReport !== null ? root.supportReport.statusText : ""
                    color: Kirigami.Theme.disabledTextColor
                    wrapMode: Text.Wrap
                }
            }
        }
    }
}
