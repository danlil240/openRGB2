// SPDX-License-Identifier: GPL-2.0-or-later
//
// Dashboard — plan section 5: lighting preview, current scene,
// playback, connected-device health, wireless performance
// status. Every field is fed by the IPC event/pull stores and
// reports honest unknowns — never a fabricated number.
import QtQuick
import QtQuick.Layouts
import OpenRGB2.UI.theme
import OpenRGB2.UI.components

Item {
    id: root
    objectName: "dashboardPage"

    readonly property bool engineUp:
        typeof bridge !== "undefined" && bridge.connected

    function deviceNameFor(key) {
        if(!key)
            return "controller";
        for(let i = 0; i < bridge.snapshots.length; ++i) {
            const k = bridge.snapshots[i].key;
            if(k && k.device === key.device && k.serial === key.serial
                   && k.location === key.location)
                return bridge.snapshots[i].name || key.device;
        }
        return key.device || "controller";
    }

    Flickable {
        anchors.fill: parent
        contentWidth: col.implicitWidth
        contentHeight: col.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: col
            width: root.width
            padding: Theme.spaceXL
            spacing: Theme.spaceL

            PageHeader {
                title: qsTr("Dashboard")
                subtitle: qsTr("Live lighting state from the engine")
            }

            /*---- Stat cards row ------------------------------*/
            GridLayout {
                width: parent.width - 2 * col.padding
                columns: 4
                columnSpacing: Theme.spaceL
                rowSpacing: Theme.spaceL
                uniformCellWidths: true

                /* Current scene */
                Card {
                    title: qsTr("SCENE")
                    Layout.fillWidth: true
                    Column {
                        spacing: Theme.spaceXS
                        Text {
                            text: root.engineUp
                                  ? (bridge.scene.name
                                     || qsTr("None loaded"))
                                  : qsTr("—")
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontSizeTitle
                        }
                        Text {
                            text: root.engineUp
                                  ? (bridge.scene.loaded
                                     ? qsTr("revision %1").arg(
                                           bridge.scene.revision !== undefined
                                               ? bridge.scene.revision : "?")
                                     : qsTr("no scene applied"))
                                  : qsTr("engine offline")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                        }
                    }
                }

                /* Playback */
                Card {
                    title: qsTr("PLAYBACK")
                    Layout.fillWidth: true
                    Column {
                        spacing: Theme.spaceS
                        StatusPill {
                            text: {
                                if(!root.engineUp)
                                    return qsTr("offline");
                                const st = bridge.playback.status;
                                if(bridge.playback.pending)
                                    return qsTr("applying…");
                                if(st && st.control)
                                    return st.control;
                                if(st && st.upload)
                                    return st.upload;
                                return qsTr("unknown");
                            }
                            tone: !root.engineUp ? "neutral"
                                : bridge.playback.pending ? "warn"
                                : (bridge.playback.status
                                   && bridge.playback.status.control === "playing")
                                  ? "ok" : "neutral"
                        }
                        Text {
                            text: root.engineUp
                                  ? (bridge.playback.requested_mode === "animation_playback"
                                     ? qsTr("Animation playback")
                                     : bridge.playback.requested_mode === "live_effects"
                                       ? qsTr("Live effects")
                                       : qsTr("mode unknown"))
                                  : ""
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                        }
                    }
                }

                /* Device health */
                Card {
                    title: qsTr("DEVICES")
                    Layout.fillWidth: true
                    Column {
                        spacing: Theme.spaceXS
                        Text {
                            text: {
                                if(!root.engineUp)
                                    return qsTr("—");
                                let up = 0;
                                for(let i = 0; i < bridge.snapshots.length; ++i)
                                    if(bridge.snapshots[i].connection === "connected")
                                        ++up;
                                return qsTr("%1 / %2 connected")
                                    .arg(up).arg(bridge.snapshots.length);
                            }
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontSizeTitle
                        }
                        Text {
                            text: root.engineUp && bridge.degraded
                                  ? qsTr("degraded — see Devices & Diagnostics")
                                  : (root.engineUp
                                     ? qsTr("enumerated controllers")
                                     : qsTr("engine offline"))
                            color: root.engineUp && bridge.degraded
                                   ? Theme.warn : Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                        }
                    }
                }

                /* Wireless performance — honest unknown until the
                   delivery counters actually move. */
                Card {
                    title: qsTr("WIRELESS PERFORMANCE")
                    Layout.fillWidth: true
                    Column {
                        spacing: Theme.spaceXS
                        Text {
                            id: perfValue
                            text: {
                                if(!root.engineUp)
                                    return qsTr("—");
                                const ag = bridge.frameStatus.aggregate;
                                if(ag && ag.mean_interval_s
                                   && ag.mean_interval_s > 0)
                                    return qsTr("%1 updates/s")
                                        .arg((1 / ag.mean_interval_s)
                                             .toFixed(1));
                                return qsTr("not measured");
                            }
                            color: perfValue.text === qsTr("not measured")
                                   ? Theme.textSecondary : Theme.textPrimary
                            font.pixelSize: Theme.fontSizeTitle
                        }
                        Text {
                            text: {
                                if(!root.engineUp)
                                    return qsTr("engine offline");
                                const ag = bridge.frameStatus.aggregate;
                                if(ag && ag.provenance)
                                    return qsTr("transport-accepted · %1")
                                        .arg(ag.provenance);
                                return qsTr("no delivery counters yet");
                            }
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                            width: parent.width
                        }
                    }
                }
            }

            /*---- Lighting preview -------------------------------
               Fed by frame_status pushes: colors[] is the last
               transport-accepted frame per controller — the same
               sampled colors output uses (protocol §7).    */
            Card {
                title: qsTr("LIGHTING PREVIEW")
                width: parent.width - 2 * col.padding

                Column {
                    spacing: Theme.spaceM

                    Repeater {
                        model: root.engineUp
                               ? (bridge.frameStatus.controllers || [])
                               : []

                        Column {
                            spacing: Theme.spaceXS

                            Text {
                                text: root.deviceNameFor(modelData.key)
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSizeSmall
                            }
                            FrameStrip {
                                colors: modelData.colors || []
                            }
                        }
                    }

                    EmptyState {
                        objectName: "previewEmpty"
                        anchors.horizontalCenter: parent.horizontalCenter
                        visible: !root.engineUp
                                 || !bridge.frameStatus.controllers
                                 || bridge.frameStatus.controllers.length === 0
                        title: qsTr("No frame data")
                        detail: root.engineUp
                                ? qsTr("The engine reports no delivery counters yet — the preview fills in when frames are submitted.")
                                : qsTr("Engine not connected — nothing is being driven and there is no preview.")
                    }
                }
            }
        }
    }
}
