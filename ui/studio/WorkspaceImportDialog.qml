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
import QtQuick.Dialogs
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
    /* Audit Q-9: a modal popup must own keyboard focus — without
       it typing went to the item behind the modal and Escape
       (CloseOnEscape acts only while the popup has focus) never
       closed it. The first field takes focus on open. */
    focus: true
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
    property string status: ""
    property var    renameMap: ({})

    function reset() {
        phase = "idle"
        report = null
        status = ""
        renameMap = ({})
        /* Audit Q-13: "replace my layout" is a per-import choice,
           never carried into the next one. */
        overwriteBox.checked = false
    }

    onOpened: {
        reset()
        pathField.forceActiveFocus()
    }

    function readAndInspect() {
        var c = ctl()
        if (c === null) { status = qsTr("workspace unavailable"); return }
        if (!canPick) return
        var r = c.readLocalFile(pathField.text.trim())
        if (r.ok !== true) {
            status = String(r.error !== undefined ? r.error
                                                  : qsTr("read failed"))
            return
        }
        var name = pathField.text.trim()
        name = name.substring(Math.max(name.lastIndexOf("/"),
                                       name.lastIndexOf("\\")) + 1)
        phase = "inspecting"
        report = null
        overwriteBox.checked = false
        status = qsTr("Inspecting bundle…")
        /* readLocalFile returns base64 text — inspectImport
           decodes it before the bounded upload. */
        c.inspectImport(name, r.base64)
    }

    /* The confirm button's real work — engine report contract:
       {valid, types[], conflicts[] (keyed `id`), assets[],
       warnings[], errors[], has_workspace}. */
    function confirm() {
        var c = ctl()
        if (c === null || report === null || phase === "committing")
            return
        var mode = ""
        if (report.has_workspace === false)
            mode = "preset_import"      /* types-only bundle */
        else if (overwriteBox.checked)
            mode = "current"          /* replace my layout */
        phase = "committing"
        status = qsTr("Importing…")
        c.confirmImport(collectChoices(), mode)
    }

    function doExport() {
        var c = ctl()
        /* "done" is quiescent too — a successful import must
           not leave the export path dead until reopen. */
        if (c === null || !quiet) return
        phase = "exporting"
        status = qsTr("Exporting…")
        /* void — the outcome lands in onExportReady or the
           controller's error signal; never read a return. */
        c.exportBundle()
    }

    /* Conflict rows rebuild this map — {conflict_id: import_id},
       keyed on the engine report's `id` field. */
    function collectChoices() {
        var out = {}
        var keys = Object.keys(renameMap)
        for (var i = 0; i < keys.length; i++)
            out[keys[i]] = renameMap[keys[i]]
        return out
    }

    /* Audit Q-13: every close path — Escape, the Close button, a
       click outside — releases a staged inspect the same way;
       Escape used to close without cancelling. (cancelImport is a
       no-op once the commit has consumed the stage.) */
    onClosed: {
        var c = ctl()
        if (c !== null && phase !== "idle")
            c.cancelImport()
    }

    /* "idle" and "done" are quiescent: after an import finishes,
       another inspect and the export stay reachable without
       reopening the dialog (audit Q-13). */
    readonly property bool quiet: phase === "idle" || phase === "done"
    readonly property bool canPick: quiet || phase === "report"

    /* FileDialog hands back file:// URLs; the fields hold plain
       local paths (the controller accepts both). */
    function localPath(u) {
        var s = String(u)
        if (s.indexOf("file:///") === 0)
            s = s.substring(8)
        else if (s.indexOf("file://") === 0)
            s = "//" + s.substring(7)      /* UNC host */
        return decodeURIComponent(s)
    }
    FileDialog {
        id: importPick
        title: qsTr("Choose a workspace bundle")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Workspace bundles (*.or2studio)"),
                      qsTr("All files (*)")]
        onAccepted: pathField.text = dialog.localPath(selectedFile)
    }
    FileDialog {
        id: exportPick
        title: qsTr("Export the workspace bundle")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "or2studio"
        nameFilters: [qsTr("Workspace bundles (*.or2studio)")]
        onAccepted: exportPathField.text = dialog.localPath(selectedFile)
    }

    Connections {
        target: {
            var c = dialog.ctl()
            return (c !== null && c.objectName !== undefined) ? c : null
        }
        function onImportInspectionReady(rep) {
            /* Engine report: {valid, types, conflicts, assets,
               warnings, errors} — `valid` is the gate. */
            if (rep.valid !== true) {
                dialog.phase = "idle"
                var errs = rep.errors !== undefined ? rep.errors : []
                dialog.status = errs.length
                    ? String(errs[0])
                    : qsTr("inspect failed")
                return
            }
            dialog.report = rep
            overwriteBox.checked = false
            var seeds = {}
            var confs = rep.conflicts !== undefined ? rep.conflicts : []
            for (var i = 0; i < confs.length; i++)
                seeds[confs[i].id] =
                    confs[i].suggested_id !== undefined
                        ? confs[i].suggested_id : confs[i].id
            dialog.renameMap = seeds
            dialog.phase = "report"
            dialog.status = ""
        }
        /* importFinished(name, ok, error) — positional args. */
        function onImportFinished(name, ok, error) {
            dialog.phase = ok === true ? "done" : "report"
            dialog.status = ok === true
                ? qsTr("Imported '%1' — not applied until you reload")
                    .arg(String(name))
                : String(error !== undefined && error !== ""
                         ? error : qsTr("import failed"))
        }
        function onExportReady(name, bytesBase64) {
            var r = dialog.ctl().writeLocalFile(
                exportPathField.text.trim(), bytesBase64)
            dialog.status = r.ok === true
                ? qsTr("Wrote %1").arg(exportPathField.text.trim())
                : String(r.error !== undefined ? r.error
                                              : qsTr("write failed"))
            dialog.phase = "idle"
        }
        /* An export/inspect failure surfaces through the shared
           error property — unlatch the busy phase so the dialog
           is never stuck "exporting". */
        function onErrorChanged() {
            var c = dialog.ctl()
            var e = (c !== null && c.error !== undefined)
                ? String(c.error) : ""
            if (e === "")
                return
            if (dialog.phase === "exporting") {
                dialog.phase = "idle"
                dialog.status = e
            } else if (dialog.phase === "committing") {
                dialog.phase = "report"
                dialog.status = e
            }
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
            AppTextField {
                id: pathField
                objectName: "importPathField"
                Layout.fillWidth: true
                placeholderText: qsTr("path to .or2studio bundle")
                enabled: dialog.canPick
            }
            AppButton {
                objectName: "importBrowseBtn"
                text: qsTr("Browse…")
                enabled: dialog.canPick
                onClicked: importPick.open()
            }
            AppButton {
                objectName: "inspectBtn"
                text: qsTr("Inspect")
                enabled: dialog.canPick
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
                        id: conflictRepeater
                        objectName: "conflictRepeater"
                        model: dialog.report !== null
                            ? (dialog.report.conflicts !== undefined
                               ? dialog.report.conflicts : []) : []
                        delegate: RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spaceS
                            Text {
                                /* Engine conflict rows:
                                   {id, local_name, bundled_name,
                                   suggested_id}. */
                                text: modelData.id +
                                      "  (" +
                                      (modelData.local_name !== undefined
                                       ? modelData.local_name : "?") +
                                      " ≠ " +
                                      (modelData.bundled_name !== undefined
                                       ? modelData.bundled_name : "?") +
                                      ")"
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSizeSmall
                                Layout.preferredWidth: 190
                                elide: Text.ElideRight
                            }
                            Text {
                                text: "→"
                                color: Theme.textDisabled
                            }
                            AppTextField {
                                objectName: "rename_" + modelData.id
                                Layout.fillWidth: true
                                text: modelData.suggested_id !== undefined
                                    ? modelData.suggested_id
                                    : modelData.id
                                font.pixelSize: Theme.fontSizeSmall
                                onTextChanged: {
                                    var m = dialog.renameMap
                                    m[modelData.id] = text.trim()
                                    dialog.renameMap = m
                                }
                            }
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceS
                AppCheckBox {
                    id: overwriteBox
                    objectName: "overwriteBox"
                    /* Commits the imported document onto the
                       ACTIVE workspace (gated on its live
                       revisions; still never applied until a
                       reload). Hidden for types-only bundles —
                       there is no layout to replace. */
                    visible: dialog.report === null
                             || dialog.report.has_workspace !== false
                    text: qsTr("replace my layout")
                    checked: false
                }
                Item { Layout.fillWidth: true }
                AppButton {
                    objectName: "confirmImportBtn"
                    text: dialog.report !== null
                             && dialog.report.has_workspace === false
                          ? qsTr("Import types")
                          : qsTr("Import")
                    enabled: dialog.report !== null
                             && (dialog.report.errors === undefined
                                 || dialog.report.errors.length === 0)
                    onClicked: dialog.confirm()
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
            /* F10: the stock Basic indicator draws near-invisible
               arcs on the dark surface — a themed dot ring. */
            contentItem: Item {
                implicitWidth: 28
                implicitHeight: 28
                Repeater {
                    model: 8
                    delegate: Rectangle {
                        required property int index
                        width: 5
                        height: 5
                        radius: 2.5
                        color: Theme.accent
                        opacity: 0.25 + (index / 8.0) * 0.75
                        x: 14 + Math.cos(index * Math.PI / 4) * 10 - 2.5
                        y: 14 + Math.sin(index * Math.PI / 4) * 10 - 2.5
                    }
                }
                RotationAnimator on rotation {
                    running: parent.visible
                    loops: Animation.Infinite
                    from: 0
                    to: 360
                    duration: 900
                }
            }
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
            AppTextField {
                id: exportPathField
                objectName: "exportPathField"
                Layout.fillWidth: true
                placeholderText: qsTr("destination path (.or2studio)")
                enabled: dialog.quiet
            }
            AppButton {
                objectName: "exportBrowseBtn"
                text: qsTr("Browse…")
                enabled: dialog.quiet
                onClicked: exportPick.open()
            }
            AppButton {
                objectName: "exportBtn"
                text: qsTr("Export")
                enabled: dialog.quiet
                         && exportPathField.text.trim() !== ""
                onClicked: dialog.doExport()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            AppButton {
                text: qsTr("Close")
                /* the cancel rides onClosed, shared with Escape */
                onClicked: dialog.close()
            }
        }
    }
}
