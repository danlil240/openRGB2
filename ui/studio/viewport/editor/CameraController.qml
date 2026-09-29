/*---------------------------------------------------------*\
|| CameraController.qml                                    ||
||                                                         ||
||   Editor camera rig: orthographic preset views (desk,  ||
||   top, front, case) plus an optional perspective free  ||
||   view; middle-button/space pan, pointer-centered      ||
||   bounded zoom, frame-selection. The rig is a Node     ||
||   (origin) whose eulerRotation orients the active      ||
||   camera child — camera nodes may use eulerRotation    ||
||   (view-only; the object-transform ban doesn't apply). ||
||   Every finished gesture emits poseFinished -> the     ||
||   host persists the pose through prefsSource (the      ||
||   WorkspaceController's setCameraState — meta.camera   ||
||   is an editor pref: never undo history, never the     ||
||   scene document's object history).                    ||
||                                                         ||
||   Adapted for OpenRGB2 (studio-import Task 8) from     ||
||   plugins/DesktopLightingStudio/ui/editor/             ||
||   CameraController.qml: the implicit `bridge` context  ||
||   property is replaced by the explicit `prefsSource`   ||
||   property (WorkspaceController.cameraState /          ||
||   setCameraState / cameraChanged).                     ||
||                                                         ||
||   SPDX-License-Identifier: GPL-2.0-or-later            ||
\*---------------------------------------------------------*/
import QtQuick

QtObject {
    id: cam
    objectName: "camCtl"

    /* Wired by the scene. */
    property var view3d: null
    property var origin: null        /* rig Node                        */
    property var orthoCam: null
    property var perspCam: null
    /* boundsOf(ids|null) -> { c: vector3d, r: real } over the object
       node map; null = whole scene. Supplied by StudioViewport. */
    property var boundsOf: null
    /* Persistence surface — the shared WorkspaceController in
       production (cameraState / setCameraState / cameraChanged);
       a plain-JS {cameraState: {...}} stub in tests. */
    property var prefsSource: null

    property string viewName: "desk"   /* desk|top|front|case|free */
    readonly property bool ortho: viewName !== "free"

    /* Pose — mirrored into meta.camera on gesture end. */
    property vector3d target: Qt.vector3d(0.1, 0.18, 0.05)
    property real yawDeg: 0
    property real pitchDeg: -38
    property real distance: 1.21      /* perspective dolly (m) */
    property real span: 0.9           /* ortho vertical extent (m) */

    readonly property real spanMin: 0.05
    readonly property real spanMax: 6.0
    readonly property real distMin: 0.06
    readonly property real distMax: 12.0

    /* Bumped on every apply() so projected overlays (gizmo ring)
       can re-evaluate their bindings. */
    property int poseStamp: 0

    /* True while any press window is open (SelectionController arms
       it at every press) — zoomAt must not persist a mid-gesture
       pose (Escape would restore the snapshot after prefs already
       saved it). Pan/orbit persist once at release via endPose;
       other gestures flush a deferred wheel-zoom save through
       flushDeferredPose. */
    property bool deferPose: false
    /* A poseFinished skipped while deferred — flushed once at
       gesture end; cancel drops it when a snapshot restore wins. */
    property bool deferredSave: false

    signal poseFinished()

    function clampNum(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v) }

    /* Push the pose into the rig + both camera nodes. */
    function apply() {
        if (!origin)
            return
        origin.position = target
        origin.eulerRotation = Qt.vector3d(pitchDeg, yawDeg, 0)
        if (orthoCam) {
            var mag = (view3d && view3d.height > 0)
                    ? view3d.height / span : 500.0
            orthoCam.horizontalMagnification = mag
            orthoCam.verticalMagnification = mag
            /* Keep the ortho rig well outside the geometry — its
               position doesn't scale the image, but near/far still
               clip. */
            orthoCam.position = Qt.vector3d(0, 0, Math.max(2.0, span * 3.0))
        }
        if (perspCam) {
            perspCam.position = Qt.vector3d(0, 0, distance)
        }
        if (view3d) {
            view3d.camera = ortho ? orthoCam : perspCam
        }
        poseStamp++
    }

    /*-----------------------------------------------------*\
    || View presets.                                        ||
    \*-----------------------------------------------------*/
    function applyView(name) {
        if (name !== "desk" && name !== "top" && name !== "front"
            && name !== "case" && name !== "free")
            return
        viewName = name
        if (name === "desk") {
            target   = Qt.vector3d(0.1, 0.18, 0.05)
            yawDeg   = 0
            pitchDeg = -38
            span     = 0.9
        } else if (name === "top") {
            /* -89.9 keeps a defined up-vector (no degenerate -90). */
            pitchDeg = -89.9
            yawDeg   = 0
            if (boundsOf) {
                var b = boundsOf(null)
                if (b) { target = Qt.vector3d(b.c.x, target.y, b.c.z)
                         span = clampNum(b.r * 2.2, spanMin, spanMax) }
            }
        } else if (name === "front") {
            pitchDeg = 0
            yawDeg   = 0
            if (boundsOf) {
                var bf = boundsOf(null)
                if (bf) target = bf.c
            }
        } else if (name === "case") {
            pitchDeg = -30
            yawDeg   = 0
            apply()
            /* frameIds saves the pose on success; a workspace without
               a "case" instance still persists the view change. */
            if (!frameIds(["case"]))
                poseFinished()
            return
        }
        /* "free" keeps the current pose; projection flips to the
           perspective camera via apply(). */
        apply()
        poseFinished()
    }

    /*-----------------------------------------------------*\
    || Frame — move target to a bounds center and size the  ||
    || view to enclose it. ids = instance ids; null = all.  ||
    \*-----------------------------------------------------*/
    function frameIds(ids) {
        if (!boundsOf)
            return false
        var b = boundsOf(ids)
        if (!b || b.r <= 0)
            return false
        target = b.c
        var r = Math.max(b.r, 0.05)
        if (ortho) {
            span = clampNum(r * 2.6, spanMin, spanMax)
        } else {
            var halfFov = (perspCam ? perspCam.fieldOfView : 60.0)
                        * Math.PI / 360.0
            distance = clampNum(r * 1.25 / Math.tan(halfFov),
                                distMin, distMax)
        }
        apply()
        poseFinished()
        return true
    }

    /*-----------------------------------------------------*\
    || Pan / orbit / zoom.                                  ||
    \*-----------------------------------------------------*/
    function metersPerPixel() {
        if (!view3d || view3d.height <= 0)
            return 0.001
        if (ortho)
            return span / view3d.height
        var fov = (perspCam ? perspCam.fieldOfView : 60.0) * Math.PI / 180.0
        return 2.0 * distance * Math.tan(fov / 2.0) / view3d.height
    }

    /* Translate the target inside the view plane — middle-drag and
       space+drag both land here. Never touches object transforms. */
    function panPixels(dx, dy) {
        if (!origin)
            return
        var mpp = metersPerPixel()
        target = target.plus(origin.right.times(-dx * mpp))
                       .plus(origin.up.times(dy * mpp))
        apply()
    }

    /* Free-view orbit (left-drag, perspective only — ortho
       callers go through enterFreeView first). Yaw follows
       the pointer; pitch runs inverted (drag up tilts down). */
    function orbitPixels(dx, dy) {
        if (ortho)
            return
        yawDeg   -= dx * 0.35
        pitchDeg  = clampNum(pitchDeg - dy * 0.35, -89.9, 89.9)
        apply()
    }

    /* Promote an ortho preset into the perspective free view
       in place — a left-drag orbit gesture starts here. The
       ortho span converts to an equivalent dolly distance so
       the framed extent doesn't jump. No poseFinished: the
       enclosing gesture's endPose persists once (and Escape's
       snapshot restore undoes the switch unsaved). */
    function enterFreeView() {
        if (!ortho)
            return
        var fov = (perspCam ? perspCam.fieldOfView : 60.0)
                * Math.PI / 180.0
        distance = clampNum(span / (2.0 * Math.tan(fov / 2.0)),
                            distMin, distMax)
        viewName = "free"
        apply()
    }

    /* Ray through a pixel: near+mid mapTo3DScene points define it. */
    function screenRay(px, py) {
        if (!view3d)
            return null
        var a = view3d.mapTo3DScene(Qt.vector3d(px, py, 0.0))
        var b = view3d.mapTo3DScene(Qt.vector3d(px, py, 0.5))
        var d = b.minus(a)
        if (d.length() < 1e-9)
            return null
        return { o: a, d: d.normalized() }
    }

    /* World point where the pixel ray meets the plane through
       `target` perpendicular to the view direction — the anchor
       pointer-centered zoom holds fixed. */
    function pointUnderPointer(px, py) {
        var r = screenRay(px, py)
        if (!r || !origin)
            return null
        var n = origin.forward
        var denom = r.d.dotProduct(n)
        if (Math.abs(denom) < 1e-9)
            return null
        var t = target.minus(r.o).dotProduct(n) / denom
        if (t < 0)
            t = 0
        return r.o.plus(r.d.times(t))
    }

    /* Ray/pointer intersection with an axis-aligned plane.
       axisIndex 0=X 1=Y 2=Z; -1 = camera-facing plane through
       `value` (a vector3d). Returns null when parallel/behind. */
    function planeHitPoint(px, py, axisIndex, value) {
        var r = screenRay(px, py)
        if (!r)
            return null
        var t
        if (axisIndex < 0) {
            var n = origin ? origin.forward : Qt.vector3d(0, 0, -1)
            var denom = r.d.dotProduct(n)
            if (Math.abs(denom) < 1e-9)
                return null
            t = value.minus(r.o).dotProduct(n) / denom
        } else {
            var dn = axisIndex === 0 ? r.d.x : axisIndex === 1 ? r.d.y : r.d.z
            var on = axisIndex === 0 ? r.o.x : axisIndex === 1 ? r.o.y : r.o.z
            if (Math.abs(dn) < 1e-9)
                return null
            t = (value - on) / dn
        }
        if (t < 0)
            return null
        return r.o.plus(r.d.times(t))
    }

    /* Wheel zoom toward the pointer: the world point under the
       cursor stays under the cursor. Bounded both directions. */
    function zoomAt(px, py, angleDelta) {
        if (angleDelta === 0)
            return
        var anchor = pointUnderPointer(px, py)
        var f = Math.pow(1.0015, angleDelta)
        if (ortho) {
            span = clampNum(span / f, spanMin, spanMax)
        } else {
            distance = clampNum(distance / f, distMin, distMax)
        }
        apply()
        var after = pointUnderPointer(px, py)
        if (anchor && after) {
            target = target.plus(anchor.minus(after))
            apply()
        }
        if (!deferPose)
            poseFinished()
        else
            deferredSave = true
    }

    function projectPoint(worldPos) {
        return view3d ? view3d.mapFrom3DScene(worldPos) : null
    }

    /*-----------------------------------------------------*\
    || Persistence + Escape snapshots.                      ||
    \*-----------------------------------------------------*/
    function poseSnapshot() {
        return { t: target, yaw: yawDeg, pitch: pitchDeg,
                 dist: distance, sp: span, vn: viewName }
    }

    function restorePose(s) {
        if (!s)
            return
        target = s.t
        yawDeg = s.yaw
        pitchDeg = s.pitch
        distance = s.dist
        span = s.sp
        viewName = s.vn
        apply()
    }

    /* Gesture end — persist the final pose (editor prefs). */
    function endPose() { deferredSave = false; poseFinished() }

    /* Release of a NON-camera gesture: persist only when a wheel
       zoom was deferred during the press window (delay-to-release,
       not drop). cancel() drops the flag instead wherever a pose
       snapshot is restored. */
    function flushDeferredPose() {
        if (deferredSave) {
            deferredSave = false
            poseFinished()
        }
    }

    function stateMap() {
        return {
            "view":       viewName,
            "projection": ortho ? "orthographic" : "perspective",
            "tx": target.x, "ty": target.y, "tz": target.z,
            "yaw": yawDeg, "pitch": pitchDeg,
            "distance": distance, "span": span
        }
    }

    /* Restore the persisted pose (startup / workspace load). */
    function applyPersisted() {
        if (!prefsSource)
            return
        var s = prefsSource.cameraState
        if (!s || s.view === undefined)
            return
        viewName = s.view
        target   = Qt.vector3d(s.tx, s.ty, s.tz)
        yawDeg   = s.yaw
        pitchDeg = s.pitch
        distance = s.distance
        span     = clampNum(s.span, spanMin, spanMax)
        apply()
    }
}
