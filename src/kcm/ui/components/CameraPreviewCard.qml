// SPDX-License-Identifier: GPL-3.0-or-later
// qmllint disable unqualified
// qmllint disable import
// qmllint disable unresolved-type
// CameraPreview is registered by KFaceAuthKcm at runtime; it is not a QML module type.

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import io.github.loofiboss_bit.KFaceAuth 4.0

Kirigami.AbstractCard {
    id: root

    property var cameraPreviewSession: null
    property var analysisSession: null
    property bool showAnalysis: false
    property bool showFramingGuide: false
    property string cardTitle: i18n("Private camera preview")
    property bool sessionReady: root.cameraPreviewSession !== null

    Accessible.role: Accessible.Grouping
    Accessible.name: root.cardTitle

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        Item {
            id: previewContainer
            Layout.fillWidth: true
            Layout.maximumWidth: Math.round(Kirigami.Units.gridUnit * 24)
            Layout.preferredHeight: Math.min(Math.round(width * 0.5625), Math.round(Kirigami.Units.gridUnit * 12))
            Layout.minimumHeight: Math.round(Kirigami.Units.gridUnit * 9)
            Layout.alignment: Qt.AlignHCenter
            clip: true

            Rectangle {
                anchors.fill: parent
                color: Kirigami.Theme.backgroundColor
                radius: Kirigami.Units.cornerRadius
                border.color: Qt.alpha(Kirigami.Theme.textColor, 0.15)
                border.width: 1
            }

            CameraPreview {
                id: previewItem
                anchors.fill: parent
                session: root.cameraPreviewSession
                mirrored: true
                Accessible.name: root.sessionReady && root.cameraPreviewSession.spectrum === "ir"
                    ? i18n("Infrared camera preview")
                    : (root.sessionReady && root.cameraPreviewSession.spectrum === "rgb"
                        ? i18n("RGB camera preview")
                        : i18n("Camera preview"))
            }

            CameraPreviewOverlay {
                id: trackingOverlay
                anchors.fill: parent
                cameraPreviewSession: root.cameraPreviewSession
                analysisSession: root.analysisSession
                mirrored: previewItem.mirrored
                visible: root.sessionReady && root.cameraPreviewSession.frameAvailable
            }

            Rectangle {
                anchors.centerIn: parent
                width: Math.min(parent.width * 0.38, parent.height * 0.65)
                height: width * 1.35
                radius: width / 2
                color: "transparent"
                border.color: Kirigami.Theme.highlightColor
                border.width: 2
                opacity: 0.78
                visible: root.sessionReady && root.showFramingGuide && root.cameraPreviewSession.frameAvailable

                QQC2.Label {
                    anchors {
                        top: parent.bottom
                        horizontalCenter: parent.horizontalCenter
                        topMargin: Kirigami.Units.smallSpacing
                    }
                    text: i18n("Framing guidance only")
                    color: Kirigami.Theme.textColor
                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                    Accessible.role: Accessible.StaticText
                }
            }

            QQC2.Label {
                anchors.centerIn: parent
                visible: !root.sessionReady || !root.cameraPreviewSession.frameAvailable
                text: !root.sessionReady
                    ? i18n("Camera service is initializing…")
                    : (root.cameraPreviewSession.canStartPreview
                    ? i18n("Preview is off")
                    : (root.cameraPreviewSession.busy
                        ? i18n("Starting camera…")
                        : i18n("Choose a camera to begin")))
                color: Kirigami.Theme.textColor
                font.weight: Font.DemiBold
                Accessible.role: Accessible.StaticText
            }

            QQC2.Label {
                anchors {
                    top: parent.top
                    right: parent.right
                    margins: Kirigami.Units.smallSpacing
                }
                visible: root.sessionReady && root.cameraPreviewSession.previewActive
                padding: Kirigami.Units.smallSpacing
                text: root.sessionReady ? i18n("%1 s", root.cameraPreviewSession.remainingSeconds) : ""
                color: Kirigami.Theme.textColor
                background: Rectangle {
                    color: Qt.alpha(Kirigami.Theme.backgroundColor, 0.88)
                    radius: Kirigami.Units.cornerRadius
                }
            }

            Rectangle {
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                    leftMargin: Kirigami.Units.smallSpacing
                    rightMargin: Kirigami.Units.smallSpacing
                    topMargin: Kirigami.Units.smallSpacing
                }
                visible: root.analysisSession !== null && root.analysisSession.resultAvailable
                height: guidanceLabel.implicitHeight + Kirigami.Units.smallSpacing * 2
                color: root.analysisSession !== null && root.analysisSession.framingSuitable
                    ? Qt.alpha(Kirigami.Theme.positiveTextColor, 0.88)
                    : Qt.alpha(Kirigami.Theme.backgroundColor, 0.92)
                radius: Kirigami.Units.cornerRadius
                border.color: root.analysisSession !== null && root.analysisSession.framingSuitable
                    ? Kirigami.Theme.positiveTextColor
                    : Kirigami.Theme.highlightColor

                QQC2.Label {
                    id: guidanceLabel
                    anchors.fill: parent
                    anchors.margins: Kirigami.Units.smallSpacing
                    text: root.analysisSession !== null ? root.analysisSession.guidanceText : ""
                    color: root.analysisSession !== null && root.analysisSession.framingSuitable
                        ? Kirigami.Theme.backgroundColor
                        : Kirigami.Theme.textColor
                    font.weight: Font.DemiBold
                    wrapMode: Text.Wrap
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            QQC2.ComboBox {
                id: deviceSelector
                objectName: "cameraDeviceSelector"
                Layout.fillWidth: true
                Layout.minimumWidth: Kirigami.Units.gridUnit * 8
                visible: root.sessionReady && root.cameraPreviewSession.deviceCount > 1
                model: root.sessionReady ? root.cameraPreviewSession : null
                textRole: "label"
                currentIndex: root.sessionReady ? root.cameraPreviewSession.selectedDeviceIndex : -1
                enabled: root.sessionReady
                    && ((root.cameraPreviewSession.canRefresh && root.cameraPreviewSession.hasUsableCamera)
                        || root.cameraPreviewSession.canStartPreview)
                property string accessibilityLabel: i18n("Local camera")
                Accessible.name: accessibilityLabel
                activeFocusOnTab: true
                onActivated: index => {
                    if (root.sessionReady)
                        root.cameraPreviewSession.selectedDeviceIndex = index
                }
            }

            QQC2.Label {
                Layout.fillWidth: true
                visible: root.sessionReady && root.cameraPreviewSession.deviceCount === 1
                text: root.cameraPreviewSession !== null && root.cameraPreviewSession.selectedDeviceIndex >= 0
                    ? i18n("Using the only available camera")
                    : i18n("One usable camera will be selected automatically")
                color: Kirigami.Theme.disabledTextColor
                elide: Text.ElideRight
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }

            QQC2.Button {
                objectName: "cameraRefreshButton"
                text: i18n("Refresh")
                icon.name: "view-refresh"
                enabled: root.sessionReady && root.cameraPreviewSession.canRefresh
                Accessible.name: text
                activeFocusOnTab: true
                onClicked: {
                    if (root.sessionReady)
                        root.cameraPreviewSession.refreshDevices()
                }
            }

            QQC2.Button {
                objectName: "cameraPreviewAction"
                text: root.sessionReady && root.cameraPreviewSession.canStopPreview
                    ? i18n("Stop preview")
                    : i18n("Start preview")
                icon.name: root.sessionReady && root.cameraPreviewSession.canStopPreview
                    ? "media-playback-stop"
                    : "camera-photo"
                enabled: root.sessionReady
                    && (root.cameraPreviewSession.canStopPreview || root.cameraPreviewSession.canStartPreview)
                Accessible.name: text
                activeFocusOnTab: true
                onClicked: {
                    if (!root.sessionReady)
                        return
                    if (root.cameraPreviewSession.canStopPreview)
                        root.cameraPreviewSession.stopPreview()
                    else
                        root.cameraPreviewSession.startPreview()
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: root.sessionReady && (root.cameraPreviewSession.busy || root.cameraPreviewSession.statusText.length > 0)
            spacing: Kirigami.Units.smallSpacing

            QQC2.BusyIndicator {
                visible: root.sessionReady && root.cameraPreviewSession.busy
                running: visible
                Accessible.ignored: true
                implicitWidth: Kirigami.Units.iconSizes.small
                implicitHeight: Kirigami.Units.iconSizes.small
            }

            QQC2.Label {
                Layout.fillWidth: true
                text: root.sessionReady
                    ? root.cameraPreviewSession.statusText
                    : i18n("Camera service is initializing…")
                color: root.sessionReady && root.cameraPreviewSession.errorCode.length > 0
                    ? Kirigami.Theme.negativeTextColor
                    : Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.smallFont.pointSize
                wrapMode: Text.Wrap
                Accessible.role: root.sessionReady && root.cameraPreviewSession.errorCode.length > 0
                    ? Accessible.Alert : Accessible.StaticText
            }

            QQC2.Label {
                visible: root.sessionReady && root.cameraPreviewSession.droppedFrames > 0
                text: root.sessionReady
                    ? i18n("Dropped preview frames: %1", root.cameraPreviewSession.droppedFrames)
                    : ""
                color: Kirigami.Theme.disabledTextColor
                font.pointSize: Kirigami.Theme.smallFont.pointSize
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: root.showAnalysis && root.analysisSession !== null
            spacing: Kirigami.Units.smallSpacing

            QQC2.Button {
                id: visionAnalyzeButton
                objectName: "visionAnalyzeAction"
                Layout.fillWidth: true
                text: i18n("Analyze current frame")
                icon.name: "view-preview"
                enabled: root.analysisSession !== null && root.analysisSession.canAnalyze
                Accessible.name: text
                activeFocusOnTab: true
                onClicked: {
                    if (root.analysisSession !== null)
                        root.analysisSession.analyzeCurrentFrame()
                }
            }

            QQC2.BusyIndicator {
                visible: root.analysisSession !== null && root.analysisSession.busy
                running: visible
                Accessible.ignored: true
                implicitWidth: Kirigami.Units.iconSizes.small
                implicitHeight: Kirigami.Units.iconSizes.small
            }

            QQC2.Label {
                Layout.fillWidth: true
                visible: root.showAnalysis && root.analysisSession !== null && root.analysisSession.resultAvailable
                text: root.analysisSession !== null
                    ? (root.analysisSession.resultSummary
                       + (root.analysisSession.guidanceText
                              && root.analysisSession.guidanceText !== root.analysisSession.resultSummary
                          ? " — " + root.analysisSession.guidanceText
                          : ""))
                    : ""
                color: root.analysisSession !== null && root.analysisSession.framingSuitable
                    ? Kirigami.Theme.positiveTextColor
                    : (root.analysisSession !== null && root.analysisSession.faceDetected
                        ? Kirigami.Theme.highlightColor
                        : Kirigami.Theme.textColor)
                font.weight: Font.DemiBold
                font.pointSize: Kirigami.Theme.smallFont.pointSize
                wrapMode: Text.Wrap
                Accessible.role: Accessible.StaticText
            }
        }
    }
}
