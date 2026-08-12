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

    required property var cameraPreviewSession
    property var analysisSession: null
    property bool showAnalysis: false
    property bool showFramingGuide: false
    property string cardTitle: i18n("Private camera preview")

    Accessible.role: Accessible.Grouping
    Accessible.name: root.cardTitle

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(Kirigami.Units.gridUnit * 13, width * 0.72)
            clip: true

            Rectangle {
                anchors.fill: parent
                color: Kirigami.Theme.backgroundColor
            }

            CameraPreview {
                anchors.fill: parent
                session: root.cameraPreviewSession
                mirrored: true
                Accessible.name: root.cameraPreviewSession.spectrum === "ir"
                    ? i18n("Infrared camera preview")
                    : (root.cameraPreviewSession.spectrum === "rgb"
                        ? i18n("RGB camera preview")
                        : i18n("Camera preview"))
            }

            Rectangle {
                anchors.centerIn: parent
                width: Math.min(parent.width * 0.48, Kirigami.Units.gridUnit * 16)
                height: Math.min(parent.height * 0.72, Kirigami.Units.gridUnit * 20)
                radius: width / 2
                color: "transparent"
                border.color: Kirigami.Theme.highlightColor
                border.width: 2
                opacity: 0.78
                visible: root.showFramingGuide && root.cameraPreviewSession.frameAvailable

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
                visible: !root.cameraPreviewSession.frameAvailable
                text: root.cameraPreviewSession.canStartPreview
                    ? i18n("Preview is off")
                    : (root.cameraPreviewSession.busy ? i18n("Starting camera…") : i18n("Choose a camera to begin"))
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
                visible: root.cameraPreviewSession.previewActive
                padding: Kirigami.Units.smallSpacing
                text: i18n("%1 s", root.cameraPreviewSession.remainingSeconds)
                color: Kirigami.Theme.textColor
                background: Rectangle {
                    color: Qt.alpha(Kirigami.Theme.backgroundColor, 0.88)
                    radius: Kirigami.Units.cornerRadius
                }
            }
        }

        Flow {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            QQC2.ComboBox {
                id: deviceSelector
                objectName: "cameraDeviceSelector"
                width: Math.max(Kirigami.Units.gridUnit * 11, parent.width - Kirigami.Units.gridUnit * 5)
                model: root.cameraPreviewSession
                textRole: "label"
                currentIndex: root.cameraPreviewSession.selectedDeviceIndex
                enabled: root.cameraPreviewSession.canRefresh && root.cameraPreviewSession.hasUsableCamera
                    || root.cameraPreviewSession.canStartPreview
                property string accessibilityLabel: i18n("Local camera")
                Accessible.name: accessibilityLabel
                activeFocusOnTab: true
                onActivated: index => root.cameraPreviewSession.selectedDeviceIndex = index
            }

            QQC2.Button {
                objectName: "cameraRefreshButton"
                text: i18n("Refresh")
                icon.name: "view-refresh"
                enabled: root.cameraPreviewSession.canRefresh
                Accessible.name: text
                activeFocusOnTab: true
                onClicked: root.cameraPreviewSession.refreshDevices()
            }

            QQC2.Button {
                objectName: "cameraPreviewAction"
                text: root.cameraPreviewSession.canStopPreview ? i18n("Stop preview") : i18n("Start preview")
                icon.name: root.cameraPreviewSession.canStopPreview ? "media-playback-stop" : "camera-photo"
                enabled: root.cameraPreviewSession.canStopPreview || root.cameraPreviewSession.canStartPreview
                Accessible.name: text
                activeFocusOnTab: true
                onClicked: {
                    if (root.cameraPreviewSession.canStopPreview)
                        root.cameraPreviewSession.stopPreview()
                    else
                        root.cameraPreviewSession.startPreview()
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true

            QQC2.BusyIndicator {
                visible: root.cameraPreviewSession.busy
                running: visible
                Accessible.ignored: true
            }

            QQC2.Label {
                Layout.fillWidth: true
                text: root.cameraPreviewSession.statusText
                color: root.cameraPreviewSession.errorCode.length > 0
                    ? Kirigami.Theme.negativeTextColor
                    : Kirigami.Theme.disabledTextColor
                wrapMode: Text.Wrap
                Accessible.role: Accessible.StaticText
            }
        }

        QQC2.Label {
            Layout.fillWidth: true
            visible: root.cameraPreviewSession.droppedFrames > 0
            text: i18n("Dropped preview frames: %1", root.cameraPreviewSession.droppedFrames)
            color: Kirigami.Theme.disabledTextColor
            wrapMode: Text.Wrap
        }

        ColumnLayout {
            Layout.fillWidth: true
            visible: root.showAnalysis && root.analysisSession !== null
            spacing: Kirigami.Units.smallSpacing

            Kirigami.Separator { Layout.fillWidth: true }

            QQC2.Label {
                Layout.fillWidth: true
                text: i18n("Frame check is one explicit YuNet action. It is framing guidance, not liveness or authentication.")
                color: Kirigami.Theme.disabledTextColor
                wrapMode: Text.Wrap
            }

            Flow {
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing

                QQC2.BusyIndicator {
                    visible: root.analysisSession !== null && root.analysisSession.busy
                    running: visible
                    Accessible.ignored: true
                }

                QQC2.Button {
                    objectName: "visionAnalyzeAction"
                    text: i18n("Analyze current frame")
                    icon.name: "view-preview"
                    enabled: root.analysisSession !== null && root.analysisSession.canAnalyze
                    Accessible.name: text
                    activeFocusOnTab: true
                    onClicked: root.analysisSession.analyzeCurrentFrame()
                }
            }

            QQC2.Label {
                Layout.fillWidth: true
                visible: root.analysisSession !== null && root.analysisSession.resultAvailable
                text: root.analysisSession !== null ? root.analysisSession.resultSummary : ""
                font.weight: Font.DemiBold
                wrapMode: Text.Wrap
                Accessible.role: Accessible.StaticText
            }

            QQC2.Label {
                Layout.fillWidth: true
                visible: root.analysisSession !== null && root.analysisSession.resultAvailable
                text: root.analysisSession !== null ? root.analysisSession.guidanceText : ""
                color: root.analysisSession !== null && root.analysisSession.framingSuitable
                    ? Kirigami.Theme.positiveTextColor
                    : Kirigami.Theme.textColor
                wrapMode: Text.Wrap
                Accessible.role: Accessible.StaticText
            }
        }
    }
}
