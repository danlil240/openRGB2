// SPDX-License-Identifier: GPL-2.0-or-later
//
// PageHeader — view title + optional subtitle/description line.
import QtQuick
import OpenRGB2.UI.theme

Column {
    id: root

    property string title:    ""
    property string subtitle: ""

    spacing: Theme.spaceXS

    Text {
        text: root.title
        color: Theme.textPrimary
        font.pixelSize: Theme.fontSizeDisplay
        font.weight: Font.DemiBold
    }
    Text {
        visible: root.subtitle !== ""
        text: root.subtitle
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSizeBody
    }
}
