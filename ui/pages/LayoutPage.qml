// SPDX-License-Identifier: GPL-2.0-or-later
//
// Layout — spatial model view (Task 4.4, plan section 5):
// case/desk arrangement over the applied SceneDocument joined
// with live topology snapshots. Every edit goes through
// layoutCtl -> EngineBridge -> LAYOUT_COMMAND / SET_SCENE; the
// engine owns the document. Mirrored fans render as linked
// copies (their own body, the source's output — LED counts
// never multiply). Undo/redo reverts the whole document —
// the honest inverse for ops like set_mirror that destroy
// emitter/binding data. Identification rides the engine's
// bounded blink overlay; calibration addresses are range-
// checked here AND on the engine.
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import OpenRGB2.UI.theme
import OpenRGB2.UI.components

Item {
    id: root
    objectName: "layoutPage"

    readonly property bool ctlUp:
        typeof layoutCtl !== "undefined"
    readonly property bool engineUp:
        typeof bridge !== "undefined" && bridge.connected
    readonly property bool editable:
        root.ctlUp && root.engineUp && layoutCtl.hasScene
        && !layoutCtl.busy

    /* items() / linkPairs() feed the canvas — both recompute
       whenever layoutCtl re-emits itemsChanged (its property
       reads are binding-tracked by QML). */
    property var itemList: []
    property var linkPairs: []
    property var view: ({ "s": 200, "ox": 20, "oz": 20,
                          "minX": 0, "minZ": 0 })

    function refreshItems() {
        itemList = root.ctlUp ? layoutCtl.items : [];
        computeView();
    }

    function computeView() {
        var its = itemList;
        var pairs = [];
        var minX = 1e9, maxX = -1e9, minZ = 1e9, maxZ = -1e9;
        var byId = {};
        for(var i = 0; i < its.length; ++i) {
            var o = its[i];
            byId[o.id] = o;
            minX = Math.min(minX, o.x - o.w / 2);
            maxX = Math.max(maxX, o.x + o.w / 2);
            minZ = Math.min(minZ, o.z - o.d / 2);
            maxZ = Math.max(maxZ, o.z + o.d / 2);
        }
        if(its.length === 0 || minX > maxX) {
            view = ({ "s": 200, "ox": 20, "oz": 20,
                      "minX": 0, "minZ": 0 });
            linkPairs = pairs;
            return;
        }
        var pad = 0.10;   /* meters of breathing room  */
        var w = Math.max(0.05, maxX - minX + 2 * pad);
        var h = Math.max(0.05, maxZ - minZ + 2 * pad);
        var cw = Math.max(1, canvasRect.width - 2 * Theme.spaceL);
        var ch = Math.max(1, canvasRect.height - 2 * Theme.spaceL);
        var s = Math.min(cw / w, ch / h);
        var ox = Theme.spaceL + (cw - s * w) / 2;
        var oz = Theme.spaceL + (ch - s * h) / 2;
        view = ({ "s": s, "ox": ox, "oz": oz,
                  "minX": minX - pad, "minZ": minZ - pad });
        for(var j = 0; j < its.length; ++j) {
            var li = its[j];
            if(li.kind !== "linked" || li.mirror_of === "")
                continue;
            var src = byId[li.mirror_of];
            if(!src)
                continue;
            pairs.push({
                "x1": ox + (li.x - view.minX) * s,
                "y1": oz + (li.z - view.minZ) * s,
                "x2": ox + (src.x - view.minX) * s,
                "y2": oz + (src.z - view.minZ) * s
            });
        }
        linkPairs = pairs;
    }

    function stateTone(o) {
        switch(o.state) {
        case "resolved":  return "ok";
        case "missing":   return "bad";
        case "ambiguous": return "warn";
        case "linked":    return "accent";
        default:          return "neutral";
        }
    }

    /* Whole-number parse for calibration/identify fields —
       blank means "not provided". -1 marks invalid. */
    function parseIdx(text) {
        if(text === undefined || text === null || text === "")
            return -1;
        var n = parseInt(text, 10);
        return (isNaN(n) || n < 0 || String(n) !== text.trim())
               ? -2 : n;
    }

    /* Live calibration-address check — the same bound
       LayoutEditModel::BuildCalibrateLed enforces, evaluated
       BEFORE a body exists so the refusal is immediate AND
       inline (Task 4.5: reject at every boundary; the field
       is the first). "" means sendable. */
    function addrErr(text) {
        var t = (text === undefined || text === null
                 ? "" : String(text)).trim();
        if(t === "")
            return qsTr("enter a whole-number LED address (-1 = render-only)");
        var n = parseInt(t, 10);
        if(isNaN(n) || String(n) !== t)
            return qsTr("LED address must be a whole number (-1 = render-only)");
        if(n < -1)
            return qsTr("LED address must be >= -1 (-1 = render-only)");
        if(root.ctlUp && layoutCtl.zoneBound <= 0)
            return qsTr("no bound zone LED count — only -1 (render-only) applies");
        if(root.ctlUp && n >= layoutCtl.zoneBound)
            return qsTr("address %1 is out of range — the bound zone has %2 LEDs")
                .arg(n).arg(layoutCtl.zoneBound);
        return "";
    }

    Connections {
        target: root.ctlUp ? layoutCtl : null
        function onItemsChanged() { root.refreshItems(); }
    }
    Component.onCompleted: refreshItems()
    onWidthChanged: computeView()
    onHeightChanged: computeView()

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
                title: qsTr("Layout")
                subtitle: qsTr("Physical arrangement, orientation, and LED identification")
            }

            /*---- toolbar: undo/redo + status -----------------*/
            RowLayout {
                width: parent.width - 2 * col.padding
                spacing: Theme.spaceL
                AppButton {
                    objectName: "undoButton"
                    text: root.ctlUp && layoutCtl.undoLabel !== ""
                          ? qsTr("Undo %1").arg(layoutCtl.undoLabel)
                          : qsTr("Undo")
                    enabled: root.ctlUp && layoutCtl.canUndo
                             && !layoutCtl.busy
                    onClicked: layoutCtl.undo()
                }
                AppButton {
                    objectName: "redoButton"
                    text: root.ctlUp && layoutCtl.redoLabel !== ""
                          ? qsTr("Redo %1").arg(layoutCtl.redoLabel)
                          : qsTr("Redo")
                    enabled: root.ctlUp && layoutCtl.canRedo
                             && !layoutCtl.busy
                    onClicked: layoutCtl.redo()
                }
                AppButton {
                    text: qsTr("Reload")
                    enabled: root.ctlUp && root.engineUp
                    onClicked: layoutCtl.reload()
                }
                Text {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignRight
                    text: root.ctlUp ? layoutCtl.statusText : ""
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSizeSmall
                    elide: Text.ElideRight
                }
            }
            Row {
                visible: root.ctlUp && layoutCtl.lastError !== ""
                spacing: Theme.spaceS
                width: parent.width - 2 * col.padding
                /* The error SOURCE is part of the state — a
                   local input refusal never reads as an
                   engine rejection (Task 4.5). */
                StatusPill {
                    id: layoutErrSrc
                    text: {
                        if(!root.ctlUp) return "";
                        switch(layoutCtl.lastErrorSource) {
                        case "engine":     return qsTr("engine rejected");
                        case "connection": return qsTr("not sent");
                        default:           return qsTr("refused before send");
                        }
                    }
                    tone: root.ctlUp
                          && layoutCtl.lastErrorSource === "engine"
                          ? "bad" : "warn"
                }
                Text {
                    width: parent.width - layoutErrSrc.width
                           - parent.spacing
                    text: root.ctlUp ? layoutCtl.lastError : ""
                    color: Theme.bad
                    font.pixelSize: Theme.fontSizeSmall
                    wrapMode: Text.WordWrap
                }
            }

            /*---- canvas + inspector ---------------------------*/
            Row {
                width: parent.width - 2 * col.padding
                spacing: Theme.spaceL

                Card {
                    title: qsTr("ARRANGEMENT")
                    width: parent.width - inspector.width
                           - Theme.spaceL

                    Item {
                        width: parent.width
                        height: 430

                        Rectangle {
                            id: canvasRect
                            anchors.fill: parent
                            radius: Theme.radiusM
                            color: Theme.bg
                            border.width: 1
                            border.color: Theme.border
                            clip: true
                            onWidthChanged: root.computeView()
                            onHeightChanged: root.computeView()

                            /* link lines: linked copy -> source */
                            Repeater {
                                model: root.linkPairs
                                Rectangle {
                                    width: Math.sqrt(
                                        (modelData.x2 - modelData.x1)
                                        * (modelData.x2 - modelData.x1)
                                        + (modelData.y2 - modelData.y1)
                                        * (modelData.y2 - modelData.y1))
                                    height: 2
                                    x: modelData.x1
                                    y: modelData.y1 - 1
                                    rotation: Math.atan2(
                                        modelData.y2 - modelData.y1,
                                        modelData.x2 - modelData.x1)
                                        * 180 / Math.PI
                                    transformOrigin: Item.Left
                                    color: Theme.accentMuted
                                }
                            }

                            /* the scene objects — real document
                               order, real transforms */
                            Repeater {
                                objectName: "canvasItems"
                                model: root.itemList
                                delegate: Item {
                                    id: d
                                    property var o: modelData
                                    property bool dragging: false
                                    visible: o.visible
                                             && o.kind !== "group"
                                    width: Math.max(10, o.w * view.s)
                                    height: Math.max(10, o.d * view.s)
                                    x: view.ox
                                       + (o.x - view.minX) * view.s
                                       - width / 2
                                    y: view.oz
                                       + (o.z - view.minZ) * view.s
                                       - height / 2
                                    rotation: o.ry

                                    Rectangle {
                                        anchors.fill: parent
                                        radius: Theme.radiusS
                                        color: d.o.kind === "decor"
                                               ? Theme.surfaceAlt
                                               : d.o.kind === "linked"
                                                 ? Theme.accentMuted
                                                 : Theme.surfaceHover
                                        border.width:
                                            root.ctlUp
                                            && layoutCtl.selectedId
                                               === d.o.id ? 2 : 1
                                        border.color:
                                            root.ctlUp
                                            && layoutCtl.selectedId
                                               === d.o.id
                                            ? Theme.accent
                                            : d.o.state === "missing"
                                              || d.o.state === "ambiguous"
                                              ? Theme.warn
                                              : Theme.borderStrong
                                        Text {
                                            anchors.centerIn: parent
                                            width: parent.width - 4
                                            horizontalAlignment:
                                                Text.AlignHCenter
                                            text: d.o.label
                                            color: Theme.textPrimary
                                            font.pixelSize:
                                                Theme.fontSizeSmall
                                            elide: Text.ElideRight
                                            wrapMode: Text.NoWrap
                                        }
                                        Rectangle {
                                            visible: d.o.kind
                                                     === "linked"
                                            anchors.top: parent.top
                                            anchors.right:
                                                parent.right
                                            anchors.margins: 2
                                            radius: Theme.radiusS
                                            color: Theme.accent
                                            width: tagText.width + 6
                                            height: tagText.height + 2
                                            Text {
                                                id: tagText
                                                text: qsTr("LINK")
                                                color: Theme.accentText
                                                font.pixelSize: 9
                                            }
                                        }
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        enabled: root.editable
                                        property real grabX: 0
                                        property real grabY: 0
                                        onPressed: function(m) {
                                            grabX = m.x;
                                            grabY = m.y;
                                            d.dragging = true;
                                            layoutCtl.select(d.o.id);
                                        }
                                        onPositionChanged: function(m) {
                                            if(!d.dragging) return;
                                            var p = mapToItem(
                                                canvasRect,
                                                m.x, m.y);
                                            d.x = Math.max(0, Math.min(
                                                canvasRect.width
                                                    - d.width,
                                                p.x - grabX));
                                            d.y = Math.max(0, Math.min(
                                                canvasRect.height
                                                    - d.height,
                                                p.y - grabY));
                                        }
                                        onReleased: {
                                            if(!d.dragging) return;
                                            d.dragging = false;
                                            var nx = (d.x + d.width / 2
                                                      - view.ox)
                                                     / view.s
                                                     + view.minX;
                                            var nz = (d.y + d.height / 2
                                                      - view.oz)
                                                     / view.s
                                                     + view.minZ;
                                            /* commit the meters —
                                               the engine's scene
                                               event repaints   */
                                            layoutCtl.moveObject(
                                                d.o.id, nx, nz);
                                            /* if the engine
                                               refused, snap back */
                                            d.x = Qt.binding(function() {
                                                return view.ox
                                                    + (d.o.x - view.minX)
                                                    * view.s
                                                    - d.width / 2; });
                                            d.y = Qt.binding(function() {
                                                return view.oz
                                                    + (d.o.z - view.minZ)
                                                    * view.s
                                                    - d.height / 2; });
                                        }
                                        onClicked: layoutCtl.select(d.o.id)
                                    }
                                }
                            }

                            EmptyState {
                                anchors.centerIn: parent
                                visible: root.itemList.length === 0
                                title: root.engineUp
                                       ? qsTr("No scene objects")
                                       : qsTr("Engine offline")
                                detail: root.engineUp
                                        ? qsTr("Apply a scene on the Lighting page — its objects arrange here.")
                                        : qsTr("The arrangement renders once the engine reports a scene.")
                            }
                        }
                        Text {
                            anchors.top: canvasRect.bottom
                            anchors.topMargin: Theme.spaceS
                            width: parent.width
                            text: qsTr("Top view, meters. Drag objects to arrange — edits apply immediately and are undoable. Linked copies (LINK) share their source's output.")
                            color: Theme.textDisabled
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                /*---- inspector -------------------------------*/
                Column {
                    id: inspector
                    width: 330
                    spacing: Theme.spaceL

                    Card {
                        title: qsTr("SELECTION")
                        width: parent.width
                        Column {
                            spacing: Theme.spaceM
                            width: parent.width - 2 * Theme.spaceL
                            property var sel: root.ctlUp
                                ? layoutCtl.selectedItem : ({})
                            visible: sel.id !== undefined
                            Text {
                                text: parent.sel.label || ""
                                color: Theme.textPrimary
                                font.pixelSize: Theme.fontSizeTitle
                                elide: Text.ElideRight
                                width: parent.width
                            }
                            RowLayout {
                                width: parent.width
                                spacing: Theme.spaceS
                                StatusPill {
                                    text: parent.parent.sel.kind || ""
                                    tone: "neutral"
                                }
                                StatusPill {
                                    visible:
                                        parent.parent.sel.state
                                        !== undefined
                                        && parent.parent.sel.state
                                           !== ""
                                    text: parent.parent.sel.state
                                          || ""
                                    tone: root.stateTone(
                                              parent.parent.sel)
                                }
                                StatusPill {
                                    visible: parent.parent.sel
                                             .connected === true
                                    text: qsTr("connected")
                                    tone: "ok"
                                }
                            }
                            Text {
                                visible: parent.sel.kind === "device"
                                text: qsTr("binding: %1\nzone: %2 · %3 LEDs\nemitters: %4 (%5 addressed)")
                                    .arg(parent.sel.binding || qsTr("none"))
                                    .arg(parent.sel.zone_name || qsTr("—"))
                                    .arg(parent.sel.zone_leds || 0)
                                    .arg(parent.sel.emitter_count || 0)
                                    .arg(parent.sel.addressed_count || 0)
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSizeSmall
                                wrapMode: Text.WordWrap
                                width: parent.width
                            }
                            Text {
                                visible: parent.sel.kind === "linked"
                                text: qsTr("mirrors: %1\nrenders the source's output — no extra LEDs")
                                    .arg(parent.sel.mirror_of || "")
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSizeSmall
                                wrapMode: Text.WordWrap
                                width: parent.width
                            }
                        }
                        EmptyState {
                            visible: !root.ctlUp
                                     || layoutCtl.selectedItem.id
                                        === undefined
                            anchors.horizontalCenter:
                                parent.horizontalCenter
                            title: qsTr("Nothing selected")
                            detail: qsTr("Click an object on the canvas.")
                        }
                    }

                    Card {
                        title: qsTr("ORIENTATION")
                        width: parent.width
                        visible: root.ctlUp
                                 && layoutCtl.selectedItem.id
                                    !== undefined
                        Column {
                            spacing: Theme.spaceM
                            width: parent.width - 2 * Theme.spaceL
                            RowLayout {
                                width: parent.width
                                spacing: Theme.spaceS
                                Text {
                                    text: qsTr("yaw %1°").arg(
                                        Math.round(
                                            (root.ctlUp
                                             ? layoutCtl.selectedItem
                                               .ry : 0) * 10) / 10)
                                    color: Theme.textPrimary
                                    font.pixelSize: Theme.fontSizeBody
                                    Layout.fillWidth: true
                                }
                                Repeater {
                                    model: [-45, -15, 15, 45]
                                    AppButton {
                                        text: (modelData > 0 ? "+" : "")
                                              + modelData + "°"
                                        enabled: root.editable
                                        onClicked: layoutCtl.rotateObject(
                                            layoutCtl.selectedId,
                                            layoutCtl.selectedItem.ry
                                            + modelData)
                                    }
                                }
                            }
                            AppButton {
                                text: qsTr("Reset to 0°")
                                enabled: root.editable
                                onClicked: layoutCtl.rotateObject(
                                    layoutCtl.selectedId, 0)
                            }
                        }
                    }

                    Card {
                        title: qsTr("MIRROR / LINK")
                        width: parent.width
                        visible: root.ctlUp
                                 && layoutCtl.selectedItem.id
                                    !== undefined
                                 && (layoutCtl.selectedItem.kind
                                     === "device"
                                     || layoutCtl.selectedItem.kind
                                        === "linked")
                        Column {
                            spacing: Theme.spaceM
                            width: parent.width - 2 * Theme.spaceL
                            Text {
                                visible: layoutCtl.selectedItem
                                         .kind === "linked"
                                text: qsTr("This object renders %1's output — its own binding and emitters are cleared while linked.")
                                    .arg(layoutCtl.selectedItem
                                         .mirror_of || "")
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSizeSmall
                                wrapMode: Text.WordWrap
                                width: parent.width
                            }
                            AppButton {
                                visible: layoutCtl.selectedItem
                                         .kind === "linked"
                                text: qsTr("Unlink")
                                enabled: root.editable
                                onClicked: layoutCtl.unlinkMirror(
                                    layoutCtl.selectedId)
                            }
                            ComboBox {
                                id: linkPick
                                visible: layoutCtl.selectedItem
                                         .kind === "device"
                                width: parent.width
                                model: root.ctlUp
                                       ? layoutCtl.linkSources : []
                                textRole: "label"
                                valueRole: "id"
                            }
                            AppButton {
                                visible: layoutCtl.selectedItem
                                         .kind === "device"
                                text: qsTr("Link as mirrored copy")
                                enabled: root.editable
                                    && linkPick.currentValue
                                       !== undefined
                                    && linkPick.currentValue !== ""
                                onClicked: layoutCtl.setMirrorTarget(
                                    layoutCtl.selectedId,
                                    linkPick.currentValue)
                            }
                            Text {
                                text: qsTr("Linking makes this object a copy of another's output — mirrored fans never double the addressable LED count.")
                                color: Theme.textDisabled
                                font.pixelSize: Theme.fontSizeSmall
                                wrapMode: Text.WordWrap
                                width: parent.width
                            }
                        }
                    }

                    Card {
                        title: qsTr("BINDING")
                        width: parent.width
                        visible: root.ctlUp
                                 && layoutCtl.selectedItem.id
                                    !== undefined
                                 && layoutCtl.selectedItem.kind
                                    === "device"
                        Column {
                            spacing: Theme.spaceM
                            width: parent.width - 2 * Theme.spaceL
                            ComboBox {
                                id: bindPick
                                width: parent.width
                                model: root.ctlUp
                                       ? layoutCtl.bindings : []
                                textRole: "id"
                                valueRole: "id"
                                displayText: currentValue !== undefined
                                    && currentValue !== ""
                                    ? qsTr("binding: %1")
                                        .arg(currentValue)
                                    : qsTr("pick a binding…")
                            }
                            RowLayout {
                                width: parent.width
                                spacing: Theme.spaceS
                                AppButton {
                                    text: qsTr("Assign")
                                    enabled: root.editable
                                        && bindPick.currentValue
                                           !== undefined
                                        && bindPick.currentValue !== ""
                                    onClicked: layoutCtl.assignBinding(
                                        layoutCtl.selectedId,
                                        bindPick.currentValue)
                                }
                                AppButton {
                                    text: qsTr("Unbind")
                                    enabled: root.editable
                                    onClicked: layoutCtl.assignBinding(
                                        layoutCtl.selectedId, "")
                                }
                            }
                            Text {
                                text: qsTr("Or bind a live controller — the engine derives the binding's identity from the enumerated topology:")
                                color: Theme.textDisabled
                                font.pixelSize: Theme.fontSizeSmall
                                wrapMode: Text.WordWrap
                                width: parent.width
                            }
                            ComboBox {
                                id: ctrlPick
                                width: parent.width
                                textRole: "label"
                                model: {
                                    var out = [];
                                    if(!root.engineUp) return out;
                                    var snaps = bridge.snapshots;
                                    for(var i = 0; i < snaps.length; ++i) {
                                        var s = snaps[i];
                                        var zs = s.zones || [];
                                        for(var j = 0; j < zs.length; ++j) {
                                            var k = {};
                                            for(var f in (s.key || {}))
                                                k[f] = s.key[f];
                                            k["zone"] = zs[j].name;
                                            out.push({
                                                "label": (s.name || "?")
                                                    + " / " + zs[j].name
                                                    + " (" + (zs[j].led_count
                                                        || 0) + ")",
                                                "key": k
                                            });
                                        }
                                    }
                                    return out;
                                }
                            }
                            AppButton {
                                text: qsTr("Bind selected controller")
                                enabled: root.editable
                                    && ctrlPick.currentIndex >= 0
                                onClicked: {
                                    var m = ctrlPick.model[
                                        ctrlPick.currentIndex];
                                    if(m) layoutCtl.bindNewController(
                                        layoutCtl.selectedId, "",
                                        m.key);
                                }
                            }
                        }
                    }

                    Card {
                        title: qsTr("CALIBRATION")
                        width: parent.width
                        visible: root.ctlUp
                                 && layoutCtl.selectedItem.id
                                    !== undefined
                                 && layoutCtl.selectedItem.kind
                                    === "device"
                                 && layoutCtl.selectedItem
                                    .emitter_count > 0
                        Column {
                            spacing: Theme.spaceM
                            width: parent.width - 2 * Theme.spaceL
                            Text {
                                text: root.ctlUp
                                    && layoutCtl.zoneBound > 0
                                    ? qsTr("Emitter → LED address inside the bound zone (%1 LEDs). -1 = render-only.")
                                        .arg(layoutCtl.zoneBound)
                                    : qsTr("No bound zone LED count — assign a binding to enable address calibration. (-1 = render-only is always allowed.)")
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSizeSmall
                                wrapMode: Text.WordWrap
                                width: parent.width
                            }
                            ListView {
                                width: parent.width
                                height: Math.min(200, contentHeight)
                                clip: true
                                model: root.ctlUp
                                       ? layoutCtl.selectedEmitters
                                       : []
                                delegate: Column {
                                    width: ListView.view.width
                                    spacing: 2
                                    RowLayout {
                                        width: parent.width
                                        spacing: Theme.spaceS
                                        Text {
                                            text: "#" + modelData.index
                                                  + (modelData.group
                                                     ? " · "
                                                       + modelData.group
                                                     : "")
                                            color: Theme.textSecondary
                                            font.pixelSize:
                                                Theme.fontSizeSmall
                                            Layout.preferredWidth: 90
                                            elide: Text.ElideRight
                                        }
                                        TextField {
                                            id: addrField
                                            Layout.fillWidth: true
                                            text: String(
                                                modelData.address)
                                            inputMethodHints:
                                                Qt.ImhDigitsOnly
                                            color: Theme.textPrimary
                                            font.pixelSize:
                                                Theme.fontSizeSmall
                                            background: Rectangle {
                                                radius: Theme.radiusS
                                                color: Theme.surfaceAlt
                                                border.width:
                                                    addrField.activeFocus
                                                    || root.addrErr(
                                                           addrField.text)
                                                           !== ""
                                                    ? 2 : 1
                                                border.color:
                                                    root.addrErr(
                                                        addrField.text)
                                                        !== ""
                                                    ? Theme.bad
                                                    : addrField.activeFocus
                                                      ? Theme.accent
                                                      : Theme.border
                                            }
                                            onEditingFinished: {
                                                /* refused HERE —
                                                   the inline error
                                                   stays until the
                                                   text is valid;
                                                   nothing is sent  */
                                                if(root.addrErr(text)
                                                   !== "")
                                                    return;
                                                /* range-checked in
                                                   the model BEFORE
                                                   send — and again
                                                   on the engine   */
                                                layoutCtl.calibrateEmitter(
                                                    layoutCtl.selectedId,
                                                    modelData.index,
                                                    parseInt(text, 10));
                                            }
                                        }
                                    }
                                    /* immediate inline error — the
                                       refusal is visible AT the
                                       field, not just the toolbar */
                                    Text {
                                        visible: root.addrErr(
                                                     addrField.text)
                                                 !== ""
                                        text: root.addrErr(
                                                  addrField.text)
                                        color: Theme.bad
                                        font.pixelSize:
                                            Theme.fontSizeSmall
                                        wrapMode: Text.WordWrap
                                        width: parent.width
                                    }
                                }
                            }
                        }
                    }
                }
            }

            /*---- LED identification ---------------------------*/
            Card {
                title: qsTr("LED IDENTIFICATION")
                width: parent.width - 2 * col.padding
                Column {
                    spacing: Theme.spaceM
                    width: parent.width - 2 * Theme.spaceL
                    Text {
                        text: qsTr("Blink a controller's LEDs through the engine's normal frame path — bounded, and never applied while an uploaded animation loop owns the group.")
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSizeBody
                        wrapMode: Text.WordWrap
                        width: parent.width
                    }
                    Repeater {
                        model: root.engineUp ? bridge.snapshots : []
                        delegate: Column {
                            width: parent.width
                            spacing: Theme.spaceS
                            property var snap: modelData
                            /* The led-index bound identify must
                               respect: the picked zone's count,
                               or the controller total on the
                               whole-controller scope. */
                            readonly property int ledBound: {
                                if(zonePick.currentIndex <= 0)
                                    return snap.led_count || 0;
                                var zs = snap.zones || [];
                                for(var i = 0; i < zs.length; ++i)
                                    if(zs[i].name === zonePick.zones[
                                                zonePick.currentIndex])
                                        return zs[i].led_count || 0;
                                return 0;
                            }
                            RowLayout {
                                width: parent.width
                                spacing: Theme.spaceL
                                Text {
                                    text: snap.name
                                          || qsTr("controller")
                                    color: Theme.textPrimary
                                    font.pixelSize:
                                        Theme.fontSizeBody
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                }
                                StatusPill {
                                    text: snap.connection
                                          || qsTr("unknown")
                                    tone: snap.connection
                                          === "connected"
                                          ? "ok" : "warn"
                                }
                            }
                            RowLayout {
                                width: parent.width
                                spacing: Theme.spaceS
                                ComboBox {
                                    id: zonePick
                                    Layout.preferredWidth: 220
                                    property var zones:
                                        ["(" + qsTr("whole controller")
                                         + ")"].concat(
                                            (snap.zones || []).map(
                                                function(z) {
                                                    return z.name; }))
                                    model: zones
                                }
                                TextField {
                                    id: ledField
                                    Layout.preferredWidth: 120
                                    placeholderText: qsTr("led #")
                                    inputMethodHints:
                                        Qt.ImhDigitsOnly
                                    color: Theme.textPrimary
                                    font.pixelSize:
                                        Theme.fontSizeSmall
                                    background: Rectangle {
                                        radius: Theme.radiusS
                                        color: Theme.surfaceAlt
                                        border.width: 1
                                        border.color: Theme.border
                                    }
                                }
                                TextField {
                                    id: durField
                                    Layout.preferredWidth: 90
                                    text: "3000"
                                    inputMethodHints:
                                        Qt.ImhDigitsOnly
                                    placeholderText: qsTr("ms")
                                    color: Theme.textPrimary
                                    font.pixelSize:
                                        Theme.fontSizeSmall
                                    background: Rectangle {
                                        radius: Theme.radiusS
                                        color: Theme.surfaceAlt
                                        border.width: 1
                                        border.color: Theme.border
                                    }
                                }
                                AppButton {
                                    text: qsTr("Identify")
                                    enabled: root.ctlUp
                                        && root.engineUp
                                        && snap.connection
                                           === "connected"
                                    onClicked: {
                                        var zone =
                                            zonePick.currentIndex > 0
                                            ? zonePick.zones[
                                                zonePick.currentIndex]
                                            : "";
                                        /* led # blank or <0 means
                                           whole scope — but a
                                           NON-NUMERIC field (-2) is
                                           refused, never silently
                                           remapped to a whole-
                                           controller blink. */
                                        var led = root.parseIdx(
                                            ledField.text);
                                        var dur = root.parseIdx(
                                            durField.text);
                                        if(led === -2 || dur === -2)
                                            return;
                                        /* an out-of-range led #
                                           is refused HERE — the
                                           engine's own range check
                                           is the second gate, not
                                           the only one */
                                        if(led >= 0 && ledBound > 0
                                           && led >= ledBound)
                                            return;
                                        if(dur < 0)    dur = 3000;
                                        layoutCtl.identify(
                                            snap.key, zone, led,
                                            dur);
                                    }
                                }
                                AppButton {
                                    text: qsTr("Stop")
                                    enabled: root.ctlUp
                                        && root.engineUp
                                        && layoutCtl.identifying
                                           .controller
                                           !== undefined
                                    onClicked: layoutCtl.cancelIdentify(
                                        snap.key)
                                }
                            }
                            /* the refusal is visible, not silent —
                               a mistyped or out-of-range field
                               never becomes a whole-controller
                               blink */
                            Text {
                                visible: {
                                    if(root.parseIdx(ledField.text)
                                       === -2
                                       || root.parseIdx(durField.text)
                                          === -2)
                                        return true;
                                    var led = root.parseIdx(
                                                  ledField.text);
                                    return led >= 0 && ledBound > 0
                                           && led >= ledBound;
                                }
                                text: {
                                    if(root.parseIdx(ledField.text)
                                       === -2
                                       || root.parseIdx(durField.text)
                                          === -2)
                                        return qsTr("led # and ms must be whole numbers");
                                    return qsTr("led # out of range — this scope has %1 LEDs")
                                        .arg(ledBound);
                                }
                                color: Theme.bad
                                font.pixelSize: Theme.fontSizeSmall
                            }
                        }
                    }
                    Text {
                        visible: root.ctlUp
                                 && layoutCtl.identifying.controller
                                    !== undefined
                        text: root.ctlUp
                            ? qsTr("identifying %1 %2 (%3 LEDs, %4 ms)")
                                .arg(layoutCtl.identifying.controller
                                     .device || "")
                                .arg(layoutCtl.identifying.zone || "")
                                .arg(layoutCtl.identifying.led_count
                                     || 0)
                                .arg(layoutCtl.identifying.duration_ms
                                     || 0)
                            : ""
                        color: Theme.accent
                        font.pixelSize: Theme.fontSizeSmall
                    }
                    EmptyState {
                        visible: !root.engineUp
                                 || bridge.snapshots.length === 0
                        anchors.horizontalCenter:
                            parent.horizontalCenter
                        title: qsTr("No devices")
                        detail: qsTr("Controllers appear here once the engine enumerates them.")
                    }
                }
            }
        }
    }
}
