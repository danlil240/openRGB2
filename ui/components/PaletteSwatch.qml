// SPDX-License-Identifier: GPL-2.0-or-later
//
// PaletteSwatch — one clickable palette color. `swatch` accepts
// a Theme token or a "#RRGGBB" wire color. Emits picked() with
// the hex string. Keyboard: focusable, Space/Enter picks.
import QtQuick
import QtQuick.Controls
import OpenRGB2.UI.theme

ItemDelegate {
    id: root

    property color swatch: "black"

    signal picked(string hex)

    implicitWidth: 30; implicitHeight: 30
    focusPolicy: Qt.StrongFocus
    activeFocusOnTab: true

    onClicked: root.picked(String(root.swatch))

    contentItem: Rectangle {
        anchors.centerIn: parent
        width: 20; height: 20; radius: Theme.radiusS
        color: root.swatch
        border.color: Theme.borderStrong
        border.width: 1
    }

    background: Rectangle {
        radius: Theme.radiusS
        color: root.hovered ? Theme.surfaceHover : "transparent"
        border.width: root.activeFocus ? 2 : 0
        border.color: Theme.accent
    }
}
