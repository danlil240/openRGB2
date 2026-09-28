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

    /* 40px hit target, 30px visible swatch — the earlier 20px
       dot inside 30px was too small to aim at or tell apart. */
    implicitWidth: 40; implicitHeight: 40
    focusPolicy: Qt.StrongFocus
    activeFocusOnTab: true
    Accessible.name: qsTr("palette color %1").arg(root.swatch)
    Accessible.role: Accessible.Button

    onClicked: root.picked(String(root.swatch))

    contentItem: Rectangle {
        anchors.centerIn: parent
        width: 30; height: 30; radius: Theme.radiusS
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
