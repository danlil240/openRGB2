// SPDX-License-Identifier: GPL-2.0-or-later
//
// GroupChip — one selectable device group on the Lighting
// page. Shows the group's label, LED truth (resolved count or
// declared emitters), and an honest state dot:
//   ok     — binding resolved and the controller is connected
//   warn   — resolved but disconnected / ambiguous / absent
//   neutral— unverified (never written) or unbound
// Keyboard: focusable, Space/Enter toggles.
import QtQuick
import QtQuick.Controls
import OpenRGB2.UI.theme

ItemDelegate {
    id: root

    property string label:      ""
    property string detail:     ""
    property string state:      "resolved"   // resolved | absent |
                                            // ambiguous | unbound
    property bool   verified:   true
    property bool   connected:  false
    property bool   selected:   false

    implicitHeight: content.implicitHeight + 2 * Theme.spaceS
    focusPolicy: Qt.StrongFocus
    activeFocusOnTab: true

    readonly property color dotColor: {
        if(!verified || state === "unbound")
            return Theme.neutral;
        if(state === "resolved" && connected)
            return Theme.ok;
        return Theme.warn;
    }

    contentItem: Row {
        id: content
        spacing: Theme.spaceS
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 8; height: 8; radius: 4
            color: root.dotColor
        }
        Column {
            anchors.verticalCenter: parent.verticalCenter
            Text {
                text: root.label
                color: !root.enabled ? Theme.textDisabled
                     : root.selected ? Theme.accent
                     : Theme.textPrimary
                font.pixelSize: Theme.fontSizeBody
                font.weight: root.selected ? Font.DemiBold : Font.Normal
            }
            Text {
                visible: root.detail !== ""
                text: root.detail
                color: Theme.textDisabled
                font.pixelSize: Theme.fontSizeSmall
            }
        }
    }

    background: Rectangle {
        radius: Theme.radiusS
        color: root.selected ? Theme.surfaceAlt
             : root.hovered  ? Theme.surfaceHover
             : Theme.surface
        border.width: root.activeFocus ? 2 : 1
        border.color: root.activeFocus ? Theme.accent
                    : root.selected    ? Theme.accentMuted
                    : Theme.border
    }
}
