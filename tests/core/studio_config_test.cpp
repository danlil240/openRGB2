/*---------------------------------------------------------*\
||| studio_config_test.cpp                                    |
|||                                                           |
|||   Deterministic tests for the Desktop Lighting Studio    |
|||   workspace document (plugins/DesktopLightingStudio/     |
|||   config/): compact v3 validation, legacy-settings       |
|||   migration, and full round-trips. No Qt, no hardware —  |
|||   plain cl build via build-studio-tests.bat.             |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "scene/SceneTypes.h"
#include "scene/SceneGraph.h"
#include "scene/SceneJson.h"
#include "scene/SceneResolver.h"
#include "scene/DefaultDesk.h"
#include "presets/DevicePreset.h"
#include "presets/PresetRegistry.h"
#include "config/StudioConfig.h"
#include "config/ConfigMigration.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using nlohmann::json;

/* The bundled type library — the authoritative defaults layer the
   plugin loads from the qrc; the test reads the same files from
   the source tree (the suite runs from tests/). */
static std::vector<studio::DevicePreset> PackagedPresets()
{
    if(const char* env = std::getenv("DLS_PRESET_DIR"))
    {
        const std::filesystem::path p(env);
        std::error_code ec;
        if(!p.empty() && std::filesystem::is_directory(p, ec))
        {
            std::vector<studio::DevicePreset> out;
            for(const auto& e :
                std::filesystem::directory_iterator(p, ec))
            {
                studio::DevicePreset preset;
                if(e.is_regular_file()
                   && studio::DevicePresetFromJsonFile(e.path().string(),
                                                       preset, nullptr))
                {
                    out.push_back(preset);
                }
            }
            if(!out.empty())
            {
                return out;
            }
        }
    }
    const std::filesystem::path candidates[3] = {
        std::filesystem::path("..") / "plugins"
            / "DesktopLightingStudio" / "presets" / "devices",
        std::filesystem::path("..") / ".." / "plugins"
            / "DesktopLightingStudio" / "presets" / "devices",
        std::filesystem::path("..") / ".." / "src"
            / "core" / "presets" / "devices",
    };
    std::vector<studio::DevicePreset> out;
    std::error_code ec;
    for(const auto& dir : candidates)
    {
        for(const auto& e :
            std::filesystem::directory_iterator(dir, ec))
        {
            studio::DevicePreset p;
            if(e.is_regular_file()
               && studio::DevicePresetFromJsonFile(e.path().string(),
                                                   p, nullptr))
            {
                out.push_back(p);
            }
        }
        if(!out.empty())
        {
            break;
        }
    }
    return out;
}

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
||| A small v3 workspace: two fan instances sharing one    ||
||| type (one mirrored), a binding, paint, prefs.          ||
\*---------------------------------------------------------*/
static studio::StudioDocument SmallWorkspace()
{
    studio::StudioDocument w;
    w.meta.name   = "Fixture desk";
    w.brightness  = 0.65f;

    studio::DeviceInstance a;
    a.type         = "fan-120";
    a.position     = { 0.0f, -0.16f, -0.14f };
    a.parent       = "case";
    w.devices["fan0"] = a;
    studio::DeviceInstance b;
    b.type         = "fan-120";
    b.position     = { 0.0f, -0.16f, 0.12f };
    b.parent       = "case";
    w.devices["fan0_copy"] = b;
    studio::DeviceInstance grp;
    grp.type       = "group";
    grp.position   = { 0.4f, 0.1f, -0.1f };
    w.devices["case"] = grp;
    studio::DeviceInstance desk;
    desk.type      = "desk";
    desk.position  = { 0.0f, -0.02f, 0.10f };
    w.devices["desk"] = desk;

    studio::DeviceBinding bind;
    bind.id              = "fan_bus";
    bind.controller_name = "X870E AORUS ELITE";
    bind.vendor          = "Gigabyte";
    bind.device_type     = -1;
    bind.zone_name       = "ARGB_V2_1";
    bind.zone_leds       = 8;
    w.bindings["fan_bus"] = bind;

    studio::DeviceSettings sa;
    sa.zones["ring"] = { "fan_bus", 0, true };
    w.device_settings["fan0"] = sa;
    studio::DeviceSettings sb;
    sb.mirror_of = "fan0";
    w.device_settings["fan0_copy"] = sb;

    w.object_colors["fan0/ring"]        = studio::MakeSceneColor(0x20, 0x40, 0x80);
    w.emitter_colors["fan0/ring"][3]    = studio::MakeSceneColor(0xFF, 0x00, 0x10);

    w.inputs.audio        = true;
    w.inputs.keys         = false;
    w.inputs.screen       = true;
    w.inputs.screen_index = 1;
    w.inputs.sens_pct     = 150;
    w.inputs.decay_pct    = 120;
    w.meta.render.quality = "high";
    w.meta.camera.view    = "top";
    w.meta.extensions["thirdparty.example"] = { {"answer", 42} };

    w.effect.preset    = "comet";
    w.effect.seed      = 4242;
    w.effect.speed     = 1.5f;
    w.effect.intensity = 0.75f;
    w.effect.playing   = true;
    return w;
}

/*---------------------------------------------------------*\
||| Round-trip: every persisted field survives             ||
||| ToJson -> FromJson, and the document shape matches the ||
||| spec's top-level sections.                             ||
\*---------------------------------------------------------*/
static void TestWorkspaceRoundTrip()
{
    using namespace studio;

    StudioDocument w = SmallWorkspace();
    const json j = ToJson(w);

    CHECK(j.value("schema_version", 0) == STUDIO_SCHEMA_VERSION,
          "workspace: schema_version written");
    CHECK(STUDIO_SCHEMA_VERSION == 3, "workspace: schema v3");
    CHECK(j.value("name", std::string()) == "Fixture desk",
          "workspace: top-level name");
    CHECK(j.contains("ui") && j.contains("camera") && j.contains("controls")
          && j.contains("render") && j.contains("inputs") && j.contains("output")
          && j.contains("devices") && j.contains("bindings")
          && j.contains("device_settings") && j.contains("colors")
          && j.contains("effects") && j.contains("extensions"),
          "workspace: all spec sections present");
    CHECK(!j.contains("scene") && !j.contains("definitions"),
          "workspace: no expanded sections");
    CHECK(Near(j["output"].value("brightness", 0.0f), 0.65f),
          "workspace: brightness under output");
    CHECK(j["output"].value("live_on_startup", true) == false,
          "workspace: live_on_startup written false");
    CHECK(j["effects"].value("preset", std::string()) == "comet",
          "workspace: effect preset under effects");
    CHECK(j["effects"].value("playing", false) == true,
          "workspace: playing under effects");
    CHECK(j["inputs"].value("sens_pct", 0) == 150,
          "workspace: input settings serialized");
    CHECK(j["extensions"].contains("thirdparty.example"),
          "workspace: extensions retained");
    CHECK(j["devices"]["fan0"].value("type", std::string()) == "fan-120"
          && j["devices"]["fan0"].value("parent", std::string()) == "case",
          "workspace: compact instance fields");
    CHECK(j["device_settings"]["fan0_copy"].value("mirror_of", std::string())
          == "fan0",
          "workspace: instance mirror in device_settings");

    StudioDocument back;
    std::vector<std::string> errors;
    CHECK(FromJson(j, back, &errors), "workspace: round-trip parses");
    if(!errors.empty())
    {
        std::printf("  (unexpected errors: %s)\n", errors.front().c_str());
    }
    CHECK(back.meta.name == "Fixture desk", "round-trip: name");
    CHECK(back.devices.size() == 4 && back.bindings.size() == 1,
          "round-trip: instances + bindings");
    CHECK(back.devices["fan0"].type == "fan-120"
          && back.devices["fan0"].parent == "case"
          && Near(back.devices["fan0"].position.y, -0.16f),
          "round-trip: instance fields");
    CHECK(back.device_settings["fan0_copy"].mirror_of == "fan0",
          "round-trip: mirror setting");
    CHECK(back.device_settings["fan0"].zones["ring"].binding == "fan_bus"
          && back.device_settings["fan0"].zones["ring"].verified,
          "round-trip: zone attachment");
    CHECK(back.object_colors["fan0/ring"] == MakeSceneColor(0x20, 0x40, 0x80),
          "round-trip: object color");
    CHECK(back.emitter_colors["fan0/ring"][3] == MakeSceneColor(0xFF, 0x00, 0x10),
          "round-trip: emitter color");
    CHECK(Near(back.brightness, 0.65f), "round-trip: brightness");
    CHECK(back.inputs.audio && !back.inputs.keys && back.inputs.screen
          && back.inputs.screen_index == 1 && back.inputs.sens_pct == 150
          && back.inputs.decay_pct == 120,
          "round-trip: input settings");
    CHECK(back.meta.render.quality == "high" && back.meta.camera.view == "top",
          "round-trip: meta prefs");
    CHECK(back.effect.preset == "comet" && back.effect.seed == 4242
          && Near(back.effect.speed, 1.5f)
          && Near(back.effect.intensity, 0.75f)
          && back.effect.playing,
          "round-trip: effect state");
    CHECK(back.meta.extensions["thirdparty.example"]["answer"] == 42,
          "round-trip: extensions verbatim");
    CHECK(back.meta.live_on_startup == false, "round-trip: live_on_startup");

    /* Deterministic serialization: same doc -> same text. */
    CHECK(ToJson(back) == j, "round-trip: deterministic serialization");
}

/*---------------------------------------------------------*\
||| Whole-document validation: field-specific errors,      ||
||| candidate left untouched on failure.                   ||
\*---------------------------------------------------------*/
static void TestWorkspaceValidation()
{
    using namespace studio;

    const json good = ToJson(SmallWorkspace());
    std::vector<std::string> errors, warnings;

    {
        json j = good;
        j["inputs"] = "on";
        StudioDocument doc;
        CHECK(!FromJson(j, doc, &errors) && HasError(errors, "inputs"),
              "validate: inputs wrong type rejected");
    }
    {
        json j = good;
        j["output"]["brightness"] = "loud";
        StudioDocument doc;
        CHECK(!FromJson(j, doc, &errors) && HasError(errors, "brightness"),
              "validate: brightness wrong type rejected");
    }
    {
        json j = good;
        j["output"]["brightness"] = 1.5;
        StudioDocument doc;
        CHECK(!FromJson(j, doc, &errors) && HasError(errors, "brightness"),
              "validate: brightness out of range rejected");
    }
    {
        json j = good;
        j["schema_version"] = "3";
        StudioDocument doc;
        CHECK(!FromJson(j, doc, &errors) && HasError(errors, "schema_version"),
              "validate: schema_version wrong type rejected");
    }
    {
        json j = good;
        j["render"]["quality"] = "ultra";
        StudioDocument doc;
        CHECK(!FromJson(j, doc, &errors) && HasError(errors, "quality"),
              "validate: unknown render quality rejected");
    }
    {
        json j = good;
        j["inputs"]["sens_pct"] = 900;
        StudioDocument doc;
        CHECK(!FromJson(j, doc, &errors) && HasError(errors, "sens_pct"),
              "validate: sens_pct out of range rejected");
    }

    /* versions: newer rejected untouched, older is the expanded
       format (migrated at the file layer, not here) */
    {
        json j = good;
        j["schema_version"] = STUDIO_SCHEMA_VERSION + 1;
        StudioDocument doc;
        CHECK(!FromJson(j, doc, &errors) && HasError(errors, "schema_version"),
              "validate: newer workspace version rejected");
    }
    {
        json j = good;
        j["schema_version"] = 2;
        StudioDocument doc;
        CHECK(!FromJson(j, doc, &errors) && HasError(errors, "schema_version"),
              "validate: expanded v2 rejected by the v3 reader");
    }

    /* expanded sections must not appear in a v3 workspace */
    {
        json j = good;
        j["scene"] = json::object();
        StudioDocument doc;
        CHECK(!FromJson(j, doc, &errors) && HasError(errors, "scene"),
              "validate: embedded scene rejected");
    }
    {
        json j = good;
        j["definitions"] = json::object();
        StudioDocument doc;
        CHECK(!FromJson(j, doc, &errors) && HasError(errors, "definitions"),
              "validate: embedded definitions rejected");
    }

    /* compact-section structural errors */
    {
        json j = good;
        j["devices"]["fan0"]["type"] = "has space";
        StudioDocument doc;
        CHECK(!FromJson(j, doc, &errors) && HasError(errors, "type"),
              "validate: bad type id rejected");
    }
    {
        json j = good;
        j["devices"]["fan0"]["parent"] = "ghost";
        StudioDocument doc;
        CHECK(!FromJson(j, doc, &errors) && HasError(errors, "parent"),
              "validate: dangling instance parent rejected");
    }
    {
        json j = good;
        j["devices"]["fan0"]["parent"] = "fan0_copy";
        j["devices"]["fan0_copy"]["parent"] = "fan0";
        StudioDocument doc;
        CHECK(!FromJson(j, doc, &errors) && HasError(errors, "cycle"),
              "validate: instance parent cycle rejected");
    }
    {
        json j = good;
        j["devices"]["fan0"]["x"] = "north";
        StudioDocument doc;
        CHECK(!FromJson(j, doc, &errors) && HasError(errors, "devices.fan0.x"),
              "validate: malformed position rejected");
    }
    {
        json j = good;
        j["device_settings"]["fan0"]["zones"]["ring"]["binding"] = "ghost";
        StudioDocument doc;
        CHECK(!FromJson(j, doc, &errors) && HasError(errors, "binding"),
              "validate: dangling zone binding rejected");
    }
    {
        json j = good;
        j["device_settings"]["fan0"]["mirror_of"] = "fan0";
        StudioDocument doc;
        CHECK(!FromJson(j, doc, &errors) && HasError(errors, "mirror_of"),
              "validate: self mirror rejected");
    }
    {
        json j = good;
        j["device_settings"]["fan0"]["mirror_of"] = "ghost";
        StudioDocument doc;
        CHECK(!FromJson(j, doc, &errors) && HasError(errors, "mirror_of"),
              "validate: dangling mirror target rejected");
    }
    {
        json j = good;
        j["device_settings"]["ghost"] = {{"visible", false}};
        StudioDocument doc;
        CHECK(!FromJson(j, doc, &errors) && HasError(errors, "ghost"),
              "validate: settings for unknown instance rejected");
    }

    /* failed loads never touch the active candidate */
    {
        StudioDocument doc = SmallWorkspace();
        const size_t n = doc.devices.size();
        json bad = good;
        bad["schema_version"] = 42;
        CHECK(!FromJson(bad, doc, &errors)
              && doc.devices.size() == n
              && doc.meta.name == "Fixture desk",
              "validate: failure leaves document untouched");
    }

    /* an empty desk is a valid workspace */
    {
        json j = good;
        j["devices"]         = json::object();
        j["bindings"]        = json::object();
        j["device_settings"] = json::object();
        j["colors"]          = {{"objects", json::object()},
                                {"emitters", json::object()}};
        StudioDocument doc;
        CHECK(FromJson(j, doc, &errors) && doc.devices.empty(),
              "validate: empty desk allowed");
    }

    /* unknown top-level fields warn instead of failing */
    {
        json j = good;
        j["camer"] = j["camera"];
        StudioDocument doc;
        errors.clear(); warnings.clear();
        CHECK(FromJson(j, doc, &errors, &warnings)
              && HasError(warnings, "camer"),
              "validate: unknown field reported as warning");
    }

    /* extensions must still be an object */
    {
        json j = good;
        j["extensions"] = json::array();
        StudioDocument doc;
        CHECK(!FromJson(j, doc, &errors) && HasError(errors, "extensions"),
              "validate: extensions wrong type rejected");
    }
}

/*---------------------------------------------------------*\
||| #RRGGBB colors on write; packed ints still accepted.   ||
\*---------------------------------------------------------*/
static void TestHexColors()
{
    using namespace studio;

    StudioDocument w = SmallWorkspace();
    const json j = ToJson(w);

    CHECK(j["colors"]["objects"]["fan0/ring"].is_string()
          && j["colors"]["objects"]["fan0/ring"] == "#204080",
          "colors: object color written as #RRGGBB");
    CHECK(j["colors"]["emitters"]["fan0/ring"]["3"] == "#FF0010",
          "colors: emitter color written as #RRGGBB");

    /* packed ints (pre-existing files) still parse */
    json legacy_color = j;
    legacy_color["colors"]["objects"]["fan0/ring"] = 0x00804020;
    StudioDocument doc;
    std::vector<std::string> errors;
    CHECK(FromJson(legacy_color, doc, &errors)
          && doc.object_colors["fan0/ring"] == MakeSceneColor(0x20, 0x40, 0x80),
          "colors: packed int accepted");

    /* malformed color rejected with a field path */
    json bad = j;
    bad["colors"]["objects"]["fan0/ring"] = "blue";
    CHECK(!FromJson(bad, doc, &errors)
          && HasError(errors, "colors"),
          "colors: malformed color rejected");
}

/*---------------------------------------------------------*\
||| Legacy host-settings blob -> v3 migration. Fixture     ||
||| mirrors what the old saveScene() wrote into OpenRGB    ||
||| settings: {"scene": v1 doc, "inputs": {...}}.          ||
\*---------------------------------------------------------*/
static json LegacyFixture()
{
    /* v1 scene: flat list, decor carried its dimensions in scale. */
    json scene = {
        {"version", 1},
        {"name", "Daniel's desk"},
        {"brightness", 0.8},
        {"bindings", json::array({
            {
                {"id", "kbd_g512"},
                {"controller_name", "G512"},
                {"vendor", "Logitech"},
                {"serial", ""},
                {"location", ""},
                {"device_type", -1},
                {"zone_name", "Keyboard"},
                {"zone_leds", 0},
            },
            {
                {"id", "fan_bus"},
                {"controller_name", "X870E AORUS ELITE"},
                {"vendor", "Gigabyte"},
                {"serial", ""},
                {"location", ""},
                {"device_type", -1},
                {"zone_name", "ARGB_V2_1"},
                {"zone_leds", 8},
            },
        })},
        {"objects", json::array({
            {
                {"id", "desk"},
                {"label", "Desk"},
                {"kind", "decor"},
                {"position", {{"x", 0.0}, {"y", -0.02}, {"z", 0.10}}},
                {"rotation", {{"x", 0}, {"y", 0}, {"z", 0}}},
                {"scale",    {{"x", 1.4}, {"y", 0.04}, {"z", 0.75}}},
                {"binding", ""},
                {"mirror_of", ""},
                {"geometry", "desk"},
                {"layout", ""},
                {"verified", false},
                {"visible", true},
                {"emitters", json::array()},
            },
            {
                {"id", "keyboard"},
                {"label", "Keyboard"},
                {"kind", "device"},
                {"position", {{"x", 0.0}, {"y", 0.02}, {"z", 0.15}}},
                {"rotation", {{"x", 0}, {"y", -8}, {"z", 0}}},
                {"scale",    {{"x", 1}, {"y", 1}, {"z", 1}}},
                {"binding", "kbd_g512"},
                {"mirror_of", ""},
                {"geometry", "keyboard"},
                {"layout", "matrix_map"},
                {"verified", true},
                {"visible", true},
                {"emitters", json::array({
                    {{"pos", {{"x", -0.1}, {"y", 0.016}, {"z", 0.0}}},
                     {"group", "keyboard"}, {"address", 12}},
                })},
            },
            {
                {"id", "case_fans"},
                {"label", "Case fans"},
                {"kind", "device"},
                {"position", {{"x", 0.4}, {"y", -0.06}, {"z", -0.24}}},
                {"rotation", {{"x", 0}, {"y", 0}, {"z", 0}}},
                {"scale",    {{"x", 1}, {"y", 1}, {"z", 1}}},
                {"binding", "fan_bus"},
                {"mirror_of", ""},
                {"geometry", "fan_body"},
                {"layout", ""},
                {"verified", true},
                {"visible", true},
                {"emitters", json::array({
                    {{"pos", {{"x", 0.05}, {"y", 0.016}, {"z", 0.0}}},
                     {"group", "case_fans"}, {"address", 0}},
                })},
            },
            {
                {"id", "case_fan_b1"},
                {"label", "Case fan (mirror)"},
                {"kind", "linked"},
                {"position", {{"x", 0.4}, {"y", -0.06}, {"z", 0.02}}},
                {"rotation", {{"x", 0}, {"y", 0}, {"z", 0}}},
                {"scale",    {{"x", 1}, {"y", 1}, {"z", 1}}},
                {"binding", ""},
                {"mirror_of", "case_fans"},
                {"geometry", "fan_body"},
                {"layout", ""},
                {"verified", false},
                {"visible", true},
                {"emitters", json::array()},
            },
        })},
        {"object_colors",  {{"keyboard", 0x000000FFu}, {"case_fans", 0x00204080u}}},
        {"emitter_colors", {{"keyboard", {{"12", 0x0000FF00u}}}}},
        {"effect", {
            {"preset", "aurora"},
            {"seed", 7},
            {"speed", 1.25},
            {"intensity", 0.9},
            {"playing", true},
        }},
    };
    return {
        {"scene", scene},
        {"inputs", {
            {"audio", true},
            {"keys", true},
            {"screen", false},
            {"screen_index", 0},
            {"sens_pct", 175},
            {"decay_pct", 200},
        }},
    };
}

static void TestLegacyMigration()
{
    using namespace studio;

    const json legacy = LegacyFixture();
    CHECK(HasLegacySettings(legacy), "migration: fixture detected");
    CHECK(!HasLegacySettings(json::object()) && !HasLegacySettings(json()),
          "migration: empty settings not a migration source");

    StudioDocument w;
    std::vector<DevicePreset> types;
    std::vector<std::string> errors;
    CHECK(MigrateLegacySettings(legacy, w, &types, &errors),
          "migration: converts");
    if(!errors.empty())
    {
        std::printf("  (migration errors: %s)\n", errors.front().c_str());
    }

    CHECK(w.schema_version == STUDIO_SCHEMA_VERSION, "migration: version");
    CHECK(w.meta.name == "Daniel's desk", "migration: name");
    CHECK(!types.empty(), "migration: types extracted");

    /* IDs, positions, mirrored ownership preserved — objects became
       compact instances, ids stable. */
    CHECK(w.devices.size() == 4, "migration: instance count");
    CHECK(w.devices.count("keyboard") == 1
          && w.devices["keyboard"].parent.empty()
          && Near(w.devices["keyboard"].rotation_deg.y, -8.0f),
          "migration: keyboard instance + rotation");
    CHECK(w.bindings.count("kbd_g512") == 1
          && w.bindings.count("fan_bus") == 1,
          "migration: binding identity preserved");
    CHECK(w.device_settings.count("case_fan_b1") == 1
          && w.device_settings["case_fan_b1"].mirror_of == "case_fans",
          "migration: mirror preserved as instance setting");
    CHECK(w.device_settings["case_fans"].zones["main"].binding == "fan_bus"
          && w.device_settings["case_fans"].zones["main"].verified,
          "migration: zone binding + verification preserved");
    /* the keyboard's single emitter sat at address 12 — sparse, so
       the extracted zone keeps explicit addressing or the base. */
    const ZoneSetting& kz = w.device_settings["keyboard"].zones["main"];
    CHECK(kz.binding == "kbd_g512" && kz.verified
          && kz.addr_base == 12,
          "migration: emitter address base preserved");

    /* colors re-keyed to <instance>/<entity> */
    CHECK(w.object_colors["keyboard/body"] == 0x000000FFu,
          "migration: object color re-keyed");
    CHECK(w.emitter_colors["keyboard/body"][12] == 0x0000FF00u,
          "migration: emitter color re-keyed");
    CHECK(Near(w.brightness, 0.8f), "migration: brightness preserved");

    /* effect state + input settings preserved */
    CHECK(w.effect.preset == "aurora" && w.effect.playing
          && w.effect.seed == 7 && Near(w.effect.speed, 1.25f),
          "migration: effect state preserved");

    /* The authored inline stack (scene.effect.layers in the
       expanded doc) survives into workspace effects.layers —
       task 6.2 requires the inline stack to ride the v1/v2
       upgrade path, not just the scalar effect fields. */
    {
        json layered = legacy;
        layered["scene"]["effect"]["layers"] = json::array({
            {{"primitive", "wave"},
             {"speed", 2.0},
             {"direction", {1.0, 0.0, 0.0}},
             {"palette", json::array({
                 {{"pos", 0.0}, {"color", "#FF0000"}},
                 {{"pos", 1.0}, {"color", "#0000FF"}}})}},
        });
        StudioDocument wl;
        std::vector<DevicePreset> tl;
        CHECK(MigrateLegacySettings(layered, wl, &tl, &errors)
              && wl.effect.layers.size() == 1
              && wl.effect.layers[0].primitive == "wave"
              && wl.effect.layers[0].palette.stops.size() == 2,
              "migration: effects.layers inline stack preserved");
    }
    CHECK(w.inputs.audio && w.inputs.keys && !w.inputs.screen
          && w.inputs.sens_pct == 175 && w.inputs.decay_pct == 200,
          "migration: input settings preserved");

    /* migration NEVER turns live output on */
    CHECK(w.meta.live_on_startup == false, "migration: live_on_startup off");

    /* the migrated doc is itself a valid workspace, stable on
       rewrite */
    StudioDocument back;
    CHECK(FromJson(ToJson(w), back, &errors),
          "migration: migrated doc round-trips");
    CHECK(ToJson(back) == ToJson(w), "migration: deterministic");

    /* an invalid legacy scene fails cleanly instead of
       half-loading */
    json bad_legacy = legacy;
    bad_legacy["scene"]["objects"][3]["mirror_of"] = "ghost";
    StudioDocument untouched = SmallWorkspace();
    std::vector<DevicePreset> t2;
    CHECK(!MigrateLegacySettings(bad_legacy, untouched, &t2, &errors)
          && untouched.meta.name == "Fixture desk",
          "migration: invalid legacy rejected, doc untouched");
}

/*---------------------------------------------------------*\
||| Effect-section validation details.                     ||
\*---------------------------------------------------------*/
static void TestEffectsSection()
{
    using namespace studio;
    const json good = ToJson(SmallWorkspace());
    std::vector<std::string> errors, warnings;

    {
        json j = good;
        j["effects"]["speed"] = -1.0;
        StudioDocument doc;
        CHECK(!FromJson(j, doc, &errors) && HasError(errors, "speed"),
              "effects: non-positive speed rejected");
    }
    {
        json j = good;
        j["effects"]["intensity"] = 2.0;
        StudioDocument doc;
        CHECK(!FromJson(j, doc, &errors) && HasError(errors, "intensity"),
              "effects: intensity out of range rejected");
    }
    {
        json j = good;
        j["effects"]["preset"] = "no_such_look";
        StudioDocument doc;
        errors.clear(); warnings.clear();
        CHECK(FromJson(j, doc, &errors, &warnings)
              && HasError(warnings, "preset"),
              "effects: unknown preset warns, doc still loads");
    }
    {
        json j = good;
        j["effects"]["playing"] = "yes";
        StudioDocument doc;
        CHECK(!FromJson(j, doc, &errors) && HasError(errors, "playing"),
              "effects: playing wrong type rejected");
    }

    /* effects.seed is a uint32 — remix() spans the whole unsigned
       range, so seeds >= 2^31 must survive the round-trip. */
    {
        json j = good;
        j["effects"]["seed"] = 0xFFFFFFFFu;
        StudioDocument doc;
        CHECK(FromJson(j, doc, &errors)
              && doc.effect.seed == 0xFFFFFFFFu,
              "effects: seed 0xFFFFFFFF loads");
        CHECK(ToJson(doc)["effects"]["seed"] == 0xFFFFFFFFu,
              "effects: seed 0xFFFFFFFF re-serializes");
    }
    {
        json j = good;
        j["effects"]["seed"] = 3000000000u;
        StudioDocument doc;
        CHECK(FromJson(j, doc, &errors)
              && doc.effect.seed == 3000000000u,
              "effects: seed > 2^31 loads");
    }
    {
        json j = good;
        j["effects"]["seed"] = 4294967296ull;
        StudioDocument doc;
        errors.clear();
        CHECK(!FromJson(j, doc, &errors) && HasError(errors, "seed"),
              "effects: seed > UINT32_MAX rejected");
    }
    {
        json j = good;
        j["effects"]["seed"] = -5;
        StudioDocument doc;
        errors.clear();
        CHECK(!FromJson(j, doc, &errors) && HasError(errors, "seed"),
              "effects: negative seed rejected");
    }
    {
        json j = good;
        j["effects"]["seed"] = 5000000000ll;
        StudioDocument doc;
        errors.clear();
        CHECK(!FromJson(j, doc, &errors) && HasError(errors, "seed"),
              "effects: signed seed > UINT32_MAX rejected");
    }
    {
        json j = good;
        j["effects"]["seed"] = json::parse("5000000000");
        StudioDocument doc;
        errors.clear();
        CHECK(!FromJson(j, doc, &errors) && HasError(errors, "seed"),
              "effects: text 5000000000 seed rejected");
    }

    /* effects.layers is the user's authored inline layer stack —
       typed + validated resolved literals (milestone 5), no
       longer retained verbatim. */
    {
        json j = good;
        j["effects"]["layers"] = json::array({
            {{"primitive", "wave"}, {"speed", 2.0},
             {"palette", json::array({
                 {{"pos", 0.0}, {"color", "#FF0000"}},
                 {{"pos", 0.5}, {"color", "#00FF00"}}})}},
        });
        StudioDocument doc;
        errors.clear();
        CHECK(FromJson(j, doc, &errors) && errors.empty()
              && doc.effect.layers.size() == 1
              && doc.effect.layers[0].primitive == "wave"
              && doc.effect.layers[0].palette.stops.size() == 2,
              "effects: layers parse typed");
        const json out_layers = ToJson(doc)["effects"]["layers"];
        CHECK(out_layers.size() == 1
              && out_layers[0]["primitive"] == "wave"
              && out_layers[0]["speed"] == 2.0
              && out_layers[0]["palette"].size() == 2,
              "effects: layers survive re-serialization");
    }
    {
        /* Unknown layer keys and remix specs are errors now —
           the resolved stack is typed, not an extension point. */
        json j = good;
        j["effects"]["layers"] = json::array({
            {{"primitive", "wave"}, {"custom", {{"x", 1}}}},
        });
        StudioDocument doc;
        errors.clear();
        CHECK(!FromJson(j, doc, &errors) && HasError(errors, "layers"),
              "effects: unknown layer key rejected");

        j = good;
        j["effects"]["layers"] = json::array({
            {{"primitive", "wave"},
             {"speed", {{"remix", {0.1, 0.2}}}}},
        });
        errors.clear();
        CHECK(!FromJson(j, doc, &errors) && HasError(errors, "layers"),
              "effects: remix spec in layers rejected");
    }
    {
        json j = good;
        j["effects"]["layers"] = json::object();
        StudioDocument doc;
        errors.clear();
        CHECK(!FromJson(j, doc, &errors) && HasError(errors, "layers"),
              "effects: layers wrong type rejected");
    }
    {
        json j = good;
        j["effects"].erase("layers");
        StudioDocument doc;
        CHECK(FromJson(j, doc, &errors)
              && ToJson(doc)["effects"]["layers"].empty(),
              "effects: missing layers writes []");
    }
}

/*---------------------------------------------------------*\
||| Size limits: malformed community content must not be   ||
||| able to destabilize the host.                          ||
\*---------------------------------------------------------*/
static void TestDocumentLimits()
{
    using namespace studio;
    const json good = ToJson(SmallWorkspace());
    std::vector<std::string> errors;

    {
        json j = good;
        for(int i = 0; i < 2100; i++)
        {
            j["devices"]["pad_" + std::to_string(i)] =
                {{"type", "fan-120"}};
        }
        StudioDocument doc;
        CHECK(!FromJson(j, doc, &errors),
              "limits: device count capped");
    }
    {
        json j = good;
        for(int i = 0; i < 9000; i++)
        {
            j["colors"]["objects"]["k_" + std::to_string(i)] = "#FF0000";
        }
        StudioDocument doc;
        CHECK(!FromJson(j, doc, &errors),
              "limits: color key count capped");
    }
}

/*---------------------------------------------------------*\
||| The default desk end-to-end: compact workspace ->      ||
||| resolved scene -> compact save text contains no        ||
||| expanded content.                                      ||
\*---------------------------------------------------------*/
static void TestDefaultWorkspace()
{
    using namespace studio;

    PresetRegistry reg;
    reg.SetDefaults(PackagedPresets());

    StudioDocument w = BuildDefaultWorkspace();
    std::vector<std::string> errors;
    SceneDocument resolved;
    CHECK(ResolveScene(w, reg, resolved, &errors),
          "default: workspace resolves");
    CHECK(resolved.objects.size() > 20,
          "default: instances expand");

    const json dj = ToJson(w);
    bool expanded = dj.contains("entities") || dj.contains("emitters")
                    || dj.contains("definitions") || dj.contains("scene");
    for(const auto& kv : dj["devices"].items())
    {
        if(kv.value().contains("entities") || kv.value().contains("emitters")
           || kv.value().contains("geometry"))
        {
            expanded = true;
        }
    }
    CHECK(!expanded, "default: compact text has no expanded content");
}

/*---------------------------------------------------------*\
||| ui.favorites — pinned device-type ids. Order kept,     ||
||| deduped; malformed entries drop with a warning         ||
||| (recoverable config-field behavior). UI state only —   ||
||| never undoable, never part of the scene.               ||
\*---------------------------------------------------------*/
static void TestUiFavorites()
{
    using namespace studio;
    std::vector<std::string> errors, warnings;
    StudioDocument doc;

    {
        json j = {{"schema_version", STUDIO_SCHEMA_VERSION}};
        j["ui"] = {
            {"favorites", { "fan-120", "ram-stick", "fan-120",
                            "gpu-fan" }},
        };
        CHECK(FromJson(j, doc, &errors, &warnings)
              && doc.meta.ui.favorites.size() == 3
              && doc.meta.ui.favorites[0] == "fan-120"
              && doc.meta.ui.favorites[1] == "ram-stick"
              && doc.meta.ui.favorites[2] == "gpu-fan",
              "favorites: order kept, duplicates collapse");
        const json back = ToJson(doc);
        CHECK(back["ui"]["favorites"].size() == 3
              && back["ui"]["favorites"][1] == "ram-stick",
              "favorites: serialized");
        StudioDocument doc2;
        errors.clear(); warnings.clear();
        CHECK(FromJson(back, doc2, &errors, &warnings)
              && doc2.meta.ui.favorites.size() == 3
              && doc2.meta.ui.favorites[2] == "gpu-fan",
              "favorites: round-trip");
    }
    /* non-string entries and bad identifiers drop with a
       warning — the doc still parses. */
    {
        json j = {{"schema_version", STUDIO_SCHEMA_VERSION}};
        j["ui"] = {
            {"favorites", { "fan-120", 42, "bad id!", json::object(),
                            "gpu-fan" }},
        };
        warnings.clear();
        CHECK(FromJson(j, doc, &errors, &warnings)
              && doc.meta.ui.favorites.size() == 2
              && doc.meta.ui.favorites[0] == "fan-120"
              && doc.meta.ui.favorites[1] == "gpu-fan",
              "favorites: bad entries dropped");
        CHECK(HasError(warnings, "favorites[1]")
              && HasError(warnings, "favorites[2]")
              && HasError(warnings, "favorites[3]"),
              "favorites: drops reported as warnings");
    }
    /* a non-array favorites field is dropped wholesale */
    {
        json j = {{"schema_version", STUDIO_SCHEMA_VERSION}};
        j["ui"] = {{"favorites", "fan-120"}};
        warnings.clear();
        CHECK(FromJson(j, doc, &errors, &warnings)
              && doc.meta.ui.favorites.empty()
              && HasError(warnings, "favorites"),
              "favorites: non-array dropped with warning");
    }
    /* absent favorites stays absent — no noise in the file */
    {
        doc = StudioDocument{};
        CHECK(!ToJson(doc)["ui"].contains("favorites"),
              "favorites: omitted when empty");
    }
}

/*---------------------------------------------------------*\
||| Task 6.2 — committed scene fixtures: the spec's two   ||
||| workloads (tests/fixtures/scenes/<name>/studio.json)  ||
||| must parse as compact v3, resolve against the packaged||
||| type library, and hit their instance/emitter targets. ||
||| They are also the perf harness's input — a fixture    ||
||| that stops validating breaks the measurement.         ||
\*---------------------------------------------------------*/
static void TestSceneFixtures()
{
    using namespace studio;
    namespace fs = std::filesystem;

    const fs::path candidates[2] = {
        fs::path("fixtures") / "scenes",
        fs::path("..") / "fixtures" / "scenes",
    };
    fs::path root;
    for(const fs::path& c : candidates)
    {
        if(fs::is_directory(c))
        {
            root = c;
            break;
        }
    }
    CHECK(!root.empty(), "fixtures: scene dir found");
    if(root.empty())
    {
        return;
    }

    PresetRegistry reg;
    reg.SetDefaults(PackagedPresets());

    const struct { const char* id; size_t devices; size_t min_emit;
                   size_t max_emit; }
    want[] = {
        { "normal", 30, 900, 1100 },
        { "stress", 100, 3600, 5000 },
    };
    for(const auto& fx : want)
    {
        const fs::path p = root / fx.id / "studio.json";
        std::ifstream f(p, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        json j;
        try
        {
            j = json::parse(ss.str());
        }
        catch(...)
        {
            CHECK(false, (std::string("fixtures: ") + fx.id
                          + " parses").c_str());
            continue;
        }
        StudioDocument w;
        std::vector<std::string> errors;
        CHECK(FromJson(j, w, &errors),
              (std::string("fixtures: ") + fx.id
               + " validates v3").c_str());
        CHECK(w.devices.size() == fx.devices
              && !w.meta.live_on_startup,
              (std::string("fixtures: ") + fx.id
               + " instance count, no live output").c_str());
        SceneDocument resolved;
        errors.clear();
        CHECK(ResolveScene(w, reg, resolved, &errors),
              (std::string("fixtures: ") + fx.id
               + " resolves").c_str());
        size_t emitters = 0;
        for(const SceneObject& o : resolved.objects)
        {
            emitters += o.emitters.size();
        }
        CHECK(emitters >= fx.min_emit && emitters <= fx.max_emit,
              (std::string("fixtures: ") + fx.id
               + " emitter workload").c_str());
        std::printf("  (fixture %s: %zu devices, %zu objects,"
                    " %zu emitters)\n",
                    fx.id, w.devices.size(), resolved.objects.size(),
                    emitters);
    }
}

int main()
{
    TestWorkspaceRoundTrip();
    TestWorkspaceValidation();
    TestHexColors();
    TestLegacyMigration();
    TestEffectsSection();
    TestDocumentLimits();
    TestDefaultWorkspace();
    TestUiFavorites();
    TestSceneFixtures();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
