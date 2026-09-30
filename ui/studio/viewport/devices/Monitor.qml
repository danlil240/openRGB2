/*---------------------------------------------------------*\
||| Monitor.qml                                           ||
|||                                                       ||
|||   monitor family body (spec §3): bezel panel, an     ||
|||   inset dark screen face on the front, and a VESA    ||
|||   bulge on the back. Decor — no emitters.            ||
|||                                                       ||
|||   `ctx` = StudioViewport object delegate.               ||
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

    /* Panel / bezel. */
    Model {
        objectName: body.pickName
        pickable: body.pickOn
        source: "#Cube"
        scale: Qt.vector3d(body.bx / 100, body.by / 100, body.bz / 100)
        materials: PrincipledMaterial {
            baseColor: "#16161c"; metalness: 0.2; roughness: 0.55
        }
    }

    /* Screen face — inset glass on the -z face, faintly lit so
       the panel reads as a display, not a slab. */
    Model {
        objectName: body.pickName
        pickable: body.pickOn
        source: "#Cube"
        position: Qt.vector3d(0, 0, -body.bz / 2 - 0.0006)
        scale: Qt.vector3d(body.bx * 0.95 / 100, body.by * 0.90 / 100,
                           0.0012 / 100)
        materials: PrincipledMaterial {
            baseColor: "#0a0c12"
            metalness: 0.0
            roughness: 0.15
            emissiveFactor: Qt.rgba(0.035, 0.045, 0.075, 1.0)
        }
    }

    /* VESA bulge on the back. */
    Model {
        objectName: body.pickName
        pickable: body.pickOn
        source: "#Cube"
        position: Qt.vector3d(0, -body.by * 0.05, body.bz / 2 + 0.008)
        scale: Qt.vector3d(0.15 / 100, body.by * 0.4 / 100,
                           0.018 / 100)
        materials: PrincipledMaterial {
            baseColor: "#1c1c22"; metalness: 0.25; roughness: 0.6
        }
    }
}
