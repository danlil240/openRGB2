// SPDX-License-Identifier: GPL-2.0-or-later
//
// DeviceTree — searchable instance hierarchy (studio-import
// Task 5), adapted from the source-of-record ui/components/
// DeviceTree.qml onto the shared workspace draft:
//
//   - rows come from objectCtl.objectModel (SceneObjectModel
//     over the WorkspaceEditSession draft); a bare row array
//     remains the stub/test fallback
//   - rows indent by depth (instance rows follow their parent
//     instance; entities nest by object-id '/' depth)
//   - selection/visibility/lock/rename/group/ungroup/mirror/
//     delete-cascade-preview all route through objectCtl's
//     editor ops — one undo record each
//   - status chips are text + color (never color alone)
import QtQuick
import QtQuick.Controls.Basic
import OpenRGB2.UI.theme

Rectangle {
    id: treeRoot
    objectName: "deviceTree"
    color: Theme.surface

    /* Test seam — production supplies the `objectCtl` context
       property (ObjectEditorController); tests inject a stub. */
    property var bridgeOverride: null
    function br() {
        if (bridgeOverride !== null)
            return bridgeOverride
        try { return objectCtl } catch (e) { return null }
    }
    function hasBr() { return br() !== null }
    /* Connections targets must be real QObjects — test stubs are
       plain JS values (no objectName property). */
    function brQObj() {
        var b = br()
        return (b !== null && b.objectName !== undefined) ? b : null
    }

    /* Status chip — text + color, the source Theme's mapping. */
    function statusColor(kind, bound) {
        if (kind === "linked")        return Theme.accent
        if (bound === "ok")           return Theme.ok
        if (bound === "ambiguous")    return Theme.warn
        if (bound === "unresolved")   return Theme.bad
        if (kind === "device")        return Theme.warn   /* unmapped */
        return Theme.textDisabled
    }
    function statusText(kind, bound) {
        if (kind === "linked")        return qsTr("Mirrored")
        if (bound === "ok")           return qsTr("Connected")
        if (bound === "ambiguous")    return qsTr("Ambiguous")
        if (bound === "unresolved")   return qsTr("Missing")
        if (kind === "device")        return qsTr("Unmapped")
        if (kind === "group")         return qsTr("Group")
        return ""
    }

    /* Re-eval stamps: the stub path has no NOTIFY, and rows also
       re-read instance flags after toggles. */
    property int selStamp: 0
    function pokeSel() { selStamp++ }

    property string search: ""
    /* Task 8 fix: the 3D viewport's focusPeer reads this — true
       while the search field or an in-row rename input owns text
       focus so viewport shortcuts (W/E/P/F/Home) never eat typing. */
    readonly property bool textFocus:
        searchField.activeFocus || renameFocus
    property bool renameFocus: false
    /* Rows currently matching the search — recomputed by
       treeList.recount() on model/search/delegate changes. */
    property int shownCount: 0
    onSearchChanged: Qt.callLater(treeList.recount)

    function modelData() {
        var b = br()
        if (!b)
            return []
        if (b.objectModel)
            return b.objectModel
        if (b.objectList)
            return b.objectList
        return []
    }

    function selCount() {
        selStamp
        var b = br()
        return b ? b.selectedInstances.length : 0
    }

    /* Depth for the plain-array stub path: walk the row list's
       parentId chain so group>group>instance chains indent for
       real. Returns -1 when the model isn't a bare array (the
       real model answers the "depth" role instead). */
    function stubDepth(rowId, parentId) {
        var rows = modelData()
        if (!rows || rows.length === undefined)
            return -1
        var byId = {}
        for (var i = 0; i < rows.length; i++)
            byId[rows[i].id] = rows[i]
        var d = 0, cur = parentId, guard = 0
        while (cur !== undefined && cur !== "" && byId[cur]
               && guard++ < 64) {
            d++
            cur = byId[cur].parentId || ""
        }
        if (d === 0 && rowId.indexOf("/") >= 0)
            return rowId.split("/").length - 1
        return d
    }

    /* Deleting a group previews the children it takes with it.
       The controller computes the exact kill set Delete would
       use; only a cascade (kill > selection) earns the prompt —
       plain deletes stay one click, undo is the backstop. */
    function requestDelete() {
        if (!hasBr())
            return
        var b = br()
        if (typeof b.deletePreview !== "function") {
            b.deleteSelected()          /* stub/test path */
            return
        }
        var kill = b.deletePreview()
        var sel = b.selectedInstances || []
        if (kill.length <= sel.length) {
            /* Empty kill set: let deleteSelected run so a locked-
               selection refusal still surfaces on the status line. */
            b.deleteSelected()
            return
        }
        delConfirm.selSet = sel.slice()
        delConfirm.killSet = kill
        delConfirm.open()
    }

    Connections {
        target: treeRoot.brQObj()
        function onSelectionChanged() { treeRoot.selStamp++ }
        function onSceneChanged()     { treeRoot.selStamp++ }
    }

    /* Compact icon button — text glyphs keep this theme-free.
       `focusable: false` opts per-row copies out of Tab order (the
       row itself is the stop). */
    component IconBtn: Rectangle {
        property string text: ""
        property bool   active: false
        property string tip: ""
        property bool   focusable: true
        signal clicked()
        width: 26; height: 22; radius: Theme.radiusS
        color: active ? Theme.accentMuted
                      : (hov.containsMouse ? Theme.surfaceAlt : Theme.bg)
        border.color: hov.activeFocus ? Theme.accent
                    : (active ? Theme.accent : Theme.borderStrong)
        Text {
            anchors.centerIn: parent
            text: parent.text
            color: parent.enabled ? Theme.textPrimary : Theme.textDisabled
            font.pixelSize: Theme.fontSizeSmall
        }
        MouseArea {
            id: hov; anchors.fill: parent; hoverEnabled: true
            enabled: parent.enabled
            activeFocusOnTab: parent.focusable
            onClicked: parent.clicked()
            Keys.onSpacePressed:  parent.clicked()
            Keys.onReturnPressed: parent.clicked()
        }
        ToolTip.visible: (hov.containsMouse || hov.activeFocus)
                         && tip !== ""
        ToolTip.text: tip
        ToolTip.delay: 500
    }

    Column {
        anchors.fill: parent
        anchors.margins: Theme.spaceM
        spacing: Theme.spaceM

        /* Header + instance ops */
        Row {
            width: parent.width
            spacing: Theme.spaceS
            Text {
                text: qsTr("Devices")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSizeTitle; font.bold: true
                anchors.verticalCenter: parent.verticalCenter
            }
            Item { width: 1; height: 1 }
            IconBtn {
                text: "Grp"; tip: qsTr("Group selected instances")
                enabled: treeRoot.selCount() >= 2
                opacity: enabled ? 1 : 0.4
                onClicked: if (treeRoot.hasBr()) treeRoot.br().groupSelected()
            }
            IconBtn {
                text: "Un"; tip: qsTr("Ungroup selected")
                enabled: treeRoot.selCount() >= 1
                opacity: enabled ? 1 : 0.4
                onClicked: if (treeRoot.hasBr()) treeRoot.br().ungroupSelected()
            }
            IconBtn {
                text: "Mir"; tip: qsTr("Duplicate mirrored (shares output)")
                enabled: treeRoot.selCount() >= 1
                opacity: enabled ? 1 : 0.4
                onClicked: if (treeRoot.hasBr()) treeRoot.br().duplicateMirrored()
            }
            IconBtn {
                text: "Del"; tip: qsTr("Delete selected (groups ask first)")
                enabled: treeRoot.selCount() >= 1
                opacity: enabled ? 1 : 0.4
                onClicked: treeRoot.requestDelete()
            }
        }

        /* Search */
        TextField {
            id: searchField
            width: parent.width
            height: 28
            placeholderText: qsTr("Search devices…")
            placeholderTextColor: Theme.textDisabled
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSizeBody
            activeFocusOnTab: true
            onTextChanged: treeRoot.search = text
            /* Escape cancels the FIELD (clear, then drop focus) —
               swallowing the ShortcutOverride keeps it away from
               the scene's Escape -> clearEditorSelection. */
            Keys.onShortcutOverride: function(e) {
                if (e.key === Qt.Key_Escape)
                    e.accepted = true
            }
            Keys.onEscapePressed: {
                if (text !== "")
                    text = ""
                else
                    focus = false
            }
            background: Rectangle {
                color: Theme.bg; radius: Theme.radiusS
                border.color: searchField.activeFocus ? Theme.accent
                                                      : Theme.borderStrong
            }
        }

        /* Instance tree — flat model, indented by id depth. */
        ListView {
            id: treeList
            objectName: "treeList"
            width: parent.width
            height: parent.height - y
            clip: true
            model: treeRoot.modelData()
            boundsBehavior: Flickable.StopAtBounds

            /* Tally shown delegates — a +1/-1 running count drifts,
               so recount after any change via callLater. */
            function recount() {
                var n = 0
                for (var i = 0; i < count; i++) {
                    var it = itemAtIndex(i)
                    if (it && it.shown)
                        n++
                }
                treeRoot.shownCount = n
            }
            onCountChanged: Qt.callLater(recount)
            onModelChanged: Qt.callLater(recount)

            delegate: Rectangle {
                id: row
                width: treeList.width
                /* Field reader — model roles first, modelData (stub
                   list path) second. */
                function rf(name, dflt) {
                    var v = model[name]
                    if (v === undefined && typeof modelData !== "undefined")
                        v = modelData[name]
                    return v === undefined ? dflt : v
                }
                property string rowId:    rf("id", "")
                property string rowInst:  rf("instancePath", rowId)
                property string rowLabel: rf("label", rowId)
                property string rowKind:  rf("kind", "decor")
                property string rowBound: rf("bound", "none")
                property bool   rowVis:   rf("visible", true)
                property bool   rowLock:  rf("locked", false)
                /* Real tree depth — the model carries it in the
                   "depth" role (group>group>instance chains indent);
                   the stub array path walks its own parentId chain. */
                property int    depth: {
                    var d = rf("depth", -1)
                    if (typeof d === "number" && d >= 0)
                        return d
                    var s = treeRoot.stubDepth(rowId,
                                               rf("parentId", ""))
                    if (s >= 0)
                        return s
                    var p = rf("parentId", "")
                    if (p === "" || p === undefined)
                        return 0
                    if (rowId.indexOf("/") < 0)
                        return 1    /* child instance under a group */
                    return rowId.split("/").length - 1
                }
                property bool   renaming: false

                /* Row activation shared by the MouseArea and tests. */
                function activate(modifiers) {
                    if (!treeRoot.hasBr())
                        return
                    if (modifiers & Qt.ControlModifier)
                        treeRoot.br().selectInstance(row.rowInst, true)
                    else
                        treeRoot.br().select(row.rowId)
                    treeRoot.selStamp++
                }

                function matches() {
                    var s = treeRoot.search.toLowerCase()
                    if (s === "")
                        return true
                    return rowId.toLowerCase().indexOf(s) >= 0
                        || rowLabel.toLowerCase().indexOf(s) >= 0
                }
                property bool shown: matches()
                height: shown ? 28 : 0
                visible: shown
                onShownChanged:          Qt.callLater(treeList.recount)
                Component.onDestruction: Qt.callLater(treeList.recount)
                radius: Theme.radiusS
                color: sel ? Theme.accentMuted
                           : (rowMouse.containsMouse ? Theme.surfaceAlt
                                                     : "transparent")
                border.color: rowMouse.activeFocus ? Theme.accent
                                                   : "transparent"

                property bool sel: {
                    treeRoot.selStamp
                    var b = treeRoot.br()
                    return b ? b.selectedInstances.indexOf(rowInst) >= 0
                             : false
                }

                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton
                    activeFocusOnTab: true   /* rows are tab stops */
                    onClicked: function(m) {
                        row.activate(m.modifiers)
                    }
                    Keys.onSpacePressed:  row.activate(0)
                    Keys.onReturnPressed: row.activate(0)
                    onDoubleClicked: function(m) {
                        /* Inline rename on root instance rows only —
                           ids map 1:1 to instance ids there. */
                        if (row.depth === 0 || row.rowId === row.rowInst) {
                            row.renaming = true
                            renameEdit.forceActiveFocus()
                        }
                    }
                }

                Row {
                    x: Theme.spaceM + row.depth * 14
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - x - Theme.spaceM
                    spacing: 6

                    Text {
                        visible: !row.renaming
                        width: parent.width - chip.width - eyeBtn.width
                               - lockBtn.width - 18
                        text: row.rowLabel !== "" ? row.rowLabel : row.rowId
                        color: row.rowVis ? Theme.textPrimary : Theme.textDisabled
                        font.pixelSize: Theme.fontSizeBody
                        elide: Text.ElideRight
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    TextInput {
                        id: renameEdit
                        visible: row.renaming
                        width: parent.width - chip.width - eyeBtn.width
                               - lockBtn.width - 18
                        text: row.rowInst
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontSizeBody
                        selectByMouse: true
                        /* Escape cancels the RENAME — swallow the
                           ShortcutOverride so the scene's Escape ->
                           clearEditorSelection can't fire through. */
                        Keys.onShortcutOverride: function(e) {
                            if (e.key === Qt.Key_Escape)
                                e.accepted = true
                        }
                        Keys.onEscapePressed: {
                            row.renaming = false
                            renameEdit.focus = false
                        }
                        onAccepted: {
                            if (treeRoot.hasBr() && text !== row.rowInst)
                                treeRoot.br().renameInstance(row.rowInst, text)
                            row.renaming = false
                        }
                        onActiveFocusChanged: {
                            treeRoot.renameFocus = activeFocus
                            if (!activeFocus && row.renaming)
                                row.renaming = false
                        }
                    }

                    /* Status chip — text + color, never color alone. */
                    Rectangle {
                        id: chip
                        visible: chipText.text !== ""
                        width: chipText.width + 10
                        height: 16
                        radius: Theme.radiusS
                        color: "transparent"
                        border.color: treeRoot.statusColor(row.rowKind,
                                                           row.rowBound)
                        anchors.verticalCenter: parent.verticalCenter
                        Text {
                            id: chipText
                            anchors.centerIn: parent
                            text: treeRoot.statusText(row.rowKind,
                                                      row.rowBound)
                            color: treeRoot.statusColor(row.rowKind,
                                                        row.rowBound)
                            font.pixelSize: Theme.fontSizeSmall
                        }
                    }

                    IconBtn {
                        id: eyeBtn
                        text: row.rowVis ? "●" : "○"
                        tip: row.rowVis ? qsTr("Hide instance")
                                        : qsTr("Show instance")
                        focusable: false   /* the row owns the tab stop */
                        onClicked: if (treeRoot.hasBr())
                            treeRoot.br().setInstanceVisible(row.rowInst,
                                                             !row.rowVis)
                    }
                    IconBtn {
                        id: lockBtn
                        text: "L"
                        active: row.rowLock
                        tip: row.rowLock ? qsTr("Unlock instance")
                                         : qsTr("Lock instance")
                        focusable: false
                        onClicked: if (treeRoot.hasBr())
                            treeRoot.br().setInstanceLocked(row.rowInst,
                                                            !row.rowLock)
                    }
                }
            }
        }

        /* Empty state + guidance. Gated on shownCount — filtering
           collapses rows without changing the model count, so a
           no-match search lands here too. */
        Column {
            visible: treeRoot.shownCount === 0
            width: parent.width
            spacing: Theme.spaceS
            Text {
                text: treeRoot.search !== ""
                      ? qsTr("No devices match the search.")
                      : qsTr("The desk is empty.")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSizeBody
            }
            Text {
                visible: treeRoot.search === ""
                width: parent.width
                wrapMode: Text.WordWrap
                text: qsTr("Add a device type from the library, then arrange it on the canvas.")
                color: Theme.textDisabled
                font.pixelSize: Theme.fontSizeSmall
            }
        }
    }

    /* Delete preview — when the selection's kill set is larger
       than the selection itself (group cascade), list the
       children before committing. Undo remains the backstop. */
    Popup {
        id: delConfirm
        property var killSet: []
        property var selSet:  []
        /* killSet minus selSet = the children that ride along. */
        property var childIds: {
            var out = []
            for (var i = 0; i < killSet.length; i++)
                if (selSet.indexOf(killSet[i]) < 0)
                    out.push(killSet[i])
            return out
        }

        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        x: Math.round((treeRoot.width - width) / 2)
        y: Math.round((treeRoot.height - implicitHeight) / 2)
        padding: Theme.spaceL
        background: Rectangle {
            color: Theme.surface; radius: Theme.radiusM
            border.color: Theme.borderStrong
        }
        onOpened: delYesBtn.ma.forceActiveFocus()

        component DlgBtn: Rectangle {
            property string text: ""
            property bool   danger: false
            /* The focusable surface — the popup primes it. */
            property alias  ma: dma
            signal clicked()
            width: 64; height: 24; radius: Theme.radiusS
            color: danger ? (dma.containsMouse ? Theme.accentMuted : Theme.bg)
                          : (dma.containsMouse ? Theme.surfaceAlt : Theme.bg)
            border.color: dma.activeFocus ? Theme.accent : Theme.borderStrong
            Text {
                anchors.centerIn: parent
                text: parent.text
                color: parent.enabled ? Theme.textPrimary : Theme.textDisabled
                font.pixelSize: Theme.fontSizeBody
            }
            MouseArea {
                id: dma; anchors.fill: parent; hoverEnabled: true
                activeFocusOnTab: true
                onClicked: parent.clicked()
                Keys.onSpacePressed:  parent.clicked()
                Keys.onReturnPressed: parent.clicked()
            }
        }

        contentItem: Column {
            spacing: Theme.spaceM
            Text {
                text: delConfirm.selSet.length <= 1
                      ? qsTr("Delete '%1' + %2 %3?")
                        .arg(delConfirm.selSet[0] || "")
                        .arg(delConfirm.childIds.length)
                        .arg(delConfirm.childIds.length === 1
                             ? qsTr("child") : qsTr("children"))
                      : qsTr("Delete %1 instances + %2 children?")
                        .arg(delConfirm.selSet.length)
                        .arg(delConfirm.childIds.length)
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSizeBody
                font.bold: true
            }
            Text {
                /* The kill set — ids beyond the selection itself. */
                property int max: 8
                width: 260
                wrapMode: Text.WrapAnywhere
                text: delConfirm.childIds.slice(0, max).join(", ")
                      + (delConfirm.childIds.length > max
                         ? "  +" + (delConfirm.childIds.length - max)
                           + qsTr(" more") : "")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSizeSmall
            }
            Row {
                spacing: Theme.spaceM
                layoutDirection: Qt.RightToLeft
                anchors.right: parent.right
                DlgBtn {
                    id: delYesBtn
                    text: qsTr("Delete"); danger: true
                    onClicked: {
                        delConfirm.close()
                        if (treeRoot.hasBr())
                            treeRoot.br().deleteSelected()
                    }
                }
                DlgBtn {
                    text: qsTr("Cancel")
                    onClicked: delConfirm.close()
                }
            }
        }
    }
}
