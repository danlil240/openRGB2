/*---------------------------------------------------------*\
||  tst_controllers.qml                                    ||
||                                                         ||
||  studio-import Task 8 — adapted from the source of     ||
||  record tests/editor_qml/tst_controllers.qml.          ||
||  Pure-QML gesture/camera tests driven by qmltestrunner ||
||  (registered as the `qml_studio_qml` CTest in           ||
||  tests/ui/CMakeLists.txt).                              ||
||                                                         ||
||  SelectionController / CameraController are instantiated||
||  with stub view/cam objects; the source's implicit      ||
||  `bridge` context property is replaced by the explicit  ||
||  `editor` / `stateSource` seams — plain-JS stubs here,  ||
||  the real ObjectEditorController / WorkspaceController  ||
||  in production. Assertions keep the interaction         ||
||  contract: Paint-mode exclusivity, gesture              ||
||  cancel-on-repress, spaceDown lifecycle, middle_drag    ||
||  pref, marquee exclusion, deferred pose persistence,    ||
||  pick-mode lifecycle.                                   ||
||                                                         ||
||  SPDX-License-Identifier: GPL-2.0-or-later              ||
\*---------------------------------------------------------*/
import QtQuick
import QtTest
import "../../../ui/studio/viewport/editor" as Ed

TestCase {
    id: tc
    name: "ViewportControllers"
    when: windowShown

    /*---------------- stubs ----------------*/
    /* The editor surface — ObjectEditorController-shaped. */
    function makeEditor() {
        return {
            cameraState: { "view": "desk", "projection": "orthographic",
                           "tx": 0.1, "ty": 0.18, "tz": 0.05,
                           "yaw": 0, "pitch": -38,
                           "distance": 1.21, "span": 0.9,
                           "middle_drag": "pan" },
            selectedInstances: [],
            paintColor: "#ff0000",
            tool: 0,
            editPlane: 1,
            snapEnabled: false,
            log: [],
            select: function(o) { this.log.push("select:" + o) },
            selectInstance: function(i, add) {
                this.log.push("selectInstance:" + i)
                if (this.selectedInstances.indexOf(i) < 0)
                    this.selectedInstances.push(i)
            },
            clearEditorSelection: function() {
                this.log.push("clearSel"); this.selectedInstances = []
            },
            beginTransformGesture: function() { this.log.push("beginT") },
            gestureActive: function() { return true },
            updateTransformGesture: function(dx, dy, dz, pl, sn) {
                this.log.push("updT:" + dx.toFixed(3) + "," + dz.toFixed(3))
            },
            updateRotateGestureAxis: function(x, y, z, a, sn) {
                this.log.push("updR:" + a.toFixed(2))
            },
            commitTransformGesture: function() { this.log.push("commitT") },
            cancelTransformGesture: function() { this.log.push("cancelT") },
            paintEmitter: function(inst, idx, c) {
                this.log.push("paint:" + inst + "|" + idx)
            },
            setStatusMessage: function(t) { this.log.push("status:" + t) }
        }
    }

    function makeCam() {
        return {
            ortho: true, deferPose: false, deferredSave: false, log: [],
            origin: { forward: Qt.vector3d(0, 0, -1) },
            poseSnapshot: function() {
                return { t: Qt.vector3d(0, 0, 0), yaw: 0, pitch: -38,
                         dist: 1.21, sp: 0.9, vn: "desk" }
            },
            panPixels: function(dx, dy) { this.log.push("pan") },
            orbitPixels: function(dx, dy) { this.log.push("orbit") },
            enterFreeView: function() { this.ortho = false
                                        this.log.push("enterFree") },
            endPose: function() { this.deferredSave = false
                                  this.log.push("endPose") },
            flushDeferredPose: function() {
                if (this.deferredSave) {
                    this.deferredSave = false
                    this.log.push("flush")
                }
            },
            restorePose: function(s) { this.log.push("restore") },
            pointUnderPointer: function(x, y) {
                return Qt.vector3d(0.1, 0.02, 0.2)
            },
            planeHitPoint: function(x, y, ax, v) {
                return Qt.vector3d(0.3, 0.02, 0.4)
            },
            zoomAt: function(x, y, a) { this.log.push("zoom") },
            frameIds: function(ids) { this.log.push("frame") }
        }
    }

    function makeView() {
        return {
            nextPick: null,
            pick: function(x, y) { return this.nextPick },
            mapFrom3DScene: function(p) { return this._map(p) },
            _map: function(p) { return Qt.vector3d(0, 0, 0) },
            mapTo3DScene: function(v) {
                return Qt.vector3d((v.x - 250) * 0.002,
                                   (250 - v.y) * 0.002, 1.0 - v.z)
            },
            height: 500, camera: null
        }
    }

    function objPick(name) {
        return { objectHit: { objectName: name },
                 scenePosition: Qt.vector3d(0.1, 0.02, 0.2) }
    }

    property var sel: null
    property var edStub: null
    property var camStub: null
    property var viewStub: null
    property var gizStub: null
    property var camCtl: null

    Component { id: selComp; Ed.SelectionController {} }
    Component { id: camComp; Ed.CameraController {} }

    function init() {
        sel = selComp.createObject(tc)
        edStub = makeEditor()
        camStub = makeCam()
        viewStub = makeView()
        gizStub = { ring: false,
                    updatePivot: function() {},
                    angleAt: function(x, y) { return x * 0.01 },
                    ringHit: function(x, y) { return this.ring } }
        /* OpenRGB2 seams: editor (ObjectEditorController) +
           stateSource (WorkspaceController, cameraState prefs). */
        sel.editor = edStub
        sel.stateSource = edStub
        sel.cam = camStub
        sel.view3d = viewStub
        sel.gizmo = gizStub
        sel.eachNode = function(cb) {}
        sel.tool = 0
    }

    function cleanup() {
        if (sel) { sel.destroy(); sel = null }
        if (camCtl) { camCtl.destroy(); camCtl = null }
    }

    function logged(log, prefix) {
        return log.filter(function(s) { return s.indexOf(prefix) === 0 })
    }

    /*---------------- SelectionController ----------------*/

    /* C1: Paint mode — a left drag on a device body must never
       promote to move/orbit/marquee: no transform calls, no camera
       calls. */
    function test_paint_drag_never_moves() {
        sel.tool = 2
        sel.beginPressAt(100, 100, Qt.LeftButton, 0,
                         "obj|kbd", Qt.vector3d(0, 0.02, 0.2))
        sel.dragTo(160, 140, 0)
        sel.dragTo(200, 180, 0)
        compare(sel.gesture, 0, "paint-mode drag promoted a gesture")
        compare(logged(edStub.log, "beginT").length, 0)
        compare(logged(edStub.log, "updT").length, 0)
        compare(logged(camStub.log, "pan").length, 0)
        compare(logged(camStub.log, "orbit").length, 0)
        sel.endGesture(200, 180, Qt.LeftButton, 0)
        compare(logged(edStub.log, "commitT").length, 0)
        compare(sel.pressed, false)
    }

    /* C1b: Alt held does not unlock orbit in Paint mode. */
    function test_paint_alt_drag_no_orbit() {
        camStub.ortho = false
        sel.tool = 2
        sel.beginPressAt(100, 100, Qt.LeftButton, Qt.AltModifier,
                         "obj|kbd", Qt.vector3d(0, 0.02, 0.2))
        sel.dragTo(160, 100, Qt.AltModifier)
        compare(sel.gesture, 0)
        compare(logged(camStub.log, "orbit").length, 0)
        sel.endGesture(160, 100, Qt.LeftButton, Qt.AltModifier)
    }

    /* C1c: empty-space drag in Paint mode never becomes marquee. */
    function test_paint_empty_drag_no_marquee() {
        sel.tool = 2
        sel.beginPressAt(100, 100, Qt.LeftButton, 0, "", null)
        sel.dragTo(300, 300, 0)
        compare(sel.gesture, 0)
        sel.endGesture(300, 300, Qt.LeftButton, 0)
        compare(logged(edStub.log, "selectInstance").length, 0)
        compare(logged(edStub.log, "clearSel").length, 0)
    }

    /* Paint drags keep painting emit| proxies. */
    function test_paint_emitter_drag_paints() {
        sel.tool = 2
        sel.beginPressAt(100, 100, Qt.LeftButton, 0,
                         "emit|kbd|3", Qt.vector3d(0, 0.02, 0.2))
        compare(sel.gesture, 6)
        viewStub.nextPick = objPick("emit|kbd|5")
        sel.dragTo(120, 100, 0)
        viewStub.nextPick = objPick("obj|kbd")
        sel.dragTo(140, 100, 0)
        sel.endGesture(140, 100, Qt.LeftButton, 0)
        var paints = logged(edStub.log, "paint:")
        compare(paints.length, 2)
        compare(paints[0], "paint:kbd|3")
        compare(paints[1], "paint:kbd|5")
    }

    /* Baseline: Ctrl + left drag on a device moves it — plain
       drags orbit now, so the transform needs the modifier. */
    function test_move_drag_moves() {
        sel.beginPressAt(100, 100, Qt.LeftButton, Qt.ControlModifier,
                         "obj|kbd", Qt.vector3d(0, 0.02, 0.2))
        sel.dragTo(160, 100, Qt.ControlModifier)  /* promotes past threshold */
        compare(sel.gesture, 2)
        compare(logged(edStub.log, "beginT").length, 1)
        sel.dragTo(170, 100, Qt.ControlModifier)  /* first post-promotion update */
        verify(logged(edStub.log, "updT").length > 0)
        sel.endGesture(160, 100, Qt.LeftButton, Qt.ControlModifier)
        compare(logged(edStub.log, "commitT").length, 1)
    }

    /* Plain left-drag orbits — even starting on a device — and an
       ortho preset promotes into the free view on the first drag. */
    function test_plain_drag_orbits_and_enters_free() {
        sel.beginPressAt(100, 100, Qt.LeftButton, 0,
                         "obj|kbd", Qt.vector3d(0, 0.02, 0.2))
        sel.dragTo(160, 100, 0)
        compare(sel.gesture, 5)
        compare(logged(camStub.log, "enterFree").length, 1,
                "ortho preset did not promote into free view")
        compare(logged(camStub.log, "orbit").length, 1)
        compare(logged(edStub.log, "beginT").length, 0,
                "plain drag on a device started a transform")
        sel.endGesture(160, 100, Qt.LeftButton, 0)
        compare(logged(camStub.log, "endPose").length, 1)
    }

    /* Ctrl+drag on EMPTY space is inert — and must NOT fall back
       to a click (it would clear the selection on release). */
    function test_ctrl_empty_drag_inert() {
        sel.beginPressAt(100, 100, Qt.LeftButton, Qt.ControlModifier,
                         "", null)
        sel.dragTo(300, 300, Qt.ControlModifier)
        compare(sel.gesture, 0)
        sel.endGesture(300, 300, Qt.LeftButton, Qt.ControlModifier)
        compare(logged(camStub.log, "orbit").length, 0)
        compare(logged(edStub.log, "select").length, 0,
                "inert ctrl+drag released into a click")
    }

    /* M2: a second press mid-gesture cancels — never commits a
       half-finished move. */
    function test_second_press_cancels() {
        sel.beginPressAt(100, 100, Qt.LeftButton, Qt.ControlModifier,
                         "obj|kbd", Qt.vector3d(0, 0.02, 0.2))
        sel.dragTo(160, 100, Qt.ControlModifier)
        compare(sel.gesture, 2)
        sel.beginPressAt(300, 100, Qt.LeftButton, 0, "", null)
        compare(sel.gesture, 0)
        compare(logged(edStub.log, "cancelT").length, 1)
        compare(logged(edStub.log, "commitT").length, 0)
        sel.endGesture(300, 100, Qt.LeftButton, 0)
        compare(logged(edStub.log, "commitT").length, 0)
    }

    /* D1: spaceDown tracks the physical key — gesture end and
       cancel() must NOT clear it (only key release / application
       deactivation do, in the host scene). Clearing it on reset()
       desynced from a still-held Space and, since held-key
       autorepeat is filtered, misrouted every second held-space
       drag to move/marquee. */
    function test_spaceDown_survives_gesture_end() {
        sel.spaceDown = true
        sel.cancel()
        compare(sel.spaceDown, true, "cancel desynced spaceDown")
        sel.beginPressAt(100, 100, Qt.LeftButton, 0,
                         "obj|kbd", Qt.vector3d(0, 0.02, 0.2))
        sel.dragTo(160, 100, 0)
        compare(sel.gesture, 1)          /* space+drag pans */
        sel.endGesture(160, 100, Qt.LeftButton, 0)
        compare(sel.spaceDown, true, "gesture end desynced spaceDown")
        compare(logged(camStub.log, "pan").length, 1)
        compare(logged(camStub.log, "endPose").length, 1)
        /* Second held-space drag must pan too — the regression. */
        sel.beginPressAt(200, 200, Qt.LeftButton, 0, "", null)
        sel.dragTo(260, 200, 0)
        compare(sel.gesture, 1, "second held-space drag did not pan")
        sel.endGesture(260, 200, Qt.LeftButton, 0)
        sel.spaceDown = false
    }

    /* D4: deferPose is armed at PRESS for every gesture-capable
       button — a wheel inside the click-candidate window can't
       persist a pose a later Escape would roll back. */
    function test_defer_pose_at_press() {
        sel.beginPressAt(100, 100, Qt.LeftButton, 0, "", null)
        compare(camStub.deferPose, true,
                "click-candidate press did not defer pose saves")
        sel.endGesture(100, 100, Qt.LeftButton, 0)
        compare(camStub.deferPose, false)
    }

    /* D4b (real CameraController): a wheel zoom deferred inside a
       non-camera gesture persists ONCE at gesture end — deferral is
       delay-to-release, not a drop — while Escape on a camera
       gesture drops it (the snapshot restore wins). */
    function test_deferred_zoom_flush_and_drop() {
        var c = makeRealCam()
        var saves = 0
        c.poseFinished.connect(function() { saves++ })
        sel.cam = c

        /* Wheel during a click-candidate -> move drag: deferred at
           zoom, flushed once at release. */
        sel.beginPressAt(100, 100, Qt.LeftButton, Qt.ControlModifier,
                         "obj|kbd", Qt.vector3d(0, 0.02, 0.2))
        compare(c.deferPose, true)
        sel.wheelAt(100, 100, 120)
        compare(saves, 0, "zoom persisted inside the press window")
        compare(c.deferredSave, true)
        sel.dragTo(160, 100, Qt.ControlModifier)
        compare(sel.gesture, 2)
        sel.endGesture(160, 100, Qt.LeftButton, 0)
        compare(saves, 1, "deferred zoom not flushed at release")
        compare(c.deferPose, false)

        /* Wheel during a pan, then Escape: snapshot restore wins —
           the deferred save is dropped, never persisted. */
        sel.beginPressAt(100, 100, Qt.MiddleButton, 0, "", null)
        compare(sel.gesture, 1)
        sel.wheelAt(100, 100, 120)
        compare(c.deferredSave, true)
        sel.cancel()
        compare(saves, 1, "deferred zoom persisted past a restore")
        compare(c.deferredSave, false)
    }

    /* D7: releasing a different button must not end the gesture —
       only the button that started the press does. */
    function test_cross_button_release_ignored() {
        sel.beginPressAt(100, 100, Qt.MiddleButton, 0, "", null)
        compare(sel.gesture, 1)
        sel.dragTo(140, 100, 0)
        sel.endGesture(140, 100, Qt.LeftButton, 0)   /* wrong button */
        compare(sel.pressed, true, "foreign release ended the gesture")
        compare(logged(camStub.log, "endPose").length, 0)
        sel.endGesture(140, 100, Qt.MiddleButton, 0)
        compare(sel.pressed, false)
        compare(logged(camStub.log, "endPose").length, 1)
    }

    /* M4: middle_drag pref — "pan" pans, "orbit" orbits; under an
       ortho preset the first orbit drag promotes into free view.
       Read through the explicit stateSource seam. */
    function test_middle_drag_pref() {
        /* default pan */
        sel.beginPressAt(100, 100, Qt.MiddleButton, 0,
                         "obj|kbd", null)
        compare(sel.gesture, 1)
        sel.dragTo(140, 100, 0)
        compare(logged(camStub.log, "pan").length, 1)
        sel.endGesture(140, 100, Qt.MiddleButton, 0)
        compare(logged(camStub.log, "endPose").length, 1)

        /* orbit honored in free view */
        edStub.cameraState = { "middle_drag": "orbit" }
        camStub.ortho = false
        sel.beginPressAt(100, 100, Qt.MiddleButton, 0,
                         "obj|kbd", null)
        compare(sel.gesture, 5)
        sel.dragTo(140, 100, 0)
        compare(logged(camStub.log, "orbit").length, 1)
        sel.endGesture(140, 100, Qt.MiddleButton, 0)
        compare(logged(camStub.log, "endPose").length, 2)

        /* orbit pref under an ortho preset still orbits — the first
           drag promotes into the free view. */
        camStub.ortho = true
        sel.beginPressAt(100, 100, Qt.MiddleButton, 0,
                         "obj|kbd", null)
        compare(sel.gesture, 5)
        sel.dragTo(140, 100, 0)
        compare(logged(camStub.log, "enterFree").length, 1)
        sel.endGesture(140, 100, Qt.MiddleButton, 0)

        /* Paint mode forbids orbit even when the pref asks for it —
           degrades to pan. */
        camStub.ortho = false
        sel.tool = 2
        sel.beginPressAt(100, 100, Qt.MiddleButton, 0,
                         "obj|kbd", null)
        compare(sel.gesture, 1)
        sel.endGesture(100, 100, Qt.MiddleButton, 0)
        sel.tool = 0
    }

    /* Middle pan on a device must never move the device. */
    function test_middle_pan_never_moves() {
        sel.beginPressAt(100, 100, Qt.MiddleButton, 0,
                         "obj|kbd", Qt.vector3d(0, 0.02, 0.2))
        sel.dragTo(160, 140, 0)
        compare(sel.gesture, 1)
        compare(logged(edStub.log, "beginT").length, 0)
        sel.endGesture(160, 140, Qt.MiddleButton, 0)
        compare(logged(edStub.log, "commitT").length, 0)
    }

    /* I1: shift+drag marquee keeps enclosed instances and ONLY
       those (left drag is orbit — marquee moved to Shift). */
    function test_marquee_excludes_outside() {
        var nodes = [
            { visible: true, oKind: "device", oInst: "mouse",
              scenePosition: Qt.vector3d(0.0, 0.0, 0.0) },
            { visible: true, oKind: "device", oInst: "kbd",
              scenePosition: Qt.vector3d(5.0, 0.0, 0.0) }
        ]
        viewStub._map = function(p) {
            return p.x > 1.0 ? Qt.vector3d(400, 400, 0)
                             : Qt.vector3d(100, 100, 0)
        }
        sel.eachNode = function(cb) { for (var i = 0; i < nodes.length; i++) cb(nodes[i]) }
        sel.beginPressAt(50, 50, Qt.LeftButton, Qt.ShiftModifier, "", null)
        sel.dragTo(200, 200, Qt.ShiftModifier)
        compare(sel.gesture, 4)
        sel.endGesture(200, 200, Qt.LeftButton, Qt.ShiftModifier)
        compare(edStub.selectedInstances.length, 1)
        compare(edStub.selectedInstances[0], "mouse")
    }

    /* Escape restores the transform snapshot; no commit. */
    function test_escape_restores() {
        sel.beginPressAt(100, 100, Qt.LeftButton, Qt.ControlModifier,
                         "obj|kbd", Qt.vector3d(0, 0.02, 0.2))
        sel.dragTo(160, 100, Qt.ControlModifier)
        compare(sel.gesture, 2)
        verify(sel.cancel())
        compare(logged(edStub.log, "cancelT").length, 1)
        compare(logged(edStub.log, "commitT").length, 0)
        compare(sel.gesture, 0)
    }

    /*---------------- CameraController -----------------*/

    function makeRealCam() {
        camCtl = camComp.createObject(tc)
        camCtl.view3d = makeView()
        camCtl.origin = {
            position: Qt.vector3d(0, 0, 0),
            eulerRotation: Qt.vector3d(0, 0, 0),
            right: Qt.vector3d(1, 0, 0),
            up: Qt.vector3d(0, 1, 0),
            forward: Qt.vector3d(0, 0, -1)
        }
        camCtl.orthoCam = { horizontalMagnification: 0,
                            verticalMagnification: 0,
                            position: Qt.vector3d(0, 0, 0) }
        camCtl.perspCam = { position: Qt.vector3d(0, 0, 0),
                            fieldOfView: 60 }
        return camCtl
    }

    /* M1: wheel mid-gesture must not persist a half-gesture pose. */
    function test_wheel_pose_deferred() {
        var c = makeRealCam()
        var saves = 0
        c.poseFinished.connect(function() { saves++ })
        c.zoomAt(250, 250, 120)
        compare(saves, 1)
        c.deferPose = true
        c.zoomAt(250, 250, 120)
        c.zoomAt(250, 250, -120)
        compare(saves, 1, "zoom persisted mid-gesture")
        c.endPose()
        compare(saves, 2, "gesture end did not persist once")
    }

    /* M5: applyView("case") persists even with no case instance —
       the view change is still an editor pref. */
    function test_case_view_persists_without_instance() {
        var c = makeRealCam()
        var saves = 0
        c.poseFinished.connect(function() { saves++ })
        c.boundsOf = function(ids) { return null }
        c.applyView("case")
        compare(c.viewName, "case")
        compare(saves, 1)
    }

    function test_frameIds_persists() {
        var c = makeRealCam()
        var saves = 0
        c.poseFinished.connect(function() { saves++ })
        c.boundsOf = function(ids) {
            return { c: Qt.vector3d(0.2, 0.05, 0.3), r: 0.4 }
        }
        verify(c.frameIds(["kbd"]))
        compare(c.target.x, 0.2)
        compare(saves, 1)
    }

    /* Persisted pose round-trips through the prefsSource seam —
       the WorkspaceController's cameraState in production. */
    function test_applyPersisted_restores() {
        var c = makeRealCam()
        var saved = null
        c.prefsSource = {
            cameraState: { "view": "free", "projection": "perspective",
                           "tx": 0.4, "ty": 0.2, "tz": -0.1,
                           "yaw": 30, "pitch": -20,
                           "distance": 2.5, "span": 0.9 }
        }
        c.applyPersisted()
        compare(c.viewName, "free")
        compare(c.ortho, false)
        compare(c.target.x, 0.4)
        compare(c.yawDeg, 30)
        compare(c.pitchDeg, -20)
        compare(c.distance, 2.5)
    }

    /*---------------- 5.2 pick mode ------------------*/

    /* Escape while a pick is ARMED but not pressed must disarm
       BOTH sides — pickMode AND pickTarget — and hand the owner a
       cancel so its armed UI clears. */
    function test_pick_escape_disarms() {
        var calls = []
        sel.pickMode = "origin"
        sel.pickPlaneY = 0.02
        sel.pickTarget = function(pt, phase) { calls.push(phase) }
        verify(sel.cancel(), "an armed pick consumes the Escape")
        compare(sel.pickMode, "")
        verify(sel.pickTarget === null, "pickTarget released")
        compare(calls.join(","), "cancel", "owner was told")
    }

    /* Mid-press Escape still routes the cancel through
       deliverPick before disarming. */
    function test_pick_midpress_escape() {
        var calls = []
        sel.pickMode = "origin"
        sel.pickPlaneY = 0.02
        sel.pickTarget = function(pt, phase) { calls.push(phase) }
        sel.beginPressAt(100, 100, Qt.LeftButton, 0, "", null)
        compare(sel.gesture, 7, "press in pick mode = gPick")
        compare(calls.join(","), "begin")
        sel.cancel()
        compare(sel.pickMode, "")
        verify(calls.join(",").indexOf("cancel") >= 0,
               "cancel delivered to the owner")
    }

    /* A lifecycle phase must land even when the ray misses the
       plane — dropping "cancel"/"end" would leak the owner's
       effect gesture (begin already ran). */
    function test_pick_cancel_without_plane_hit() {
        var calls = []
        camStub.planeHitPoint = function(x, y, ax, v) { return null }
        sel.pickMode = "origin"
        sel.pickTarget = function(pt, phase) {
            calls.push(phase + ":" + (pt === null))
        }
        sel.deliverPick(10, 10, "cancel")
        compare(calls.join(","), "cancel:true",
                "cancel landed with a null point")
        sel.deliverPick(10, 10, "begin")
        compare(calls.length, 1, "begin without a hit is dropped")
        sel.pickMode = ""
        sel.pickTarget = null
    }

    /*---------------- Compile smoke ------------------*/

    /* Every viewport QML file must compile — caught here before
       the page ever tries the Loader. Paths resolve against the
       source tree (the same files the module packs). */
    function test_qml_files_compile() {
        var ui = "../../../ui/studio/viewport/"
        var files = [ "StudioViewport.qml",
                      "editor/CameraController.qml",
                      "editor/SelectionController.qml",
                      "editor/TransformGizmo.qml",
                      "devices/Case.qml", "devices/Fan.qml",
                      "devices/Gpu.qml", "devices/Keyboard.qml",
                      "devices/Monitor.qml", "devices/Mouse.qml",
                      "devices/Ram.qml", "devices/SelectionFrame.qml",
                      "devices/Strip.qml",
                      "materials/StudioEnvironment.qml" ]
        for (var i = 0; i < files.length; i++) {
            var c = Qt.createComponent(Qt.resolvedUrl(ui + files[i]))
            compare(c.status, Component.Ready,
                    files[i] + ": " + c.errorString())
        }
    }
}
