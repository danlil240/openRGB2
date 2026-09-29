/*---------------------------------------------------------*\
|| TransformGizmo.qml                                      ||
||                                                         ||
||   Selection pivot + transform handles overlay:          ||
||   - pivot marker (shared multi-selection pivot)         ||
||   - rotate ring in Rotate mode — dragging the ring     ||
||     rotates around the active plane normal              ||
||   - move axis cross is drawn in 3D by the scene on     ||
||     pivotWorld (see StudioViewport pivotMarker)         ||
||   The ring hit test (ringHit) and sweep angle           ||
||   (angleAt) are what SelectionController gestures use.  ||
||                                                         ||
||   Adapted for OpenRGB2 (studio-import Task 8) from     ||
||   plugins/DesktopLightingStudio/ui/editor/             ||
||   TransformGizmo.qml: the implicit `bridge` context    ||
||   property is replaced by the explicit `editor`        ||
||   property (ObjectEditorController —                 ||
||   selectedInstances + selectionChanged/sceneChanged).  ||
||                                                         ||
||   SPDX-License-Identifier: GPL-2.0-or-later            ||
\*---------------------------------------------------------*/
import QtQuick

Item {
    id: gizmo
    objectName: "gizmo"

    property var view3d: null
    property var cam: null
    property var ctl: null
    /* The selection surface — ObjectEditorController in
       production; a plain-JS stub in tests. */
    property var editor: null
    /* eachNode(cb) — iterate the live object-node map. */
    property var eachNode: null

    property real ringRadius: 56
    property var  pivotWorld: null
    property bool hasPivot: false

    /* Screen-space pivot — re-evaluates on camera moves
       (cam.poseStamp), pivot recomputes and resizes. */
    property point pivotScreen: {
        var stamp = cam ? cam.poseStamp : 0
        if (!hasPivot || !pivotWorld || !view3d)
            return Qt.point(-1e6, -1e6)
        var p = view3d.mapFrom3DScene(pivotWorld)
        return Qt.point(p.x, p.y)
    }

    readonly property bool rotateMode: ctl && ctl.tool === 1
    readonly property bool moveMode:   ctl && ctl.tool === 0

    function updatePivot() {
        var ids = editor ? editor.selectedInstances : []
        if (!eachNode || !ids || ids.length === 0) {
            hasPivot = false
            pivotWorld = null
            return
        }
        var want = {}
        for (var k = 0; k < ids.length; k++)
            want[ids[k]] = true
        var sx = 0, sy = 0, sz = 0, n = 0
        eachNode(function(item) {
            if (!item || !want[item.oInst])
                return
            var p = item.scenePosition
            sx += p.x; sy += p.y; sz += p.z; n++
        })
        if (n === 0) {
            hasPivot = false
            pivotWorld = null
            return
        }
        pivotWorld = Qt.vector3d(sx / n, sy / n, sz / n)
        hasPivot = true
    }

    /* Ring band hit test — the rotate handle proper. */
    function ringHit(x, y) {
        if (!(rotateMode && hasPivot))
            return false
        var dx = x - pivotScreen.x
        var dy = y - pivotScreen.y
        var d = Math.sqrt(dx * dx + dy * dy)
        return d >= ringRadius - 12 && d <= ringRadius + 12
    }

    /* Screen angle around the pivot — the rotate gesture's input. */
    function angleAt(x, y) {
        return Math.atan2(y - pivotScreen.y, x - pivotScreen.x)
               * 180.0 / Math.PI
    }

    Connections {
        target: gizmo.editor
        function onSelectionChanged() { gizmo.updatePivot() }
        function onSceneChanged()     { gizmo.updatePivot() }
    }
    /* Recompute when a gesture ends — transform previews are
       granular dataChanged (no sceneChanged), and the pivot must
       follow the committed position. */
    Connections {
        target: ctl
        function onGestureChanged() {
            if (ctl && ctl.gesture === 0)
                gizmo.updatePivot()
        }
    }

    /* Pivot cross — both tools. */
    Rectangle {
        visible: gizmo.hasPivot && (!gizmo.ctl || gizmo.ctl.gesture !== 2)
        width: 10; height: 2; color: "#ffd040"
        x: gizmo.pivotScreen.x - 5; y: gizmo.pivotScreen.y - 1
    }
    Rectangle {
        visible: gizmo.hasPivot && (!gizmo.ctl || gizmo.ctl.gesture !== 2)
        width: 2; height: 10; color: "#ffd040"
        x: gizmo.pivotScreen.x - 1; y: gizmo.pivotScreen.y - 5
    }

    /* Rotate ring handle. */
    Rectangle {
        visible: gizmo.rotateMode && gizmo.hasPivot
        width: gizmo.ringRadius * 2; height: gizmo.ringRadius * 2
        radius: gizmo.ringRadius
        x: gizmo.pivotScreen.x - gizmo.ringRadius
        y: gizmo.pivotScreen.y - gizmo.ringRadius
        color: "transparent"
        border.color: "#ffd040"
        border.width: 2
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            y: -4
            width: 8; height: 8; radius: 4
            color: "#ffd040"
        }
    }
}
