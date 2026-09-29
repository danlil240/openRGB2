/*---------------------------------------------------------*\
||| SelectionFrame.qml                                    ||
|||                                                       ||
|||   Selection treatment (spec §3: "outline/bounds +    ||
|||   clear handles"): a 12-edge wireframe box around    ||
|||   the resolved body bounds plus 8 corner handles.    ||
|||   NoLighting — readable at every quality tier and    ||
|||   never bloom-dependent. Instantiated directly in    ||
|||   the StudioViewport delegate so its size binds the     ||
|||   node's oBx/oBy/oBz.                                ||
||   SPDX-License-Identifier: GPL-2.0-or-later            ||
\*---------------------------------------------------------*/
import QtQuick
import QtQuick3D

Node {
    id: frame

    property real bx: 0
    property real by: 0
    property real bz: 0
    property color selColor: "#ffd040"

    /* 2 mm edges, 4 mm overshoot so the frame clears the body. */
    readonly property real t:   0.002
    readonly property real pad: 0.004
    readonly property real hx: bx / 2 + pad
    readonly property real hy: by / 2 + pad
    readonly property real hz: bz / 2 + pad

    /* Degenerate bounds (zero-size bodies, empty groups) still
       show a small marker so the selection is locatable. */
    readonly property bool degenerate: (bx + by + bz) < 0.005

    Repeater3D {
        model: 12
        delegate: Model {
            required property var modelData
            property int index: modelData
            source: "#Cube"
            /* Edges: 0-3 along X (y,z corners), 4-7 along Y,
               8-11 along Z. */
            property int axis: Math.floor(index / 4)
            property real a: (index % 2 === 0) ? 1 : -1
            property real b: (Math.floor(index / 2) % 2 === 0) ? 1 : -1
            position: axis === 0 ? Qt.vector3d(0, a * frame.hy, b * frame.hz)
                    : axis === 1 ? Qt.vector3d(a * frame.hx, 0, b * frame.hz)
                                 : Qt.vector3d(a * frame.hx, b * frame.hy, 0)
            scale: axis === 0 ? Qt.vector3d(frame.bx / 100 + 0.00008, 0.00002, 0.00002)
                 : axis === 1 ? Qt.vector3d(0.00002, frame.by / 100 + 0.00008, 0.00002)
                              : Qt.vector3d(0.00002, 0.00002, frame.bz / 100 + 0.00008)
            materials: PrincipledMaterial {
                lighting: PrincipledMaterial.NoLighting
                baseColor: frame.selColor
            }
        }
    }

    Repeater3D {
        model: 8
        delegate: Model {
            required property var modelData
            property int index: modelData
            source: "#Cube"
            property real cx: (index & 1) ? 1 : -1
            property real cy: (index & 2) ? 1 : -1
            property real cz: (index & 4) ? 1 : -1
            position: Qt.vector3d(cx * frame.hx, cy * frame.hy,
                                  cz * frame.hz)
            scale: Qt.vector3d(0.00009, 0.00009, 0.00009)  /* 9 mm cubes */
            materials: PrincipledMaterial {
                lighting: PrincipledMaterial.NoLighting
                baseColor: frame.selColor
            }
        }
    }

    /* Point marker for degenerate bounds. */
    Model {
        visible: frame.degenerate
        source: "#Sphere"
        scale: Qt.vector3d(0.00014, 0.00014, 0.00014)
        materials: PrincipledMaterial {
            lighting: PrincipledMaterial.NoLighting
            baseColor: frame.selColor
        }
    }
}
