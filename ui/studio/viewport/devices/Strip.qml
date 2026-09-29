/*---------------------------------------------------------*\
||| Strip.qml                                             ||
|||                                                       ||
|||   strip family body (spec §3) — used for gpu_logo    ||
|||   and any narrow LED-bar device whose emitters ARE   ||
|||   the geometry: a translucent diffuser bar sized to  ||
|||   the emitter span, with one emissive lens segment   ||
|||   per real emitter. bx/by/bz can be zero for these   ||
|||   tags, so the bar derives its length from           ||
|||   ctx.emitterPos and falls back to a 60 mm default.  ||
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

    /* Emitter span along X/Z — the bar hugs the LED line. */
    readonly property var span: {
        var es = ctx ? ctx.emitterPos : []
        var lo = Qt.vector3d(0, 0, 0), hi = Qt.vector3d(0, 0, 0)
        for (var i = 0; i < es.length; i++) {
            var e = es[i]
            if (i === 0) { lo = Qt.vector3d(e.x, e.y, e.z)
                           hi = lo }
            lo = Qt.vector3d(Math.min(lo.x, e.x), Math.min(lo.y, e.y),
                             Math.min(lo.z, e.z))
            hi = Qt.vector3d(Math.max(hi.x, e.x), Math.max(hi.y, e.y),
                             Math.max(hi.z, e.z))
        }
        return { "lo": lo, "hi": hi }
    }
    readonly property real lenX: Math.max(span.hi.x - span.lo.x, 0.01)
    readonly property real lenZ: Math.max(span.hi.z - span.lo.z, 0.01)
    readonly property real midY: (span.hi.y + span.lo.y) / 2
    readonly property real midX: (span.hi.x + span.lo.x) / 2
    readonly property real midZ: (span.hi.z + span.lo.z) / 2
    readonly property bool xMajor: lenX >= lenZ

    function segColor(i) {
        var cs = ctx ? ctx.emitterColors : null
        return (cs && i < cs.length) ? cs[i] : "#000000"
    }

    /* Diffuser bar — long axis along the emitter span. */
    Model {
        objectName: body.pickName
        pickable: body.pickOn
        source: "#Cube"
        position: Qt.vector3d(body.midX, body.midY - 0.0025,
                              body.midZ)
        scale: body.xMajor
               ? Qt.vector3d((body.lenX + 0.012) / 100, 0.0035 / 100,
                             Math.max(body.lenZ * 0.6, 0.005) / 100)
               : Qt.vector3d(Math.max(body.lenX * 0.6, 0.005) / 100,
                             0.0035 / 100, (body.lenZ + 0.012) / 100)
        materials: PrincipledMaterial {
            baseColor: "#dcdce8"
            opacity: 0.4
            roughness: 0.25
        }
    }

    /* Per-emitter lens segments riding the bar. */
    Repeater3D {
        model: body.ctx ? body.ctx.emitterPos : []
        delegate: Model {
            required property var modelData
            source: "#Cube"
            position: Qt.vector3d(modelData.x, modelData.y - 0.0018,
                                  modelData.z)
            scale: Qt.vector3d(0.012 / 100, 0.0025 / 100, 0.012 / 100)
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
