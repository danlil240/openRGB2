// SPDX-License-Identifier: GPL-2.0-or-later
//
// AppCheckBox — themed checkbox. The stock Basic control paints
// a light indicator and a near-black label, which is unreadable
// on the dark surface; this keeps the control honest: themed
// label color, visible focus ring, muted disabled state.
import QtQuick
import QtQuick.Controls
import OpenRGB2.UI.theme

CheckBox {
    id: root

    implicitHeight: Theme.controlHeight
    spacing: Theme.spaceS
    focusPolicy: Qt.StrongFocus

    indicator: Rectangle {
        implicitWidth: 18
        implicitHeight: 18
        x: root.leftPadding
        y: root.topPadding + root.availableHeight / 2 - height / 2
        radius: Theme.radiusS
        color: root.enabled ? Theme.surfaceAlt : Theme.surface
        border.width: root.activeFocus ? 2 : 1
        border.color: root.activeFocus ? Theme.accent
                      : root.checked ? Theme.accent
                      : Theme.borderStrong

        Text {
            anchors.centerIn: parent
            visible: root.checked
            text: "✓"
            color: root.enabled ? Theme.accent : Theme.textDisabled
            font.pixelSize: Theme.fontSizeBody
            font.weight: Font.Bold
        }
    }

    contentItem: Text {
        text: root.text
        color: root.enabled ? Theme.textPrimary : Theme.textDisabled
        font.pixelSize: Theme.fontSizeBody
        verticalAlignment: Text.AlignVCenter
        leftPadding: root.indicator.width + root.spacing
    }
}
