/*---------------------------------------------------------*\
||| device_preset_test.cpp                                    |
|||                                                           |
|||   Qt-free tests for the v3 external-type stack:        ||
|||   presets/DevicePreset (type JSON + validation),       ||
|||   presets/PresetRegistry (dir load, id match, dep      ||
|||   cycles, defaults fallback), scene/SceneResolver      ||
|||   (compact workspace -> expanded SceneDocument), and   ||
|||   config/ConfigMigration (v2 -> v3 extraction).        ||
|||   Plain cl build via build-studio-tests.bat.           ||
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "scene/SceneTypes.h"
#include "scene/SceneGraph.h"
#include "scene/DefaultDesk.h"
#include "scene/SceneJson.h"
#include "scene/SceneResolver.h"
#include "presets/DevicePreset.h"
#include "presets/PresetRegistry.h"
#include "presets/PresetBundle.h"
#include "config/StudioConfig.h"
#include "config/ConfigMigration.h"
#include "effects/EffectTypes.h"
#include "effects/Presets.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using nlohmann::json;
using namespace studio;

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
||| Fixtures                                                 ||
\*---------------------------------------------------------*/
static json FanPreset(const std::string& id, int leds = 8)
{
    return {
        {"$schema", "../../schemas/device.schema.json"},
        {"schema_version", 1},
        {"id", id},
        {"name", "Test fan"},
        {"category", "fan"},
        {"entities", {
            {"frame", {
                {"geometry", "fan_frame"},
                {"size_m", {0.12, 0.025, 0.12}},
                {"x", 0}, {"y", 0}, {"z", 0},
                {"rx", 0}, {"ry", 0}, {"rz", 0},
                {"appearance", {{"body_color", "#202028"}}},
            }},
            {"diffuser", {
                {"geometry", "fan_ring"},
                {"size_m", {0.11, 0.004, 0.11}},
                {"x", 0}, {"y", 0.013}, {"z", 0},
                {"rx", 0}, {"ry", 0}, {"rz", 0},
                {"zone", "ring"},
            }},
        }},
        {"zones", json::array({
            {
                {"id", "ring"},
                {"entity", "diffuser"},
                {"led_count", leds},
                {"layout", {
                    {"type", "ring"},
                    {"radius_m", 0.052},
                    {"start_angle_deg", 0},
                    {"reverse", false},
                }},
            },
        })},
    };
}

/* A 2x3 static-matrix device — the layout GenerateZoneEmitters
   indexes as map[row*cols+col]. */
static json MatrixPreset(const std::string& id)
{
    return {
        {"schema_version", 1},
        {"id", id},
        {"name", "Matrix pad"},
        {"category", "test"},
        {"entities", {
            {"body", {
                {"geometry", "pad_body"},
                {"size_m", {0.06, 0.01, 0.04}},
                {"x", 0}, {"y", 0}, {"z", 0},
                {"rx", 0}, {"ry", 0}, {"rz", 0},
                {"zone", "mx"},
            }},
        }},
        {"zones", json::array({
            {
                {"id", "mx"},
                {"entity", "body"},
                {"led_count", 5},
                {"layout", {
                    {"type", "matrix"},
                    {"rows", 2},
                    {"cols", 3},
                    {"pitch_x_m", 0.019},
                    {"pitch_z_m", 0.019},
                    /* one empty cell -> 5 emitters */
                    {"map", {0, 1, 4294967295u, 2, 3, 4}},
                }},
            },
        })},
    };
}

static std::filesystem::path TempDir(const std::string& name)
{
    const auto dir = std::filesystem::temp_directory_path()
                     / ("dls_preset_test_" + name);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

static void WriteFile(const std::filesystem::path& p, const json& j)
{
    std::ofstream f(p, std::ios::binary);
    f << j.dump(2) << "\n";
}

static void WriteRaw(const std::filesystem::path& p, const std::string& s)
{
    std::ofstream f(p, std::ios::binary);
    f << s;
}

static json ReadJson(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return json::parse(f);
}

static size_t CountTypeFiles(const std::filesystem::path& dir)
{
    size_t n = 0;
    std::error_code ec;
    for(const auto& e : std::filesystem::directory_iterator(dir, ec))
    {
        if(e.is_regular_file()
           && e.path().filename().string().find(".device.json")
                  != std::string::npos)
        {
            n++;
        }
    }
    return n;
}

/* A type whose only entity mounts another type — a nested
   assembly used to exercise transitive-dep collection and
   nested `type` ref remapping. */
static json RefPreset(const std::string& id, const std::string& dep)
{
    return {
        {"schema_version", 1},
        {"id", id},
        {"name", id},
        {"category", "test"},
        {"entities", {
            {"mount", {
                {"type", dep},
                {"x", 0}, {"y", 0}, {"z", 0},
                {"rx", 0}, {"ry", 0}, {"rz", 0},
            }},
        }},
        {"zones", json::array()},
    };
}

/* The bundled type library (the authoritative defaults layer the
   plugin loads from the qrc; the test reads the same files from
   the source tree). The suite runs from tests/. */
static std::filesystem::path PackagedPresetDir()
{
    if(const char* env = std::getenv("DLS_PRESET_DIR"))
    {
        const std::filesystem::path p(env);
        std::error_code ec;
        if(!p.empty() && std::filesystem::is_directory(p, ec))
        {
            return p;
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
    for(const auto& c : candidates)
    {
        std::error_code ec;
        if(std::filesystem::is_directory(c, ec))
        {
            return c;
        }
    }
    return candidates[0];
}

static StudioDocument TwoFanWorkspace();   /* defined below */

static std::vector<DevicePreset> PackagedPresets()
{
    std::vector<DevicePreset> out;
    std::error_code ec;
    for(const auto& e :
        std::filesystem::directory_iterator(PackagedPresetDir(), ec))
    {
        DevicePreset p;
        if(e.is_regular_file()
           && DevicePresetFromJsonFile(e.path().string(), p, nullptr))
        {
            out.push_back(p);
        }
    }
    return out;
}

/* World-space emitter positions: object world matrix x emitter
   local position — the transform the effects engine renders. */
static std::vector<Vec3> WorldEmitters(const SceneDocument& doc,
                                       const std::string& object_id)
{
    std::vector<Vec3> out;
    const SceneObject* o = FindObject(doc, object_id);
    if(o == nullptr)
    {
        return out;
    }
    const auto worlds = ResolveWorldMatrices(doc);
    const auto it = worlds.find(object_id);
    if(it == worlds.end())
    {
        return out;
    }
    for(const Emitter& e : o->emitters)
    {
        out.push_back(TransformPoint(it->second, e.local_pos));
    }
    return out;
}

/*---------------------------------------------------------*\
||| DevicePreset — parse/validate/serialize.               ||
\*---------------------------------------------------------*/
static void TestPresetBasics()
{
    DevicePreset p;
    std::vector<std::string> errors;
    CHECK(DevicePresetFromJson(FanPreset("fan-120"), p, &errors),
          "preset: fan-120 parses");
    CHECK(p.id == "fan-120" && p.entities.size() == 2
          && p.zones.size() == 1 && p.zones[0].led_count == 8,
          "preset: fields populated");
    CHECK(errors.empty(), "preset: no errors on valid doc");

    /* deterministic round-trip */
    const json j1 = ToJson(p);
    DevicePreset back;
    CHECK(DevicePresetFromJson(j1, back, &errors),
          "preset: serialized form re-parses");
    CHECK(ToJson(back) == j1, "preset: deterministic serialization");

    /* emitter generation: 8 points on a ring in the XZ plane */
    const std::vector<Emitter> em =
        GenerateZoneEmitters(p.zones[0], "grp", 0);
    CHECK(em.size() == 8, "preset: ring generates 8 emitters");
    bool on_ring = true;
    for(const Emitter& e : em)
    {
        const float r = std::sqrt(e.local_pos.x * e.local_pos.x
                                  + e.local_pos.z * e.local_pos.z);
        if(!Near(r, 0.052f, 1e-3f)) { on_ring = false; }
        if(e.group != "grp")        { on_ring = false; }
        if(e.address < 0 || e.address > 7) { on_ring = false; }
    }
    CHECK(on_ring, "preset: ring emitter positions + addresses");

    /* addr_base shifts generated addresses */
    const std::vector<Emitter> em2 =
        GenerateZoneEmitters(p.zones[0], "grp", 8);
    CHECK(em2[0].address == 8 && em2[7].address == 15,
          "preset: addr_base shifts addresses");
}

static void TestPresetValidation()
{
    std::vector<std::string> errors;
    DevicePreset p;

    {
        json j = FanPreset("fan-120");
        j.erase("id");
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "id"),
              "preset: missing id rejected");
    }
    {
        json j = FanPreset("fan-120");
        j["schema_version"] = 2;
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "schema_version"),
              "preset: newer schema rejected");
    }
    {
        json j = FanPreset("fan-120");
        j["id"] = "has space";
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "id"),
              "preset: id charset enforced");
    }
    {
        json j = FanPreset("fan-120");
        j["entities"]["frame"]["size_m"] = {0.12, -1.0, 0.12};
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "size_m"),
              "preset: negative size rejected");
    }
    {
        json j = FanPreset("fan-120");
        j["entities"]["diffuser"]["zone"] = "no_such_zone";
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "zone"),
              "preset: dangling entity zone rejected");
    }
    {
        json j = FanPreset("fan-120");
        j["zones"][0]["entity"] = "ghost";
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "entity"),
              "preset: dangling zone entity rejected");
    }
    {
        json j = FanPreset("fan-120");
        j["entities"]["frame"]["parent"] = "diffuser";
        j["entities"]["diffuser"]["parent"] = "frame";
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "cycle"),
              "preset: entity parent cycle rejected");
    }
    {
        json j = FanPreset("fan-120");
        j["zones"][0]["layout"]["type"] = "spiral";
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "layout"),
              "preset: unknown layout rejected");
    }
    {
        /* child type refs may not carry local-part fields */
        json j = FanPreset("fan-120");
        j["entities"]["child"] = {
            {"type", "other-type"},
            {"x", 0.1}, {"y", 0}, {"z", 0},
            {"rx", 0}, {"ry", 0}, {"rz", 0},
            {"geometry", "fan_frame"},
        };
        CHECK(!DevicePresetFromJson(j, p, &errors),
              "preset: child ref with geometry rejected");
    }
    {
        /* a valid child ref parses and reports the dependency */
        json j = FanPreset("fan-120");
        j["entities"]["child"] = {
            {"type", "other-type"},
            {"x", 0.1}, {"y", 0}, {"z", 0},
            {"rx", 0}, {"ry", 0}, {"rz", 0},
        };
        CHECK(DevicePresetFromJson(j, p, &errors),
              "preset: child ref parses");
        const auto deps = PresetDependencies(p);
        CHECK(deps.size() == 1 && deps[0] == "other-type",
              "preset: dependency reported");
    }
    {
        /* failed parse leaves the candidate untouched */
        DevicePreset keep;
        CHECK(DevicePresetFromJson(FanPreset("keep-me"), keep, &errors),
              "preset: baseline parses");
        CHECK(!DevicePresetFromJson(json::object(), keep, &errors)
              && keep.id == "keep-me",
              "preset: failure leaves candidate untouched");
    }
}

/*---------------------------------------------------------*\
||| Matrix + address validation: the static-matrix map is  ||
||| indexed map[row*cols+col]; a short map or a value that ||
||| wraps int must be a file error, never an OOB read or a ||
||| negative address at generation time.                   ||
\*---------------------------------------------------------*/
static void TestMatrixValidation()
{
    std::vector<std::string> errors;
    DevicePreset p;

    /* valid static matrix parses and generates */
    CHECK(DevicePresetFromJson(MatrixPreset("matrix-dev"), p, &errors)
          && errors.empty(),
          "matrix: valid static map parses");
    if(!p.zones.empty())
    {
        const std::vector<Emitter> em =
            GenerateZoneEmitters(p.zones[0], "pad/body", 0);
        CHECK(em.size() == 5, "matrix: empty cell skipped");
        CHECK(em.size() == 5 && em[0].address == 0
              && em[2].address == 2 && em[4].address == 4,
              "matrix: addresses come from the map");
    }

    /* short map — the old hole: rows*cols indexing past the end */
    {
        json j = MatrixPreset("matrix-dev");
        j["zones"][0]["layout"]["map"] = { 0 };
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "rows*cols"),
              "matrix: short map rejected");
    }
    /* wrapping values: >= 2^31 wraps (int)led negative */
    {
        json j = MatrixPreset("matrix-dev");
        j["zones"][0]["layout"]["map"] = { 0, 1, 2147483648ll, 2, 3, 4 };
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "map["),
              "matrix: 2^31 map value rejected");
    }
    /* ... unless it IS the empty sentinel */
    {
        json j = MatrixPreset("matrix-dev");
        CHECK(DevicePresetFromJson(j, p, &errors),
              "matrix: empty sentinel allowed in map");
    }
    /* negatives */
    {
        json j = MatrixPreset("matrix-dev");
        j["zones"][0]["layout"]["map"] = { 0, 1, -5, 2, 3, 4 };
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "map["),
              "matrix: negative map value rejected");
    }
    /* empty sentinel itself must fit u32 */
    {
        json j = MatrixPreset("matrix-dev");
        j["zones"][0]["layout"]["empty"] = -1;
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "empty"),
              "matrix: negative empty sentinel rejected");
    }

    /* points.addresses: -1 is render-only, lower is invalid */
    {
        json j = FanPreset("pts");
        j["zones"][0]["led_count"] = 3;
        j["zones"][0]["layout"] = {
            {"type", "points"},
            {"points", {{0,0,0}, {0.01,0,0}, {0.02,0,0}}},
            {"addresses", {0, -5, 2}},
        };
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "addresses"),
              "points: address < -1 rejected");
    }
    {
        json j = FanPreset("pts");
        j["zones"][0]["led_count"] = 3;
        j["zones"][0]["layout"] = {
            {"type", "points"},
            {"points", {{0,0,0}, {0.01,0,0}, {0.02,0,0}}},
            {"addresses", {0, -1, 2}},
        };
        CHECK(DevicePresetFromJson(j, p, &errors),
              "points: -1 render-only address allowed");
    }
    /* Two emitters on one hardware LED is a mapping error — the
       second write silently shadows the first at push time (the
       calibration E2E persisted exactly this class of corruption). */
    {
        json j = FanPreset("pts");
        j["zones"][0]["led_count"] = 3;
        j["zones"][0]["layout"] = {
            {"type", "points"},
            {"points", {{0,0,0}, {0.01,0,0}, {0.02,0,0}}},
            {"addresses", {0, 2, 2}},
        };
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "addresses"),
              "points: duplicate addresses rejected");
    }
    /* ...but -1 pairs may repeat — render-only emitters share the
       sentinel legitimately. */
    {
        json j = FanPreset("pts");
        j["zones"][0]["led_count"] = 3;
        j["zones"][0]["layout"] = {
            {"type", "points"},
            {"points", {{0,0,0}, {0.01,0,0}, {0.02,0,0}}},
            {"addresses", {0, -1, -1}},
        };
        CHECK(DevicePresetFromJson(j, p, &errors),
              "points: repeated -1 render-only allowed");
    }
    /* Subset mapping: an address above the point count is legal —
       the bound hardware zone's real LED count is enforced at
       resolve (SceneResolver), where the binding is known. */
    {
        json j = FanPreset("pts");
        j["zones"][0]["led_count"] = 3;
        j["zones"][0]["layout"] = {
            {"type", "points"},
            {"points", {{0,0,0}, {0.01,0,0}, {0.02,0,0}}},
            {"addresses", {4, 9, 15}},
        };
        CHECK(DevicePresetFromJson(j, p, &errors),
              "points: subset addresses above count allowed");
    }

    /* led_count must agree with the generated points count when
       declared — a stale count misreports the zone. */
    {
        json j = FanPreset("pts");
        j["zones"][0]["led_count"] = 99;
        j["zones"][0]["layout"] = {
            {"type", "points"},
            {"points", {{0,0,0}, {0.01,0,0}, {0.02,0,0}}},
        };
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "led_count"),
              "points: led_count must match points count");
    }
    {
        json j = FanPreset("pts");
        j["zones"][0]["led_count"] = 3;
        j["zones"][0]["layout"] = {
            {"type", "points"},
            {"points", {{0,0,0}, {0.01,0,0}, {0.02,0,0}}},
        };
        CHECK(DevicePresetFromJson(j, p, &errors),
              "points: matching led_count accepted");
    }

    /* Resolver-side: a programmatic preset can carry an invalid
       address the file parser would have caught — it must still
       fail at resolve, not die silently in PushZone's clamp. */
    {
        DevicePreset bad;
        bad.id       = "bad-addr";
        bad.name     = "Bad addresses";
        bad.category = "test";
        PresetEntity e;
        e.id       = "body";
        e.geometry = "pad_body";
        e.zone     = "pts";
        bad.entities["body"] = e;
        DeviceZone z;
        z.id        = "pts";
        z.entity    = "body";
        z.led_count = 2;
        z.layout.type = "points";
        z.layout.points = { {0,0,0}, {0.01f,0,0} };
        z.layout.addresses = { 0, -5 };
        bad.zones.push_back(z);
        PresetRegistry reg;
        reg.SetDefaults({ bad });
        StudioDocument w;
        DeviceInstance inst;
        inst.type = "bad-addr";
        w.devices["pad"] = inst;
        w.device_settings["pad"].zones["pts"].binding = "bus";
        DeviceBinding b;
        b.id = "bus"; b.zone_leds = 8;
        w.bindings["bus"] = b;
        SceneDocument doc;
        errors.clear();
        CHECK(!ResolveScene(w, reg, doc, &errors)
              && HasError(errors, "invalid"),
              "resolve: address < -1 rejected");
    }
}

/*---------------------------------------------------------*\
||| Emitter generation — geometry and LED ordering for   ||
||| every layout kind. Address order always walks the    ||
||| physical LED chain; layout params only move where    ||
||| each address sits in space.                          ||
\*---------------------------------------------------------*/
static void TestEmitterGeneration()
{
    std::vector<std::string> errors;
    DevicePreset p;

    /*--- ring: count, radius circle, face plane, angle knobs ---*/
    {
        json j = FanPreset("ring-dev", 4);
        j["zones"][0]["layout"]["radius_m"] = 0.1;
        j["zones"][0]["layout"]["face_y_m"] = 0.02;
        CHECK(DevicePresetFromJson(j, p, &errors), "gen: ring parses");
        const std::vector<Emitter> em =
            GenerateZoneEmitters(p.zones[0], "g", 0);
        /* angle 0 = +X; CCW seen from +Y maps +angle to -z. */
        CHECK(em.size() == 4
              && Near(em[0].local_pos.x, 0.1f)
              && Near(em[0].local_pos.z, 0.0f)
              && Near(em[1].local_pos.x, 0.0f)
              && Near(em[1].local_pos.z, -0.1f)
              && Near(em[2].local_pos.x, -0.1f)
              && Near(em[3].local_pos.z, 0.1f)
              && Near(em[0].local_pos.y, 0.02f)
              && em[0].address == 0 && em[3].address == 3,
              "gen: ring positions on the face plane");
    }
    {
        /* start_angle_deg rotates where LED 0 sits */
        json j = FanPreset("ring-dev", 4);
        j["zones"][0]["layout"]["radius_m"]        = 0.1;
        j["zones"][0]["layout"]["start_angle_deg"] = 90.0;
        CHECK(DevicePresetFromJson(j, p, &errors),
              "gen: start-angle ring parses");
        const std::vector<Emitter> em =
            GenerateZoneEmitters(p.zones[0], "g", 0);
        CHECK(em.size() == 4
              && Near(em[0].local_pos.x, 0.0f)
              && Near(em[0].local_pos.z, -0.1f)
              && Near(em[1].local_pos.x, -0.1f),
              "gen: start_angle_deg moves LED 0");
    }
    {
        /* reverse walks the addresses the other way around the
           circle — same positions, opposite index order. */
        json j = FanPreset("ring-dev", 4);
        j["zones"][0]["layout"]["radius_m"] = 0.1;
        j["zones"][0]["layout"]["reverse"]  = true;
        CHECK(DevicePresetFromJson(j, p, &errors), "gen: reverse parses");
        const std::vector<Emitter> em =
            GenerateZoneEmitters(p.zones[0], "g", 0);
        CHECK(em.size() == 4
              && Near(em[0].local_pos.x, 0.1f)
              && Near(em[1].local_pos.z, 0.1f)
              && Near(em[3].local_pos.z, -0.1f)
              && em[1].address == 1 && em[3].address == 3,
              "gen: reverse flips the LED walk");
    }

    /*--- strip: +X march from origin, addr_base offsets ---*/
    {
        json j = FanPreset("strip-dev");
        j["zones"][0]["led_count"] = 5;
        j["zones"][0]["layout"] = {
            {"type",      "strip"},
            {"spacing_m", 0.01},
            {"origin",    {0.05, 0.01, -0.02}},
        };
        CHECK(DevicePresetFromJson(j, p, &errors), "gen: strip parses");
        const std::vector<Emitter> em =
            GenerateZoneEmitters(p.zones[0], "s", 10);
        bool ok = em.size() == 5;
        for(size_t i = 0; i < em.size() && ok; i++)
        {
            ok = Near(em[i].local_pos.x, 0.05f + 0.01f * (float)i)
                 && Near(em[i].local_pos.y, 0.01f)
                 && Near(em[i].local_pos.z, -0.02f)
                 && em[i].address == 10 + (int)i;
        }
        CHECK(ok, "gen: strip spacing, origin + addr_base");
    }

    /*--- matrix: map[row*cols+col] gives the address sitting at
          cell (row,col); emitters come out in row-major order,
          empty cells skipped. ---*/
    {
        CHECK(DevicePresetFromJson(MatrixPreset("mx"), p, &errors),
              "gen: matrix parses");
        const std::vector<Emitter> em =
            GenerateZoneEmitters(p.zones[0], "pad", 0);
        /* map {0,1,empty,2,3,4} on a 2x3 grid, pitch 0.019. */
        bool ok = em.size() == 5;
        if(ok)
        {
            ok = em[0].address == 0
                 && Near(em[0].local_pos.x, 0.0f)
                 && Near(em[0].local_pos.z, 0.0f)
              && em[2].address == 2      /* row 1, col 0 */
                 && Near(em[2].local_pos.x, 0.0f)
                 && Near(em[2].local_pos.z, -0.019f)
              && em[4].address == 4      /* row 1, col 2 */
                 && Near(em[4].local_pos.x, 0.038f)
                 && Near(em[4].local_pos.z, -0.019f);
        }
        CHECK(ok, "gen: matrix map order -> cell positions");
    }
    {
        /* dynamic matrix: no emitters — rebuilt from the bound
           zone's hardware map at runtime. */
        json j = FanPreset("dyn");
        j["zones"][0]["led_count"] = 0;
        j["zones"][0]["layout"] = {
            {"type", "matrix"}, {"dynamic", true},
        };
        CHECK(DevicePresetFromJson(j, p, &errors), "gen: dynamic parses");
        CHECK(GenerateZoneEmitters(p.zones[0], "d", 0).empty(),
              "gen: dynamic matrix emits nothing");
    }

    /*--- points: explicit order preserved; addresses optional ---*/
    {
        json j = FanPreset("pts-dev");
        j["zones"][0]["led_count"] = 3;
        j["zones"][0]["layout"] = {
            {"type", "points"},
            /* deliberately unsorted — order IS the LED order */
            {"points", {{0.03, 0, 0}, {0.01, 0, 0}, {0.02, 0, 0}}},
        };
        CHECK(DevicePresetFromJson(j, p, &errors), "gen: points parses");
        const std::vector<Emitter> em =
            GenerateZoneEmitters(p.zones[0], "pt", 7);
        CHECK(em.size() == 3
              && Near(em[0].local_pos.x, 0.03f)
              && Near(em[1].local_pos.x, 0.01f)
              && Near(em[2].local_pos.x, 0.02f)
              && em[0].address == 7 && em[2].address == 9,
              "gen: points keep file order, addr_base offsets");
    }
    {
        json j = FanPreset("pts-dev");
        j["zones"][0]["led_count"] = 3;
        j["zones"][0]["layout"] = {
            {"type", "points"},
            {"points", {{0, 0, 0}, {0.01, 0, 0}, {0.02, 0, 0}}},
            {"addresses", {5, -1, 9}},
        };
        CHECK(DevicePresetFromJson(j, p, &errors),
              "gen: explicit addresses parse");
        const std::vector<Emitter> em =
            GenerateZoneEmitters(p.zones[0], "pt", 7);
        CHECK(em.size() == 3 && em[0].address == 5
              && em[1].address == -1 && em[2].address == 9,
              "gen: explicit addresses win over addr_base");
    }
}

/*---------------------------------------------------------*\
||| Invalid zone sizes — led_count vs layout, degenerate ||
||| parameters. A bad size must be a file error at parse,||
||| never a silent zero-emitter zone or a generator that ||
||| divides by a zero count.                             ||
\*---------------------------------------------------------*/
static void TestZoneSizeValidation()
{
    std::vector<std::string> errors;
    DevicePreset p;

    /* ring/strip are static generators — led_count 0 is reserved
       for dynamic matrix zones. */
    {
        json j = FanPreset("no-leds");
        j["zones"][0]["led_count"] = 0;
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "led_count"),
              "size: zero-LED ring rejected");
    }
    {
        json j = FanPreset("no-leds");
        j["zones"][0]["led_count"] = 0;
        j["zones"][0]["layout"] = {
            {"type", "strip"}, {"spacing_m", 0.01},
        };
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "led_count"),
              "size: zero-LED strip rejected");
    }
    /* negative led_count */
    {
        json j = FanPreset("neg-leds");
        j["zones"][0]["led_count"] = -4;
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "led_count"),
              "size: negative led_count rejected");
    }
    /* zero/negative radius + spacing */
    {
        json j = FanPreset("flat-ring");
        j["zones"][0]["layout"]["radius_m"] = 0.0;
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "radius_m"),
              "size: zero radius rejected");
        j["zones"][0]["layout"]["radius_m"] = -0.5;
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "radius_m"),
              "size: negative radius rejected");
    }
    {
        json j = FanPreset("flat-strip");
        j["zones"][0]["led_count"] = 4;
        j["zones"][0]["layout"] = {
            {"type", "strip"}, {"spacing_m", 0.0},
        };
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "spacing_m"),
              "size: zero spacing rejected");
        j["zones"][0]["layout"]["spacing_m"] = -0.01;
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "spacing_m"),
              "size: negative spacing rejected");
    }
    /* empty points list */
    {
        json j = FanPreset("no-points");
        j["zones"][0]["led_count"] = 0;
        j["zones"][0]["layout"] = {
            {"type", "points"}, {"points", json::array()},
        };
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "points"),
              "size: empty points rejected");
    }
    /* static matrix: led_count must equal the non-empty cell
       count — the generated emitter count, like the points rule. */
    {
        json j = MatrixPreset("mx-bad");
        j["zones"][0]["led_count"] = 4;   /* map has 5 live cells */
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "led_count"),
              "size: static matrix led_count mismatch rejected");
    }
    {
        json j = MatrixPreset("mx-bad");
        j["zones"][0]["led_count"] = 0;
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "led_count"),
              "size: static matrix led_count 0 rejected");
    }
    /* matrix rows*cols beyond the map is already covered
       (short map); rows/cols of 0 with a map is the same hole. */
    {
        json j = MatrixPreset("mx-flat");
        j["zones"][0]["layout"]["rows"] = 0;
        CHECK(!DevicePresetFromJson(j, p, &errors),
              "size: static matrix without rows rejected");
    }
    /* a 1x2 matrix with a custom empty sentinel */
    {
        json j = MatrixPreset("mx-sent");
        j["zones"][0]["led_count"] = 1;
        j["zones"][0]["layout"]["rows"]  = 1;
        j["zones"][0]["layout"]["cols"]  = 2;
        j["zones"][0]["layout"]["empty"] = 77;
        j["zones"][0]["layout"]["map"]   = {0, 77};
        CHECK(DevicePresetFromJson(j, p, &errors),
              "size: custom empty sentinel accepted");
        const std::vector<Emitter> em =
            GenerateZoneEmitters(p.zones[0], "m", 0);
        CHECK(em.size() == 1 && em[0].address == 0,
              "size: sentinel cell skipped at generation");
    }
}

/*---------------------------------------------------------*\
||| Effect targets: every WithTargets string in the        ||
||| built-in presets must resolve to at least one emitter- ||
||| bearing object in the default workspace. A dead target ||
||| is a silently inert layer.                             ||
\*---------------------------------------------------------*/
static void TestEffectTargetsResolve()
{
    PresetRegistry reg;
    reg.SetDefaults(PackagedPresets());
    const StudioDocument w = BuildDefaultWorkspace();
    SceneDocument doc;
    std::vector<std::string> errors;
    CHECK(ResolveScene(w, reg, doc, &errors),
          "targets: default desk resolves");

    int checked = 0;
    for(const PresetInfo& info : PresetList())
    {
        for(const EffectLayer& L : BuildPreset(info.id, 0))
        {
            for(const std::string& t : L.targets)
            {
                ++checked;
                /* Match through the real matcher; emitters use their
                   object id as group, and matrix_map objects get
                   their emitters at runtime — count them as
                   emitter-bearing. */
                EffectLayer one;
                one.targets = { t };
                bool hit = false;
                for(const SceneObject& o : doc.objects)
                {
                    if(o.emitters.empty() && o.layout != "matrix_map")
                    {
                        continue;
                    }
                    Emitter probe;
                    probe.group = o.id;
                    if(LayerMatches(one, o, probe))
                    {
                        hit = true;
                        break;
                    }
                }
                const std::string name =
                    "targets: '" + t + "' resolves (" + info.id + ")";
                CHECK(hit, name.c_str());
            }
        }
    }
    CHECK(checked >= 5, "targets: presets carry targeted layers");
}

/*---------------------------------------------------------*\
||| PresetRegistry — directory load, id match, deps.       ||
\*---------------------------------------------------------*/
static void TestRegistry()
{
    std::vector<std::string> errors;

    {
        const auto dir = TempDir("basic");
        WriteFile(dir / "fan-120.device.json", FanPreset("fan-120"));
        WriteFile(dir / "fan-80.device.json",  FanPreset("fan-80"));

        PresetRegistry reg;
        CHECK(reg.LoadDirectory(dir.string(), &errors),
              "registry: clean dir loads");
        CHECK(reg.FileCount() == 2 && reg.Contains("fan-120")
              && reg.Contains("fan-80"),
              "registry: types resolvable");
    }
    {
        /* id must equal the file basename — never pick whichever
           file happened to define the id first. */
        const auto dir = TempDir("mismatch");
        WriteFile(dir / "fan-120.device.json", FanPreset("other-id"));
        PresetRegistry reg;
        CHECK(!reg.LoadDirectory(dir.string(), &errors)
              && HasError(errors, "does not match"),
              "registry: id/filename mismatch rejected");
        CHECK(!reg.Contains("other-id") && !reg.Contains("fan-120"),
              "registry: mismatched type excluded");
    }
    {
        /* missing dependency is removed and reported */
        const auto dir = TempDir("missing-dep");
        json j = FanPreset("assembly");
        j["entities"]["child"] = {
            {"type", "ghost-type"},
            {"x", 0}, {"y", 0}, {"z", 0},
            {"rx", 0}, {"ry", 0}, {"rz", 0},
        };
        WriteFile(dir / "assembly.device.json", j);
        WriteFile(dir / "fan-120.device.json", FanPreset("fan-120"));
        PresetRegistry reg;
        CHECK(!reg.LoadDirectory(dir.string(), &errors)
              && HasError(errors, "missing dependency"),
              "registry: missing dependency reported");
        CHECK(!reg.Contains("assembly") && reg.Contains("fan-120"),
              "registry: dependent removed, sibling kept");
    }
    {
        /* a recursive reference cycle is rejected */
        const auto dir = TempDir("cycle");
        json a = FanPreset("type-a");
        a["entities"]["child"] = {
            {"type", "type-b"},
            {"x", 0}, {"y", 0}, {"z", 0},
            {"rx", 0}, {"ry", 0}, {"rz", 0},
        };
        json b = FanPreset("type-b");
        b["entities"]["child"] = {
            {"type", "type-a"},
            {"x", 0}, {"y", 0}, {"z", 0},
            {"rx", 0}, {"ry", 0}, {"rz", 0},
        };
        WriteFile(dir / "type-a.device.json", a);
        WriteFile(dir / "type-b.device.json", b);
        PresetRegistry reg;
        CHECK(!reg.LoadDirectory(dir.string(), &errors)
              && HasError(errors, "cycle"),
              "registry: reference cycle rejected");
        CHECK(!reg.Contains("type-a") && !reg.Contains("type-b"),
              "registry: cyclic types removed");
    }
    {
        /* defaults fill in underneath the file layer */
        PresetRegistry reg;
        reg.SetDefaults(PackagedPresets());
        CHECK(reg.Contains("fan-120") && reg.Contains("group"),
              "registry: packaged defaults resolvable");
        CHECK(reg.LoadDirectory(TempDir("empty").string(), &errors),
              "registry: empty dir is not an error");
        CHECK(reg.Contains("fan-120"),
              "registry: default survives empty file layer");
    }
    {
        /* LoadFile failure leaves the registry unchanged */
        const auto dir = TempDir("reload");
        WriteFile(dir / "fan-120.device.json", FanPreset("fan-120"));
        PresetRegistry reg;
        CHECK(reg.LoadDirectory(dir.string(), &errors),
              "registry: baseline dir loads");
        const auto bad = dir / "broken.device.json";
        {
            std::ofstream f(bad, std::ios::binary);
            f << "{ not json";
        }
        CHECK(!reg.LoadFile(bad.string(), &errors)
              && reg.Contains("fan-120"),
              "registry: failed LoadFile leaves registry intact");
    }
}

/*---------------------------------------------------------*\
||| Registry edge cases: duplicate ids, dependency        ||
||| chains/cycles, asset references.                      ||
\*---------------------------------------------------------*/
static json ChainPreset(const std::string& id, const std::string& dep)
{
    json j = FanPreset(id);
    if(!dep.empty())
    {
        j["entities"]["child"] = {
            {"type", dep},
            {"x", 0}, {"y", 0}, {"z", 0},
            {"rx", 0}, {"ry", 0}, {"rz", 0},
        };
    }
    return j;
}

static void TestRegistryEdgeCases()
{
    std::vector<std::string> errors;

    /* Duplicate ids: two files claiming one id. The file whose
       NAME matches the id wins deterministically; the impostor is
       rejected — never "whichever the scan found first". */
    {
        const auto dir = TempDir("dup-ids");
        json real = FanPreset("fan-x");
        real["name"] = "Real fan";
        json impostor = FanPreset("fan-x");
        impostor["name"] = "Impostor fan";
        WriteFile(dir / "fan-x.device.json",    real);
        WriteFile(dir / "impostor.device.json", impostor);
        PresetRegistry reg;
        CHECK(!reg.LoadDirectory(dir.string(), &errors)
              && HasError(errors, "does not match"),
              "dup-ids: second claim rejected");
        const DevicePreset* got = reg.Find("fan-x");
        CHECK(got != nullptr && got->name == "Real fan",
              "dup-ids: filename-named file wins deterministically");
        CHECK(reg.FileCount() == 1,
              "dup-ids: impostor never entered the file layer");
    }
    /* When NEITHER file is named after the claimed id, both are
       rejected — there is no winner to pick. */
    {
        const auto dir = TempDir("dup-orphans");
        WriteFile(dir / "a.device.json", FanPreset("dup"));
        WriteFile(dir / "b.device.json", FanPreset("dup"));
        PresetRegistry reg;
        errors.clear();
        CHECK(!reg.LoadDirectory(dir.string(), &errors)
              && !reg.Contains("dup") && reg.FileCount() == 0,
              "dup-ids: orphan claims both rejected");
    }

    /* Transitive dependency chain a -> b -> c loads whole; a
       missing tail removes only the dependents above it. */
    {
        const auto dir = TempDir("dep-chain");
        WriteFile(dir / "chain-a.device.json", ChainPreset("chain-a", "chain-b"));
        WriteFile(dir / "chain-b.device.json", ChainPreset("chain-b", "chain-c"));
        WriteFile(dir / "chain-c.device.json", ChainPreset("chain-c", ""));
        PresetRegistry reg;
        errors.clear();
        CHECK(reg.LoadDirectory(dir.string(), &errors)
              && reg.Contains("chain-a") && reg.Contains("chain-b")
              && reg.Contains("chain-c"),
              "deps: transitive chain loads");
    }
    {
        const auto dir = TempDir("dep-chain-broken");
        WriteFile(dir / "chain-a.device.json", ChainPreset("chain-a", "chain-b"));
        WriteFile(dir / "chain-b.device.json", ChainPreset("chain-b", "ghost"));
        WriteFile(dir / "chain-c.device.json", ChainPreset("chain-c", ""));
        PresetRegistry reg;
        errors.clear();
        CHECK(!reg.LoadDirectory(dir.string(), &errors)
              && HasError(errors, "missing dependency"),
              "deps: missing tail reported");
        CHECK(!reg.Contains("chain-a") && !reg.Contains("chain-b")
              && reg.Contains("chain-c"),
              "deps: dependents cascade-removed, unrelated kept");
    }
    /* Self-reference is a reference cycle of length 1. */
    {
        const auto dir = TempDir("self-ref");
        WriteFile(dir / "selfy.device.json", ChainPreset("selfy", "selfy"));
        PresetRegistry reg;
        errors.clear();
        CHECK(!reg.LoadDirectory(dir.string(), &errors)
              && HasError(errors, "cycle")
              && !reg.Contains("selfy"),
              "deps: self-reference rejected as cycle");
    }

    /* Asset references in appearance resolve against the
       workspace assets/ dir (sibling of presets/). A missing file
       must be REPORTED, not crash — the type still registers (a
       dangling icon must not kill every instance of it). */
    {
        const auto root = TempDir("assets");
        const auto dir  = root / "presets" / "devices";
        std::filesystem::create_directories(dir);
        std::filesystem::create_directories(root / "assets");
        {
            std::ofstream f(root / "assets" / "plate.png");
            f << "png";
        }
        json good = FanPreset("with-asset");
        good["entities"]["frame"]["appearance"] = {{"model", "plate.png"}};
        WriteFile(dir / "with-asset.device.json", good);
        json bad = FanPreset("missing-asset");
        bad["entities"]["frame"]["appearance"] =
            {{"model", "no-such-file.glb"}};
        WriteFile(dir / "missing-asset.device.json", bad);
        PresetRegistry reg;
        errors.clear();
        CHECK(!reg.LoadDirectory(dir.string(), &errors)
              && HasError(errors, "no-such-file.glb"),
              "assets: missing file reported");
        CHECK(reg.Contains("with-asset") && reg.Contains("missing-asset"),
              "assets: types still resolvable");
    }
    /* Asset path SYNTAX is parse-layer validation: relative
       portable paths only — no drive letters, no absolute paths,
       no .. escapes. */
    {
        DevicePreset p;
        json j = FanPreset("path-check");
        j["entities"]["frame"]["appearance"] =
            {{"model", "../escape.glb"}};
        errors.clear();
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "model"),
              "assets: .. escape rejected at parse");
        j["entities"]["frame"]["appearance"] =
            {{"model", "C:/abs.glb"}};
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "model"),
              "assets: absolute path rejected at parse");
        j["entities"]["frame"]["appearance"] =
            {{"model", "fans/fan-a.glb"}};
        CHECK(DevicePresetFromJson(j, p, &errors),
              "assets: clean relative path parses");
        /* the collected refs feed export/bundling (task 4.4) */
        const std::vector<std::string> refs = PresetAssetRefs(p);
        CHECK(refs.size() == 1 && refs[0] == "fans/fan-a.glb",
              "assets: ref enumeration");
    }
}

/*---------------------------------------------------------*\
||| PresetRegistry::List — the merged file-over-default    ||
||| library view the device-library UI (task 4.2) reads.   ||
\*---------------------------------------------------------*/
static void TestRegistryList()
{
    std::vector<std::string> errors;
    PresetRegistry reg;

    DevicePreset fan, grp;
    CHECK(DevicePresetFromJson(FanPreset("fan-120"), fan, nullptr),
          "list: fixture parses");
    grp.id = "group"; grp.name = "Placement group"; grp.category = "group";
    reg.SetDefaults({ fan, grp });

    /* A file-layer preset with the same id overrides its default;
       a file-only preset joins the list. */
    const auto dir = TempDir("list");
    json jf = FanPreset("fan-120", 16);
    jf["name"] = "Edited fan";
    WriteFile(dir / "fan-120.device.json", jf);
    WriteFile(dir / "pad.device.json",      MatrixPreset("pad"));
    /* a dynamic-matrix type reports led_total 0 */
    json dyn = FanPreset("kbd");
    dyn["entities"]["diffuser"]["zone"] = "mx";
    dyn["entities"].erase("frame");
    dyn["entities"]["diffuser"]["size_m"] = {0.4, 0.02, 0.15};
    dyn["zones"][0] = {
        {"id", "mx"}, {"entity", "diffuser"}, {"led_count", 0},
        {"layout", {{"type", "matrix"}, {"dynamic", true}}},
    };
    WriteFile(dir / "kbd.device.json", dyn);
    CHECK(reg.LoadDirectory(dir.string(), &errors), "list: dir loads");

    const std::vector<PresetRegistry::PresetInfo> infos = reg.List();
    CHECK(infos.size() == 4, "list: merged file-over-default view");
    if(infos.size() == 4)
    {
        /* sorted by id: fan-120, group, kbd, pad */
        CHECK(infos[0].id == "fan-120" && infos[1].id == "group"
              && infos[2].id == "kbd" && infos[3].id == "pad",
              "list: sorted by id");
        CHECK(infos[0].from_file && infos[0].name == "Edited fan",
              "list: file layer shadows the default");
        CHECK(!infos[1].from_file && infos[1].name == "Placement group",
              "list: default-only type marked packaged");
        CHECK(infos[0].zone_count == 1 && infos[0].led_total == 16,
              "list: zone + led totals from the file version");
        /* bounds_m = union of entity size_m footprints: the edited
           fan's frame (0.12,0.025,0.12) at origin plus diffuser
           (0.11,0.004,0.11) at y=0.013 -> y union
           [-0.0125, 0.015] = 0.0275 tall. */
        CHECK(Near(infos[0].bounds_m.x, 0.12f)
              && Near(infos[0].bounds_m.y, 0.0275f)
              && Near(infos[0].bounds_m.z, 0.12f),
              "list: bounds union of entity footprints");
        CHECK(infos[2].led_total == 0,
              "list: dynamic matrix reports led_total 0");
        CHECK(infos[3].led_total == 5
              && Near(infos[3].bounds_m.x, 0.06f),
              "list: static matrix counts live cells");
    }
}

/*---------------------------------------------------------*\
||| binding_hints — optional compatible-hardware hints on  ||
||| the TYPE (never per-instance serials, never verified). ||
\*---------------------------------------------------------*/
static void TestBindingHints()
{
    std::vector<std::string> errors;
    DevicePreset p;

    {
        json j = FanPreset("hinted");
        j["binding_hints"] = json::array({
            {{"controller_name", "X870E AORUS ELITE"},
             {"vendor",          "Gigabyte"},
             {"zone_name",       "ARGB_V2_1"}},
            {{"vendor", "Lian Li"}},
        });
        CHECK(DevicePresetFromJson(j, p, &errors)
              && p.binding_hints.size() == 2
              && p.binding_hints[0].controller_name == "X870E AORUS ELITE"
              && p.binding_hints[0].zone_name == "ARGB_V2_1"
              && p.binding_hints[1].vendor == "Lian Li"
              && p.binding_hints[1].zone_name.empty(),
              "hints: parsed");
        const json back = ToJson(p);
        CHECK(back.contains("binding_hints")
              && back["binding_hints"].size() == 2
              && back["binding_hints"][1].size() == 1,
              "hints: serialized, empty fields omitted");
        DevicePreset p2;
        CHECK(DevicePresetFromJson(back, p2, &errors)
              && p2.binding_hints.size() == 2
              && p2.binding_hints[0].vendor == "Gigabyte",
              "hints: round-trip");
    }
    /* unknown hint keys are reported as likely typos — hints are
       known-fields-only, never retained verbatim. */
    {
        json j = FanPreset("hinted");
        j["binding_hints"] = json::array({
            {{"controller_name", "G512"}, {"controlller", "typo"}},
        });
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "binding_hints")
              && HasError(errors, "typo"),
              "hints: unknown key reported as typo");
    }
    {
        json j = FanPreset("hinted");
        j["binding_hints"] = json::array({ "G512" });
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "binding_hints"),
              "hints: non-object entry rejected");
        j["binding_hints"] = json::array({ {{"vendor", 42}} });
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "binding_hints"),
              "hints: non-string value rejected");
        j["binding_hints"] = "G512";
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "binding_hints"),
              "hints: non-array rejected");
    }
    /* control characters are not sane name charset */
    {
        json j = FanPreset("hinted");
        j["binding_hints"] = json::array({
            {{"controller_name", "bad\nname"}},
        });
        CHECK(!DevicePresetFromJson(j, p, &errors)
              && HasError(errors, "binding_hints"),
              "hints: control characters rejected");
    }
    /* omitted by default; never written when empty */
    {
        CHECK(DevicePresetFromJson(FanPreset("plain"), p, nullptr)
              && p.binding_hints.empty()
              && !ToJson(p).contains("binding_hints"),
              "hints: omitted when empty");
    }
    /* hints are informational — they never grant verified. A bound
       zone's verified flag still comes only from device_settings. */
    {
        DevicePreset hinted;
        json j = FanPreset("fan-120");
        j["binding_hints"] = json::array({
            {{"controller_name", "Ctrl A"}, {"zone_name", "Z1"}},
        });
        CHECK(DevicePresetFromJson(j, hinted, nullptr),
              "hints: fixture parses");
        PresetRegistry reg;
        reg.SetDefaults({ hinted });
        StudioDocument w = TwoFanWorkspace();
        /* unverified zone setting + a matching hint: still not
           verified after resolve. */
        w.device_settings["fan_a"].zones["ring"].verified = false;
        SceneDocument doc;
        CHECK(ResolveScene(w, reg, doc, &errors),
              "hints: workspace resolves");
        const SceneObject* da = FindObject(doc, "fan_a/diffuser");
        CHECK(da != nullptr && !da->verified,
              "hints: never grant verified");
    }
}

/*---------------------------------------------------------*\
||| Shared-definition reload via the programmatic path:    ||
||| mutate a DevicePreset, re-Add it, re-resolve — every  ||
||| dependent instance updates together while siblings   ||
||| and the generated <instance>/<entity> ids are stable.||
\*---------------------------------------------------------*/
static void TestSharedDefinitionReload()
{
    std::vector<std::string> errors;
    PresetRegistry reg;

    DevicePreset fan, pad;
    CHECK(DevicePresetFromJson(FanPreset("fan-120", 8), fan, nullptr)
          && DevicePresetFromJson(MatrixPreset("pad"), pad, nullptr),
          "sreload: fixtures parse");
    CHECK(reg.Add(fan, &errors) && reg.Add(pad, &errors),
          "sreload: types register");

    StudioDocument w = TwoFanWorkspace();
    /* room for the edited 16- and 24-LED type revisions */
    w.bindings["bus_a"].zone_leds = 32;
    w.bindings["bus_b"].zone_leds = 32;
    DeviceInstance d;
    d.type     = "pad";
    d.position = { 0.0f, 0.0f, 0.5f };
    w.devices["pad"] = d;

    SceneDocument doc;
    CHECK(ResolveScene(w, reg, doc, &errors), "sreload: baseline resolve");
    std::set<std::string> ids_before;
    for(const SceneObject& o : doc.objects)
    {
        ids_before.insert(o.id);
    }

    /* Mutate the shared type — 16 LEDs now — and re-Add. */
    fan.zones[0].led_count = 16;
    CHECK(reg.Add(fan, &errors), "sreload: edited type re-added");
    SceneDocument doc2;
    errors.clear();
    CHECK(ResolveScene(w, reg, doc2, &errors), "sreload: re-resolve");
    std::set<std::string> ids_after;
    for(const SceneObject& o : doc2.objects)
    {
        ids_after.insert(o.id);
    }
    CHECK(ids_before == ids_after,
          "sreload: entity ids stable across reload");
    const SceneObject* da = FindObject(doc2, "fan_a/diffuser");
    const SceneObject* db = FindObject(doc2, "fan_b/diffuser");
    const SceneObject* dp = FindObject(doc2, "pad/body");
    CHECK(da != nullptr && db != nullptr
          && da->emitters.size() == 16 && db->emitters.size() == 16,
          "sreload: every instance picks up the edit");
    CHECK(dp != nullptr && dp->emitters.size() == 5,
          "sreload: sibling type untouched");

    /* The file path behaves the same: LoadFile replaces the file-
       layer definition and re-resolution picks it up. */
    const auto dir = TempDir("sreload-file");
    WriteFile(dir / "fan-120.device.json", FanPreset("fan-120", 24));
    CHECK(reg.LoadFile((dir / "fan-120.device.json").string(), &errors),
          "sreload: LoadFile replaces the type");
    SceneDocument doc3;
    errors.clear();
    CHECK(ResolveScene(w, reg, doc3, &errors), "sreload: file re-resolve");
    da = FindObject(doc3, "fan_a/diffuser");
    db = FindObject(doc3, "fan_b/diffuser");
    CHECK(da != nullptr && db != nullptr
          && da->emitters.size() == 24 && db->emitters.size() == 24,
          "sreload: LoadFile edit rebuilds all instances");
}

/*---------------------------------------------------------*\
||| The packaged library: every bundled                  ||
||| presets/devices/*.device.json must parse + validate, ||
||| the default workspace must resolve against them, and ||
||| the minimal C++ fallback still produces a resolvable ||
||| desk when no packaged file is readable.              ||
\*---------------------------------------------------------*/
static void TestPackagedDefaults()
{
    std::vector<std::string> errors;

    const std::filesystem::path dir = PackagedPresetDir();
    std::error_code ec;
    CHECK(std::filesystem::is_directory(dir, ec),
          "packaged: presets/devices exists");

    /* Loading the packaged dir exercises every file: parse +
       validate + id==filename + dependency graph. */
    PresetRegistry reg;
    CHECK(reg.LoadDirectory(dir.string(), &errors),
          "packaged: every bundled type validates");
    if(!errors.empty())
    {
        std::printf("  (packaged errors: %s)\n", errors.front().c_str());
    }
    CHECK(reg.FileCount() >= 10,
          "packaged: library populated");
    CHECK(reg.Contains("desk") && reg.Contains("group")
          && reg.Contains("fan-120") && reg.Contains("fan-slw"),
          "packaged: core types present");

    /* the default compact workspace resolves against them */
    StudioDocument w = BuildDefaultWorkspace();
    SceneDocument doc;
    errors.clear();
    CHECK(ResolveScene(w, reg, doc, &errors),
          "packaged: default workspace resolves");
    if(!errors.empty())
    {
        std::printf("  (packaged resolve: %s)\n", errors.front().c_str());
    }

    /* Fallback: defaults layer = the minimal C++ set, no files —
       the recoverable desk must still resolve. */
    PresetRegistry minimal;
    minimal.SetDefaults(DefaultDevicePresets());
    CHECK(minimal.Contains("desk") && minimal.Contains("group"),
          "fallback: desk + group in the minimal set");
    StudioDocument fw = BuildFallbackWorkspace();
    SceneDocument fdoc;
    errors.clear();
    CHECK(ResolveScene(fw, minimal, fdoc, &errors),
          "fallback: minimal workspace resolves");
    CHECK(FindObject(fdoc, "desk/body") != nullptr,
          "fallback: desk body expands");
    /* ... and the full default workspace is NOT resolvable on the
       minimal set — the split is honest, not vestigial. */
    SceneDocument keep;
    keep.objects.push_back(SceneObject{});
    CHECK(!ResolveScene(w, minimal, keep, &errors)
          && keep.objects.size() == 1,
          "fallback: full workspace needs the packaged types");
}

/*---------------------------------------------------------*\
||| SceneResolver — the contract from the brief:           ||
||| two fan instances sharing one definition with          ||
||| different root transforms, identical local layouts,    ||
||| correct world emitter positions and independent        ||
||| bindings.                                              ||
\*---------------------------------------------------------*/
static PresetRegistry TestReg()
{
    PresetRegistry reg;
    std::vector<DevicePreset> defs;
    DevicePreset fan;
    CHECK(DevicePresetFromJson(FanPreset("fan-120"), fan, nullptr),
          "registry: fixture preset parses");
    defs.push_back(fan);
    reg.SetDefaults(defs);
    return reg;
}

static StudioDocument TwoFanWorkspace()
{
    StudioDocument w;
    w.meta.name = "Two fans";

    DeviceInstance a;
    a.type     = "fan-120";
    a.position = { 0.30f, 0.10f, 0.0f };
    a.rotation_deg = { 90.0f, 0.0f, 0.0f };
    w.devices["fan_a"] = a;

    DeviceInstance b;
    b.type     = "fan-120";
    b.position = { -0.30f, 0.05f, 0.0f };
    b.rotation_deg = { 0.0f, 45.0f, 0.0f };
    w.devices["fan_b"] = b;

    DeviceBinding ba;
    ba.id              = "bus_a";
    ba.controller_name = "Ctrl A";
    ba.zone_name       = "Z1";
    ba.zone_leds       = 8;
    w.bindings["bus_a"] = ba;
    DeviceBinding bb;
    bb.id              = "bus_b";
    bb.controller_name = "Ctrl B";
    bb.zone_name       = "Z2";
    bb.zone_leds       = 16;   /* addr_base 8 + 8 LEDs must fit */
    w.bindings["bus_b"] = bb;

    w.device_settings["fan_a"].zones["ring"] =
        { "bus_a", 0, true };
    w.device_settings["fan_b"].zones["ring"] =
        { "bus_b", 8, true };
    w.object_colors["fan_a/diffuser"] = MakeSceneColor(255, 0, 0);
    w.object_colors["fan_b/diffuser"] = MakeSceneColor(0, 0, 255);
    return w;
}

static void TestResolveTwoFans()
{
    const PresetRegistry reg = TestReg();
    const StudioDocument w   = TwoFanWorkspace();

    SceneDocument doc;
    std::vector<std::string> errors;
    CHECK(ResolveScene(w, reg, doc, &errors), "resolve: two fans expand");
    if(!errors.empty())
    {
        std::printf("  (resolve errors: %s)\n", errors.front().c_str());
    }

    /* every instance expands the shared definition's entities under
       its own stable id — the definition is stored once. */
    const SceneObject* fa = FindObject(doc, "fan_a/diffuser");
    const SceneObject* fb = FindObject(doc, "fan_b/diffuser");
    CHECK(fa != nullptr && fb != nullptr,
          "resolve: instanced entity ids namespaced");
    CHECK(FindObject(doc, "fan_a/frame") != nullptr
          && FindObject(doc, "fan_b/frame") != nullptr,
          "resolve: sibling entities expanded");

    /* identical local layouts — same emitter local positions */
    CHECK(fa->emitters.size() == 8 && fb->emitters.size() == 8,
          "resolve: emitter counts match type");
    bool same_local = true;
    for(size_t i = 0; i < fa->emitters.size(); i++)
    {
        if(!Near(fa->emitters[i].local_pos.x, fb->emitters[i].local_pos.x)
           || !Near(fa->emitters[i].local_pos.z, fb->emitters[i].local_pos.z))
        {
            same_local = false;
        }
    }
    CHECK(same_local, "resolve: identical local emitter layout");

    /* different root transforms — instance groups carry them */
    const SceneObject* ga = FindObject(doc, "fan_a");
    const SceneObject* gb = FindObject(doc, "fan_b");
    CHECK(ga != nullptr && gb != nullptr
          && ga->kind == ObjectKind::Group
          && Near(ga->transform.position.x, 0.30f)
          && Near(ga->transform.rotation_deg.x, 90.0f)
          && Near(gb->transform.rotation_deg.y, 45.0f),
          "resolve: instance transforms on group nodes");

    /* world emitter positions follow instance transform x entity
       chain x emitter local — fan_a's ring stands up (rx=90) so a
       local XZ circle lands in the world XY plane. */
    const std::vector<Vec3> wa = WorldEmitters(doc, "fan_a/diffuser");
    CHECK(wa.size() == 8, "resolve: world emitters computed");
    if(wa.size() == 8)
    {
        /* local {r,0,0} -> rotated to (0.30, 0.10+r*cos? ) — just
           check the world ring is centered on the entity world pos
           and NOT equal to fan_b's (different transforms). */
        const std::vector<Vec3> wb = WorldEmitters(doc, "fan_b/diffuser");
        CHECK(wb.size() == 8, "resolve: fan_b world emitters");
        bool differ = false;
        for(size_t i = 0; i < wa.size() && i < wb.size(); i++)
        {
            if(!Near(wa[i].x, wb[i].x) || !Near(wa[i].z, wb[i].z))
            {
                differ = true;
            }
        }
        CHECK(differ, "resolve: distinct world emitter clouds");
        /* rx=90 tips the local +z ring axis onto -y: emitter 0 at
           local (+r, 0, 0) stays +x; the y spread spans r*2. */
        float ymin = 1e9f, ymax = -1e9f;
        for(const Vec3& v : wa)
        {
            ymin = std::min(ymin, v.y);
            ymax = std::max(ymax, v.y);
        }
        CHECK(ymax - ymin > 0.08f,
              "resolve: rx=90 tips the ring vertical");
    }

    /* independent physical bindings + addr bases */
    CHECK(fa->binding == "bus_a" && fb->binding == "bus_b",
          "resolve: independent bindings");
    CHECK(fa->emitters[0].address == 0
          && fb->emitters[0].address == 8,
          "resolve: per-instance addr_base");
    CHECK(fa->verified && fb->verified,
          "resolve: verification state per instance");

    /* colors fan out to resolved ids */
    CHECK(doc.object_colors.at("fan_a/diffuser")
              == MakeSceneColor(255, 0, 0),
          "resolve: object colors keyed by resolved path");
}

static void TestResolveMirror()
{
    const PresetRegistry reg = TestReg();
    StudioDocument w = TwoFanWorkspace();
    w.device_settings["fan_b"].mirror_of = "fan_a";

    SceneDocument doc;
    std::vector<std::string> errors;
    CHECK(ResolveScene(w, reg, doc, &errors),
          "mirror: instance mirror resolves");

    /* every part object under fan_b is Linked to the corresponding
       fan_a object — output ownership, not placement. */
    const SceneObject* b = FindObject(doc, "fan_b/diffuser");
    CHECK(b != nullptr && b->kind == ObjectKind::Linked
          && b->mirror_of == "fan_a/diffuser"
          && b->emitters.empty() && b->binding.empty(),
          "mirror: expanded objects Linked to owner");
    const SceneObject* bf = FindObject(doc, "fan_b/frame");
    CHECK(bf != nullptr && bf->kind == ObjectKind::Linked
          && bf->mirror_of == "fan_a/frame",
          "mirror: all parts mirror 1:1");

    /* placement is untouched — fan_b keeps its own transform */
    const SceneObject* gb = FindObject(doc, "fan_b");
    CHECK(gb != nullptr && gb->kind == ObjectKind::Group
          && Near(gb->transform.position.x, -0.30f),
          "mirror: placement transform preserved");

    /* the resolved document still satisfies scene-graph rules */
    std::vector<std::string> gerrs;
    CHECK(ValidateSceneGraph(doc, &gerrs),
          "mirror: resolved scene validates");

    /* mirror chains are rejected */
    StudioDocument chained = w;
    chained.device_settings["fan_a"].mirror_of = "fan_b";
    SceneDocument keep = doc;
    CHECK(!ResolveScene(chained, reg, keep, &errors)
          && HasError(errors, "chain"),
          "mirror: chains rejected");
}

static void TestResolveFailures()
{
    const PresetRegistry reg = TestReg();
    StudioDocument w = TwoFanWorkspace();
    std::vector<std::string> errors;

    {
        StudioDocument bad = w;
        bad.devices["fan_a"].type = "no_such_type";
        SceneDocument doc;
        CHECK(!ResolveScene(bad, reg, doc, &errors)
              && HasError(errors, "devices.fan_a.type")
              && HasError(errors, "no_such_type"),
              "resolve: missing type reports the field");
    }
    {
        StudioDocument bad = w;
        bad.device_settings["fan_a"].zones["ghost"] = { "bus_a", 0, true };
        SceneDocument doc;
        CHECK(!ResolveScene(bad, reg, doc, &errors)
              && HasError(errors, "zones.ghost"),
              "resolve: unknown zone id reports the field");
    }
    {
        StudioDocument bad = w;
        bad.device_settings["fan_a"].zones["ring"].binding = "ghost";
        /* StudioConfig catches this at parse; resolver double-checks
           only through validation — make sure parse catches it. */
        std::vector<std::string> verrs;
        StudioDocument chk;
        json j = ToJson(bad);
        CHECK(!FromJson(j, chk, &verrs)
              && HasError(verrs, "binding"),
              "resolve: dangling zone binding rejected at parse");
    }
    {
        /* a failed resolve leaves the previous document alone */
        SceneDocument keep;
        keep.objects.push_back(SceneObject{});
        keep.objects[0].id = "sentinel";
        StudioDocument bad = w;
        bad.devices["fan_a"].type = "ghost";
        CHECK(!ResolveScene(bad, reg, keep, &errors)
              && keep.objects.size() == 1
              && keep.objects[0].id == "sentinel",
              "resolve: failure leaves document untouched");
    }
    {
        /* instance parent chain: child placement follows parent */
        StudioDocument nested = w;
        DeviceInstance c;
        c.type     = "fan-120";
        c.position = { 0.05f, 0.0f, 0.0f };
        c.parent   = "fan_a";
        nested.devices["fan_c"] = c;
        SceneDocument doc;
        errors.clear();
        CHECK(ResolveScene(nested, reg, doc, &errors),
              "resolve: instance parent resolves");
        const SceneObject* gc = FindObject(doc, "fan_c");
        CHECK(gc != nullptr && gc->parent_id == "fan_a",
              "resolve: parent chain on group node");
        const auto worlds = ResolveWorldMatrices(doc);
        const Vec3 p = TransformPoint(worlds.at("fan_c/diffuser"),
                                      Vec3{});
        const Vec3 pa = TransformPoint(worlds.at("fan_a/diffuser"),
                                       Vec3{});
        (void)p; (void)pa;
    }
    {
        StudioDocument bad = w;
        bad.devices["fan_a"].parent = "fan_b";
        bad.devices["fan_b"].parent = "fan_a";
        StudioDocument chk;
        std::vector<std::string> verrs;
        CHECK(!FromJson(ToJson(bad), chk, &verrs)
              && HasError(verrs, "parent"),
              "resolve: instance parent cycle rejected at parse");
    }
    {
        /* LED bounds — the v2 contract: an address must be < the
           bound zone's led count. fan_a sits on bus_a (8 LEDs);
           addr_base 4 pushes addresses 4..11 out of range. */
        StudioDocument bad = w;
        bad.device_settings["fan_a"].zones["ring"].addr_base = 4;
        SceneDocument doc;
        CHECK(!ResolveScene(bad, reg, doc, &errors)
              && HasError(errors, "out of range")
              && HasError(errors, "zones.ring"),
              "bounds: addr_base + led_count > zone_leds rejected");
    }
    {
        /* boundary: addr_base 0 on the 8-LED zone fits exactly */
        StudioDocument ok = w;
        ok.device_settings["fan_a"].zones["ring"].addr_base = 0;
        SceneDocument doc;
        CHECK(ResolveScene(ok, reg, doc, &errors),
              "bounds: in-range zone resolves");
    }
    {
        /* an unbound zone can't be bounds-checked — no error */
        StudioDocument unbound = w;
        unbound.device_settings["fan_a"].zones["ring"].binding.clear();
        SceneDocument doc;
        CHECK(ResolveScene(unbound, reg, doc, &errors),
              "bounds: unbound zone resolves");
    }
}

/*---------------------------------------------------------*\
||| Expansion caps — the v2 scene validator's bounds,    ||
||| enforced while instances unfold.                     ||
\*---------------------------------------------------------*/
static void TestResolveCaps()
{
    std::vector<std::string> errors;

    /* Object cap: 700 fan instances x (group + 2 entities) =
       2100 objects > STUDIO_MAX_OBJECTS (2048). */
    {
        const PresetRegistry reg = TestReg();
        StudioDocument w;
        DeviceInstance d;
        d.type = "fan-120";
        for(int i = 0; i < 700; i++)
        {
            w.devices["fan_" + std::to_string(i)] = d;
        }
        SceneDocument doc;
        CHECK(!ResolveScene(w, reg, doc, &errors)
              && HasError(errors, "object count exceeds cap"),
              "caps: object expansion limited");
    }
    /* Emitter cap: one entity carrying 17 zones of 4096-LED rings
       = 69632 emitters > STUDIO_MAX_EMITTERS (65536), while the
       object count stays tiny — the emitter limit is what trips. */
    {
        DevicePreset fat;
        fat.id   = "fat";
        fat.name = "Over-fed strip";
        PresetEntity e;
        e.id       = "body";
        e.geometry = "fan_ring";
        e.zone     = "z0";
        fat.entities["body"] = e;
        for(int i = 0; i < 17; i++)
        {
            DeviceZone z;
            z.id        = "z" + std::to_string(i);
            z.entity    = "body";
            z.led_count = 4096;
            z.layout.type = "ring";
            z.layout.radius_m = 0.05f;
            fat.zones.push_back(z);
        }
        PresetRegistry reg;
        CHECK(reg.Add(fat, &errors), "caps: fat type registers");
        StudioDocument w;
        DeviceInstance d;
        d.type = "fat";
        w.devices["one"] = d;
        SceneDocument doc;
        errors.clear();
        CHECK(!ResolveScene(w, reg, doc, &errors)
              && HasError(errors, "emitter count exceeds cap"),
              "caps: emitter expansion limited");
    }
}

/*---------------------------------------------------------*\
||| Compact serialization — the workspace file carries     ||
||| instances + settings only; never expanded content.     ||
\*---------------------------------------------------------*/
static void TestCompactRoundTrip()
{
    const StudioDocument w = TwoFanWorkspace();
    const json j = ToJson(w);

    CHECK(j.value("schema_version", 0) == 3,
          "compact: schema_version 3");
    CHECK(!j.contains("scene") && !j.contains("definitions"),
          "compact: no expanded sections");
    CHECK(j.contains("devices") && j.contains("bindings")
          && j.contains("device_settings") && j.contains("colors"),
          "compact: v3 sections present");

    /* no inline entities, generated emitters or embedded defs —
       top level and inside each device entry (colors.emitters is a
       legit v3 section: per-emitter paint, not generated layout). */
    CHECK(!j.contains("entities") && !j.contains("emitters")
          && !j.contains("definitions") && !j.contains("scene"),
          "compact: no inline entities/emitters/definitions");
    for(const auto& kv : j["devices"].items())
    {
        CHECK(!kv.value().contains("entities")
              && !kv.value().contains("emitters")
              && !kv.value().contains("geometry"),
              "compact: device entry is placement only");
    }

    /* device entries are placements only */
    const json& fa = j["devices"]["fan_a"];
    CHECK(fa.value("type", std::string()) == "fan-120"
          && fa.contains("x") && fa.contains("rx")
          && !fa.contains("entities") && !fa.contains("emitters")
          && !fa.contains("geometry"),
          "compact: instance carries type + transform only");

    StudioDocument back;
    std::vector<std::string> errors;
    CHECK(FromJson(j, back, &errors), "compact: round-trip parses");
    CHECK(back.devices.size() == 2 && back.bindings.size() == 2
          && back.device_settings.size() == 2
          && back.object_colors.size() == 2,
          "compact: all sections restored");
    CHECK(ToJson(back) == j, "compact: deterministic serialization");

    /* colors write #RRGGBB */
    CHECK(j["colors"]["objects"]["fan_a/diffuser"] == "#FF0000",
          "compact: hex colors on disk");
}

/*---------------------------------------------------------*\
||| Type reload — editing a type file and reloading the    ||
||| registry rebuilds every instance that references it.   ||
\*---------------------------------------------------------*/
static void TestTypeReload()
{
    const auto dir = TempDir("type-reload");
    WriteFile(dir / "fan-120.device.json", FanPreset("fan-120", 8));

    PresetRegistry reg;
    std::vector<std::string> errors;
    CHECK(reg.LoadDirectory(dir.string(), &errors), "reload: initial load");

    StudioDocument w = TwoFanWorkspace();
    /* The edited type doubles to 16 LEDs — the bound zones must
       have room (fan_a: 0..15, fan_b: addr_base 8 -> 8..23). */
    w.bindings["bus_a"].zone_leds = 16;
    w.bindings["bus_b"].zone_leds = 24;
    SceneDocument doc;
    CHECK(ResolveScene(w, reg, doc, &errors), "reload: initial resolve");
    const SceneObject* da = FindObject(doc, "fan_a/diffuser");
    const SceneObject* db = FindObject(doc, "fan_b/diffuser");
    CHECK(da != nullptr && db != nullptr, "reload: diffusers exist");
    if(da != nullptr && db != nullptr)
    {
        CHECK(da->emitters.size() == 8 && db->emitters.size() == 8,
              "reload: 8 emitters before edit");
    }

    /* edit the TYPE file — 16 LEDs now — and reload: both instances
       update together. */
    WriteFile(dir / "fan-120.device.json", FanPreset("fan-120", 16));
    reg.ClearFiles();
    CHECK(reg.LoadDirectory(dir.string(), &errors), "reload: edited load");
    CHECK(ResolveScene(w, reg, doc, &errors), "reload: edited resolve");
    da = FindObject(doc, "fan_a/diffuser");
    db = FindObject(doc, "fan_b/diffuser");
    CHECK(da != nullptr && db != nullptr
          && da->emitters.size() == 16 && db->emitters.size() == 16,
          "reload: type edit rebuilds all instances");

    /* moving one instance touches only its placement — the sibling
       and the type file are unchanged. */
    StudioDocument moved = w;
    moved.devices["fan_a"].position.x += 0.10f;
    SceneDocument moved_doc;
    CHECK(ResolveScene(moved, reg, moved_doc, &errors),
          "reload: moved resolve");
    const SceneObject* ma = FindObject(moved_doc, "fan_a");
    const SceneObject* mb = FindObject(moved_doc, "fan_b");
    CHECK(ma != nullptr && mb != nullptr
          && Near(ma->transform.position.x, 0.40f)
          && Near(mb->transform.position.x, -0.30f),
          "reload: instance move is placement-only");
}

/*---------------------------------------------------------*\
||| Migration — expanded v2 scene -> compact v3 + types.   ||
||| The migrated desk must resolve to the SAME world       ||
||| emitter layout: transforms, bindings and mirrors all   ||
||| preserved.                                             ||
\*---------------------------------------------------------*/
static void TestMigration()
{
    const SceneDocument scene = BuildDefaultDesk();
    StudioDocument w;
    std::vector<DevicePreset> types;
    std::vector<std::string> errors, warnings;
    CHECK(MigrateExpandedScene(scene, w, types, &errors, &warnings),
          "migrate: default desk extracts");
    if(!errors.empty())
    {
        std::printf("  (migration errors: %s)\n", errors.front().c_str());
    }

    /* dedupe: the desk's repeated fans collapse — the 8-LED ring
       fan (case_fans + 3 mirrors + 3 rad fans + gpu_top_fan = 8
       instances) shares ONE type. */
    CHECK(types.size() <= 16 && types.size() < scene.objects.size(),
          "migrate: structural dedupe collapses repeats");
    std::map<std::string, int> inst_per_type;
    for(const auto& kv : w.devices)
    {
        inst_per_type[kv.second.type]++;
    }
    int max_shared = 0;
    for(const auto& kv : inst_per_type)
    {
        max_shared = std::max(max_shared, kv.second);
    }
    CHECK(max_shared >= 8,
          "migrate: repeated fans share one definition");
    CHECK(!types.empty(), "migrate: types extracted");
    CHECK(w.devices.size() == scene.objects.size(),
          "migrate: every object became an instance");

    /* binding identity preserved */
    CHECK(w.bindings.size() == scene.bindings.size(),
          "migrate: bindings preserved");

    /* the migrated workspace is itself valid compact v3 */
    StudioDocument back;
    CHECK(FromJson(ToJson(w), back, &errors),
          "migrate: compact doc round-trips");

    /* registry builds from the extracted types */
    PresetRegistry reg;
    for(const DevicePreset& p : types)
    {
        CHECK(reg.Add(p, &errors), "migrate: extracted type validates");
    }

    SceneDocument resolved;
    CHECK(ResolveScene(w, reg, resolved, &errors),
          "migrate: workspace resolves");

    /* world emitter parity — same addressable LEDs in the same
       world spots (ids change to <instance>/<entity>, positions
       must not). */
    const auto old_worlds = ResolveWorldMatrices(scene);
    std::vector<Vec3> old_pts;
    for(const SceneObject& o : scene.objects)
    {
        const auto wit = old_worlds.find(o.id);
        if(wit == old_worlds.end()) { continue; }
        for(const Emitter& e : o.emitters)
        {
            old_pts.push_back(TransformPoint(wit->second, e.local_pos));
        }
    }
    const auto new_worlds = ResolveWorldMatrices(resolved);
    std::vector<Vec3> new_pts;
    for(const SceneObject& o : resolved.objects)
    {
        const auto wit = new_worlds.find(o.id);
        if(wit == new_worlds.end()) { continue; }
        for(const Emitter& e : o.emitters)
        {
            new_pts.push_back(TransformPoint(wit->second, e.local_pos));
        }
    }
    CHECK(old_pts.size() == new_pts.size(),
          "migrate: emitter count preserved");
    auto key = [](const Vec3& v) {
        return std::make_tuple(std::llround(v.x * 10000),
                               std::llround(v.y * 10000),
                               std::llround(v.z * 10000));
    };
    std::multiset<std::tuple<long long, long long, long long>> os, ns;
    for(const Vec3& v : old_pts) { os.insert(key(v)); }
    for(const Vec3& v : new_pts) { ns.insert(key(v)); }
    CHECK(os == ns, "migrate: world emitter positions preserved");

    /* mirror output behavior: v2 linked copies are v3 mirrored
       instances of the same type. */
    const SceneObject* b1_old = FindObject(scene, "case_fan_b1");
    const SceneObject* b1_new = FindObject(resolved, "case_fan_b1/body");
    CHECK(b1_old != nullptr && b1_new != nullptr
          && b1_new->kind == ObjectKind::Linked
          && b1_new->mirror_of == "case_fans/body",
          "migrate: mirror instances linked to owner");

    /* addresses: the pump's LEDs stay at 8..15 on its zone. */
    const SceneObject* pump = FindObject(resolved, "pump/body");
    CHECK(pump != nullptr && !pump->emitters.empty()
          && pump->emitters.front().address == 8,
          "migrate: addr_base preserved");
}

/*---------------------------------------------------------*\
||| Migration — sparse emitter maps: objects with the     ||
||| same non-contiguous address PATTERN at different      ||
||| bases must not collapse into one type — absolute      ||
||| addresses are physical identity.                       ||
\*---------------------------------------------------------*/
static void TestMigrationSparseAddresses()
{
    SceneDocument scene;
    scene.name = "Sparse";

    DeviceBinding b;
    b.id              = "ctrl";
    b.controller_name = "Ctrl";
    b.zone_name       = "Z";
    b.zone_leds       = 16;
    scene.bindings.push_back(b);

    auto strip = [&](const std::string& id, float x,
                     const int* addrs) {
        SceneObject o;
        o.id       = id;
        o.label    = id;
        o.kind     = ObjectKind::Device;
        o.geometry = "led_strip";
        o.binding  = "ctrl";
        o.transform.position = { x, 0.0f, 0.0f };
        for(int i = 0; i < 3; i++)
        {
            Emitter e;
            e.local_pos = { 0.01f * (float)i, 0.0f, 0.0f };
            e.group     = id;
            e.address   = addrs[i];
            o.emitters.push_back(e);
        }
        scene.objects.push_back(o);
    };
    const int a_addr[3] = { 0, 2, 4 };
    const int b_addr[3] = { 10, 12, 14 };
    strip("strip_a", 0.0f, a_addr);
    strip("strip_b", 0.5f, b_addr);

    StudioDocument w;
    std::vector<DevicePreset> types;
    std::vector<std::string> errors, warnings;
    CHECK(MigrateExpandedScene(scene, w, types, &errors, &warnings),
          "sparse: migration succeeds");
    if(!errors.empty())
    {
        std::printf("  (sparse errors: %s)\n", errors.front().c_str());
    }

    /* The sparse patterns are variants, not one shared type. */
    CHECK(w.devices["strip_a"].type != w.devices["strip_b"].type,
          "sparse: shifted sparse maps become variants");

    PresetRegistry reg;
    for(const DevicePreset& p : types)
    {
        CHECK(reg.Add(p, &errors), "sparse: extracted type validates");
    }
    SceneDocument resolved;
    CHECK(ResolveScene(w, reg, resolved, &errors),
          "sparse: migrated workspace resolves");
    const SceneObject* ra = FindObject(resolved, "strip_a/body");
    const SceneObject* rb = FindObject(resolved, "strip_b/body");
    CHECK(ra != nullptr && rb != nullptr
          && ra->emitters.size() == 3 && rb->emitters.size() == 3,
          "sparse: emitters preserved");
    if(ra != nullptr && rb != nullptr
       && ra->emitters.size() == 3 && rb->emitters.size() == 3)
    {
        CHECK(ra->emitters[0].address == 0
              && ra->emitters[1].address == 2
              && ra->emitters[2].address == 4,
              "sparse: first instance addresses absolute");
        CHECK(rb->emitters[0].address == 10
              && rb->emitters[1].address == 12
              && rb->emitters[2].address == 14,
              "sparse: second instance keeps its own base");
    }

    /* And contiguous strips at different bases still dedupe —
       addr_base is instance state there. */
    SceneDocument cont;
    cont.bindings.push_back(b);
    auto cstrip = [&](const std::string& id, int base0) {
        SceneObject o;
        o.id       = id;
        o.kind     = ObjectKind::Device;
        o.geometry = "led_strip";
        o.binding  = "ctrl";
        for(int i = 0; i < 4; i++)
        {
            Emitter e;
            e.local_pos = { 0.01f * (float)i, 0.0f, 0.0f };
            e.address   = base0 + i;
            o.emitters.push_back(e);
        }
        cont.objects.push_back(o);
    };
    cstrip("s0", 0);
    cstrip("s1", 8);
    StudioDocument cw;
    std::vector<DevicePreset> ctypes;
    errors.clear();
    CHECK(MigrateExpandedScene(cont, cw, ctypes, &errors, &warnings),
          "sparse: contiguous migration succeeds");
    CHECK(cw.devices["s0"].type == cw.devices["s1"].type,
          "sparse: shifted contiguous maps share one type");
    PresetRegistry creg;
    for(const DevicePreset& p : ctypes)
    {
        creg.Add(p, &errors);
    }
    SceneDocument cr;
    CHECK(ResolveScene(cw, creg, cr, &errors),
          "sparse: contiguous resolve");
    const SceneObject* cs = FindObject(cr, "s1/body");
    CHECK(cs != nullptr && cs->emitters.size() == 4
          && cs->emitters[0].address == 8,
          "sparse: contiguous addr_base preserved");
}

/*---------------------------------------------------------*\
||| The packaged default: compact workspace + bundled     ||
||| type files resolve to the same desk BuildDefaultDesk()||
||| produces (transform/effect parity).                   ||
\*---------------------------------------------------------*/
static void TestDefaultWorkspaceParity()
{
    PresetRegistry reg;
    reg.SetDefaults(PackagedPresets());

    StudioDocument w = BuildDefaultWorkspace();
    std::vector<std::string> errors;
    SceneDocument resolved;
    CHECK(ResolveScene(w, reg, resolved, &errors),
          "defaults: packaged workspace resolves");
    if(!errors.empty())
    {
        std::printf("  (defaults errors: %s)\n", errors.front().c_str());
    }

    const SceneDocument expanded = BuildDefaultDesk();
    const auto ow = ResolveWorldMatrices(expanded);
    const auto nw = ResolveWorldMatrices(resolved);
    auto collect = [](const SceneDocument& d,
                      const std::map<std::string, Mat4>& worlds) {
        std::multiset<std::tuple<long long, long long, long long>> pts;
        for(const SceneObject& o : d.objects)
        {
            const auto it = worlds.find(o.id);
            if(it == worlds.end()) { continue; }
            for(const Emitter& e : o.emitters)
            {
                const Vec3 p = TransformPoint(it->second, e.local_pos);
                pts.insert(std::make_tuple(std::llround(p.x * 10000),
                                           std::llround(p.y * 10000),
                                           std::llround(p.z * 10000)));
            }
        }
        return pts;
    };
    CHECK(collect(expanded, ow) == collect(resolved, nw),
          "defaults: world emitters match expanded desk");
    CHECK(resolved.bindings.size() == expanded.bindings.size(),
          "defaults: bindings match expanded desk");
}

/*---------------------------------------------------------*\
||| Task 4.4 — bundle export: transitive type set,         ||
||| sanitization, assets, dest refusal.                    ||
\*---------------------------------------------------------*/
static void TestBundleExport()
{
    namespace fs = std::filesystem;
    const auto src  = TempDir("bundle-src");
    const auto sdir = src / "presets" / "devices";
    fs::create_directories(sdir);
    WriteFile(sdir / "fan-120.device.json",   FanPreset("fan-120", 8));
    WriteFile(sdir / "ring-assy.device.json", RefPreset("ring-assy", "fan-120"));
    WriteFile(sdir / "tower.device.json",     RefPreset("tower", "ring-assy"));
    WriteFile(sdir / "unused-x.device.json",  FanPreset("unused-x", 4));

    PresetRegistry reg;
    std::vector<std::string> errors, warnings;
    CHECK(reg.LoadDirectory(sdir.string(), &errors), "bexport: library loads");

    /* Workspace: two fan instances + a tower (transitive
       tower -> ring-assy -> fan-120). */
    StudioDocument w = TwoFanWorkspace();
    DeviceInstance t;
    t.type     = "tower";
    t.position = { 0.0f, 0.0f, 0.4f };
    w.devices["tower_inst"] = t;
    /* identity + flags the export must strip */
    w.bindings["bus_a"].serial   = "SN-1234";
    w.bindings["bus_a"].location = "USB:1-2";
    w.meta.live_on_startup = true;
    w.effect.playing       = true;

    const auto dest = TempDir("bundle-out");
    unsigned int nt = 0, na = 0;
    CHECK(ExportBundle(dest.string(), w, reg, sdir.string(),
                       &errors, &warnings, false, &nt, &na),
          "bexport: writes the bundle");
    CHECK(nt == 3,
          "bexport: transitive set is fan-120 + ring-assy + tower");
    CHECK(fs::exists(dest / "studio.json")
          && fs::exists(dest / "presets" / "devices" / "fan-120.device.json")
          && fs::exists(dest / "presets" / "devices" / "ring-assy.device.json")
          && fs::exists(dest / "presets" / "devices" / "tower.device.json"),
          "bexport: studio.json + every used type file");
    CHECK(!fs::exists(dest / "presets" / "devices" / "unused-x.device.json"),
          "bexport: unused library types stay behind");

    /* The exported doc is sanitized: serial/location stripped,
       verified/live flags forced off, placements kept. */
    const json dj = ReadJson(dest / "studio.json");
    CHECK(dj["bindings"]["bus_a"].value("serial", std::string("x")).empty()
          && dj["bindings"]["bus_a"].value("location", std::string("x")).empty(),
          "bexport: serial + location stripped");
    CHECK(!dj["output"].value("live_on_startup", true),
          "bexport: live_on_startup forced off");
    CHECK(!dj["effects"].value("playing", true),
          "bexport: effects.playing forced off");
    CHECK(!dj["device_settings"]["fan_a"]["zones"]["ring"]
              .value("verified", false)
          && dj["device_settings"]["fan_a"]["zones"]["ring"]
                 .value("binding", std::string()) == "bus_a",
          "bexport: zone verified off, binding skeleton kept");
    CHECK(dj["devices"]["fan_a"].value("type", std::string()) == "fan-120",
          "bexport: placements kept");

    /* Occupied-bundle refusal + explicit overwrite. */
    CHECK(!ExportBundle(dest.string(), w, reg, sdir.string(), &errors)
          && HasError(errors, "already"),
          "bexport: existing bundle refused without overwrite");
    CHECK(ExportBundle(dest.string(), w, reg, sdir.string(),
                       nullptr, nullptr, true),
          "bexport: explicit overwrite exports");

    /* A non-empty dir with NO bundle is refused too. */
    const auto stray = TempDir("bundle-stray");
    {
        std::ofstream f(stray / "unrelated.txt");
        f << "keep out";
    }
    CHECK(!ExportBundle(stray.string(), w, reg, sdir.string(), &errors)
          && HasError(errors, "not"),
          "bexport: non-empty destination refused");
    CHECK(ExportBundle(stray.string(), w, reg, sdir.string(),
                       nullptr, nullptr, true),
          "bexport: overwrite exports alongside foreign files");

    /* Overwrite re-export prunes type files a previous export left
       behind — a stale bundled type must not end up installed in
       the importer's library. */
    StudioDocument w2 = TwoFanWorkspace();   /* fan-120 only */
    CHECK(ExportBundle(dest.string(), w2, reg, sdir.string(),
                       nullptr, nullptr, true),
          "bexport: narrower overwrite re-export succeeds");
    CHECK(fs::exists(dest / "presets" / "devices" / "fan-120.device.json")
          && !fs::exists(dest / "presets" / "devices" / "ring-assy.device.json")
          && !fs::exists(dest / "presets" / "devices" / "tower.device.json"),
          "bexport: stale bundled type files pruned on overwrite");
}

static void TestBundleAssets()
{
    namespace fs = std::filesystem;
    const auto src  = TempDir("bundle-assets-src");
    const auto sdir = src / "presets" / "devices";
    const auto adir = src / "assets" / "models";
    fs::create_directories(sdir);
    fs::create_directories(adir);
    WriteRaw(adir / "fan.obj", "o fan\n");

    json present = FanPreset("asset-fan", 8);
    present["entities"]["frame"]["appearance"]["model"] = "models/fan.obj";
    WriteFile(sdir / "asset-fan.device.json", present);

    /* A type whose asset ref dangles — registered via Add so the
       file-layer asset check (a warning, not a gate) is bypassed,
       matching a hand-built library entry. */
    DevicePreset ghost;
    json gj = FanPreset("ghost-fan", 8);
    gj["entities"]["frame"]["appearance"]["model"] = "models/ghost.obj";
    CHECK(DevicePresetFromJson(gj, ghost, nullptr),
          "bassets: ghost type parses");

    PresetRegistry reg;
    std::vector<std::string> errors, warnings;
    CHECK(reg.LoadDirectory(sdir.string(), &errors)
          && reg.Add(ghost, &errors),
          "bassets: library loads");

    StudioDocument w;
    w.meta.name = "assets";
    DeviceInstance a;
    a.type     = "asset-fan";
    a.position = { 0.0f, 0.0f, 0.0f };
    w.devices["af"] = a;
    DeviceInstance g;
    g.type     = "ghost-fan";
    g.position = { 0.2f, 0.0f, 0.0f };
    w.devices["gf"] = g;

    const auto dest = TempDir("bundle-assets-out");
    unsigned int nt = 0, na = 0;
    warnings.clear();
    CHECK(ExportBundle(dest.string(), w, reg, sdir.string(),
                       &errors, &warnings, false, &nt, &na),
          "bassets: export succeeds despite a missing asset");
    if(!errors.empty())
    {
        std::printf("  (bassets errors: %s)\n", errors.front().c_str());
    }
    CHECK(na == 1 && fs::exists(dest / "assets" / "models" / "fan.obj"),
          "bassets: resolvable asset copied with its rel path");
    CHECK(!fs::exists(dest / "assets" / "models" / "ghost.obj"),
          "bassets: missing asset not fabricated");
    CHECK(HasError(warnings, "ghost.obj"),
          "bassets: missing asset surfaces as a warning");
}

/*---------------------------------------------------------*\
||| Task 4.4 — bundle import: classify, conflict choices, ||
||| nested ref remap, malformed rejection.                ||
\*---------------------------------------------------------*/
static void TestBundleImport()
{
    namespace fs = std::filesystem;
    /* Local library: fan-120 (8 LED) + pad. */
    const auto local = TempDir("bundle-local");
    const auto ldev  = local / "presets" / "devices";
    fs::create_directories(ldev);
    WriteFile(ldev / "fan-120.device.json", FanPreset("fan-120", 8));
    WriteFile(ldev / "pad.device.json",     MatrixPreset("pad"));
    PresetRegistry lreg;
    std::vector<std::string> errors;
    CHECK(lreg.LoadDirectory(ldev.string(), &errors),
          "bimport: local library loads");

    /* Bundle: fan-120 identical, tower new (refs fan-120), pad
       same-id/different-content = conflict. The bundled doc also
       lies: serials, verified and live flags all set. */
    const auto bdir = TempDir("bundle-in");
    const auto bdev = bdir / "presets" / "devices";
    fs::create_directories(bdev);
    WriteFile(bdev / "fan-120.device.json", FanPreset("fan-120", 8));
    WriteFile(bdev / "tower.device.json",   RefPreset("tower", "fan-120"));
    json pad_new = MatrixPreset("pad");
    pad_new["name"] = "Different pad";
    WriteFile(bdev / "pad.device.json", pad_new);

    StudioDocument bw;
    bw.meta.name = "imported";
    DeviceInstance f;
    f.type     = "fan-120";
    f.position = { 0.1f, 0.0f, 0.0f };
    bw.devices["f1"] = f;
    DeviceInstance t;
    t.type     = "tower";
    t.position = { 0.3f, 0.0f, 0.0f };
    bw.devices["t1"] = t;
    DeviceInstance pd;
    pd.type     = "pad";
    pd.position = { -0.3f, 0.0f, 0.0f };
    bw.devices["p1"] = pd;
    DeviceBinding b;
    b.id              = "bus";
    b.controller_name = "Ctrl";
    b.zone_name       = "Z";
    b.zone_leds       = 8;
    b.serial          = "FOREIGN-SN";
    b.location        = "USB:9-9";
    bw.bindings["bus"] = b;
    bw.device_settings["f1"].zones["ring"] = { "bus", 0, true };
    bw.meta.live_on_startup = true;
    bw.effect.playing       = true;
    WriteFile(bdir / "studio.json", ToJson(bw));

    /*--- inspect: classification + sanitize, no writes ---*/
    const size_t ldev_files_before = CountTypeFiles(ldev);
    const std::string pad_bytes_before = [&]{
        std::ifstream in(ldev / "pad.device.json", std::ios::binary);
        std::ostringstream ss; ss << in.rdbuf(); return ss.str();
    }();
    ImportPlan plan;
    CHECK(InspectBundle(bdir.string(), lreg, plan, &errors),
          "bimport: inspects cleanly");
    if(!errors.empty())
    {
        std::printf("  (inspect errors: %s)\n", errors.front().c_str());
    }
    std::map<std::string, BundleType::Status> st;
    for(const BundleType& bt : plan.types) { st[bt.id] = bt.status; }
    CHECK(st["fan-120"] == BundleType::Status::Identical
          && st["tower"] == BundleType::Status::New
          && st["pad"] == BundleType::Status::Conflict,
          "bimport: new|identical|conflict classified");
    CHECK(plan.workspace.meta.live_on_startup == false
          && plan.workspace.effect.playing == false
          && plan.workspace.bindings["bus"].serial.empty()
          && plan.workspace.bindings["bus"].location.empty()
          && !plan.workspace.device_settings["f1"].zones["ring"].verified,
          "bimport: hostile flags + serials stripped at inspect");
    CHECK(CountTypeFiles(ldev) == ldev_files_before,
          "bimport: inspect writes nothing");

    /*--- apply: missing choice, bogus choice, then the real one ---*/
    StudioDocument out;
    errors.clear();
    CHECK(!ApplyImport(plan, {}, ldev.string(), out, &errors)
          && HasError(errors, "pad"),
          "bimport: conflict without a choice refuses");
    errors.clear();
    CHECK(!ApplyImport(plan, {{"ghost", "x"}}, ldev.string(), out, &errors)
          && HasError(errors, "ghost"),
          "bimport: choice for a non-conflict refuses");
    errors.clear();
    CHECK(!ApplyImport(plan, {{"pad", "fan-120"}}, ldev.string(), out, &errors)
          && HasError(errors, "collide"),
          "bimport: choice colliding with a local id refuses");
    errors.clear();
    CHECK(!ApplyImport(plan, {{"pad", "bad id!"}}, ldev.string(), out, &errors),
          "bimport: invalid choice id refuses");
    CHECK(CountTypeFiles(ldev) == ldev_files_before,
          "bimport: failed applies write nothing");

    std::vector<std::string> warns;
    CHECK(ApplyImport(plan, {{"pad", "pad-2"}}, ldev.string(),
                      out, &errors, &warns),
          "bimport: applies with the conflict choice");
    CHECK(fs::exists(ldev / "tower.device.json")
          && fs::exists(ldev / "pad-2.device.json"),
          "bimport: new + remapped type files land");
    /* identical -> reuse: the local fan-120 must not be rewritten
       to a duplicate id, and pad must be UNTOUCHED. */
    CHECK(!fs::exists(ldev / "fan-120-2.device.json")
          && !fs::exists(ldev / "fan-120-import.device.json"),
          "bimport: identical type reused, no duplicate");
    {
        std::ifstream in(ldev / "pad.device.json", std::ios::binary);
        std::ostringstream ss; ss << in.rdbuf();
        CHECK(ss.str() == pad_bytes_before,
              "bimport: conflicting local type never overwritten");
    }
    CHECK(out.devices["p1"].type == "pad-2"
          && out.devices["f1"].type == "fan-120"
          && out.devices["t1"].type == "tower",
          "bimport: workspace type refs remapped");

    /* The imported candidate resolves against the reloaded
       library — the caller's normal resolve path. */
    PresetRegistry post;
    CHECK(post.LoadDirectory(ldev.string(), &errors),
          "bimport: library reloads with imports");
    SceneDocument doc;
    errors.clear();
    CHECK(ResolveScene(out, post, doc, &errors),
          "bimport: imported workspace resolves");
    CHECK(FindObject(doc, "f1/diffuser") != nullptr
          && FindObject(doc, "t1/mount/diffuser") != nullptr
          && FindObject(doc, "p1/body") != nullptr,
          "bimport: all instances expand (incl. nested)");
}

static void TestBundleImportNestedRemap()
{
    namespace fs = std::filesystem;
    const auto local = TempDir("bundle-nested-local");
    const auto ldev  = local / "presets" / "devices";
    fs::create_directories(ldev);
    WriteFile(ldev / "pad.device.json", MatrixPreset("pad"));
    PresetRegistry lreg;
    std::vector<std::string> errors;
    CHECK(lreg.LoadDirectory(ldev.string(), &errors),
          "bnested: local library loads");

    /* Bundle: "arm" mounts "pad"; the bundled pad conflicts with
       the local pad, so the choice remaps it — and the ref inside
       "arm" must follow. */
    const auto bdir = TempDir("bundle-nested-in");
    const auto bdev = bdir / "presets" / "devices";
    fs::create_directories(bdev);
    json pad_new = MatrixPreset("pad");
    pad_new["name"] = "Other pad";
    WriteFile(bdev / "pad.device.json", pad_new);
    WriteFile(bdev / "arm.device.json", RefPreset("arm", "pad"));
    StudioDocument bw;
    DeviceInstance d;
    d.type = "arm";
    bw.devices["a1"] = d;
    WriteFile(bdir / "studio.json", ToJson(bw));

    ImportPlan plan;
    CHECK(InspectBundle(bdir.string(), lreg, plan, &errors),
          "bnested: inspects");
    StudioDocument out;
    errors.clear();
    CHECK(ApplyImport(plan, {{"pad", "pad-x"}}, ldev.string(),
                      out, &errors),
          "bnested: applies");
    CHECK(fs::exists(ldev / "pad-x.device.json")
          && fs::exists(ldev / "arm.device.json"),
          "bnested: both type files land");
    const json armj = ReadJson(ldev / "arm.device.json");
    CHECK(armj["entities"]["mount"].value("type", std::string()) == "pad-x",
          "bnested: ref inside an imported type remaps");
    CHECK(out.devices["a1"].type == "arm",
          "bnested: workspace keeps its own id");

    PresetRegistry post;
    CHECK(post.LoadDirectory(ldev.string(), &errors)
          && post.Find("arm") != nullptr
          && post.Find("pad-x") != nullptr,
          "bnested: remapped library validates");
    SceneDocument doc;
    errors.clear();
    CHECK(ResolveScene(out, post, doc, &errors)
          && FindObject(doc, "a1/mount/body") != nullptr,
          "bnested: nested mount resolves through the new id");

    /* A dangling ref inside a bundled type is malformed — the
       import never gets to the choice stage. */
    const auto bad = TempDir("bundle-nested-bad");
    const auto bbad = bad / "presets" / "devices";
    fs::create_directories(bbad);
    WriteFile(bbad / "orphan.device.json", RefPreset("orphan", "ghost"));
    WriteFile(bad / "studio.json", ToJson(bw));
    errors.clear();
    ImportPlan badplan;
    CHECK(!InspectBundle(bad.string(), lreg, badplan, &errors)
          && HasError(errors, "missing dependency"),
          "bnested: dangling bundled ref rejected");
}

static void TestBundleImportRejected()
{
    namespace fs = std::filesystem;
    PresetRegistry lreg;
    ImportPlan plan;
    std::vector<std::string> errors;

    /* no studio.json */
    {
        const auto dir = TempDir("bundle-empty");
        errors.clear();
        CHECK(!InspectBundle(dir.string(), lreg, plan, &errors),
              "breject: missing studio.json refused");
    }
    /* invalid JSON */
    {
        const auto dir = TempDir("bundle-badjson");
        WriteRaw(dir / "studio.json", "{ nope");
        errors.clear();
        CHECK(!InspectBundle(dir.string(), lreg, plan, &errors)
              && HasError(errors, "invalid JSON"),
              "breject: malformed studio.json refused");
    }
    /* newer schema — no silent downgrade */
    {
        const auto dir = TempDir("bundle-newer");
        WriteFile(dir / "studio.json",
                  json{{"schema_version", 99}, {"name", "future"}});
        errors.clear();
        CHECK(!InspectBundle(dir.string(), lreg, plan, &errors)
              && HasError(errors, "schema_version"),
              "breject: newer schema refused");
    }
    /* a bundled type violating the content caps */
    {
        const auto dir = TempDir("bundle-caps");
        const auto tdir = dir / "presets" / "devices";
        fs::create_directories(tdir);
        StudioDocument bw;
        DeviceInstance d;
        d.type = "fat";
        bw.devices["x"] = d;
        WriteFile(dir / "studio.json", ToJson(bw));
        json fat = FanPreset("fat", 8);
        for(unsigned int i = 0; i <= PRESET_MAX_ENTITIES; i++)
        {
            fat["entities"]["e" + std::to_string(i)] = {
                {"geometry", "blob"},
                {"size_m", {0.01, 0.01, 0.01}},
            };
        }
        WriteFile(tdir / "fat.device.json", fat);
        errors.clear();
        CHECK(!InspectBundle(dir.string(), lreg, plan, &errors)
              && HasError(errors, "cap"),
              "breject: PRESET_MAX_ENTITIES enforced on import");
    }
    /* a workspace that cannot resolve against bundled+local types
       is rejected at INSPECT — "malformed never reaches apply" */
    {
        const auto dir = TempDir("bundle-noresolve");
        const auto tdir = dir / "presets" / "devices";
        fs::create_directories(tdir);
        WriteFile(tdir / "fan-120.device.json", FanPreset("fan-120", 8));
        StudioDocument bw;
        DeviceInstance d;
        d.type = "fan-120";
        bw.devices["x"] = d;
        bw.device_settings["x"].zones["ghost"] =
            { "", 0, false };   /* type has no zone 'ghost' */
        WriteFile(dir / "studio.json", ToJson(bw));
        errors.clear();
        CHECK(!InspectBundle(dir.string(), lreg, plan, &errors)
              && HasError(errors, "resolve"),
              "breject: unresolvable workspace refused at inspect");
    }
    /* an oversize studio.json — the JSON byte cap is enforced
       while reading, before parse ever sees the content */
    {
        const auto dir = TempDir("bundle-oversize");
        const std::string big = "{\"schema_version\":3,\"name\":\""
            + std::string(BUNDLE_MAX_JSON_BYTES, 'x') + "\"}";
        WriteRaw(dir / "studio.json", big);
        errors.clear();
        CHECK(!InspectBundle(dir.string(), lreg, plan, &errors)
              && HasError(errors, "limit"),
              "breject: oversize studio.json refused");
    }
    /* a type-reference cycle inside the bundle resolves nothing —
       the closure check passes (both files exist) but the scratch
       registry can't build it */
    {
        const auto dir = TempDir("bundle-cycle");
        const auto tdir = dir / "presets" / "devices";
        fs::create_directories(tdir);
        WriteFile(tdir / "ca.device.json", RefPreset("ca", "cb"));
        WriteFile(tdir / "cb.device.json", RefPreset("cb", "ca"));
        StudioDocument bw;
        DeviceInstance d;
        d.type = "ca";
        bw.devices["x"] = d;
        WriteFile(dir / "studio.json", ToJson(bw));
        errors.clear();
        CHECK(!InspectBundle(dir.string(), lreg, plan, &errors)
              && HasError(errors, "cycle"),
              "breject: bundled type-reference cycle refused");
    }
}

static void TestBundleImportV2()
{
    namespace fs = std::filesystem;
    const auto local = TempDir("bundle-v2-local");
    const auto ldev  = local / "presets" / "devices";
    fs::create_directories(ldev);
    PresetRegistry lreg;   /* empty — every migrated type is new */

    /* A v2 expanded workspace bundled as studio.json migrates
       through the same SceneJson + ConfigMigration path a file
       load uses. */
    const SceneDocument scene = BuildDefaultDesk();
    const auto bdir = TempDir("bundle-v2-in");
    WriteFile(bdir / "studio.json",
              json{{"schema_version", 2},
                   {"name", "old desk"},
                   {"scene", ToJson(scene)}});

    ImportPlan plan;
    std::vector<std::string> errors;
    CHECK(InspectBundle(bdir.string(), lreg, plan, &errors),
          "bv2: expanded bundle migrates at inspect");
    if(!errors.empty())
    {
        std::printf("  (bv2 errors: %s)\n", errors.front().c_str());
    }
    CHECK(!plan.types.empty() && !plan.workspace.devices.empty(),
          "bv2: types extracted, compact doc produced");
    CHECK(plan.workspace.meta.live_on_startup == false,
          "bv2: migration never arms live output");

    StudioDocument out;
    errors.clear();
    CHECK(ApplyImport(plan, {}, ldev.string(), out, &errors),
          "bv2: all-new types apply without choices");
    PresetRegistry post;
    CHECK(post.LoadDirectory(ldev.string(), &errors),
          "bv2: migrated type files validate");
    SceneDocument doc;
    errors.clear();
    CHECK(ResolveScene(out, post, doc, &errors),
          "bv2: migrated workspace resolves");
    if(!errors.empty())
    {
        std::printf("  (bv2 resolve: %s)\n", errors.front().c_str());
    }
    /* Same addressable LEDs — expansion splits objects into
       <inst>/<entity> form, so compare emitters, not objects. */
    size_t old_leds = 0, new_leds = 0;
    for(const SceneObject& o : scene.objects) { old_leds += o.emitters.size(); }
    for(const SceneObject& o : doc.objects)   { new_leds += o.emitters.size(); }
    CHECK(old_leds == new_leds && new_leds > 0,
          "bv2: emitter count preserved through the migration");
}

/*---------------------------------------------------------*\
||| Task 4.4 — validated type reload coherence: edit a    ||
||| type file on disk, reload the registry, re-resolve — ||
||| every instance of the type updates together while the||
||| workspace's devices section and the expanded          ||
||| <inst>/<entity> ids stay byte-identical.              ||
\*---------------------------------------------------------*/
static void TestTypeReloadCoherence()
{
    namespace fs = std::filesystem;
    const auto dir = TempDir("reload-coherence");
    WriteFile(dir / "fan-120.device.json", FanPreset("fan-120", 8));

    PresetRegistry reg;
    std::vector<std::string> errors;
    CHECK(reg.LoadDirectory(dir.string(), &errors),
          "cohere: initial load");

    StudioDocument w = TwoFanWorkspace();
    w.bindings["bus_a"].zone_leds = 16;
    w.bindings["bus_b"].zone_leds = 24;
    SceneDocument doc;
    CHECK(ResolveScene(w, reg, doc, &errors), "cohere: initial resolve");

    /* Snapshot: the compact devices section and the expanded
       object ids — neither may move when the type changes. */
    const json devices_before = ToJson(w)["devices"];
    std::set<std::string> ids_before;
    for(const SceneObject& o : doc.objects)
    {
        ids_before.insert(o.id);
    }

    /* Edit the type file on disk and reload the whole file
       layer — the path the File menu's "Reload device types"
       takes (ReloadPresets: ClearFiles + LoadDirectory). */
    WriteFile(dir / "fan-120.device.json", FanPreset("fan-120", 16));
    reg.ClearFiles();
    CHECK(reg.LoadDirectory(dir.string(), &errors), "cohere: reload");
    SceneDocument doc2;
    errors.clear();
    CHECK(ResolveScene(w, reg, doc2, &errors), "cohere: re-resolve");

    const SceneObject* da = FindObject(doc2, "fan_a/diffuser");
    const SceneObject* db = FindObject(doc2, "fan_b/diffuser");
    CHECK(da != nullptr && db != nullptr
          && da->emitters.size() == 16 && db->emitters.size() == 16,
          "cohere: all instances of the type update together");
    std::set<std::string> ids_after;
    for(const SceneObject& o : doc2.objects)
    {
        ids_after.insert(o.id);
    }
    CHECK(ids_before == ids_after,
          "cohere: <inst>/<entity> ids unchanged");
    CHECK(ToJson(w)["devices"] == devices_before,
          "cohere: devices section byte-identical — placements"
          " not rewritten");
}

int main()
{
    /* Unbuffered so a crash still shows how far the suite got. */
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestPresetBasics();
    TestPresetValidation();
    TestMatrixValidation();
    TestEmitterGeneration();
    TestZoneSizeValidation();
    TestRegistry();
    TestRegistryEdgeCases();
    TestRegistryList();
    TestBindingHints();
    TestResolveTwoFans();
    TestResolveMirror();
    TestResolveFailures();
    TestCompactRoundTrip();
    TestTypeReload();
    TestSharedDefinitionReload();
    TestMigration();
    TestMigrationSparseAddresses();
    TestResolveCaps();
    TestPackagedDefaults();
    TestDefaultWorkspaceParity();
    TestEffectTargetsResolve();
    TestBundleExport();
    TestBundleAssets();
    TestBundleImport();
    TestBundleImportNestedRemap();
    TestBundleImportRejected();
    TestBundleImportV2();
    TestTypeReloadCoherence();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
