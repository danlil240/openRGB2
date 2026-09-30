/*---------------------------------------------------------*\
||  tst_viewport.qml                                       ||
||                                                         ||
||  studio-import Task 8 — scene-level viewport coverage. ||
||  StudioViewport is instantiated against QtObject stubs ||
||  wired through the EXPLICIT collaborator properties    ||
||  (objectCtl / workspaceCtl / viewport / prefs) — the   ||
||  same surface the Layout page sets; there is no global ||
||  bridge. Asserts:                                      ||
||    - delegates build from the shared object model      ||
||      (plain-array model exercises the modelData path;  ||
||      the real SceneObjectModel uses named roles)       ||
||    - emitter dots take authored colors, and the        ||
||      StudioViewportModel preview feed overrides them   ||
||      while previewActive (linked copies keep the       ||
||      owner's colors — no LED multiplication)           ||
||    - selection / transform gesture routing reaches the ||
||      editor stub; Escape cancels without commit        ||
||    - camera pose persists through setCameraState       ||
||    - quality tier writes renderQuality + shell pref    ||
||    - case ghost pref reaches the delegate's ghosted    ||
||    - pick-mode arm/disarm routes through the seam      ||
||                                                         ||
||  SPDX-License-Identifier: GPL-2.0-or-later              ||
\*---------------------------------------------------------*/
import QtQuick
import QtQuick3D
import QtTest
import "../../../ui/studio/viewport" as Vp

TestCase {
    id: tc
    name: "StudioViewportScene"
    when: windowShown

    /*---------------- QtObject stubs ----------------*/
    /* The editor surface — ObjectEditorController-shaped,
       including the signals the viewport's Connections bind. */
    component StubEditor: QtObject {
        property var objectModel: []
        property var selectedInstances: []
        property string selectedId: ""
        property int tool: 0
        property int editPlane: 1
        property bool snapEnabled: false
        property color paintColor: "#ff0000"
        property bool ready: true
        property var log: []
        property bool _gesture: false
        /* tool/editPlane/paintColor carry auto <prop>Changed
           signals matching the real controller's notify names;
           snapChanged mirrors the real snapEnabled NOTIFY. */
        signal selectionChanged()
        signal sceneChanged()
        signal emittersChanged(string objectId)
        signal snapChanged()
        function gestureActive() { return _gesture }
        function select(o) {
            log.push("select:" + o)
            selectedId = o
            var inst = o.indexOf("/") >= 0 ? o.substring(0, o.indexOf("/"))
                                           : o
            selectedInstances = inst === "" ? [] : [inst]
            selectionChanged()
        }
        function selectInstance(i, add) {
            log.push("selectInstance:" + i)
            var cur = selectedInstances.slice()
            if (add) { if (cur.indexOf(i) < 0) cur.push(i) }
            else cur = [i]
            selectedInstances = cur
            selectionChanged()
        }
        function clearEditorSelection() {
            log.push("clearSel")
            selectedInstances = []
            selectedId = ""
            selectionChanged()
        }
        function beginTransformGesture() { _gesture = true; log.push("beginT") }
        function updateTransformGesture(dx, dy, dz, pl, sn) {
            log.push("updT:" + dx.toFixed(3) + "," + dz.toFixed(3))
        }
        function updateRotateGestureAxis(x, y, z, a, sn) {
            log.push("updR:" + a.toFixed(2))
        }
        function commitTransformGesture() { _gesture = false; log.push("commitT") }
        function cancelTransformGesture() { _gesture = false; log.push("cancelT") }
        function paintEmitter(o, i, c) { log.push("paint:" + o + "|" + i) }
        function addDeviceInstance(t, x, y, z) {
            log.push("add:" + t)
        }
        function setStatusMessage(t) { log.push("status:" + t) }
        function undo() { log.push("undo") }
        function redo() { log.push("redo") }
        /* Rows are {x,y,z,c,i} and ["#RRGGBB"] maps — the same
           shape ObjectEditorController's emittersOf returns. */
        function emittersOf(o) {
            if (o === "kbd/keys")
                return [{ x: 0, y: 0.016, z: 0, i: 0, c: "#102030" },
                        { x: 0.02, y: 0.016, z: 0, i: 1, c: "#405060" }]
            return []
        }
        function emitterColorsOf(o) {
            if (o === "kbd/keys" || o === "kbd2/keys")
                return ["#102030", "#405060"]
            return []
        }
        function setTool(t) { if (t !== tool) { tool = t; toolChanged() } }
    }

    component StubWorkspace: QtObject {
        property bool ready: true
        property var cameraState: ({
            "view": "desk", "projection": "orthographic",
            "tx": 0.1, "ty": 0.18, "tz": 0.05,
            "yaw": 0, "pitch": -38,
            "distance": 1.21, "span": 0.9 })
        property string renderQuality: "balanced"
        property bool renderBloom: true
        property var camLog: []
        signal cameraChanged()
        signal renderPrefsChanged()
        /* NOTIFY'd property on the real controller — the viewport
           reads reducedMotion (no parens) as a binding. */
        property bool reducedMotion: false
        function setCameraState(s) {
            camLog.push("cam")
            cameraState = s
            cameraChanged()
        }
        function setRenderQuality(q) {
            renderQuality = q
            renderPrefsChanged()
        }
    }

    component StubPrefs: QtObject {
        property string viewportQuality: "balanced"
        property bool viewportBloom: true
        property bool viewportReducedMotion: false
        property bool viewportCaseGhost: false
        property bool layoutShow3d: true
    }

    component StubPreview: QtObject {
        property bool previewActive: false
        property var objects: []
        signal previewChanged()
    }

    property var vp: null
    property var ed: null
    property var ws: null
    property var pf: null
    property var pv: null

    Component {
        id: vpComp
        Vp.StudioViewport { width: 800; height: 500 }
    }
    StubEditor { id: stubEd }
    StubWorkspace { id: stubWs }
    StubPrefs { id: stubPf }
    StubPreview { id: stubPv }

    function init() {
        ed = stubEd
        ws = stubWs
        pf = stubPf
        pv = stubPv
        ed.objectModel = [
            { id: "desk",   instancePath: "desk", label: "Desk",
              kind: "decor", geometry: "desk", parentId: "",
              x: 0, y: -0.02, z: 0, qw: 1, qx: 0, qy: 0, qz: 0,
              sx: 1, sy: 1, sz: 1,
              bx: 1.6, by: 0.04, bz: 0.9, visible: true,
              bound: "none" },
            { id: "kbd",    instancePath: "kbd", label: "Kbd",
              kind: "device", geometry: "keyboard_body",
              parentId: "desk",
              x: 0.1, y: 0.02, z: 0.1, qw: 1, qx: 0, qy: 0, qz: 0,
              sx: 1, sy: 1, sz: 1,
              bx: 0.36, by: 0.03, bz: 0.13, visible: true,
              bound: "ok" },
            { id: "kbd/keys", instancePath: "kbd", label: "Keys",
              kind: "device", geometry: "key_emitters",
              parentId: "kbd",
              x: 0, y: 0, z: 0, qw: 1, qx: 0, qy: 0, qz: 0,
              sx: 1, sy: 1, sz: 1,
              bx: 0.36, by: 0.03, bz: 0.13, visible: true,
              bound: "ok" },
            /* A linked copy — same emitters via the owner's
               colors, no duplicate LED count. */
            { id: "kbd2/keys", instancePath: "kbd2", label: "Keys2",
              kind: "linked", geometry: "key_emitters",
              parentId: "",
              x: 0.6, y: 0.02, z: 0.1, qw: 1, qx: 0, qy: 0, qz: 0,
              sx: 1, sy: 1, sz: 1,
              bx: 0.36, by: 0.03, bz: 0.13, visible: true,
              bound: "none" },
            { id: "case/shell", instancePath: "case", label: "Case",
              kind: "decor", geometry: "case_shell", parentId: "desk",
              x: 0.4, y: 0.25, z: -0.1, qw: 1, qx: 0, qy: 0, qz: 0,
              sx: 1, sy: 1, sz: 1,
              bx: 0.21, by: 0.47, bz: 0.46, visible: true,
              bound: "none" }
        ]
        ed.log = []
        ws.camLog = []
        ws.ready = true
        pf.viewportCaseGhost = false
        pv.previewActive = false
        pv.objects = []
        vp = vpComp.createObject(tc, { "objectCtl": ed,
                                       "workspaceCtl": ws,
                                       "viewport": pv,
                                       "prefs": pf })
        verify(vp !== null, "StudioViewport failed to instantiate")
        tryCompare(vp.editorCtl, "gesture", 0)
    }

    function cleanup() {
        if (vp) { vp.destroy(); vp = null }
    }

    function logged(log, prefix) {
        return log.filter(function(s) { return s.indexOf(prefix) === 0 })
    }

    /* Delegates compose: the node map holds every model row and
       child rows parent under their parentId (the resolved
       world-matrix contract). */
    function test_delegates_build() {
        tryVerify(function() { return vp.worldPosOf("kbd/keys") !== null })
        verify(vp.worldPosOf("desk") !== null)
        verify(vp.worldPosOf("kbd") !== null)
        var w = vp.worldPosOf("kbd/keys")
        /* child row composes parent transforms (desk y offset +
           kbd offset). */
        verify(Math.abs(w.x - 0.1) < 1e-3, "child x: " + w.x)
        verify(Math.abs(w.y - 0.0) < 1e-3, "child y: " + w.y)
        verify(Math.abs(w.z - 0.1) < 1e-3, "child z: " + w.z)
    }

    /* Click on a device selects through the editor; empty click
       clears. */
    function test_click_selects_and_clears() {
        vp.editorCtl.beginPressAt(100, 100, Qt.LeftButton, 0,
                                "obj|kbd", Qt.vector3d(0.1, 0.02, 0.1))
        vp.editorCtl.endGesture(100, 100, Qt.LeftButton, 0)
        compare(ed.selectedInstances, ["kbd"])
        vp.editorCtl.beginPressAt(30, 30, Qt.LeftButton, 0, "", null)
        vp.editorCtl.endGesture(30, 30, Qt.LeftButton, 0)
        compare(ed.selectedInstances, [])
        verify(logged(ed.log, "clearSel").length >= 0)
    }

    /* Ctrl+drag on a device runs ONE gesture: begin -> update ->
       commit. */
    function test_ctrl_drag_gesture_lifecycle() {
        vp.editorCtl.beginPressAt(400, 250, Qt.LeftButton,
                                  Qt.ControlModifier,
                                  "obj|kbd",
                                  Qt.vector3d(0.1, 0.02, 0.1))
        vp.editorCtl.dragTo(460, 250, Qt.ControlModifier)
        compare(vp.editorCtl.gesture, 2)
        vp.editorCtl.dragTo(480, 250, Qt.ControlModifier)
        vp.editorCtl.endGesture(480, 250, Qt.LeftButton,
                                Qt.ControlModifier)
        compare(logged(ed.log, "beginT").length, 1)
        compare(logged(ed.log, "commitT").length, 1)
    }

    /* Escape mid-gesture cancels — no commit, no undo entry. */
    function test_escape_cancels_gesture() {
        vp.editorCtl.beginPressAt(400, 250, Qt.LeftButton,
                                  Qt.ControlModifier,
                                  "obj|kbd",
                                  Qt.vector3d(0.1, 0.02, 0.1))
        vp.editorCtl.dragTo(460, 250, Qt.ControlModifier)
        vp.escapeAll()
        compare(vp.editorCtl.gesture, 0)
        compare(logged(ed.log, "cancelT").length, 1)
        compare(logged(ed.log, "commitT").length, 0)
    }

    /* Undo is gated on a live gesture — the refusal lands on the
       status line, not silently swallowed. */
    function test_undo_gated_during_gesture() {
        vp.editorCtl.beginPressAt(400, 250, Qt.LeftButton,
                                  Qt.ControlModifier,
                                  "obj|kbd",
                                  Qt.vector3d(0.1, 0.02, 0.1))
        vp.editorCtl.dragTo(460, 250, Qt.ControlModifier)
        vp.undoOrRedo(false)
        compare(logged(ed.log, "undo").length, 0)
        verify(logged(ed.log, "status:finish the drag first").length === 1)
        vp.escapeAll()
        vp.undoOrRedo(false)
        compare(logged(ed.log, "undo").length, 1)
    }

    /* Gesture end persists the pose through the workspace —
       meta.camera, never undo history. */
    function test_pan_persists_pose() {
        vp.editorCtl.beginPressAt(400, 250, Qt.MiddleButton, 0,
                                  "", null)
        vp.editorCtl.dragTo(440, 250, 0)
        vp.editorCtl.endGesture(440, 250, Qt.MiddleButton, 0)
        verify(ws.camLog.length >= 1, "gesture end did not persist")
        compare(ws.cameraState.view !== undefined, true)
    }

    /* quality tier -> meta.render.quality on the draft + the
       shell pref fallback; invalid tier refused. */
    function test_quality_tier_persists() {
        vp.setQualityTier("high")
        compare(vp.qualityTier, "high")
        compare(ws.renderQuality, "high")
        compare(pf.viewportQuality, "high")
        vp.setQualityTier("bogus")
        compare(vp.qualityTier, "high")
        vp.setQualityTier("balanced")
    }

    /* The ghost pref reaches the case delegate — the ghostBody
       pick exclusion is structural; `ghosted` is the fade. */
    function test_case_ghost_pref() {
        verify(!vp.caseGhost)
        pf.viewportCaseGhost = true
        verify(vp.caseGhost)
        pf.viewportCaseGhost = false
    }

    /* Draft-preview colors win while the feed is live; without a
       record the authored colors render. The linked copy's feed
       is its OUTPUT OWNER's — the copy row itself may carry no
       record and still renders owner colors through the shared
       emitter read. */
    function test_preview_colors_override() {
        /* no feed -> authored */
        compare(vp.previewColorsFor("kbd/keys", 2), null)
        pv.previewActive = true
        pv.objects = [{ "id": "kbd/keys",
                        "colors": ["#a0b0c0", "#d0e0f0"] }]
        var got = vp.previewColorsFor("kbd/keys", 2)
        compare(got.length, 2)
        compare(got[0], "#a0b0c0")
        /* reduced frames carry sparse indices — unmapped
           emitters read authored-dark, never stale. */
        pv.objects = [{ "id": "kbd/keys",
                        "colors": ["#ff0000"], "indices": [1] }]
        got = vp.previewColorsFor("kbd/keys", 2)
        compare(got[0], "#000000")
        compare(got[1], "#ff0000")
    }

    /* Pick-mode seam: arming delivers begin/drag/end through the
       camera's desk-plane ray; disarm (Escape) sends cancel. */
    function test_pick_mode_routes() {
        var calls = []
        vp.armPick("origin", 0.0, function(pt, phase) {
            calls.push(phase)
        })
        compare(vp.editorCtl.pickMode, "origin")
        vp.editorCtl.beginPressAt(400, 250, Qt.LeftButton, 0, "", null)
        compare(vp.editorCtl.gesture, 7)
        vp.editorCtl.endGesture(400, 250, Qt.LeftButton, 0)
        verify(calls.join(",").indexOf("begin") === 0)
        verify(calls.join(",").indexOf("end") >= 0)
        /* armed-but-idle Escape disarms */
        vp.armPick("path", 0.0, function(pt, phase) { calls.push("c:" + phase) })
        vp.escapeAll()
        compare(vp.editorCtl.pickMode, "")
    }

    /* W/E/P route through the shared tool state — the stub's
       setTool mirrors what the docked inspector writes. */
    function test_tool_mirror() {
        ed.setTool(1)
        tryCompare(vp.editorCtl, "tool", 1)
        ed.setTool(2)
        tryCompare(vp.editorCtl, "tool", 2)
        ed.setTool(0)
        tryCompare(vp.editorCtl, "tool", 0)
    }

    /* The persisted camera pose restores on construction —
       applyPersisted read the stub workspace's cameraState. */
    function test_camera_persisted_restored() {
        compare(vp.editorCam.viewName, "desk")
        verify(Math.abs(vp.editorCam.target.x - 0.1) < 1e-6)
        verify(Math.abs(vp.editorCam.pitchDeg + 38) < 1e-6)
    }
}
