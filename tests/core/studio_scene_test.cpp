/*---------------------------------------------------------*\
|| studio_scene_test.cpp                                     |
||                                                           |
||   Deterministic tests for the Desktop Lighting Studio    |
||   scene core (plugins/DesktopLightingStudio/scene/).     |
||   No Qt, no hardware — plain cl build.                   |
||                                                           |
||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "scene/SceneTypes.h"
#include "scene/SceneGraph.h"
#include "scene/EmitterLayout.h"
#include "scene/SceneJson.h"
#include "scene/BindingResolver.h"
#include "scene/DefaultDesk.h"
#include "effects/EffectTypes.h"
#include "effects/EffectEngine.h"
#include "effects/Presets.h"
#include "inputs/InputBus.h"
#include "inputs/OnsetDetect.h"
#include "inputs/KeyMap.h"

#include <cmath>
#include <cstdio>
#include <string>

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, name)                                              \
    do {                                                               \
        ++checks;                                                      \
        if(!(cond)) { ++failures; std::printf("FAIL: %s\n", name); }   \
    } while(0)

static bool Near(float a, float b, float eps = 1e-4f)
{
    return std::fabs(a - b) < eps;
}

static void TestTransform()
{
    using namespace studio;

    Transform t;
    t.position = { 1.0f, 2.0f, 3.0f };
    Vec3 p = TransformPoint(t, { 0.1f, 0.0f, 0.0f });
    CHECK(Near(p.x, 1.1f) && Near(p.y, 2.0f) && Near(p.z, 3.0f), "transform translate");

    t.scale = { 2.0f, 1.0f, 1.0f };
    p = TransformPoint(t, { 0.1f, 0.0f, 0.0f });
    CHECK(Near(p.x, 1.2f), "transform scale");

    /* +90 deg about X: +Y -> +Z */
    t = Transform{};
    t.rotation_deg = { 90.0f, 0.0f, 0.0f };
    p = TransformPoint(t, { 0.0f, 1.0f, 0.0f });
    CHECK(Near(p.z, 1.0f) && Near(p.y, 0.0f), "transform rotX 90");

    /* +90 deg about Y: +X -> -Z */
    t.rotation_deg = { 0.0f, 90.0f, 0.0f };
    p = TransformPoint(t, { 1.0f, 0.0f, 0.0f });
    CHECK(Near(p.z, -1.0f) && Near(p.x, 0.0f), "transform rotY 90");

    /* +90 deg about Z: +X -> +Y */
    t.rotation_deg = { 0.0f, 0.0f, 90.0f };
    p = TransformPoint(t, { 1.0f, 0.0f, 0.0f });
    CHECK(Near(p.y, 1.0f) && Near(p.x, 0.0f), "transform rotZ 90");
}

static void TestRing()
{
    using namespace studio;

    auto ring = layout::Ring(8, 0.05f, 0.0f, false, "g");
    CHECK(ring.size() == 8, "ring count");
    CHECK(Near(ring[0].local_pos.x, 0.05f) && Near(ring[0].local_pos.z, 0.0f), "ring led0 +X");
    /* address 2 at 90 deg -> +X rotates toward -Z */
    CHECK(Near(ring[2].local_pos.x, 0.0f) && Near(ring[2].local_pos.z, -0.05f), "ring led2 angle");
    CHECK(ring[3].address == 3, "ring sequential addresses");

    auto rev = layout::Ring(8, 0.05f, 0.0f, true, "g");
    CHECK(Near(rev[2].local_pos.z, 0.05f), "ring reversed direction");

    auto rot = layout::Ring(8, 0.05f, 90.0f, false, "g", 4);
    CHECK(Near(rot[0].local_pos.x, 0.0f) && Near(rot[0].local_pos.z, -0.05f), "ring start angle");
    CHECK(rot[0].address == 4, "ring address base");

    auto face = layout::Ring(8, 0.05f, 0.0f, false, "g", 0, 0.016f);
    CHECK(Near(face[0].local_pos.y, 0.016f) && Near(face[4].local_pos.y, 0.016f),
          "ring face lift");

    /* all emitters on the circle */
    for(const auto& e : ring)
    {
        const float r = std::sqrt(e.local_pos.x * e.local_pos.x + e.local_pos.z * e.local_pos.z);
        CHECK(Near(r, 0.05f), "ring radius");
    }
}

static void TestStripAndMatrix()
{
    using namespace studio;

    auto strip = layout::Strip(10, 0.012f, { -0.05f, 0.01f, 0.0f }, "g");
    CHECK(strip.size() == 10, "strip count");
    CHECK(Near(strip[9].local_pos.x, -0.05f + 9 * 0.012f), "strip spacing");

    std::vector<unsigned int> map = { 0, 1, 99, 2, 3, 4 };
    auto keys = layout::KeyboardMatrix(2, 3, map, 99u, 0.019f, 0.019f, { 0, 0, 0 }, "kbd");
    /* a short map stops at its end — never reads out of bounds */
    std::vector<unsigned int> short_map = { 0, 1 };
    auto short_keys = layout::KeyboardMatrix(2, 3, short_map, 99u,
                                             0.019f, 0.019f, { 0, 0, 0 }, "kbd");
    CHECK(short_keys.size() == 2, "matrix short map is bounds-safe");
    CHECK(keys.size() == 5, "matrix skips empty cell");
    CHECK(keys[2].address == 2, "matrix address from map");
    CHECK(Near(keys[4].local_pos.z, -0.019f), "matrix row 1 offset");
}

static void TestMirrorSemantics()
{
    using namespace studio;

    SceneDocument doc;
    SceneObject owner;
    owner.id   = "ring";
    owner.kind = ObjectKind::Device;
    doc.objects.push_back(owner);

    SceneObject copy;
    copy.id        = "ring_copy";
    copy.kind      = ObjectKind::Linked;
    copy.mirror_of = "ring";
    doc.objects.push_back(copy);

    doc.object_colors["ring"] = MakeSceneColor(10, 20, 30);
    doc.emitter_colors["ring"][2] = MakeSceneColor(200, 0, 0);

    CHECK(OutputOwner(doc, "ring_copy") != nullptr
          && OutputOwner(doc, "ring_copy")->id == "ring", "mirror follows owner");
    CHECK(EmitterColor(doc, "ring_copy", 0) == MakeSceneColor(10, 20, 30),
          "mirror reads source base color");
    CHECK(EmitterColor(doc, "ring_copy", 2) == MakeSceneColor(200, 0, 0),
          "mirror reads source emitter override");
    CHECK(EmitterColor(doc, "missing", 0) == 0, "missing object -> black");
}

static void TestJsonRoundTrip()
{
    using namespace studio;

    SceneDocument doc = BuildDefaultDesk();
    doc.brightness = 0.7f;
    doc.emitter_colors["keyboard"][42] = MakeSceneColor(1, 2, 3);

    SceneDocument back;
    CHECK(FromJson(ToJson(doc), back), "json round-trip parses");
    CHECK(back.version == 2, "json version");
    CHECK(back.objects.size() == doc.objects.size(), "json object count");
    CHECK(back.bindings.size() == doc.bindings.size(), "json binding count");
    CHECK(Near(back.brightness, 0.7f), "json brightness");
    CHECK(back.emitter_colors["keyboard"][42] == MakeSceneColor(1, 2, 3),
          "json emitter override");

    const SceneObject* kbd = FindObject(back, "keyboard");
    CHECK(kbd != nullptr && kbd->kind == ObjectKind::Device
          && kbd->layout == "matrix_map" && kbd->verified, "json keyboard fields");

    /* unknown fields tolerated */
    nlohmann::json j = ToJson(doc);
    j["future_field"] = { {"nested", 1} };
    j["objects"][0]["future"] = "x";
    SceneDocument tol;
    CHECK(FromJson(j, tol) && tol.objects.size() == doc.objects.size(),
          "json unknown fields tolerated");

    /* newer version rejected */
    j["version"] = 99;
    CHECK(!FromJson(j, tol), "json newer version rejected");

    /* malformed rejected */
    CHECK(!FromJson(nlohmann::json::array(), tol), "json malformed rejected");
}

static std::vector<studio::ControllerSnapshot> FakeControllers()
{
    using namespace studio;
    std::vector<ControllerSnapshot> cs;

    ControllerSnapshot kbd;
    kbd.name = "Logitech G512"; kbd.vendor = "Logitech"; kbd.device_type = 5;
    kbd.zones.push_back({ "Keyboard", 107, 0, 2, true });
    cs.push_back(kbd);

    ControllerSnapshot mb;
    mb.name = "X870E AORUS ELITE WIFI7 ICE"; mb.vendor = "Gigabyte";
    mb.device_type = 0; mb.location = "HID: /dev/0";
    mb.zones.push_back({ "ARGB_V2_1", 8, 0, 3, true });
    mb.zones.push_back({ "ARGB_V2_2", 16, 8, 3, true });
    cs.push_back(mb);

    ControllerSnapshot dimm0;
    dimm0.name = "Corsair Vengeance RGB"; dimm0.vendor = "Corsair";
    dimm0.device_type = 1; dimm0.location = "SMBus: 0x58";
    dimm0.zones.push_back({ "DIMM", 10, 0, 1, true });
    cs.push_back(dimm0);

    ControllerSnapshot dimm1 = dimm0;
    dimm1.location = "SMBus: 0x59";
    dimm1.serial   = "SN123";
    cs.push_back(dimm1);

    return cs;
}

static void TestResolver()
{
    using namespace studio;
    const auto cs = FakeControllers();

    /* exact + substring match, zone by name */
    DeviceBinding b{ "b1", "X870E AORUS ELITE", "Gigabyte", "", "", -1, "ARGB_V2_2", 16 };
    auto r = ResolveBinding(b, cs);
    CHECK(r.status == BindingStatus::Resolved && r.zone_index == 1, "resolve mb zone");

    /* zone LED-count mismatch still resolves but reports */
    b.zone_leds = 8;
    r = ResolveBinding(b, cs);
    CHECK(r.status == BindingStatus::Resolved && !r.reason.empty(), "resolve count mismatch");

    /* identical controllers -> ambiguous without disambiguator */
    DeviceBinding d{ "d0", "Corsair", "Corsair", "", "", 1, "", 0 };
    r = ResolveBinding(d, cs);
    CHECK(r.status == BindingStatus::Ambiguous, "resolve ambiguous dimms");

    /* location narrows to one */
    d.location = "SMBus: 0x59";
    r = ResolveBinding(d, cs);
    CHECK(r.status == BindingStatus::Resolved && r.controller_index == 3,
          "resolve location disambiguation");

    /* serial wins even with a shared name */
    DeviceBinding s{ "s0", "Corsair", "Corsair", "SN123", "", -1, "", 0 };
    r = ResolveBinding(s, cs);
    CHECK(r.status == BindingStatus::Resolved && r.controller_index == 3,
          "resolve serial disambiguation");

    /* missing zone -> unresolved with reason */
    DeviceBinding z{ "z0", "Logitech G512", "", "", "", -1, "NoSuchZone", 0 };
    r = ResolveBinding(z, cs);
    CHECK(r.status == BindingStatus::Unresolved && r.controller_index == 0,
          "resolve zone missing");

    /* missing controller -> unresolved */
    DeviceBinding m{ "m0", "Nonexistent", "", "", "", -1, "", 0 };
    r = ResolveBinding(m, cs);
    CHECK(r.status == BindingStatus::Unresolved && r.controller_index == -1,
          "resolve controller missing");

    /* reordered enumeration still resolves the same identity */
    auto reordered = cs;
    std::swap(reordered[1], reordered[3]);
    r = ResolveBinding(DeviceBinding{ "b1", "X870E AORUS ELITE", "Gigabyte",
                                      "", "", -1, "ARGB_V2_1", 8 }, reordered);
    CHECK(r.status == BindingStatus::Resolved && r.controller_index == 3
          && r.zone_index == 0, "resolve survives reordering");
}

static void TestDefaultDesk()
{
    using namespace studio;
    SceneDocument doc = BuildDefaultDesk();

    CHECK(FindObject(doc, "keyboard") != nullptr, "desk has keyboard");
    CHECK(FindObject(doc, "case_fans") != nullptr, "desk has case fans owner");

    /* mirrored copies exist and point at the owner */
    const SceneObject* mirror = FindObject(doc, "case_fan_b1");
    CHECK(mirror != nullptr && mirror->kind == ObjectKind::Linked
          && mirror->mirror_of == "case_fans", "desk mirror wiring");

    /* unverified GPU fans refuse nothing here but carry the flag */
    const SceneObject* gf = FindObject(doc, "gpu_fan_r");
    CHECK(gf != nullptr && !gf->verified, "gpu fans unverified");

    /* Fan/pump emitters must sit on a face, not the mid-plane —
       dots at |y| < body half-thickness are sealed inside the
       opaque mesh and render as an unlit (black) body.        */
    bool faces_clear = true;
    for(const SceneObject& o : doc.objects)
    {
        const float half = (o.geometry == "fan_body")  ? 0.014f
                         : (o.geometry == "pump_body") ? 0.0225f
                                                     : 0.0f;
        if(half == 0.0f)
        {
            continue;
        }
        for(const Emitter& e : o.emitters)
        {
            if(std::fabs(e.local_pos.y) <= half)
            {
                faces_clear = false;
            }
        }
    }
    CHECK(faces_clear, "fan/pump emitters clear the body face");

    /* every Device binding id exists */
    bool all_bound = true;
    for(const SceneObject& o : doc.objects)
    {
        if(o.kind == ObjectKind::Device && !o.binding.empty())
        {
            bool found = false;
            for(const DeviceBinding& b : doc.bindings)
            {
                if(b.id == o.binding) { found = true; break; }
            }
            if(!found) all_bound = false;
        }
    }
    CHECK(all_bound, "all device objects have bindings");
}

/*---------------------------------------------------------*\
||| Stage 2 — effects engine                                 |
\*---------------------------------------------------------*/
static studio::SceneObject OneLedDevice(const std::string& id, float x)
{
    using namespace studio;
    SceneObject o;
    o.id   = id;
    o.kind = ObjectKind::Device;
    o.geometry = "test_body";
    o.transform.position = { x, 0.0f, 0.0f };
    o.verified = true;
    Emitter e; e.local_pos = { 0, 0, 0 }; e.group = id; e.address = 0;
    o.emitters.push_back(e);
    return o;
}

static int RedOf(studio::SceneColor c) { return (int)(c & 0xFF); }

static void TestPaletteAndBlend()
{
    using namespace studio;

    Palette p = MakePalette({ MakeSceneColor(0, 0, 0), MakeSceneColor(255, 255, 255) });
    CHECK(Near(p.Sample(0.0f).r, 0.0f), "palette stop 0");
    CHECK(Near(p.Sample(0.5f).r, 1.0f), "palette stop 1");
    CHECK(Near(p.Sample(0.25f).r, 0.5f), "palette midpoint");
    /* wraps: u = 1.25 lands back on the 0.5 segment */
    CHECK(Near(p.Sample(1.5f).r, p.Sample(0.5f).r), "palette wraps");
    CHECK(p.stops.empty() == false && Palette{}.Sample(0.3f).r == 0.0f,
          "empty palette safe");

    const ColorF black = ToColorF(0), white = ToColorF(MakeSceneColor(255, 255, 255));
    const ColorF mid { 0.5f, 0.5f, 0.5f, 1.0f };
    ColorF half = white; half.a = 0.5f;       /* white at 50% coverage */

    ColorF r = BlendOver(black, half, BlendMode::Replace);
    CHECK(Near(r.r, 0.5f), "blend replace");
    r = BlendOver(black, half, BlendMode::Add);
    CHECK(Near(r.r, 0.5f), "blend add");
    r = BlendOver(white, half, BlendMode::Add);
    CHECK(Near(r.r, 1.0f), "blend add clamps");
    r = BlendOver(mid, half, BlendMode::Screen);
    CHECK(Near(r.r, 0.75f), "blend screen");   /* 1-(0.5)(0.5) */
    ColorF zero; zero.a = 0.0f;
    r = BlendOver(white, zero, BlendMode::Add);
    CHECK(Near(r.r, 1.0f), "blend zero coverage keeps dst");
}

static void TestWaveSpatial()
{
    using namespace studio;

    /* Linear ramp black->white->black over 1 m along +X. */
    EffectLayer wave;
    wave.primitive = "wave";
    wave.direction = { 1.0f, 0.0f, 0.0f };
    wave.scale     = 1.0f;
    wave.speed     = 0.0f;
    wave.density   = 0.0f;          /* full coverage, pure color sweep */
    wave.palette   = MakePalette({
        { 0.0f, ToColorF(MakeSceneColor(0, 0, 0))     },
        { 0.5f, ToColorF(MakeSceneColor(255, 255, 255)) },
        { 1.0f, ToColorF(MakeSceneColor(0, 0, 0))     },
    });

    SceneDocument doc;
    doc.objects.push_back(OneLedDevice("near", 0.125f));
    doc.objects.push_back(OneLedDevice("mid",  0.25f));
    doc.objects.push_back(OneLedDevice("far",  0.45f));
    doc.objects.push_back(OneLedDevice("wrap", 1.125f));  /* same u as near (exact) */

    EffectEngine engine;
    engine.SetLayers({ wave });
    FrameColors frame;
    engine.Evaluate(doc, 0.0, frame);

    /* gate property: a wave crossing devices lands in spatial order */
    CHECK(RedOf(frame["near"][0]) < RedOf(frame["mid"][0]),  "wave order near<mid");
    CHECK(RedOf(frame["mid"][0])  < RedOf(frame["far"][0]),  "wave order mid<far");
    CHECK(frame["wrap"][0] == frame["near"][0],              "wave periodic wrap");

    /* determinism: same t -> identical frame; later t -> phase moves */
    FrameColors again;
    engine.Evaluate(doc, 0.0, again);
    CHECK(again == frame, "engine deterministic at fixed t");

    EffectLayer moving = wave; moving.speed = 0.1f;
    engine.SetLayers({ moving });
    FrameColors moved;
    engine.Evaluate(doc, 0.0, moved);
    engine.Evaluate(doc, 1.0, frame);
    CHECK(frame["near"][0] != moved["near"][0], "wave advances with t");
}

static void TestPulseCometSpin()
{
    using namespace studio;

    /* pulse: coverage peaks half a spacing ahead of the wavefront */
    EffectLayer pulse;
    pulse.primitive = "pulse";
    pulse.origin    = { 0, 0, 0 };
    pulse.scale     = 0.5f;         /* spacing (m) */
    pulse.speed     = 0.2f;
    pulse.density   = 1.0f;
    pulse.palette   = MakePalette({ MakeSceneColor(255, 255, 255) });
    pulse.opacity   = 1.0f;

    SceneDocument doc;
    /* t=0: wavefront at 0 -> peak coverage at d = 0.25 (u=0.5) */
    doc.objects.push_back(OneLedDevice("front", 0.25f));
    doc.objects.push_back(OneLedDevice("trough", 0.50f));

    EffectEngine engine;
    engine.SetLayers({ pulse });
    FrameColors frame;
    engine.Evaluate(doc, 0.0, frame);
    CHECK(RedOf(frame["front"][0]) > 200 && RedOf(frame["trough"][0]) < 30,
          "pulse ring position");

    /* comet: head bright, tail decays, beyond tail is dark */
    EffectLayer comet;
    comet.primitive = "comet";
    comet.path = { { 0, 0, 0 }, { 1.0f, 0, 0 } };   /* 2 m closed loop  */
    comet.speed   = 0.5f;                           /* m/s              */
    comet.scale   = 0.4f;                           /* tail length (m)  */
    comet.palette = MakePalette({ MakeSceneColor(255, 255, 255),
                                  MakeSceneColor(0, 0, 0) });
    comet.opacity = 1.0f;

    SceneDocument cdoc;
    cdoc.objects.push_back(OneLedDevice("head", 0.5f));    /* t=1s: head at 0.5 */
    cdoc.objects.push_back(OneLedDevice("tail", 0.42f));   /* 0.08 behind head  */
    cdoc.objects.push_back(OneLedDevice("dark", 1.4f));    /* >tail behind      */

    engine.SetLayers({ comet });
    engine.Evaluate(cdoc, 1.0, frame);
    CHECK(RedOf(frame["head"][0]) > 200, "comet head bright");
    CHECK(RedOf(frame["tail"][0]) > 10
          && RedOf(frame["tail"][0]) < RedOf(frame["head"][0]), "comet tail fades");
    CHECK(RedOf(frame["dark"][0]) == 0, "comet beyond tail dark");

    /* spin: N spokes periodic — emitters 2pi/N apart share a color */
    EffectLayer spin;
    spin.primitive = "spin";
    spin.space     = CoordSpace::Local;
    spin.scale     = 2.0f;          /* two spokes */
    spin.speed     = 0.0f;
    spin.palette   = MakePalette({ MakeSceneColor(255, 255, 255),
                                   MakeSceneColor(0, 0, 0) });
    spin.opacity   = 1.0f;

    SceneObject ring;
    ring.id   = "ring"; ring.kind = ObjectKind::Device; ring.verified = true;
    ring.geometry = "fan_body";
    ring.emitters = layout::Ring(8, 0.05f, 0.0f, false, "ring");

    SceneDocument sdoc;
    sdoc.objects.push_back(ring);
    engine.SetLayers({ spin });
    engine.Evaluate(sdoc, 0.0, frame);
    CHECK(frame["ring"].size() == 8, "spin fills ring");
    CHECK(frame["ring"][0] == frame["ring"][4], "spin spoke periodicity");
    CHECK(frame["ring"][0] != frame["ring"][2], "spin spoke contrast");
}

static void TestNoiseAndMasks()
{
    using namespace studio;

    /* noise determinism + range */
    const Vec3 p { 1.25f, -0.5f, 3.75f };
    const float n1 = Noise3(p, 7), n2 = Noise3(p, 7);
    CHECK(n1 == n2, "noise deterministic");
    CHECK(n1 >= 0.0f && n1 <= 1.0f, "noise range");
    bool differs = false;
    for(unsigned int s = 0; s < 8 && !differs; s++)
    {
        differs = !Near(Noise3(p, s), n1);
    }
    CHECK(differs, "noise varies by seed");

    /* mask matching: id, geometry, emitter group */
    EffectLayer L; L.primitive = "static";
    L.palette = MakePalette({ MakeSceneColor(255, 0, 0) });
    L.targets = { "kbd", "fan_body", "ring_group" };

    SceneObject kbd; kbd.id = "kbd";      kbd.geometry = "keyboard_body";
    SceneObject fan; fan.id = "rear_fan"; fan.geometry = "fan_body";
    SceneObject oth; oth.id = "pump";     oth.geometry = "pump_body";
    Emitter e; e.group = "ring_group"; Emitter oe; oe.group = "other";

    CHECK(LayerMatches(L, kbd, oe),  "mask by object id");
    CHECK(LayerMatches(L, fan, oe),  "mask by geometry");
    CHECK(LayerMatches(L, oth, e),   "mask by emitter group");
    CHECK(!LayerMatches(L, oth, oe), "mask rejects unmatched");
    L.targets.clear();
    CHECK(LayerMatches(L, oth, oe),  "empty mask matches all");
}

static void TestEngineMirrorAndJson()
{
    using namespace studio;

    /* Linked copies never get frame entries — they read the owner. */
    SceneDocument doc;
    doc.objects.push_back(OneLedDevice("owner", 0.0f));
    SceneObject copy;
    copy.id = "copy"; copy.kind = ObjectKind::Linked; copy.mirror_of = "owner";
    Emitter ce; ce.local_pos = { 0.5f, 0, 0 }; ce.group = "copy"; ce.address = -1;
    copy.emitters.push_back(ce);
    doc.objects.push_back(copy);
    doc.object_colors["owner"] = MakeSceneColor(9, 9, 9);

    EffectLayer solid; solid.primitive = "static";
    solid.palette = MakePalette({ MakeSceneColor(100, 0, 0) });

    EffectEngine engine;
    engine.SetLayers({ solid });
    FrameColors frame;
    engine.Evaluate(doc, 0.0, frame);
    CHECK(frame.count("owner") == 1 && frame.count("copy") == 0,
          "mirror copies own no frame entries");
    CHECK(frame["owner"][0] == MakeSceneColor(100, 0, 0), "static layer color");

    /* untargeted object keeps painted color via fallback (no entry) */
    solid.targets = { "owner" };
    doc.objects.push_back(OneLedDevice("other", 0.2f));
    doc.object_colors["other"] = MakeSceneColor(1, 2, 3);
    engine.SetLayers({ solid });
    engine.Evaluate(doc, 0.0, frame);
    CHECK(frame.count("other") == 0, "untargeted object has no frame entry");

    /* effect state survives the JSON round trip */
    doc.effect.preset = "aurora"; doc.effect.seed = 4242;
    doc.effect.speed = 1.5f; doc.effect.intensity = 0.6f; doc.effect.playing = true;
    SceneDocument back;
    CHECK(FromJson(ToJson(doc), back), "effect json parses");
    CHECK(back.effect.preset == "aurora" && back.effect.seed == 4242
          && Near(back.effect.speed, 1.5f) && Near(back.effect.intensity, 0.6f)
          && back.effect.playing, "effect state round-trips");
}

static void TestPresetsAndRemix()
{
    using namespace studio;

    CHECK(PresetList().size() == 9, "nine presets registered");
    for(const PresetInfo& p : PresetList())
    {
        const auto layers = BuildPreset(p.id, 1);
        CHECK(!layers.empty(), "preset builds layers");
    }
    CHECK(BuildPreset("nope", 0).empty(), "unknown preset -> empty");

    /* remix reproducibility: same seed = same params, new seed varies */
    const auto a1 = BuildPreset("aurora", 42);
    const auto a2 = BuildPreset("aurora", 42);
    const auto b  = BuildPreset("aurora", 43);
    CHECK(a1.size() == a2.size(), "remix same layer count");
    bool same = true, varies = false;
    for(size_t i = 0; i < a1.size() && i < a2.size(); i++)
    {
        same &= a1[i].speed == a2[i].speed && a1[i].scale == a2[i].scale
             && a1[i].phase == a2[i].phase && a1[i].seed == a2[i].seed;
    }
    for(size_t i = 0; i < a1.size() && i < b.size(); i++)
    {
        varies |= a1[i].speed != b[i].speed || a1[i].scale != b[i].scale
               || a1[i].phase != b[i].phase || a1[i].seed != b[i].seed
               || a1[i].direction.x != b[i].direction.x;
    }
    CHECK(same,   "same seed reproduces layers");
    CHECK(varies, "new seed varies params");

    /* global params scale speed + cap intensity */
    auto layers = BuildPreset("portal", 7);
    ApplyGlobalParams(layers, 2.0f, 0.5f);
    bool ok = true;
    for(const EffectLayer& L : layers)
    {
        ok &= L.opacity <= 0.5f + 1e-6f;
    }
    CHECK(ok, "intensity caps layer opacity");

    /* every preset evaluates without touching hardware */
    SceneDocument doc = BuildDefaultDesk();
    EffectEngine engine;
    FrameColors frame;
    for(const PresetInfo& p : PresetList())
    {
        engine.SetLayers(BuildPreset(p.id, 3));
        engine.Evaluate(doc, 1.25, frame);
        CHECK(!frame.empty(), "preset produces a frame");
    }
}

/*---------------------------------------------------------*\
||| Stage 3 — reactive inputs                                |
\*---------------------------------------------------------*/
static int GreenOf(studio::SceneColor c) { return (int)((c >> 8) & 0xFF); }
static int BlueOf(studio::SceneColor c)  { return (int)((c >> 16) & 0xFF); }
static int MaxCh(studio::SceneColor c)
{
    const int r = RedOf(c), g = GreenOf(c), b = BlueOf(c);
    return r > g ? (r > b ? r : b) : (g > b ? g : b);
}

static void TestInputBus()
{
    using namespace studio;

    double now = 10.0;
    InputBus bus;
    bus.SetNow([&now]() { return now; });

    /* Events stamp on the supplied clock and snapshot fresh. */
    bus.PushEvent("key", 1.0f, 0x51);
    bus.SetAudioLevel(0.6f);
    InputState s = bus.Snapshot(6.0);
    CHECK(s.events.size() == 1, "bus: event snapshot");
    CHECK(s.events[0].source == "key" && s.events[0].code == 0x51
          && !s.events[0].has_pos, "bus: event fields");
    CHECK(Near((float)s.events[0].t, 10.0f), "bus: event stamped on now()");
    CHECK(Near(s.audio_level, 0.6f), "bus: audio level snapshot");

    /* Events older than max_age are dropped. */
    now = 20.0;
    s = bus.Snapshot(6.0);
    CHECK(s.events.empty(), "bus: stale event pruned");
    now = 14.0;
    s = bus.Snapshot(6.0);
    CHECK(s.events.size() == 1, "bus: event inside window kept");

    /* The queue is bounded — a burst can't grow memory, newest win. */
    bus.ClearEvents();
    for(int i = 0; i < 120; i++)
    {
        bus.PushEvent("key", 1.0f, i);
    }
    s = bus.Snapshot(60.0);
    CHECK(s.events.size() == 96, "bus: cap enforced");
    CHECK(s.events.back().code == 119, "bus: newest events kept");

    /* Screen grid and audio level round-trip through the snapshot. */
    bus.SetScreenGrid(2, 1, { ColorF{1,0,0,1}, ColorF{0,0,1,1} });
    s = bus.Snapshot(6.0);
    CHECK(s.screen_cols == 2 && s.screen_rows == 1
          && s.screen_cells.size() == 2
          && Near(s.screen_cells[0].r, 1.0f)
          && Near(s.screen_cells[1].b, 1.0f), "bus: screen grid");

    bus.ClearAll();
    s = bus.Snapshot(6.0);
    CHECK(s.events.empty() && s.screen_cells.empty()
          && Near(s.audio_level, 0.0f), "bus: ClearAll resets");
}

static void TestOnsetDetect()
{
    using namespace studio;

    /* Silence: the output level decays to zero. */
    OnsetDetect quiet;
    for(int i = 0; i < 80; i++)
    {
        quiet.Feed(0.0f, 0.05);
    }
    CHECK(quiet.Level() < 0.05f, "onset: silence decays to ~0");

    /* Sharp spike over a quiet baseline: one onset, then refractory. */
    OnsetDetect det;
    det.SetSensitivity(1.0f);
    for(int i = 0; i < 60; i++)
    {
        det.Feed(0.02f, 0.05);
    }
    const float s1 = det.Feed(0.9f, 0.05);
    const float s2 = det.Feed(0.9f, 0.05);
    CHECK(s1 > 0.5f, "onset: spike detected");
    CHECK(s2 == 0.0f, "onset: refractory blocks repeat");
    CHECK(det.Level() > 0.3f, "onset: level follows spike");

    /* Sustained loud input adapts — the slow envelope catches up and
       onsets stop firing instead of machine-gunning. */
    for(int i = 0; i < 300; i++)
    {
        det.Feed(0.9f, 0.05);
    }
    bool any = false;
    for(int i = 0; i < 40; i++)
    {
        any = (det.Feed(0.9f, 0.05) > 0.0f) || any;
    }
    CHECK(!any, "onset: sustained level adapts");

    /* Sensitivity moves the over-threshold factor: a 3x spike fires
       at full sensitivity but not at minimum. */
    OnsetDetect lo;
    lo.SetSensitivity(0.0f);
    for(int i = 0; i < 60; i++)
    {
        lo.Feed(0.02f, 0.05);
    }
    CHECK(lo.Feed(0.06f, 0.05) == 0.0f, "onset: min sensitivity ignores small spike");
    OnsetDetect hi;
    hi.SetSensitivity(1.0f);
    for(int i = 0; i < 60; i++)
    {
        hi.Feed(0.02f, 0.05);
    }
    CHECK(hi.Feed(0.06f, 0.05) > 0.0f, "onset: max sensitivity catches small spike");
}

static void TestKeyMap()
{
    using namespace studio;

    CHECK(VkForKeyName("Key: Q") == 0x51, "keymap: Key: Q");
    CHECK(VkForKeyName("Key: Escape") == 0x1B, "keymap: Escape");
    CHECK(VkForKeyName("Key: 1") == 0x31, "keymap: digit 1");
    CHECK(VkForKeyName("Key: Space") == 0x20, "keymap: space");
    CHECK(VkForKeyName("Key: F1") == 0x70, "keymap: F1");
    CHECK(VkForKeyName("Key: F12") == 0x7B, "keymap: F12");
    CHECK(VkForKeyName("Key: Number Pad 1") == 0x61, "keymap: numpad 1");
    CHECK(VkForKeyName("Key: Left Windows") == 0x5B, "keymap: LWin");
    CHECK(VkForKeyName("Key: Right Alt") == 0xA5, "keymap: RAlt");
    CHECK(VkForKeyName("Key: Enter (ISO)") == 0x0D, "keymap: ISO enter");
    CHECK(VkForKeyName("Caps Lock Indicator") == -1, "keymap: indicator ignored");
    CHECK(VkForKeyName("Key: Fn") == -1, "keymap: Fn has no VK");
    CHECK(VkForKeyName("Logo") == -1, "keymap: non-key LED ignored");
    CHECK(VkForKeyName("") == -1, "keymap: empty ignored");
    CHECK(VkForKeyName("key: q") == -1, "keymap: exact match required");
}

static void TestRipplePrimitive()
{
    using namespace studio;

    SceneDocument doc;
    doc.objects.push_back(OneLedDevice("near",     0.3f));
    doc.objects.push_back(OneLedDevice("far",      0.6f));
    doc.objects.push_back(OneLedDevice("farthest", 1.0f));

    /* Ring spawned at t=0 at the world origin, speed 1 m/s,
       band half-width 5 cm, decay 1/s. */
    EffectLayer rip;
    rip.primitive = "ripple";
    rip.source    = "key";
    rip.speed     = 1.0f;
    rip.scale     = 0.05f;
    rip.density   = 1.0f;
    rip.opacity   = 1.0f;
    rip.palette   = MakePalette({ MakeSceneColor(0, 255, 0),
                                  MakeSceneColor(255, 0, 0) });

    InputEvent ev;
    ev.source = "key"; ev.code = 0x51; ev.t = 0.0;
    ev.pos = { 0, 0, 0 }; ev.has_pos = true; ev.strength = 1.0f;
    InputState in;
    in.events.push_back(ev);

    EffectEngine engine;
    engine.SetLayers({ rip });
    FrameColors frame;

    /* t=0.3: ring front at 0.3 m — near lit, far/farthest dark. */
    engine.Evaluate(doc, 0.3, frame, &in);
    CHECK(MaxCh(frame["near"][0]) > 40, "ripple: ring front lights emitter");
    CHECK(MaxCh(frame["far"][0]) < 5, "ripple: inside band is dark");
    CHECK(MaxCh(frame["farthest"][0]) < 5, "ripple: ahead of ring is dark");

    /* t=0.6: the ring reaches the 0.6 m emitter. */
    engine.Evaluate(doc, 0.6, frame, &in);
    CHECK(MaxCh(frame["far"][0]) > 40, "ripple: ring propagates outward");

    /* A non-matching source contributes nothing. */
    InputState in2;
    InputEvent ae;
    ae.source = "audio"; ae.t = 0.0; ae.has_pos = true; ae.strength = 1.0f;
    in2.events.push_back(ae);
    engine.Evaluate(doc, 0.3, frame, &in2);
    CHECK(MaxCh(frame["near"][0]) < 5, "ripple: source filter");

    /* Position-less events spawn at the layer origin. */
    EffectLayer rip2 = rip;
    rip2.origin = { 1.0f, 0.0f, 0.0f };
    engine.SetLayers({ rip2 });
    InputState in3;
    InputEvent ev2;
    ev2.source = "key"; ev2.t = 0.05; ev2.has_pos = false; ev2.strength = 1.0f;
    in3.events.push_back(ev2);
    engine.Evaluate(doc, 0.05, frame, &in3);
    CHECK(MaxCh(frame["farthest"][0]) > 40, "ripple: origin fallback");
    CHECK(MaxCh(frame["near"][0]) < 5, "ripple: origin fallback not at event");

    /* Old events decay away (age 5.3 s at decay 1/s -> amp < 0.01). */
    engine.SetLayers({ rip });
    InputState in4;
    in4.events.push_back(ev);
    engine.Evaluate(doc, 5.3, frame, &in4);
    CHECK(MaxCh(frame["near"][0]) < 5, "ripple: old event decayed");

    /* Events from the future (clock reset) are ignored. */
    InputState in5;
    InputEvent fe = ev; fe.t = 10.0;
    in5.events.push_back(fe);
    engine.Evaluate(doc, 0.3, frame, &in5);
    CHECK(MaxCh(frame["near"][0]) < 5, "ripple: future event ignored");

    /* No input at all -> null state -> painted base only (black). */
    engine.Evaluate(doc, 0.3, frame, nullptr);
    CHECK(MaxCh(frame["near"][0]) == 0, "ripple: null input dark");

    /* Determinism: same doc+t+input -> identical frame. */
    FrameColors a, b;
    engine.Evaluate(doc, 0.3, a, &in);
    engine.Evaluate(doc, 0.3, b, &in);
    CHECK(a == b, "ripple: deterministic");
}

static void TestScreenField()
{
    using namespace studio;

    SceneDocument doc;
    doc.objects.push_back(OneLedDevice("left",  -0.31f));
    doc.objects.push_back(OneLedDevice("mid",    0.0f));
    doc.objects.push_back(OneLedDevice("right",  0.31f));

    /* 0.62 m screen centered at the origin; 2x1 red|blue grid. */
    EffectLayer sf;
    sf.primitive = "screenfield";
    sf.origin    = { 0.0f, 0.0f, 0.0f };
    sf.scale     = 0.62f;
    sf.opacity   = 1.0f;

    InputState in;
    in.screen_cols = 2;
    in.screen_rows = 1;
    in.screen_cells = { ColorF{1,0,0,1}, ColorF{0,0,1,1} };

    EffectEngine engine;
    engine.SetLayers({ sf });
    FrameColors frame;
    engine.Evaluate(doc, 0.0, frame, &in);
    CHECK(RedOf(frame["left"][0]) > 200 && BlueOf(frame["left"][0]) < 30,
          "screenfield: left cell red");
    CHECK(BlueOf(frame["right"][0]) > 200 && RedOf(frame["right"][0]) < 30,
          "screenfield: right cell blue");
    CHECK(RedOf(frame["mid"][0]) > 90 && RedOf(frame["mid"][0]) < 165
          && BlueOf(frame["mid"][0]) > 90 && BlueOf(frame["mid"][0]) < 165,
          "screenfield: center blends");

    /* Empty grid -> the layer contributes nothing (base = black). */
    InputState empty;
    engine.Evaluate(doc, 0.0, frame, &empty);
    CHECK(MaxCh(frame["left"][0]) < 5, "screenfield: empty grid dark");

    /* Malformed grid (fewer cells than cols*rows) is rejected. */
    InputState bad;
    bad.screen_cols = 3;
    bad.screen_rows = 1;
    bad.screen_cells = in.screen_cells;
    engine.Evaluate(doc, 0.0, frame, &bad);
    CHECK(MaxCh(frame["left"][0]) < 5, "screenfield: malformed grid rejected");
}

static void TestLevelPrimitive()
{
    using namespace studio;

    SceneDocument doc;
    doc.objects.push_back(OneLedDevice("dev", 0.0f));

    EffectLayer lv;
    lv.primitive = "level";
    lv.opacity   = 1.0f;
    lv.palette   = MakePalette({ MakeSceneColor(255, 255, 255) });

    EffectEngine engine;
    engine.SetLayers({ lv });
    FrameColors frame;

    InputState quiet, loud;
    quiet.audio_level = 0.0f;
    loud.audio_level  = 0.9f;
    engine.Evaluate(doc, 1.0, frame, &quiet);
    CHECK(MaxCh(frame["dev"][0]) < 5, "level: silent = dark");
    engine.Evaluate(doc, 1.0, frame, &loud);
    CHECK(MaxCh(frame["dev"][0]) > 180, "level: loud = lit");
}

static void TestReactivePresets()
{
    using namespace studio;

    /* The three reactive presets exist, declare their input need,
       and build layers; the original six declare none. */
    CHECK(FindPreset("shockwave") != nullptr
          && FindPreset("shockwave")->needs == "audio", "shockwave needs audio");
    CHECK(FindPreset("keyripple") != nullptr
          && FindPreset("keyripple")->needs == "key", "keyripple needs key");
    CHECK(FindPreset("ambient") != nullptr
          && FindPreset("ambient")->needs == "screen", "ambient needs screen");
    for(const char* pid : { "aurora", "reactor", "comet",
                            "chrome", "portal", "embers" })
    {
        CHECK(FindPreset(pid) != nullptr && FindPreset(pid)->needs.empty(),
              "non-reactive preset has no input need");
    }

    /* Under a synthetic input snapshot each reactive preset produces
       output on the default desk. The key event sits on the mouse
       wheel so the expanding ring has emitters to strike (the desk
       keyboard's emitters only exist once its binding resolves). */
    SceneDocument doc = BuildDefaultDesk();

    InputState in;
    in.audio_level = 0.7f;
    InputEvent ke;
    ke.source = "key"; ke.t = 0.9; ke.strength = 1.0f;
    ke.pos = { 0.26f, 0.045f, 0.235f }; ke.has_pos = true;
    in.events.push_back(ke);
    in.screen_cols = 4;
    in.screen_rows = 2;
    in.screen_cells.assign(8, ColorF{ 0.2f, 0.6f, 1.0f, 1.0f });

    for(const char* pid : { "shockwave", "keyripple", "ambient" })
    {
        EffectEngine engine;
        engine.SetLayers(BuildPreset(pid, 1));
        FrameColors frame;
        engine.Evaluate(doc, 0.95, frame, &in);
        CHECK(!frame.empty(), "reactive preset produces a frame");
        bool lit = false;
        for(const auto& kv : frame)
        {
            for(const SceneColor c : kv.second)
            {
                lit = lit || MaxCh(c) > 30;
            }
        }
        CHECK(lit, "reactive preset lights something under input");
    }

    /* keyripple with no events still composites its base layer. */
    EffectEngine engine;
    engine.SetLayers(BuildPreset("keyripple", 1));
    InputState none;
    FrameColors frame;
    engine.Evaluate(doc, 1.0, frame, &none);
    CHECK(!frame.empty(), "keyripple frame without events");

    /* Presets stay deterministic under the same input. */
    engine.SetLayers(BuildPreset("ambient", 1));
    FrameColors a, b;
    engine.Evaluate(doc, 0.95, a, &in);
    engine.Evaluate(doc, 0.95, b, &in);
    CHECK(a == b, "reactive preset deterministic");
}

/*---------------------------------------------------------*\
|| Task 1.1 — one transform graph: hierarchy, quaternions,  |
|| validation, size_m vs scale.                             |
\*---------------------------------------------------------*/
static void TestRotationQuat()
{
    using namespace studio;

    /* Identity + normalization */
    Quat q0 = RotationQuat({ 0.0f, 0.0f, 0.0f });
    CHECK(Near(q0.w, 1.0f) && Near(q0.x, 0.0f) && Near(q0.y, 0.0f)
          && Near(q0.z, 0.0f), "quat: identity");
    Quat q = RotationQuat({ 30.0f, 45.0f, 60.0f });
    CHECK(Near(q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z, 1.0f, 1e-5f),
          "quat: normalized");

    /* Axis spot checks, same convention as TransformPoint (Rz*Ry*Rx) */
    Vec3 v = RotateVec(RotationQuat({ 90, 0, 0 }), { 0, 1, 0 });
    CHECK(Near(v.z, 1.0f) && Near(v.y, 0.0f, 1e-5f), "quat: rotX 90");
    v = RotateVec(RotationQuat({ 0, 90, 0 }), { 1, 0, 0 });
    CHECK(Near(v.z, -1.0f) && Near(v.x, 0.0f, 1e-5f), "quat: rotY 90");
    v = RotateVec(RotationQuat({ 0, 0, 90 }), { 1, 0, 0 });
    CHECK(Near(v.y, 1.0f) && Near(v.x, 0.0f, 1e-5f), "quat: rotZ 90");

    /* Lock the composition order with hand-computed Rz*Ry*Rx results
       for rot = {30, 45, 60}:
       {1,0,0}: Rx leaves it; Ry45 -> {.7071,0,-.7071};
                Rz60 -> {.3536, .6124, -.7071}
       {0,0,1}: Rx30 -> {0,-.5,.866}; Ry45 -> {.6124,-.5,.6124};
                Rz60 -> {.7392, .2803, .6124}                      */
    q = RotationQuat({ 30, 45, 60 });
    v = RotateVec(q, { 1, 0, 0 });
    CHECK(Near(v.x, 0.3536f, 1e-3f) && Near(v.y, 0.6124f, 1e-3f)
          && Near(v.z, -0.7071f, 1e-3f), "quat: mixed-order X");
    v = RotateVec(q, { 0, 0, 1 });
    CHECK(Near(v.x, 0.7392f, 1e-3f) && Near(v.y, 0.2803f, 1e-3f)
          && Near(v.z, 0.6124f, 1e-3f), "quat: mixed-order Z");

    /* The quaternion must equal the matrix path — one conversion
       feeds both the QML node rotation and the core's transforms. */
    Transform t;
    t.rotation_deg = { 37.0f, -24.0f, 81.0f };
    const Vec3 p { 0.31f, -0.17f, 0.42f };
    const Vec3 via_q = RotateVec(RotationQuat(t.rotation_deg), p);
    const Vec3 via_m = TransformPoint(t, p);
    CHECK(Near(via_q.x, via_m.x, 1e-5f) && Near(via_q.y, via_m.y, 1e-5f)
          && Near(via_q.z, via_m.z, 1e-5f), "quat == transform path");

    /* LocalMatrix point application == TransformPoint. */
    const Mat4 lm = LocalMatrix(t);
    const Vec3 via_l = TransformPoint(lm, p);
    CHECK(Near(via_l.x, via_m.x, 1e-5f) && Near(via_l.y, via_m.y, 1e-5f)
          && Near(via_l.z, via_m.z, 1e-5f), "local matrix == transform");
}

static studio::SceneObject GroupObj(const std::string& id, const studio::Vec3& pos)
{
    studio::SceneObject o;
    o.id = id;
    o.kind = studio::ObjectKind::Group;
    o.transform.position = pos;
    return o;
}

static void TestWorldGraph()
{
    using namespace studio;

    /* Translated group: child origin = parent pos + child local. */
    SceneDocument doc;
    doc.objects.push_back(GroupObj("grp", { 1.0f, 2.0f, 3.0f }));
    SceneObject dev = OneLedDevice("dev", 0.1f);
    dev.parent_id = "grp";
    doc.objects.push_back(dev);

    CHECK(ValidateSceneGraph(doc, nullptr), "graph: translated group valid");
    {
        const auto world = ResolveWorldMatrices(doc);
        const Vec3 w = TransformPoint(world.at("dev"), { 0, 0, 0 });
        CHECK(Near(w.x, 1.1f) && Near(w.y, 2.0f) && Near(w.z, 3.0f),
              "graph: child inherits parent translation");
    }

    /* Scaled parent: the child's offset AND its emitters scale. */
    FindObject(doc, "grp")->transform.scale = { 2, 2, 2 };
    {
        const auto world = ResolveWorldMatrices(doc);
        const Vec3 w = TransformPoint(world.at("dev"), { 0, 0, 0 });
        CHECK(Near(w.x, 1.0f + 2.0f * 0.1f) && Near(w.y, 2.0f),
              "graph: parent scale scales child offset");
        const Vec3 e = TransformPoint(world.at("dev"), { 0.05f, 0, 0 });
        CHECK(Near(e.x, 1.0f + 2.0f * 0.15f), "graph: parent scale reaches emitter");
    }

    /* Rotated parent swings the child; child rotation composes on top.
       grp pos {0.5,0.2,0} rot {0,90,0}; dev local {0.1,0,0} rot {90,0,0};
       emitter {0,1,0}:  Rx90 -> {0,0,1}; +devT -> {0.1,0,1};
       Ry90 -> {1,0,-0.1}; +grpT -> {1.5,0.2,-0.1}.               */
    {
        SceneDocument rdoc;
        SceneObject g = GroupObj("grp", { 0.5f, 0.2f, 0.0f });
        g.transform.rotation_deg = { 0, 90, 0 };
        rdoc.objects.push_back(g);
        SceneObject d = OneLedDevice("dev", 0.1f);
        d.parent_id = "grp";
        d.transform.rotation_deg = { 90, 0, 0 };
        d.emitters[0].local_pos = { 0, 1, 0 };
        rdoc.objects.push_back(d);

        const auto world = ResolveWorldMatrices(rdoc);
        const Vec3 w = TransformPoint(world.at("dev"), { 0, 1, 0 });
        CHECK(Near(w.x, 1.5f) && Near(w.y, 0.2f) && Near(w.z, -0.1f),
              "graph: composed parent+child rotation");
    }

    /* Deep nesting: g1{x=1} -> g2{y=1} -> dev{z=1}. */
    {
        SceneDocument deep;
        SceneObject g1 = GroupObj("g1", { 1, 0, 0 });
        SceneObject g2 = GroupObj("g2", { 0, 1, 0 });
        g2.parent_id = "g1";
        SceneObject d = OneLedDevice("dev", 0.0f);
        d.parent_id = "g2";
        d.transform.position = { 0, 0, 1 };
        deep.objects = { g1, g2, d };
        const auto world = ResolveWorldMatrices(deep);
        const Vec3 w = TransformPoint(world.at("dev"), { 0, 0, 0 });
        CHECK(Near(w.x, 1.0f) && Near(w.y, 1.0f) && Near(w.z, 1.0f),
              "graph: nested groups compose");
    }

    /* Object order must not matter: child listed before parent. */
    {
        SceneDocument rev;
        SceneObject d = OneLedDevice("dev", 0.1f);
        d.parent_id = "grp";
        rev.objects.push_back(d);
        rev.objects.push_back(GroupObj("grp", { 1.0f, 0, 0 }));
        const auto world = ResolveWorldMatrices(rev);
        const Vec3 w = TransformPoint(world.at("dev"), { 0, 0, 0 });
        CHECK(Near(w.x, 1.1f), "graph: order-independent resolution");

        /* And TopologicalOrder still emits the parent first — the
           bridge model needs that for the QML node map. */
        const auto ord = TopologicalOrder(rev);
        CHECK(ord.size() == 2 && ord[0]->id == "grp" && ord[1]->id == "dev",
              "graph: topological order parents-first");
    }
}

static void TestGraphValidation()
{
    using namespace studio;

    std::vector<std::string> errs;

    /* Missing parent is rejected. */
    {
        SceneDocument doc;
        SceneObject d = OneLedDevice("dev", 0.0f);
        d.parent_id = "ghost";
        doc.objects.push_back(d);
        CHECK(!ValidateSceneGraph(doc, &errs) && !errs.empty(),
              "graph: dangling parent rejected");
    }

    /* Self-parent and a 2-cycle are rejected. */
    {
        SceneDocument doc;
        SceneObject a = GroupObj("a", { 0, 0, 0 });
        a.parent_id = "a";
        doc.objects.push_back(a);
        CHECK(!ValidateSceneGraph(doc, nullptr), "graph: self parent rejected");
    }
    {
        SceneDocument doc;
        SceneObject a = GroupObj("a", { 0, 0, 0 });
        SceneObject b = GroupObj("b", { 0, 0, 0 });
        a.parent_id = "b";
        b.parent_id = "a";
        doc.objects.push_back(a);
        doc.objects.push_back(b);
        CHECK(!ValidateSceneGraph(doc, nullptr), "graph: parent cycle rejected");
    }

    /* Resolution stays safe even on an invalid doc (cycle treated
       as a root — validation is what keeps documents clean). */
    {
        SceneDocument doc;
        SceneObject a = GroupObj("a", { 1, 0, 0 });
        SceneObject b = GroupObj("b", { 0, 1, 0 });
        a.parent_id = "b";
        b.parent_id = "a";
        doc.objects = { a, b };
        const auto world = ResolveWorldMatrices(doc);
        CHECK(world.count("a") == 1 && world.count("b") == 1,
              "graph: cycle resolution terminates");
    }

    /* Mirrored output ownership: dangling mirror, chain, cycle,
       missing mirror on a Linked object, mirror on a Decor. */
    {
        SceneDocument doc;
        SceneObject c;
        c.id = "c"; c.kind = ObjectKind::Linked; c.mirror_of = "ghost";
        doc.objects.push_back(c);
        CHECK(!ValidateSceneGraph(doc, nullptr), "graph: dangling mirror rejected");
    }
    {
        SceneDocument doc;
        SceneObject c;
        c.id = "c"; c.kind = ObjectKind::Linked;   /* no mirror_of */
        doc.objects.push_back(c);
        CHECK(!ValidateSceneGraph(doc, nullptr), "graph: linked w/o mirror rejected");
    }
    {
        SceneDocument doc;
        doc.objects.push_back(OneLedDevice("owner", 0.0f));
        SceneObject c1;
        c1.id = "c1"; c1.kind = ObjectKind::Linked; c1.mirror_of = "owner";
        SceneObject c2;
        c2.id = "c2"; c2.kind = ObjectKind::Linked; c2.mirror_of = "c1";
        doc.objects.push_back(c1);
        doc.objects.push_back(c2);
        CHECK(!ValidateSceneGraph(doc, nullptr), "graph: mirror chain rejected");
    }
    {
        SceneDocument doc;
        SceneObject c;
        c.id = "c"; c.kind = ObjectKind::Linked; c.mirror_of = "c";  /* self */
        doc.objects.push_back(c);
        CHECK(!ValidateSceneGraph(doc, nullptr), "graph: self mirror rejected");
    }
    {
        SceneDocument doc;
        doc.objects.push_back(OneLedDevice("owner", 0.0f));
        SceneObject d;
        d.id = "d"; d.kind = ObjectKind::Decor; d.mirror_of = "owner";
        doc.objects.push_back(d);
        CHECK(!ValidateSceneGraph(doc, nullptr), "graph: mirror on non-linked rejected");
    }

    /* Dimensionless scale must be positive and finite. */
    {
        SceneDocument doc;
        SceneObject d = OneLedDevice("dev", 0.0f);
        d.transform.scale = { 0, 1, 1 };
        doc.objects.push_back(d);
        CHECK(!ValidateSceneGraph(doc, nullptr), "graph: zero scale rejected");
    }
    {
        SceneDocument doc;
        SceneObject d = OneLedDevice("dev", 0.0f);
        d.transform.scale = { -1, 1, 1 };
        doc.objects.push_back(d);
        CHECK(!ValidateSceneGraph(doc, nullptr), "graph: negative scale rejected");
    }
    {
        SceneDocument doc;
        SceneObject d = OneLedDevice("dev", 0.0f);
        d.transform.position = { std::nanf(""), 0, 0 };
        doc.objects.push_back(d);
        CHECK(!ValidateSceneGraph(doc, nullptr), "graph: non-finite position rejected");
    }

    /* Duplicate ids rejected. */
    {
        SceneDocument doc;
        doc.objects.push_back(OneLedDevice("dev", 0.0f));
        doc.objects.push_back(OneLedDevice("dev", 1.0f));
        CHECK(!ValidateSceneGraph(doc, nullptr), "graph: duplicate id rejected");
    }

    /* The default desk itself validates clean. */
    {
        SceneDocument doc = BuildDefaultDesk();
        errs.clear();
        CHECK(ValidateSceneGraph(doc, &errs) && errs.empty(),
              "graph: default desk valid");
    }
}

static void TestJsonHierarchy()
{
    using namespace studio;

    /* parent_id / size_m / kind "group" round-trip. */
    SceneDocument doc;
    doc.objects.push_back(GroupObj("grp", { 1, 2, 3 }));
    SceneObject d = OneLedDevice("dev", 0.1f);
    d.parent_id = "grp";
    d.size_m = { 0.3f, 0.05f, 0.13f };
    doc.objects.push_back(d);

    SceneDocument back;
    CHECK(FromJson(ToJson(doc), back), "json: hierarchy parses");
    const SceneObject* g = FindObject(back, "grp");
    const SceneObject* c = FindObject(back, "dev");
    CHECK(g != nullptr && g->kind == ObjectKind::Group, "json: group kind");
    CHECK(c != nullptr && c->parent_id == "grp", "json: parent_id round-trip");
    CHECK(c != nullptr && Near(c->size_m.x, 0.3f) && Near(c->size_m.z, 0.13f),
          "json: size_m round-trip");

    /* v1 migration: a version-1 file stored body dims in `scale`.
       Decor gets them as size_m; devices reset to identity. */
    {
        nlohmann::json j;
        j["version"] = 1;
        j["objects"] = nlohmann::json::array();
        j["objects"].push_back({
            {"id", "shelf"}, {"kind", "decor"}, {"geometry", "desk"},
            {"scale", { {"x", 1.4}, {"y", 0.04}, {"z", 0.75} }},
        });
        j["objects"].push_back({
            {"id", "dev"}, {"kind", "device"}, {"binding", "b"},
            {"scale", { {"x", 2.0}, {"y", 2.0}, {"z", 2.0} }},
        });
        SceneDocument m;
        CHECK(FromJson(j, m), "json: v1 migrates");
        const SceneObject* shelf = FindObject(m, "shelf");
        const SceneObject* dev   = FindObject(m, "dev");
        CHECK(shelf && Near(shelf->size_m.x, 1.4f) && Near(shelf->size_m.z, 0.75f)
              && Near(shelf->transform.scale.x, 1.0f), "json: v1 decor scale -> size_m");
        CHECK(dev && Near(dev->transform.scale.x, 1.0f)
              && Near(dev->size_m.x, 0.0f), "json: v1 device scale reset");
        CHECK(m.version == 2, "json: migrated doc is current version");
    }

    /* Invalid graphs are rejected on load, leaving the doc untouched. */
    {
        SceneDocument keep;
        keep.objects.push_back(GroupObj("keep", { 0, 0, 0 }));
        nlohmann::json j;
        j["version"] = 2;
        j["objects"] = nlohmann::json::array();
        j["objects"].push_back({ {"id", "a"}, {"kind", "group"},
                                 {"parent_id", "b"} });
        j["objects"].push_back({ {"id", "b"}, {"kind", "group"},
                                 {"parent_id", "a"} });
        CHECK(!FromJson(j, keep), "json: parent cycle rejected");
        CHECK(keep.objects.size() == 1 && keep.objects[0].id == "keep",
              "json: rejected load leaves doc untouched");

        j["objects"][0]["parent_id"] = "missing";
        j["objects"][1]["parent_id"] = "";
        CHECK(!FromJson(j, keep), "json: dangling parent rejected");
    }
}

static void TestDefaultDeskGroups()
{
    using namespace studio;
    SceneDocument doc = BuildDefaultDesk();
    const auto world = ResolveWorldMatrices(doc);

    /* Mouse parts became children of a "mouse" group; their world
       placement is unchanged from the flat scene. */
    const SceneObject* mouse = FindObject(doc, "mouse");
    CHECK(mouse != nullptr && mouse->kind == ObjectKind::Group
          && mouse->parent_id.empty(), "desk: mouse group exists");
    for(const char* id : { "mouse_body", "mouse_wheel", "mouse_logo",
                           "mouse_strip" })
    {
        const SceneObject* o = FindObject(doc, id);
        CHECK(o != nullptr && o->parent_id == "mouse", "desk: mouse child");
    }
    {
        const Vec3 w = TransformPoint(world.at("mouse_wheel"), { 0, 0, 0 });
        CHECK(Near(w.x, 0.26f) && Near(w.y, 0.045f) && Near(w.z, 0.235f),
              "desk: mouse wheel world preserved");
        /* underglow led0: ring r=0.028 start 90 -> local {0,0,-0.028},
           old world {0.26,0.012,0.275} + offset -> {0.26,0.012,0.247} */
        const Vec3 s = TransformPoint(world.at("mouse_strip"),
                                      { 0, 0, -0.028f });
        CHECK(Near(s.x, 0.26f) && Near(s.y, 0.012f) && Near(s.z, 0.247f),
              "desk: mouse strip world preserved");
    }

    /* Case + components under a "case" group, worlds preserved. */
    const SceneObject* pc = FindObject(doc, "case");
    CHECK(pc != nullptr && pc->kind == ObjectKind::Group
          && pc->parent_id.empty(), "desk: case group exists");
    for(const char* id : { "case_shell", "case_fans", "case_fan_b1",
                           "case_fan_b2", "case_fan_rear", "rad_fans",
                           "rad_fan_m1", "rad_fan_m2", "pump",
                           "gpu_top_fan", "slw_fan_0", "slw_fan_1",
                           "slw_fan_2", "dimm_0", "dimm_1", "gpu_body",
                           "gpu_logo", "gpu_fan_r", "gpu_fan_m",
                           "gpu_fan_l" })
    {
        const SceneObject* o = FindObject(doc, id);
        CHECK(o != nullptr && o->parent_id == "case", "desk: case child");
    }
    {
        /* rear exhaust: world pos {0.47,0.30,-0.30} rot {90,0,0};
           emitter0 local {0.052,0.016,0} -> Rx90 -> {0.052,0,0.016} */
        const Vec3 w = TransformPoint(world.at("case_fan_rear"),
                                      { 0.052f, 0.016f, 0.0f });
        CHECK(Near(w.x, 0.522f) && Near(w.y, 0.30f) && Near(w.z, -0.284f),
              "desk: rear fan world preserved");
        /* gpu_logo on the card's glass-facing (-x) edge: node local
           {-0.04,-0.035,-0.02} under the case anchor -> {0.43,0.20,-0.12};
           ry=90 turns strip led0 local {-0.03,0,0} into +z 0.03 ->
           {0.43,0.20,-0.09} */
        const Vec3 g = TransformPoint(world.at("gpu_logo"), { -0.03f, 0, 0 });
        CHECK(Near(g.x, 0.43f) && Near(g.y, 0.20f) && Near(g.z, -0.09f),
              "desk: gpu logo world preserved");
    }

    /* Mirrored copies still resolve their owner for output. */
    CHECK(OutputOwner(doc, "case_fan_b1")->id == "case_fans",
          "desk: mirrored child still shares output");

    /* Decor bodies carry dimensions in size_m; transform.scale is
       dimensionless identity. */
    const SceneObject* desk = FindObject(doc, "desk");
    CHECK(desk != nullptr && Near(desk->size_m.x, 1.4f)
          && Near(desk->size_m.z, 0.75f)
          && Near(desk->transform.scale.x, 1.0f), "desk: size_m split");
}

static void TestEngineWorldGraph()
{
    using namespace studio;

    /* World-space effects must see resolved positions, not local ones:
       a device child under a rotated+translated group gets the same
       wave color as a root device at the same world X. */
    EffectLayer wave;
    wave.primitive = "wave";
    wave.direction = { 1, 0, 0 };
    wave.scale     = 1.0f;
    wave.speed     = 0.0f;
    wave.density   = 0.0f;
    wave.palette   = MakePalette({
        { 0.0f, ToColorF(MakeSceneColor(0, 0, 0))       },
        { 0.5f, ToColorF(MakeSceneColor(255, 255, 255)) },
        { 1.0f, ToColorF(MakeSceneColor(0, 0, 0))       },
    });

    SceneDocument doc;
    SceneObject g = GroupObj("grp", { 0.3f, 0, 0 });
    g.transform.rotation_deg = { 0, 90, 0 };
    doc.objects.push_back(g);
    SceneObject child = OneLedDevice("child", 0.0f);
    child.parent_id = "grp";
    child.transform.position = { -0.2f, 0, 0.05f };
    /* world = 0.3 + Ry90*(-0.2,0,0.05) = {0.35, 0, 0.2} */
    doc.objects.push_back(child);
    doc.objects.push_back(OneLedDevice("rootref", 0.35f));  /* world x .35 */

    EffectEngine engine;
    engine.SetLayers({ wave });
    FrameColors frame;
    engine.Evaluate(doc, 0.0, frame);
    CHECK(frame.count("child") == 1 && frame.count("rootref") == 1,
          "engine: grouped device evaluated");
    CHECK(frame["child"][0] == frame["rootref"][0],
          "engine: resolved world position drives wave");

    /* A key ripple originates where the key's emitter actually is —
       move the group and the resolved position moves with it. */
    SceneDocument kdoc;
    SceneObject kg = GroupObj("kgrp", { 0.5f, 0, 0 });
    kdoc.objects.push_back(kg);
    SceneObject kbd = OneLedDevice("kbd", 0.0f);
    kbd.parent_id = "kgrp";
    kdoc.objects.push_back(kbd);
    kdoc.objects.push_back(OneLedDevice("spot", 0.8f));

    EffectLayer rip;
    rip.primitive = "ripple";
    rip.source    = "key";
    rip.speed     = 1.0f;
    rip.scale     = 0.05f;
    rip.density   = 1.0f;
    rip.opacity   = 1.0f;
    rip.palette   = MakePalette({ MakeSceneColor(255, 255, 255) });
    engine.SetLayers({ rip });

    auto world = ResolveWorldMatrices(kdoc);
    Vec3 keypos = TransformPoint(world.at("kbd"), { 0, 0, 0 });
    CHECK(Near(keypos.x, 0.5f), "key lookup: initial world");

    InputState in;
    InputEvent ev;
    ev.source = "key"; ev.code = 0x51; ev.t = 0.0;
    ev.strength = 1.0f; ev.has_pos = true;
    ev.pos = keypos;
    in.events.push_back(ev);
    engine.Evaluate(kdoc, 0.0, frame, &in);
    CHECK(MaxCh(frame["kbd"][0]) > 40 && MaxCh(frame["spot"][0]) < 5,
          "key lookup: ripple at key emitter");

    /* Move the group -> resolved key position follows -> a ripple
       spawned there lights the moved key and whatever sits at the
       new spot, not the stale position. */
    FindObject(kdoc, "kgrp")->transform.position.x = 0.8f;
    world = ResolveWorldMatrices(kdoc);
    keypos = TransformPoint(world.at("kbd"), { 0, 0, 0 });
    CHECK(Near(keypos.x, 0.8f), "key lookup: world follows group move");

    in.events[0].pos = keypos;
    engine.Evaluate(kdoc, 0.0, frame, &in);
    CHECK(MaxCh(frame["kbd"][0]) > 40 && MaxCh(frame["spot"][0]) > 40,
          "key lookup: ripple originates at moved keys");
}

/*---------------------------------------------------------*\
||| effect.seed is a uint32 spanning the full range —       ||
||| remix() produces seeds >= 2^31 roughly half the time,   ||
||| and a saved document must still load.                   ||
\*---------------------------------------------------------*/
static void TestEffectSeedBounds()
{
    using namespace studio;

    SceneDocument doc = BuildDefaultDesk();
    doc.effect.preset = "comet";
    doc.effect.seed   = 0xFFFFFFFFu;

    std::vector<std::string> errors;
    SceneDocument back;
    CHECK(FromJson(ToJson(doc), back, &errors)
          && back.effect.seed == 0xFFFFFFFFu,
          "seed: 0xFFFFFFFF round-trips");

    nlohmann::json j = ToJson(doc);
    j["effect"]["seed"] = 3000000000u;   /* > 2^31, < 2^32 */
    errors.clear();
    CHECK(FromJson(j, back, &errors) && back.effect.seed == 3000000000u,
          "seed: > 2^31 loads");

    j["effect"]["seed"] = 4294967296ull; /* > uint32 range */
    errors.clear();
    CHECK(!FromJson(j, back, &errors) && errors.size() > 0,
          "seed: > UINT32_MAX rejected");

    j["effect"]["seed"] = -1;
    errors.clear();
    CHECK(!FromJson(j, back, &errors), "seed: negative rejected");

    /* Signed-integer path: a positive value stored as number_integer
       (json(long long), binary-format deserializers) must still hit
       the upper bound — the old check only rejected s < 0 and let
       (unsigned int)s wrap 5000000000 into a silently-wrong seed. */
    j["effect"]["seed"] = 5000000000ll; /* signed, > UINT32_MAX */
    errors.clear();
    CHECK(!FromJson(j, back, &errors) && errors.size() > 0,
          "seed: signed > UINT32_MAX rejected");

    /* Same bound via the text parser — a plain 5000000000 literal
       on disk must be rejected whichever integer type it lands on. */
    j["effect"]["seed"] = nlohmann::json::parse("5000000000");
    errors.clear();
    CHECK(!FromJson(j, back, &errors) && errors.size() > 0,
          "seed: text 5000000000 rejected");

    j["effect"]["seed"] = 1.5;
    errors.clear();
    CHECK(!FromJson(j, back, &errors), "seed: non-integer rejected");
}

/*---------------------------------------------------------*\
||| Body size contract: a size_m component of 0 falls     ||
||| back to the geometry's canonical axis size — schema,  ||
||| validator, and renderer agree (ResolvedBodySize).     ||
\*---------------------------------------------------------*/
static void TestBodySizeContract()
{
    using namespace studio;

    SceneObject desk;
    desk.geometry = "desk";
    desk.size_m   = { 0.0f, 0.0f, 0.0f };
    const Vec3 full = ResolvedBodySize(desk);
    CHECK(full.x > 0.0f && full.y > 0.0f && full.z > 0.0f,
          "body: all-zero size_m falls back to canonical (decor)");

    /* partial size: authored axes kept, zero axes canonical */
    desk.size_m = { 0.9f, 0.0f, 0.0f };
    const Vec3 part = ResolvedBodySize(desk);
    CHECK(Near(part.x, 0.9f)
          && Near(part.y, CanonicalBodySize("desk").y)
          && Near(part.z, CanonicalBodySize("desk").z),
          "body: partial size_m fills canonical axes");

    desk.size_m = { 1.0f, 2.0f, 3.0f };
    const Vec3 authored = ResolvedBodySize(desk);
    CHECK(Near(authored.x, 1.0f) && Near(authored.y, 2.0f)
          && Near(authored.z, 3.0f),
          "body: authored size_m wins");

    /* every geometry the renderer knows has a non-zero canonical */
    const char* known[] = { "desk", "case_shell", "monitor", "mouse_body",
                            "gpu_body", "keyboard_body", "fan_body",
                            "ram_body", "pump_body" };
    bool all = true;
    for(const char* g : known)
    {
        const Vec3 c = CanonicalBodySize(g);
        if(c.x <= 0.0f || c.y <= 0.0f || c.z <= 0.0f)
        {
            all = false;
        }
    }
    CHECK(all, "body: all known geometries have canonical sizes");
    CHECK(CanonicalBodySize("no_such_geo").x == 0.0f,
          "body: unknown geometry has no canonical");

    /* the validator agrees: {0,0,0} is schema-valid (canonical is a
       render fallback, not a validation error) */
    SceneDocument doc;
    SceneObject d2;
    d2.id = "d2"; d2.kind = ObjectKind::Decor; d2.geometry = "desk";
    doc.objects.push_back(d2);
    std::vector<std::string> errs;
    CHECK(ValidateSceneGraph(doc, &errs) && errs.empty(),
          "body: zero size_m is valid");
}

int main()
{
    TestTransform();
    TestRotationQuat();
    TestWorldGraph();
    TestGraphValidation();
    TestJsonHierarchy();
    TestEffectSeedBounds();
    TestBodySizeContract();
    TestDefaultDeskGroups();
    TestEngineWorldGraph();
    TestRing();
    TestStripAndMatrix();
    TestMirrorSemantics();
    TestJsonRoundTrip();
    TestResolver();
    TestDefaultDesk();
    TestPaletteAndBlend();
    TestWaveSpatial();
    TestPulseCometSpin();
    TestNoiseAndMasks();
    TestEngineMirrorAndJson();
    TestPresetsAndRemix();
    TestInputBus();
    TestOnsetDetect();
    TestKeyMap();
    TestRipplePrimitive();
    TestScreenField();
    TestLevelPrimitive();
    TestReactivePresets();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
