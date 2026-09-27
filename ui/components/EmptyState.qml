// SPDX-License-Identifier: GPL-2.0-or-later
//
// EmptyState — honest "nothing to show" placeholder: a quiet
// glyph, a title, and a reason. Used whenever data is absent —
// never a fabricated value.
import QtQuick
import OpenRGB2.UI.theme

Item {
    id: root

    property string title:  "Nothing to show"
    property string detail: ""

    implicitWidth:  col.implicitWidth
    implicitHeight: col.implicitHeight
    width:  implicitWidth
    height: implicitHeight

    Column {
        id: col
        anchors.centerIn: parent
        spacing: Theme.spaceS

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "\u25CC"   // dotted circle — quiet placeholder glyph
            color: Theme.textDisabled
            font.pixelSize: 28
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.title
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSizeBody
            font.weight: Font.DemiBold
        }
        Text {
            visible: root.detail !== ""
            anchors.horizontalCenter: parent.horizontalCenter
            horizontalAlignment: Text.AlignHCenter
            width: Math.min(implicitWidth, 360)
            wrapMode: Text.WordWrap
            text: root.detail
            color: Theme.textDisabled
            font.pixelSize: Theme.fontSizeSmall
        }
    }
}
