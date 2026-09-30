// SPDX-License-Identifier: GPL-2.0-or-later
//
// TransformInspector — numeric transform fields for the
// selected instance (studio-import Task 5), adapted from the
// source-of-record ui/editor/TransformInspector.qml:
//
//   - tool buttons (Move/Rotate/Paint), numeric transform
//     fields (mm + deg, drag-to-scrub, per-field reset, 90°
//     yaw steppers), active-plane selector, snap/lock/hide
//     toggles and the align/distribute row
//   - `ctl` is the ObjectEditorController (tool/editPlane/
//     snapEnabled); `cam` is the optional viewport camera —
//     hidden on the 2D canvas (view presets land with the 3D
//     viewport task)
//   - every numeric commit calls objectCtl.setInstancePosition/
//     setInstanceRotation — one undo record per commit through
//     the shared session
import QtQuick
import QtQuick.Controls.Basic
import OpenRGB2.UI.theme
import OpenRGB2.UI.components

Rectangle {
    id: inspector
    objectName: "inspector"

    property var ctl: null
    property var cam: null
    /* Test seam — production supplies `objectCtl`; tests inject a
       stub. */
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
    /* True while any numeric field holds focus — shortcuts that
       aren't text (W/E/F/Home, undo) must not fire then. */
    readonly property bool textFocus: activeField !== null
    property var  activeField: null

    property string targetId: ""
    property bool   hasTarget: false
    property real   posX: 0   /* mm — fields display millimeters */
    property real   posY: 0
    property real   posZ: 0
    property real   rotX: 0   /* deg */
    property real   rotY: 0
    property real   rotZ: 0
    property bool   instLocked: false
    property bool   instVisible: true

    implicitWidth: 296
    implicitHeight: col.implicitHeight + 16
    color: Qt.rgba(Theme.surface.r, Theme.surface.g, Theme.surface.b, 0.88)
    radius: Theme.radiusS
    border.color: Theme.borderStrong

    function reload() {
        if (!hasBr()) {
            targetId = ""; hasTarget = false
            return
        }
        var ids = br().selectedInstances
        targetId = ids.length ? ids[ids.length - 1] : ""
        var st = targetId ? br().instanceState(targetId) : {}
        hasTarget = targetId !== "" && st.id !== undefined
        if (!hasTarget)
            return
        posX = st.x * 1000; posY = st.y * 1000; posZ = st.z * 1000
        rotX = st.rx; rotY = st.ry; rotZ = st.rz
        instLocked  = !!st.locked
        instVisible = st.visible !== false
    }

    function commitPos(axis, mm) {
        var st = br().instanceState(targetId)
        if (st.id === undefined)
            return
        var x = st.x, y = st.y, z = st.z
        if (axis === 0)      x = mm / 1000.0
        else if (axis === 1) y = mm / 1000.0
        else                 z = mm / 1000.0
        br().setInstancePosition(targetId, x, y, z)
        reload()
    }

    function commitRot(axis, deg) {
        var st = br().instanceState(targetId)
        if (st.id === undefined)
            return
        var rx = st.rx, ry = st.ry, rz = st.rz
        if (axis === 0)      rx = deg
        else if (axis === 1) ry = deg
        else                 rz = deg
        br().setInstanceRotation(targetId, rx, ry, rz)
        reload()
    }

    function stepRot(axis, ddeg) {
        var st = br().instanceState(targetId)
        if (st.id === undefined)
            return
        var rx = st.rx, ry = st.ry, rz = st.rz
        if (axis === 0)      rx += ddeg
        else if (axis === 1) ry += ddeg
        else                 rz += ddeg
        br().setInstanceRotation(targetId, rx, ry, rz)
        reload()
    }
    function stepYaw(ddeg) { stepRot(1, ddeg) }

    Connections {
        target: inspector.brQObj()
        function onSelectionChanged() { inspector.reload() }
        function onUndoChanged()      { inspector.reload() }
        function onSceneChanged()     { inspector.reload() }
    }
    /* Granular transform updates emit only dataChanged — the timer
       keeps the fields honest after drags/commits. */
    Timer {
        interval: 400; running: inspector.visible; repeat: true
        onTriggered: inspector.reload()
    }
    Component.onCompleted: reload()

    /* Small label button used everywhere. */
    component TBtn: Rectangle {
        property string text: ""
        property bool   active: false
        property int    w: 44
        signal clicked()
        width: w; height: 22; radius: 3
        color: active ? Theme.accentMuted
                      : (hov.containsMouse ? Theme.surfaceAlt : Theme.bg)
        border.color: hov.activeFocus ? Theme.accent
                    : (active ? Theme.accent : Theme.borderStrong)
        Text {
            anchors.centerIn: parent
            text: parent.text
            color: parent.enabled ? Theme.textPrimary : Theme.textDisabled
            font.pixelSize: 11
        }
        MouseArea {
            id: hov; anchors.fill: parent; hoverEnabled: true
            enabled: parent.enabled
            activeFocusOnTab: true
            onClicked: parent.clicked()
            Keys.onSpacePressed:  parent.clicked()
            Keys.onReturnPressed: parent.clicked()
        }
    }

    /* Numeric field: text commit + horizontal drag-to-scrub on the
       left edge + reset-to-zero. Each commit = one undo record. */
    component NumField: Item {
        id: nf
        property string label: ""
        property real   value: 0
        property string suffix: ""
        property real   scrubStep: 1.0
        property int    decimals: 1
        signal committed(real v)

        width: parent ? parent.width : 0
        height: 24

        readonly property bool editing: input.activeFocus || scrub.pressed

        function fmt(v) { return v.toFixed(decimals) }
        function commitText() {
            var v = parseFloat(input.text)
            if (isFinite(v))
                committed(v)
        }
        onValueChanged: {
            if (!editing)
                input.text = fmt(value)
        }

        Text {
            x: 0; anchors.verticalCenter: parent.verticalCenter
            text: nf.label; color: Theme.textSecondary; font.pixelSize: 11
            width: 18
        }
        Rectangle {
            x: 22; width: nf.width - 78; height: 22
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.bg; radius: 3; border.color: Theme.borderStrong
            TextInput {
                id: input
                anchors.fill: parent; anchors.margins: 3
                color: Theme.textPrimary; font.pixelSize: 11
                text: nf.fmt(nf.value)
                selectByMouse: true
                validator: DoubleValidator { locale: "C" }
                onAccepted: { nf.commitText(); focus = false }
                onActiveFocusChanged: {
                    if (activeFocus)
                        inspector.activeField = nf
                    else if (inspector.activeField === nf)
                        inspector.activeField = null
                }
            }
            /* Drag-to-scrub strip along the field's left edge. */
            MouseArea {
                id: scrub
                anchors.left: parent.left
                width: 10; height: parent.height
                cursorShape: Qt.SizeHorCursor
                property real anchor: 0
                property real startVal: 0
                onPressed: function(m) {
                    anchor = mapToItem(null, m.x, m.y).x
                    startVal = parseFloat(input.text)
                    if (!isFinite(startVal))
                        startVal = 0
                    /* Scrubbing is editing — shortcuts (undo)
                       must not fire mid-scrub. */
                    inspector.activeField = nf
                }
                onPositionChanged: function(m) {
                    if (!pressed)
                        return
                    var cur = mapToItem(null, m.x, m.y).x
                    input.text = nf.fmt(startVal + (cur - anchor) * nf.scrubStep)
                }
                onReleased: function(m) {
                    if (inspector.activeField === nf && !input.activeFocus)
                        inspector.activeField = null
                    nf.commitText()
                }
                onCanceled: {
                    if (inspector.activeField === nf)
                        inspector.activeField = null
                }
            }
        }
        Text {
            x: nf.width - 52; anchors.verticalCenter: parent.verticalCenter
            text: nf.suffix; color: Theme.textDisabled; font.pixelSize: 10
            width: 26
        }
        TBtn {
            x: nf.width - 24; w: 24; text: "0"
            anchors.verticalCenter: parent.verticalCenter
            enabled: nf.enabled
            onClicked: nf.committed(0)
        }
    }

    Column {
        id: col
        anchors.left: parent.left; anchors.right: parent.right
        anchors.top: parent.top; anchors.margins: 8
        spacing: 6

        /* Tool + undo/redo row */
        Row {
            spacing: 4
            TBtn { text: qsTr("Move");   w: 52; active: ctl && ctl.tool === 0
                   onClicked: if (ctl) ctl.tool = 0 }
            TBtn { text: qsTr("Rotate"); w: 52; active: ctl && ctl.tool === 1
                   onClicked: if (ctl) ctl.tool = 1 }
            TBtn { text: qsTr("Paint");  w: 52; active: ctl && ctl.tool === 2
                   onClicked: if (ctl) ctl.tool = 2 }
            Item { width: 8; height: 1 }
            TBtn { text: qsTr("Undo"); w: 44
                   opacity: (inspector.hasBr() && inspector.br().canUndo) ? 1 : 0.4
                   onClicked: if (inspector.hasBr()
                                  && !inspector.br().gestureActive())
                                  inspector.br().undo() }
            TBtn { text: qsTr("Redo"); w: 44
                   opacity: (inspector.hasBr() && inspector.br().canRedo) ? 1 : 0.4
                   onClicked: if (inspector.hasBr()
                                  && !inspector.br().gestureActive())
                                  inspector.br().redo() }
        }
        /* Paint tool — the color the canvas click applies
           (LayoutPage calls paintObject/paintEmitter with
           ctl.paintColor). Current swatch + the module's quick
           palette, same convention as the Lighting page; the
           Flow wraps the picks under the swatch line. */
        Column {
            spacing: 4
            width: parent.width
            visible: inspector.ctl !== null
                     && inspector.ctl.tool === 2
            height: visible ? implicitHeight : 0
            Row {
                spacing: 4
                Text {
                    text: qsTr("Paint")
                    color: Theme.textSecondary
                    font.pixelSize: 11
                    width: 34
                    anchors.verticalCenter: parent.verticalCenter
                }
                ColorWell {
                    objectName: "paintColorWell"
                    swatch: inspector.ctl ? inspector.ctl.paintColor
                                          : "white"
                    implicitHeight: 22
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    text: qsTr("click an object")
                    color: Theme.textDisabled
                    font.pixelSize: 10
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
            Flow {
                width: parent.width
                spacing: 4
                Repeater {
                    model: Theme.paletteSwatches
                    PaletteSwatch {
                        objectName: "paintSwatch" + index
                        swatch: modelData
                        onPicked: function(hex) {
                            if (inspector.ctl)
                                inspector.ctl.paintColor = hex
                        }
                    }
                }
            }
        }
        /* View presets need a 3D camera — hidden on the 2D canvas. */
        Row {
            spacing: 4
            visible: inspector.cam !== null
            height: visible ? implicitHeight : 0
            Text { text: qsTr("View"); color: Theme.textSecondary; font.pixelSize: 11
                   anchors.verticalCenter: parent.verticalCenter; width: 30 }
            Repeater {
                model: ["desk", "top", "front", "case", "free"]
                TBtn {
                    text: modelData.charAt(0).toUpperCase() + modelData.slice(1)
                    w: 44
                    active: cam && cam.viewName === modelData
                    onClicked: if (cam) cam.applyView(modelData)
                }
            }
        }

        Rectangle { width: parent.width; height: 1; color: Theme.border }

        Text {
            text: inspector.hasTarget
                  ? inspector.targetId + (inspector.instLocked ? qsTr("  (locked)") : "")
                  : qsTr("no selection")
            color: inspector.hasTarget ? Theme.textPrimary : Theme.textDisabled
            font.pixelSize: 11
        }

        /* Position fields — millimeters (saved positions stay m). */
        NumField { label: "X"; suffix: "mm"; value: inspector.posX; decimals: 1; scrubStep: 1
                   enabled: inspector.hasTarget; opacity: enabled ? 1 : 0.4
                   onCommitted: function(v) { inspector.commitPos(0, v) } }
        NumField { label: "Y"; suffix: "mm"; value: inspector.posY; decimals: 1; scrubStep: 1
                   enabled: inspector.hasTarget; opacity: enabled ? 1 : 0.4
                   onCommitted: function(v) { inspector.commitPos(1, v) } }
        NumField { label: "Z"; suffix: "mm"; value: inspector.posZ; decimals: 1; scrubStep: 1
                   enabled: inspector.hasTarget; opacity: enabled ? 1 : 0.4
                   onCommitted: function(v) { inspector.commitPos(2, v) } }

        /* Rotation fields — degrees; every row carries ±90° steppers. */
        component RotRow: Item {
            property string label: ""
            property real   value: 0
            property int    axis: 0
            signal committed(real v)
            width: parent ? parent.width : 0; height: 24
            enabled: inspector.hasTarget; opacity: enabled ? 1 : 0.4
            NumField {
                label: parent.label; suffix: "°"; value: parent.value
                decimals: 1; scrubStep: 1
                width: parent.width - 56
                onCommitted: function(v) { parent.committed(v) }
            }
            TBtn { x: parent.width - 52; w: 24; text: "-90"
                   onClicked: inspector.stepRot(parent.axis, -90) }
            TBtn { x: parent.width - 26; w: 24; text: "+90"
                   onClicked: inspector.stepRot(parent.axis, 90) }
        }
        RotRow { label: "rX"; value: inspector.rotX; axis: 0
                 onCommitted: function(v) { inspector.commitRot(0, v) } }
        RotRow { label: "rY"; value: inspector.rotY; axis: 1
                 onCommitted: function(v) { inspector.commitRot(1, v) } }
        RotRow { label: "rZ"; value: inspector.rotZ; axis: 2
                 onCommitted: function(v) { inspector.commitRot(2, v) } }

        Rectangle { width: parent.width; height: 1; color: Theme.border }

        /* Plane + snap */
        Row {
            spacing: 4
            Text { text: qsTr("Plane"); color: Theme.textSecondary; font.pixelSize: 11; width: 34
                   anchors.verticalCenter: parent.verticalCenter }
            Repeater {
                model: [
                    { t: qsTr("Desk XZ"),  p: 1 },
                    { t: qsTr("Front XY"), p: 2 },
                    { t: qsTr("Side YZ"),  p: 3 },
                    { t: qsTr("Free"),     p: 0 }
                ]
                TBtn {
                    text: modelData.t; w: 56
                    active: ctl && ctl.editPlane === modelData.p
                    onClicked: if (ctl) ctl.editPlane = modelData.p
                }
            }
        }
        Row {
            spacing: 4
            TBtn { text: qsTr("Snap"); w: 44; active: ctl && ctl.snapEnabled
                   onClicked: if (ctl) ctl.snapEnabled = !ctl.snapEnabled }
            Text { text: qsTr("10mm / 15° (Shift)"); color: Theme.textDisabled; font.pixelSize: 10
                   anchors.verticalCenter: parent.verticalCenter }
        }

        /* Lock / hide */
        Row {
            spacing: 4
            enabled: inspector.hasTarget; opacity: enabled ? 1 : 0.4
            TBtn { text: qsTr("Locked"); w: 52; active: inspector.instLocked
                   onClicked: if (inspector.hasTarget && inspector.hasBr())
                       inspector.br().setInstanceLocked(inspector.targetId,
                                                        !inspector.instLocked) }
            TBtn { text: qsTr("Hidden"); w: 52; active: !inspector.instVisible
                   onClicked: if (inspector.hasTarget && inspector.hasBr())
                       inspector.br().setInstanceVisible(inspector.targetId,
                                                         !inspector.instVisible) }
            Text { text: qsTr("lock = no edit · hide = visual only")
                   color: Theme.textDisabled; font.pixelSize: 10
                   anchors.verticalCenter: parent.verticalCenter }
        }

        /* Align / distribute — one undo record each. */
        Row {
            spacing: 4
            enabled: inspector.hasTarget; opacity: enabled ? 1 : 0.4
            Text { text: qsTr("Align"); color: Theme.textSecondary; font.pixelSize: 11; width: 34
                   anchors.verticalCenter: parent.verticalCenter }
            TBtn { text: "X-"; w: 28; onClicked: if (inspector.hasBr()) inspector.br().alignSelected(0, 0) }
            TBtn { text: "X";  w: 28; onClicked: if (inspector.hasBr()) inspector.br().alignSelected(0, 1) }
            TBtn { text: "X+"; w: 28; onClicked: if (inspector.hasBr()) inspector.br().alignSelected(0, 2) }
            TBtn { text: "Z-"; w: 28; onClicked: if (inspector.hasBr()) inspector.br().alignSelected(2, 0) }
            TBtn { text: "Z";  w: 28; onClicked: if (inspector.hasBr()) inspector.br().alignSelected(2, 1) }
            TBtn { text: "Z+"; w: 28; onClicked: if (inspector.hasBr()) inspector.br().alignSelected(2, 2) }
            TBtn { text: "Y";  w: 28; onClicked: if (inspector.hasBr()) inspector.br().alignSelected(1, 1) }
        }
        Row {
            spacing: 4
            enabled: inspector.hasTarget; opacity: enabled ? 1 : 0.4
            Text { text: qsTr("Dist"); color: Theme.textSecondary; font.pixelSize: 11; width: 34
                   anchors.verticalCenter: parent.verticalCenter }
            TBtn { text: "X"; w: 28; onClicked: if (inspector.hasBr()) inspector.br().distributeSelected(0) }
            TBtn { text: "Z"; w: 28; onClicked: if (inspector.hasBr()) inspector.br().distributeSelected(2) }
            Text { text: qsTr("needs 3+"); color: Theme.textDisabled; font.pixelSize: 10
                   anchors.verticalCenter: parent.verticalCenter }
        }
    }
}
