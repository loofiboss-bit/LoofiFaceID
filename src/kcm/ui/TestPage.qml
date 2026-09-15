// SPDX-License-Identifier: GPL-3.0-or-later
// qmllint disable unqualified

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import "components" as Components

Kirigami.ScrollablePage {
    id: root

    property var cameraPreviewSession: null
    property var localVerificationSession: null
    property bool backendReady: root.cameraPreviewSession !== null
        && root.localVerificationSession !== null

    title: i18n("Test")
    padding: Kirigami.Units.mediumSpacing

    onVisibleChanged: {
        if (!root.backendReady)
            return

        root.localVerificationSession.setPageActive(visible)
        if (visible) {
            if (!root.cameraPreviewSession.hasUsableCamera)
                root.cameraPreviewSession.refreshDevices()
        } else {
            root.cameraPreviewSession.stopPreview()
        }
    }

    onBackendReadyChanged: {
        if (root.backendReady && root.visible)
            root.localVerificationSession.setPageActive(true)
    }

    ColumnLayout {
        width: root.availableWidth
        spacing: Kirigami.Units.mediumSpacing

        Kirigami.InlineMessage {
            objectName: "backendInitializationMessage"
            Layout.fillWidth: true
            visible: !root.backendReady
            type: Kirigami.MessageType.Warning
            text: i18n("LoofiFace-ID is still initializing. The local comparison controls will become available when the backend is ready.")
        }

        GridLayout {
            Layout.fillWidth: true
            columns: root.availableWidth >= 700 ? 2 : 1
            columnSpacing: Kirigami.Units.largeSpacing
            rowSpacing: Kirigami.Units.largeSpacing

            // LEFT COLUMN: Private Test Preview Card
            Components.CameraPreviewCard {
                id: testCameraCard
                Layout.fillWidth: true
                Layout.preferredWidth: root.availableWidth >= 700 ? Math.round(root.availableWidth * 0.48) : root.availableWidth
                Layout.alignment: Qt.AlignTop
                cameraPreviewSession: root.cameraPreviewSession
                showFramingGuide: false
                cardTitle: i18n("Private test preview")
            }

            // RIGHT COLUMN: One-Frame Test Cockpit & Match Verdict
            Kirigami.AbstractCard {
                id: verificationCard
                Layout.fillWidth: true
                Layout.preferredWidth: root.availableWidth >= 700 ? Math.round(root.availableWidth * 0.52) : root.availableWidth
                Layout.alignment: Qt.AlignTop
                Accessible.role: Accessible.Grouping
                Accessible.name: i18n("One-frame test")

                contentItem: ColumnLayout {
                    spacing: Kirigami.Units.mediumSpacing

                    // Header Row
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        Kirigami.Icon {
                            source: "security-high"
                            implicitWidth: Kirigami.Units.iconSizes.medium
                            implicitHeight: Kirigami.Units.iconSizes.medium
                            color: Kirigami.Theme.highlightColor
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1

                            Kirigami.Heading {
                                level: 2
                                Layout.fillWidth: true
                                text: i18n("One-frame test")
                            }

                            QQC2.Label {
                                Layout.fillWidth: true
                                text: i18n("Test one explicit current frame against the encrypted profile. A result affects only this page.")
                                color: Kirigami.Theme.disabledTextColor
                                font.pointSize: Kirigami.Theme.smallFont.pointSize
                                wrapMode: Text.Wrap
                            }
                        }
                    }

                    Kirigami.Separator { Layout.fillWidth: true }

                    // Action Buttons Flow
                    Flow {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        QQC2.Button {
                            id: verifyButton
                            objectName: "verifyButton"
                            text: i18n("Test current frame")
                            icon.name: "view-preview"
                            enabled: root.localVerificationSession !== null && root.localVerificationSession.canVerify
                            activeFocusOnTab: true
                            Accessible.name: text
                            onClicked: {
                                if (root.localVerificationSession !== null)
                                    root.localVerificationSession.verifyCurrentFrame()
                            }
                        }

                        QQC2.Button {
                            id: clearButton
                            objectName: "clearVerificationButton"
                            text: i18n("Clear result")
                            icon.name: "edit-clear"
                            enabled: root.localVerificationSession !== null && root.localVerificationSession.canClearResult
                            activeFocusOnTab: true
                            Accessible.name: text
                            onClicked: {
                                if (root.localVerificationSession !== null)
                                    root.localVerificationSession.clearResult()
                            }
                        }

                        QQC2.BusyIndicator {
                            visible: root.localVerificationSession !== null && root.localVerificationSession.busy
                            running: visible
                            implicitWidth: Kirigami.Units.iconSizes.small
                            implicitHeight: Kirigami.Units.iconSizes.small
                            Accessible.ignored: true
                        }
                    }

                    // Visual Match Verdict Display Box
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 4.2
                        radius: Kirigami.Units.cornerRadius
                        color: (root.localVerificationSession !== null && root.localVerificationSession.hasResult)
                            ? (root.localVerificationSession.isMatch
                                ? Qt.alpha(Kirigami.Theme.positiveTextColor, 0.15)
                                : (root.localVerificationSession.isAmbiguous
                                    ? Qt.alpha(Kirigami.Theme.neutralTextColor, 0.15)
                                    : Qt.alpha(Kirigami.Theme.negativeTextColor, 0.15)))
                            : Qt.alpha(Kirigami.Theme.backgroundColor, 0.5)
                        border.color: (root.localVerificationSession !== null && root.localVerificationSession.hasResult)
                            ? (root.localVerificationSession.isMatch
                                ? Kirigami.Theme.positiveTextColor
                                : (root.localVerificationSession.isAmbiguous
                                    ? Kirigami.Theme.neutralTextColor
                                    : Kirigami.Theme.negativeTextColor))
                            : Qt.alpha(Kirigami.Theme.textColor, 0.2)
                        border.width: (root.localVerificationSession !== null && root.localVerificationSession.hasResult) ? 2 : 1

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: Kirigami.Units.mediumSpacing
                            spacing: Kirigami.Units.mediumSpacing

                            Kirigami.Icon {
                                source: (root.localVerificationSession !== null && root.localVerificationSession.hasResult)
                                    ? (root.localVerificationSession.isMatch
                                        ? "emblem-checked"
                                        : (root.localVerificationSession.isAmbiguous
                                            ? "dialog-warning"
                                            : "dialog-cancel"))
                                    : "view-preview"
                                implicitWidth: Kirigami.Units.iconSizes.large
                                implicitHeight: Kirigami.Units.iconSizes.large
                                color: (root.localVerificationSession !== null && root.localVerificationSession.hasResult)
                                    ? (root.localVerificationSession.isMatch
                                        ? Kirigami.Theme.positiveTextColor
                                        : (root.localVerificationSession.isAmbiguous
                                            ? Kirigami.Theme.neutralTextColor
                                            : Kirigami.Theme.negativeTextColor))
                                    : Kirigami.Theme.disabledTextColor
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2

                                QQC2.Label {
                                    Layout.fillWidth: true
                                    text: root.localVerificationSession !== null
                                        ? root.localVerificationSession.statusText
                                        : i18n("Comparison service is initializing…")
                                    color: (root.localVerificationSession !== null && root.localVerificationSession.hasResult)
                                        ? (root.localVerificationSession.isMatch
                                            ? Kirigami.Theme.positiveTextColor
                                            : (root.localVerificationSession.isAmbiguous
                                                ? Kirigami.Theme.neutralTextColor
                                                : Kirigami.Theme.negativeTextColor))
                                        : Kirigami.Theme.disabledTextColor
                                    wrapMode: Text.Wrap
                                    font.weight: (root.localVerificationSession !== null && root.localVerificationSession.hasResult) ? Font.Bold : Font.Normal
                                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                                    Accessible.role: Accessible.StaticText
                                }
                            }
                        }
                    }

                    // Actionable Issue if test unavailable
                    Components.ActionableIssue {
                        issueTitle: root.localVerificationSession !== null && root.localVerificationSession.isUnavailable
                            ? i18n("Test unavailable")
                            : ""
                        recoveryText: root.localVerificationSession !== null && root.localVerificationSession.isUnavailable
                            ? root.localVerificationSession.statusText
                            : ""
                    }

                    // Informational disclaimer message
                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        type: Kirigami.MessageType.Warning
                        text: i18n("Match is an experimental in-session comparison only. It cannot unlock, authenticate, authorize, call PAM, invoke Polkit, or change the Linux session.")
                    }
                }
            }
        }
    }
}
