/*---------------------------------------------------------*\
||| Ram.qml                                               ||
|||                                                       ||
|||   ram_body family body (spec §3): dark PCB with a    ||
|||   gold contact edge, twin heat-spreader plates, and  ||
|||   a continuous translucent diffuser bar along the    ||
|||   top edge with one emissive lens segment under each ||
|||   real emitter (ctx.emitterPos-driven). Dots stay    ||
|||   above the diffuser.                                ||
|||                                                       ||
|||   `ctx` = StudioViewport object delegate.               ||
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

    function segColor(i) {
        var cs = ctx ? ctx.emitterColors : null
        return (cs && i < cs.length) ? cs[i] : "#000000"
    }

    /* PCB — slightly thinner than the resolved body so the
       spreader plates read as clamped on. */
    Model {
        objectName: body.pickName
        pickable: body.pickOn
        source: "#Cube"
        scale: Qt.vector3d(body.bx * 0.97 / 100, body.by * 0.82 / 100,
                           body.bz * 0.6 / 100)
        materials: PrincipledMaterial {
            baseColor: "#14141c"; roughness: 0.7
        }
    }

    /* Gold contact edge along the bottom. */
    Model {
        objectName: body.pickName
        pickable: body.pickOn
        source: "#Cube"
        position: Qt.vector3d(0, -body.by * 0.44, 0)
        scale: Qt.vector3d(body.bx * 0.9 / 100, 0.0025 / 100,
                           body.bz * 0.5 / 100)
        materials: PrincipledMaterial {
            baseColor: "#b8963c"; metalness: 0.8; roughness: 0.35
        }
    }

    /* Twin heat-spreader plates. */
    Repeater3D {
        model: 2
        delegate: Model {
            required property var modelData
            property int index: modelData
            objectName: body.pickName
            pickable: body.pickOn
            source: "#Cube"
            property real s: index === 0 ? 1 : -1
            position: Qt.vector3d(0, -body.by * 0.06,
                                  s * body.bz * 0.30)
            scale: Qt.vector3d(body.bx / 100, body.by * 0.68 / 100,
                               0.0018 / 100)
            materials: PrincipledMaterial {
                baseColor: "#26262e"; metalness: 0.55; roughness: 0.45
            }
        }
    }

    /* Continuous diffuser bar along the top edge. */
    Model {
        objectName: body.pickName
        pickable: body.pickOn
        source: "#Cube"
        position: Qt.vector3d(0, body.by * 0.40, 0)
        scale: Qt.vector3d(body.bx * 0.88 / 100, 0.005 / 100,
                           body.bz * 0.8 / 100)
        materials: PrincipledMaterial {
            baseColor: "#d8d8e4"
            opacity: 0.4
            roughness: 0.25
        }
    }

    /* Underlying LED segments — one emissive lens per emitter,
       just above the diffuser, just below the dot. */
    Repeater3D {
        model: body.ctx ? body.ctx.emitterPos : []
        delegate: Model {
            required property var modelData
            source: "#Cube"
            position: Qt.vector3d(modelData.x, modelData.y - 0.0035,
                                  modelData.z)
            scale: Qt.vector3d(0.010 / 100, 0.003 / 100,
                               body.bz * 0.7 / 100)
            materials: PrincipledMaterial {
                baseColor: "#e8e8f0"
                opacity: 0.5
                roughness: 0.3
                property color ec: body.segColor(modelData.i)
                emissiveFactor: Qt.rgba(ec.r * 0.5, ec.g * 0.5,
                                        ec.b * 0.5, 1.0)
            }
        }
    }
}
