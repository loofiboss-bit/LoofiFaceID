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
    property var visionAnalysisSession: null
    property var enrollmentSession: null
    property bool backendReady: root.systemState !== null
        && root.cameraPreviewSession !== null
        && root.visionAnalysisSession !== null
        && root.enrollmentSession !== null
    property var openTest: () => {}

    title: i18n("Setup")
    padding: Kirigami.Units.largeSpacing

    onVisibleChanged: {
        if (!root.backendReady)
            return

        root.enrollmentSession.setPageActive(visible)
        if (visible) {
            root.cameraPreviewSession.refreshDevices()
        } else {
            root.visionAnalysisSession.continuousTracking = false
            root.visionAnalysisSession.cancelAnalysis()
            root.cameraPreviewSession.stopPreview()
        }
    }

    onBackendReadyChanged: {
        if (root.backendReady && root.visible)
            root.enrollmentSession.setPageActive(true)
    }

    QQC2.Dialog {
        id: deleteConfirmation
        objectName: "deleteProfileConfirmation"
        parent: QQC2.Overlay.overlay
        modal: true
        title: i18n("Delete face profile?")
        standardButtons: QQC2.Dialog.Ok | QQC2.Dialog.Cancel
        onAccepted: {
            if (root.enrollmentSession !== null)
                root.enrollmentSession.deleteProfile()
        }
        onClosed: deleteButton.forceActiveFocus()

        QQC2.Label {
            width: Math.min(Kirigami.Units.gridUnit * 28, root.width)
            text: i18n("This deletes the encrypted local profile. Storage hardware, snapshots, backups, SSDs, and copy-on-write filesystems may retain physical copies.")
            wrapMode: Text.Wrap
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }
    }

    QQC2.Dialog {
        id: resetConfirmation
        objectName: "resetProfileConfirmation"
        parent: QQC2.Overlay.overlay
        modal: true
        title: i18n("Reset unreadable profile data?")
        standardButtons: QQC2.Dialog.Ok | QQC2.Dialog.Cancel
        onAccepted: {
            if (root.enrollmentSession !== null)
                root.enrollmentSession.resetUnreadable()
        }
        onClosed: resetButton.forceActiveFocus()

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

        Kirigami.Heading {
            Layout.fillWidth: true
            level: 1
            text: i18n("Setup")
            wrapMode: Text.Wrap
        }

        QQC2.Label {
            Layout.fillWidth: true
            text: i18n("Follow one bounded path: check the camera, test one frame for framing guidance, then create an encrypted profile.")
            wrapMode: Text.Wrap
        }

        Components.ActionableIssue {
            issueTitle: root.systemState !== null && root.systemState.issueCode.length > 0
                ? i18n("Setup needs attention")
                : ""
            recoveryText: root.systemState !== null && root.systemState.issueCode.length > 0
                ? root.systemState.summary
                : ""
        }

        Kirigami.InlineMessage {
            objectName: "backendInitializationMessage"
            Layout.fillWidth: true
            visible: !root.backendReady
            type: Kirigami.MessageType.Warning
            text: i18n("LoofiFace-ID is still initializing. Camera and profile controls will become available when the local backend is ready.")
        }

        Components.CameraPreviewCard {
            Layout.fillWidth: true
            cameraPreviewSession: root.cameraPreviewSession
            analysisSession: root.visionAnalysisSession
            showAnalysis: true
            showFramingGuide: true
            cardTitle: i18n("Step 1 and 2: camera and frame check")
        }

        Components.EnrollmentWizard {
            Layout.fillWidth: true
            visible: root.enrollmentSession !== null && (root.enrollmentSession.enrollmentActive || root.enrollmentSession.sampleCount > 0)
            enrollmentSession: root.enrollmentSession
            visionAnalysisSession: root.visionAnalysisSession
            cameraPreviewSession: root.cameraPreviewSession
        }

        Kirigami.AbstractCard {
            Layout.fillWidth: true
            Accessible.role: Accessible.Grouping
            Accessible.name: i18n("Step 3: encrypted face profile")

            contentItem: ColumnLayout {
                spacing: Kirigami.Units.smallSpacing

                QQC2.Label {
                    Layout.fillWidth: true
                    text: i18n("Step 3: Create a face profile")
                    font.weight: Font.DemiBold
                    wrapMode: Text.Wrap
                }

                QQC2.Label {
                    Layout.fillWidth: true
                    text: root.enrollmentSession !== null
                        ? root.enrollmentSession.profileStatusText
                        : i18n("Profile service is initializing…")
                    wrapMode: Text.Wrap
                    Accessible.role: Accessible.StaticText
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing

                    QQC2.Button {
                        objectName: "refreshStatusButton"
                        text: i18n("Refresh profile status")
                        icon.name: "view-refresh"
                        enabled: root.enrollmentSession !== null && !root.enrollmentSession.busy
                        activeFocusOnTab: true
                        Accessible.name: text
                        onClicked: {
                            if (root.enrollmentSession !== null)
                                root.enrollmentSession.refreshProfileStatus()
                        }
                    }

                    QQC2.Button {
                        objectName: "startEnrollmentButton"
                        text: i18n("Create face profile")
                        icon.name: "list-add-user"
                        enabled: root.enrollmentSession !== null && root.enrollmentSession.canStartEnrollment
                        activeFocusOnTab: true
                        Accessible.name: text
                        onClicked: {
                            if (root.enrollmentSession !== null) {
                                root.enrollmentSession.startEnrollment()
                                if (root.visionAnalysisSession !== null)
                                    root.visionAnalysisSession.continuousTracking = true
                            }
                        }
                    }
                }

                QQC2.Label {
                    Layout.fillWidth: true
                    visible: root.enrollmentSession !== null && root.enrollmentSession.enrollmentActive
                    text: i18n("Capture exactly one sample per click. Three are required, five are recommended, and eight is the hard maximum.")
                    color: Kirigami.Theme.disabledTextColor
                    wrapMode: Text.Wrap
                }

                QQC2.ProgressBar {
                    Layout.fillWidth: true
                    visible: root.enrollmentSession !== null
                        && (root.enrollmentSession.enrollmentActive || root.enrollmentSession.sampleCount > 0)
                    from: 0
                    to: root.enrollmentSession !== null ? root.enrollmentSession.maximumSamples : 0
                    value: root.enrollmentSession !== null ? root.enrollmentSession.sampleCount : 0
                    Accessible.name: i18n("Enrollment sample progress")
                    Accessible.description: root.enrollmentSession !== null
                        ? i18n("%1 of %2 maximum samples; five are recommended", root.enrollmentSession.sampleCount, root.enrollmentSession.maximumSamples)
                        : i18n("Enrollment is initializing")
                }

                QQC2.Label {
                    Layout.fillWidth: true
                    visible: root.enrollmentSession !== null
                        && (root.enrollmentSession.enrollmentActive || root.enrollmentSession.sampleCount > 0)
                    text: root.enrollmentSession !== null
                        ? i18np("%1 sample accepted", "%1 samples accepted", root.enrollmentSession.sampleCount)
                        : ""
                    wrapMode: Text.Wrap
                    Accessible.role: Accessible.StaticText
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing

                    QQC2.Button {
                        id: captureButton
                        objectName: "captureButton"
                        text: i18n("Capture sample")
                        icon.name: "camera-photo"
                        enabled: root.enrollmentSession !== null && root.enrollmentSession.canCapture
                        activeFocusOnTab: true
                        Accessible.name: text
                        onClicked: {
                            if (root.enrollmentSession !== null)
                                root.enrollmentSession.captureSample()
                        }
                    }

                    QQC2.Button {
                        id: retryButton
                        objectName: "retrySampleButton"
                        text: i18n("Retry sample")
                        icon.name: "edit-undo"
                        enabled: root.enrollmentSession !== null
                            && root.enrollmentSession.sampleCount > 0
                            && !root.enrollmentSession.busy
                        activeFocusOnTab: true
                        Accessible.name: text
                        onClicked: {
                            if (root.enrollmentSession !== null)
                                root.enrollmentSession.discardLastSample()
                        }
                    }

                    QQC2.Button {
                        id: cancelButton
                        objectName: "cancelEnrollmentButton"
                        text: i18n("Cancel")
                        icon.name: "dialog-cancel"
                        enabled: root.enrollmentSession !== null && root.enrollmentSession.canCancel
                        activeFocusOnTab: true
                        Accessible.name: text
                        onClicked: {
                            if (root.enrollmentSession !== null)
                                root.enrollmentSession.cancel()
                            if (root.visionAnalysisSession !== null)
                                root.visionAnalysisSession.continuousTracking = false
                        }
                    }

                    QQC2.Button {
                        id: finishButton
                        objectName: "finishEnrollmentButton"
                        text: i18n("Finish and save")
                        icon.name: "document-save"
                        enabled: root.enrollmentSession !== null && root.enrollmentSession.canFinish
                        activeFocusOnTab: true
                        Accessible.name: text
                        onClicked: {
                            if (root.enrollmentSession !== null)
                                root.enrollmentSession.finishAndSave()
                            if (root.visionAnalysisSession !== null)
                                root.visionAnalysisSession.continuousTracking = false
                        }
                    }
                }

                QQC2.Label {
                    Layout.fillWidth: true
                    visible: root.enrollmentSession !== null
                        && (root.enrollmentSession.enrollmentActive || root.enrollmentSession.enrollmentComplete)
                    text: root.enrollmentSession !== null ? root.enrollmentSession.statusText : ""
                    color: root.enrollmentSession !== null && root.enrollmentSession.errorCode.length > 0
                        ? Kirigami.Theme.negativeTextColor
                        : Kirigami.Theme.textColor
                    wrapMode: Text.Wrap
                    Accessible.role: Accessible.StaticText
                }

                QQC2.Button {
                    objectName: "openTestAfterEnrollmentButton"
                    visible: root.enrollmentSession !== null && root.enrollmentSession.enrollmentComplete
                    text: i18n("Open Test")
                    icon.name: "view-preview"
                    activeFocusOnTab: true
                    Accessible.name: text
                    onClicked: root.openTest()
                }
            }
        }

        Kirigami.AbstractCard {
            Layout.fillWidth: true
            Accessible.role: Accessible.Grouping
            Accessible.name: i18n("Profile management")

            contentItem: ColumnLayout {
                spacing: Kirigami.Units.smallSpacing

                QQC2.Label {
                    Layout.fillWidth: true
                    text: i18n("Profile management")
                    font.weight: Font.DemiBold
                }

                QQC2.Label {
                    Layout.fillWidth: true
                    text: i18n("Deletion and unreadable-vault reset are separate destructive actions. Neither promises physical erasure from storage, snapshots, or backups.")
                    color: Kirigami.Theme.disabledTextColor
                    wrapMode: Text.Wrap
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing

                    QQC2.Button {
                        id: deleteButton
                        objectName: "deleteProfileButton"
                        text: i18n("Delete face profile")
                        icon.name: "edit-delete"
                        enabled: root.enrollmentSession !== null
                            && root.enrollmentSession.profileReady
                            && !root.enrollmentSession.busy
                        activeFocusOnTab: true
                        Accessible.name: text
                        onClicked: deleteConfirmation.open()
                    }

                    QQC2.Button {
                        id: resetButton
                        objectName: "resetProfileButton"
                        text: i18n("Reset unreadable data")
                        icon.name: "edit-clear-all"
                        enabled: root.enrollmentSession !== null
                            && root.enrollmentSession.profileNeedsAttention
                            && !root.enrollmentSession.busy
                        activeFocusOnTab: true
                        Accessible.name: text
                        onClicked: resetConfirmation.open()
                    }
                }
            }
        }
    }
}
