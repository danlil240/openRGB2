/*---------------------------------------------------------*\
||| Mouse.qml                                             ||
|||                                                       ||
|||   mouse_body family body (spec §3): a curved shell   ||
|||   (scaled-sphere dome), a pinched base that leaves   ||
|||   the underglow ring emitters proud of the body, a   ||
|||   scroll wheel, and a button split line. The logo /  ||
|||   wheel / underglow zones are separate mouse_zone    ||
|||   objects whose emitter dots stay the color channel  ||
|||   — this body only has to not cover them.            ||
|||                                                       ||
|||   `ctx` = StudioViewport object delegate (see           ||
|||   Keyboard.qml for the contract).                    ||
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

    /* Narrow base slab — its half-width (0.72 * bx / 2) stays
       inside the underglow ring radius (~28 mm), so the ring
       emitters at the shell's widest silhouette read cleanly
       from above and from the side. */
    Model {
        objectName: body.pickName
        pickable: body.pickOn
        source: "#Cube"
        position: Qt.vector3d(0, -body.by * 0.32, 0)
        scale: Qt.vector3d(body.bx * 0.72 / 100, body.by * 0.22 / 100,
                           body.bz * 0.90 / 100)
        materials: PrincipledMaterial {
            baseColor: "#1a1a20"
            metalness: 0.15
            roughness: 0.6
        }
    }

    /* Curved shell — a squashed ellipsoid dome. Pinched hard at
       the bottom (narrow y radius, center lifted) so its lower
       cross-sections shrink below the underglow ring radius
       instead of swallowing the side dots (SLW-style regression
       guard). */
    Model {
        objectName: body.pickName
        pickable: body.pickOn
        source: "#Sphere"
        position: Qt.vector3d(0, body.by * 0.10, body.bz * 0.02)
        scale: Qt.vector3d(body.bx * 0.47 / 50, body.by * 0.45 / 50,
                           body.bz * 0.45 / 50)
        materials: PrincipledMaterial {
            baseColor: "#22222a"
            metalness: 0.2
            roughness: 0.45
        }
    }

    /* Scroll wheel — cylinder axis along X (quaternion: 90 deg
       about Z), poking through the dome near the nose. */
    Model {
        objectName: body.pickName
        pickable: body.pickOn
        source: "#Cylinder"
        position: Qt.vector3d(0, body.by * 0.28, -body.bz * 0.33)
        rotation: Qt.quaternion(0.70710678, 0, 0, 0.70710678)
        scale: Qt.vector3d(0.007 / 100, 0.009 / 50, 0.009 / 50)
        materials: PrincipledMaterial {
            baseColor: "#38383f"
            metalness: 0.0
            roughness: 0.7
        }
    }
}
