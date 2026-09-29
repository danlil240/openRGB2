/*---------------------------------------------------------*\
||  tst_devices.qml                                        ||
||                                                         ||
||  studio-import Task 8 — adapted from the source of     ||
||  record tests/editor_qml/tst_devices.qml. Asserts:     ||
||    - every device-family file + the environment        ||
||      component compiles and instantiates               ||
||    - geometry tag -> family component dispatch         ||
||      (familySource) with generic fallback for unknown  ||
||    - quality tier transitions drive AA/AO/glow on the  ||
||      environment while the emitter path (sizes,        ||
||      fallback spec table, per-tick color buffer) is    ||
||      identical under every tier                        ||
||    - case shell keeps the ghost/pick-exclusion flags   ||
||    - family components instantiate under a real        ||
||      View3D with a plain-object ctx stub               ||
||                                                         ||
||  SPDX-License-Identifier: GPL-2.0-or-later              ||
\*---------------------------------------------------------*/
import QtQuick
import QtQuick3D
import QtTest
import "../../../ui/studio/viewport" as Vp

TestCase {
    id: tc
    name: "ViewportDevices"
    when: windowShown

    property string uiDir: "../../../ui/studio/viewport/"

    /* A real 3D parent for family components — Node roots can't
       be parented to plain Items. */
    View3D {
        id: view3d
        width: 64; height: 64; visible: false
        Node { id: nodeParent }
    }

    function mkScene() {
        var c = Qt.createComponent(Qt.resolvedUrl(
                uiDir + "StudioViewport.qml"))
        compare(c.status, Component.Ready, c.errorString())
        var s = c.createObject(tc)
        verify(s !== null, "StudioViewport failed to instantiate")
        return s
    }

    /*---------------- compile smoke ----------------*/

    function test_family_files_compile() {
        var files = [ "devices/Keyboard.qml", "devices/Mouse.qml",
                      "devices/Fan.qml", "devices/Ram.qml",
                      "devices/Case.qml", "devices/Gpu.qml",
                      "devices/Monitor.qml", "devices/Strip.qml",
                      "devices/SelectionFrame.qml",
                      "materials/StudioEnvironment.qml" ]
        for (var i = 0; i < files.length; i++) {
            var c = Qt.createComponent(Qt.resolvedUrl(uiDir + files[i]))
            compare(c.status, Component.Ready,
                    files[i] + ": " + c.errorString())
        }
    }

    /*---------------- dispatch ----------------*/

    function test_family_dispatch() {
        var s = mkScene()
        var want = {
            "keyboard_body": "devices/Keyboard.qml",
            "mouse_body":    "devices/Mouse.qml",
            "fan_body":      "devices/Fan.qml",
            "pump_body":     "devices/Fan.qml",
            "ram_body":      "devices/Ram.qml",
            "case_shell":    "devices/Case.qml",
            "gpu_body":      "devices/Gpu.qml",
            "gpu_logo":      "devices/Strip.qml",
            "monitor":       "devices/Monitor.qml"
        }
        for (var g in want)
            compare(s.familySource(g), want[g], g)
        /* Fallback / body-less tags stay on the old path. */
        compare(s.familySource("desk"), "")
        compare(s.familySource("mouse_zone"), "")
        compare(s.familySource("group"), "")
        compare(s.familySource("mystery_widget"), "")
        /* Fallback spec table + ghost flag survive. */
        compare(s.bodySpec("desk").src, "#Cube")
        compare(s.bodySpec("case_shell").ghost, true)
        compare(s.bodySpec("mouse_zone"), null)
        s.destroy()
    }

    /*---------------- quality tiers ----------------*/

    function test_tier_drives_environment() {
        var s = mkScene()
        var env = s.sceneEnvironment
        verify(env !== null, "sceneEnvironment seam missing")

        s.setQualityTier("low")
        compare(s.qualityTier, "low")
        compare(env.quality, "low")
        compare(env.antialiasingMode, SceneEnvironment.NoAA)
        verify(env.fxaaEnabled)
        verify(!env.aoEnabled)
        verify(!env.glowEnabled)

        s.setQualityTier("balanced")
        compare(env.antialiasingMode, SceneEnvironment.MSAA)
        verify(env.aoEnabled)
        verify(env.glowEnabled)

        s.setQualityTier("high")
        verify(env.temporalAAEnabled)
        verify(env.specularAAEnabled)
        verify(env.glowQualityHigh)

        /* Invalid tier is refused. */
        s.setQualityTier("ultra")
        compare(s.qualityTier, "high")

        s.destroy()
    }

    /* The emitter path is identical under every tier — same dot
       size table, same fallback spec table; the per-tick color
       buffer is a model read the env never touches. */
    function test_tier_preserves_emitter_path() {
        var s = mkScene()
        var before = { k: s.emitterSize("keyboard_body"),
                       d: s.bodySpec("desk").c,
                       g: s.bodySpec("case_shell").ghost }
        for (var i = 0; i < 3; i++)
            s.setQualityTier(["low", "balanced", "high"][i])
        compare(s.emitterSize("keyboard_body"), before.k)
        compare(s.bodySpec("desk").c, before.d)
        compare(s.bodySpec("case_shell").ghost, before.g)
        s.destroy()
    }

    /* Balanced is the spec default — the local property defaults
       to it and the UiSettings shell pref reads it back when no
       draft is loaded (round-trip covered in the C++ suite). */
    function test_balanced_is_default() {
        var s = mkScene()
        compare(s.qualityTier, "balanced")
        s.destroy()
    }

    /*---------------- family instantiation ----------------*/

    /* Plain-object ctx stub — mirrors the StudioViewport delegate
       contract (oBx/oBy/oBz, oId, oGeom, ghostBody, emitterPos,
       emitterColors). */
    function fakeCtx(geom, bx, by, bz, emitters) {
        return { oId: "test_" + geom, oGeom: geom,
                 oBx: bx, oBy: by, oBz: bz, ghostBody: false,
                 emitterPos: emitters || [], emitterColors: [] }
    }

    function test_families_instantiate() {
        var files = {
            "devices/Keyboard.qml":
                fakeCtx("keyboard_body", 0.45, 0.03, 0.145,
                        [{ x: 0, y: 0.016, z: 0, i: 0, c: "#fff" }]),
            "devices/Mouse.qml":
                fakeCtx("mouse_body", 0.066, 0.04, 0.117),
            "devices/Fan.qml":
                fakeCtx("fan_body", 0.125, 0.028, 0.125,
                        [{ x: 0.052, y: 0.016, z: 0, i: 0, c: "#fff" }]),
            "devices/Ram.qml":
                fakeCtx("ram_body", 0.135, 0.045, 0.008,
                        [{ x: -0.054, y: 0.03, z: 0, i: 0, c: "#fff" }]),
            "devices/Case.qml":
                fakeCtx("case_shell", 0.21, 0.47, 0.46),
            "devices/Gpu.qml":
                fakeCtx("gpu_body", 0.30, 0.05, 0.13),
            "devices/Monitor.qml":
                fakeCtx("monitor", 0.62, 0.36, 0.02),
            "devices/Strip.qml":
                fakeCtx("gpu_logo", 0, 0, 0,
                        [{ x: -0.03, y: 0, z: 0, i: 0, c: "#fff" },
                         { x: 0.03, y: 0, z: 0, i: 1, c: "#fff" }]),
            "devices/SelectionFrame.qml": null
        }
        for (var f in files) {
            var c = Qt.createComponent(Qt.resolvedUrl(uiDir + f))
            compare(c.status, Component.Ready, f + ": " + c.errorString())
            var n = c.createObject(nodeParent)
            verify(n !== null, f + " failed to instantiate")
            if (files[f])
                n.ctx = files[f]
            /* Fan's pump variant shares the component. */
            n.destroy()
        }

        /* Pump variant + ghost case still create cleanly. */
        var fc = Qt.createComponent(Qt.resolvedUrl(
                    uiDir + "devices/Fan.qml"))
        var pump = fc.createObject(nodeParent)
        pump.ctx = fakeCtx("pump_body", 0.055, 0.045, 0.055)
        verify(pump.isPump)
        pump.destroy()
    }

    /* Pick contract: device bodies are pickable under a normal
       ctx, and ghosted bodies (the case shell flag) switch every
       pick surface off so interior hardware stays selectable. */
    function test_pick_contract() {
        var kc = Qt.createComponent(Qt.resolvedUrl(
                    uiDir + "devices/Keyboard.qml"))
        compare(kc.status, Component.Ready, kc.errorString())
        var kb = kc.createObject(nodeParent)
        kb.ctx = fakeCtx("keyboard_body", 0.45, 0.03, 0.145,
                         [{ x: 0, y: 0.016, z: 0, i: 0, c: "#fff" }])
        compare(kb.pickName, "obj|test_keyboard_body")
        verify(kb.pickOn, "keyboard body must be pickable")
        var ghost = fakeCtx("case_shell", 0.21, 0.47, 0.46)
        ghost.ghostBody = true
        kb.ctx = ghost
        verify(!kb.pickOn, "ghost body must not be pickable")
        kb.destroy()
    }
}
