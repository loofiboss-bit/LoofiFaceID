// SPDX-License-Identifier: GPL-3.0-or-later
// qmllint disable unqualified

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.AbstractCard {
    id: root

    Accessible.role: Accessible.Grouping
    Accessible.name: i18n("Privacy summary")

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        QQC2.Label {
            Layout.fillWidth: true
            text: i18n("Privacy at a glance")
            font.weight: Font.DemiBold
        }

        Flow {
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing

            ColumnLayout {
                width: Math.max(Kirigami.Units.gridUnit * 10, (parent.width - Kirigami.Units.largeSpacing * 2) / 3)
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Icon {
                    Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                    Layout.preferredHeight: width
                    source: "network-offline"
                    color: Kirigami.Theme.highlightColor
                    Accessible.ignored: true
                }

                QQC2.Label {
                    Layout.fillWidth: true
                    text: i18n("Local only")
                    font.weight: Font.DemiBold
                    wrapMode: Text.Wrap
                }

                QQC2.Label {
                    Layout.fillWidth: true
                    text: i18n("Processing stays in this user session.")
                    color: Kirigami.Theme.disabledTextColor
                    wrapMode: Text.Wrap
                }
            }

            ColumnLayout {
                width: Math.max(Kirigami.Units.gridUnit * 10, (parent.width - Kirigami.Units.largeSpacing * 2) / 3)
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Icon {
                    Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                    Layout.preferredHeight: width
                    source: "image-x-generic"
                    color: Kirigami.Theme.highlightColor
                    Accessible.ignored: true
                }

                QQC2.Label {
                    Layout.fillWidth: true
                    text: i18n("Images are not stored")
                    font.weight: Font.DemiBold
                    wrapMode: Text.Wrap
                }

                QQC2.Label {
                    Layout.fillWidth: true
                    text: i18n("Frames are cleared after each bounded action.")
                    color: Kirigami.Theme.disabledTextColor
                    wrapMode: Text.Wrap
                }
            }

            ColumnLayout {
                width: Math.max(Kirigami.Units.gridUnit * 10, (parent.width - Kirigami.Units.largeSpacing * 2) / 3)
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Icon {
                    Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                    Layout.preferredHeight: width
                    source: "wallet-open"
                    color: Kirigami.Theme.highlightColor
                    Accessible.ignored: true
                }

                QQC2.Label {
                    Layout.fillWidth: true
                    text: i18n("Profile encrypted with KWallet")
                    font.weight: Font.DemiBold
                    wrapMode: Text.Wrap
                }

                QQC2.Label {
                    Layout.fillWidth: true
                    text: i18n("The key is not kept in a file or sent over a network.")
                    color: Kirigami.Theme.disabledTextColor
                    wrapMode: Text.Wrap
                }
            }
        }
    }
}
