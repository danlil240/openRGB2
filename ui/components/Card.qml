// SPDX-License-Identifier: GPL-2.0-or-later
//
// Card — the standard surface container: neutral dark surface,
// hairline border, generous padding, optional title. Pages drop
// their content in as children.
import QtQuick
import OpenRGB2.UI.theme

Rectangle {
    id: root

    property string title: ""
    property int    padding: Theme.spaceL
    default property alias content: body.data

    color:        Theme.surface
    radius:       Theme.radiusM
    border.color: Theme.border
    border.width: 1

    implicitWidth:  col.implicitWidth  + 2 * padding
    implicitHeight: col.implicitHeight + 2 * padding

    Column {
        id: col
        anchors.fill: parent
        anchors.margins: root.padding
        spacing: Theme.spaceM

        Text {
            visible: root.title !== ""
            text: root.title
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSizeSmall
            font.weight: Font.DemiBold
            font.letterSpacing: 1.1
        }

        Item {
            id: body
            width: parent.width
            implicitHeight: childrenRect.height
            implicitWidth: childrenRect.width
        }
    }
}
