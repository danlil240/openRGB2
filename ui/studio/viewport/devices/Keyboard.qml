/*---------------------------------------------------------*\
||| Keyboard.qml                                          ||
|||                                                       ||
|||   keyboard_body family body (spec §3 "Device         ||
|||   graphics"): shaped chassis, a recessed top deck,   ||
|||   and one keycap per real emitter — cap positions    ||
|||   come straight from ctx.emitterPos, so the rendered ||
|||   keys ARE the readable key/LED map. Caps top out    ||
|||   below the emitter-dot plane so dots stay fully     ||
|||   visible; each cap carries a soft emissive tint     ||
|||   from the shared emitterColors buffer ("emissive    ||
|||   key surfaces").                                    ||
|||                                                       ||
|||   Contract: `ctx` is the StudioViewport object delegate ||
|||   (oBx/oBy/oBz meters, oId pick id, ghostBody,       ||
|||   emitterPos, emitterColors). Drawn in meters; the   ||
|||   100-unit primitives scale as meters/100.           ||
||   SPDX-License-Identifier: GPL-2.0-or-later            ||
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

    function capColor(i) {
        var cs = ctx ? ctx.emitterColors : null
        return (cs && i < cs.length) ? cs[i] : "#000000"
    }

    /* Chassis slab — full footprint, lower 60% of the body height. */
    Model {
        objectName: body.pickName
        pickable: body.pickOn
        source: "#Cube"
        position: Qt.vector3d(0, -body.by * 0.20, 0)
        scale: Qt.vector3d(body.bx / 100, body.by * 0.60 / 100,
                           body.bz / 100)
        materials: PrincipledMaterial {
            baseColor: "#1c1c22"
            metalness: 0.25
            roughness: 0.6
        }
    }

    /* Inset top deck — the plate the caps stand on. */
    Model {
        objectName: body.pickName
        pickable: body.pickOn
        source: "#Cube"
        position: Qt.vector3d(0, body.by * 0.13, -body.bz * 0.015)
        scale: Qt.vector3d(body.bx * 0.965 / 100, body.by * 0.34 / 100,
                           body.bz * 0.90 / 100)
        materials: PrincipledMaterial {
            baseColor: "#24242c"
            metalness: 0.2
            roughness: 0.55
        }
    }

    /* One keycap per emitter — readable key positions by
       construction. Standard 19 mm pitch reads as ~16.5 mm caps
       with a visible grid gap. Cap tops sit below the dot plane
       (dots at by/2 + ~1 mm) so the shared emitter dots are never
       sealed inside the geometry. */
    Repeater3D {
        model: body.ctx ? body.ctx.emitterPos : []
        delegate: Model {
            required property var modelData
            objectName: body.pickName
            pickable: body.pickOn
            source: "#Cube"
            /* 4 mm tall cap, top ~1.5 mm below the emitter dot. */
            position: Qt.vector3d(modelData.x, modelData.y - 0.0046,
                                  modelData.z)
            scale: Qt.vector3d(0.0165 / 100, 0.0035 / 100, 0.0165 / 100)
            materials: PrincipledMaterial {
                baseColor: "#2e2e38"
                metalness: 0.05
                roughness: 0.5
                property color ec: body.capColor(modelData.i)
                emissiveFactor: Qt.rgba(ec.r * 0.45, ec.g * 0.45,
                                        ec.b * 0.45, 1.0)
            }
        }
    }
}
