/*---------------------------------------------------------*\
||| Fan.qml                                               ||
|||                                                       ||
|||   fan_body + pump_body family body (spec §3):        ||
|||                                                       ||
|||   fan  — square frame (4 corner posts + 4 edge       ||
|||          bars), hub, seven static blades, and a      ||
|||          diffuser SEGMENT per real emitter: the      ||
|||          visible ring segments are a Repeater3D over ||
|||          ctx.emitterPos, so they always correspond   ||
|||          to the actual addressable LEDs.             ||
|||   pump — cylindrical cap body with the same          ||
|||          per-emitter diffuser treatment on the top   ||
|||          face.                                       ||
|||                                                       ||
|||   The frame's light face stays OPEN (posts + edge    ||
|||   bars only) and segments top out just under the     ||
|||   emitter dots — face/side emitters are never sealed ||
|||   inside opaque geometry (the fan-slw WIP moved its  ||
|||   LEDs onto the face plane for exactly this reason). ||
|||                                                       ||
|||   `ctx` = StudioViewport object delegate (contract in   ||
|||   Keyboard.qml). Rotations are quaternions — no      ||
|||   eulerRotation on scene objects.                    ||
\*---------------------------------------------------------*/
import QtQuick
import QtQuick3D

Node {
    id: body

    property var ctx: null
    readonly property real bx: ctx ? ctx.oBx : 0
    readonly property real by: ctx ? ctx.oBy : 0
    readonly property real bz: ctx ? ctx.oBz : 0
    readonly property string pickName: ctx ? ("obj|" + ctx.oId) : ""
    readonly property bool pickOn: ctx !== null && ctx.ghostBody !== true
    readonly property bool isPump: ctx ? ctx.oGeom === "pump_body" : false

    function segColor(i) {
        var cs = ctx ? ctx.emitterColors : null
        return (cs && i < cs.length) ? cs[i] : "#000000"
    }
    /* Yaw quaternion about +Y for a tangent-aligned segment at
       angle a (rad). */
    function yawQuat(a) {
        return Qt.quaternion(Math.cos(a / 2), 0, Math.sin(a / 2), 0)
    }

    /* ---- square frame (fan only) ---- */

    Repeater3D {
        model: body.isPump ? 0 : 4
        delegate: Model {
            required property var modelData
            property int index: modelData
            objectName: body.pickName
            pickable: body.pickOn
            source: "#Cube"
            property real c: (index % 2 === 0 ? 1 : -1)
            property real d: (index < 2 ? 1 : -1)
            position: Qt.vector3d(c * (body.bx / 2 - 0.006), 0,
                                  d * (body.bz / 2 - 0.006))
            scale: Qt.vector3d(0.012 / 100, body.by / 100, 0.012 / 100)
            materials: PrincipledMaterial {
                baseColor: "#202028"; roughness: 0.6; metalness: 0.15
            }
        }
    }
    Repeater3D {
        model: body.isPump ? 0 : 4
        delegate: Model {
            required property var modelData
            property int index: modelData
            objectName: body.pickName
            pickable: body.pickOn
            source: "#Cube"
            property real c: (index % 2 === 0 ? 1 : -1)
            property bool alongX: index < 2
            position: alongX ? Qt.vector3d(0, 0, c * (body.bz / 2 - 0.003))
                             : Qt.vector3d(c * (body.bx / 2 - 0.003), 0, 0)
            scale: alongX ? Qt.vector3d(body.bx / 100, body.by / 100,
                                        0.006 / 100)
                          : Qt.vector3d(0.006 / 100, body.by / 100,
                                        (body.bz - 0.024) / 100)
            materials: PrincipledMaterial {
                baseColor: "#202028"; roughness: 0.6; metalness: 0.15
            }
        }
    }

    /* ---- hub + blades (fan only) ---- */

    Model {
        visible: !body.isPump
        objectName: body.pickName
        pickable: body.pickOn
        source: "#Cylinder"
        scale: Qt.vector3d(0.040 / 100, body.by * 0.85 / 100, 0.040 / 100)
        materials: PrincipledMaterial {
            baseColor: "#1a1a22"; roughness: 0.5; metalness: 0.2
        }
    }
    /* Invisible pick plate covering the open light face — the
       posts/bars/hub alone left most of the face unpickable, so
       clicks through the blades fell to empty space and cleared
       the selection. Same pattern as the emit| proxies. */
    Model {
        visible: !body.isPump
        objectName: body.pickName
        pickable: body.pickOn
        source: "#Cube"
        scale: Qt.vector3d(body.bx / 100, 0.001 / 100, body.bz / 100)
        castsShadows: false
        materials: PrincipledMaterial {
            lighting: PrincipledMaterial.NoLighting
            baseColor: "#00000000"
            opacity: 0.0
            depthDrawMode: PrincipledMaterial.NeverDepthDraw
        }
    }

    Repeater3D {
        model: body.isPump ? 0 : 7
        delegate: Model {
            required property var modelData
            property int index: modelData
            objectName: body.pickName
            pickable: body.pickOn
            source: "#Cube"
            /* Blade yaw = index * 360/7 + a fixed sweep offset. */
            property real a: index * (2 * Math.PI / 7) + 0.35
            rotation: body.yawQuat(a)
            position: Qt.vector3d(Math.cos(a) * 0.036, 0,
                                  Math.sin(a) * 0.036)
            scale: Qt.vector3d(0.034 / 100, 0.0025 / 100, 0.019 / 100)
            materials: PrincipledMaterial {
                baseColor: "#2c2c38"
                roughness: 0.55
                metalness: 0.1
                opacity: 0.85
            }
        }
    }

    /* ---- pump cap (pump only) ---- */

    Model {
        visible: body.isPump
        objectName: body.pickName
        pickable: body.pickOn
        source: "#Cylinder"
        scale: Qt.vector3d(body.bx / 100, body.by / 100, body.bz / 100)
        materials: PrincipledMaterial {
            baseColor: "#22242c"; roughness: 0.45; metalness: 0.3
        }
    }
    Model {
        visible: body.isPump
        objectName: body.pickName
        pickable: body.pickOn
        source: "#Cylinder"
        position: Qt.vector3d(0, body.by * 0.30, 0)
        scale: Qt.vector3d(body.bx * 0.82 / 100, body.by * 0.35 / 100,
                           body.bz * 0.82 / 100)
        materials: PrincipledMaterial {
            baseColor: "#2a2c36"; roughness: 0.4; metalness: 0.35
        }
    }

    /* ---- segmented diffuser — one translucent segment per real
       emitter, tangent-aligned, top face just below the dot so the
       shared emitter dots stay readable on any face plane. ---- */
    Repeater3D {
        model: body.ctx ? body.ctx.emitterPos : []
        delegate: Model {
            required property var modelData
            source: "#Cube"
            property real a: Math.atan2(modelData.z, modelData.x)
            rotation: body.yawQuat(a + Math.PI / 2)
            position: Qt.vector3d(modelData.x, modelData.y - 0.0032,
                                  modelData.z)
            scale: Qt.vector3d(0.014 / 100, 0.0045 / 100, 0.010 / 100)
            materials: PrincipledMaterial {
                baseColor: "#e8e8f0"
                opacity: 0.45
                roughness: 0.3
                property color ec: body.segColor(modelData.i)
                emissiveFactor: Qt.rgba(ec.r * 0.5, ec.g * 0.5,
                                        ec.b * 0.5, 1.0)
            }
        }
    }
}
