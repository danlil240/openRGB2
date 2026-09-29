/*---------------------------------------------------------*\
||| workspace_candidate_test.cpp                              |
|||                                                           |
|||   Tests for the canonical workspace candidate           |
|||   (src/core/config/WorkspaceCandidate.h — studio-import |
|||   Task 2): the ONE parse/migrate/validate/resolve path  |
|||   every persisted form funnels through.                  |
|||                                                           |
|||   Covered cases (task brief):                           |
|||   - v3_roundtrip_keeps_authoring_data                   |
|||   - legacy_extracts_types                               |
|||   - expanded_scene_migrates                             |
|||   - missing_type_rejected                               |
|||   - future_version_not_rewritten                        |
|||   - registry isolation (supplied registries are never   |
|||     mutated by extracted types)                         |
|||                                                           |
|||   Comparisons check compact fields AND resolved         |
|||   transforms/colors/layers — never just parser success. |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "config/WorkspaceCandidate.h"

#include "config/StudioConfig.h"
#include "effects/EffectJson.h"
#include "presets/DevicePreset.h"
#include "presets/EffectRegistry.h"
#include "presets/PresetRegistry.h"
#include "scene/SceneJson.h"
#include "scene/SceneResolver.h"
#include "scene/SceneTypes.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using nlohmann::json;
using openrgb2::ParseWorkspaceCandidate;
using openrgb2::WorkspaceCandidate;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

static bool Near(float a, float b, float eps = 1e-4f)
{
    return std::fabs(a - b) < eps;
}

static bool HasError(const std::vector<std::string>& errors,
                     const std::string& needle)
{
    for(const std::string& e : errors)
    {
        if(e.find(needle) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

/*---------------------------------------------------------*\
||| Registries — the packaged device types / effect looks  ||
||| resolve through DLS_PRESET_DIR / DLS_EFFECT_DIR (set   ||
||| by tests/core/CMakeLists.txt) with a source-tree       ||
||| probe fallback.                                        ||
\*---------------------------------------------------------*/
static std::string EnvDir(const char* var, const char* fallback)
{
    if(const char* env = std::getenv(var))
    {
        if(*env != '\0' &&
           std::filesystem::is_directory(
               std::filesystem::path(env)))
        {
            return env;
        }
    }
    std::error_code ec;
    if(std::filesystem::is_directory(fallback, ec))
    {
        return fallback;
    }
    return {};
}

static studio::PresetRegistry DeviceLib()
{
    studio::PresetRegistry reg;
    const std::string dir =
        EnvDir("DLS_PRESET_DIR", "../../src/core/presets/devices");
    if(!dir.empty())
    {
        reg.LoadDirectory(dir, nullptr);
    }
    return reg;
}

static studio::EffectRegistry EffectLib()
{
    studio::EffectRegistry reg;
    const std::string dir =
        EnvDir("DLS_EFFECT_DIR", "../../src/core/presets/effects");
    if(!dir.empty())
    {
        reg.LoadDirectory(dir, nullptr);
    }
    return reg;
}

static bool Parse(const json& j, const studio::PresetRegistry& dev,
                  const studio::EffectRegistry& eff,
                  WorkspaceCandidate& out,
                  std::vector<std::string>& errors,
                  std::vector<std::string>& warnings)
{
    return ParseWorkspaceCandidate(j, dev, eff, out, errors, warnings);
}

static const studio::SceneObject* FindObj(const studio::SceneDocument& d,
                                          const std::string& id)
{
    return studio::FindObject(d, id);
}

/* const-safe map lookups (std::map::operator[] is non-const). */
static const studio::SceneColor* ColorAt(
    const std::map<std::string, studio::SceneColor>& m,
    const std::string& k)
{
    const auto it = m.find(k);
    return it == m.end() ? nullptr : &it->second;
}
static const studio::SceneColor* EmitterColorAt(
    const std::map<std::string, std::map<int, studio::SceneColor>>& m,
    const std::string& k, int idx)
{
    const auto it = m.find(k);
    if(it == m.end())
    {
        return nullptr;
    }
    const auto eit = it->second.find(idx);
    return eit == it->second.end() ? nullptr : &eit->second;
}

/*---------------------------------------------------------*\
||| v3_roundtrip_keeps_authoring_data: a compact v3        ||
||| workspace parses into a candidate whose document keeps ||
||| every compact field and whose resolved scene carries   ||
||| the expanded transforms, colors, and effect layers.    ||
\*---------------------------------------------------------*/
static void TestV3RoundTripKeepsAuthoringData()
{
    studio::StudioDocument w;
    w.meta.name   = "authored desk";
    w.brightness  = 0.7f;

    studio::DeviceInstance fan;
    fan.type         = "fan-120";
    fan.position     = { 0.10f, 0.05f, -0.02f };
    fan.rotation_deg = { 0.0f, 45.0f, 0.0f };
    w.devices["fan0"] = fan;

    studio::DeviceBinding bind;
    bind.id              = "fan_bus";
    bind.controller_name = "X870E AORUS ELITE";
    bind.vendor          = "Gigabyte";
    bind.zone_name       = "ARGB_V2_1";
    bind.zone_leds       = 8;
    w.bindings["fan_bus"] = bind;

    studio::DeviceSettings ds;
    ds.zones["ring"] = { "fan_bus", 0, true };
    w.device_settings["fan0"] = ds;

    w.object_colors["fan0/body"]     = studio::MakeSceneColor(0x20, 0x40, 0x80);
    w.emitter_colors["fan0/body"][3] = studio::MakeSceneColor(0xFF, 0x00, 0x10);

    w.inputs.audio        = true;
    w.inputs.screen_index = 2;
    w.inputs.sens_pct     = 140;
    w.meta.camera.view    = "front";
    w.meta.ui.favorites   = { "fan-120" };
    w.meta.extensions["thirdparty.example"] = { {"answer", 42} };

    w.effect.preset    = "comet";        /* resolves via look registry */
    w.effect.seed      = 4242;
    w.effect.speed     = 1.5f;
    w.effect.intensity = 0.75f;
    /* An authored inline layer survives alongside the named preset —
       the layer stack is authoring data, not applied output. */
    studio::EffectLayer layer;
    layer.primitive = "wave";
    layer.speed     = 0.8f;
    layer.palette   = studio::MakePalette(
        { studio::MakeSceneColor(0xFF, 0x00, 0x00),
          studio::MakeSceneColor(0x00, 0x00, 0xFF) });
    w.effect.layers.push_back(layer);

    const json j = studio::ToJson(w);

    WorkspaceCandidate c;
    std::vector<std::string> errors, warnings;
    CHECK(Parse(j, DeviceLib(), EffectLib(), c, errors, warnings));
    for(const std::string& e : errors)
    {
        std::printf("  (unexpected error: %s)\n", e.c_str());
    }

    /* Provenance + registry snapshots. */
    CHECK(c.source_form == WorkspaceCandidate::SourceForm::CompactWorkspace);
    CHECK(!c.migrated);
    CHECK(c.extracted_types.empty());
    CHECK(c.device_registry.Contains("fan-120"));
    CHECK(c.effect_registry.Contains("comet"));

    /* Compact authoring fields survived — not just parser success. */
    CHECK(c.document.meta.name == "authored desk");
    CHECK(c.document.devices.size() == 1);
    CHECK(c.document.devices["fan0"].type == "fan-120");
    CHECK(Near(c.document.devices["fan0"].position.x, 0.10f));
    CHECK(Near(c.document.devices["fan0"].rotation_deg.y, 45.0f));
    CHECK(c.document.bindings["fan_bus"].zone_leds == 8);
    CHECK(c.document.device_settings["fan0"].zones["ring"].verified);
    CHECK(c.document.object_colors["fan0/body"]
              == studio::MakeSceneColor(0x20, 0x40, 0x80));
    CHECK(c.document.emitter_colors["fan0/body"][3]
              == studio::MakeSceneColor(0xFF, 0x00, 0x10));
    CHECK(c.document.inputs.audio && c.document.inputs.screen_index == 2
          && c.document.inputs.sens_pct == 140);
    CHECK(c.document.meta.camera.view == "front");
    CHECK(c.document.meta.ui.favorites.size() == 1);
    CHECK(c.document.meta.extensions["thirdparty.example"]["answer"] == 42);
    CHECK(c.document.effect.preset == "comet"
          && c.document.effect.seed == 4242
          && c.document.effect.layers.size() == 1);

    /* The resolved runtime scene: expanded objects, transforms,
       colors, and the authored layer stack. */
    CHECK(c.resolved != nullptr);
    if(c.resolved)
    {
        const studio::SceneDocument& s = *c.resolved;
        CHECK(s.name == "authored desk");
        CHECK(Near(s.brightness, 0.7f));
        const studio::SceneObject* grp = FindObj(s, "fan0");
        const studio::SceneObject* ent = FindObj(s, "fan0/body");
        CHECK(grp != nullptr && ent != nullptr);
        if(ent)
        {
            CHECK(ent->kind == studio::ObjectKind::Device);
            CHECK(ent->binding == "fan_bus");
            CHECK(ent->verified);
            CHECK(ent->emitters.size() == 8);
            bool addrs_ok = true;
            for(size_t i = 0; i < ent->emitters.size(); ++i)
            {
                addrs_ok &= ent->emitters[i].address == (int)i;
            }
            CHECK(addrs_ok);
        }
        const studio::SceneColor* oc = ColorAt(s.object_colors,
                                               "fan0/body");
        CHECK(oc != nullptr &&
              *oc == studio::MakeSceneColor(0x20, 0x40, 0x80));
        const studio::SceneColor* ec =
            EmitterColorAt(s.emitter_colors, "fan0/body", 3);
        CHECK(ec != nullptr &&
              *ec == studio::MakeSceneColor(0xFF, 0x00, 0x10));
        CHECK(s.effect.preset == "comet");
        CHECK(s.effect.layers.size() == 1);
        if(s.effect.layers.size() == 1)
        {
            CHECK(s.effect.layers[0].primitive == "wave");
            CHECK(Near(s.effect.layers[0].speed, 0.8f));
            CHECK(s.effect.layers[0].palette.stops.size() == 2);
        }
    }
}

/*---------------------------------------------------------*\
||| legacy_extracts_types: a host-settings blob            ||
||| {"scene":..., "inputs":...} migrates into a compact    ||
||| workspace, extracts its shared type, keeps inputs, and ||
||| resolves — without mutating the supplied registry.     ||
\*---------------------------------------------------------*/
static void TestLegacyExtractsTypes()
{
    /* Two identical device objects + a decor — the two devices
       deduplicate into ONE extracted type. */
    const json blob = json::parse(R"json({
        "scene": {
            "version": 2,
            "name": "legacy desk",
            "bindings": [
                { "id": "b1", "controller_name": "Ctrl",
                  "zone_name": "z", "zone_leds": 2 },
                { "id": "b2", "controller_name": "Ctrl",
                  "zone_name": "z", "zone_leds": 2 }
            ],
            "objects": [
                { "id": "fa", "kind": "device", "binding": "b1",
                  "geometry": "fan_ring", "label": "Fan A",
                  "verified": true,
                  "emitters": [
                      { "address": 0, "group": "ring",
                        "pos": { "x": 0.0, "y": 0.0, "z": 0.0 } },
                      { "address": 1, "group": "ring",
                        "pos": { "x": 0.01, "y": 0.0, "z": 0.0 } }
                  ] },
                { "id": "fb", "kind": "device", "binding": "b2",
                  "geometry": "fan_ring", "label": "Fan B",
                  "verified": true,
                  "emitters": [
                      { "address": 0, "group": "ring",
                        "pos": { "x": 0.0, "y": 0.0, "z": 0.0 } },
                      { "address": 1, "group": "ring",
                        "pos": { "x": 0.01, "y": 0.0, "z": 0.0 } }
                  ] }
            ]
        },
        "inputs": { "audio": true, "screen_index": 2,
                    "sens_pct": 175, "decay_pct": 90 }
    })json");

    studio::PresetRegistry base = DeviceLib();
    const size_t base_ids = base.Ids().size();

    WorkspaceCandidate c;
    std::vector<std::string> errors, warnings;
    CHECK(Parse(blob, base, EffectLib(), c, errors, warnings));
    if(!errors.empty())
    {
        std::printf("  (unexpected error: %s)\n", errors.front().c_str());
    }

    CHECK(c.source_form == WorkspaceCandidate::SourceForm::LegacySettings);
    CHECK(c.migrated);

    /* One extracted type for the structurally identical pair. */
    CHECK(c.extracted_types.size() == 1);
    std::string type_id;
    if(!c.extracted_types.empty())
    {
        type_id = c.extracted_types[0].id;
    }

    /* The compact document kept the authoring data. */
    CHECK(c.document.schema_version == studio::STUDIO_SCHEMA_VERSION);
    CHECK(c.document.meta.name == "legacy desk");
    CHECK(c.document.devices.count("fa") == 1
          && c.document.devices.count("fb") == 1);
    CHECK(!type_id.empty() && c.document.devices["fa"].type == type_id
          && c.document.devices["fb"].type == type_id);
    CHECK(c.document.inputs.audio
          && c.document.inputs.screen_index == 2
          && c.document.inputs.sens_pct == 175
          && c.document.inputs.decay_pct == 90);
    /* Migration never arms live output. */
    CHECK(!c.document.meta.live_on_startup);

    /* The resolved scene re-expands the migrated workspace —
       both devices bound and verified (verified was authored). */
    CHECK(c.resolved != nullptr);
    if(c.resolved)
    {
        int bound_devices = 0;
        for(const studio::SceneObject& o : c.resolved->objects)
        {
            if(o.kind == studio::ObjectKind::Device &&
               (o.binding == "b1" || o.binding == "b2"))
            {
                ++bound_devices;
                CHECK(o.verified);
                CHECK(o.emitters.size() == 2);
            }
        }
        CHECK(bound_devices == 2);
    }

    /* Registry isolation: extracted types layered onto a COPY —
       the caller's registry never gains them. */
    CHECK(base.Ids().size() == base_ids);
    CHECK(!type_id.empty() && !base.Contains(type_id));
    CHECK(!type_id.empty() && c.device_registry.Contains(type_id));
}

/*---------------------------------------------------------*\
||| expanded_scene_migrates: an expanded OpenRGB2          ||
||| SceneDocument ("version") keeps its resolved scene     ||
||| verbatim AND yields a migrated compact document — the  ||
||| authoring data is no longer discarded.                 ||
\*---------------------------------------------------------*/
static void TestExpandedSceneMigrates()
{
    const json j = json::parse(R"json({
        "version": 2,
        "name": "expanded scene",
        "bindings": [
            { "id": "b1", "controller_name": "Ctrl",
              "zone_name": "z", "zone_leds": 2 }
        ],
        "objects": [
            { "id": "fan0", "kind": "device", "binding": "b1",
              "geometry": "fan_ring", "label": "Fan",
              "position": { "x": 0.25, "y": 0.0, "z": 0.1 },
              "verified": true,
              "emitters": [
                  { "address": 0, "group": "ring",
                    "pos": { "x": 0.0, "y": 0.0, "z": 0.0 } },
                  { "address": 1, "group": "ring",
                    "pos": { "x": 0.01, "y": 0.0, "z": 0.0 } }
              ] }
        ],
        "object_colors": { "fan0": "#2050A0" },
        "emitter_colors": { "fan0": { "1": "#FF0000" } }
    })json");

    WorkspaceCandidate c;
    std::vector<std::string> errors, warnings;
    CHECK(Parse(j, DeviceLib(), EffectLib(), c, errors, warnings));
    if(!errors.empty())
    {
        std::printf("  (unexpected error: %s)\n", errors.front().c_str());
    }

    CHECK(c.source_form == WorkspaceCandidate::SourceForm::ExpandedScene);
    CHECK(c.migrated);

    /* The migrated authoring form: instance + extracted type +
       the binding/color state carried into compact sections. */
    CHECK(c.document.schema_version == studio::STUDIO_SCHEMA_VERSION);
    CHECK(c.document.meta.name == "expanded scene");
    CHECK(!c.extracted_types.empty());
    CHECK(c.document.devices.count("fan0") == 1);
    CHECK(c.document.bindings.count("b1") == 1);
    if(c.document.devices.count("fan0") && !c.extracted_types.empty())
    {
        CHECK(c.document.devices["fan0"].type == c.extracted_types[0].id);
    }
    /* Placement kept. */
    if(c.document.devices.count("fan0"))
    {
        CHECK(Near(c.document.devices["fan0"].position.x, 0.25f));
        CHECK(Near(c.document.devices["fan0"].position.z, 0.10f));
    }

    /* The resolved scene is the expanded document itself —
       expanded-scene compatibility is preserved verbatim
       (object ids and emitter sets do not get re-keyed). */
    CHECK(c.resolved != nullptr);
    if(c.resolved)
    {
        CHECK(c.resolved->name == "expanded scene");
        const studio::SceneObject* o = FindObj(*c.resolved, "fan0");
        CHECK(o != nullptr);
        if(o)
        {
            CHECK(o->kind == studio::ObjectKind::Device);
            CHECK(o->binding == "b1");
            CHECK(o->verified);
            CHECK(o->emitters.size() == 2);
        }
        const studio::SceneColor* oc =
            ColorAt(c.resolved->object_colors, "fan0");
        CHECK(oc != nullptr &&
              *oc == studio::MakeSceneColor(0x20, 0x50, 0xA0));
    }

    /* The migrated compact form is itself resolvable against the
       snapshot registry — durable, not a dead-end. */
    studio::SceneDocument reresolved;
    std::vector<std::string> rerrs;
    CHECK(studio::ResolveScene(c.document, c.device_registry,
                               reresolved, &rerrs));
    if(!rerrs.empty())
    {
        std::printf("  (re-resolve error: %s)\n", rerrs.front().c_str());
    }
    int bound = 0;
    for(const studio::SceneObject& o : reresolved.objects)
    {
        if(o.kind == studio::ObjectKind::Device && o.binding == "b1")
        {
            ++bound;
        }
    }
    CHECK(bound == 1);
}

/*---------------------------------------------------------*\
||| missing_type_rejected: a compact workspace referencing ||
||| a device type the supplied registry cannot resolve     ||
||| fails — never a silently-empty scene.                  ||
\*---------------------------------------------------------*/
static void TestMissingTypeRejected()
{
    const json j = json::parse(R"json({
        "schema_version": 3,
        "name": "dangling",
        "devices": {
            "mystery": { "type": "ghost-type",
                         "x": 0.0, "y": 0.0, "z": 0.0 }
        }
    })json");

    WorkspaceCandidate c;
    std::vector<std::string> errors, warnings;
    CHECK(!Parse(j, DeviceLib(), EffectLib(), c, errors, warnings));
    CHECK(HasError(errors, "ghost-type"));
    CHECK(c.resolved == nullptr);
}

/*---------------------------------------------------------*\
||| future_version_not_rewritten: a workspace newer than   ||
||| this build understands is rejected outright — never    ||
||| migrated down, never rewritten, output untouched.      ||
\*---------------------------------------------------------*/
static void TestFutureVersionNotRewritten()
{
    const json j = json::parse(R"json({
        "schema_version": 4,
        "name": "from the future",
        "devices": {}
    })json");
    const std::string before = j.dump();

    WorkspaceCandidate c;
    std::vector<std::string> errors, warnings;
    CHECK(!Parse(j, DeviceLib(), EffectLib(), c, errors, warnings));
    CHECK(HasError(errors, "schema_version"));
    CHECK(HasError(errors, "newer"));
    /* The input document is untouched — the caller keeps the
       original bytes for the user, nothing is rewritten. */
    CHECK(j.dump() == before);
    CHECK(!c.migrated);
    CHECK(c.resolved == nullptr);
}

/*---------------------------------------------------------*\
||| v1/v2 expanded WORKSPACE: {"schema_version":<3,"scene"}||
||| migrates through ConfigMigration and overlays the      ||
||| file's own preference sections (ConfigStore parity).   ||
\*---------------------------------------------------------*/
static void TestExpandedWorkspaceCarriesPrefs()
{
    const json j = json::parse(R"json({
        "schema_version": 2,
        "name": "v2 desk",
        "ui": { "favorites": ["fan-120"] },
        "inputs": { "audio": true, "keys": false, "screen": true,
                    "screen_index": 1, "sens_pct": 160,
                    "decay_pct": 110 },
        "output": { "brightness": 0.5, "live_on_startup": true },
        "extensions": { "old.plugin": { "kept": true } },
        "scene": {
            "version": 2,
            "name": "v2 scene",
            "objects": [
                { "id": "lamp", "kind": "decor",
                  "geometry": "box", "emitters": [] }
            ]
        }
    })json");

    WorkspaceCandidate c;
    std::vector<std::string> errors, warnings;
    CHECK(Parse(j, DeviceLib(), EffectLib(), c, errors, warnings));
    if(!errors.empty())
    {
        std::printf("  (unexpected error: %s)\n", errors.front().c_str());
    }

    CHECK(c.source_form == WorkspaceCandidate::SourceForm::ExpandedWorkspace);
    CHECK(c.migrated);
    /* Preference sections overlay the migrated document —
       except live_on_startup, which migration always clears. */
    CHECK(c.document.meta.name == "v2 desk");
    CHECK(c.document.inputs.audio && c.document.inputs.screen
          && c.document.inputs.screen_index == 1
          && c.document.inputs.sens_pct == 160);
    CHECK(Near(c.document.brightness, 0.5f));
    CHECK(!c.document.meta.live_on_startup);
    CHECK(c.document.meta.ui.favorites.size() == 1
          && c.document.meta.ui.favorites[0] == "fan-120");
    CHECK(c.document.meta.extensions["old.plugin"]["kept"] == true);
    CHECK(c.resolved != nullptr && !c.resolved->objects.empty());
}

int main()
{
    TestV3RoundTripKeepsAuthoringData();
    TestLegacyExtractsTypes();
    TestExpandedSceneMigrates();
    TestMissingTypeRejected();
    TestFutureVersionNotRewritten();
    TestExpandedWorkspaceCarriesPrefs();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
