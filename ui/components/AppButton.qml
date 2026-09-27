// SPDX-License-Identifier: GPL-2.0-or-later
//
// AppButton — themed button. `primary` paints the ICE accent.
// Keyboard access: focusable, Space/Enter activates, visible
// focus ring (plan section 5 — keyboard access).
import QtQuick
import QtQuick.Controls
import OpenRGB2.UI.theme

Button {
    id: root

    property bool primary: false

    implicitHeight: Theme.controlHeight
    padding: Theme.spaceM
    focusPolicy: Qt.StrongFocus
    activeFocusOnTab: true
    Accessible.name: root.text
    Accessible.role: Accessible.Button

    contentItem: Text {
        text: root.text
        color: !root.enabled ? Theme.textDisabled
             : root.primary  ? Theme.accentText
             :                 Theme.textPrimary
        font.pixelSize: Theme.fontSizeBody
        font.weight: root.primary ? Font.DemiBold : Font.Normal
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        radius: Theme.radiusS
        color: !root.enabled ? Theme.surface
             : root.primary  ? (root.pressed ? Theme.accentStrong
                              : root.hovered ? Theme.accentStrong
                              : Theme.accent)
             : (root.pressed ? Theme.surfaceHover
              : root.hovered ? Theme.surfaceHover
              : Theme.surfaceAlt)
        border.width: root.activeFocus ? 2 : 1
        border.color: root.activeFocus ? Theme.accent : Theme.border

        Behavior on color {
            ColorAnimation { duration: Theme.animFast }
        }
    }
}
