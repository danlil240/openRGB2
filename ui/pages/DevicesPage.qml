// SPDX-License-Identifier: GPL-2.0-or-later
//
// Devices & Diagnostics — scaffold (plan section 5): device
// mapping surface, ownership/reconnect state, measured
// delivery rates + retry/replacement counters, diagnostic
// export. Pulls DIAG_GET/GET_STATE over the command pipe —
// the engine's honest aggregate view; missing measurements
// are shown as unknown, never fabricated.
import QtQuick
import QtQuick.Layouts
import OpenRGB2.UI.theme
import OpenRGB2.UI.components

Item {
    id: root
    objectName: "devicesPage"

    readonly property bool engineUp:
        typeof bridge !== "undefined" && bridge.connected

    function fmt(v, suffix) {
        return (v === undefined || v === null) ? "—" : v + (suffix || "");
    }

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
                title: qsTr("Devices & Diagnostics")
                subtitle: qsTr("Enumerated controllers and engine-reported counters")
            }

            /*---- Device list -----------------------------------*/
            Card {
                title: qsTr("CONTROLLERS")
                width: parent.width - 2 * col.padding
                Column {
                    spacing: Theme.spaceS
                    Repeater {
                        model: root.engineUp ? bridge.snapshots : []
                        RowLayout {
                            width: parent.width
                            spacing: Theme.spaceL
                            Column {
                                Layout.fillWidth: true
                                Text {
                                    text: modelData.name || qsTr("controller")
                                    color: Theme.textPrimary
                                    font.pixelSize: Theme.fontSizeBody
                                    elide: Text.ElideRight
                                }
                                Text {
                                    text: qsTr("%1 · %2 LEDs · %3 zone(s)")
                                        .arg(modelData.vendor || qsTr("unknown vendor"))
                                        .arg(root.fmt(modelData.led_count))
                                        .arg(modelData.zones ? modelData.zones.length : 0)
                                    color: Theme.textSecondary
                                    font.pixelSize: Theme.fontSizeSmall
                                }
                            }
                            StatusPill {
                                text: modelData.connection || qsTr("unknown")
                                tone: modelData.connection === "connected"
                                      ? "ok" : "warn"
                            }
                        }
                    }
                    EmptyState {
                        visible: !root.engineUp || bridge.snapshots.length === 0
                        anchors.horizontalCenter: parent.horizontalCenter
                        title: qsTr("No controllers")
                        detail: root.engineUp
                                ? qsTr("The engine enumerated no controllers.")
                                : qsTr("Controllers appear once the engine is connected.")
                    }
                }
            }

            /*---- Engine + link state ----------------------------*/
            GridLayout {
                width: parent.width - 2 * col.padding
                columns: 2
                columnSpacing: Theme.spaceL
                rowSpacing: Theme.spaceL
                uniformCellWidths: true

                Card {
                    title: qsTr("ENGINE")
                    Layout.fillWidth: true
                    Column {
                        spacing: Theme.spaceS
                        Text {
                            text: qsTr("state: %1").arg(
                                root.engineUp
                                    ? (bridge.serverInfo.running === false
                                       ? qsTr("not running") : qsTr("running"))
                                    : qsTr("offline"))
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontSizeBody
                        }
                        Text {
                            text: qsTr("scene clock: %1").arg(
                                root.engineUp
                                    ? root.fmt(bridge.stateReport.engine
                                               ? bridge.stateReport.engine.scene_clock_hz
                                               : undefined, " Hz")
                                    : "—")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                        }
                        Text {
                            text: qsTr("IPC sessions: %1 · events dropped: %2")
                                .arg(root.fmt(bridge.stateReport.ipc
                                              ? bridge.stateReport.ipc.sessions_active
                                              : undefined))
                                .arg(root.fmt(bridge.stateReport.ipc
                                              ? bridge.stateReport.ipc.events_dropped
                                              : undefined))
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                        }
                    }
                }

                Card {
                    title: qsTr("BACKEND LINK")
                    Layout.fillWidth: true
                    Column {
                        spacing: Theme.spaceS
                        StatusPill {
                            text: root.engineUp
                                  ? (bridge.connection.state || qsTr("unknown"))
                                  : qsTr("offline")
                            tone: !root.engineUp ? "neutral"
                                : bridge.connection.state === "connected"
                                  ? "ok" : "warn"
                        }
                        Text {
                            text: qsTr("reconnect attempts: %1")
                                .arg(root.engineUp
                                     ? root.fmt(bridge.connection.reconnect_attempt)
                                     : "—")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                        }
                        Text {
                            visible: root.engineUp
                                     && !!bridge.connection.last_error
                            text: bridge.connection.last_error || ""
                            color: Theme.warn
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                            width: parent.width
                        }
                    }
                }
            }

            /*---- Diagnostics pull -------------------------------*/
            Card {
                title: qsTr("DIAGNOSTICS")
                width: parent.width - 2 * col.padding
                Column {
                    spacing: Theme.spaceM
                    Row {
                        spacing: Theme.spaceM
                        AppButton {
                            text: qsTr("Refresh")
                            enabled: root.engineUp
                            onClicked: {
                                bridge.getDiagnostics();
                                bridge.getState();
                                bridge.getDeliveryStats();
                            }
                        }
                        AppButton {
                            text: qsTr("Export…")
                            enabled: false
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("diagnostic export arrives with a later task")
                            color: Theme.textDisabled
                            font.pixelSize: Theme.fontSizeSmall
                        }
                    }

                    GridLayout {
                        width: parent.width
                        columns: 3
                        columnSpacing: Theme.spaceXL
                        rowSpacing: Theme.spaceS

                        Text { text: qsTr("uptime"); color: Theme.textSecondary; font.pixelSize: Theme.fontSizeSmall }
                        Text { text: qsTr("messages in"); color: Theme.textSecondary; font.pixelSize: Theme.fontSizeSmall }
                        Text { text: qsTr("resync bytes"); color: Theme.textSecondary; font.pixelSize: Theme.fontSizeSmall }

                        Text {
                            text: root.fmt(bridge.diagnostics.engine
                                           ? bridge.diagnostics.engine.uptime_s
                                           : undefined, " s")
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontSizeBody
                        }
                        Text {
                            text: root.fmt(bridge.diagnostics.ipc
                                           ? bridge.diagnostics.ipc.messages_in
                                           : undefined)
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontSizeBody
                        }
                        Text {
                            text: root.fmt(bridge.diagnostics.ipc
                                           ? bridge.diagnostics.ipc.resync_bytes
                                           : undefined)
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontSizeBody
                        }
                    }

                    Text {
                        visible: !root.engineUp
                        text: qsTr("Engine not connected — no diagnostics.")
                        color: Theme.textDisabled
                        font.pixelSize: Theme.fontSizeSmall
                    }
                    Text {
                        visible: root.engineUp
                                 && !bridge.diagnostics.backend_counters
                        text: qsTr("backend counters: not yet published by the backend")
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSizeSmall
                    }
                }
            }
        }
    }
}
