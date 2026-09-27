// SPDX-License-Identifier: GPL-2.0-or-later
//
// Lighting — plan section 5 scaffold: preset cards, the
// Animation playback / Live effects selector with its plain
// explanations, and the supported parameter controls (speed,
// direction, color) the wire protocol actually offers.
// Scene composition (choose -> devices -> save) is layered on
// by later Milestone-4 tasks; what is wired here is real.
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import OpenRGB2.UI.theme
import OpenRGB2.UI.components

Item {
    id: root
    objectName: "lightingPage"

    readonly property bool engineUp:
        typeof bridge !== "undefined" && bridge.connected
    readonly property string requestedMode:
        engineUp ? (bridge.playback.requested_mode || "") : ""

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
                title: qsTr("Lighting")
                subtitle: qsTr("Effects, palette, and playback mode")
            }

            /*---- Playback mode selector -------------------------
               Spec wording verbatim: "Smooth repeating effects
               played by the fans" / "Responds to music, screen,
               and live changes." Show pending honestly.   */
            RowLayout {
                width: parent.width - 2 * col.padding
                spacing: Theme.spaceL

                Repeater {
                    model: [
                        {
                            key: "animation_playback",
                            title: qsTr("Animation playback"),
                            blurb: qsTr("Smooth repeating effects played by the fans")
                        },
                        {
                            key: "live_effects",
                            title: qsTr("Live effects"),
                            blurb: qsTr("Responds to music, screen, and live changes")
                        }
                    ]

                    Card {
                        Layout.fillWidth: true
                        border.color: root.requestedMode === modelData.key
                                      ? Theme.accent : Theme.border
                        border.width: root.requestedMode === modelData.key ? 2 : 1

                        Column {
                            spacing: Theme.spaceXS
                            Text {
                                text: modelData.title
                                color: Theme.textPrimary
                                font.pixelSize: Theme.fontSizeBody
                                font.weight: Font.DemiBold
                            }
                            Text {
                                text: modelData.blurb
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSizeSmall
                                wrapMode: Text.WordWrap
                                width: parent.width
                            }
                            StatusPill {
                                visible: root.requestedMode === modelData.key
                                text: bridge.playback.pending
                                      ? qsTr("applying…")
                                      : qsTr("selected")
                                tone: bridge.playback.pending ? "warn" : "accent"
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            enabled: root.engineUp
                                     && root.requestedMode !== modelData.key
                            cursorShape: enabled ? Qt.PointingHandCursor
                                                 : Qt.ArrowCursor
                            onClicked: bridge.setPlaybackMode(
                                           modelData.key, "", {})
                        }
                    }
                }
            }

            /*---- Preset parameters ------------------------------
               The protocol offers speed / reverse / color for the
               uploaded presets — expose exactly those, and only
               while a mode is requested.                    */
            Card {
                title: qsTr("PARAMETERS")
                width: parent.width - 2 * col.padding

                Column {
                    spacing: Theme.spaceM

                    RowLayout {
                        width: parent.width
                        spacing: Theme.spaceL

                        Text {
                            text: qsTr("Speed")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSizeBody
                            Layout.preferredWidth: 90
                        }
                        AppSlider {
                            id: speedSlider
                            Layout.fillWidth: true
                            enabled: root.engineUp
                            from: 10; to: 200; value: 100; stepSize: 1
                            onMoved: sendTimer.restart()
                        }
                        Text {
                            text: Math.round(speedSlider.value)
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontSizeBody
                            Layout.preferredWidth: 40
                        }
                    }

                    /* Debounce: one SET_PLAYBACK_MODE per gesture,
                       never a flood of in-flight requests. */
                    Timer {
                        id: sendTimer
                        interval: 150
                        onTriggered: if(root.engineUp && root.requestedMode)
                            bridge.setPlaybackMode(
                                root.requestedMode, "",
                                ({ speed: Math.round(speedSlider.value) }))
                    }

                    RowLayout {
                        spacing: Theme.spaceL
                        Text {
                            text: qsTr("Color")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSizeBody
                            Layout.preferredWidth: 90
                        }
                        ColorWell { swatch: "#7FCFFF" }
                        Text {
                            text: qsTr("applies to presets that take a color")
                            color: Theme.textDisabled
                            font.pixelSize: Theme.fontSizeSmall
                        }
                    }

                    Text {
                        visible: !root.engineUp
                        text: qsTr("Engine not connected — controls are inert.")
                        color: Theme.textDisabled
                        font.pixelSize: Theme.fontSizeSmall
                    }
                }
            }

            /*---- Preset cards ----------------------------------*/
            Text {
                text: qsTr("PRESETS")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSizeSmall
                font.weight: Font.DemiBold
                font.letterSpacing: 1.1
            }

            Flow {
                width: parent.width - 2 * col.padding
                spacing: Theme.spaceL

                Repeater {
                    model: root.engineUp ? bridge.presets : []
                    Card {
                        width: 260
                        Column {
                            spacing: Theme.spaceXS
                            Text {
                                text: modelData.name || modelData.id || qsTr("preset")
                                color: Theme.textPrimary
                                font.pixelSize: Theme.fontSizeBody
                                font.weight: Font.DemiBold
                            }
                            Text {
                                text: modelData.description || ""
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSizeSmall
                                wrapMode: Text.WordWrap
                                width: parent.width
                            }
                        }
                    }
                }

                EmptyState {
                    visible: !root.engineUp || bridge.presets.length === 0
                    title: qsTr("No presets")
                    detail: root.engineUp
                            ? qsTr("The engine published no preset cards.")
                            : qsTr("Preset cards arrive with the engine connection.")
                }
            }
        }
    }
}
