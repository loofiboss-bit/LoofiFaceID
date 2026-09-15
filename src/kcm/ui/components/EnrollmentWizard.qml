// SPDX-License-Identifier: GPL-3.0-or-later
// qmllint disable unqualified

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.AbstractCard {
    id: root

    property var enrollmentSession: null
    property var visionAnalysisSession: null
    property var cameraPreviewSession: null
    property bool autoCapture: true

    readonly property int currentStep: enrollmentSession ? enrollmentSession.sampleCount : 0
    readonly property bool isEnrolling: enrollmentSession !== null && enrollmentSession.enrollmentActive
    readonly property bool hasTracking: visionAnalysisSession !== null && visionAnalysisSession.faceDetected

    // Pose detection metrics from landmarks
    readonly property var landmarks: hasTracking ? visionAnalysisSession.landmarks : []
    readonly property bool validLandmarks: landmarks && landmarks.length === 5

    // Yaw metric: negative = turned one way, positive = turned the other way
    readonly property real yawMetric: {
        if (!validLandmarks) return 0.0
        let rEye = landmarks[0]
        let lEye = landmarks[1]
        let nose = landmarks[2]
        let eyeDist = Math.abs(lEye.x - rEye.x)
        if (eyeDist < 1.0) return 0.0
        let eyeMidX = (rEye.x + lEye.x) / 2.0
        return (nose.x - eyeMidX) / eyeDist
    }

    // Pitch metric: vertical offset of nose between eyes and mouth
    readonly property real pitchMetric: {
        if (!validLandmarks) return 0.0
        let rEye = landmarks[0]
        let lEye = landmarks[1]
        let nose = landmarks[2]
        let rMouth = landmarks[3]
        let lMouth = landmarks[4]
        let eyeMidY = (rEye.y + lEye.y) / 2.0
        let mouthMidY = (rMouth.y + lMouth.y) / 2.0
        let eyeToMouth = mouthMidY - eyeMidY
        if (eyeToMouth < 1.0) return 0.0
        let relY = (nose.y - eyeMidY) / eyeToMouth
        return relY - 0.50 // 0 = centered, positive = down, negative = up
    }

    readonly property bool currentPoseMatched: {
        if (!hasTracking || !isEnrolling) return false
        switch (root.currentStep) {
        case 0: // Frontal
            return Math.abs(yawMetric) < 0.09 && Math.abs(pitchMetric) < 0.12
        case 1: // Yaw Left
            return yawMetric < -0.08
        case 2: // Yaw Right
            return yawMetric > 0.08
        case 3: // Pitch Up/Down
            return Math.abs(pitchMetric) > 0.10
        case 4: // Expression / Neutral
            return Math.abs(yawMetric) < 0.12
        default:
            return false
        }
    }

    readonly property string coachingMessage: {
        if (!root.isEnrolling) {
            return i18n("Start guided enrollment to create your biometric profile.")
        }
        if (!root.hasTracking) {
            return i18n("Position your face in front of the camera.")
        }
        if (visionAnalysisSession.multipleFaces) {
            return i18n("Ensure only one face is visible.")
        }
        if (visionAnalysisSession.distance === 1) { // TooClose
            return i18n("Move slightly further away from the camera.")
        }
        if (visionAnalysisSession.distance === 2) { // TooFar
            return i18n("Move slightly closer to the camera.")
        }
        if (visionAnalysisSession.position === 1) { // OffCenter
            return i18n("Center your face in the preview.")
        }
        if (visionAnalysisSession.brightness === 1) { // Low
            return i18n("Lighting is too low. Move to a brighter area.")
        }
        if (visionAnalysisSession.brightness === 2) { // High
            return i18n("Lighting is too bright or glare is present.")
        }

        switch (root.currentStep) {
        case 0:
            return root.currentPoseMatched
                ? i18n("Hold steady… Frontal pose detected")
                : i18n("Look straight into the camera.")
        case 1:
            return root.currentPoseMatched
                ? i18n("Hold steady… Left pose detected")
                : i18n("Turn your head slightly to the left.")
        case 2:
            return root.currentPoseMatched
                ? i18n("Hold steady… Right pose detected")
                : i18n("Turn your head slightly to the right.")
        case 3:
            return root.currentPoseMatched
                ? i18n("Hold steady… Tilt pose detected")
                : i18n("Tilt your head slightly up or down.")
        case 4:
            return root.currentPoseMatched
                ? i18n("Hold steady… Neutral expression detected")
                : i18n("Look straight with a relaxed, natural expression.")
        default:
            return i18n("All poses captured. Finishing enrollment…")
        }
    }

    // Auto-capture timer: triggers when current pose is held steady
    Timer {
        id: autoCaptureTimer
        interval: 650
        running: root.autoCapture && root.isEnrolling && root.currentPoseMatched && root.enrollmentSession.canCapture
        repeat: false
        onTriggered: {
            if (root.enrollmentSession && root.enrollmentSession.canCapture) {
                root.enrollmentSession.captureSample()
            }
        }
    }

    // Auto-finish when 5 poses are collected
    Timer {
        id: autoFinishTimer
        interval: 500
        running: root.autoCapture && root.isEnrolling && root.currentStep >= 5 && root.enrollmentSession.canFinish
        repeat: false
        onTriggered: {
            if (root.enrollmentSession && root.enrollmentSession.canFinish) {
                root.enrollmentSession.finishAndSave()
            }
        }
    }

    Accessible.role: Accessible.Grouping
    Accessible.name: i18n("Automated guided enrollment wizard")

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.mediumSpacing

        // Title and Step Progress
        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            Kirigami.Heading {
                level: 2
                Layout.fillWidth: true
                text: i18n("Guided Biometric Enrollment")
            }

            QQC2.Label {
                visible: root.isEnrolling
                text: root.enrollmentSession ? i18n("%1 s remaining", root.enrollmentSession.remainingSeconds) : ""
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.smallFont.pointSize
            }
        }

        // 5 Poses Visual Indicator
        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

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
                    Layout.preferredHeight: Kirigami.Units.gridUnit * 2.8
                    radius: Kirigami.Units.cornerRadius
                    color: index < root.currentStep
                        ? Qt.alpha(Kirigami.Theme.positiveTextColor, 0.15)
                        : (index === root.currentStep && root.isEnrolling
                            ? Qt.alpha(Kirigami.Theme.highlightColor, 0.2)
                            : Qt.alpha(Kirigami.Theme.backgroundColor, 0.5))
                    border.color: index < root.currentStep
                        ? Kirigami.Theme.positiveTextColor
                        : (index === root.currentStep && root.isEnrolling
                            ? Kirigami.Theme.highlightColor
                            : Qt.alpha(Kirigami.Theme.textColor, 0.2))
                    border.width: index === root.currentStep && root.isEnrolling ? 2 : 1

                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: 2

                        Kirigami.Icon {
                            Layout.alignment: Qt.AlignHCenter
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
                            Layout.alignment: Qt.AlignHCenter
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

        // Live Real-Time Coaching Banner
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Kirigami.Units.gridUnit * 2.2
            radius: Kirigami.Units.cornerRadius
            color: root.currentPoseMatched
                ? Qt.alpha(Kirigami.Theme.positiveTextColor, 0.15)
                : Qt.alpha(Kirigami.Theme.highlightColor, 0.1)
            border.color: root.currentPoseMatched
                ? Kirigami.Theme.positiveTextColor
                : Kirigami.Theme.highlightColor
            border.width: 1

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Kirigami.Units.mediumSpacing
                anchors.rightMargin: Kirigami.Units.mediumSpacing
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Icon {
                    source: root.currentPoseMatched ? "emblem-checked" : "dialog-information"
                    implicitWidth: Kirigami.Units.iconSizes.small
                    implicitHeight: Kirigami.Units.iconSizes.small
                    color: root.currentPoseMatched ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.highlightColor
                }

                QQC2.Label {
                    Layout.fillWidth: true
                    text: root.coachingMessage
                    font.weight: Font.DemiBold
                    color: root.currentPoseMatched ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.textColor
                    elide: Text.ElideRight
                }

                QQC2.BusyIndicator {
                    visible: autoCaptureTimer.running
                    running: visible
                    implicitWidth: Kirigami.Units.iconSizes.small
                    implicitHeight: Kirigami.Units.iconSizes.small
                }
            }
        }

        // Progress Bar
        QQC2.ProgressBar {
            Layout.fillWidth: true
            from: 0
            to: 5
            value: root.currentStep
            Accessible.name: i18n("Guided enrollment progress")
        }

        // Auto-capture toggle switch
        RowLayout {
            Layout.fillWidth: true
            visible: root.isEnrolling

            QQC2.CheckBox {
                id: autoCaptureCheck
                text: i18n("Automatic pose capture (recommended)")
                checked: root.autoCapture
                onToggled: root.autoCapture = checked
            }
        }
    }
}
