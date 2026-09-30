// SPDX-License-Identifier: GPL-2.0-or-later
//
// AppTextField — themed text field. The stock Basic TextField
// paints a white field with near-black text — unreadable on
// (and against) the dark surfaces, the F10 unstyled-control
// failure. Same contract as AppButton / AppCheckBox /
// AppComboBox: dark field, readable text, accent focus border,
// muted disabled state.

import QtQuick
import QtQuick.Controls
import OpenRGB2.UI.theme

TextField {
    id: root

    implicitHeight: Theme.controlHeight
    color: root.enabled ? Theme.textPrimary : Theme.textDisabled
    placeholderTextColor: Theme.textDisabled
    selectionColor: Theme.accentMuted
    selectedTextColor: Theme.textPrimary
    font.pixelSize: Theme.fontSizeBody

    background: Rectangle {
        radius: Theme.radiusS
        color: root.enabled ? Theme.surfaceAlt : Theme.surface
        border.width: root.activeFocus ? 2 : 1
        border.color: root.activeFocus ? Theme.accent
                                       : Theme.borderStrong
    }
}
