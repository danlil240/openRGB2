// SPDX-License-Identifier: GPL-2.0-or-later
//
// ColorWell — a swatch showing one color value. `color` accepts
// any Qt color string ("#RRGGBB" from the wire).
import QtQuick
import OpenRGB2.UI.theme

Rectangle {
    id: root

    property color swatch: "black"
    property string label: ""

    implicitWidth:  row.implicitWidth
    implicitHeight: 26
    color: "transparent"

    Row {
        id: row
        spacing: Theme.spaceS

        Rectangle {
            width: 26; height: 26; radius: Theme.radiusS
            color: root.swatch
            border.color: Theme.borderStrong
            border.width: 1
        }
        Text {
            visible: root.label !== ""
            text: root.label
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSizeSmall
        }
    }
}
