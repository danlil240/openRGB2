// SPDX-License-Identifier: GPL-2.0-or-later
//
// WaveControls — the Wave card: one set of simple controls that
// drives every enabled `wave` layer of the current look together
// (effectEdit.setFlow* → core EditorController::SetFlow, one undo
// record per change). Shown whenever the effective stack has a
// wave layer; the packaged "Wave" look is built for it.
//
//   Direction — presets plus x/y/z sliders (world axes: +x right,
//               +y up, +z toward you / the front of the PC).
//               Bands travel along the direction.
//   Colors    — 2..5 colors, evenly spaced on the first wave
//               layer's palette (it wraps, so the loop is seamless).
//   Speed     — m/s; Wave length — meters per color cycle.
//   Keyboard  — toggles a static layer targeting keyboard_body
//               (keeps the keys readable) when the look has one.
//
// Everything lands in the workspace draft like any layer edit;
// Save applies it to the lights.
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Dialogs
import OpenRGB2.UI.theme
import OpenRGB2.UI.components

Rectangle {
    id: wave
    objectName: "waveControls"
    color: Theme.surfaceAlt
    radius: Theme.radiusM
    border.color: Theme.border
    border.width: 1
    implicitHeight: col.implicitHeight + 2 * Theme.spaceM
    visible: flow.available === true

    readonly property bool ready:
        typeof effectEdit !== "undefined" && effectEdit.ready

    property var flow: ({})
    function reload() {
        flow = ready ? effectEdit.flowState() : ({})
    }
    Component.onCompleted: reload()
    onReadyChanged: reload()
    Connections {
        target: typeof effectEdit !== "undefined" ? effectEdit : null
        function onStackChanged() { wave.reload() }
        function onReadyChanged() { wave.reload() }
    }

    readonly property var dir: flow.direction || ({ x: 0, y: 0, z: -1 })
    readonly property var colors: flow.colors || []

    /* Direction presets — +z is the front of the PC. */
    readonly property var dirPresets: [
        { label: qsTr("Front → Back"), x: 0,  y: 0,  z: -1 },
        { label: qsTr("Back → Front"), x: 0,  y: 0,  z: 1 },
        { label: qsTr("Left → Right"), x: 1,  y: 0,  z: 0 },
        { label: qsTr("Right → Left"), x: -1, y: 0,  z: 0 },
        { label: qsTr("Bottom → Top"), x: 0,  y: 1,  z: 0 },
        { label: qsTr("Top → Bottom"), x: 0,  y: -1, z: 0 }
    ]
    function isDir(p) {
        return Math.abs(dir.x - p.x) < 0.02 && Math.abs(dir.y - p.y) < 0.02
            && Math.abs(dir.z - p.z) < 0.02
    }

    function setColorAt(i, c) {
        const list = colors.slice()
        list[i] = c
        effectEdit.setFlowColors(list)
    }

    ColorDialog {
        id: colorDialog
        property int target: -1
        title: qsTr("Wave color")
        onAccepted: {
            if(target >= 0)
                wave.setColorAt(target, selectedColor.toString())
        }
    }

    Column {
        id: col
        x: Theme.spaceM
        y: Theme.spaceM
        width: parent.width - 2 * Theme.spaceM
        spacing: Theme.spaceM

        Text {
            text: qsTr("Wave")
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSizeTitle
        }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.textSecondary
            text: qsTr("Colors sweep across your devices by their position in the layout. These controls change every wave layer of this look together.")
        }

        /*---- Direction -------------------------------------*/
        Text {
            text: qsTr("Direction")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSizeSmall
        }
        Flow {
            width: parent.width
            spacing: Theme.spaceS
            Repeater {
                model: wave.dirPresets
                delegate: AppButton {
                    required property var modelData
                    required property int index
                    objectName: "waveDir_" + index
                    text: modelData.label
                    primary: wave.isDir(modelData)
                    onClicked: effectEdit.setFlowDirection(
                                   modelData.x, modelData.y, modelData.z)
                }
            }
        }
        GridLayout {
            width: parent.width
            columns: 3
            columnSpacing: Theme.spaceS
            rowSpacing: 0
            Repeater {
                model: [
                    { axis: "x", hint: qsTr("left ↔ right") },
                    { axis: "y", hint: qsTr("down ↔ up") },
                    { axis: "z", hint: qsTr("back ↔ front") }
                ]
                delegate: Item {
                    id: axisRow
                    required property var modelData
                    Layout.columnSpan: 3
                    Layout.fillWidth: true
                    implicitHeight: Theme.controlHeight
                    RowLayout {
                        anchors.fill: parent
                        spacing: Theme.spaceS
                        Text {
                            text: axisRow.modelData.axis.toUpperCase()
                            color: Theme.textSecondary
                            Layout.preferredWidth: 14
                        }
                        AppSlider {
                            id: axisSlider
                            objectName: "waveDirSlider_" + axisRow.modelData.axis
                            Accessible.name: qsTr("wave direction %1")
                                                 .arg(axisRow.modelData.axis)
                            Layout.fillWidth: true
                            from: -1; to: 1; stepSize: 0.05
                            modelValue: Number(wave.dir[axisRow.modelData.axis]) || 0
                            Binding on value {
                                when: !axisSlider.pressed
                                value: axisSlider.modelValue
                                restoreMode: Binding.RestoreNone
                            }
                            onGestureCommitted: (v) => {
                                const d = { x: Number(wave.dir.x) || 0,
                                            y: Number(wave.dir.y) || 0,
                                            z: Number(wave.dir.z) || 0 };
                                d[axisRow.modelData.axis] = v;
                                if(Math.abs(d.x) + Math.abs(d.y) + Math.abs(d.z) < 1e-3)
                                    value = modelValue;   /* zero vector refused */
                                else
                                    effectEdit.setFlowDirection(d.x, d.y, d.z);
                            }
                        }
                        Text {
                            text: (axisSlider.pressed ? axisSlider.value
                                   : Number(axisSlider.modelValue)).toFixed(2)
                                  + "  " + axisRow.modelData.hint
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                            Layout.preferredWidth: 120
                        }
                    }
                }
            }
        }

        /*---- Colors ----------------------------------------*/
        Text {
            text: qsTr("Colors (%1 of 2–5)").arg(wave.colors.length)
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSizeSmall
        }
        Row {
            spacing: Theme.spaceS
            Repeater {
                model: wave.colors
                delegate: Rectangle {
                    required property int index
                    required property var modelData
                    objectName: "waveColor_" + index
                    width: 34; height: 34; radius: Theme.radiusS
                    color: modelData
                    border.color: swatchMouse.containsMouse
                                  ? Theme.accent : Theme.borderStrong
                    border.width: swatchMouse.containsMouse ? 2 : 1
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("wave color %1").arg(index + 1)
                    MouseArea {
                        id: swatchMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            colorDialog.target = index
                            colorDialog.selectedColor = modelData
                            colorDialog.open()
                        }
                    }
                }
            }
            AppButton {
                objectName: "waveColorAdd"
                text: "+"
                Accessible.name: qsTr("add wave color")
                enabled: wave.colors.length < 5
                onClicked: {
                    const list = wave.colors.slice()
                    list.push(list.length ? list[list.length - 1] : "#FFFFFF")
                    effectEdit.setFlowColors(list)
                    colorDialog.target = list.length - 1
                    colorDialog.selectedColor = list[list.length - 1]
                    colorDialog.open()
                }
            }
            AppButton {
                objectName: "waveColorRemove"
                text: "−"
                Accessible.name: qsTr("remove last wave color")
                enabled: wave.colors.length > 2
                onClicked: effectEdit.setFlowColors(
                               wave.colors.slice(0, wave.colors.length - 1))
            }
        }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.textDisabled
            text: qsTr("Click a color to change it. The last color blends back into the first, so the wave loops seamlessly.")
        }

        /*---- Speed + wave length ---------------------------*/
        GridLayout {
            width: parent.width
            columns: 3
            columnSpacing: Theme.spaceS
            rowSpacing: Theme.spaceXS

            Text { text: qsTr("Speed"); color: Theme.textSecondary }
            AppSlider {
                id: speedSlider
                objectName: "waveSpeed"
                Accessible.name: qsTr("wave speed")
                Layout.fillWidth: true
                from: 0.02; to: 0.6; stepSize: 0.01
                modelValue: Number(wave.flow.speed) || 0.12
                Binding on value {
                    when: !speedSlider.pressed
                    value: speedSlider.modelValue
                    restoreMode: Binding.RestoreNone
                }
                onGestureCommitted: (v) => effectEdit.setFlowSpeed(v)
            }
            Text {
                text: qsTr("%1 m/s").arg((speedSlider.pressed
                        ? speedSlider.value
                        : Number(speedSlider.modelValue)).toFixed(2))
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSizeSmall
                Layout.preferredWidth: 70
            }

            Text { text: qsTr("Wave length"); color: Theme.textSecondary }
            AppSlider {
                id: lengthSlider
                objectName: "waveLength"
                Accessible.name: qsTr("wave length")
                Layout.fillWidth: true
                from: 0.2; to: 3.0; stepSize: 0.05
                modelValue: Number(wave.flow.wavelength) || 1.25
                Binding on value {
                    when: !lengthSlider.pressed
                    value: lengthSlider.modelValue
                    restoreMode: Binding.RestoreNone
                }
                onGestureCommitted: (v) => effectEdit.setFlowWavelength(v)
            }
            Text {
                text: qsTr("%1 m").arg((lengthSlider.pressed
                        ? lengthSlider.value
                        : Number(lengthSlider.modelValue)).toFixed(2))
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSizeSmall
                Layout.preferredWidth: 70
            }
        }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.textDisabled
            text: qsTr("Wave length is the distance of one full color cycle. Keep it well above a fan's size (~0.12 m) so each fan shows one color at a time.")
        }

        AppCheckBox {
            objectName: "waveKeyboardWhite"
            visible: (wave.flow.keyboardLayer ?? -1) >= 0
            text: qsTr("Keep keyboard white (readable keys)")
            checked: wave.flow.keyboardOn === true
            onToggled: effectEdit.setLayerEnabled(wave.flow.keyboardLayer,
                                                  checked)
        }
    }
}
