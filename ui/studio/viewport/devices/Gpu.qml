/*---------------------------------------------------------*\
||| Gpu.qml                                               ||
|||                                                       ||
|||   gpu_body family body (spec §3): shroud, backplate, ||
|||   two fan recesses and a PCIe bracket tab. The lit   ||
|||   logo/fans are separate gpu_logo / fan_body objects ||
|||   — this is the decor card.                          ||
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

    /* Shroud — lower 70% of the card. */
    Model {
        objectName: body.pickName
        pickable: body.pickOn
        source: "#Cube"
        position: Qt.vector3d(0, -body.by * 0.12, 0)
        scale: Qt.vector3d(body.bx / 100, body.by * 0.72 / 100,
                           body.bz * 0.94 / 100)
        materials: PrincipledMaterial {
            baseColor: "#23232b"; metalness: 0.35; roughness: 0.5
        }
    }

    /* Backplate — thin plate proud of the top face. */
    Model {
        objectName: body.pickName
        pickable: body.pickOn
        source: "#Cube"
        position: Qt.vector3d(0, body.by * 0.30, 0)
        scale: Qt.vector3d(body.bx * 0.98 / 100, 0.003 / 100,
                           body.bz * 0.92 / 100)
        materials: PrincipledMaterial {
            baseColor: "#2e2e38"; metalness: 0.6; roughness: 0.4
        }
    }

    /* Fan recesses — two dark discs sunk into the front face. */
    Repeater3D {
        model: 2
        delegate: Model {
            required property var modelData
            property int index: modelData
            objectName: body.pickName
            pickable: body.pickOn
            source: "#Cylinder"
            property real s: index === 0 ? 1 : -1
            position: Qt.vector3d(s * body.bx * 0.22, 0,
                                  -body.bz / 2 + 0.002)
            /* Axis along Z: 90 deg about X. */
            rotation: Qt.quaternion(0.70710678, 0.70710678, 0, 0)
            scale: Qt.vector3d(0.052 / 100, 0.004 / 100, 0.052 / 100)
            materials: PrincipledMaterial {
                baseColor: "#15151c"; roughness: 0.7
            }
        }
    }

    /* PCIe bracket tab at the card's rear edge. */
    Model {
        objectName: body.pickName
        pickable: body.pickOn
        source: "#Cube"
        position: Qt.vector3d(-body.bx / 2 + 0.003, body.by * 0.15, 0)
        scale: Qt.vector3d(0.006 / 100, body.by * 1.1 / 100,
                           body.bz * 0.5 / 100)
        materials: PrincipledMaterial {
            baseColor: "#8a8a92"; metalness: 0.7; roughness: 0.35
        }
    }
}
