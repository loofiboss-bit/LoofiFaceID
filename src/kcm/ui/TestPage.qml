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
    padding: Kirigami.Units.largeSpacing

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
        spacing: Kirigami.Units.largeSpacing

        Kirigami.Heading {
            Layout.fillWidth: true
            level: 1
            text: i18n("Test")
            wrapMode: Text.Wrap
        }

        QQC2.Label {
            Layout.fillWidth: true
            text: i18n("Test one explicit current frame against the encrypted profile. A result affects only this page.")
            wrapMode: Text.Wrap
        }

        Kirigami.InlineMessage {
            objectName: "backendInitializationMessage"
            Layout.fillWidth: true
            visible: !root.backendReady
            type: Kirigami.MessageType.Warning
            text: i18n("LoofiFace-ID is still initializing. The local comparison controls will become available when the backend is ready.")
        }

        Components.CameraPreviewCard {
            Layout.fillWidth: true
            cameraPreviewSession: root.cameraPreviewSession
            showFramingGuide: false
            cardTitle: i18n("Private test preview")
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            type: Kirigami.MessageType.Warning
            text: i18n("Match is an experimental in-session comparison only. It cannot unlock, authenticate, authorize, call PAM, invoke Polkit, or change the Linux session.")
        }

        Kirigami.AbstractCard {
            Layout.fillWidth: true
            Accessible.role: Accessible.Grouping
            Accessible.name: i18n("One-frame test")

            contentItem: ColumnLayout {
                spacing: Kirigami.Units.smallSpacing

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
                }

                QQC2.BusyIndicator {
                    visible: root.localVerificationSession !== null && root.localVerificationSession.busy
                    running: visible
                    Accessible.ignored: true
                }

                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: root.localVerificationSession !== null && root.localVerificationSession.hasResult
                    type: root.localVerificationSession !== null && root.localVerificationSession.isMatch
                        ? Kirigami.MessageType.Positive
                        : (root.localVerificationSession !== null && root.localVerificationSession.isAmbiguous
                            ? Kirigami.MessageType.Warning
                            : Kirigami.MessageType.Information)
                    text: root.localVerificationSession !== null
                        ? root.localVerificationSession.statusText
                        : ""
                    Accessible.role: Accessible.Alert
                }

                Components.ActionableIssue {
                    issueTitle: root.localVerificationSession !== null && root.localVerificationSession.isUnavailable
                        ? i18n("Test unavailable")
                        : ""
                    recoveryText: root.localVerificationSession !== null && root.localVerificationSession.isUnavailable
                        ? root.localVerificationSession.statusText
                        : ""
                }

                QQC2.Label {
                    Layout.fillWidth: true
                    visible: root.localVerificationSession === null || !root.localVerificationSession.hasResult
                    text: root.localVerificationSession !== null
                        ? root.localVerificationSession.statusText
                        : i18n("Comparison service is initializing…")
                    color: Kirigami.Theme.disabledTextColor
                    wrapMode: Text.Wrap
                    Accessible.role: Accessible.StaticText
                }
            }
        }

        QQC2.Label {
            Layout.fillWidth: true
            text: i18n("Similarity scores, thresholds, liveness, and spoof-resistance claims are intentionally not shown or made.")
            color: Kirigami.Theme.disabledTextColor
            wrapMode: Text.Wrap
        }
    }
}
