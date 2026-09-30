// SPDX-License-Identifier: GPL-2.0-or-later
//
// WorkspaceImportDialog — Task 9 portable-workspace popup.
// Reads a bundle through workspace.readLocalFile, stages the
// bounded inspect job (the engine unpacks under a scratch
// root and runs PresetBundle::InspectBundle — nothing lands),
// shows the report with per-conflict rename fields, then
// confirmImport applies it non-automatically. The export row
// drives the same packed-bundle surface the other way.

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import OpenRGB2.UI.theme
import OpenRGB2.UI.components

Popup {
    id: dialog
    objectName: "workspaceImportDialog"

    property var ctlOverride: null
    function ctl() {
        if (ctlOverride !== null)
            return ctlOverride
        try { return workspace } catch (e) { return null }
    }

    width: 560; height: 600
    modal: true
    closePolicy: Popup.CloseOnEscape
    padding: 0
    background: Rectangle {
        color: Theme.surface
        radius: Theme.radiusM
        border.color: Theme.borderStrong
    }

    /* "idle" | "inspecting" | "report" | "committing" |
       "done" | "exporting"                                    */
    property string phase: "idle"
    property var    report: null
    property var    pendingBytes: null
    property string status: ""
    property var    renameMap: ({})

    function reset() {
        phase = "idle"
        report = null
        pendingBytes = null
        status = ""
        renameMap = ({})
    }

    onOpened: reset()

    function readAndInspect() {
        var c = ctl()
        if (c === null) { status = qsTr("workspace unavailable"); return }
        var r = c.readLocalFile(pathField.text.trim())
        if (r.ok !== true) {
            status = String(r.error !== undefined ? r.error
                                                  : qsTr("read failed"))
            return
        }
        pendingBytes = r.base64   /* latin1 base64 wire form */
        var name = pathField.text.trim()
        name = name.substring(Math.max(name.lastIndexOf("/"),
                                       name.lastIndexOf("\\")) + 1)
        phase = "inspecting"
        status = qsTr("Inspecting bundle…")
        c.inspectImport(name, r.base64)
    }

    /* Conflict rows rebuild this map — {source_id: target_id}. */
    function collectChoices() {
        var out = {}
        var keys = Object.keys(renameMap)
        for (var i = 0; i < keys.length; i++)
            out[keys[i]] = renameMap[keys[i]]
        return out
    }

    Connections {
        target: {
            var c = dialog.ctl()
            return (c !== null && c.objectName !== undefined) ? c : null
        }
        function onImportInspectionReady(rep) {
            if (rep.ok !== true) {
                dialog.phase = "idle"
                dialog.status = rep.error !== undefined
                    ? String(rep.error) : qsTr("inspect failed")
                return
            }
            dialog.report = rep
            var seeds = {}
            var confs = rep.conflicts !== undefined ? rep.conflicts : []
            for (var i = 0; i < confs.length; i++)
                seeds[confs[i].source_id] =
                    confs[i].suggested_id !== undefined
                        ? confs[i].suggested_id : confs[i].source_id
            dialog.renameMap = seeds
            dialog.phase = "report"
            dialog.status = ""
        }
        function onImportFinished(res) {
            dialog.phase = res.ok === true ? "done" : "report"
            dialog.status = res.ok === true
                ? qsTr("Imported — not applied until you commit")
                : String(res.error !== undefined ? res.error
                                                 : qsTr("import failed"))
        }
        function onExportReady(name, bytesBase64, hash) {
            var r = dialog.ctl().writeLocalFile(
                exportPathField.text.trim(), bytesBase64)
            dialog.status = r.ok === true
                ? qsTr("Wrote %1").arg(exportPathField.text.trim())
                : String(r.error !== undefined ? r.error
                                              : qsTr("write failed"))
            dialog.phase = "idle"
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spaceL
        spacing: Theme.spaceM

        Text {
            text: qsTr("PORTABLE WORKSPACE")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSizeSmall
            font.letterSpacing: 1.5
        }

        /*---- import: pick + inspect ----*/
        Text {
            text: qsTr("IMPORT")
            color: Theme.textDisabled
            font.pixelSize: Theme.fontSizeSmall
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceS
            TextField {
                id: pathField
                objectName: "importPathField"
                Layout.fillWidth: true
                placeholderText: qsTr("path to .or2studio bundle")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSizeBody
                enabled: dialog.phase === "idle" || dialog.phase === "report"
                background: Rectangle {
                    color: Theme.bg; radius: Theme.radiusS
                    border.color: pathField.activeFocus ? Theme.accent
                                                        : Theme.borderStrong
                }
            }
            AppButton {
                objectName: "inspectBtn"
                text: qsTr("Inspect")
                enabled: (dialog.phase === "idle" || dialog.phase === "report")
                         && pathField.text.trim() !== ""
                onClicked: dialog.readAndInspect()
            }
        }

        /*---- report ----*/
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: dialog.phase === "report" && dialog.report !== null
            spacing: Theme.spaceS

            Text {
                objectName: "importSummary"
                Layout.fillWidth: true
                text: {
                    var r = dialog.report
                    if (r === null) return ""
                    var ws = r.workspace !== undefined ? r.workspace : {}
                    var types = r.types !== undefined ? r.types : []
                    var assets = r.assets !== undefined ? r.assets : []
                    var errs = r.errors !== undefined ? r.errors : []
                    var warns = r.warnings !== undefined ? r.warnings : []
                    return qsTr("%1 · %2 type(s) · %3 asset(s) · %4 error(s) · %5 warning(s)")
                        .arg(ws.name !== undefined ? ws.name : qsTr("workspace"))
                        .arg(types.length).arg(assets.length)
                        .arg(errs.length).arg(warns.length)
                }
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSizeBody
                wrapMode: Text.WordWrap
            }

            Flickable {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentHeight: reportCol.implicitHeight
                ColumnLayout {
                    id: reportCol
                    width: parent.width
                    spacing: Theme.spaceS

                    Repeater {
                        model: dialog.report !== null
                            ? (dialog.report.errors !== undefined
                               ? dialog.report.errors : []) : []
                        Text {
                            text: "· " + modelData
                            color: Theme.bad
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }
                    Repeater {
                        model: dialog.report !== null
                            ? (dialog.report.warnings !== undefined
                               ? dialog.report.warnings : []) : []
                        Text {
                            text: "· " + modelData
                            color: Theme.warn
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }
                    Text {
                        visible: dialog.report !== null
                            && dialog.report.conflicts !== undefined
                            && dialog.report.conflicts.length > 0
                        text: qsTr("TYPE CONFLICTS — rename before import")
                        color: Theme.textDisabled
                        font.pixelSize: Theme.fontSizeSmall
                    }
                    Repeater {
                        model: dialog.report !== null
                            ? (dialog.report.conflicts !== undefined
                               ? dialog.report.conflicts : []) : []
                        delegate: RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spaceS
                            Text {
                                text: modelData.source_id
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSizeSmall
                                Layout.preferredWidth: 150
                                elide: Text.ElideRight
                            }
                            Text {
                                text: "→"
                                color: Theme.textDisabled
                            }
                            TextField {
                                objectName: "rename_" + modelData.source_id
                                Layout.fillWidth: true
                                text: modelData.suggested_id !== undefined
                                    ? modelData.suggested_id
                                    : modelData.source_id
                                color: Theme.textPrimary
                                font.pixelSize: Theme.fontSizeSmall
                                onTextChanged: {
                                    var m = dialog.renameMap
                                    m[modelData.source_id] = text.trim()
                                    dialog.renameMap = m
                                }
                                background: Rectangle {
                                    color: Theme.bg; radius: Theme.radiusS
                                    border.color: Theme.borderStrong
                                }
                            }
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceS
                CheckBox {
                    id: overwriteBox
                    objectName: "overwriteBox"
                    text: qsTr("replace my layout")
                    checked: false
                }
                Item { Layout.fillWidth: true }
                AppButton {
                    objectName: "confirmImportBtn"
                    text: qsTr("Import")
                    enabled: dialog.report !== null
                             && (dialog.report.errors === undefined
                                 || dialog.report.errors.length === 0)
                    onClicked: {
                        var c = dialog.ctl()
                        if (c === null) return
                        dialog.phase = "committing"
                        dialog.status = qsTr("Importing…")
                        c.confirmImport(
                            dialog.collectChoices(),
                            overwriteBox.checked
                                ? "overwrite_workspace" : "")
                    }
                }
            }
        }

        Item {
            Layout.fillHeight: true
            visible: !(dialog.phase === "report" && dialog.report !== null)
        }

        Text {
            objectName: "importStatus"
            Layout.fillWidth: true
            visible: dialog.status !== ""
            text: dialog.status
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.WordWrap
        }
        BusyIndicator {
            visible: dialog.phase === "inspecting"
                     || dialog.phase === "committing"
                     || dialog.phase === "exporting"
            running: visible
        }

        /*---- export ----*/
        Text {
            text: qsTr("EXPORT")
            color: Theme.textDisabled
            font.pixelSize: Theme.fontSizeSmall
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceS
            TextField {
                id: exportPathField
                objectName: "exportPathField"
                Layout.fillWidth: true
                placeholderText: qsTr("destination path (.or2studio)")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSizeBody
                enabled: dialog.phase === "idle"
                background: Rectangle {
                    color: Theme.bg; radius: Theme.radiusS
                    border.color: exportPathField.activeFocus
                                  ? Theme.accent : Theme.borderStrong
                }
            }
            AppButton {
                objectName: "exportBtn"
                text: qsTr("Export")
                enabled: dialog.phase === "idle"
                         && exportPathField.text.trim() !== ""
                onClicked: {
                    var c = dialog.ctl()
                    if (c === null) return
                    var r = c.exportBundle()
                    if (r.ok === true) {
                        dialog.phase = "exporting"
                        dialog.status = qsTr("Exporting…")
                    } else {
                        dialog.status = String(
                            r.error !== undefined ? r.error
                                                  : qsTr("export refused"))
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            AppButton {
                text: qsTr("Close")
                onClicked: {
                    var c = dialog.ctl()
                    if (c !== null && dialog.phase !== "idle")
                        c.cancelImport()
                    dialog.close()
                }
            }
        }
    }
}
