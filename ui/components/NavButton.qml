// SPDX-License-Identifier: GPL-2.0-or-later
//
// NavButton — sidebar navigation entry: quiet text row with an
// ICE selection bar. Focusable for keyboard navigation.
import QtQuick
import QtQuick.Controls
import OpenRGB2.UI.theme

ItemDelegate {
    id: root

    property bool selected: false

    implicitHeight: 40
    focusPolicy: Qt.StrongFocus
    activeFocusOnTab: true

    contentItem: Text {
        text: root.text
        color: !root.enabled ? Theme.textDisabled
             : root.selected ? Theme.textPrimary
             : Theme.textSecondary
        font.pixelSize: Theme.fontSizeBody
        font.weight: root.selected ? Font.DemiBold : Font.Normal
        verticalAlignment: Text.AlignVCenter
        leftPadding: Theme.spaceM + Theme.spaceS
    }

    background: Rectangle {
        color: root.selected ? Theme.surfaceAlt
             : root.hovered  ? Theme.surface
             : "transparent"
        border.width: root.activeFocus ? 1 : 0
        border.color: Theme.accent

        Rectangle {
            visible: root.selected
            width: 3
            anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
            color: Theme.accent
        }
    }
}
