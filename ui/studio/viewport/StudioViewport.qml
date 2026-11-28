/*---------------------------------------------------------*\
|| StudioViewport.qml                                      ||
||                                                         ||
||   OpenRGB2 Layout 3D viewport (studio-import Task 8) — ||
||   adapted from the Desktop Lighting Studio source of   ||
||   record ui/StudioScene.qml. View3D + object delegates ||
||   + the editor layer (CameraController /               ||
||   SelectionController / TransformGizmo).               ||
||                                                         ||
||   WIRING (no global bridge / studioHost / plugin       ||
||   assumptions — every collaborator is an explicit      ||
||   property the host page sets):                        ||
||     objectCtl     ObjectEditorController — selection,  ||
||                   transform gestures, paint, undo, the  ||
||                   object model + emitter reads          ||
||     workspaceCtl  WorkspaceController — meta.camera    ||
||                   persistence (cameraState/             ||
||                   setCameraState), meta.render +        ||
||                   meta.ui prefs                         ||
||     viewport      StudioViewportModel — the bounded    ||
||                   draft-preview feed (evaluated emitter ||
||                   colors, honestly labelled)            ||
||     prefs         UiSettings — shell-level fallbacks   ||
||                   (quality/bloom/reduced-motion/case-   ||
||                   ghost) used when no draft is loaded   ||
||                                                         ||
||   Interaction contract (unchanged from the source):    ||
||     left click           select / empty-space clears   ||
||     left drag            orbit (enters free view)      ||
||     ctrl + left drag     move / rotate device or ring  ||
||     shift + left drag    marquee selection             ||
||     middle drag          camera pan — ALWAYS           ||
||     space + left drag    camera pan (alternative)      ||
||     wheel                pointer-centered zoom, clamped||
||     W / E / P            move / rotate / paint tools   ||
||     Shift mid-transform  temporary snap 10mm / 15°     ||
||     F / Home             frame selection / frame desk  ||
||     Escape               cancel gesture, restore state ||
||     (keys above: only while the viewport is visible   ||
||     and focused)                                       ||
||     Ctrl+Z / Ctrl+Shift+Z undo / redo — the shell's    ||
||                          (Main.qml), not declared here ||
||   Paint is an explicit tool: its drags paint emitters  ||
||   and can never move objects or orbit the camera.      ||
||                                                         ||
||   Requires the Quick3D-enabled build                   ||
||   (OPENRGB2_WITH_QUICK3D); basic builds load Layout's  ||
||   2D canvas path instead — see LayoutPage.             ||
||                                                         ||
||   SPDX-License-Identifier: GPL-2.0-or-later            ||
\*---------------------------------------------------------*/
import QtQuick
import QtQuick3D
import "editor" as Ed
import "devices" as Dev
import "materials" as Mats

Rectangle {
    id: root
    objectName: "studioViewport"
    color: "#101014"
    focus: true

    /*---- explicit collaborators (see header) -----------------*/
    property var objectCtl: null
    property var workspaceCtl: null
    property var viewport: null
    property var prefs: null
    /* Live-output badge — the page binds its bridge-derived
       "output is live" state; the viewport itself never reads a
       global. */
    property bool outputLive: false
    /* Text-entry focus peers — the docked panels (transform
       inspector, effect editor, device panels) report textFocus
       so viewport shortcuts never fire mid-edit. */
    property var focusPeer: null
    property var focusPeer2: null
    property var focusPeer3: null

    readonly property var editorCtl: selCtl
    readonly property var editorCam: camCtl
    /* Test/QA seams: the tiered environment instance (tier ->
       AA/AO/glow assertions) and the View3D itself (pick/map
       calls). */
    readonly property var sceneEnvironment: sceneEnv
    readonly property var sceneView3d: view

    /* Effect-pick arming seam (the effect editor drives this
       later; tests exercise it directly): armPick arms the
       SelectionController's exclusive pick gesture; disarmPick /
       Escape release it. */
    function armPick(mode, planeY, targetFn) {
        selCtl.pickMode = mode
        selCtl.pickPlaneY = planeY
        selCtl.pickTarget = targetFn
    }
    function disarmPick() {
        selCtl.pickMode = ""
        selCtl.pickTarget = null
    }

    function editingText() {
        return (focusPeer && focusPeer.textFocus)
               || (focusPeer2 && focusPeer2.textFocus)
               || (focusPeer3 && focusPeer3.textFocus)
    }

    // Diagnostic state readable from the probe / debug overlays.
    property string dbg: ""
    property real camYaw: cameraOrigin.eulerRotation.y

    /* Editor prefs resolve: the workspace document's meta wins
       whenever a draft is loaded; UiSettings is the shell-level
       fallback (and the no-draft default store). */
    readonly property bool workspaceReady:
        workspaceCtl !== null && workspaceCtl.ready === true

    /* Case-shell ghosting — display-only shell pref. The
       case_shell delegate family keeps its members unpickable
       either way (ctx.ghostBody); this toggle just fades the
       panels so interior hardware reads. */
    readonly property bool caseGhost:
        prefs ? prefs.viewportCaseGhost === true : false

    /* Render quality tier (plan §4): low | balanced | high.
       Persisted as meta.render.quality through workspaceCtl
       when a draft exists, and as UiSettings.viewportQuality
       either way — the workspace header's quality controls
       route here. */
    property string qualityTier: "balanced"
    function setQualityTier(q) {
        if (q !== "low" && q !== "balanced" && q !== "high")
            return
        qualityTier = q
        if (workspaceCtl && workspaceReady)
            workspaceCtl.renderQuality = q
        if (prefs)
            prefs.viewportQuality = q
    }

    /* Reduced motion — meta.ui.reduced_motion with the draft,
       UiSettings.viewportReducedMotion otherwise. A NOTIFY'd
       property read (renderPrefsChanged), so a live toggle
       re-applies the environment. */
    readonly property bool reducedMotion:
        workspaceReady ? workspaceCtl.reducedMotion
                       : (prefs ? prefs.viewportReducedMotion : false)

    readonly property bool bloomOn:
        workspaceReady ? workspaceCtl.renderBloom
                       : (prefs ? prefs.viewportBloom : true)

    /* Device-family dispatch: geometry tag -> family component
       source (viewport/devices/). Empty = the generic primitive
       path below (desk + anything unknown). mouse_zone / group
       have no body — emitter dots only, as before. */
    function familySource(geom) {
        switch (geom) {
        case "keyboard_body": return "devices/Keyboard.qml"
        case "mouse_body":    return "devices/Mouse.qml"
        case "fan_body":
        case "pump_body":     return "devices/Fan.qml"
        case "ram_body":      return "devices/Ram.qml"
        case "case_shell":    return "devices/Case.qml"
        case "gpu_body":      return "devices/Gpu.qml"
        case "gpu_logo":      return "devices/Strip.qml"
        case "monitor":       return "devices/Monitor.qml"
        default:              return ""
        }
    }

    // Canonical body specs — dispatch on the geometry string.
    // #Cube/#Cylinder/#Sphere are 100-unit primitives, so scale =
    // meters / 100. transform.scale stays dimensionless on the node.
    // Family-handled tags keep their entry here for the ghost flag;
    // the fallback Model only renders when familySource() is empty.
    function bodySpec(geom) {
        switch (geom) {
        case "desk":          return { src: "#Cube",     c: "#4a3b32" }
        case "case_shell":    return { src: "#Cube",     c: "#e8e8ec", ghost: true }
        case "monitor":       return { src: "#Cube",     c: "#0a0a0c" }
        case "mouse_body":    return { src: "#Cube",     c: "#22222a" }
        case "gpu_body":      return { src: "#Cube",     c: "#e8e8ec" }
        case "keyboard_body": return { src: "#Cube",     c: "#1c1c22" }
        case "fan_body":      return { src: "#Cylinder", c: "#202028" }
        case "ram_body":      return { src: "#Cube",     c: "#18181f" }
        case "pump_body":     return { src: "#Cylinder", c: "#22242c" }
        default:              return null
        }
    }

    function emitterSize(geom) {
        return geom === "keyboard_body" ? 0.006 : 0.011
    }

    /* Evaluated draft-preview colors for a resolved object id —
       the Task 7 feed. Reduced frames carry `indices` (sparse
       sample map); a dense array is authoritative per-emitter.
       Returns null when the preview has no record for the id
       (fall back to the document's authored colors). */
    function previewColorsFor(oId, emitterCount) {
        if (!viewport || !viewport.previewActive)
            return null
        /* The real model answers from an id index in C++ — no
           per-object copy + linear scan of the whole frame. */
        if (typeof viewport.colorsFor === "function") {
            var r = viewport.colorsFor(oId, emitterCount)
            return (r === undefined || r === null) ? null : r
        }
        var objs = viewport.objects || []
        for (var i = 0; i < objs.length; i++) {
            var o = objs[i]
            if (o.id !== oId)
                continue
            var cols = o.colors || []
            var idxs = o.indices
            if (idxs && idxs.length === cols.length) {
                var out = []
                for (var k = 0; k < emitterCount; k++)
                    out.push("#000000")
                for (var j = 0; j < idxs.length; j++)
                    if (idxs[j] < emitterCount)
                        out[idxs[j]] = cols[j]
                return out
            }
            return cols
        }
        return null
    }

    // object id -> delegate Node. The delegate reparents through
    // this map so the QML node tree composes exactly like the
    // core's resolved world matrices (parentWorld * local).
    property var objNodes: ({})
    function fixupParents() {
        for (var id in objNodes) {
            var n = objNodes[id]
            var p = (n.oParent && objNodes[n.oParent])
            n.parent = p ? p : sceneRoot
        }
    }

    // World bounds over the object-node map. ids = instance ids to
    // include; null/empty = the whole scene. Used by F / Home and
    // the case preset view.
    function instanceWorldBounds(ids) {
        var want = null
        if (ids && ids.length) {
            want = {}
            for (var k = 0; k < ids.length; k++)
                want[ids[k]] = true
        }
        var sx = 0, sy = 0, sz = 0, n = 0
        var pts = []
        for (var id in objNodes) {
            var item = objNodes[id]
            if (!item || !item.visible)
                continue
            if (want && !want[item.oInst])
                continue
            var p = item.scenePosition
            pts.push({ p: p, r: 0.5 * Math.max(item.oBx, item.oBy, item.oBz) })
            sx += p.x; sy += p.y; sz += p.z; n++
        }
        if (n === 0)
            return null
        var c = Qt.vector3d(sx / n, sy / n, sz / n)
        var r = 0.05
        for (var i = 0; i < pts.length; i++) {
            var d = pts[i].p.minus(c).length() + pts[i].r
            if (d > r)
                r = d
        }
        return { c: c, r: r }
    }

    // Test seam: world/screen position of an object id's node.
    function worldPosOf(objectId) {
        var n = objNodes[objectId]
        return n ? n.scenePosition : null
    }
    function screenPosOf(objectId) {
        var w = worldPosOf(objectId)
        return (w && view) ? view.mapFrom3DScene(w) : null
    }

    function eachNode(cb) {
        for (var id in objNodes)
            cb(objNodes[id])
    }

    function escapeAll() {
        /* Escape priority: live gesture > text focus > selection. */
        if (selCtl.cancel())
            return
        if (root.editingText()) {
            forceActiveFocus()
            return
        }
        if (objectCtl)
            objectCtl.clearEditorSelection()
    }

    /* True while any gesture-capable press window is open: the
       click candidate, live pan/orbit/move/rotate/marquee/paint,
       or the controller's transform gesture. Frame commands emit
       poseFinished under the press's camera snapshot — gate them
       on this the same way undo is gated. */
    function gestureBusy() {
        return selCtl.pressed
               || (objectCtl && objectCtl.gestureActive())
    }

    /* Undo/redo never run under a live gesture — the stack op would
       write beneath the snapshot the gesture is previewing against.
       The controller refuses too (defense for ungated callers);
       gating here lets the swallowed shortcut say WHY via the
       status line. */
    function undoOrRedo(redo) {
        if (!objectCtl)
            return
        if (objectCtl.gestureActive()) {
            objectCtl.setStatusMessage("finish the drag first")
            return
        }
        if (redo)
            objectCtl.redo()
        else
            objectCtl.undo()
    }

    View3D {
        id: view
        anchors.fill: parent
        /* camCtl is declared further down the file — guard the
           forward id refs so a resize during construction is safe. */
        onHeightChanged: if (typeof camCtl !== "undefined" && camCtl) camCtl.apply()
        onWidthChanged:  if (typeof camCtl !== "undefined" && camCtl) camCtl.apply()

        /* Tiered environment (plan §4 "Rendering"): AA, contact
           shading and restrained bloom scale with qualityTier;
           the emitter color buffer and NoLighting dots are
           identical under every tier. */
        environment: Mats.StudioEnvironment {
            id: sceneEnv
            quality: root.qualityTier
            bloomPref: root.bloomOn
            reducedMotion: root.reducedMotion
        }

        // Camera rig: `origin` carries the pose (position = target,
        // eulerRotation = view yaw/pitch — camera nodes may use
        // eulerRotation; the object-transform ban doesn't apply).
        Node {
            id: cameraOrigin
            position: Qt.vector3d(0.1, 0.18, 0.05)
            eulerRotation.x: -38

            OrthographicCamera {
                id: camOrtho
                clipNear: 0.01
                clipFar: 100
                position: Qt.vector3d(0, 0, 3)
            }
            PerspectiveCamera {
                id: camPersp
                clipNear: 0.01
                clipFar: 100
                fieldOfView: 60
                position: Qt.vector3d(0, 0, 1.21)
            }
        }

        /* Small fixed lighting rig (spec: "a small lighting rig
           rather than one real light per LED") — key + cool fill +
           rim + a soft desk pool. Shadows are a High-tier
           decoration; tiers never touch the emitter path. */
        DirectionalLight {
            // key
            eulerRotation.x: -50
            eulerRotation.y: -35
            brightness: 1.5
            castsShadow: root.qualityTier === "high"
            shadowFactor: 35
            shadowBias: 0.02
            softShadowQuality: Light.PCF16
            pcfFactor: 2.0
        }
        DirectionalLight {
            // cool fill
            eulerRotation.x: -25
            eulerRotation.y: 115
            brightness: 0.45
            color: "#8e97b8"
        }
        DirectionalLight {
            // rim from behind
            eulerRotation.x: -10
            eulerRotation.y: 205
            brightness: 0.8
            color: "#b9c2dd"
        }
        PointLight {
            // soft desk pool
            position: Qt.vector3d(0.25, 0.7, 0.2)
            brightness: 4
            color: "#a8b0cc"
        }

        // Scene objects — the shared SceneObjectModel is the
        // primary source (granular transform updates keep delegates
        // alive through drags).
        Node {
            id: sceneRoot

        Repeater3D {
            id: objRepeater
            model: (root.objectCtl && root.objectCtl.objectModel)
                   ? root.objectCtl.objectModel : []

            // New model = new delegate set; drop stale node refs.
            onModelChanged: root.objNodes = ({})

            delegate: Node {
                id: objNode

                // Field reader: list-model role first, plain-map
                // modelData second (probe fallback).
                function rf(name, dflt) {
                    var v = model[name]
                    if (v === undefined && typeof modelData !== "undefined")
                        v = modelData[name]
                    return v === undefined ? dflt : v
                }

                property string oId:     rf("id", "")
                property string oGeom:   rf("geometry", "")
                property string oParent: rf("parentId", "")
                property string oInst:   rf("instancePath", oId)
                property string oKind:   rf("kind", "decor")
                property string oBound:  rf("bound", "none")
                property real   oBx:     rf("bx", 0)
                property real   oBy:     rf("by", 0)
                property real   oBz:     rf("bz", 0)
                property var    spec:    root.bodySpec(oGeom)
                /* Ghost-shell flag the family components read via
                   ctx — kept here so the pick-exclusion rule stays
                   next to the spec lookup. `ghosted` is the user's
                   fade-the-case toggle (caseGhost pref). */
                readonly property bool ghostBody:
                    spec !== null && spec.ghost === true
                readonly property bool ghosted: root.caseGhost

                // Structure (positions) and colors are split: the
                // Repeater's model is emitterPos, which only changes
                // on a scene/layout change, so delegates persist.
                // Per-tick updates only reassign emitterColors.
                property var emitterPos: []
                property var emitterColors: []

                /* sceneChanged fires per mouse move during a gesture
                   preview; emitter positions are in the object's
                   LOCAL frame, so a transform-only change leaves
                   them identical. Reassigning emitterPos (the
                   Repeater3D model) recreates every emitter Model —
                   only do it when the layout really changed. */
                function sameLayout(a, b) {
                    if (a.length !== b.length)
                        return false
                    for (var i = 0; i < a.length; i++) {
                        var p = a[i], q = b[i]
                        if (p.i !== q.i || p.x !== q.x || p.y !== q.y
                            || p.z !== q.z)
                            return false
                    }
                    return true
                }
                function reloadLayout() {
                    var list = root.objectCtl
                               ? root.objectCtl.emittersOf(oId) : []
                    if (!sameLayout(list, emitterPos))
                        emitterPos = list
                    reloadColors()
                }
                function sameColors(a, b) {
                    if (a.length !== b.length)
                        return false
                    for (var i = 0; i < a.length; i++)
                        if (String(a[i]) !== String(b[i]))
                            return false
                    return true
                }

                /* Draft-preview colors win while the Task 7 feed is
                   live (evaluated output of the UNSAVED draft);
                   otherwise the authored document colors — a linked
                   copy resolves to its output owner through both
                   paths, so mirrored hardware never duplicates LED
                   writes and always renders the owner's frame. */
                function reloadColors() {
                    var pv = root.previewColorsFor(oId,
                                                   emitterPos.length)
                    var next = pv !== null ? pv
                        : (root.objectCtl
                           ? root.objectCtl.emitterColorsOf(oId) : [])
                    /* Unchanged colors: keep the array so the
                       per-dot material bindings don't all re-run. */
                    if (!sameColors(next, emitterColors))
                        emitterColors = next
                }

                position: Qt.vector3d(rf("x", 0), rf("y", 0), rf("z", 0))
                // One shared rotation convention: the core converts
                // the stored XYZ degrees (Rz*Ry*Rx) into this
                // quaternion — eulerRotation here would silently
                // apply ZXY instead.
                rotation: Qt.quaternion(rf("qw", 1), rf("qx", 0),
                                        rf("qy", 0), rf("qz", 0))
                scale: Qt.vector3d(rf("sx", 1), rf("sy", 1), rf("sz", 1))
                visible: rf("visible", true)

                Component.onCompleted: {
                    root.objNodes[oId] = objNode
                    var p = root.objNodes[oParent]
                    objNode.parent = p ? p : sceneRoot
                    reloadLayout()
                    // Last delegate done — repair any parent that was
                    // created after its child.
                    if (Object.keys(root.objNodes).length === objRepeater.count)
                        root.fixupParents()
                }
                Component.onDestruction: {
                    if (root.objNodes[oId] === objNode)
                        delete root.objNodes[oId]
                }

                Connections {
                    target: root.objectCtl
                    function onEmittersChanged(changedId) {
                        if (changedId === objNode.oId) objNode.reloadColors()
                    }
                    function onSceneChanged() { objNode.reloadLayout() }
                }
                /* The draft-preview feed repaints emitter colors in
                   place — structure is untouched, so delegates and
                   gestures survive frame updates. */
                Connections {
                    target: root.viewport
                    function onPreviewChanged() { objNode.reloadColors() }
                }

                /* Device-family body — Loader3D swaps in the
                   per-family component for known geometry tags.
                   The loaded root gets `ctx` = this delegate, so it
                   reads the resolved size (oBx/oBy/oBz), pick id,
                   ghost flags and the shared emitter arrays without
                   any per-family plumbing. */
                Loader3D {
                    id: bodyLoader
                    source: root.familySource(objNode.oGeom)
                    onLoaded: item.ctx = objNode
                }

                // Generic fallback body — ghost shells (the case)
                // are translucent containers; leaving them pickable
                // would swallow every pick aimed at hardware inside.
                Model {
                    objectName: "obj|" + objNode.oId
                    pickable: !objNode.ghostBody
                    visible: objNode.spec !== null
                             && root.familySource(objNode.oGeom) === ""
                    source: objNode.spec ? objNode.spec.src : "#Cube"
                    scale: objNode.spec
                           ? Qt.vector3d(objNode.oBx / 100,
                                         objNode.oBy / 100,
                                         objNode.oBz / 100)
                           : Qt.vector3d(0, 0, 0)
                    materials: PrincipledMaterial {
                        baseColor: objNode.spec ? objNode.spec.c : "#000000"
                        opacity: (objNode.spec && objNode.spec.ghost)
                                 ? (root.caseGhost ? 0.12 : 0.42)
                                 : 1.0
                    }
                }

                // Emitter dots (local frame — inherits node transform)
                Repeater3D {
                    model: objNode.emitterPos
                    delegate: Node {
                        required property var modelData

                        // Visible dot — not pickable itself (too
                        // small); the proxy below handles picking.
                        Model {
                            source: "#Sphere"
                            position: Qt.vector3d(modelData.x, modelData.y, modelData.z)
                            /* Paint tool = LED mapping mode: enlarged
                               address markers (spec §3). */
                            property real d: root.emitterSize(objNode.oGeom)
                                             * (selCtl.tool === 2 ? 1.6 : 1.0) / 100
                            scale: Qt.vector3d(d, d, d)
                            materials: PrincipledMaterial {
                                lighting: PrincipledMaterial.NoLighting
                                property color c: (modelData.i < objNode.emitterColors.length)
                                                  ? objNode.emitterColors[modelData.i] : "#000000"
                                baseColor: (objNode.oBound === "ok" || objNode.oBound === "none")
                                           ? c : Qt.darker(c, 2.5)
                            }
                        }

                        // Invisible pick proxy ~2.6x the dot so LEDs
                        // are actually hittable with a mouse.
                        Model {
                            objectName: "emit|" + objNode.oId + "|" + modelData.i
                            pickable: true
                            source: "#Sphere"
                            position: Qt.vector3d(modelData.x, modelData.y, modelData.z)
                            property real pd: root.emitterSize(objNode.oGeom) * 2.6 / 100
                            scale: Qt.vector3d(pd, pd, pd)
                            castsShadows: false
                            materials: PrincipledMaterial {
                                lighting: PrincipledMaterial.NoLighting
                                baseColor: "#00000000"
                                opacity: 0.0
                                depthDrawMode: PrincipledMaterial.NeverDepthDraw
                            }
                        }
                    }
                }

                /* Selection treatment (spec §3): bounds frame +
                   corner handles instead of the old floating dot —
                   visible for the selected object AND every member
                   of the multi-selection. */
                Dev.SelectionFrame {
                    bx: objNode.oBx
                    by: objNode.oBy
                    bz: objNode.oBz
                    visible: root.objectCtl !== null
                             && (root.objectCtl.selectedId === objNode.oId
                                 || root.objectCtl.selectedInstances.indexOf(
                                        objNode.oInst) >= 0)
                }
            }
        }

        // Move-tool pivot marker: 3D axis cross at the shared
        // selection pivot (rotate mode's handle is the 2D ring on
        // the gizmo overlay).
        Node {
            id: pivotMarker
            visible: gizmo.hasPivot && selCtl.tool === 0 && selCtl.gesture === 0
            position: gizmo.pivotWorld ? gizmo.pivotWorld : Qt.vector3d(0, 0, 0)
            Model {
                source: "#Cube"; position: Qt.vector3d(0.035, 0, 0)
                scale: Qt.vector3d(0.0007, 0.00008, 0.00008)
                materials: PrincipledMaterial {
                    lighting: PrincipledMaterial.NoLighting
                    baseColor: "#e05050"
                }
            }
            Model {
                source: "#Cube"; position: Qt.vector3d(0, 0.035, 0)
                scale: Qt.vector3d(0.00008, 0.0007, 0.00008)
                materials: PrincipledMaterial {
                    lighting: PrincipledMaterial.NoLighting
                    baseColor: "#50c050"
                }
            }
            Model {
                source: "#Cube"; position: Qt.vector3d(0, 0, 0.035)
                scale: Qt.vector3d(0.00008, 0.00008, 0.0007)
                materials: PrincipledMaterial {
                    lighting: PrincipledMaterial.NoLighting
                    baseColor: "#5080e0"
                }
            }
            Model {
                source: "#Sphere"
                scale: Qt.vector3d(0.00012, 0.00012, 0.00012)
                materials: PrincipledMaterial {
                    lighting: PrincipledMaterial.NoLighting
                    baseColor: "#ffd040"
                }
            }
        }
        }

        /*-----------------------------------------------------*\
        || Editor controllers                                   ||
        \*-----------------------------------------------------*/
        Ed.CameraController {
            id: camCtl
            view3d: view
            origin: cameraOrigin
            orthoCam: camOrtho
            perspCam: camPersp
            boundsOf: root.instanceWorldBounds
            prefsSource: root.workspaceCtl
            onPoseFinished: {
                if (root.workspaceCtl)
                    root.workspaceCtl.setCameraState(stateMap())
            }
        }

        Ed.SelectionController {
            id: selCtl
            view3d: view
            cam: camCtl
            gizmo: gizmo
            eachNode: root.eachNode
            editor: root.objectCtl
            stateSource: root.workspaceCtl
        }

        Ed.TransformGizmo {
            id: gizmo
            anchors.fill: parent
            view3d: view
            cam: camCtl
            ctl: selCtl
            editor: root.objectCtl
            eachNode: root.eachNode
        }

        /* Shared tool/plane/snap state — the docked panels write
           objectCtl; the viewport mirrors it into the router. The
           mirror triggers setTool's cancel so a panel tool switch
           mid-drag still drops the live gesture. */
        Connections {
            target: root.objectCtl
            function onToolChanged() {
                selCtl.setTool(root.objectCtl.tool)
            }
            function onEditPlaneChanged() {
                selCtl.editPlane = root.objectCtl.editPlane
            }
            function onSnapChanged() {
                selCtl.snapEnabled = root.objectCtl.snapEnabled
            }
        }

        /* Single input router — every pointer press/drag/release
           and wheel goes through selCtl. Middle-button pan wins
           over any surface; see SelectionController.qml. */
        MouseArea {
            objectName: "viewportInput"
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
            /* Hosted inside a page Flickable (LayoutPage): without
               this the page steals orbit/pan/gizmo drags past its
               threshold, scrolls, and cancels the gesture. */
            preventStealing: true
            onPressed: function(m) {
                root.forceActiveFocus()
                selCtl.press(m.x, m.y, m.button, m.modifiers)
            }
            onPositionChanged: function(m) {
                if (m.buttons === 0)
                    return
                selCtl.dragTo(m.x, m.y, m.modifiers)
            }
            onReleased: function(m) {
                selCtl.endGesture(m.x, m.y, m.button, m.modifiers)
            }
            onCanceled: selCtl.cancel()
            onWheel: function(w) {
                selCtl.wheelAt(w.x, w.y, w.angleDelta.y)
                w.accepted = true
            }
        }

        /* Marquee rubber band. */
        Rectangle {
            visible: selCtl.gesture === 4
            x: selCtl.marqueeRect.x
            y: selCtl.marqueeRect.y
            width: selCtl.marqueeRect.width
            height: selCtl.marqueeRect.height
            color: "#206a9ad0"
            border.color: "#6a9ad0"
            border.width: 1
        }

        /* Device-library drops. The drag chip carries the type id
           (drop.source.dropTypeId, mimeData key as fallback); the
           pixel lands on the desk plane through the camera's
           planeHitPoint — the same ray/plane math the move gesture
           uses. The controller clamps the footprint onto the
           surface and the add is one undoable op. Input-only (no
           drag) it sits inert — it never intercepts clicks. */
        DropArea {
            anchors.fill: parent
            keys: ["studio-device-type"]
            onDropped: function(drop) {
                var tid = ""
                if (drop.source && drop.source.dropTypeId !== undefined)
                    tid = drop.source.dropTypeId
                else
                    tid = drop.getDataAsString("studio-device-type") || ""
                if (tid === "" || !root.objectCtl
                    || typeof root.objectCtl.addDeviceInstance !== "function") {
                    drop.accepted = false
                    return
                }
                var p = camCtl.planeHitPoint(drop.x, drop.y, 1, 0.0)
                if (!p && camCtl.target)
                    p = camCtl.planeHitPoint(drop.x, drop.y, -1,
                                             camCtl.target)
                if (!p && camCtl.target)
                    p = camCtl.target
                drop.accept(Qt.CopyAction)
                root.objectCtl.addDeviceInstance(tid, p ? p.x : 0, 0,
                                               p ? p.z : 0)
            }
        }
    }

    /*-----------------------------------------------------*\
    || Hints + live badge — the floating transform          ||
    || inspector of the source is replaced by Layout's      ||
    || docked panels (focusPeer covers their text fields).  ||
    \*-----------------------------------------------------*/
    Text {
        id: pickedLabel
        objectName: "viewportHint"
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 10
        color: "#d8d8e0"
        font.pixelSize: 13
        text: (root.objectCtl && root.objectCtl.selectedId !== "")
              ? "selected: " + root.objectCtl.selectedId
              : "click: select · drag: orbit · Ctrl+drag: move · Shift+drag: marquee · MMB/Space: pan · wheel: zoom · W/E/P · F/Home: frame"
    }

    Rectangle {
        objectName: "viewportLiveBadge"
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 10
        visible: root.outputLive
        color: "#401014"
        radius: 4
        width: liveText.width + 16
        height: liveText.height + 8
        Text {
            id: liveText
            anchors.centerIn: parent
            color: "#ff6060"
            font.pixelSize: 12
            font.bold: true
            text: "LIVE"
        }
    }

    /*-----------------------------------------------------*\
    || Keyboard — shortcuts never fire while a text field   ||
    || has focus (focusPeer*.textFocus). Escape is global:  ||
    || it always cancels the live gesture.                  ||
    \*-----------------------------------------------------*/
    Keys.onPressed: function(e) {
        if (e.key === Qt.Key_Space && !e.isAutoRepeat) {
            selCtl.spaceDown = true
            e.accepted = true
        }
    }
    Keys.onReleased: function(e) {
        if (e.key === Qt.Key_Space) {
            selCtl.spaceDown = false
            e.accepted = true
        }
    }

    /* Audit Q-2: these Shortcuts are window-wide and ignore
       visibility, and LayoutPage keeps this viewport's Loader
       alive while another page is current — so W/E/P/F/Home/
       Escape used to fire from every page. They are scoped to
       the visible viewport holding focus (a press in it takes
       focus). Escape also stays live while a docked focus peer
       edits text, to hand focus back here. Undo/redo are NOT
       declared here: Main.qml owns Ctrl+Z / Ctrl+Shift+Z /
       Ctrl+Y (the same shared workspace timeline), and a second
       declaration made those keys ambiguous so neither fired. */
    readonly property bool keyScope: {
        if (!root.visible)
            return false
        for (var it = root.Window.activeFocusItem; it; it = it.parent)
            if (it === root)
                return true
        return false
    }
    Shortcut { sequence: "W"
               enabled: root.keyScope && !root.editingText()
               onActivated: selCtl.setTool(0) }
    Shortcut { sequence: "E"
               enabled: root.keyScope && !root.editingText()
               onActivated: selCtl.setTool(1) }
    Shortcut { sequence: "P"
               enabled: root.keyScope && !root.editingText()
               onActivated: selCtl.setTool(2) }
    /* Frame commands emit poseFinished under a live gesture's camera
       snapshot — gate them on the press window the same way. */
    Shortcut { sequence: "F"
               enabled: root.keyScope && !root.editingText()
               onActivated: if (!root.gestureBusy()) selCtl.frameSelection() }
    Shortcut { sequence: "Home"
               enabled: root.keyScope && !root.editingText()
               onActivated: if (!root.gestureBusy()) selCtl.frameAll() }
    Shortcut { sequence: "Escape"
               enabled: root.visible
                        && (root.keyScope || !!root.editingText())
               onActivated: root.escapeAll() }

    Connections {
        target: root.workspaceCtl
        function onCameraChanged() { camCtl.applyPersisted() }
        function onRenderPrefsChanged() {
            /* Workspace load / external edit re-reads the persisted
               tier; the setter path (setQualityTier) writes through
               the controller instead of assigning here. */
            if (root.workspaceCtl && root.workspaceReady)
                root.qualityTier = root.workspaceCtl.renderQuality
        }
    }

    /* Focus loss mid-space-hold would leave a sticky pan modifier —
       the next left-drag would unexpectedly pan. spaceDown mirrors
       the physical key, so it is cleared ONLY here (app deactivation)
       and on key release — never on gesture end.
       This keys off Qt.application.state so it also fires when the
       viewport lives inside a host that never activates a window. */
    Connections {
        target: Qt.application
        function onStateChanged() {
            if (Qt.application.state !== Qt.ApplicationActive) {
                selCtl.spaceDown = false
                selCtl.cancel()
            }
        }
    }

    Component.onCompleted: {
        if (root.workspaceCtl && root.workspaceReady)
            qualityTier = root.workspaceCtl.renderQuality
        else if (root.prefs)
            qualityTier = prefs.viewportQuality
        camCtl.applyPersisted()
        /* Mirror the shared tool/plane/snap state into the router. */
        if (root.objectCtl) {
            selCtl.tool       = root.objectCtl.tool
            selCtl.editPlane  = root.objectCtl.editPlane
            selCtl.snapEnabled = root.objectCtl.snapEnabled
        }
    }
}
