// SPDX-License-Identifier: GPL-2.0-or-later
//
// LookShelf — personal-look save/reload surface (studio-import
// Task 6). The shelf lists the engine catalog's file-supplied
// looks (personal installs land under the workspace library);
// "Save current as" serializes the EFFECTIVE layer stack into
// a resource_install job — collision-safe engine-side (the
// landed id is reported in the result, never an overwrite).
// Picking a look adopts it as the draft's preset through ONE
// undoable record — undo restores the authored stack.
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import OpenRGB2.UI.theme
import OpenRGB2.UI.components

Rectangle {
    id: shelf
    objectName: "lookShelf"
    color: "transparent"
    implicitHeight: col.implicitHeight

    readonly property bool ready:
        typeof effectEdit !== "undefined" && effectEdit.ready

    /* Last saveLookAs outcome — surfaced inline (the async job
       resolves through lookSaveResult, not the return value). */
    property string saveStatus: ""
    Connections {
        target: typeof effectEdit !== "undefined" ? effectEdit : null
        function onLookSaveResult(res) {
            shelf.saveStatus =
                res.ok === true
                ? qsTr("Saved as look “%1”.").arg(
                      res.landed_id || res.id || "")
                : (res.errors || []).join(" ");
        }
    }

    Column {
        id: col
        width: parent.width
        spacing: Theme.spaceS

        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.textSecondary
            text: qsTr("Personal looks install into the workspace library and resolve like the packaged looks — a look pick is one undoable edit.")
        }

        /* Personal-look rows (from_file catalog entries minus the
           packaged ids the preset strip already lists). */
        Repeater {
            objectName: "lookRows"
            model: shelf.ready ? effectEdit.looks : []

            delegate: Rectangle {
                id: lookRow
                required property var modelData
                width: parent ? parent.width : 0
                height: Theme.controlHeight
                radius: Theme.radiusS
                color: effectEdit.presetId === modelData.id
                       ? Theme.surfaceAlt : Theme.panel
                border.width: effectEdit.presetId === modelData.id ? 1 : 0
                border.color: Theme.accent

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spaceS
                    anchors.rightMargin: Theme.spaceS
                    spacing: Theme.spaceS
                    Text {
                        objectName: "lookName_" + modelData.id
                        text: modelData.name || modelData.id
                        color: Theme.text
                        font.pixelSize: Theme.fontSizeBody
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }
                    StatusPill {
                        visible: (modelData.needs || "") !== ""
                        text: modelData.needs
                        tone: "neutral"
                    }
                    AppButton {
                        objectName: "lookApply_" + modelData.id
                        text: qsTr("Apply")
                        onClicked: {
                            /* Explicit notice before replacing
                               authored layers (plan mandate) —
                               a pick that loses nothing (no
                               authored stack) goes straight
                               through. */
                            if(effectEdit.hasInlineLayers) {
                                applyDialog.lookId   = modelData.id
                                applyDialog.lookName =
                                    modelData.name || modelData.id
                                applyDialog.open()
                            } else {
                                effectEdit.selectPreset(modelData.id)
                            }
                        }
                    }
                }
            }
        }

        EmptyState {
            objectName: "looksEmpty"
            anchors.horizontalCenter: parent.horizontalCenter
            visible: shelf.ready && effectEdit.looks.length === 0
            title: qsTr("No personal looks")
            detail: qsTr("Save the current layer stack as a look below.")
        }

        /* Save-as: validated id + optional name; the result is
           the async job record (collision-safe landed id). */
        RowLayout {
            width: parent.width
            spacing: Theme.spaceS
            TextField {
                id: lookId
                objectName: "lookSaveId"
                Accessible.name: qsTr("new look id")
                Layout.fillWidth: true
                enabled: shelf.ready
                placeholderText: qsTr("look id [A-Za-z0-9_-]")
            }
            TextField {
                id: lookName
                objectName: "lookSaveName"
                Accessible.name: qsTr("new look name")
                Layout.fillWidth: true
                enabled: shelf.ready
                placeholderText: qsTr("display name (optional)")
            }
            AppButton {
                objectName: "lookSave"
                text: shelf.ready && effectEdit.savingLook
                      ? qsTr("Saving…") : qsTr("Save look")
                enabled: shelf.ready && !effectEdit.savingLook
                         && lookId.text.trim() !== ""
                onClicked: {
                    const res = effectEdit.saveLookAs(
                        lookId.text, lookName.text);
                    if(res.ok !== true) {
                        shelf.saveStatus =
                            (res.errors || []).join(" ");
                    }
                }
            }
        }
        Text {
            objectName: "lookSaveStatus"
            visible: shelf.saveStatus !== ""
            text: shelf.saveStatus
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.WordWrap
            width: parent.width
        }

        /* Remix + reset — both one-record draft edits. The reset
           carries the brief's explicit notice: authored layers
           are replaced by the named look. */
        RowLayout {
            width: parent.width
            spacing: Theme.spaceS
            AppButton {
                objectName: "lookRemix"
                text: qsTr("Remix")
                enabled: shelf.ready && effectEdit.presetId !== ""
                Accessible.name: qsTr("remix look seed")
                onClicked: effectEdit.remix()
            }
            AppButton {
                objectName: "lookReset"
                text: qsTr("Reset to look")
                enabled: shelf.ready && effectEdit.hasInlineLayers
                Accessible.name: qsTr("reset layers to named look")
                onClicked: resetDialog.open()
            }
            Item { Layout.fillWidth: true }
        }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.textDisabled
            text: qsTr("Remix re-rolls the deterministic seed; Reset replaces the authored inline layers with the named look (undoable).")
        }
    }

    Dialog {
        id: resetDialog
        objectName: "lookResetDialog"
        title: qsTr("Reset effect layers?")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        anchors.centerIn: Overlay.overlay
        /* The brief's explicit notice — authored layers are
           replaced by the named look; the edit is undoable. */
        Text {
            width: 320
            wrapMode: Text.WordWrap
            color: Theme.text
            font.pixelSize: Theme.fontSizeBody
            text: qsTr("The authored layer stack is replaced by the look “%1”. You can undo this edit.")
                      .arg(shelf.ready ? effectEdit.presetId : "")
        }
        onAccepted: effectEdit.resetToPreset()
    }

    /* Same mandated notice for adopting a look over an authored
       inline stack — the pick is one undoable edit. */
    Dialog {
        id: applyDialog
        objectName: "lookApplyDialog"
        property string lookId: ""
        property string lookName: ""
        title: qsTr("Replace effect layers?")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        anchors.centerIn: Overlay.overlay
        Text {
            width: 320
            wrapMode: Text.WordWrap
            color: Theme.text
            font.pixelSize: Theme.fontSizeBody
            text: qsTr("Applying “%1” replaces the authored layer stack with the named look. You can undo this edit.")
                      .arg(applyDialog.lookName)
        }
        onAccepted: effectEdit.selectPreset(applyDialog.lookId)
    }
}
