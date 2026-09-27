// SPDX-License-Identifier: GPL-2.0-or-later
//
// AppSlider — themed slider (groove + ICE handle/fill).
import QtQuick
import QtQuick.Controls
import OpenRGB2.UI.theme

Slider {
    id: root

    implicitHeight: Theme.controlHeight

    background: Rectangle {
        x: root.leftPadding
        y: root.topPadding + root.availableHeight / 2 - height / 2
        width: root.availableWidth
        height: 4
        radius: 2
        color: Theme.surfaceAlt

        Rectangle {
            width: root.visualPosition * parent.width
            height: parent.height
            radius: 2
            color: root.enabled ? Theme.accent : Theme.textDisabled
        }
    }

    handle: Rectangle {
        x: root.leftPadding + root.visualPosition
           * (root.availableWidth - width)
        y: root.topPadding + root.availableHeight / 2 - height / 2
        width: 16; height: 16; radius: 8
        color: root.enabled ? Theme.accent : Theme.textDisabled
        border.width: root.activeFocus ? 2 : 0
        border.color: Theme.accentStrong
    }
}
