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
import OpenRGB2.UI.studio

Item {
    id: root
    objectName: "layoutPage"

    readonly property bool ctlUp:
        typeof layoutCtl !== "undefined"
    /* studio-import Task 5: the scene/object editor surfaces
       (hierarchy tree, device library, transform inspector) mount
       below the canvas whenever the shared-draft session is up —
       objectCtl is absent in legacy/no-workspace contexts, and
       `ready` is false while no workspace draft is loaded. */
    readonly property bool studioUp:
        typeof objectCtl !== "undefined" && objectCtl.ready
    readonly property bool engineUp:
        typeof bridge !== "undefined" && bridge.connected
    /* A recovered local draft has no engine baseline
       (hasScene) and may exist with NO engine at all —
       workspace.ready is the honest "a draft is staged" gate
       for that case; the managed/applied paths keep
       hasScene + engineUp. */
    readonly property bool editable:
        root.ctlUp && !layoutCtl.busy
        && ((root.engineUp && layoutCtl.hasScene)
            || (typeof workspace !== "undefined"
                && workspace.ready))

    /* studio-import Task 7: the unsaved-draft preview feed —
       engine-evaluated colors for objects the draft carries,
       distinctly labelled and never confused with delivered
       output (frame_status). */
    readonly property bool previewUp:
        typeof viewport !== "undefined" && viewport.previewActive

    /* studio-import Task 8: the optional Quick3D viewport. The
       studio/viewport subtree only compiles into the module in
       the full-Studio build (main.cpp's quick3dAvailable); a
       failed Loader degrades honestly — the 2D canvas below is
       the always-available path and stays the default until the
       user toggles (the choice persists in uiPrefs). */
    readonly property bool quick3dUp:
        typeof quick3dAvailable !== "undefined"
        && quick3dAvailable === true
    /* stays a binding — uiPrefs.layoutShow3d holds the user's
       intent, quick3dUp gates display (fix round 1: assigning
       this would have killed the binding). */
    readonly property bool show3d:
        (typeof uiPrefs !== "undefined" ? uiPrefs.layoutShow3d : false)
        && quick3dUp
    /* The loaded viewport item (null in 2D mode / on load
       failure) — panels read editorCam/editorCtl off it. */
    property var viewport3d: null
    property bool viewportLoadFailed: false

    function setShow3d(on) {
        if(typeof uiPrefs !== "undefined")
            uiPrefs.layoutShow3d = on;
    }

    function draftColorsFor(id) {
        if(!root.previewUp) return [];
        const objs = viewport.objects || [];
        for(let i = 0; i < objs.length; ++i)
            if(objs[i].id === id)
                return objs[i].colors || [];
        return [];
    }

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
            /* Footprints center at (x+ox, z+oz) — Task 5 world-
               space items carry ox/oz (0 on legacy rows). */
            var fx = o.x + (o.ox || 0), fz = o.z + (o.oz || 0);
            minX = Math.min(minX, fx - o.w / 2);
            maxX = Math.max(maxX, fx + o.w / 2);
            minZ = Math.min(minZ, fz - o.d / 2);
            maxZ = Math.max(maxZ, fz + o.d / 2);
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

    /* Desk meters under the canvas' center — the device
       library's button-add targets this point; the drop handler
       maps exact pixels the same way. */
    function canvasCenter() {
        var px = canvasRect.width / 2;
        var pz = canvasRect.height / 2;
        return ({ "x": view.minX + (px - view.ox) / view.s,
                  "y": 0,
                  "z": view.minZ + (pz - view.oz) / view.s });
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

    /* Task 8 fix round 1: reconcile the two selection surfaces —
       the 3D viewport selects through objectCtl while the 2D
       canvas, SELECTION card and LED views read layoutCtl.
       Mirror the primary instance both ways (guarded against
       ping-pong; each select() also early-outs on no change). */
    property bool selSyncing: false
    Connections {
        target: root.studioUp ? objectCtl : null
        function onSelectionChanged() {
            if (root.selSyncing) return
            root.selSyncing = true
            const ids = objectCtl.selectedInstances
            layoutCtl.select(ids.length ? ids[ids.length - 1] : "")
            root.selSyncing = false
        }
    }
    Connections {
        target: root.studioUp ? layoutCtl : null
        function onSelectedChanged() {
            if (root.selSyncing) return
            root.selSyncing = true
            objectCtl.select(layoutCtl.selectedId)
            root.selSyncing = false
        }
    }
    Component.onCompleted: refreshItems()
    onWidthChanged: computeView()
    onHeightChanged: computeView()

    Flickable {
        objectName: "layoutScroll"
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
                    objectName: "reloadLayout"
                    text: qsTr("Reload")
                    enabled: root.ctlUp && root.engineUp
                    onClicked: layoutCtl.reload()
                }
                /* New scene applies an EMPTY document — a
                   staging state the engine accepts but never
                   snapshots (an object-less apply can't
                   clobber the last-scene file). Replacing a
                   populated scene needs a second click. */
                AppButton {
                    id: newSceneBtn
                    objectName: "newSceneButton"
                    property bool armed: false
                    text: armed
                          ? qsTr("Replace current scene — click again")
                          : qsTr("New scene")
                    enabled: root.ctlUp && root.engineUp
                             && !layoutCtl.busy
                    onClicked: {
                        if(root.itemList.length === 0 || armed) {
                            armed = false;
                            newSceneTimer.stop();
                            layoutCtl.newScene();
                        } else {
                            armed = true;
                            newSceneTimer.restart();
                        }
                    }
                    Timer {
                        id: newSceneTimer
                        interval: 4000
                        onTriggered: newSceneBtn.armed = false
                    }
                }
                AppButton {
                    objectName: "addAllDevicesButton"
                    text: qsTr("Add all devices")
                    enabled: root.ctlUp && root.engineUp
                             && layoutCtl.hasScene
                             && !layoutCtl.busy
                    onClicked: layoutCtl.addAllDevices()
                }
                AppButton {
                    objectName: "wsBundleButton"
                    /* Task 9: portable workspace bundle — inspect
                       + conflict-resolve an import, or stage the
                       export download. Gates on `available`, not
                       `ready`: import is also the FIRST-workspace
                       bootstrap (a fresh id + 0/0 revisions creates
                       it), so it must be clickable before any
                       managed draft exists. */
                    text: qsTr("Import / Export…")
                    enabled: typeof workspace !== "undefined"
                             && workspace.available
                    onClicked: bundleDlg.open()
                }
                AppButton {
                    objectName: "wsActionsButton"
                    /* Task 10: the remaining workspace file
                       actions — open/save-copy/reload/restore
                       backup/reset/open folder. `available` (not
                       `managed`): the workspace list populates
                       whenever the surface is connected, and Open
                       is the path that adopts a managed workspace
                       that isn't current — e.g. the first import. */
                    text: qsTr("Workspace…")
                    enabled: typeof workspace !== "undefined"
                             && workspace.available
                    onClicked: wsActionsDlg.open()
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
                    objectName: "arrangementCard"
                    width: parent.width - inspector.width
                           - Theme.spaceL

                    /* The instruction line lives BELOW the canvas —
                       the wrapper's height must cover both, or the
                       text paints past the card's reserved area and
                       the next card covers it. */
                    Item {
                        width: parent.width
                        height: viewBar.implicitHeight + Theme.spaceS
                                + 430 + Theme.spaceS
                                + canvasHint.implicitHeight
                                + (draftPreviewStatus.visible
                                   ? draftPreviewStatus.implicitHeight
                                     + Theme.spaceXS : 0)

                        /* studio-import Task 8: the 2D/3D view
                           toggle + the 3D-only render controls.
                           Hidden entirely without Quick3D — the 2D
                           canvas never degrades. */
                        Row {
                            id: viewBar
                            objectName: "viewBar"
                            visible: root.quick3dUp
                            height: visible ? implicitHeight : 0
                            spacing: Theme.spaceS
                            AppButton {
                                objectName: "view2dButton"
                                text: qsTr("2D")
                                checkable: true
                                checked: !root.show3d
                                onClicked: root.setShow3d(false)
                            }
                            AppButton {
                                objectName: "view3dButton"
                                text: qsTr("3D")
                                checkable: true
                                checked: root.show3d
                                onClicked: root.setShow3d(true)
                            }
                            /* Render quality tier — persisted as
                               meta.render.quality on the draft
                               (workspace.renderQuality) with the
                               UiSettings shell fallback; routed
                               through the viewport's setter. */
                            AppComboBox {
                                id: qualityPick
                                objectName: "viewportQualityPick"
                                Accessible.name:
                                    qsTr("3D render quality")
                                visible: root.show3d
                                width: 130
                                model: ["low", "balanced", "high"]
                                currentIndex: root.viewport3d
                                    ? Math.max(0, model.indexOf(
                                          root.viewport3d.qualityTier))
                                    : 1
                                onActivated: function(i) {
                                    if(root.viewport3d)
                                        root.viewport3d.setQualityTier(
                                            model[i]);
                                }
                            }
                            /* Case-shell ghosting — a display-only
                               shell pref (UiSettings), never a
                               document edit; the shell stays
                               non-pickable either way. */
                            AppCheckBox {
                                objectName: "viewportGhostCheck"
                                text: qsTr("ghost case")
                                visible: root.show3d
                                checked: typeof uiPrefs !== "undefined"
                                         && uiPrefs.viewportCaseGhost
                                onToggled: {
                                    if(typeof uiPrefs !== "undefined")
                                        uiPrefs.viewportCaseGhost =
                                            checked;
                                }
                            }
                            /* Bloom — meta.render.bloom on the
                               draft with the UiSettings fallback;
                               display-only, never delivered to
                               hardware. */
                            AppCheckBox {
                                objectName: "viewportBloomCheck"
                                text: qsTr("bloom")
                                visible: root.show3d
                                checked: {
                                    if(root.studioUp
                                       && typeof workspace
                                          !== "undefined"
                                       && workspace.ready)
                                        return workspace.renderBloom;
                                    return typeof uiPrefs
                                           !== "undefined"
                                           && uiPrefs.viewportBloom;
                                }
                                onToggled: {
                                    if(root.studioUp
                                       && typeof workspace
                                          !== "undefined"
                                       && workspace.ready)
                                        workspace.renderBloom = checked;
                                    if(typeof uiPrefs !== "undefined")
                                        uiPrefs.viewportBloom = checked;
                                }
                            }
                            /* Reduced motion — meta.ui.reduced_motion
                               on the draft (display-only temporal-AA
                               gate) with the UiSettings fallback;
                               same write-through shape as bloom. */
                            AppCheckBox {
                                objectName: "viewportMotionCheck"
                                text: qsTr("reduced motion")
                                visible: root.show3d
                                checked: {
                                    if(root.studioUp
                                       && typeof workspace
                                          !== "undefined"
                                       && workspace.ready)
                                        return workspace.reducedMotion;
                                    return typeof uiPrefs
                                           !== "undefined"
                                           && uiPrefs.viewportReducedMotion;
                                }
                                onToggled: {
                                    if(root.studioUp
                                       && typeof workspace
                                          !== "undefined"
                                       && workspace.ready)
                                        workspace.reducedMotion = checked;
                                    if(typeof uiPrefs !== "undefined")
                                        uiPrefs.viewportReducedMotion =
                                            checked;
                                }
                            }
                        }

                        /* studio-import Task 8: the Quick3D
                           viewport, lazily created — every
                           collaborator is wired EXPLICITLY (the
                           shared objectCtl/workspace/viewport
                           context surfaces), never a global inside
                           the subtree. A load failure keeps the
                           page on the 2D path with an honest
                           message. */
                        Loader {
                            id: viewportLoader
                            objectName: "viewportLoader"
                            anchors {
                                left: parent.left
                                right: parent.right
                                top: viewBar.bottom
                                topMargin: Theme.spaceS
                            }
                            height: 430
                            clip: true
                            active: root.quick3dUp && root.show3d
                            source: Qt.resolvedUrl(
                                "../studio/viewport/StudioViewport.qml")
                            visible: status === Loader.Ready
                            onStatusChanged: {
                                if(status === Loader.Error)
                                    root.viewportLoadFailed = true;
                            }
                            onLoaded: {
                                item.objectCtl =
                                    (typeof objectCtl !== "undefined")
                                    ? objectCtl : null;
                                item.workspaceCtl =
                                    (typeof workspace !== "undefined")
                                    ? workspace : null;
                                item.viewport =
                                    (typeof viewport !== "undefined")
                                    ? viewport : null;
                                item.prefs =
                                    (typeof uiPrefs !== "undefined")
                                    ? uiPrefs : null;
                                item.focusPeer = transformInspector;
                                item.focusPeer2 = deviceTree;
                                item.focusPeer3 = deviceLibrary;
                                /* LIVE = the transport-accepted
                                   frame_status feed — draft preview
                                   colors are labelled separately,
                                   never hardware output. */
                                item.outputLive = Qt.binding(
                                    function() {
                                        if(!root.engineUp)
                                            return false;
                                        const ag =
                                            bridge.frameAggregate;
                                        return !!(ag
                                                  && ag.mean_interval_s
                                                     > 0);
                                    });
                                root.viewport3d = item;
                            }
                            onActiveChanged: {
                                if(!active)
                                    root.viewport3d = null;
                            }
                        }
                        Text {
                            anchors.fill: viewportLoader
                            visible: root.show3d
                                     && (root.viewportLoadFailed
                                         || viewportLoader.status
                                            === Loader.Error)
                            text: qsTr("3D viewport unavailable — Quick3D did not load. The 2D view stays fully functional.")
                            color: Theme.warn
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        Rectangle {
                            id: canvasRect
                            anchors {
                                left: parent.left
                                right: parent.right
                                top: viewBar.bottom
                                topMargin: Theme.spaceS
                            }
                            height: 430
                            visible: !root.show3d
                                     || root.viewport3d === null
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
                                    objectName: "canvasObj_" + o.id
                                    property var o: modelData
                                    property bool dragging: false
                                    /* Keyboard parity with drag
                                       (Task 4.8): Tab reaches the
                                       object, arrows nudge it
                                       through the SAME moveObject
                                       path (applied, undoable,
                                       engine-acknowledged — a
                                       refused nudge snaps back like
                                       a refused drag), Space/Enter
                                       selects. */
                                    activeFocusOnTab:
                                        root.editable && visible
                                    Accessible.name:
                                        qsTr("Layout object %1")
                                            .arg(d.o.label || d.o.id)
                                    Accessible.role: Accessible.Button
                                    Keys.onPressed: e => {
                                        const step =
                                            (e.modifiers
                                             & Qt.ShiftModifier)
                                            ? 0.01 : 0.1;
                                        var nx = d.o.x, nz = d.o.z;
                                        switch(e.key) {
                                        case Qt.Key_Left:  nx -= step; break;
                                        case Qt.Key_Right: nx += step; break;
                                        case Qt.Key_Up:    nz -= step; break;
                                        case Qt.Key_Down:  nz += step; break;
                                        case Qt.Key_Space:
                                        case Qt.Key_Return:
                                        case Qt.Key_Enter:
                                            if(root.editable)
                                                layoutCtl.select(d.o.id);
                                            e.accepted = true;
                                            return;
                                        default:
                                            return;
                                        }
                                        if(root.editable) {
                                            layoutCtl.select(d.o.id);
                                            layoutCtl.moveObject(
                                                d.o.id, nx, nz);
                                        }
                                        e.accepted = true;
                                    }
                                    visible: o.visible
                                             && o.kind !== "group"
                                    width: Math.max(10, o.w * view.s)
                                    height: Math.max(10, o.d * view.s)
                                    /* Task 5: workspace rows carry
                                       a world-space footprint —
                                       (ox,oz) is its center offset
                                       from the instance origin and
                                       the box is already world-
                                       aligned, so it must not take
                                       the authored ry again. */
                                    x: view.ox
                                       + (o.x + (o.ox || 0)
                                          - view.minX) * view.s
                                       - width / 2
                                    y: view.oz
                                       + (o.z + (o.oz || 0)
                                          - view.minZ) * view.s
                                       - height / 2
                                    rotation: (o.ox !== undefined
                                               && o.w > 0)
                                              ? 0 : o.ry

                                    Rectangle {
                                        anchors.fill: parent
                                        radius: Theme.radiusS
                                        color: d.o.kind === "decor"
                                               ? Theme.surfaceAlt
                                               : d.o.kind === "linked"
                                                 ? Theme.accentMuted
                                                 : Theme.surfaceHover
                                        border.width:
                                            (root.ctlUp
                                             && layoutCtl.selectedId
                                                === d.o.id)
                                            || d.activeFocus ? 2 : 1
                                        border.color:
                                            (root.ctlUp
                                             && layoutCtl.selectedId
                                                === d.o.id)
                                            || d.activeFocus
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
                                        /* Task 7: the draft's
                                           evaluated colors under the
                                           object — unsaved preview
                                           data, labelled at the page
                                           level; never presented as
                                           delivered output. */
                                        FrameStrip {
                                            anchors {
                                                left: parent.left
                                                right: parent.right
                                                bottom: parent.bottom
                                                margins: 2
                                            }
                                            visible: root.previewUp
                                            colors:
                                                root.draftColorsFor(
                                                    d.o.id)
                                            maxSwatches: 12
                                        }
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        enabled: root.editable
                                        /* The page Flickable would
                                           otherwise steal the drag
                                           past its threshold and
                                           scroll the page instead of
                                           moving the object. */
                                        preventStealing: true
                                        property real grabX: 0
                                        property real grabY: 0
                                        /* F09: press marks the drag
                                           intent but NOT the drag —
                                           only an actual position
                                           change arms the move
                                           commit. A plain click is
                                           select-only and must never
                                           reach moveObject with its
                                           unchanged position (the
                                           editor used to surface
                                           that as "edit refused"). */
                                        property bool dragMoved: false
                                        /* Task 5 fix: the inspector's
                                           Paint tool turns clicks into
                                           paintObject(paintColor) and
                                           drags stay inert so a paint
                                           stroke can't move geometry. */
                                        readonly property bool paintMode:
                                            root.studioUp
                                            && objectCtl.tool === 2
                                        cursorShape: paintMode
                                                     ? Qt.CrossCursor
                                                     : Qt.ArrowCursor
                                        onPressed: function(m) {
                                            if(paintMode) {
                                                objectCtl.paintObject(
                                                    d.o.id,
                                                    objectCtl.paintColor);
                                                return;
                                            }
                                            grabX = m.x;
                                            grabY = m.y;
                                            dragMoved = false;
                                            d.dragging = true;
                                            layoutCtl.select(d.o.id);
                                        }
                                        onPositionChanged: function(m) {
                                            if(!d.dragging) return;
                                            var p = mapToItem(
                                                canvasRect,
                                                m.x, m.y);
                                            var nx = Math.max(0, Math.min(
                                                canvasRect.width
                                                    - d.width,
                                                p.x - grabX));
                                            var ny = Math.max(0, Math.min(
                                                canvasRect.height
                                                    - d.height,
                                                p.y - grabY));
                                            if(nx !== d.x || ny !== d.y) {
                                                dragMoved = true;
                                                d.x = nx;
                                                d.y = ny;
                                            }
                                        }
                                        onReleased: {
                                            if(!d.dragging) return;
                                            d.dragging = false;
                                            /* Selection already
                                               happened on press —
                                               a release without a
                                               drag is done. */
                                            if(!dragMoved) return;
                                            /* The rect centers on the
                                               footprint (origin + ox/oz)
                                               — the move target is the
                                               ORIGIN, so subtract the
                                               offset back out. */
                                            var nx = (d.x + d.width / 2
                                                      - view.ox)
                                                     / view.s
                                                     + view.minX
                                                     - (d.o.ox || 0);
                                            var nz = (d.y + d.height / 2
                                                      - view.oz)
                                                     / view.s
                                                     + view.minZ
                                                     - (d.o.oz || 0);
                                            /* snapEnabled quantizes the
                                               commit to the 10mm grid
                                               (objectCtl.snapMeters is
                                               the pass-through when off). */
                                            if(root.studioUp) {
                                                nx = objectCtl.snapMeters(nx);
                                                nz = objectCtl.snapMeters(nz);
                                            }
                                            /* commit the meters —
                                               the engine's scene
                                               event repaints   */
                                            layoutCtl.moveObject(
                                                d.o.id, nx, nz);
                                            /* if the engine
                                               refused, snap back */
                                            rebind();
                                        }
                                        /* A grab taken away mid-drag
                                           (focus loss, popup) drops
                                           the uncommitted move. */
                                        onCanceled: {
                                            if(!d.dragging) return;
                                            d.dragging = false;
                                            dragMoved = false;
                                            rebind();
                                        }
                                        function rebind() {
                                            d.x = Qt.binding(function() {
                                                return view.ox
                                                    + (d.o.x + (d.o.ox || 0)
                                                       - view.minX)
                                                    * view.s
                                                    - d.width / 2; });
                                            d.y = Qt.binding(function() {
                                                return view.oz
                                                    + (d.o.z + (d.o.oz || 0)
                                                       - view.minZ)
                                                    * view.s
                                                    - d.height / 2; });
                                        }
                                        /* Paint already landed on
                                           press — don't select on
                                           release. */
                                        onClicked: if(!paintMode)
                                            layoutCtl.select(d.o.id)
                                    }
                                }
                            }

                            /* Device-library drag target — rows
                               drag "studio-device-type" with the
                               type id on drag.source.dropTypeId
                               (mimeData string is the fallback);
                               the pixel maps to desk meters and
                               the add commits through the shared
                               session — one undo record. */
                            DropArea {
                                anchors.fill: parent
                                keys: ["studio-device-type"]
                                onDropped: function(drop) {
                                    if(!root.studioUp) {
                                        drop.accepted = false;
                                        return;
                                    }
                                    var tid = "";
                                    if(drop.source
                                       && drop.source.dropTypeId
                                              !== undefined)
                                        tid = drop.source.dropTypeId;
                                    else
                                        tid = drop.getDataAsString(
                                                  "studio-device-type");
                                    if(tid === "") {
                                        drop.accepted = false;
                                        return;
                                    }
                                    var x = view.minX
                                        + (drop.x - view.ox)
                                          / view.s;
                                    var z = view.minZ
                                        + (drop.y - view.oz)
                                          / view.s;
                                    objectCtl.addDeviceInstance(
                                        tid, x, 0, z);
                                    drop.accept(Qt.CopyAction);
                                }
                            }

                            Column {
                                anchors.centerIn: parent
                                visible: root.itemList.length === 0
                                spacing: Theme.spaceM
                                EmptyState {
                                    anchors.horizontalCenter:
                                        parent.horizontalCenter
                                    title: root.engineUp
                                           ? (root.ctlUp
                                              && layoutCtl.hasScene
                                              ? qsTr("No scene objects")
                                              : qsTr("No scene"))
                                           : qsTr("Engine offline")
                                    detail: root.engineUp
                                            ? (root.ctlUp
                                               && layoutCtl.hasScene
                                               ? qsTr("Add detected devices from the ADD DEVICES card — they become scene objects here.")
                                               : qsTr("Create a scene to start arranging detected devices."))
                                            : qsTr("The arrangement renders once the engine reports a scene.")
                                }
                                AppButton {
                                    objectName: "emptyNewScene"
                                    anchors.horizontalCenter:
                                        parent.horizontalCenter
                                    text: qsTr("New scene")
                                    visible: root.ctlUp
                                             && root.engineUp
                                    enabled: !layoutCtl.busy
                                    onClicked: layoutCtl.newScene()
                                }
                            }
                        }
                        Text {
                            id: canvasHint
                            objectName: "canvasInstructions"
                            anchors.top: canvasRect.bottom
                            anchors.topMargin: Theme.spaceS
                            width: parent.width
                            text: root.show3d
                                  ? qsTr("3D view. Click selects · drag orbits · Ctrl+drag moves/rotates · Shift+drag marquee · MMB/Space pans · wheel zooms · W/E/P tools · F/Home frame · Esc cancels. Edits are undoable (Ctrl+Z / Ctrl+Shift+Z). Emitter dots show the unsaved draft preview — not delivered output.")
                                  : qsTr("Top view, meters. Drag objects to arrange — or Tab to an object and nudge with the arrow keys (Shift for fine steps). Edits apply immediately and are undoable (Ctrl+Z / Ctrl+Shift+Z). Linked copies (LINK) share their source's output.")
                            color: Theme.textDisabled
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                        }
                        /* Draft-preview status — the honest label
                           (unsaved / paused / reduced) so colors on
                           objects are never mistaken for hardware
                           output. */
                        Text {
                            id: draftPreviewStatus
                            objectName: "draftPreviewStatus"
                            anchors.top: canvasHint.bottom
                            anchors.topMargin: Theme.spaceXS
                            width: parent.width
                            visible: root.previewUp
                            text: root.previewUp ? viewport.label : ""
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
                            width: parent.width
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

                    /*---- ADD DEVICES: live topology -> scene
                           objects (LAYOUT_COMMAND add_object).
                           Rows come from the engine's snapshot
                           feed — never fabricated. Adding needs
                           a scene: the engine answers
                           Unavailable honestly if none is
                           loaded, so the buttons gate on
                           hasScene and the hint explains. ----*/
                    Card {
                        title: qsTr("ADD DEVICES")
                        objectName: "addDevicesCard"
                        width: parent.width
                        visible: root.engineUp
                        Column {
                            spacing: Theme.spaceS
                            width: parent.width
                            Text {
                                visible: root.ctlUp
                                         && !layoutCtl.hasScene
                                text: qsTr("Create a scene first — detected devices add as scene objects.")
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSizeSmall
                                wrapMode: Text.WordWrap
                                width: parent.width
                            }
                            AppButton {
                                objectName: "addAllCard"
                                text: qsTr("Add all devices")
                                enabled: root.ctlUp
                                         && layoutCtl.hasScene
                                         && !layoutCtl.busy
                                onClicked: layoutCtl.addAllDevices()
                            }
                            Repeater {
                                model: root.engineUp
                                       ? bridge.snapshots : []
                                delegate: Column {
                                    id: devGroup
                                    required property var modelData
                                    readonly property var snap:
                                        modelData
                                    width: parent.width
                                    spacing: Theme.spaceS
                                    Repeater {
                                        model: devGroup.snap.zones
                                               || []
                                        delegate: RowLayout {
                                            id: zoneRow
                                            required property var modelData
                                            required property int index
                                            width: parent.width
                                            spacing: Theme.spaceS
                                            Column {
                                                Layout.fillWidth: true
                                                spacing: 0
                                                Text {
                                                    width: parent.width
                                                    text: (devGroup.snap.name
                                                           || devGroup.snap.key.device
                                                           || qsTr("device"))
                                                          + " · "
                                                          + zoneRow.modelData.name
                                                    color: Theme.textPrimary
                                                    font.pixelSize:
                                                        Theme.fontSizeBody
                                                    elide: Text.ElideRight
                                                }
                                                Text {
                                                    width: parent.width
                                                    text: qsTr("%1 LEDs · %2")
                                                        .arg(zoneRow.modelData
                                                             .led_count || 0)
                                                        .arg(devGroup.snap
                                                             .connection
                                                             || qsTr("unknown"))
                                                    color: Theme.textDisabled
                                                    font.pixelSize:
                                                        Theme.fontSizeSmall
                                                    elide: Text.ElideRight
                                                }
                                            }
                                            AppButton {
                                                objectName:
                                                    "addZone_" + zoneRow.index
                                                text: qsTr("Add")
                                                enabled: root.ctlUp
                                                    && layoutCtl.hasScene
                                                    && !layoutCtl.busy
                                                onClicked: {
                                                    /* fresh key
                                                       object — never
                                                       mutate the
                                                       snapshot map */
                                                    var src =
                                                        devGroup.snap.key
                                                        || ({});
                                                    layoutCtl.addObject({
                                                        "device":
                                                            src.device || "",
                                                        "serial":
                                                            src.serial || "",
                                                        "location":
                                                            src.location || "",
                                                        "zone":
                                                            zoneRow.modelData
                                                                .name || ""
                                                    });
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            Text {
                                visible: root.engineUp
                                         && bridge.snapshots.length === 0
                                text: qsTr("No devices detected — connect hardware or check the backend.")
                                color: Theme.textDisabled
                                font.pixelSize: Theme.fontSizeSmall
                                wrapMode: Text.WordWrap
                                width: parent.width
                            }
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
                            width: parent.width
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
                                        objectName: "rotStep_" + index
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
                                objectName: "rotateReset"
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
                            width: parent.width
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
                                objectName: "unlinkMirror"
                                visible: layoutCtl.selectedItem
                                         .kind === "linked"
                                text: qsTr("Unlink")
                                enabled: root.editable
                                onClicked: layoutCtl.unlinkMirror(
                                    layoutCtl.selectedId)
                            }
                            AppComboBox {
                                id: linkPick
                                objectName: "linkPick"
                                Accessible.name: qsTr("mirror source")
                                visible: layoutCtl.selectedItem
                                         .kind === "device"
                                width: parent.width
                                model: root.ctlUp
                                       ? layoutCtl.linkSources : []
                                textRole: "label"
                                valueRole: "id"
                            }
                            AppButton {
                                objectName: "linkMirror"
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
                            width: parent.width
                            AppComboBox {
                                id: bindPick
                                objectName: "bindPick"
                                Accessible.name: qsTr("scene binding")
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
                                    objectName: "assignBinding"
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
                                    objectName: "unbindObject"
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
                            AppComboBox {
                                id: ctrlPick
                                objectName: "ctrlPick"
                                Accessible.name: qsTr("live controller to bind")
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
                                objectName: "bindController"
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
                            width: parent.width
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
                                        /* Paint tool: per-emitter
                                           target — the row's `object`
                                           + `local` index name the
                                           emitter inside its resolved
                                           object. */
                                        Rectangle {
                                            id: emitPaint
                                            objectName: "emitPaint_"
                                                        + modelData.index
                                            visible: root.studioUp
                                                     && objectCtl.tool
                                                        === 2
                                            Layout.preferredWidth: 16
                                            Layout.preferredHeight: 16
                                            Layout.alignment:
                                                Qt.AlignVCenter
                                            radius: Theme.radiusS
                                            color: root.studioUp
                                                   ? objectCtl.paintColor
                                                   : "transparent"
                                            border.width: 1
                                            border.color:
                                                Theme.borderStrong
                                            ToolTip.visible:
                                                emitPaintMa.containsMouse
                                            ToolTip.text:
                                                qsTr("Paint this emitter")
                                            ToolTip.delay: 400
                                            MouseArea {
                                                id: emitPaintMa
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                onClicked:
                                                    objectCtl.paintEmitter(
                                                        modelData.object,
                                                        modelData.local
                                                        !== undefined
                                                        ? modelData.local
                                                        : modelData.index,
                                                        objectCtl.paintColor)
                                            }
                                        }
                                        TextField {
                                            id: addrField
                                            objectName: "ledAddr_" + modelData.index
                                            Accessible.name:
                                                qsTr("emitter %1 LED address")
                                                    .arg(modelData.index)
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

            /*---- Scene objects (studio-import Task 5) ---------*/
            /* One card carrying the adapted Desktop Lighting
               Studio surfaces against the shared workspace
               draft: the hierarchy tree owns selection and
               structural ops, the transform inspector owns
               numeric edits, the library adds instances —
               everything commits through objectCtl → the one
               WorkspaceEditSession, so undo groups these edits
               with the canvas drags above. Hidden (not
               disabled) until a draft is staged — the legacy
               canvas + binding cards keep working offline. */
            Card {
                title: qsTr("SCENE OBJECTS")
                width: parent.width - 2 * col.padding
                visible: root.studioUp
                height: visible ? implicitHeight : 0

                Column {
                    width: parent.width
                    spacing: Theme.spaceM

                    Row {
                        width: parent.width
                        spacing: Theme.spaceL

                        DeviceTree {
                            id: deviceTree
                            objectName: "deviceTreePanel"
                            width: 320
                            height: 430
                        }
                        DeviceLibrary {
                            id: deviceLibrary
                            objectName: "deviceLibraryPanel"
                            width: 340
                            height: 430
                            /* Task 8: while the 3D viewport is
                               active the library's button-add
                               targets the desk plane through the
                               viewport camera (its editorCam
                               answers planeHitPoint); the 2D
                               canvas keeps canvasCenter(). */
                            sceneView: root.viewport3d
                                       ? root.viewport3d : root
                            /* Task 9: the row edit button and the
                               header ✚ dispatch onto the preset
                               editor popup hosted below. */
                            onEditRequested: function(typeId) {
                                presetEditor.openFor(typeId)
                            }
                            onNewRequested: presetEditor.openNew()
                        }
                        TransformInspector {
                            id: transformInspector
                            objectName: "transformInspectorPanel"
                            /* `ctl` feeds tool/plane/snap reads — the
                               ObjectEditorController answers. Task 8:
                               `cam` unhides the camera preset chips
                               while the 3D viewport is live. */
                            ctl: root.studioUp ? objectCtl : null
                            cam: root.viewport3d
                                 ? root.viewport3d.editorCam : null
                        }
                    }

                    /* The object editor's status line — refusals
                       (locked chains, unknown types, gesture rejects)
                       are visible where they happened instead of
                       only living on a C++ signal. */
                    Text {
                        objectName: "objectStatus"
                        visible: root.studioUp
                                 && objectCtl.statusText !== ""
                        width: parent.width
                        text: root.studioUp ? objectCtl.statusText : ""
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSizeSmall
                        wrapMode: Text.WordWrap
                        elide: Text.ElideRight
                    }
                }
            }

            /*---- LED identification ---------------------------*/
            Card {
                title: qsTr("LED IDENTIFICATION")
                width: parent.width - 2 * col.padding
                Column {
                    spacing: Theme.spaceM
                    width: parent.width
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
                                AppComboBox {
                                    id: zonePick
                                    objectName: "identifyZone"
                                    Accessible.name:
                                        qsTr("identify zone")
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
                                    objectName: "identifyLed"
                                    Accessible.name:
                                        qsTr("identify LED index")
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
                                    objectName: "identifyDuration"
                                    Accessible.name:
                                        qsTr("identify duration milliseconds")
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
                                    objectName: "identifyStart"
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
                                    /* Per-controller: a SECOND
                                       controller's identify doesn't
                                       displace this row's state
                                       (Task 4.8). */
                                    objectName: "identifyStop_" + index
                                    Accessible.name: qsTr("Stop identifying %1").arg(
                                        snap.name || snap.key.device || "")
                                    text: qsTr("Stop")
                                    /* naming the list first keeps
                                       the binding subscribed to
                                       identifyingChanged */
                                    enabled: root.ctlUp
                                        && root.engineUp
                                        && layoutCtl.identifying
                                                  .length >= 0
                                        && layoutCtl.identifyingFor(
                                               snap.key).controller
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
                    /* One line per active overlay — concurrent
                       identifies on different controllers each
                       stay visible (Task 4.8). */
                    Repeater {
                        model: root.ctlUp ? layoutCtl.identifying : []
                        delegate: Text {
                            required property var modelData
                            Accessible.name: text
                            text: qsTr("identifying %1 %2 (%3 LEDs, %4 ms)")
                                .arg(modelData.controller
                                     ? (modelData.controller.device || "")
                                     : "")
                                .arg(modelData.zone || "")
                                .arg(modelData.led_count || 0)
                                .arg(modelData.duration_ms || 0)
                            color: Theme.accent
                            font.pixelSize: Theme.fontSizeSmall
                        }
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

    /* Task 9 popups — the device-type editor (from the library's
       edit / new seams) and the portable-workspace bundle dialog
       (from the toolbar button). Both take their controller from
       the context object; nothing here holds workspace state. */
    DevicePresetEditor {
        id: presetEditor
        anchors.centerIn: parent
    }
    WorkspaceImportDialog {
        id: bundleDlg
        anchors.centerIn: parent
    }
    WorkspaceActionsDialog {
        id: wsActionsDlg
        anchors.centerIn: parent
    }
}
