// SPDX-License-Identifier: GPL-2.0-or-later
//
// LayerStack — the ordered effect-layer list (studio-import
// Task 6). Rows come from effectEdit.layerModel (a real
// QAbstractListModel — row-granular updates keep a pressed
// slider's delegate alive mid-gesture, same guarantee the
// source-of-record model documented).
//
// Undo contract: every button/toggle lands ONE record through
// the controller invokables; the per-row opacity slider wraps
// its scrub in beginLayerGesture/commitLayerGesture so a drag
// is exactly one history entry even across model refresh.
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import OpenRGB2.UI.theme
import OpenRGB2.UI.components

Rectangle {
    id: stack
    objectName: "layerStack"
    color: "transparent"
    implicitHeight: col.implicitHeight

    readonly property bool ready:
        typeof effectEdit !== "undefined" && effectEdit.ready

    Column {
        id: col
        width: parent.width
        spacing: Theme.spaceS

        /* Effective-stack provenance: named look vs authored
           inline layers — honest about which definition the
           rows came from. */
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.textSecondary
            text: !stack.ready
                  ? qsTr("Open a workspace to edit effect layers.")
                  : effectEdit.hasInlineLayers
                  ? qsTr("Inline layer stack — edits override the named look.")
                  : effectEdit.presetId !== ""
                  ? qsTr("Resolving look “%1” — the first edit materializes it as inline layers.")
                        .arg(effectEdit.presetId)
                  : qsTr("No effect — add a layer or pick a look.")
        }

        Repeater {
            id: rows
            objectName: "layerRows"
            model: stack.ready ? effectEdit.layerModel : null

            /* Row roles arrive through the `model`/`index` context
               objects — `required property` aliases can't be used
               here because `opacity`/`enabled` are FINAL Item
               properties and would shadow instead of bind. */
            delegate: Rectangle {
                id: row
                width: parent ? parent.width : 0
                height: Theme.controlHeight + Theme.spaceM
                radius: Theme.radiusS
                color: effectEdit.selectedLayer === index
                       ? Theme.surfaceAlt : Theme.bg
                border.width: effectEdit.selectedLayer === index ? 1 : 0
                border.color: Theme.accent

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spaceS
                    anchors.rightMargin: Theme.spaceS
                    spacing: Theme.spaceS

                    AppCheckBox {
                        objectName: "layerEnabled_" + index
                        checked: model.enabled
                        Accessible.name: qsTr("layer %1 enabled").arg(index)
                        onClicked:
                            effectEdit.setLayerEnabled(index, checked)
                    }
                    Text {
                        objectName: "layerName_" + index
                        text: model.primitive
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontSizeBody
                        Layout.preferredWidth: 90
                        elide: Text.ElideRight
                    }
                    Text {
                        text: model.summary
                        color: Theme.textDisabled
                        font.pixelSize: Theme.fontSizeSmall
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }
                    StatusPill {
                        objectName: "layerSource_" + index
                        visible: model.source !== ""
                        text: model.source
                        tone: "neutral"
                    }
                    AppSlider {
                        id: opacitySlider
                        objectName: "layerOpacity_" + index
                        Accessible.name: qsTr("layer %1 opacity").arg(index)
                        Layout.preferredWidth: 90
                        from: 0; to: 1; stepSize: 0.01
                        onPressedChanged: {
                            if(pressed) {
                                effectEdit.beginLayerGesture();
                            } else {
                                effectEdit.commitLayerGesture(
                                    qsTr("layer opacity"));
                            }
                        }
                        onMoved:
                            effectEdit.setLayerOpacity(index, value)
                        Binding {
                            target: opacitySlider
                            property: "value"
                            value: model.opacity
                        }
                    }
                    AppButton {
                        objectName: "layerUp_" + index
                        text: "↑"
                        enabled: index > 0
                        Accessible.name: qsTr("move layer %1 up").arg(index)
                        onClicked:
                            effectEdit.moveLayer(index, index - 1)
                    }
                    AppButton {
                        objectName: "layerDown_" + index
                        text: "↓"
                        enabled: index < effectEdit.layerCount - 1
                        Accessible.name: qsTr("move layer %1 down").arg(index)
                        onClicked:
                            effectEdit.moveLayer(index, index + 1)
                    }
                    AppButton {
                        objectName: "layerRemove_" + index
                        text: "✕"
                        Accessible.name: qsTr("remove layer %1").arg(index)
                        onClicked: effectEdit.removeLayer(index)
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    /* Clicks select; control children take their
                       own presses (propagateComposedEvents). */
                    propagateComposedEvents: true
                    z: -1
                    onClicked: effectEdit.selectedLayer = index
                }
            }
        }

        /* Add-layer row — the primitive whitelist is the
           parser's grammar (controller refuses anything else). */
        RowLayout {
            width: parent.width
            spacing: Theme.spaceS
            AppComboBox {
                id: primitiveBox
                objectName: "addLayerPrimitive"
                Accessible.name: qsTr("new layer primitive")
                Layout.fillWidth: true
                enabled: stack.ready
                model: ["static", "gradient", "wave", "pulse",
                        "comet", "noise", "spin", "ripple",
                        "screenfield", "level"]
            }
            AppButton {
                objectName: "addLayer"
                text: qsTr("Add layer")
                enabled: stack.ready
                onClicked:
                    effectEdit.addLayer(primitiveBox.currentText)
            }
        }
    }
}
