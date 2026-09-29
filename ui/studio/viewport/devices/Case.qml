/*---------------------------------------------------------*\
||| Case.qml                                              ||
|||                                                       ||
|||   case_shell family body (spec §3): corner frame,    ||
|||   top/bottom caps, back + front panels, a solid      ||
|||   right side panel and a glass left panel, plus      ||
|||   interior cues (motherboard tray, PSU shroud, fan   ||
|||   mounts).                                           ||
|||                                                       ||
|||   Ghost contract (unchanged from the flat-cube       ||
|||   shell): the case is a CONTAINER — every part is    ||
|||   non-pickable so clicks pass through to hardware    ||
|||   inside, and ctx.ghosted drops every opacity   ||
|||   to near-invisible. Panels are translucent even     ||
|||   when solid-looking so interior devices always read.||
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

    /* caseGhost toggle — the delegate's `ghosted` flag carries the
       user pref (UiSettings.viewportCaseGhost through
       StudioViewport); guarded so bare-component tests stay
       fully solid. */
    readonly property bool ghost: ctx ? ctx.ghosted === true : false
    function panelOp(solid) { return ghost ? solid * 0.22 : solid }

    /* ---- frame: 4 vertical corner posts ---- */
    Repeater3D {
        model: 4
        delegate: Model {
            required property var modelData
            property int index: modelData
            source: "#Cube"
            property real c: (index % 2 === 0 ? 1 : -1)
            property real d: (index < 2 ? 1 : -1)
            position: Qt.vector3d(c * (body.bx / 2 - 0.008), 0,
                                  d * (body.bz / 2 - 0.008))
            scale: Qt.vector3d(0.016 / 100, body.by / 100, 0.016 / 100)
            materials: PrincipledMaterial {
                baseColor: "#1e1e24"; metalness: 0.4; roughness: 0.5
                opacity: body.panelOp(0.75)
            }
        }
    }

    /* ---- top + bottom caps ---- */
    Repeater3D {
        model: 2
        delegate: Model {
            required property var modelData
            property int index: modelData
            source: "#Cube"
            property real s: index === 0 ? 1 : -1
            position: Qt.vector3d(0, s * (body.by / 2 - 0.006), 0)
            scale: Qt.vector3d(body.bx / 100, 0.012 / 100,
                               body.bz / 100)
            materials: PrincipledMaterial {
                baseColor: "#23232a"; metalness: 0.35; roughness: 0.55
                opacity: body.panelOp(0.7)
            }
        }
    }

    /* ---- back + front panels (z faces) ---- */
    Repeater3D {
        model: 2
        delegate: Model {
            required property var modelData
            property int index: modelData
            source: "#Cube"
            property real s: index === 0 ? 1 : -1
            position: Qt.vector3d(0, 0, s * (body.bz / 2 - 0.0035))
            scale: Qt.vector3d(body.bx / 100, body.by / 100,
                               0.007 / 100)
            materials: PrincipledMaterial {
                baseColor: index === 0 ? "#22222a" : "#1b1b22"
                metalness: 0.3
                roughness: 0.6
                opacity: body.panelOp(0.5)
            }
        }
    }

    /* ---- right side panel (solid) — x = +bx/2 ---- */
    Model {
        source: "#Cube"
        position: Qt.vector3d(body.bx / 2 - 0.0035, 0, 0)
        scale: Qt.vector3d(0.007 / 100, body.by / 100, body.bz / 100)
        materials: PrincipledMaterial {
            baseColor: "#24242c"; metalness: 0.3; roughness: 0.6
            opacity: body.panelOp(0.5)
        }
    }

    /* ---- left side panel (glass) — x = -bx/2.
       Always the most translucent face; under ghost it all but
       disappears. Non-pickable like the rest of the shell. ---- */
    Model {
        source: "#Cube"
        position: Qt.vector3d(-body.bx / 2 + 0.002, 0, 0)
        scale: Qt.vector3d(0.004 / 100, body.by * 0.94 / 100,
                           body.bz * 0.92 / 100)
        materials: PrincipledMaterial {
            baseColor: "#8a94a8"
            metalness: 0.0
            roughness: 0.08
            opacity: body.ghost ? 0.05 : 0.16
        }
    }

    /* ---- interior cues (visible through the glass) ---- */

    /* Motherboard tray — offset toward the solid side. */
    Model {
        source: "#Cube"
        position: Qt.vector3d(body.bx * 0.28, body.by * 0.08, 0)
        scale: Qt.vector3d(0.008 / 100, body.by * 0.62 / 100,
                           body.bz * 0.72 / 100)
        materials: PrincipledMaterial {
            baseColor: "#17171d"; roughness: 0.8
            opacity: body.panelOp(0.85)
        }
    }

    /* PSU shroud along the bottom. */
    Model {
        source: "#Cube"
        position: Qt.vector3d(0, -body.by * 0.40, 0)
        scale: Qt.vector3d(body.bx * 0.82 / 100, body.by * 0.16 / 100,
                           body.bz * 0.86 / 100)
        materials: PrincipledMaterial {
            baseColor: "#1d1d24"; roughness: 0.7
            opacity: body.panelOp(0.8)
        }
    }

    /* Fan mounts — three thin frames on the front panel, one on
       the back. */
    Repeater3D {
        model: 4
        delegate: Model {
            required property var modelData
            property int index: modelData
            source: "#Cube"
            property bool front: index < 3
            position: front
                      ? Qt.vector3d(0,
                                    body.by * (0.22 - index * 0.22),
                                    -body.bz / 2 + 0.012)
                      : Qt.vector3d(0, body.by * 0.18,
                                    body.bz / 2 - 0.012)
            scale: Qt.vector3d(body.bx * 0.55 / 100, body.by * 0.19 / 100,
                               0.006 / 100)
            materials: PrincipledMaterial {
                baseColor: "#14141a"; roughness: 0.8
                opacity: body.panelOp(0.6)
            }
        }
    }
}
