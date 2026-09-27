// SPDX-License-Identifier: GPL-2.0-or-later
//
// StatusPill — compact status readout: colored dot + label on a
// quiet surface. tone: "ok" | "warn" | "bad" | "accent" | "neutral".
import QtQuick
import OpenRGB2.UI.theme

Rectangle {
    id: root

    property string text: ""
    property string tone: "neutral"

    readonly property color toneColor: Theme.toneColor(tone)

    implicitWidth:  row.implicitWidth  + 2 * Theme.spaceS
    implicitHeight: 22

    radius:       height / 2
    color:        Theme.surfaceAlt
    border.color: Theme.border
    border.width: 1

    Row {
        id: row
        anchors.centerIn: parent
        spacing: Theme.spaceXS + 2

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 7; height: 7; radius: 4
            color: root.toneColor
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: root.text
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSizeSmall
        }
    }
}
