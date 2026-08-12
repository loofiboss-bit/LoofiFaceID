// SPDX-License-Identifier: GPL-3.0-or-later
// qmllint disable unqualified

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.AbstractCard {
    id: root

    required property string eyebrow
    required property string title
    required property string description
    required property string actionText
    property string actionIcon: "go-next"
    property bool actionEnabled: true
    signal primaryAction()

    Accessible.role: Accessible.Grouping
    Accessible.name: root.title

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        QQC2.Label {
            Layout.fillWidth: true
            text: root.eyebrow
            color: Kirigami.Theme.disabledTextColor
            font.pointSize: Kirigami.Theme.smallFont.pointSize
            font.weight: Font.DemiBold
            wrapMode: Text.Wrap
        }

        Kirigami.Heading {
            Layout.fillWidth: true
            level: 1
            text: root.title
            wrapMode: Text.Wrap
        }

        QQC2.Label {
            Layout.fillWidth: true
            text: root.description
            wrapMode: Text.Wrap
        }

        QQC2.Button {
            objectName: "primaryStatusAction"
            Layout.alignment: Qt.AlignLeft
            text: root.actionText
            icon.name: root.actionIcon
            enabled: root.actionEnabled
            activeFocusOnTab: true
            Accessible.name: text
            onClicked: root.primaryAction()
        }
    }
}
