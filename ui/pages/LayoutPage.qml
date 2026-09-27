// SPDX-License-Identifier: GPL-2.0-or-later
//
// Layout — scaffold (plan section 5): case/desk arrangement,
// linked mirrored fans, orientation, LED identification and
// calibration land through the engine's reserved
// LAYOUT_COMMAND verb space in later Milestone-4 tasks. Today
// the verbs exist and are answered honestly (the engine
// reports "unavailable"), so the page wires identify against
// the real surface and states the reservation plainly.
import QtQuick
import QtQuick.Layouts
import OpenRGB2.UI.theme
import OpenRGB2.UI.components

Item {
    id: root
    objectName: "layoutPage"

    readonly property bool engineUp:
        typeof bridge !== "undefined" && bridge.connected

    property string lastReply: ""

    Connections {
        target: typeof bridge !== "undefined" ? bridge : null
        function onReplyReceived(requestId, typeName, body) {
            if(requestId === root.pendingIdentify) {
                root.lastReply = typeName === "OK"
                    ? qsTr("identify acknowledged")
                    : qsTr("%1 — %2").arg(typeName)
                        .arg(body.message || body.error || "");
                root.pendingIdentify = 0;
            }
        }
    }

    property int pendingIdentify: 0

    Flickable {
        anchors.fill: parent
        contentHeight: col.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: col
            width: root.width
            padding: Theme.spaceXL
            spacing: Theme.spaceL

            PageHeader {
                title: qsTr("Layout")
                subtitle: qsTr("Physical arrangement, orientation, and LED identification")
            }

            Card {
                title: qsTr("ARRANGEMENT")
                width: parent.width - 2 * col.padding
                EmptyState {
                    anchors.horizontalCenter: parent.horizontalCenter
                    title: qsTr("Layout editor — coming soon")
                    detail: qsTr("Case/desk arrangement, linked mirrored fans, orientation, and undo/redo arrive with the layout engine commands. The wire surface is already reserved in the IPC protocol.")
                }
            }

            Card {
                title: qsTr("LED IDENTIFICATION")
                width: parent.width - 2 * col.padding
                Column {
                    spacing: Theme.spaceM
                    Text {
                        text: qsTr("Blink a controller's LEDs to confirm which physical device it is.")
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSizeBody
                        wrapMode: Text.WordWrap
                        width: parent.width
                    }
                    Repeater {
                        model: root.engineUp ? bridge.snapshots : []
                        RowLayout {
                            width: parent.width
                            spacing: Theme.spaceL
                            Text {
                                text: modelData.name || qsTr("controller")
                                color: Theme.textPrimary
                                font.pixelSize: Theme.fontSizeBody
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                            }
                            StatusPill {
                                text: modelData.connection || qsTr("unknown")
                                tone: modelData.connection === "connected"
                                      ? "ok" : "warn"
                            }
                            AppButton {
                                text: qsTr("Identify")
                                enabled: root.engineUp
                                onClicked: {
                                    root.pendingIdentify =
                                        bridge.layoutCommand("identify",
                                            ({ key: modelData.key }));
                                    if(root.pendingIdentify === 0)
                                        root.lastReply = qsTr("not sent — engine offline");
                                }
                            }
                        }
                    }
                    Text {
                        visible: root.lastReply !== ""
                        text: root.lastReply
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSizeSmall
                    }
                    EmptyState {
                        visible: !root.engineUp || bridge.snapshots.length === 0
                        anchors.horizontalCenter: parent.horizontalCenter
                        title: qsTr("No devices")
                        detail: qsTr("Controllers appear here once the engine enumerates them.")
                    }
                }
            }
        }
    }
}
