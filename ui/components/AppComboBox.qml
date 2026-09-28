// SPDX-License-Identifier: GPL-2.0-or-later
//
// AppComboBox — themed combo box. The stock Basic control draws
// a light field with near-black text that clashes with the dark
// surfaces; this restyles the field, indicator, and popup list
// on the theme tokens. Keyboard/focus behavior is the stock
// ComboBox's — only the painting changes.
import QtQuick
import QtQuick.Controls
import OpenRGB2.UI.theme

ComboBox {
    id: root

    implicitHeight: Theme.controlHeight
    focusPolicy: Qt.StrongFocus

    delegate: ItemDelegate {
        id: dlg
        width: root.width
        implicitHeight: Theme.controlHeight
        highlighted: root.highlightedIndex === index

        contentItem: Text {
            text: root.textRole
                  ? (Array.isArray(root.model)
                     ? modelData[root.textRole]
                     : model[root.textRole])
                  : modelData
            color: root.enabled ? Theme.textPrimary
                                : Theme.textDisabled
            font.pixelSize: Theme.fontSizeSmall
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }

        background: Rectangle {
            color: dlg.highlighted ? Theme.surfaceHover : "transparent"
        }
    }

    contentItem: Text {
        text: root.displayText
        color: root.enabled ? Theme.textPrimary : Theme.textDisabled
        font.pixelSize: Theme.fontSizeSmall
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        leftPadding: Theme.spaceM
        rightPadding: root.indicator.width + Theme.spaceS
    }

    indicator: Text {
        x: root.width - width - Theme.spaceS
        y: root.topPadding + (root.availableHeight - height) / 2
        text: "▾"
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSizeSmall
    }

    background: Rectangle {
        implicitWidth: 140
        radius: Theme.radiusS
        color: !root.enabled ? Theme.surface
             : root.pressed || root.hovered ? Theme.surfaceHover
             : Theme.surfaceAlt
        border.width: root.activeFocus || root.popup.visible ? 2 : 1
        border.color: root.activeFocus || root.popup.visible
                      ? Theme.accent : Theme.border
    }

    popup: Popup {
        y: root.height + Theme.spaceXS
        width: root.width
        padding: 2
        implicitHeight: Math.min(contentItem.implicitHeight + 4,
                                 8 * Theme.controlHeight)

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: root.popup.visible ? root.delegateModel : null
            currentIndex: root.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator {}
        }

        background: Rectangle {
            radius: Theme.radiusS
            color: Theme.surfaceAlt
            border.width: 1
            border.color: Theme.borderStrong
        }
    }
}
