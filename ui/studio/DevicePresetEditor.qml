// SPDX-License-Identifier: GPL-2.0-or-later
//
// DevicePresetEditor — the Task 9 device-type editor popup:
// id/name/category fields plus the preset body (entities,
// zones, binding_hints) as schema-true JSON. Validation and
// the throwaway preview resolve through presetCtl — nothing
// here touches the draft, bindings, or hardware. Save/delete
// ride the engine's resource jobs; terminal outcomes land in
// presetSaveResult / presetDeleteResult and surface inline.

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import OpenRGB2.UI.theme
import OpenRGB2.UI.components

Popup {
    id: editor
    objectName: "devicePresetEditor"

    /* Test seam — production uses the `presetCtl` context
       property; tests inject a stub with the same surface. */
    property var ctlOverride: null
    function ctl() {
        if (ctlOverride !== null)
            return ctlOverride
        try { return presetCtl } catch (e) { return null }
    }

    width: 520; height: 560
    modal: true
    closePolicy: Popup.CloseOnEscape
    padding: 0
    background: Rectangle {
        color: Theme.surface
        radius: Theme.radiusM
        border.color: Theme.borderStrong
    }

    /*---- state ----*/
    property bool editing:  false    /* editing an existing type */
    property bool busy:     false
    property string status: ""
    property var   lastErrors: []
    property var   previewRows: []
    /* The loaded document's non-field sections, edited as JSON. */
    property var   baseDoc: null
    /* Late-reply guard: the engine's resource_get can land while
       the user is already editing — it must never clobber
       in-progress fields. Snapshot what applyDoc last wrote and
       remember which id the outstanding fetch is for. */
    property string appliedBody: ""
    property string appliedName: ""
    property string fetchId: ""

    function buildCandidate() {
        var body = {}
        try {
            body = JSON.parse(bodyField.text)
            if (body === null || typeof body !== "object" || Array.isArray(body))
                return null
        } catch (e) {
            return null
        }
        body.schema_version = 1
        body.id       = idField.text.trim()
        body.name     = nameField.text.trim()
        body.category = catField.text.trim()
        return body
    }

    /* Open on an existing type: local view first, then the
       engine's resource_get for the full document. */
    function openFor(typeId) {
        editing = true
        status = ""
        lastErrors = []
        previewRows = []
        var c = ctl()
        if (c === null) { status = qsTr("preset controller unavailable"); return }
        var d = c.presetDocument(typeId)
        if (d && d.doc !== undefined)
            applyDoc(d.doc, d.packaged === true, d.instances || 0)
        else {
            idField.text = typeId
            nameField.text = ""
            catField.text = ""
            bodyField.text = "{\n    \"entities\": [],\n    \"zones\": []\n}"
            appliedBody = bodyField.text
            appliedName = ""
            status = qsTr("Fetching the library copy…")
        }
        fetchId = typeId
        if (typeof c.fetchPresetDocument === "function")
            c.fetchPresetDocument(typeId)
        open()
    }

    function openNew() {
        editing = false
        status = ""
        lastErrors = []
        previewRows = []
        fetchId = ""
        idField.text = ""
        nameField.text = ""
        catField.text = "misc"
        bodyField.text =
            "{\n    \"entities\": [],\n    \"zones\": [],\n    \"binding_hints\": {}\n}"
        appliedBody = bodyField.text
        appliedName = ""
        open()
    }

    function applyDoc(doc, packaged, instances) {
        if (doc === null || doc === undefined)
            return
        baseDoc = doc
        idField.text   = doc.id   !== undefined ? doc.id   : ""
        nameField.text = doc.name !== undefined ? doc.name : ""
        catField.text  = doc.category !== undefined ? doc.category : "misc"
        var body = {}
        var keys = Object.keys(doc)
        for (var i = 0; i < keys.length; i++) {
            var k = keys[i]
            if (k === "schema_version" || k === "id" || k === "name"
                    || k === "category")
                continue
            body[k] = doc[k]
        }
        bodyField.text = JSON.stringify(body, null, 4)
        appliedBody = bodyField.text
        appliedName = nameField.text
        status = packaged
            ? qsTr("Packaged default — saving writes a user override")
            : qsTr("%1 instance(s) use this type").arg(instances)
    }

    Connections {
        target: {
            var c = editor.ctl()
            return (c !== null && c.objectName !== undefined) ? c : null
        }
        function onPresetDocumentReady(doc) {
            /* A fetch reply is only honest when it answers the
               OUTSTANDING request — and only applied when the
               user hasn't typed since openFor; a late engine
               reply must never clobber in-progress edits. */
            if (doc.ok === true && doc.doc !== undefined) {
                if (String(doc.id) !== editor.fetchId)
                    return
                editor.fetchId = ""
                if (bodyField.text !== editor.appliedBody ||
                        nameField.text !== editor.appliedName) {
                    editor.status = qsTr(
                        "Library copy arrived — your edits were kept")
                    return
                }
                editor.applyDoc(doc.doc,
                                doc.packaged === true,
                                doc.instances || 0)
            }
            else if (doc.error !== undefined)
                editor.status = String(doc.error)
        }
        function onPresetSaveResult(res) {
            editor.busy = false
            if (res.ok === true) {
                var w = res.warnings !== undefined ? res.warnings : []
                editor.status = qsTr("Saved '%1'%2")
                    .arg(res.landed_id !== undefined ? res.landed_id
                                                    : res.id)
                    .arg(w.length ? qsTr(" — %1 warning(s)").arg(w.length) : "")
                editor.lastErrors = w
            } else {
                var errs = res.errors !== undefined ? res.errors : []
                editor.status = qsTr("Save failed")
                editor.lastErrors = errs
            }
        }
        function onPresetDeleteResult(res) {
            editor.busy = false
            if (res.ok === true) {
                editor.status = res.reverted_to_default === true
                    ? qsTr("Deleted — reverted to the packaged default")
                    : qsTr("Deleted '%1'").arg(res.id)
            } else {
                editor.status = res.error !== undefined
                    ? String(res.error) : qsTr("Delete failed")
            }
        }
        function onStatusMessage(text) { editor.status = text }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spaceL
        spacing: Theme.spaceM

        Text {
            text: editor.editing ? qsTr("EDIT DEVICE TYPE")
                                 : qsTr("NEW DEVICE TYPE")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSizeSmall
            font.letterSpacing: 1.5
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceM
            TextField {
                id: idField
                objectName: "presetIdField"
                Layout.fillWidth: true
                enabled: !editor.editing
                placeholderText: qsTr("type id (letters, digits, _ -)")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSizeBody
                background: Rectangle {
                    color: Theme.bg; radius: Theme.radiusS
                    border.color: idField.activeFocus ? Theme.accent
                                                      : Theme.borderStrong
                }
            }
            TextField {
                id: catField
                Layout.preferredWidth: 130
                placeholderText: qsTr("category")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSizeBody
                background: Rectangle {
                    color: Theme.bg; radius: Theme.radiusS
                    border.color: catField.activeFocus ? Theme.accent
                                                      : Theme.borderStrong
                }
            }
        }
        TextField {
            id: nameField
            objectName: "presetNameField"
            Layout.fillWidth: true
            placeholderText: qsTr("display name")
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSizeBody
            background: Rectangle {
                color: Theme.bg; radius: Theme.radiusS
                border.color: nameField.activeFocus ? Theme.accent
                                                    : Theme.borderStrong
            }
        }

        Text {
            text: qsTr("ENTITIES · ZONES · BINDING HINTS (JSON)")
            color: Theme.textDisabled
            font.pixelSize: Theme.fontSizeSmall
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            TextArea {
                id: bodyField
                objectName: "presetBodyField"
                font.family: "Consolas"
                font.pixelSize: Theme.fontSizeBody
                color: Theme.textPrimary
                wrapMode: TextEdit.NoWrap
                background: Rectangle {
                    color: Theme.bg; radius: Theme.radiusS
                    border.color: bodyField.activeFocus ? Theme.accent
                                                        : Theme.borderStrong
                }
            }
        }

        /* Errors / warnings — honest lines, never hidden. */
        Text {
            objectName: "presetEditorStatus"
            Layout.fillWidth: true
            visible: editor.status !== "" || editor.lastErrors.length > 0
            text: {
                var t = editor.status
                for (var i = 0; i < editor.lastErrors.length; i++)
                    t += (t.length ? "\n" : "") + "· " + editor.lastErrors[i]
                return t
            }
            color: editor.lastErrors.length ? Theme.warn
                                            : Theme.textSecondary
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.WordWrap
            maximumLineCount: 6
            elide: Text.ElideRight
        }
        Text {
            visible: editor.previewRows.length > 0
            text: qsTr("preview: %1 object(s), %2 emitter(s)")
                .arg(editor.previewRows.length)
                .arg(function() {
                    var n = 0
                    for (var i = 0; i < editor.previewRows.length; i++)
                        n += editor.previewRows[i].emitterCount || 0
                    return n
                }())
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSizeSmall
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceS
            AppButton {
                objectName: "presetValidateBtn"
                text: qsTr("Validate")
                enabled: !editor.busy
                onClicked: {
                    var c = editor.ctl()
                    var cand = editor.buildCandidate()
                    if (c === null || cand === null) {
                        editor.lastErrors = [qsTr("body is not a JSON object")]
                        return
                    }
                    var r = c.validatePreset(cand)
                    editor.lastErrors = r.errors || []
                    editor.status = r.ok === true
                        ? qsTr("valid") : qsTr("%1 problem(s)")
                            .arg(editor.lastErrors.length)
                }
            }
            AppButton {
                objectName: "presetPreviewBtn"
                text: qsTr("Preview")
                enabled: !editor.busy
                onClicked: {
                    var c = editor.ctl()
                    var cand = editor.buildCandidate()
                    if (c === null || cand === null) {
                        editor.lastErrors = [qsTr("body is not a JSON object")]
                        return
                    }
                    var r = c.previewPreset(cand)
                    editor.lastErrors = r.errors || []
                    editor.previewRows = r.ok === true ? (r.objects || []) : []
                    editor.status = r.ok === true
                        ? qsTr("resolved cleanly")
                        : qsTr("preview failed")
                }
            }
            Item { Layout.fillWidth: true }
            AppButton {
                objectName: "presetDeleteBtn"
                text: qsTr("Delete")
                visible: editor.editing
                enabled: !editor.busy
                onClicked: {
                    var c = editor.ctl()
                    if (c === null)
                        return
                    var r = c.deletePresetType(idField.text.trim())
                    if (r && r.ok === true)
                        editor.busy = true
                    else
                        editor.lastErrors = [
                            (r && r.error) ? String(r.error)
                                           : qsTr("delete refused")]
                }
            }
            AppButton {
                objectName: "presetSaveBtn"
                text: editor.editing ? qsTr("Save") : qsTr("Create")
                enabled: !editor.busy
                onClicked: {
                    var c = editor.ctl()
                    var cand = editor.buildCandidate()
                    if (c === null || cand === null) {
                        editor.lastErrors = [qsTr("body is not a JSON object")]
                        return
                    }
                    var r = c.savePresetType(cand, editor.editing)
                    if (r && r.ok === true) {
                        editor.busy = true
                        editor.status = qsTr("Saving…")
                    } else {
                        editor.lastErrors =
                            (r && r.errors) ? r.errors
                                            : [qsTr("save refused")]
                    }
                }
            }
            AppButton {
                text: qsTr("Close")
                onClicked: editor.close()
            }
        }
    }
}
