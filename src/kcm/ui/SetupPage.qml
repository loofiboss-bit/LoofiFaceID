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
    required property var cameraPreviewSession
    required property var visionAnalysisSession
    required property var enrollmentSession
    property var openTest: () => {}

    title: i18n("Setup")
    padding: Kirigami.Units.largeSpacing

    onVisibleChanged: {
        enrollmentSession.setPageActive(visible)
        if (visible) {
            cameraPreviewSession.refreshDevices()
        } else {
            visionAnalysisSession.cancelAnalysis()
            cameraPreviewSession.stopPreview()
        }
    }

    QQC2.Dialog {
        id: deleteConfirmation
        objectName: "deleteProfileConfirmation"
        parent: QQC2.Overlay.overlay
        modal: true
        title: i18n("Delete face profile?")
        standardButtons: QQC2.Dialog.Ok | QQC2.Dialog.Cancel
        onAccepted: enrollmentSession.deleteProfile()
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
        onAccepted: enrollmentSession.resetUnreadable()
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
            issueTitle: systemState.issueCode.length > 0 ? i18n("Setup needs attention") : ""
            recoveryText: systemState.issueCode.length > 0 ? systemState.summary : ""
        }

        Components.CameraPreviewCard {
            Layout.fillWidth: true
            cameraPreviewSession: root.cameraPreviewSession
            analysisSession: root.visionAnalysisSession
            showAnalysis: true
            showFramingGuide: true
            cardTitle: i18n("Step 1 and 2: camera and frame check")
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
                    text: enrollmentSession.profileStatusText
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
                        enabled: !enrollmentSession.busy
                        activeFocusOnTab: true
                        Accessible.name: text
                        onClicked: enrollmentSession.refreshProfileStatus()
                    }

                    QQC2.Button {
                        objectName: "startEnrollmentButton"
                        text: i18n("Create face profile")
                        icon.name: "list-add-user"
                        enabled: enrollmentSession.canStartEnrollment
                        activeFocusOnTab: true
                        Accessible.name: text
                        onClicked: enrollmentSession.startEnrollment()
                    }
                }

                QQC2.Label {
                    Layout.fillWidth: true
                    visible: enrollmentSession.enrollmentActive
                    text: i18n("Capture exactly one sample per click. Three are required, five are recommended, and eight is the hard maximum.")
                    color: Kirigami.Theme.disabledTextColor
                    wrapMode: Text.Wrap
                }

                QQC2.ProgressBar {
                    Layout.fillWidth: true
                    visible: enrollmentSession.enrollmentActive || enrollmentSession.sampleCount > 0
                    from: 0
                    to: enrollmentSession.maximumSamples
                    value: enrollmentSession.sampleCount
                    Accessible.name: i18n("Enrollment sample progress")
                    Accessible.description: i18n("%1 of %2 maximum samples; five are recommended", enrollmentSession.sampleCount, enrollmentSession.maximumSamples)
                }

                QQC2.Label {
                    Layout.fillWidth: true
                    visible: enrollmentSession.enrollmentActive || enrollmentSession.sampleCount > 0
                    text: i18np("%1 sample accepted", "%1 samples accepted", enrollmentSession.sampleCount)
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
                        enabled: enrollmentSession.canCapture
                        activeFocusOnTab: true
                        Accessible.name: text
                        onClicked: enrollmentSession.captureSample()
                    }

                    QQC2.Button {
                        id: retryButton
                        objectName: "retrySampleButton"
                        text: i18n("Retry sample")
                        icon.name: "edit-undo"
                        enabled: enrollmentSession.sampleCount > 0 && !enrollmentSession.busy
                        activeFocusOnTab: true
                        Accessible.name: text
                        onClicked: enrollmentSession.discardLastSample()
                    }

                    QQC2.Button {
                        id: cancelButton
                        objectName: "cancelEnrollmentButton"
                        text: i18n("Cancel")
                        icon.name: "dialog-cancel"
                        enabled: enrollmentSession.canCancel
                        activeFocusOnTab: true
                        Accessible.name: text
                        onClicked: enrollmentSession.cancel()
                    }

                    QQC2.Button {
                        id: finishButton
                        objectName: "finishEnrollmentButton"
                        text: i18n("Finish and save")
                        icon.name: "document-save"
                        enabled: enrollmentSession.canFinish
                        activeFocusOnTab: true
                        Accessible.name: text
                        onClicked: enrollmentSession.finishAndSave()
                    }
                }

                QQC2.Label {
                    Layout.fillWidth: true
                    visible: enrollmentSession.enrollmentActive || enrollmentSession.enrollmentComplete
                    text: enrollmentSession.statusText
                    color: enrollmentSession.errorCode.length > 0
                        ? Kirigami.Theme.negativeTextColor
                        : Kirigami.Theme.textColor
                    wrapMode: Text.Wrap
                    Accessible.role: Accessible.StaticText
                }

                QQC2.Button {
                    objectName: "openTestAfterEnrollmentButton"
                    visible: enrollmentSession.enrollmentComplete
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
                        enabled: enrollmentSession.profileReady && !enrollmentSession.busy
                        activeFocusOnTab: true
                        Accessible.name: text
                        onClicked: deleteConfirmation.open()
                    }

                    QQC2.Button {
                        id: resetButton
                        objectName: "resetProfileButton"
                        text: i18n("Reset unreadable data")
                        icon.name: "edit-clear-all"
                        enabled: enrollmentSession.profileNeedsAttention && !enrollmentSession.busy
                        activeFocusOnTab: true
                        Accessible.name: text
                        onClicked: resetConfirmation.open()
                    }
                }
            }
        }
    }
}
