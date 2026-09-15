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
    property bool firstStartPending: false
    property bool autoCaptureEnabled: true
    property int stableObservations: 0
    property double stableSinceMs: 0
    property string lastGuidanceGeneration: ""

    readonly property int currentStep: enrollmentSession ? enrollmentSession.sampleCount : 0
    readonly property bool isEnrolling: enrollmentSession !== null && enrollmentSession.enrollmentActive
    readonly property bool guidanceReady: root.visionAnalysisSession !== null
        && root.visionAnalysisSession.resultAvailable
        && root.visionAnalysisSession.framingSuitable
        && root.visionAnalysisSession.poseMatches(root.currentStep)

    readonly property string currentGuidanceText: {
        if (!root.isEnrolling)
            return i18n("Start guided registration when the camera preview is ready.")
        if (root.currentStep >= 5)
            return i18n("Five samples are ready. Review them and choose Save profile.")
        if (root.visionAnalysisSession === null || !root.visionAnalysisSession.resultAvailable)
            return i18n("Place one face in front of the camera.")
        if (!root.guidanceReady)
            return root.visionAnalysisSession.guidanceText
        if (root.stableObservations > 0)
            return i18n("Hold still — %1 of 3 fresh observations", root.stableObservations)
        return root.stepInstruction
    }

    function resetStability() {
        root.stableObservations = 0
        root.stableSinceMs = 0
    }

    function startGuidedEnrollment() {
        if (!root.backendReady || !root.cameraPreviewSession.previewActive)
            return
        if (root.enrollmentSession !== null && root.enrollmentSession.canStartEnrollment) {
            root.firstStartPending = false
            root.resetStability()
            autoCaptureCooldown.stop()
            root.enrollmentSession.startEnrollment()
            if (root.visionAnalysisSession !== null)
                root.visionAnalysisSession.startGuidance()
        }
    }

    function beginFirstStart() {
        if (!root.backendReady)
            return
        root.firstStartPending = true
        if (root.cameraPreviewSession.previewActive) {
            root.startGuidedEnrollment()
        } else if (root.cameraPreviewSession.canStartPreview) {
            root.cameraPreviewSession.startPreview()
        } else {
            root.cameraPreviewSession.refreshDevices()
        }
    }

    readonly property string stepInstruction: {
        switch (root.currentStep) {
        case 0:
            return i18n("Look straight into the camera.")
        case 1:
            return i18n("Turn your head slightly to the left.")
        case 2:
            return i18n("Turn your head slightly to the right.")
        case 3:
            return i18n("Tilt your head slightly up or down.")
        case 4:
            return i18n("Look straight with a relaxed, natural expression.")
        default:
            return i18n("Review your samples and choose Save profile.")
        }
    }

    Timer {
        id: autoCaptureCooldown
        interval: 800
        repeat: false
    }

    title: i18n("Setup")
    padding: Kirigami.Units.mediumSpacing

    onVisibleChanged: {
        if (!root.backendReady)
            return

        root.enrollmentSession.setPageActive(visible)
        if (visible) {
            root.cameraPreviewSession.refreshDevices()
        } else {
            root.firstStartPending = false
            root.resetStability()
            autoCaptureCooldown.stop()
            root.visionAnalysisSession.stopGuidance()
            root.cameraPreviewSession.stopPreview()
        }
    }

    onBackendReadyChanged: {
        if (root.backendReady && root.visible)
            root.enrollmentSession.setPageActive(true)
    }

    Connections {
        target: root.cameraPreviewSession
        function onStateChanged() {
            if (!root.firstStartPending || !root.cameraPreviewSession)
                return
            if (root.cameraPreviewSession.canStartPreview)
                root.cameraPreviewSession.startPreview()
            else if (root.cameraPreviewSession.previewActive)
                root.startGuidedEnrollment()
        }
    }

    Connections {
        target: root.visionAnalysisSession
        function onResultChanged() {
            if (!root.autoCaptureEnabled || !root.isEnrolling || !root.visionAnalysisSession
                || !root.visionAnalysisSession.resultAvailable) {
                root.resetStability()
                return
            }
            if (autoCaptureCooldown.running || root.enrollmentSession === null
                || !root.enrollmentSession.canCapture) {
                root.resetStability()
                return
            }
            const generation = String(root.visionAnalysisSession.generation)
            if (generation === root.lastGuidanceGeneration)
                return
            root.lastGuidanceGeneration = generation
            if (!root.guidanceReady) {
                root.resetStability()
                return
            }
            if (root.stableObservations === 0)
                root.stableSinceMs = Date.now()
            root.stableObservations += 1
            const stableForMs = Date.now() - root.stableSinceMs
            if (root.stableObservations >= 3 && stableForMs >= 600) {
                root.enrollmentSession.captureSample(true)
                root.resetStability()
            }
        }
    }

    Connections {
        target: root.enrollmentSession
        function onSamplesChanged() {
            root.resetStability()
        }
        function onStateChanged() {
            if (root.enrollmentSession !== null && !root.enrollmentSession.enrollmentActive
                && root.visionAnalysisSession !== null && root.visionAnalysisSession.continuousTracking)
                root.visionAnalysisSession.stopGuidance()
            if (root.firstStartPending && root.cameraPreviewSession !== null
                && root.cameraPreviewSession.previewActive && root.enrollmentSession.canStartEnrollment)
                root.startGuidedEnrollment()
        }
        function onSampleCaptured(sampleIndex, automatic) {
            root.resetStability()
            autoCaptureCooldown.restart()
        }
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
        spacing: Kirigami.Units.mediumSpacing

        Kirigami.InlineMessage {
            objectName: "backendInitializationMessage"
            Layout.fillWidth: true
            visible: !root.backendReady
            type: Kirigami.MessageType.Warning
            text: i18n("LoofiFace-ID is still initializing. Camera and profile controls will become available when the local backend is ready.")
        }

        Components.ActionableIssue {
            issueTitle: root.systemState !== null && root.systemState.issueCode.length > 0
                ? i18n("Setup needs attention")
                : ""
            recoveryText: root.systemState !== null && root.systemState.issueCode.length > 0
                ? root.systemState.summary
                : ""
        }

        Kirigami.AbstractCard {
            Layout.fillWidth: true
            visible: !root.isEnrolling && (root.enrollmentSession === null
                || root.enrollmentSession.sampleCount === 0)
                && root.cameraPreviewSession !== null && !root.cameraPreviewSession.previewActive
            Accessible.role: Accessible.Grouping
            Accessible.name: i18n("Get started with the camera")

            contentItem: RowLayout {
                spacing: Kirigami.Units.mediumSpacing

                Kirigami.Icon {
                    source: "camera-photo"
                    implicitWidth: Kirigami.Units.iconSizes.medium
                    implicitHeight: Kirigami.Units.iconSizes.medium
                    color: Kirigami.Theme.highlightColor
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing

                    Kirigami.Heading {
                        level: 2
                        Layout.fillWidth: true
                        text: i18n("Ready for a guided start?")
                        wrapMode: Text.Wrap
                    }

                    QQC2.Label {
                        Layout.fillWidth: true
                        text: i18n("Choose the available camera, start a private preview, and follow five short pose prompts. Nothing is saved until you choose Save profile.")
                        wrapMode: Text.Wrap
                        color: Kirigami.Theme.disabledTextColor
                    }
                }

                QQC2.Button {
                    objectName: "startGuidedSetupButton"
                    text: i18n("Get started")
                    icon.name: "go-next"
                    enabled: root.backendReady && (root.cameraPreviewSession.canStartPreview
                        || root.cameraPreviewSession.previewActive)
                    activeFocusOnTab: true
                    Accessible.name: text
                    onClicked: root.beginFirstStart()
                }
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: root.availableWidth >= 700 ? 2 : 1
            columnSpacing: Kirigami.Units.largeSpacing
            rowSpacing: Kirigami.Units.largeSpacing

            // LEFT COLUMN: Live Camera View
            Components.CameraPreviewCard {
                id: cameraCard
                Layout.fillWidth: true
                Layout.preferredWidth: root.availableWidth >= 700 ? Math.round(root.availableWidth * 0.48) : root.availableWidth
                Layout.alignment: Qt.AlignTop
                cameraPreviewSession: root.cameraPreviewSession
                analysisSession: root.visionAnalysisSession
                showAnalysis: false
                showFramingGuide: true
                cardTitle: i18n("Camera and guided registration")
            }

            // RIGHT COLUMN: Simplified, Logical Enrollment Cockpit
            Kirigami.AbstractCard {
                id: cockpitCard
                Layout.fillWidth: true
                Layout.preferredWidth: root.availableWidth >= 700 ? Math.round(root.availableWidth * 0.52) : root.availableWidth
                Layout.alignment: Qt.AlignTop
                Accessible.role: Accessible.Grouping
                Accessible.name: i18n("Step 3: encrypted face profile")

                contentItem: ColumnLayout {
                    spacing: Kirigami.Units.mediumSpacing

                    // Header & Status
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        Kirigami.Icon {
                            source: root.enrollmentSession && root.enrollmentSession.profileReady ? "security-high" : "user-identity"
                            implicitWidth: Kirigami.Units.iconSizes.medium
                            implicitHeight: Kirigami.Units.iconSizes.medium
                            color: root.enrollmentSession && root.enrollmentSession.profileReady
                                ? Kirigami.Theme.positiveTextColor
                                : Kirigami.Theme.highlightColor
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1

                            Kirigami.Heading {
                                level: 2
                                Layout.fillWidth: true
                                text: i18n("Guided face registration")
                            }

                            QQC2.Label {
                                Layout.fillWidth: true
                                text: root.enrollmentSession !== null
                                    ? root.enrollmentSession.profileStatusText
                                    : i18n("Profile service is initializing…")
                                color: Kirigami.Theme.disabledTextColor
                                font.pointSize: Kirigami.Theme.smallFont.pointSize
                                wrapMode: Text.Wrap
                                Accessible.role: Accessible.StaticText
                            }
                        }

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
                    }

                    Kirigami.Separator { Layout.fillWidth: true }

                    // When enrolling: 5 step indicators + clear prompt
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: root.isEnrolling || (root.enrollmentSession !== null && root.enrollmentSession.sampleCount > 0)
                        spacing: Kirigami.Units.smallSpacing

                        // 5 Steps Visual Indicators
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Repeater {
                                model: [
                                    { name: i18n("Frontal"), icon: "user" },
                                    { name: i18n("Left"), icon: "go-previous" },
                                    { name: i18n("Right"), icon: "go-next" },
                                    { name: i18n("Tilt"), icon: "go-up" },
                                    { name: i18n("Natural"), icon: "user-identity" }
                                ]

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: Kirigami.Units.gridUnit * 2.2
                                    radius: Kirigami.Units.cornerRadius
                                    color: index < root.currentStep
                                        ? Qt.alpha(Kirigami.Theme.positiveTextColor, 0.18)
                                        : (index === root.currentStep && root.isEnrolling
                                            ? Qt.alpha(Kirigami.Theme.highlightColor, 0.22)
                                            : Qt.alpha(Kirigami.Theme.backgroundColor, 0.5))
                                    border.color: index < root.currentStep
                                        ? Kirigami.Theme.positiveTextColor
                                        : (index === root.currentStep && root.isEnrolling
                                            ? Kirigami.Theme.highlightColor
                                            : Qt.alpha(Kirigami.Theme.textColor, 0.2))
                                    border.width: index === root.currentStep && root.isEnrolling ? 2 : 1

                                    RowLayout {
                                        anchors.centerIn: parent
                                        spacing: 4

                                        Kirigami.Icon {
                                            source: index < root.currentStep ? "dialog-ok" : modelData.icon
                                            implicitWidth: Kirigami.Units.iconSizes.small
                                            implicitHeight: Kirigami.Units.iconSizes.small
                                            color: index < root.currentStep
                                                ? Kirigami.Theme.positiveTextColor
                                                : (index === root.currentStep && root.isEnrolling
                                                    ? Kirigami.Theme.highlightColor
                                                    : Kirigami.Theme.disabledTextColor)
                                        }

                                        QQC2.Label {
                                            text: modelData.name
                                            font.pointSize: Kirigami.Theme.smallFont.pointSize * 0.9
                                            font.weight: index === root.currentStep && root.isEnrolling ? Font.Bold : Font.Normal
                                            color: index < root.currentStep
                                                ? Kirigami.Theme.positiveTextColor
                                                : (index === root.currentStep && root.isEnrolling
                                                    ? Kirigami.Theme.highlightColor
                                                    : Kirigami.Theme.disabledTextColor)
                                        }
                                    }
                                }
                            }
                        }

                        // Prominent Current Instruction Banner
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: Kirigami.Units.gridUnit * 2.5
                            radius: Kirigami.Units.cornerRadius
                            color: Qt.alpha(Kirigami.Theme.highlightColor, 0.12)
                            border.color: Kirigami.Theme.highlightColor
                            border.width: 1

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: Kirigami.Units.smallSpacing
                                spacing: Kirigami.Units.smallSpacing

                                Kirigami.Icon {
                                    source: root.guidanceReady ? "emblem-checked" : "dialog-information"
                                    implicitWidth: Kirigami.Units.iconSizes.small
                                    implicitHeight: Kirigami.Units.iconSizes.small
                                    color: root.guidanceReady ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.highlightColor
                                }

                                QQC2.Label {
                                    Layout.fillWidth: true
                                    text: root.currentGuidanceText
                                    font.weight: Font.DemiBold
                                    font.pointSize: Kirigami.Theme.defaultFont.pointSize
                                    color: root.guidanceReady ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.textColor
                                    wrapMode: Text.Wrap
                                    Accessible.role: Accessible.StaticText
                                    Accessible.name: text
                                }

                                QQC2.Label {
                                    visible: root.isEnrolling
                                    text: root.enrollmentSession ? i18n("%1 s remaining", root.enrollmentSession.remainingSeconds) : ""
                                    color: Kirigami.Theme.disabledTextColor
                                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                                }
                            }
                        }

                        // Progress bar
                        QQC2.ProgressBar {
                            Layout.fillWidth: true
                            from: 0
                            to: root.enrollmentSession !== null ? Math.max(5, root.enrollmentSession.maximumSamples) : 5
                            value: root.enrollmentSession !== null ? root.enrollmentSession.sampleCount : 0
                            Accessible.name: i18n("Enrollment sample progress")
                            Accessible.description: root.enrollmentSession !== null
                                ? i18n("%1 of %2 maximum samples; five are recommended", root.enrollmentSession.sampleCount, root.enrollmentSession.maximumSamples)
                                : i18n("Enrollment is initializing")
                        }
                    }

                    // Static helper when not enrolling
                    QQC2.Label {
                        Layout.fillWidth: true
                        visible: !root.isEnrolling && (root.enrollmentSession === null || root.enrollmentSession.sampleCount === 0)
                        text: i18n("Start the guided registration to capture one pose at a time. Three samples are required, five are recommended, and eight is the hard maximum.")
                        color: Kirigami.Theme.disabledTextColor
                        wrapMode: Text.Wrap
                    }

                    QQC2.CheckBox {
                        id: autoCaptureCheck
                        objectName: "autoCaptureCheck"
                        Layout.fillWidth: true
                        visible: root.isEnrolling
                        text: i18n("Automatically capture after three stable observations (recommended)")
                        checked: root.autoCaptureEnabled
                        activeFocusOnTab: true
                        Accessible.name: text
                        onToggled: {
                            root.autoCaptureEnabled = checked
                            if (!checked)
                                root.resetStability()
                        }
                    }

                    // Main Action Buttons
                    Flow {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        QQC2.Button {
                            id: startButton
                            objectName: "startEnrollmentButton"
                            text: i18n("Start guided registration")
                            icon.name: "list-add-user"
                            enabled: root.enrollmentSession !== null && root.enrollmentSession.canStartEnrollment
                            visible: !root.isEnrolling && root.cameraPreviewSession !== null
                                && root.cameraPreviewSession.previewActive
                            activeFocusOnTab: true
                            Accessible.name: text
                            onClicked: {
                                root.startGuidedEnrollment()
                            }
                        }

                        QQC2.Button {
                            id: captureButton
                            objectName: "captureButton"
                            text: i18n("Capture manually")
                            icon.name: "camera-photo"
                            enabled: root.enrollmentSession !== null && root.enrollmentSession.canCapture
                            visible: root.isEnrolling
                            activeFocusOnTab: true
                            Accessible.name: text
                            onClicked: {
                                if (root.enrollmentSession !== null) {
                                    root.resetStability()
                                    root.enrollmentSession.captureSample(false)
                                }
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
                            visible: root.isEnrolling || (root.enrollmentSession !== null && root.enrollmentSession.sampleCount > 0 && !root.enrollmentSession.enrollmentComplete)
                            activeFocusOnTab: true
                            Accessible.name: text
                            onClicked: {
                                if (root.enrollmentSession !== null)
                                    root.enrollmentSession.discardLastSample()
                            }
                        }

                        QQC2.Button {
                            id: finishButton
                            objectName: "finishEnrollmentButton"
                            text: i18n("Save profile")
                            icon.name: "document-save"
                            enabled: root.enrollmentSession !== null && root.enrollmentSession.canFinish
                            visible: root.enrollmentSession !== null && root.enrollmentSession.canFinish
                                && root.enrollmentSession.sampleCount >= root.enrollmentSession.recommendedSamples
                            activeFocusOnTab: true
                            Accessible.name: text
                            onClicked: {
                                if (root.enrollmentSession !== null)
                                    root.enrollmentSession.finishAndSave()
                                if (root.visionAnalysisSession !== null)
                                    root.visionAnalysisSession.stopGuidance()
                            }
                        }

                        QQC2.Button {
                            id: saveNowButton
                            objectName: "saveNowButton"
                            text: i18n("Save now (three samples)")
                            icon.name: "document-save"
                            enabled: root.enrollmentSession !== null && root.enrollmentSession.canFinish
                            visible: root.enrollmentSession !== null && root.enrollmentSession.canFinish
                                && root.enrollmentSession.sampleCount >= root.enrollmentSession.minimumSamples
                                && root.enrollmentSession.sampleCount < root.enrollmentSession.recommendedSamples
                            activeFocusOnTab: true
                            Accessible.name: text
                            Accessible.description: i18n("Five samples are recommended for a more varied local profile.")
                            onClicked: {
                                if (root.enrollmentSession !== null)
                                    root.enrollmentSession.finishAndSave()
                                if (root.visionAnalysisSession !== null)
                                    root.visionAnalysisSession.stopGuidance()
                            }
                        }

                        QQC2.Button {
                            id: cancelButton
                            objectName: "cancelEnrollmentButton"
                            text: i18n("Cancel")
                            icon.name: "dialog-cancel"
                            enabled: root.enrollmentSession !== null && root.enrollmentSession.canCancel
                            visible: root.isEnrolling
                            activeFocusOnTab: true
                            Accessible.name: text
                            onClicked: {
                                if (root.enrollmentSession !== null)
                                    root.enrollmentSession.cancel()
                                if (root.visionAnalysisSession !== null)
                                    root.visionAnalysisSession.stopGuidance()
                                root.resetStability()
                            }
                        }

                        QQC2.Button {
                            id: openTestBtn
                            objectName: "openTestAfterEnrollmentButton"
                            visible: root.enrollmentSession !== null && root.enrollmentSession.enrollmentComplete
                            text: i18n("Open Test")
                            icon.name: "view-preview"
                            activeFocusOnTab: true
                            Accessible.name: text
                            onClicked: root.openTest()
                        }
                    }

                    // Status / Error message
                    QQC2.Label {
                        Layout.fillWidth: true
                        visible: root.enrollmentSession !== null
                            && (root.enrollmentSession.statusText.length > 0 || root.enrollmentSession.errorCode.length > 0)
                        text: root.enrollmentSession !== null ? root.enrollmentSession.statusText : ""
                        color: root.enrollmentSession !== null && root.enrollmentSession.errorCode.length > 0
                            ? Kirigami.Theme.negativeTextColor
                            : Kirigami.Theme.textColor
                        font.pointSize: Kirigami.Theme.smallFont.pointSize
                        wrapMode: Text.Wrap
                        Accessible.role: Accessible.Alert
                        Accessible.name: text
                    }

                    QQC2.Label {
                        Layout.fillWidth: true
                        visible: root.isEnrolling && root.visionAnalysisSession !== null
                            && root.visionAnalysisSession.errorCode.length > 0
                        text: root.visionAnalysisSession !== null ? root.visionAnalysisSession.statusText : ""
                        color: Kirigami.Theme.negativeTextColor
                        wrapMode: Text.Wrap
                        Accessible.role: Accessible.Alert
                        Accessible.name: text
                    }

                    // Profile Management (Delete / Reset)
                    Kirigami.Separator {
                        Layout.fillWidth: true
                        visible: !root.isEnrolling && root.enrollmentSession !== null
                            && (root.enrollmentSession.profileReady || root.enrollmentSession.profileNeedsAttention)
                    }

                    Flow {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing
                        visible: !root.isEnrolling

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
}
