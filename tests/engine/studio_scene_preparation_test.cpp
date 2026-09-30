// SPDX-License-Identifier: GPL-2.0-or-later
//
// Studio-import Task 6 — engine-side prepared scene
// (src/engine/StudioScenePreparation.{h,cpp}): hardware-
// dependent matrix geometry against matched identities, the
// runtime layer copy (global params + ripple decay, authored
// data untouched), and the VK->world key-position lookup the
// worker and preview share.

#include "StudioScenePreparation.h"

#include "effects/EffectJson.h"
#include "inputs/KeyMap.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace openrgb2;
using namespace studio;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

static bool Near(float a, float b)
{
    return std::fabs(a - b) < 1e-5f;
}
static bool NearV(const Vec3& a, const Vec3& b)
{
    return Near(a.x, b.x) && Near(a.y, b.y) && Near(a.z, b.z);
}

/*---------------------------------------------------------*\
||| Fixtures                                                ||
\*---------------------------------------------------------*/

/* One keyboard object bound to a 2x3-matrix zone (one empty
   cell), source-adapter parity: 19 mm pitch, 16 mm key-top,
   centered on the object origin. */
static WorkspaceCandidate KeyboardCandidate()
{
    WorkspaceCandidate cand;

    DeviceBinding b;
    b.id              = "kb";
    b.controller_name = "Corsair K95";
    b.vendor          = "Corsair";
    b.serial          = "K1";
    b.location        = "hid:k1";
    b.zone_name       = "Keyboard";
    b.zone_leds       = 6;

    SceneObject kb;
    kb.id      = "kb0";
    kb.label   = "Keyboard";
    kb.kind    = ObjectKind::Device;
    kb.binding = "kb";
    kb.layout  = "matrix_map";

    auto doc = std::make_shared<SceneDocument>();
    doc->bindings.push_back(b);
    doc->objects.push_back(kb);
    cand.resolved = doc;
    return cand;
}

static DeviceSnapshot KeyboardSnapshot()
{
    DeviceSnapshot snap;
    snap.name        = "Corsair K95";
    snap.vendor      = "Corsair";
    snap.key.device  = "Corsair:Corsair K95";
    snap.key.serial  = "K1";
    snap.key.location = "hid:k1";
    snap.device_type = -1;
    snap.connection  = ConnectionState::Connected;

    ZoneSnapshot z;
    z.name        = "Keyboard";
    z.led_count   = 6;
    z.leds_min    = 6;
    z.leds_max    = 6;
    z.matrix_rows = 2;
    z.matrix_cols = 3;
    z.matrix_map  = { 0, 1, 2,
                      3, 0xFFFFFFFFu, 5 };   /* one empty cell */
    snap.zones = { z };
    snap.led_names = { "Key: Escape", "Key: A", "Key: S",
                       "Key: D",      "Key: F", "Key: G" };
    return snap;
}

/*---------------------------------------------------------*\
||| Matrix expansion                                        ||
\*---------------------------------------------------------*/

static void TestMatrixExpansion()
{
    WorkspaceCandidate cand = KeyboardCandidate();
    const std::vector<DeviceSnapshot> snaps = { KeyboardSnapshot() };

    PreparedStudioScene p;
    std::string err;
    CHECK(PrepareStudioScene(cand, snaps, p, err));
    CHECK(p.doc != nullptr);
    /* Empty cell skipped; the five populated cells emit at the
       cell's LED address. */
    const SceneObject& kb = p.doc->objects[0];
    CHECK(kb.emitters.size() == 5);
    CHECK(kb.emitters[0].address == 0);
    CHECK(kb.emitters[3].address == 3);
    CHECK(kb.emitters[4].address == 5);
    /* Geometry: centered grid, +X across, -Z down. */
    CHECK(NearV(kb.emitters[0].local_pos,
                Vec3{ -0.019f, 0.016f, 0.0095f }));
    CHECK(NearV(kb.emitters[2].local_pos,
                Vec3{ 0.019f, 0.016f, 0.0095f }));
    CHECK(NearV(kb.emitters[3].local_pos,
                Vec3{ -0.019f, 0.016f, -0.0095f }));
    CHECK(kb.emitters[0].group == "kb0");
    /* Authored document is NEVER mutated. */
    CHECK(cand.resolved->objects[0].emitters.empty());
}

static void TestMatrixOutOfRange()
{
    WorkspaceCandidate cand = KeyboardCandidate();
    DeviceSnapshot snap = KeyboardSnapshot();
    snap.zones[0].matrix_map = { 0, 1, 2, 3, 4, 99 };
    PreparedStudioScene p;
    std::string err;
    CHECK(!PrepareStudioScene(cand, { snap }, p, err));
    CHECK(!err.empty());
}

static void TestMissingKeyboard()
{
    WorkspaceCandidate cand = KeyboardCandidate();
    /* No matching snapshot at all: prepare succeeds, the
       object keeps its authored (empty) emitters, no key
       positions exist. */
    PreparedStudioScene p;
    std::string err;
    CHECK(PrepareStudioScene(cand, {}, p, err));
    CHECK(p.doc->objects[0].emitters.empty());
    CHECK(p.key_positions.empty());

    /* A different controller must not steal the binding —
       identity resolution, never index. */
    DeviceSnapshot other;
    other.name          = "Other Board";
    other.vendor        = "Acme";
    other.key.device    = "Acme:Other Board";
    other.key.serial    = "ZZ";
    other.key.location  = "hid:z";
    ZoneSnapshot z;
    z.name        = "Keyboard";
    z.led_count   = 6;
    z.matrix_rows = 1;
    z.matrix_cols = 6;
    z.matrix_map  = { 0, 1, 2, 3, 4, 5 };
    other.zones   = { z };
    CHECK(PrepareStudioScene(cand, { other }, p, err));
    CHECK(p.doc->objects[0].emitters.empty());
}

static void TestTransformedKeyboard()
{
    WorkspaceCandidate cand = KeyboardCandidate();
    /* Move + yaw the keyboard, then parent it under a moved
       group — BOTH frames must ripple into key_positions. */
    {
        auto mdoc = std::make_shared<SceneDocument>(*cand.resolved);
        mdoc->objects[0].transform.position    = { 0.5f, 0.0f, 0.2f };
        mdoc->objects[0].transform.rotation_deg = { 0.0f, 90.0f, 0.0f };
        cand.resolved = mdoc;
    }

    PreparedStudioScene p;
    std::string err;
    CHECK(PrepareStudioScene(cand, { KeyboardSnapshot() }, p, err));
    const int vk_a = VkForKeyName("Key: A");
    CHECK(vk_a >= 0);
    auto it = p.key_positions.find(vk_a);
    CHECK(it != p.key_positions.end());
    /* Local {0,0.016,0.0095} (led 0 sits at row 0 col 0...
       "Key: A" is led 1 -> col 1 -> x = 0) rotated +90 deg
       about Y then translated: +X -> -Z in world. */
    CHECK(!NearV(it->second, Vec3{ 0.0f, 0.016f, 0.0095f }));

    /* A parent group's transform composes through
       ResolveWorldMatrices. */
    SceneObject group;
    group.id   = "g0";
    group.kind = ObjectKind::Group;
    group.transform.position = { 1.0f, 0.0f, 0.0f };
    auto doc2 = std::make_shared<SceneDocument>(*cand.resolved);
    doc2->objects.insert(doc2->objects.begin(), group);
    doc2->objects[1].parent_id = "g0";
    cand.resolved = doc2;
    PreparedStudioScene p2;
    CHECK(PrepareStudioScene(cand, { KeyboardSnapshot() }, p2, err));
    auto it2 = p2.key_positions.find(vk_a);
    CHECK(it2 != p2.key_positions.end());
    CHECK(Near(it2->second.x - it->second.x, 1.0f));
    CHECK(Near(it2->second.y, it->second.y));
    CHECK(Near(it2->second.z, it->second.z));
}

static void TestDuplicateKeyNamesFirstWins()
{
    WorkspaceCandidate cand = KeyboardCandidate();
    DeviceSnapshot snap = KeyboardSnapshot();
    /* LEDs 1 and 3 both report "Key: A" — the FIRST emitter
       (lowest address) wins the position. */
    snap.led_names[3] = "Key: A";
    PreparedStudioScene p;
    std::string err;
    CHECK(PrepareStudioScene(cand, { snap }, p, err));
    const int vk_a = VkForKeyName("Key: A");
    const auto it = p.key_positions.find(vk_a);
    CHECK(it != p.key_positions.end());
    /* led 1 = row 0 col 1 -> origin x. */
    CHECK(NearV(it->second, Vec3{ 0.0f, 0.016f, 0.0095f }));
}

static void TestTopologyRebuild()
{
    WorkspaceCandidate cand = KeyboardCandidate();
    std::string err;

    /* Same binding, different matrix: a replugged/enumeration-
       changed device rebuilds emitters + positions on the next
       prepare — nothing is cached across calls. */
    PreparedStudioScene p1;
    CHECK(PrepareStudioScene(cand, { KeyboardSnapshot() }, p1, err));
    CHECK(p1.doc->objects[0].emitters.size() == 5);

    DeviceSnapshot bigger = KeyboardSnapshot();
    bigger.zones[0].matrix_rows = 3;
    bigger.zones[0].matrix_map  = { 0, 1, 2,
                                    3, 4, 5,
                                    0xFFFFFFFFu, 0xFFFFFFFFu,
                                    0xFFFFFFFFu };
    PreparedStudioScene p2;
    CHECK(PrepareStudioScene(cand, { bigger }, p2, err));
    CHECK(p2.doc->objects[0].emitters.size() == 6);
    /* The authored doc is still empty — growth never leaks
       back into the workspace. */
    CHECK(cand.resolved->objects[0].emitters.empty());
}

/*---------------------------------------------------------*\
||| Runtime layers: global params + ripple decay            ||
\*---------------------------------------------------------*/

static void TestRuntimeLayerCopy()
{
    WorkspaceCandidate cand = KeyboardCandidate();
    auto doc = std::make_shared<SceneDocument>(*cand.resolved);
    EffectLayer ripple;
    ripple.primitive = "ripple";
    ripple.density   = 2.0f;
    ripple.speed     = 1.0f;
    ripple.opacity   = 0.5f;
    EffectLayer wave;
    wave.primitive = "wave";
    wave.density   = 3.0f;
    wave.speed     = 1.0f;
    doc->effect.layers   = { ripple, wave };
    doc->effect.speed    = 2.0f;
    doc->effect.intensity = 0.5f;
    cand.resolved = doc;
    cand.document.inputs.decay_pct = 200;

    PreparedStudioScene p;
    std::string err;
    CHECK(PrepareStudioScene(cand, { KeyboardSnapshot() }, p, err));
    CHECK(p.layers.size() == 2);
    /* Global params first: speed/opacity scale; decay then
       multiplies ONLY ripple density (2.0 * 2.0). */
    CHECK(Near(p.layers[0].density, 4.0f));
    CHECK(Near(p.layers[0].speed,   2.0f));
    CHECK(Near(p.layers[0].opacity, 0.25f));
    /* Non-ripple density is untouched by decay. */
    CHECK(Near(p.layers[1].density, 3.0f));
    /* Authored layers never mutated. */
    CHECK(Near(doc->effect.layers[0].density, 2.0f));
    CHECK(Near(doc->effect.layers[0].opacity, 0.5f));

    /* Rebuilds do not compound: same candidate, same result. */
    PreparedStudioScene p2;
    CHECK(PrepareStudioScene(cand, { KeyboardSnapshot() }, p2, err));
    CHECK(Near(p2.layers[0].density, 4.0f));
}

static void TestDecayDefaultAndClamp()
{
    WorkspaceCandidate cand = KeyboardCandidate();
    auto doc = std::make_shared<SceneDocument>(*cand.resolved);
    EffectLayer ripple;
    ripple.primitive = "ripple";
    ripple.density   = 1.5f;
    doc->effect.layers = { ripple };
    cand.resolved = doc;

    /* Default 100 -> multiplier 1.0. */
    PreparedStudioScene p;
    std::string err;
    CHECK(PrepareStudioScene(cand, {}, p, err));
    CHECK(Near(p.layers[0].density, 1.5f));

    /* A hand-rolled out-of-range value clamps, never scales
       unboundedly (parse bounds are 50..300). */
    cand.document.inputs.decay_pct = 9999;
    CHECK(PrepareStudioScene(cand, {}, p, err));
    CHECK(Near(p.layers[0].density, 1.5f * 3.0f));
    cand.document.inputs.decay_pct = 1;
    CHECK(PrepareStudioScene(cand, {}, p, err));
    CHECK(Near(p.layers[0].density, 1.5f * 0.5f));
}

static void TestPresetResolutionViaCandidateRegistry()
{
    WorkspaceCandidate cand = KeyboardCandidate();
    auto doc = std::make_shared<SceneDocument>(*cand.resolved);
    doc->effect.preset = "look-one";
    doc->effect.seed   = 7;
    doc->effect.layers.clear();
    cand.resolved = doc;

    /* The candidate's registry snapshot resolves the look —
       the engine-global registry is not consulted. */
    EffectDocument fx;
    fx.id   = "look-one";
    fx.name = "Look One";
    fx.layers = nlohmann::ordered_json::array({
        nlohmann::ordered_json::object({
            { "primitive", "static" },
            { "opacity",   0.6      },
            { "palette", nlohmann::ordered_json::array({
                  nlohmann::ordered_json::object({
                      { "pos",   0.0       },
                      { "color", "#FF0000" } }) }) } }) });
    std::vector<std::string> errs;
    CHECK(cand.effect_registry.Add(fx, &errs));

    PreparedStudioScene p;
    std::string err;
    CHECK(PrepareStudioScene(cand, {}, p, err));
    CHECK(p.layers.size() == 1);
    CHECK(p.layers[0].primitive == "static");
    CHECK(Near(p.layers[0].opacity, 0.6f));

    /* An unknown preset leaves an empty runtime stack without
       failing the prepare — ApplyWorkspaceCandidate lets the
       caller decide name validity (playback names resolve to
       no live layers legitimately). */
    doc->effect.preset = "no-such-look";
    CHECK(PrepareStudioScene(cand, {}, p, err));
    CHECK(p.layers.empty());
}

/*---------------------------------------------------------*\
||| Key event positioning                                   ||
\*---------------------------------------------------------*/

static void TestPositionInput()
{
    WorkspaceCandidate cand = KeyboardCandidate();
    PreparedStudioScene p;
    std::string err;
    CHECK(PrepareStudioScene(cand, { KeyboardSnapshot() }, p, err));
    CHECK(!p.key_positions.empty());

    const int vk_a = VkForKeyName("Key: A");
    const int vk_f = VkForKeyName("Key: F");
    CHECK(vk_a >= 0 && vk_f >= 0);

    InputState in;
    InputEvent key_a;
    key_a.source = "key";
    key_a.code   = vk_a;
    InputEvent unknown_key;
    unknown_key.source = "key";
    unknown_key.code   = 0xFF61;             /* unmapped VK */
    InputEvent prepositioned;
    prepositioned.source  = "key";
    prepositioned.code    = vk_f;
    prepositioned.has_pos = true;
    prepositioned.pos     = { 9.0f, 9.0f, 9.0f };
    InputEvent audio_evt;
    audio_evt.source = "audio";
    in.events = { key_a, unknown_key, prepositioned, audio_evt };

    const InputState out = PositionStudioInput(in, p);
    /* Matched key gains the emitter's world position. */
    CHECK(out.events[0].has_pos);
    CHECK(NearV(out.events[0].pos, p.key_positions.at(vk_a)));
    /* Unmatched key keeps the layer-origin fallback. */
    CHECK(!out.events[1].has_pos);
    /* An already-positioned event is never overridden. */
    CHECK(out.events[2].has_pos);
    CHECK(NearV(out.events[2].pos, Vec3{ 9.0f, 9.0f, 9.0f }));
    /* Non-key sources pass through untouched. */
    CHECK(!out.events[3].has_pos);
    /* Input object itself is never mutated. */
    CHECK(!in.events[0].has_pos);
}

int main()
{
    TestMatrixExpansion();
    TestMatrixOutOfRange();
    TestMissingKeyboard();
    TestTransformedKeyboard();
    TestDuplicateKeyNamesFirstWins();
    TestTopologyRebuild();
    TestRuntimeLayerCopy();
    TestDecayDefaultAndClamp();
    TestPresetResolutionViaCandidateRegistry();
    TestPositionInput();
    std::printf("scene preparation: %d checks, %d failures\n",
                checks, failures);
    return failures ? 1 : 0;
}
