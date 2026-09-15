// SPDX-License-Identifier: GPL-3.0-or-later
// qmllint disable unqualified

import QtQuick
import org.kde.kirigami as Kirigami

Item {
    id: root

    property var cameraPreviewSession: null
    property var analysisSession: null
    property bool mirrored: true
    readonly property bool hasTracking: analysisSession !== null && analysisSession.faceDetected

    readonly property real frameW: (analysisSession && analysisSession.frameWidth > 0)
        ? analysisSession.frameWidth
        : ((cameraPreviewSession && cameraPreviewSession.frameWidth > 0) ? cameraPreviewSession.frameWidth : 1920)
    readonly property real frameH: (analysisSession && analysisSession.frameHeight > 0)
        ? analysisSession.frameHeight
        : ((cameraPreviewSession && cameraPreviewSession.frameHeight > 0) ? cameraPreviewSession.frameHeight : 1080)

    readonly property real scaleFactor: Math.min(width / frameW, height / frameH)
    readonly property real displayedW: frameW * scaleFactor
    readonly property real displayedH: frameH * scaleFactor
    readonly property real offsetX: (width - displayedW) / 2.0
    readonly property real offsetY: (height - displayedH) / 2.0

    function mapX(x, w) {
        let boxWidth = w || 0
        if (root.mirrored) {
            return offsetX + (frameW - (x + boxWidth)) * scaleFactor
        } else {
            return offsetX + x * scaleFactor
        }
    }

    function mapY(y) {
        return offsetY + y * scaleFactor
    }

    // Dynamic Bounding Box
    Item {
        id: boundingBox
        visible: root.hasTracking
        opacity: root.hasTracking ? 1.0 : 0.0

        readonly property var rect: root.hasTracking ? root.analysisSession.faceRect : Qt.rect(0, 0, 0, 0)
        readonly property real targetX: root.mapX(rect.x, rect.width)
        readonly property real targetY: root.mapY(rect.y)
        readonly property real targetW: rect.width * root.scaleFactor
        readonly property real targetH: rect.height * root.scaleFactor

        x: targetX
        y: targetY
        width: targetW
        height: targetH

        Behavior on x { NumberAnimation { duration: 80; easing.type: Easing.OutQuad } }
        Behavior on y { NumberAnimation { duration: 80; easing.type: Easing.OutQuad } }
        Behavior on width { NumberAnimation { duration: 80; easing.type: Easing.OutQuad } }
        Behavior on height { NumberAnimation { duration: 80; easing.type: Easing.OutQuad } }
        Behavior on opacity { NumberAnimation { duration: 150 } }

        // Subtle box border
        Rectangle {
            anchors.fill: parent
            color: "transparent"
            border.color: Kirigami.Theme.highlightColor
            border.width: 1
            opacity: 0.45
            radius: 4
        }

        // Corner accents
        readonly property real cornerLen: Math.min(width, height) * 0.22
        readonly property real cornerThick: 2.5

        // Top-left
        Rectangle {
            anchors { top: parent.top; left: parent.left }
            width: boundingBox.cornerLen
            height: boundingBox.cornerThick
            color: Kirigami.Theme.highlightColor
            radius: 1
        }
        Rectangle {
            anchors { top: parent.top; left: parent.left }
            width: boundingBox.cornerThick
            height: boundingBox.cornerLen
            color: Kirigami.Theme.highlightColor
            radius: 1
        }

        // Top-right
        Rectangle {
            anchors { top: parent.top; right: parent.right }
            width: boundingBox.cornerLen
            height: boundingBox.cornerThick
            color: Kirigami.Theme.highlightColor
            radius: 1
        }
        Rectangle {
            anchors { top: parent.top; right: parent.right }
            width: boundingBox.cornerThick
            height: boundingBox.cornerLen
            color: Kirigami.Theme.highlightColor
            radius: 1
        }

        // Bottom-left
        Rectangle {
            anchors { bottom: parent.bottom; left: parent.left }
            width: boundingBox.cornerLen
            height: boundingBox.cornerThick
            color: Kirigami.Theme.highlightColor
            radius: 1
        }
        Rectangle {
            anchors { bottom: parent.bottom; left: parent.left }
            width: boundingBox.cornerThick
            height: boundingBox.cornerLen
            color: Kirigami.Theme.highlightColor
            radius: 1
        }

        // Bottom-right
        Rectangle {
            anchors { bottom: parent.bottom; right: parent.right }
            width: boundingBox.cornerLen
            height: boundingBox.cornerThick
            color: Kirigami.Theme.highlightColor
            radius: 1
        }
        Rectangle {
            anchors { bottom: parent.bottom; right: parent.right }
            width: boundingBox.cornerThick
            height: boundingBox.cornerLen
            color: Kirigami.Theme.highlightColor
            radius: 1
        }
    }

}
