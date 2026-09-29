// SPDX-License-Identifier: GPL-2.0-or-later
//
// EffectInspector — per-layer detail editor (studio-import
// Task 6). Binds to effectEdit.selectedLayer and re-reads
// effectEdit.layerAt() on every stackChanged — the shared
// draft is the only truth; nothing here caches a private copy.
//
// Sliders wrap scrubs in beginLayerGesture/commitLayerGesture
// so a drag is ONE undo record (the source bridge's gesture
// contract). Discrete edits (combos, checkboxes, text fields)
// land a record per commit.
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import OpenRGB2.UI.theme
import OpenRGB2.UI.components

Rectangle {
    id: insp
    objectName: "effectInspector"
    color: "transparent"
    implicitHeight: col.implicitHeight

    readonly property bool ready:
        typeof effectEdit !== "undefined" && effectEdit.ready
    readonly property int layerIndex:
        ready ? effectEdit.selectedLayer : -1

    /* Live layer snapshot — re-read on stack changes; empty when
       no layer is selected or the index fell off the end. */
    property var layerData: ({})
    function reload() {
        layerData = (ready && layerIndex >= 0)
                ? effectEdit.layerAt(layerIndex) : ({})
    }
    Component.onCompleted: reload()
    onLayerIndexChanged: reload()
    Connections {
        target: typeof effectEdit !== "undefined" ? effectEdit : null
        function onStackChanged() { insp.reload() }
    }

    /* One wrapped scrub: begin on press, write on move, commit
       on release — a slider drag is one history entry even as
       the model refreshes underneath it. */
    function scrubField(field, v) {
        effectEdit.setLayerField(layerIndex, field, v)
    }

    Column {
        id: col
        width: parent.width
        spacing: Theme.spaceM

        Text {
            visible: insp.layerIndex < 0
            text: qsTr("Select a layer in the stack to edit it.")
            color: Theme.textDisabled
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.WordWrap
            width: parent.width
        }

        GridLayout {
            visible: insp.layerIndex >= 0
            width: parent.width
            columns: 2
            columnSpacing: Theme.spaceL
            rowSpacing: Theme.spaceS

            Text { text: qsTr("Primitive"); color: Theme.textSecondary }
            Text {
                objectName: "inspPrimitive"
                text: insp.layerData.primitive || ""
                color: Theme.text
            }

            Text { text: qsTr("Blend"); color: Theme.textSecondary }
            AppComboBox {
                id: blendBox
                objectName: "inspBlend"
                Accessible.name: qsTr("layer blend mode")
                Layout.fillWidth: true
                model: ["replace", "add", "screen"]
                onActivated:
                    effectEdit.setLayerBlend(insp.layerIndex,
                                             currentText)
                Binding {
                    target: blendBox
                    property: "currentIndex"
                    value: ["replace", "add", "screen"].indexOf(
                               insp.layerData.blend || "replace")
                    restoreMode: Binding.RestoreBinding
                }
            }

            Text { text: qsTr("Space"); color: Theme.textSecondary }
            AppComboBox {
                id: spaceBox
                objectName: "inspSpace"
                Accessible.name: qsTr("layer coordinate space")
                Layout.fillWidth: true
                model: ["world", "local"]
                onActivated:
                    effectEdit.setLayerSpace(insp.layerIndex,
                                             currentText)
                Binding {
                    target: spaceBox
                    property: "currentIndex"
                    value: ["world", "local"].indexOf(
                               insp.layerData.space || "world")
                    restoreMode: Binding.RestoreBinding
                }
            }

            Text { text: qsTr("Source"); color: Theme.textSecondary }
            RowLayout {
                spacing: Theme.spaceS
                Layout.fillWidth: true
                AppComboBox {
                    id: sourceBox
                    objectName: "inspSource"
                    Accessible.name: qsTr("layer reactive source")
                    Layout.fillWidth: true
                    model: ["", "audio", "key", "screen"]
                    displayText: currentText === ""
                                 ? qsTr("all inputs") : currentText
                    onActivated:
                        effectEdit.setLayerSource(insp.layerIndex,
                                                  currentText)
                    Binding {
                        target: sourceBox
                        property: "currentIndex"
                        value: Math.max(0,
                            ["", "audio", "key", "screen"].indexOf(
                                insp.layerData.source || ""))
                        restoreMode: Binding.RestoreBinding
                    }
                }
                /* Honest input-state badge — the engine reports
                   whether the source is actually running. */
                StatusPill {
                    objectName: "inspSourceState"
                    visible: (insp.layerData.source || "") !== ""
                    readonly property var st:
                        (insp.layerData.source || "") !== ""
                        && typeof effectEdit !== "undefined"
                        ? effectEdit.inputSourceState(
                              insp.layerData.source) : ({})
                    text: !st.ready ? qsTr("not running") : qsTr("live")
                    tone: st.ready ? "ok" : "warn"
                }
            }

            Text { text: qsTr("Targets"); color: Theme.textSecondary }
            TextField {
                id: targetsField
                objectName: "inspTargets"
                Accessible.name: qsTr("layer targets")
                Layout.fillWidth: true
                placeholderText: qsTr("object ids / tags — empty = all")
                text: (insp.layerData.targets || []).join(", ")
                onAccepted:
                    effectEdit.setLayerTargets(
                        insp.layerIndex,
                        text.split(",").map(function(t) {
                            return t.trim();
                        }).filter(function(t) { return t !== ""; }))
            }
        }

        /* Scalar fields — one wrapped gesture per scrub. */
        GridLayout {
            visible: insp.layerIndex >= 0
            width: parent.width
            columns: 4
            columnSpacing: Theme.spaceS
            rowSpacing: Theme.spaceXS

            Repeater {
                model: [
                    { "f": "speed",   "label": qsTr("Speed"),
                      "from": -10, "to": 10, "step": 0.05 },
                    { "f": "scale",   "label": qsTr("Scale"),
                      "from": 0.01, "to": 4, "step": 0.01 },
                    { "f": "phase",   "label": qsTr("Phase"),
                      "from": 0, "to": 1, "step": 0.01 },
                    { "f": "density", "label": qsTr("Density"),
                      "from": 0, "to": 8, "step": 0.05 },
                ]
                delegate: ColumnLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 0
                    Text {
                        text: modelData.label
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSizeSmall
                    }
                    AppSlider {
                        id: fieldSlider
                        objectName: "insp_" + modelData.f
                        Accessible.name: "layer " + modelData.f
                        Layout.fillWidth: true
                        from: modelData.from; to: modelData.to
                        stepSize: modelData.step
                        onPressedChanged: {
                            if(pressed)
                                effectEdit.beginLayerGesture();
                            else
                                effectEdit.commitLayerGesture(
                                    "layer " + modelData.f);
                        }
                        onMoved:
                            insp.scrubField(modelData.f, value)
                        Binding {
                            target: fieldSlider
                            property: "value"
                            value: insp.layerData[modelData.f] || 0
                        }
                    }
                    Text {
                        text: Number(insp.layerData[modelData.f] || 0)
                                  .toFixed(2)
                        color: Theme.textDisabled
                        font.pixelSize: Theme.fontSizeSmall
                    }
                }
            }
        }

        /* Origin + direction — world or object-local meters,
           numeric triples committed on accept. */
        GridLayout {
            visible: insp.layerIndex >= 0
            width: parent.width
            columns: 4
            columnSpacing: Theme.spaceS
            rowSpacing: Theme.spaceXS

            Text { text: qsTr("Origin"); color: Theme.textSecondary }
            Repeater {
                model: ["x", "y", "z"]
                delegate: TextField {
                    required property string modelData
                    objectName: "inspOrigin_" + modelData
                    Accessible.name: qsTr("layer origin %1").arg(modelData)
                    Layout.fillWidth: true
                    inputMethodHints: Qt.ImhFormattedNumbersOnly
                    text: insp.layerData.origin
                          ? String(insp.layerData.origin[modelData]) : "0"
                    onAccepted: {
                        const o = insp.layerData.origin || {};
                        const v = { "x": o.x || 0,
                                    "y": o.y || 0,
                                    "z": o.z || 0 };
                        v[modelData] = parseFloat(text) || 0;
                        effectEdit.setLayerOrigin(insp.layerIndex,
                                                  v.x, v.y, v.z);
                    }
                }
            }
            Text { text: qsTr("Direction"); color: Theme.textSecondary }
            Repeater {
                model: ["x", "y", "z"]
                delegate: TextField {
                    required property string modelData
                    objectName: "inspDir_" + modelData
                    Accessible.name: qsTr("layer direction %1").arg(modelData)
                    Layout.fillWidth: true
                    inputMethodHints: Qt.ImhFormattedNumbersOnly
                    text: insp.layerData.direction
                          ? String(insp.layerData.direction[modelData]) : "0"
                    onAccepted: {
                        const d = insp.layerData.direction || {};
                        const v = { "x": d.x || 0,
                                    "y": d.y || 0,
                                    "z": d.z || 0 };
                        v[modelData] = parseFloat(text) || 0;
                        effectEdit.setLayerDirection(insp.layerIndex,
                                                     v.x, v.y, v.z);
                    }
                }
            }
        }

        /* Gradient stops. */
        PaletteEditor {
            visible: insp.layerIndex >= 0
            width: parent.width
            layerIndex: insp.layerIndex
        }

        /* Path points (comet waypoints) — numeric rows. */
        Column {
            visible: insp.layerIndex >= 0
                     && (insp.layerData.path || []).length > 0
            width: parent.width
            spacing: Theme.spaceXS
            Text {
                text: qsTr("Path points")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSizeSmall
            }
            Repeater {
                model: insp.layerData.path || []
                delegate: RowLayout {
                    required property int index
                    required property var modelData
                    spacing: Theme.spaceS
                    Text {
                        text: qsTr("#%1  %2, %3, %4")
                                  .arg(index)
                                  .arg(Number(modelData.x).toFixed(2))
                                  .arg(Number(modelData.y).toFixed(2))
                                  .arg(Number(modelData.z).toFixed(2))
                        color: Theme.text
                        font.pixelSize: Theme.fontSizeSmall
                        Layout.fillWidth: true
                    }
                    AppButton {
                        objectName: "pathRemove_" + index
                        text: "✕"
                        Accessible.name: qsTr("remove path point %1").arg(index)
                        onClicked:
                            effectEdit.removeLayerPathPoint(
                                insp.layerIndex, index)
                    }
                }
            }
        }
    }
}
