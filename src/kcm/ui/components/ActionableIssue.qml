// SPDX-License-Identifier: GPL-3.0-or-later
// qmllint disable unqualified

import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.InlineMessage {
    id: root

    required property string issueTitle
    required property string recoveryText

    Layout.fillWidth: true
    visible: root.issueTitle.length > 0
    type: Kirigami.MessageType.Warning
    text: root.issueTitle + "\n" + root.recoveryText
    Accessible.role: Accessible.Alert
    Accessible.name: text
}
