// SPDX-License-Identifier: GPL-2.0-or-later
//
// DeviceLibrary — device-type catalog panel (studio-import
// Task 5), adapted from the source-of-record ui/components/
// DeviceLibrary.qml onto the OpenRGB2 architecture:
//
//   - rows come from objectCtl.presetModel (PresetListModel
//     over the local PresetRegistry merged with the engine's
//     WORKSPACE_COMMAND catalog); a plain-JS stub — array of
//     row maps or a {count,rowAt} object — drives the same
//     pipeline in tests
//   - search + category chips filter; favorites pin first,
//     then alphabetical
//   - rows: category-initial thumbnail, name, dims in mm, LED
//     total, source badge (file|packaged|engine), star toggle,
//     add button; rows drag with Drag.keys
//     ["studio-device-type"] — the LayoutPage canvas DropArea
//     maps the pixel to desk meters and calls
//     objectCtl.addDeviceInstance
//   - type authoring (Save variant / Create preset / delete
//     type / Edit) surfaces ONLY when the controller exposes
//     those verbs — type authoring files land with the bundle
//     task, so a stock Task-5 build hides them honestly
import QtQuick
import QtQuick.Controls.Basic
import OpenRGB2.UI.theme

Rectangle {
    id: lib
    objectName: "deviceLibrary"
    color: Theme.surface

    /* Test seam — production supplies the `objectCtl` context
       property; tests inject a stub. sceneView is the drop
       target the button-add maps through (the 2D canvas on the
       Layout page exposes canvasCenter() in meters). */
    property var bridgeOverride: null
    property var sceneView: null
    /* Task 9 seams — kept so the panel doesn't change shape when
       type authoring lands. */
    signal editRequested(string typeId)
    signal newRequested()
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
    /* Type authoring exists only when the controller exposes it —
       today that's Task 9; the buttons hide rather than dead-end. */
    function canAuthorTypes() {
        var b = br()
        return b !== null
            && typeof b.saveInstanceAsVariant === "function"
            && typeof b.createTypeFromSelection === "function"
    }
    function canDeleteTypes() {
        var b = br()
        return b !== null && typeof b.deletePresetType === "function"
    }

    /* Re-eval stamps — presetLibraryChanged/selectionChanged bump
       them on a real controller; the stub path pokes by hand. */
    property int stamp: 0
    property int selStamp: 0

    property string search: ""
    property string category: ""      /* "" = All */

    Connections {
        target: lib.brQObj()
        function onPresetLibraryChanged() { lib.stamp++ }
        function onSelectionChanged()     { lib.selStamp++ }
    }

    /*===================== data pipeline ====================*/
    /* Merged registry rows as plain maps — QAbstractListModel
       ({count(),rowAt(i)}) or a bare array (stub). */
    function libRows() {
        lib.stamp                      /* re-read on refresh */
        var b = br()
        var m = (b && b.presetModel !== undefined) ? b.presetModel : null
        var rows = []
        if (m === null)
            return rows
        if (m.length !== undefined) {            /* array stub */
            for (var i = 0; i < m.length; i++)
                rows.push(m[i])
            return rows
        }
        if (typeof m.rowAt === "function") {
            var n = (typeof m.count === "function") ? m.count()
                                                  : m.count
            for (var r = 0; r < n; r++) {
                var row = m.rowAt(r)
                if (row && row.typeId !== undefined)
                    rows.push(row)
            }
        }
        return rows
    }

    function allTypeIds() {
        var rows = libRows()
        var out = []
        for (var i = 0; i < rows.length; i++)
            out.push(rows[i].typeId)
        return out
    }

    /* Derived category list (sorted, unique) for the chip row. */
    function categories() {
        var rows = libRows()
        var seen = {}, out = []
        for (var i = 0; i < rows.length; i++) {
            var c = rows[i].category || "misc"
            if (!seen[c]) {
                seen[c] = 1
                out.push(c)
            }
        }
        out.sort()
        return out
    }

    /* Search + category filter, then favorites-first alphabetical.
       Pure JS — the test suite exercises this directly. */
    function filteredRows() {
        var q = lib.search.toLowerCase()
        var rows = libRows()
        var out = []
        for (var i = 0; i < rows.length; i++) {
            var r = rows[i]
            var cat = r.category || "misc"
            if (lib.category !== "" && cat !== lib.category)
                continue
            if (q !== ""
                && (r.name   || "").toLowerCase().indexOf(q) < 0
                && (r.typeId || "").toLowerCase().indexOf(q) < 0
                && cat.toLowerCase().indexOf(q) < 0)
                continue
            out.push(r)
        }
        out.sort(function(a, b) {
            var fa = a.favorite ? 1 : 0
            var fb = b.favorite ? 1 : 0
            if (fa !== fb)
                return fb - fa                    /* favorites first */
            var na = (a.name || a.typeId || "").toLowerCase()
            var nb = (b.name || b.typeId || "").toLowerCase()
            if (na !== nb)
                return na < nb ? -1 : 1           /* then A-Z */
            return (a.typeId || "") < (b.typeId || "") ? -1 : 1
        })
        return out
    }

    /* One compute shared by the list and the empty-state checks. */
    property var shown: lib.filteredRows()
    property int totalCount: lib.libRows().length

    function selIds() {
        lib.selStamp
        var b = br()
        return (b && b.selectedInstances) ? b.selectedInstances : []
    }

    function toggleFav(tid, on) {
        var b = br()
        if (b && typeof b.setTypeFavorite === "function")
            b.setTypeFavorite(tid, on)
        lib.stamp++                  /* stub path has no NOTIFY */
    }

    /* Returns "" on success, else the refusal — the caller
       (delete dialog) shows it inline. */
    function deleteType(tid) {
        var b = br()
        if (!b || typeof b.deletePresetType !== "function")
            return "type deletion isn't available yet"
        var r = b.deletePresetType(tid)
        if (r && r.ok)
            return ""
        return (r && r.error) ? String(r.error) : "delete failed"
    }

    /* Button add: the drop surface's center point — the 2D canvas
       exposes canvasCenter() returning {x,y,z} meters on the desk
       plane; a 3D sceneView's camera answers planeHitPoint like
       the source. Falls back to the origin. */
    function addAtCenter(tid) {
        var b = br()
        if (!b || typeof b.addDeviceInstance !== "function")
            return
        var p = null
        var sv = lib.sceneView
        if (sv && typeof sv.canvasCenter === "function") {
            p = sv.canvasCenter()
        } else if (sv && sv.editorCam) {
            p = sv.editorCam.planeHitPoint(sv.width / 2,
                                           sv.height / 2, 1, 0.0)
            if (!p && sv.editorCam.target)
                p = sv.editorCam.target
        }
        b.addDeviceInstance(tid, p ? p.x : 0, 0, p ? p.z : 0)
    }

    /* Dialog validation — mirrors the bridge's IsPresetId charset
       so obvious rejects never leave QML (C++ still re-checks). */
    function validTypeId(t) { return /^[A-Za-z0-9_-]+$/.test(t) }
    function dlgError(tid, nm) {
        if (tid === "")
            return "type id required"
        if (!validTypeId(tid))
            return "id: letters, digits, _ and - only"
        if (lib.allTypeIds().indexOf(tid) >= 0)
            return "type id already exists"
        if (nm === "")
            return "display name required"
        return ""
    }

    /* Dialog dispatch — one seam for the ok button AND the tests.
       mode "variant" repoints `inst`; "assembly" builds a
       child-ref type from the CURRENT selection. */
    function submitNewType(mode, inst, tid, nm) {
        var b = br()
        if (!b)
            return
        if (mode === "variant") {
            if (typeof b.saveInstanceAsVariant === "function")
                b.saveInstanceAsVariant(inst, tid, nm)
        } else if (typeof b.createTypeFromSelection === "function") {
            b.createTypeFromSelection(selIds(), tid, nm)
        }
    }

    /* Compact text button (toolbar set — matches DeviceTree's). */
    component LBtn: Rectangle {
        property string text: ""
        property bool   active: false
        property string tip: ""
        property alias  ma: lma
        signal clicked()
        height: 22; radius: Theme.radiusS
        color: active ? Theme.accentMuted
                      : (lma.containsMouse ? Theme.surfaceAlt : Theme.bg)
        border.color: lma.activeFocus ? Theme.accent
                    : (active ? Theme.accent : Theme.borderStrong)
        opacity: enabled ? 1 : 0.4
        Text {
            anchors.centerIn: parent
            text: parent.text
            color: parent.enabled ? Theme.textPrimary : Theme.textDisabled
            font.pixelSize: Theme.fontSizeBody
        }
        MouseArea {
            id: lma; anchors.fill: parent; hoverEnabled: true
            enabled: parent.enabled
            activeFocusOnTab: true
            onClicked: parent.clicked()
            Keys.onSpacePressed:  parent.clicked()
            Keys.onReturnPressed: parent.clicked()
        }
        ToolTip.visible: (lma.containsMouse || lma.activeFocus)
                         && tip !== ""
        ToolTip.text: tip
        ToolTip.delay: 500
    }

    /* Small square icon button for row actions (star / add). */
    component IconBtn: Rectangle {
        property string text: ""
        property bool   active: false
        property string tip: ""
        signal clicked()
        width: 26; height: 22; radius: Theme.radiusS
        color: active ? Theme.accentMuted
                      : (ihov.containsMouse ? Theme.surfaceAlt : Theme.bg)
        border.color: ihov.activeFocus ? Theme.accent
                    : (active ? Theme.accent : Theme.borderStrong)
        opacity: enabled ? 1 : 0.4
        Text {
            anchors.centerIn: parent
            text: parent.text
            color: parent.active ? Theme.accent
                   : (parent.enabled ? Theme.textPrimary : Theme.textDisabled)
            font.pixelSize: Theme.fontSizeSmall
        }
        MouseArea {
            id: ihov; anchors.fill: parent; hoverEnabled: true
            enabled: parent.enabled
            onClicked: parent.clicked()
            Keys.onSpacePressed:  parent.clicked()
            Keys.onReturnPressed: parent.clicked()
        }
        ToolTip.visible: ihov.containsMouse && tip !== ""
        ToolTip.text: tip
        ToolTip.delay: 500
    }

    /*======================= layout =========================*/
    Column {
        anchors.fill: parent
        anchors.margins: Theme.spaceM
        spacing: Theme.spaceM

        Row {
            width: parent.width; spacing: Theme.spaceS
            Text {
                text: qsTr("Library")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSizeTitle
                font.bold: true
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                text: lib.totalCount + qsTr(" types")
                color: Theme.textDisabled
                font.pixelSize: Theme.fontSizeSmall
                anchors.verticalCenter: parent.verticalCenter
            }
            Item { width: 1; height: 1 }
            IconBtn {
                text: "✚"
                tip: qsTr("New type — open the preset editor on a blank candidate")
                visible: lib.canAuthorTypes()
                onClicked: lib.newRequested()
            }
        }

        /* Search — same field chrome + Escape-eats-itself as the
           device tree. */
        TextField {
            id: searchField
            width: parent.width
            height: 28
            placeholderText: qsTr("Search types…")
            placeholderTextColor: Theme.textDisabled
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSizeBody
            onTextChanged: lib.search = text
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

        /* Category chips — derived from the merged listing. */
        Flow {
            width: parent.width
            spacing: Theme.spaceS
            Repeater {
                model: {
                    var cats = lib.categories()
                    var out = [{ label: qsTr("All"), cat: "" }]
                    for (var i = 0; i < cats.length; i++)
                        out.push({ label: cats[i], cat: cats[i] })
                    return out
                }
                delegate: Rectangle {
                    required property var modelData
                    height: 18
                    width: chipText.width + 12
                    radius: Theme.radiusS
                    color: lib.category === modelData.cat ? Theme.accentMuted
                           : (chipMa.containsMouse ? Theme.surfaceAlt
                                                   : Theme.bg)
                    border.color: lib.category === modelData.cat
                                  ? Theme.accent : Theme.borderStrong
                    Text {
                        id: chipText
                        anchors.centerIn: parent
                        text: modelData.label
                        color: lib.category === modelData.cat ? Theme.textPrimary
                                                              : Theme.textSecondary
                        font.pixelSize: Theme.fontSizeSmall
                    }
                    MouseArea {
                        id: chipMa
                        anchors.fill: parent; hoverEnabled: true
                        onClicked: lib.category = modelData.cat
                    }
                }
            }
        }

        /* Type rows + empty states — the state text overlays the
           (empty) list region so the footer keeps its slot. */
        Item {
            width: parent.width
            height: parent.height - y - footer.height - Theme.spaceM

        ListView {
            id: libList
            objectName: "libList"
            anchors.fill: parent
            clip: true
            model: lib.shown
            boundsBehavior: Flickable.StopAtBounds

            delegate: Rectangle {
                id: row
                required property var modelData
                width: libList.width
                height: 54
                radius: Theme.radiusS
                color: rowMa.containsMouse ? Theme.surfaceAlt : "transparent"

                property string typeId:   modelData.typeId   || ""
                property string rowName:  modelData.name     || typeId
                property string rowCat:   modelData.category || "misc"
                property bool   rowFav:   modelData.favorite === true
                property bool   fromFile: modelData.fromFile === true
                property bool   engineOnly: modelData.engineOnly === true
                property int    leds:     modelData.ledTotal || 0
                property var    bnd:      modelData.bounds

                function dimsText() {
                    if (!bnd || bnd.x === undefined)
                        return ""
                    var mm = function(v) { return Math.round(v * 1000) }
                    return mm(bnd.x) + "×" + mm(bnd.y) + "×"
                         + mm(bnd.z) + " mm"
                }

                /* Drag chip — a proxy reparented to `lib` so it
                   isn't clipped by the list; carries the type id the
                   canvas DropArea reads (drop.source.dropTypeId, with
                   the mimeData key as the fallback path). */
                Item {
                    id: dragChip
                    parent: lib
                    visible: rowMa.drag.active
                    width: chipLbl.implicitWidth + 18
                    height: 24
                    z: 99
                    property string dropTypeId: row.typeId
                    Drag.active: rowMa.drag.active
                    Drag.dragType: Drag.Automatic
                    Drag.supportedActions: Qt.CopyAction
                    Drag.keys: ["studio-device-type"]
                    Drag.source: dragChip
                    Drag.mimeData: ({ "studio-device-type": row.typeId })
                    Drag.hotSpot.x: width / 2
                    Drag.hotSpot.y: height / 2
                    Rectangle {
                        anchors.fill: parent
                        color: Theme.accentMuted; radius: Theme.radiusS
                        border.color: Theme.accent
                        Text {
                            id: chipLbl
                            anchors.centerIn: parent
                            text: row.rowName
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontSizeBody
                        }
                    }
                }

                MouseArea {
                    id: rowMa
                    anchors.fill: parent
                    hoverEnabled: true
                    drag.target: dragChip
                    drag.axis: Drag.XAndYAxis
                    drag.threshold: 6
                    onPressed: function(m) {
                        /* Park the chip under the cursor so the first
                           drag delta doesn't jump it. */
                        var p = mapToItem(lib, m.x, m.y)
                        dragChip.x = p.x - dragChip.width / 2
                        dragChip.y = p.y - dragChip.height / 2
                    }
                    onReleased: dragChip.Drag.drop()
                }

                Row {
                    x: Theme.spaceS; y: Theme.spaceS
                    width: parent.width - Theme.spaceM
                    height: parent.height - Theme.spaceM
                    spacing: Theme.spaceS

                    /* Glyph thumbnail — cheap category placeholder. */
                    Rectangle {
                        width: 34; height: 34; radius: Theme.radiusS
                        anchors.verticalCenter: parent.verticalCenter
                        color: Theme.bg
                        border.color: Theme.borderStrong
                        Text {
                            anchors.centerIn: parent
                            text: (row.rowCat.charAt(0) || "?").toUpperCase()
                            color: Theme.accent
                            font.pixelSize: Theme.fontSizeTitle
                            font.bold: true
                        }
                    }

                    Column {
                        width: parent.width - 34 - starBtn.width
                               - addBtn.width - editBtn.width
                               - delBtn.width - Theme.spaceS * 6
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 1
                        Text {
                            width: parent.width
                            text: row.rowName
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontSizeBody
                            elide: Text.ElideRight
                        }
                        Text {
                            width: parent.width
                            text: row.rowCat
                                + (row.dimsText() !== ""
                                   ? " · " + row.dimsText() : "")
                                + " · " + row.leds + qsTr(" LEDs")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                            elide: Text.ElideRight
                        }
                        /* Source badge — text + color, never color
                           alone. */
                        Text {
                            text: row.engineOnly ? qsTr("engine")
                                : (row.fromFile ? qsTr("file")
                                                : qsTr("packaged"))
                            color: row.engineOnly ? Theme.warn
                                 : (row.fromFile ? Theme.accent
                                                 : Theme.textDisabled)
                            font.pixelSize: Theme.fontSizeSmall
                        }
                    }

                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2
                        IconBtn {
                            id: starBtn
                            text: row.rowFav ? "★" : "☆"
                            active: row.rowFav
                            tip: row.rowFav ? qsTr("Remove from favorites")
                                            : qsTr("Pin to favorites")
                            onClicked: lib.toggleFav(row.typeId,
                                                     !row.rowFav)
                        }
                        IconBtn {
                            id: addBtn
                            text: "+"
                            tip: qsTr("Add to desk (viewport center)")
                            enabled: lib.hasBr() && !row.engineOnly
                            onClicked: lib.addAtCenter(row.typeId)
                        }
                    }
                    IconBtn {
                        id: editBtn
                        anchors.verticalCenter: parent.verticalCenter
                        text: "✎"
                        tip: qsTr("Edit type — dimensions, zones, layout")
                        visible: lib.canAuthorTypes()
                        onClicked: lib.editRequested(row.typeId)
                    }
                    IconBtn {
                        id: delBtn
                        anchors.verticalCenter: parent.verticalCenter
                        text: "✕"
                        tip: row.fromFile
                             ? qsTr("Delete this type's preset file")
                             : qsTr("Packaged type — nothing to delete")
                        visible: lib.canDeleteTypes()
                        enabled: row.fromFile && lib.hasBr()
                        onClicked: {
                            delTypeDlg.typeId = row.typeId
                            delTypeDlg.typeName = row.rowName
                            delTypeDlg.err = ""
                            delTypeDlg.open()
                        }
                    }
                }
            }
        }

            /* Empty states — empty registry vs. a filter that ate
               everything. */
            Column {
                visible: lib.shown.length === 0
                x: 0; y: 0
                width: parent.width
                spacing: Theme.spaceS
                Text {
                    text: lib.totalCount === 0
                          ? qsTr("No device types found.")
                          : qsTr("No types match the filter.")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSizeBody
                    width: parent.width
                    wrapMode: Text.WordWrap
                }
                Text {
                    visible: lib.totalCount === 0
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: qsTr("Preset files failed to load — check the diagnostics page for preset errors.")
                    color: Theme.textDisabled
                    font.pixelSize: Theme.fontSizeSmall
                }
            }
        }

        /*================ footer: type actions ================*/
        Column {
            id: footer
            width: parent.width
            spacing: Theme.spaceS
            visible: lib.canAuthorTypes()
            Rectangle { width: parent.width; height: 1; color: Theme.border }
            Text {
                text: {
                    var n = lib.selIds().length
                    return n === 0 ? qsTr("select desk instances to save types")
                        : n + qsTr(" instance") + (n > 1 ? "s" : "")
                          + qsTr(" selected")
                }
                color: Theme.textDisabled
                font.pixelSize: Theme.fontSizeSmall
            }
            LBtn {
                width: parent.width
                text: qsTr("Save variant…")
                tip: qsTr("Write the selected instance's type as a new preset and repoint the instance to it")
                enabled: lib.hasBr() && lib.selIds().length === 1
                onClicked: {
                    typeDlg.mode = "variant"
                    idField.text = ""
                    nameField.text = ""
                    typeDlg.open()
                }
            }
            LBtn {
                width: parent.width
                text: qsTr("Create preset from selection…")
                tip: qsTr("Write a child-reference type from the selected instances' arrangement")
                enabled: lib.hasBr() && lib.selIds().length >= 1
                onClicked: {
                    typeDlg.mode = "assembly"
                    idField.text = ""
                    nameField.text = ""
                    typeDlg.open()
                }
            }
        }
    }

    /* Shared new-type dialog — mode "variant" (repoints the single
       selected instance) or "assembly" (child-ref preset from the
       selection). Live validation above the buttons; the controller
       re-validates everything anyway. */
    Popup {
        id: typeDlg
        property string mode: "variant"     /* variant | assembly */
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        x: Math.round((lib.width - implicitWidth) / 2)
        y: Math.round(lib.height * 0.15)
        padding: Theme.spaceL
        background: Rectangle {
            color: Theme.surface; radius: Theme.radiusM
            border.color: Theme.borderStrong
        }
        onOpened: {
            errText.text = lib.dlgError(idField.text.trim(),
                                        nameField.text.trim())
            idField.forceActiveFocus()
        }

        component DlgBtn: Rectangle {
            property string text: ""
            property alias  ma: dma
            signal clicked()
            width: 72; height: 24; radius: Theme.radiusS
            color: dma.containsMouse ? Theme.surfaceAlt : Theme.bg
            border.color: dma.activeFocus ? Theme.accent : Theme.borderStrong
            opacity: enabled ? 1 : 0.4
            Text {
                anchors.centerIn: parent
                text: parent.text
                color: parent.enabled ? Theme.textPrimary : Theme.textDisabled
                font.pixelSize: Theme.fontSizeBody
            }
            MouseArea {
                id: dma; anchors.fill: parent; hoverEnabled: true
                enabled: parent.enabled
                activeFocusOnTab: true
                onClicked: parent.clicked()
                Keys.onSpacePressed:  parent.clicked()
                Keys.onReturnPressed: parent.clicked()
            }
        }

        contentItem: Column {
            spacing: Theme.spaceM
            Text {
                text: typeDlg.mode === "variant"
                      ? qsTr("Save variant") : qsTr("Create preset from selection")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSizeTitle
                font.bold: true
            }
            Text {
                text: typeDlg.mode === "variant"
                      ? qsTr("Copies the instance's type; the instance is repointed to the new id.")
                      : qsTr("One child-reference entity per selected instance — the arrangement becomes a type.")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSizeSmall
                width: 220; wrapMode: Text.WordWrap
            }
            Text {
                text: qsTr("Type id")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSizeSmall
            }
            TextField {
                id: idField
                width: 220; height: 26
                placeholderText: qsTr("e.g. fan-120-purple")
                placeholderTextColor: Theme.textDisabled
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSizeBody
                selectByMouse: true
                onTextChanged: errText.text = lib.dlgError(
                    text.trim(), nameField.text.trim())
                onAccepted: if (okBtn.enabled) okBtn.clicked()
                Keys.onShortcutOverride: function(e) {
                    if (e.key === Qt.Key_Escape)
                        e.accepted = true
                }
                Keys.onEscapePressed: typeDlg.close()
                background: Rectangle {
                    color: Theme.bg; radius: Theme.radiusS
                    border.color: idField.activeFocus ? Theme.accent
                                                      : Theme.borderStrong
                }
            }
            Text {
                text: qsTr("Display name")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSizeSmall
            }
            TextField {
                id: nameField
                width: 220; height: 26
                placeholderText: qsTr("e.g. 120 mm fan (purple)")
                placeholderTextColor: Theme.textDisabled
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSizeBody
                selectByMouse: true
                onTextChanged: errText.text = lib.dlgError(
                    idField.text.trim(), text.trim())
                onAccepted: if (okBtn.enabled) okBtn.clicked()
                Keys.onShortcutOverride: function(e) {
                    if (e.key === Qt.Key_Escape)
                        e.accepted = true
                }
                Keys.onEscapePressed: typeDlg.close()
                background: Rectangle {
                    color: Theme.bg; radius: Theme.radiusS
                    border.color: nameField.activeFocus ? Theme.accent
                                                        : Theme.borderStrong
                }
            }
            Text {
                id: errText
                width: 220; wrapMode: Text.WordWrap
                color: Theme.bad; font.pixelSize: Theme.fontSizeSmall
                visible: text !== ""
            }
            Row {
                spacing: Theme.spaceS
                DlgBtn {
                    id: okBtn
                    text: typeDlg.mode === "variant" ? qsTr("Save") : qsTr("Create")
                    enabled: errText.text === ""
                    onClicked: {
                        var sel = lib.selIds()
                        lib.submitNewType(typeDlg.mode,
                                          sel.length === 1 ? sel[0] : "",
                                          idField.text.trim(),
                                          nameField.text.trim())
                        lib.stamp++
                        typeDlg.close()
                    }
                }
                DlgBtn {
                    text: qsTr("Cancel")
                    onClicked: typeDlg.close()
                }
            }
        }
    }

    /* Delete-type confirmation — only file-backed rows reach this
       dialog. The controller re-checks instances + dependents on
       click; a refusal is shown inline and the file stays. */
    Popup {
        id: delTypeDlg
        property string typeId: ""
        property string typeName: ""
        property string err: ""
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        x: Math.round((lib.width - implicitWidth) / 2)
        y: Math.round(lib.height * 0.15)
        padding: Theme.spaceL
        background: Rectangle {
            color: Theme.surface; radius: Theme.radiusM
            border.color: Theme.borderStrong
        }
        onOpened: delConfirmBtn.forceActiveFocus()

        component DelBtn: Rectangle {
            property string text: ""
            property bool   danger: false
            signal clicked()
            width: 72; height: 24; radius: Theme.radiusS
            color: danger
                   ? (dma2.containsMouse ? Theme.accentMuted : Theme.bg)
                   : (dma2.containsMouse ? Theme.surfaceAlt : Theme.bg)
            border.color: danger ? Theme.bad
                                 : (dma2.activeFocus ? Theme.accent
                                                     : Theme.borderStrong)
            Text {
                anchors.centerIn: parent
                text: parent.text
                color: parent.danger ? Theme.bad : Theme.textPrimary
                font.pixelSize: Theme.fontSizeBody
            }
            MouseArea {
                id: dma2; anchors.fill: parent; hoverEnabled: true
                activeFocusOnTab: true
                onClicked: parent.clicked()
                Keys.onSpacePressed:  parent.clicked()
                Keys.onReturnPressed: parent.clicked()
            }
        }

        contentItem: Column {
            spacing: Theme.spaceM
            Text {
                text: qsTr("Delete type")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSizeTitle
                font.bold: true
            }
            Text {
                width: 280; wrapMode: Text.WordWrap
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSizeBody
                text: qsTr("Remove the preset file for '%1' (%2)?")
                      .arg(delTypeDlg.typeName || delTypeDlg.typeId)
                      .arg(delTypeDlg.typeId)
            }
            Text {
                width: 280; wrapMode: Text.WordWrap
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSizeSmall
                text: qsTr("Only your file is removed — a packaged default underneath (if any) takes over again.")
            }
            Text {
                width: 280; wrapMode: Text.WordWrap
                color: Theme.bad; font.pixelSize: Theme.fontSizeSmall
                visible: delTypeDlg.err !== ""
                text: delTypeDlg.err
            }
            Row {
                spacing: Theme.spaceS
                DelBtn {
                    id: delConfirmBtn
                    text: qsTr("Delete")
                    danger: true
                    onClicked: {
                        var e = lib.deleteType(delTypeDlg.typeId)
                        if (e === "")
                            delTypeDlg.close()
                        else
                            delTypeDlg.err = e
                    }
                }
                DelBtn {
                    text: qsTr("Cancel")
                    onClicked: delTypeDlg.close()
                }
            }
        }
    }
}
