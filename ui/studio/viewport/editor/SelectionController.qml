/*---------------------------------------------------------*\
|| SelectionController.qml                                 ||
||                                                         ||
||   The ONE input router for the desk viewport. Every    ||
||   pointer press lands in press() -> beginPressAt();    ||
||   drags promote past a ~4 px threshold into exactly    ||
||   one gesture:                                         ||
||     middle drag            -> camera pan (wins over    ||
||                               everything, any surface) ||
||     space + left drag      -> camera pan (alternative) ||
||     left drag              -> orbit (promotes ortho    ||
||                               presets into free view)  ||
||     ctrl + left drag       -> move / rotate a device   ||
||                               or ring (by tool)        ||
||     shift + left drag      -> marquee                  ||
||     left drag in Paint     -> emitter painting only    ||
||   A press released under the threshold is a click:     ||
||   select, ctrl-additive toggle, or empty-space clear.  ||
||   Escape -> cancel(): transform gestures restore the   ||
||   begin snapshot via editor.cancelTransformGesture,    ||
||   pan/orbit restore the pose snapshot, marquee just    ||
||   clears.                                              ||
||                                                         ||
||   Adapted for OpenRGB2 (studio-import Task 8) from     ||
||   plugins/DesktopLightingStudio/ui/editor/             ||
||   SelectionController.qml: the implicit `bridge`       ||
||   context property is replaced by the explicit         ||
||   `editor` (ObjectEditorController) and `stateSource`  ||
||   (WorkspaceController — camera/controls prefs)        ||
||   properties.                                          ||
||                                                         ||
||   Test seam: beginPressAt/dragTo/endGesture take       ||
||   explicit hit info, so the test suite can drive the   ||
||   same routing without synthesizing real mouse events. ||
||   `editor` accepts a plain-JS stub — the suite drives  ||
||   the routing with fakes before the real controller    ||
||   end-to-end cases.                                    ||
||                                                         ||
||   SPDX-License-Identifier: GPL-2.0-or-later            ||
\*---------------------------------------------------------*/
import QtQuick

Item {
    id: ctl
    objectName: "selCtl"

    property var view3d: null
    property var cam: null          /* CameraController        */
    property var gizmo: null        /* TransformGizmo          */
    /* eachNode(cb) — iterate the live object-node map. */
    property var eachNode: null
    /* The mutation surface — ObjectEditorController in
       production (select/selectInstance/clearEditorSelection,
       begin/update/commit/cancel transform gestures, undo/redo,
       paintEmitter, addDeviceInstance, selectedInstances); a
       plain-JS stub in tests. */
    property var editor: null
    /* Where controls prefs live — WorkspaceController in
       production (cameraState.middle_drag); falls back to the
       camera's own prefsSource when unset, then "pan". */
    property var stateSource: null

    property int  tool: 0           /* 0 move, 1 rotate, 2 paint */
    property int  editPlane: 1      /* 0 free 1 deskXZ 2 frontXY 3 sideYZ */
    property bool snapEnabled: false
    property bool spaceDown: false
    property real clickPx: 4.0

    /* Gesture ids (mirrored as ints for the C++ test). */
    readonly property int gNone:    0
    readonly property int gPan:     1
    readonly property int gMove:    2
    readonly property int gRotate:  3
    readonly property int gMarquee: 4
    readonly property int gOrbit:   5
    readonly property int gPaint:   6
    readonly property int gPick:    7

    /* Effect pick mode, armed by the host's effect editor
       surface: "origin" | "path". While armed a left press
       becomes a pick gesture instead of select/move/marquee —
       exclusive like Paint (it can never move devices); middle-
       button pan still wins. Picks land on the desk plane at
       pickPlaneY (the layer origin's height) through
       cam.planeHitPoint — the same math the move gesture and
       device-drop use. pickTarget(pt, phase) receives
       "begin" | "drag" | "end" | "cancel"; the callback owns the
       undo bookkeeping (one effect gesture per pick). */
    property string pickMode: ""        /* "" | "origin" | "path" */
    property real   pickPlaneY: 0.0
    property var    pickTarget: null    /* fn(vector3d pt, string phase) */

    function deliverPick(x, y, phase) {
        if (pickTarget === null)
            return
        var p = null
        if (cam) {
            p = cam.planeHitPoint(x, y, 1, pickPlaneY)
            if (!p && cam.target)
                p = cam.planeHitPoint(x, y, -1, cam.target)
        }
        /* Lifecycle phases must land even without a plane hit —
           dropping "cancel"/"end" here would leak the owner's
           effect gesture (begin already ran). pt is null; the
           callback still commits/cancels. */
        if (!p) {
            if (phase === "cancel" || phase === "end")
                pickTarget(null, phase)
            return
        }
        pickTarget(p, phase)
    }

    property int    gesture: gNone
    property bool   pressed: false
    property point  pressPos: Qt.point(0, 0)
    property point  lastPos: Qt.point(0, 0)
    property int    pressButton: 0
    property int    pressMods: 0
    property string pressHit: ""     /* objectName hit at press, "" empty */
    property string hitObject: ""    /* object id owning pressHit        */
    property var    startHit: null   /* world-space press point          */
    property var    camSnapshot: null
    property int    planeAxis: 1
    property real   planeValue: 0
    property real   startAngle: 0
    property rect   marqueeRect: Qt.rect(0, 0, 0, 0)
    property string lastPainted: ""

    function ed() { return editor }
    function hasEditor() { return editor !== null }

    /* controls.middle_drag — "pan" (default) | "orbit". Read
       through stateSource (workspace meta.controls) or the
       camera's own prefs source. */
    function middleDragPref() {
        var src = stateSource
        if ((!src || src.cameraState === undefined)
            && cam && cam.prefsSource)
            src = cam.prefsSource
        var s = src ? src.cameraState : null
        return (s && s.middle_drag !== undefined)
               ? s.middle_drag : "pan"
    }

    function ownerOf(hitName) {
        if (!hitName)
            return ""
        if (hitName.indexOf("emit|") === 0)
            return hitName.split("|")[1]
        if (hitName.indexOf("obj|") === 0)
            return hitName.substring(4)
        return ""
    }

    function instOf(objectId) {
        var s = objectId.indexOf("/")
        return s < 0 ? objectId : objectId.substring(0, s)
    }

    function snapNow(mods) {
        return snapEnabled || ((mods & Qt.ShiftModifier) !== 0)
    }

    function pickPos(r) {
        if (r.scenePosition !== undefined)
            return r.scenePosition
        if (r.position !== undefined)
            return r.position
        return null
    }

    function normRect(a, b) {
        var x0 = Math.min(a.x, b.x), y0 = Math.min(a.y, b.y)
        return Qt.rect(x0, y0, Math.abs(a.x - b.x), Math.abs(a.y - b.y))
    }

    /*-----------------------------------------------------*\
    || Press — real input picks first; the test suite calls ||
    || beginPressAt directly with the resolved hit.         ||
    \*-----------------------------------------------------*/
    function press(x, y, button, mods) {
        var name = "", pt = null
        if (view3d) {
            var r = view3d.pick(x, y)
            if (r && r.objectHit) {
                name = r.objectHit.objectName
                pt   = pickPos(r)
            }
        }
        beginPressAt(x, y, button, mods, name, pt)
    }

    function beginPressAt(x, y, button, mods, hitName, hitPos) {
        /* A second press mid-gesture cancels the live one — a release
           here would commit a half-finished move. */
        if (pressed)
            cancel()
        pressed     = true
        gesture     = gNone
        pressPos    = Qt.point(x, y)
        lastPos     = pressPos
        pressButton = button
        pressMods   = mods
        pressHit    = hitName ? hitName : ""
        hitObject   = ownerOf(pressHit)
        startHit    = hitPos
        camSnapshot = cam ? cam.poseSnapshot() : null
        lastPainted = ""
        marqueeRect = Qt.rect(0, 0, 0, 0)
        /* Defer pose persistence for the WHOLE press window, not just
           promoted pan/orbit: a wheel inside the click-candidate
           window must not write a pose a later Escape would roll
           back. Cleared in reset(). */
        if (cam) cam.deferPose = true

        /* Middle pan wins over every surface and every tool. The
           middle_drag controls pref can redirect it to orbit —
           ortho presets promote into the free view on the first
           drag (enterFreeView). Paint mode still forbids orbit:
           the pref degrades to pan there. */
        if (button === Qt.MiddleButton) {
            var md = middleDragPref()
            gesture = (md === "orbit" && tool !== 2 && cam)
                    ? gOrbit : gPan
            return
        }
        if (button !== Qt.LeftButton)
            return

        /* Effect pick mode: the press is already the gesture — a
           click delivers begin+end, a drag streams "drag" points
           (origin scrub / waypoint reposition). No selection, no
           move, no marquee. */
        if (pickMode !== "") {
            gesture = gPick
            deliverPick(x, y, "begin")
            return
        }

        if (tool === 2) {
            /* Paint mode: pointer gestures can never start movement
               or camera orbit — only emitter painting. */
            if (pressHit.indexOf("emit|") === 0) {
                paintHit(pressHit)
                gesture = gPaint
            }
            return
        }
        /* Move/Rotate: stay a click candidate until dragTo() trips
           the threshold. */
    }

    /*-----------------------------------------------------*\
    || Drag                                                 ||
    \*-----------------------------------------------------*/
    function rotateAxis() {
        if (editPlane === 1)
            return Qt.vector3d(0, 1, 0)
        if (editPlane === 2)
            return Qt.vector3d(0, 0, 1)
        if (editPlane === 3)
            return Qt.vector3d(1, 0, 0)
        return cam && cam.origin ? cam.origin.forward : Qt.vector3d(0, 1, 0)
    }

    function beginTransform() {
        if (!hasEditor())
            return false
        var b = ed()
        b.beginTransformGesture()
        if (b.gestureActive && !b.gestureActive())
            return false
        if (!startHit)
            startHit = cam ? cam.pointUnderPointer(pressPos.x, pressPos.y) : null
        if (editPlane === 1)      { planeAxis = 1; planeValue = startHit ? startHit.y : 0 }
        else if (editPlane === 2) { planeAxis = 2; planeValue = startHit ? startHit.z : 0 }
        else if (editPlane === 3) { planeAxis = 0; planeValue = startHit ? startHit.x : 0 }
        else                      { planeAxis = -1 }
        return true
    }

    function planePoint(x, y) {
        if (!cam)
            return null
        if (planeAxis < 0) {
            if (!startHit)
                return null
            return cam.planeHitPoint(x, y, -1, startHit)
        }
        return cam.planeHitPoint(x, y, planeAxis, planeValue)
    }

    function ensureHitSelected(mods) {
        if (!hasEditor() || hitObject === "")
            return
        var inst = instOf(hitObject)
        if (ed().selectedInstances.indexOf(inst) >= 0)
            return
        if (mods & Qt.ControlModifier)
            ed().selectInstance(inst, true)
        else
            ed().select(hitObject)
    }

    function startRotate() {
        if (gizmo)
            gizmo.updatePivot()
        startAngle = gizmo ? gizmo.angleAt(pressPos.x, pressPos.y) : 0
    }

    function dragTo(x, y, mods) {
        if (!pressed)
            return
        var dx = x - lastPos.x
        var dy = y - lastPos.y
        lastPos = Qt.point(x, y)

        if (gesture === gPan) {
            if (cam) cam.panPixels(dx, dy)
            return
        }
        if (gesture === gOrbit) {
            if (cam) {
                if (cam.ortho && typeof cam.enterFreeView === "function")
                    cam.enterFreeView()
                cam.orbitPixels(dx, dy)
            }
            return
        }
        if (gesture === gPaint) {
            if (view3d) {
                var rp = view3d.pick(x, y)
                if (rp && rp.objectHit
                    && rp.objectHit.objectName.indexOf("emit|") === 0)
                    paintHit(rp.objectHit.objectName)
            }
            return
        }
        if (gesture === gPick) {
            deliverPick(x, y, "drag")
            return
        }
        if (gesture === gMove) {
            var hp = planePoint(x, y)
            if (hp && startHit && hasEditor()) {
                var d = hp.minus(startHit)
                ed().updateTransformGesture(d.x, d.y, d.z,
                                            editPlane, snapNow(mods))
            }
            return
        }
        if (gesture === gRotate) {
            var a  = gizmo ? gizmo.angleAt(x, y) : 0
            var ax = rotateAxis()
            if (hasEditor())
                ed().updateRotateGestureAxis(ax.x, ax.y, ax.z,
                                             a - startAngle,
                                             snapNow(mods))
            return
        }
        if (gesture === gMarquee) {
            marqueeRect = normRect(pressPos, lastPos)
            return
        }

        /* Still a click candidate — the threshold decides. */
        if (pressButton !== Qt.LeftButton)
            return
        var mx = x - pressPos.x
        var my = y - pressPos.y
        if (Math.sqrt(mx * mx + my * my) < clickPx)
            return

        /* Paint mode: a left-drag that started outside an emit| proxy
           can NEVER promote into movement, orbit or marquee — pointer
           gestures in Paint do nothing but paint. */
        if (tool === 2)
            return

        /* Gesture promotion, in contract order. deferPose is already
           armed from beginPressAt. */
        if (spaceDown) {
            gesture = gPan
            if (cam) cam.panPixels(mx, my)
            return
        }
        if (mods & Qt.ControlModifier) {
            /* Ctrl+drag = scene gestures: the rotate ring, or a
               device move/rotate by tool. On empty space it stays
               inert — unless Shift is also held (Ctrl+Shift still
               marquees). */
            if (tool === 1 && gizmo && gizmo.ringHit(pressPos.x, pressPos.y)
                && hasEditor() && ed().selectedInstances.length > 0) {
                if (beginTransform()) {
                    startRotate()
                    gesture = gRotate
                }
                return
            }
            if (hitObject !== "") {
                ensureHitSelected(mods)
                if (beginTransform()) {
                    if (tool === 1) {
                        startRotate()
                        gesture = gRotate
                    } else {
                        gesture = gMove
                    }
                }
                return
            }
            if ((mods & Qt.ShiftModifier) === 0)
                return
        }
        if (mods & Qt.ShiftModifier) {
            /* Shift+drag = marquee, on any surface. */
            gesture = gMarquee
            marqueeRect = normRect(pressPos, lastPos)
            return
        }
        /* Plain left-drag orbits — ortho presets promote into the
           perspective free view on the first drag. */
        gesture = gOrbit
        if (cam) {
            if (cam.ortho && typeof cam.enterFreeView === "function")
                cam.enterFreeView()
            cam.orbitPixels(mx, my)
        }
    }

    /*-----------------------------------------------------*\
    || Release / cancel                                     ||
    \*-----------------------------------------------------*/
    function endGesture(x, y, button, mods) {
        if (!pressed)
            return
        /* Only the button that STARTED the press ends it — a release
           of a different button (e.g. letting go of left while a
           middle-pan is still held) must not commit/reset early. */
        if (button !== pressButton)
            return
        lastPos = Qt.point(x, y)
        var g = gesture
        if (g === gMove || g === gRotate) {
            if (hasEditor())
                ed().commitTransformGesture()
        } else if (g === gMarquee) {
            var r = normRect(pressPos, lastPos)
            selectMarquee(r.x, r.y, r.x + r.width, r.y + r.height)
        } else if (g === gPan || g === gOrbit) {
            if (cam)
                cam.endPose()     /* persist final pose */
        } else if (g === gPick) {
            /* One completed pick gesture — the callback folds it
               into a single undo command. */
            deliverPick(x, y, "end")
        } else if (g === gNone && button === Qt.LeftButton
                   && pressButton === Qt.LeftButton) {
            /* A drag that never promoted (ctrl+empty drag, Paint on
               a non-emitter) is no longer a click — the threshold
               still decides. */
            var rx = x - pressPos.x, ry = y - pressPos.y
            if (Math.sqrt(rx * rx + ry * ry) < clickPx)
                clickAt(mods)
        }
        /* A wheel zoom deferred during the press window persists
           once here (pan/orbit already saved via endPose above —
           its deferredSave flag is cleared there). */
        flushCamPose()
        reset()
    }

    function clickAt(mods) {
        if (!hasEditor())
            return
        if (hitObject === "") {
            ed().select("")                /* empty space clears */
            return
        }
        if (mods & Qt.ControlModifier)
            ed().selectInstance(instOf(hitObject), true)
        else
            ed().select(hitObject)
    }

    /* Escape — restore pre-gesture state, zero history. */
    function cancel() {
        var g = gesture
        var wasPressed = pressed
        if (g === gMove || g === gRotate) {
            if (hasEditor())
                ed().cancelTransformGesture()
            flushCamPose()        /* camera pose isn't restored — a
                                     deferred wheel zoom still saves */
        } else if (g === gPan || g === gOrbit) {
            if (cam)
                cam.restorePose(camSnapshot)   /* no save; the snapshot
                                                  wins over any
                                                  deferred zoom */
        } else {
            if (g === gPick)
                deliverPick(lastPos.x, lastPos.y, "cancel")
            flushCamPose()        /* gNone/gPaint/gMarquee — nothing
                                     camera-side to restore */
        }
        /* Escape also exits an ARMED pick mode (press already over)
           — "Escape exits" per the pick contract. Hand the owner a
           cancel so its armed state clears too (the effect
           editor's armedPick highlight + its pickTarget); with no
           live gesture the editor cancel is a no-op. Then clear
           our side regardless. */
        var disarmedPick = pickMode !== ""
        if (disarmedPick) {
            if (pickTarget !== null)
                pickTarget(null, "cancel")
            pickMode = ""
            pickTarget = null
        }
        reset()
        return wasPressed || g !== gNone || disarmedPick
    }

    /* Persist a wheel-zoom pose deferred during the press window —
       no-op when the camera doesn't track one (test stubs). */
    function flushCamPose() {
        if (cam && typeof cam.flushDeferredPose === "function")
            cam.flushDeferredPose()
    }

    function reset() {
        gesture     = gNone
        pressed     = false
        /* spaceDown is NOT cleared here — it mirrors the physical
           Space key and is synced only by key press/release and
           application deactivation (StudioViewport). Clearing it on
           gesture end would desync from a still-held Space (held-key
           autorepeat is filtered out, so nothing would re-arm it) and
           misroute the next left-drag to move/marquee. */
        pressHit    = ""
        hitObject   = ""
        startHit    = null
        marqueeRect = Qt.rect(0, 0, 0, 0)
        lastPainted = ""
        if (cam) { cam.deferPose = false; cam.deferredSave = false }
    }

    /*-----------------------------------------------------*\
    || Non-drag entry points                                ||
    \*-----------------------------------------------------*/
    function wheelAt(x, y, angleDelta) {
        if (cam)
            cam.zoomAt(x, y, angleDelta)
    }

    function setTool(t) {
        if (t === tool)
            return
        cancel()
        tool = t
    }

    function paintHit(name) {
        if (!hasEditor() || name === lastPainted)
            return
        var p = name.split("|")
        if (p.length < 3)
            return
        lastPainted = name
        ed().paintEmitter(p[1], parseInt(p[2]), ed().paintColor)
    }

    /* Marquee: project every non-decor node and keep the enclosed
       instance ids. */
    function selectMarquee(x0, y0, x1, y1) {
        if (!hasEditor() || !view3d || !eachNode)
            return
        var lo = Math.min(x0, x1), hi = Math.max(x0, x1)
        var to = Math.min(y0, y1), bo = Math.max(y0, y1)
        var ids = []
        eachNode(function(item) {
            if (!item || !item.visible || item.oKind === "decor")
                return
            var p = view3d.mapFrom3DScene(item.scenePosition)
            if (p.x >= lo && p.x <= hi && p.y >= to && p.y <= bo
                && ids.indexOf(item.oInst) < 0)
                ids.push(item.oInst)
        })
        ed().clearEditorSelection()
        for (var k = 0; k < ids.length; k++)
            ed().selectInstance(ids[k], true)
    }

    function frameSelection() {
        if (hasEditor() && cam && ed().selectedInstances.length > 0)
            cam.frameIds(ed().selectedInstances)
    }

    function frameAll() {
        if (cam)
            cam.frameIds(null)
    }
}
