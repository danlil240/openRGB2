/*---------------------------------------------------------*\
||| effect_json_test.cpp                                    |
|||                                                           |
|||   Task 5.1 — the JSON effect-look migration. Verifies  |
|||   the nine presets/effects/*.effect.json documents     |
|||   reproduce the pre-migration C++ builders EXACTLY:    |
|||   resolved layer stacks + evaluated frames are         |
|||   compared against tests/fixtures/effects/<id>.fixture.|
|||   json (captured from BuildPreset BEFORE the           |
|||   representation changed — the parity oracle).         |
|||                                                           |
|||   Also covers: document round-trips, the remix-spec    |
|||   grammar (including out-of-document-order misuse),    |
|||   the EffectRegistry file-over-default layering, and   |
|||   the persisted inline effects.layers workspace        |
|||   stack.                                               |
|||                                                           |
|||   No Qt, no hardware — plain cl build.                 |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "effects/EffectTypes.h"
#include "effects/EffectEngine.h"
#include "effects/EffectJson.h"
#include "effects/Presets.h"
#include "presets/EffectRegistry.h"
#include "scene/DefaultDesk.h"
#include "scene/SceneJson.h"
#include "config/StudioConfig.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace studio;
using nlohmann::json;

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, name)                                              \
    do {                                                               \
        ++checks;                                                      \
        if(!(cond)) { ++failures; std::printf("FAIL: %s\n", name); }   \
    } while(0)

static bool Near(float a, float b, float eps = 1e-6f)
{
    return std::fabs(a - b) < eps;
}

/*---------------------------------------------------------*\
|||| Tolerant parity compare — the fixture oracle was     |
|||| captured under MSVC. Other toolchains produce ULP-   |
|||| level drift in float math (libm trig + fp            |
|||| contraction), so bit-exact JSON equality is not      |
|||| portable. A ~1e-5 relative tolerance still fails on  |
|||| any semantic regression (wrong primitive, missing    |
|||| layer, palette change) while absorbing FP noise.     |
|\*---------------------------------------------------------*/
static bool JsonNear(const json& a, const json& b, double eps = 1e-5)
{
    if(a.is_number() && b.is_number())
    {
        const double x = a.get<double>(), y = b.get<double>();
        const double m = std::fabs(x) > std::fabs(y)
                       ? std::fabs(x) : std::fabs(y);
        return std::fabs(x - y) <= eps * (m > 1.0 ? m : 1.0);
    }
    if(a.is_array() && b.is_array() && a.size() == b.size())
    {
        for(size_t i = 0; i < a.size(); ++i)
        {
            if(!JsonNear(a[i], b[i], eps))
            {
                return false;
            }
        }
        return true;
    }
    if(a.is_object() && b.is_object() && a.size() == b.size())
    {
        for(auto it = a.begin(); it != a.end(); ++it)
        {
            if(!b.contains(it.key())
               || !JsonNear(it.value(), b[it.key()], eps))
            {
                return false;
            }
        }
        return true;
    }
    return a == b;
}

/* Frame colors serialize as "#RRGGBB" — quantized to integer
   channels, so ULP drift can only flip a channel by one count.
   Treat a per-channel delta of <= 1 as parity. */
static bool HexNear(const std::string& a, const std::string& b)
{
    if(a == b)
    {
        return true;
    }
    if(a.size() != 7 || b.size() != 7 || a[0] != '#' || b[0] != '#')
    {
        return false;
    }
    for(size_t i = 1; i < 7; i += 2)
    {
        const long ca = std::strtol(a.substr(i, 2).c_str(), nullptr, 16);
        const long cb = std::strtol(b.substr(i, 2).c_str(), nullptr, 16);
        if(std::labs(ca - cb) > 1)
        {
            return false;
        }
    }
    return true;
}

static bool FramesNear(const json& want, const json& got)
{
    if(!want.is_object() || !got.is_object()
       || want.size() != got.size())
    {
        return false;
    }
    for(auto it = want.begin(); it != want.end(); ++it)
    {
        const json gw = got.value(it.key(), json());
        if(!gw.is_array() || !it.value().is_array()
           || gw.size() != it.value().size())
        {
            return false;
        }
        for(size_t i = 0; i < it.value().size(); ++i)
        {
            if(!it.value()[i].is_string() || !gw[i].is_string()
               || !HexNear(it.value()[i].get<std::string>(),
                           gw[i].get<std::string>()))
            {
                return false;
            }
        }
    }
    return true;
}

/*---------------------------------------------------------*\
||| Paths — fixtures + packaged effect files resolve       |
||| relative to the test cwd (tests/) with env overrides   |
||| for unusual layouts. NEVER touch %APPDATA%.            |
\*---------------------------------------------------------*/
static std::string FixtureDir()
{
    if(const char* env = std::getenv("DLS_FIXTURE_DIR"))
    {
        if(*env != '\0') { return env; }
    }
    return "fixtures/effects";
}

static std::string PackagedDir()
{
    if(const char* env = std::getenv("DLS_EFFECT_DIR"))
    {
        if(*env != '\0') { return env; }
    }
    return "../plugins/DesktopLightingStudio/presets/effects";
}

static std::string FixtureDirAlt() { return "tests/fixtures/effects"; }

static std::string PackagedDirAlt()
{
    return "plugins/DesktopLightingStudio/presets/effects";
}

static bool LoadJsonFile(const std::string& path, json& out)
{
    std::ifstream f(path, std::ios::binary);
    if(!f.is_open())
    {
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    out = json::parse(ss.str(), nullptr, false);
    return !out.is_discarded();
}

/* Resolve the fixture dir across the two cwds the suites run
   under (tests/ and repo root). */
static std::string FindFixtureDir()
{
    std::error_code ec;
    if(std::filesystem::is_directory(FixtureDir(), ec))
    {
        return FixtureDir();
    }
    if(std::filesystem::is_directory(FixtureDirAlt(), ec))
    {
        return FixtureDirAlt();
    }
    return FixtureDir();
}

static std::string FindPackagedDir()
{
    std::error_code ec;
    if(std::filesystem::is_directory(PackagedDir(), ec))
    {
        return PackagedDir();
    }
    if(std::filesystem::is_directory(PackagedDirAlt(), ec))
    {
        return PackagedDirAlt();
    }
    return PackagedDir();
}

/*---------------------------------------------------------*\
||| Canonical frame dump — same shape + FNV-1a 64 hash as |
||| the fixture generator.                                 |
\*---------------------------------------------------------*/
static json FramesJson(const FrameColors& frame)
{
    json j = json::object();
    for(const auto& kv : frame)
    {
        json arr = json::array();
        for(SceneColor c : kv.second)
        {
            arr.push_back(SceneColorHex(c));
        }
        j[kv.first] = arr;
    }
    return j;
}

static std::string HashFrame(const FrameColors& frame)
{
    const std::string  s = FramesJson(frame).dump();
    unsigned long long h = 1469598103934665603ull;
    for(unsigned char c : s)
    {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", h);
    return buf;
}

static InputState InputFromJson(const json& j)
{
    InputState in;
    in.audio_level = (float)j.value("audio_level", 0.0);
    for(const json& je : j.value("events", json::array()))
    {
        InputEvent e;
        e.t        = je.value("t", 0.0);
        e.source   = je.value("source", std::string());
        e.has_pos  = je.value("has_pos", false);
        e.strength = (float)je.value("strength", 1.0);
        e.code     = je.value("code", 0);
        const json& p = je.value("pos", json::array());
        if(p.is_array() && p.size() == 3)
        {
            e.pos = { (float)p[0].get<double>(),
                      (float)p[1].get<double>(),
                      (float)p[2].get<double>() };
        }
        in.events.push_back(e);
    }
    in.screen_cols = j.value("screen_cols", 0);
    in.screen_rows = j.value("screen_rows", 0);
    for(const json& jc : j.value("screen_cells", json::array()))
    {
        if(jc.is_array() && jc.size() >= 3)
        {
            in.screen_cells.push_back({
                (float)jc[0].get<double>(), (float)jc[1].get<double>(),
                (float)jc[2].get<double>(),
                jc.size() > 3 ? (float)jc[3].get<double>() : 1.0f });
        }
    }
    return in;
}

/*---------------------------------------------------------*\
||| Parity — resolved stacks + evaluated frames against   |
||| the pre-change fixture oracle.                         |
\*---------------------------------------------------------*/
static void TestFixtureParity()
{
    static const char* const ids[] = {
        "aurora", "reactor", "comet", "chrome", "portal",
        "embers", "shockwave", "keyripple", "ambient",
    };
    const std::string dir = FindFixtureDir();
    const SceneDocument doc = BuildDefaultDesk();
    EffectEngine engine;
    int cases_total = 0;

    for(const char* id : ids)
    {
        json fx;
        const std::string path = dir + "/" + id + ".fixture.json";
        CHECK(LoadJsonFile(path, fx), (std::string("fixture loads: ") + id).c_str());
        if(fx.is_discarded() || fx.empty())
        {
            continue;
        }
        CHECK(fx.value("preset", std::string()) == id,
              (std::string("fixture preset id: ") + id).c_str());

        /* Resolved stacks — every captured seed. */
        const json stacks = fx.value("stacks", json::object());
        int stack_cases = 0;
        for(auto it = stacks.begin(); it != stacks.end(); ++it)
        {
            const unsigned int seed =
                (unsigned int)std::stoul(it.key());
            const std::vector<EffectLayer> built = BuildPreset(id, seed);
            const json got = EffectLayersToJson(built);
            if(!JsonNear(got, it.value()))
            {
                ++failures;
                std::printf("FAIL: stack parity %s seed %s\n",
                            id, it.key().c_str());
                ++checks;
                /* Localize the first differing layer. */
                for(size_t i = 0; i < got.size()
                                 && i < it.value().size(); i++)
                {
                    if(!JsonNear(got[i], it.value()[i]))
                    {
                        std::printf("  layer %d:\n    want %s\n"
                                    "    got  %s\n", (int)i,
                                    it.value()[i].dump().c_str(),
                                    got[i].dump().c_str());
                        break;
                    }
                }
            }
            else
            {
                ++checks;
            }
            ++stack_cases;
        }
        CHECK(stack_cases == 4,
              (std::string("four seed stacks: ") + id).c_str());

        /* Frame cases — rebuilt through the JSON path, evaluated
           against the same scene + input. */
        for(const json& c : fx.value("cases", json::array()))
        {
            const unsigned int seed = c.value("seed", 0u);
            const double t          = c.value("t", 0.0);
            engine.SetLayers(BuildPreset(id, seed));
            FrameColors frame;
            if(c.contains("input") && c["input"].is_object())
            {
                const InputState in = InputFromJson(c["input"]);
                engine.Evaluate(doc, t, frame, &in);
            }
            else
            {
                engine.Evaluate(doc, t, frame);
            }
            ++cases_total;
            const std::string want_hash = c.value("frame_hash", std::string());
            const std::string got_hash  = HashFrame(frame);
            if(got_hash != want_hash
               && !FramesNear(c.value("frames", json::object()),
                              FramesJson(frame)))
            {
                ++failures;
                std::printf("FAIL: frame hash %s seed %u t %.2f\n"
                            "    want %s got %s\n",
                            id, seed, t, want_hash.c_str(),
                            got_hash.c_str());
                /* Spot-check: first differing emitter color. */
                const json want = c.value("frames", json::object());
                const json got  = FramesJson(frame);
                for(auto it = want.begin(); it != want.end(); ++it)
                {
                    const json gw = got.value(it.key(), json::array());
                    if(!gw.is_array() || it.value().size() != gw.size()
                       || !FramesNear(json::object({{it.key(), it.value()}}),
                                      json::object({{it.key(), gw}})))
                    {
                        for(size_t i = 0; i < it.value().size()
                                         && i < gw.size(); i++)
                        {
                            if(!gw[i].is_string()
                               || !it.value()[i].is_string()
                               || !HexNear(gw[i].get<std::string>(),
                                           it.value()[i].get<std::string>()))
                            {
                                std::printf("    %s[%d] want %s"
                                            " got %s\n",
                                            it.key().c_str(), (int)i,
                                            it.value()[i].dump().c_str(),
                                            gw[i].dump().c_str());
                                break;
                            }
                        }
                        break;
                    }
                }
            }
            ++checks;
        }
    }
    std::printf("parity: %d frame cases checked\n", cases_total);
    CHECK(cases_total == 144, "144 fixture cases");
}

/*---------------------------------------------------------*\
||| Document round-trip + the packaged file set.           |
\*---------------------------------------------------------*/
static void TestPackagedDocs()
{
    const std::string dir = FindPackagedDir();
    static const char* const ids[] = {
        "aurora", "reactor", "comet", "chrome", "portal",
        "embers", "shockwave", "keyripple", "ambient",
    };
    for(const char* id : ids)
    {
        const std::string path = dir + "/" + id + ".effect.json";
        EffectDocument d;
        std::vector<std::string> errs;
        CHECK(EffectDocumentFromJsonFile(path, d, &errs),
              (std::string("packaged parses: ") + id).c_str());
        CHECK(d.id == id, (std::string("id == filename: ") + id).c_str());
        /* parse -> serialize -> parse is stable. */
        const nlohmann::ordered_json j = EffectDocumentToJson(d);
        EffectDocument d2;
        CHECK(EffectDocumentFromJson(j, d2, &errs),
              (std::string("round-trip parses: ") + id).c_str());
        CHECK(d2.id == d.id && d2.name == d.name
              && d2.description == d.description && d2.needs == d.needs
              && d2.layers == d.layers,
              (std::string("round-trip stable: ") + id).c_str());
        /* Every seed resolves a non-empty stack. */
        std::vector<EffectLayer> layers;
        CHECK(ResolveEffectLayers(d, 7, layers, &errs) && !layers.empty(),
              (std::string("packaged resolves: ") + id).c_str());
    }

    /* Registry-backed listing reproduces the shipped metadata. */
    CHECK(PresetList().size() == 9, "nine looks registered");
    const PresetInfo* p = FindPreset("shockwave");
    CHECK(p != nullptr && p->needs == "audio" && p->name == "Bass shockwave",
          "shockwave metadata from JSON");
}

/*---------------------------------------------------------*\
||| Validation — the error matrix.                         |
\*---------------------------------------------------------*/
static nlohmann::ordered_json MinimalDoc()
{
    nlohmann::ordered_json j;
    j["schema_version"] = 1;
    j["id"]             = "test";
    j["layers"]         = nlohmann::ordered_json::array();
    nlohmann::ordered_json l;
    l["primitive"] = "static";
    nlohmann::ordered_json stop;
    stop["pos"] = 0.0; stop["color"] = "#102030";
    l["palette"] = nlohmann::ordered_json::array({ stop });
    j["layers"].push_back(l);
    return j;
}

static void TestValidation()
{
    EffectDocument d;
    std::vector<std::string> errs;

    /* malformed JSON file */
    {
        const std::string dir =
            (std::filesystem::temp_directory_path()
             / "dls_fx_malformed").string();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::string path = dir + "/bad.effect.json";
        {
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            f << "{ not json ]";
        }
        CHECK(!EffectDocumentFromJsonFile(path, d, &errs),
              "malformed JSON rejected");
        std::filesystem::remove_all(dir, ec);
    }

    /* wrong schema version */
    {
        nlohmann::ordered_json j = MinimalDoc();
        j["schema_version"] = 2;
        CHECK(!EffectDocumentFromJson(j, d, &errs)
              && !errs.empty(),
              "schema_version != 1 rejected");
    }

    /* id charset */
    {
        nlohmann::ordered_json j = MinimalDoc();
        j["id"] = "bad id!";
        CHECK(!EffectDocumentFromJson(j, d, &errs), "bad id rejected");
    }

    /* unknown primitive */
    {
        nlohmann::ordered_json j = MinimalDoc();
        j["layers"][0]["primitive"] = "fireworks";
        CHECK(!EffectDocumentFromJson(j, d, &errs),
              "unknown primitive rejected");
    }

    /* non-finite scalar (constructed — JSON text can't spell it) */
    {
        nlohmann::ordered_json j = MinimalDoc();
        j["layers"][0]["speed"] =
            std::numeric_limits<double>::infinity();
        CHECK(!EffectDocumentFromJson(j, d, &errs),
              "non-finite speed rejected");
    }

    /* out-of-range opacity */
    {
        nlohmann::ordered_json j = MinimalDoc();
        j["layers"][0]["opacity"] = 1.5;
        CHECK(!EffectDocumentFromJson(j, d, &errs),
              "opacity > 1 rejected");
    }

    /* scale must be > 0 */
    {
        nlohmann::ordered_json j = MinimalDoc();
        j["layers"][0]["scale"] = 0.0;
        CHECK(!EffectDocumentFromJson(j, d, &errs),
              "scale 0 rejected");
        j["layers"][0]["scale"] = {{"remix", {-1.0, 0.5}}};
        CHECK(!EffectDocumentFromJson(j, d, &errs),
              "remix range crossing 0 on scale rejected");
    }

    /* A finite-positive double that underflows to 0.0f at float
       width still violates scale's "> 0" contract — the double-
       domain bound check passes it, then narrowing produces the
       divisor EvalWave/EvalPulse would divide by. */
    {
        nlohmann::ordered_json j = MinimalDoc();
        j["layers"][0]["scale"] = 1e-46;
        CHECK(!EffectDocumentFromJson(j, d, &errs),
              "scale 1e-46 (narrows to 0.0f) rejected");
        j = MinimalDoc();
        j["layers"][0]["scale"] = {{"remix", {1e-46, 0.5}}};
        CHECK(!EffectDocumentFromJson(j, d, &errs),
              "remix lo bound 1e-46 on scale rejected");
        /* Same check on the resolved inline-stack path — both go
           through ParseLayer. */
        json layer;
        layer["primitive"] = "wave";
        layer["scale"]     = 1e-46;
        std::vector<EffectLayer> out;
        CHECK(!EffectLayersFromJson(json::array({ layer }), out, &errs),
              "inline scale 1e-46 rejected");
        /* Positive control: a denormal-but-positive float survives
           narrowing and stays a valid scale. */
        layer["scale"] = 1e-40;
        CHECK(EffectLayersFromJson(json::array({ layer }), out, &errs)
              && out.size() == 1 && out[0].scale > 0.0f,
              "inline scale 1e-40 (float-positive) accepted");
    }

    /* remix range exceeding a bounded field */
    {
        nlohmann::ordered_json j = MinimalDoc();
        j["layers"][0]["opacity"] = {{"remix", {0.0, 2.0}}};
        CHECK(!EffectDocumentFromJson(j, d, &errs),
              "remix range beyond opacity rejected");
    }

    /* spec on a field that can't draw — wrong shape, wrong place */
    {
        nlohmann::ordered_json j = MinimalDoc();
        j["layers"][0]["targets"] = {{"remix", {0, 1}}};
        CHECK(!EffectDocumentFromJson(j, d, &errs),
              "remix on targets rejected");
        j["layers"][0].erase("targets");
        j["layers"][0]["palette"][0]["color"] = {{"remix", {0, 1}}};
        CHECK(!EffectDocumentFromJson(j, d, &errs),
              "remix on palette color rejected");
        j = MinimalDoc();
        j["layers"][0]["origin"] = {{"remix_yaw",
                                     {0, 0, 0, -1, 1}}};
        CHECK(!EffectDocumentFromJson(j, d, &errs),
              "remix_yaw on origin rejected");
        j = MinimalDoc();
        j["layers"][0]["speed"] = {{"remix", {0, 1}},
                                   {"remix01", true}};
        CHECK(!EffectDocumentFromJson(j, d, &errs),
              "two spec keys on one field rejected");
        j = MinimalDoc();
        j["layers"][0]["seed"] = {{"remix", {0, 1}}};
        CHECK(!EffectDocumentFromJson(j, d, &errs),
              "remix (float) on seed rejected");
        j = MinimalDoc();
        j["layers"][0]["bogus"] = 1;
        CHECK(!EffectDocumentFromJson(j, d, &errs),
              "unknown layer key rejected");
    }

    /* Spec objects carry EXACTLY one key — an extra key after the
       spec key used to pass SpecKeyCount and get silently dropped.
       Rejected in every field position that accepts a spec. */
    {
        nlohmann::ordered_json j = MinimalDoc();
        j["layers"][0]["speed"] = {{"remix", {0.0, 1.0}}, {"foo", 2}};
        CHECK(!EffectDocumentFromJson(j, d, &errs),
              "scalar spec + trailing key rejected");
        j = MinimalDoc();
        j["layers"][0]["speed"] = {{"foo", 2}, {"remix", {0.0, 1.0}}};
        CHECK(!EffectDocumentFromJson(j, d, &errs),
              "scalar spec + leading key rejected");
        j = MinimalDoc();
        j["layers"][0]["seed"] = {{"remix_u32", {0, 10}}, {"foo", 2}};
        CHECK(!EffectDocumentFromJson(j, d, &errs),
              "remix_u32 + extra key rejected");
        j = MinimalDoc();
        j["layers"][0]["direction"] =
            {{"remix_yaw", {1.0, 0.0, 0.0, -1.0, 1.0}}, {"foo", 2}};
        CHECK(!EffectDocumentFromJson(j, d, &errs),
              "remix_yaw + extra key rejected");
        j = MinimalDoc();
        nlohmann::ordered_json el;
        el["remix01"] = true;
        el["foo"]     = 2;
        j["layers"][0]["origin"] =
            nlohmann::ordered_json::array({ el, 0.0, 0.0 });
        CHECK(!EffectDocumentFromJson(j, d, &errs),
              "vector-element spec + extra key rejected");
        /* palette stop pos goes through the same scalar path */
        j = MinimalDoc();
        nlohmann::ordered_json s0;
        s0["pos"]   = {{"remix", {0.0, 0.5}}, {"foo", 2}};
        s0["color"] = "#102030";
        j["layers"][0]["palette"] =
            nlohmann::ordered_json::array({ s0 });
        CHECK(!EffectDocumentFromJson(j, d, &errs),
              "palette pos spec + extra key rejected");
    }

    /* Finite-double values that overflow float width are errors,
       not clamps — check AFTER narrowing like PathField does. */
    {
        nlohmann::ordered_json j = MinimalDoc();
        j["layers"][0]["speed"] = 1e300;
        CHECK(!EffectDocumentFromJson(j, d, &errs),
              "1e300 speed rejected (narrows to inf)");
        j = MinimalDoc();
        j["layers"][0]["phase"] = -1e300;
        CHECK(!EffectDocumentFromJson(j, d, &errs),
              "-1e300 phase rejected");
        j = MinimalDoc();
        j["layers"][0]["density"] = 1e300;
        CHECK(!EffectDocumentFromJson(j, d, &errs),
              "1e300 density rejected");
        j = MinimalDoc();
        j["layers"][0]["scale"] = 1e300;
        CHECK(!EffectDocumentFromJson(j, d, &errs),
              "1e300 scale rejected");
        j = MinimalDoc();
        j["layers"][0]["origin"] =
            nlohmann::ordered_json::array({ 1e300, 0.0, 0.0 });
        CHECK(!EffectDocumentFromJson(j, d, &errs),
              "1e300 vector component rejected");
        /* a remix spec whose implied domain can't fit float is
           rejected too — some seed would draw non-finite */
        j = MinimalDoc();
        j["layers"][0]["speed"] = {{"remix", {0.0, 1e300}}};
        CHECK(!EffectDocumentFromJson(j, d, &errs),
              "remix bound 1e300 rejected");
        j = MinimalDoc();
        j["layers"][0]["direction"] =
            {{"remix_yaw", {1e300, 0.0, 0.0, -1.0, 1.0}}};
        CHECK(!EffectDocumentFromJson(j, d, &errs),
              "remix_yaw component 1e300 rejected");
        /* float-finite huge values still pass — no false alarm */
        j = MinimalDoc();
        j["layers"][0]["speed"] = 1e38;
        CHECK(EffectDocumentFromJson(j, d, &errs),
              "1e38 (float-finite) speed accepted");
    }

    /* palette errors */
    {
        nlohmann::ordered_json j = MinimalDoc();
        j["layers"][0]["palette"] = nlohmann::ordered_json::array();
        CHECK(!EffectDocumentFromJson(j, d, &errs),
              "empty palette rejected");
        j = MinimalDoc();
        nlohmann::ordered_json s0, s1;
        s0["pos"] = 0.6; s0["color"] = "#FFFFFF";
        s1["pos"] = 0.4; s1["color"] = "#000000";
        j["layers"][0]["palette"] =
            nlohmann::ordered_json::array({ s0, s1 });
        CHECK(!EffectDocumentFromJson(j, d, &errs),
              "unsorted palette rejected");
    }

    /* resolved-layer context refuses remix specs */
    {
        nlohmann::ordered_json l;
        l["primitive"] = "wave";
        l["speed"]     = {{"remix", {0.1, 0.2}}};
        json jarr = json::array({ json(l) });
        std::vector<EffectLayer> out;
        CHECK(!EffectLayersFromJson(jarr, out, &errs),
              "remix spec in resolved stack rejected");
    }
}

/*---------------------------------------------------------*\
||| Draw order — the spec stream consumes in document      |
||| order; parity fixtures pin the sequence end-to-end.    |
||| These check the mechanics directly.                    |
\*---------------------------------------------------------*/
static void TestDrawOrder()
{
    /* Same document, same seed -> identical resolve. */
    nlohmann::ordered_json j = MinimalDoc();
    {
        nlohmann::ordered_json l;
        l["primitive"] = "wave";
        l["speed"]     = {{"remix", {0.1, 0.2}}};
        l["phase"]     = {{"remix01", true}};
        nlohmann::ordered_json s0;
        s0["pos"] = 0.0; s0["color"] = "#FF0000";
        l["palette"] = nlohmann::ordered_json::array({ s0 });
        j["layers"].push_back(l);
    }
    EffectDocument d;
    std::vector<std::string> errs;
    CHECK(EffectDocumentFromJson(j, d, &errs), "remix doc parses");

    std::vector<EffectLayer> a, b;
    CHECK(ResolveEffectLayers(d, 99, a, &errs), "resolve a");
    CHECK(ResolveEffectLayers(d, 99, b, &errs), "resolve b");
    CHECK(a.size() == b.size() && a.size() == 2, "two layers");
    if(a.size() == 2 && b.size() == 2)
    {
        CHECK(Near(a[1].speed, b[1].speed) && Near(a[1].phase, b[1].phase),
              "same seed same draws");
        /* The drawn speed lands inside the declared range. */
        CHECK(a[1].speed >= 0.1f - 1e-7f && a[1].speed <= 0.2f + 1e-7f,
              "remix range respected");
    }
    std::vector<EffectLayer> c;
    ResolveEffectLayers(d, 100, c, nullptr);
    if(c.size() == 2 && a.size() == 2)
    {
        CHECK(!Near(c[1].speed, a[1].speed) || !Near(c[1].phase, a[1].phase),
              "new seed varies draws");
    }

    /* Document order, not field declaration order: a layer whose
       spec keys arrive (speed, phase) draws Range then Next01 —
       moving the spec to a different key changes the stream. */
    nlohmann::ordered_json j2 = MinimalDoc();
    {
        nlohmann::ordered_json l;
        l["primitive"] = "wave";
        l["phase"]     = {{"remix01", true}};
        l["speed"]     = {{"remix", {0.1, 0.2}}};
        nlohmann::ordered_json s0;
        s0["pos"] = 0.0; s0["color"] = "#FF0000";
        l["palette"] = nlohmann::ordered_json::array({ s0 });
        j2["layers"].push_back(l);
    }
    EffectDocument d2;
    CHECK(EffectDocumentFromJson(j2, d2, &errs), "reordered doc parses");
    std::vector<EffectLayer> r;
    CHECK(ResolveEffectLayers(d2, 99, r, &errs), "resolve reordered");
    if(r.size() == 2 && a.size() == 2)
    {
        /* phase drew FIRST here (remix01 then remix) vs. speed
           first above — the streams differ, so values differ. */
        CHECK(!Near(r[1].speed, a[1].speed) || !Near(r[1].phase, a[1].phase),
              "field order drives draw order");
    }

    /* remix_u32 full range = Next() verbatim. */
    {
        nlohmann::ordered_json l;
        l["primitive"] = "noise";
        l["seed"]      = {{"remix_u32", {0, 4294967295}}};
        nlohmann::ordered_json arr = nlohmann::ordered_json::array({ l });
        std::vector<EffectLayer> out;
        CHECK(ResolveEffectLayers(arr, 5, out, &errs) && out.size() == 1,
              "remix_u32 resolves");
        RemixRng probe(5);
        CHECK(out[0].seed == probe.Next(), "remix_u32 == Next()");
    }
}

/*---------------------------------------------------------*\
||| Registry — file-over-default, bad sibling exclusion,   |
||| missing-file fallback, personal looks. Temp dirs only. |
\*---------------------------------------------------------*/
static void TestRegistry()
{
    std::error_code ec;
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "dls_fx_registry";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    /* Defaults first — file layer wins over them. */
    EffectRegistry reg;
    {
        EffectDocument d;
        d.id = "mylook"; d.name = "Default Mylook";
        nlohmann::ordered_json l;
        l["primitive"] = "static";
        nlohmann::ordered_json s0;
        s0["pos"] = 0.0; s0["color"] = "#112233";
        l["palette"] = nlohmann::ordered_json::array({ s0 });
        d.layers.push_back(l);
        reg.SetDefaults({ d });
    }
    CHECK(reg.Find("mylook") != nullptr
          && reg.Find("mylook")->name == "Default Mylook",
          "default resolves before files");

    /* A good file overriding the default + a bad sibling. */
    {
        const std::string good = (dir / "mylook.effect.json").string();
        const std::string bad  = (dir / "other.effect.json").string();
        const std::string pers = (dir / "neon.effect.json").string();
        {
            std::ofstream f(good, std::ios::binary | std::ios::trunc);
            f << "{\n"
                 "  \"schema_version\": 1,\n"
                 "  \"id\": \"mylook\",\n"
                 "  \"name\": \"File Mylook\",\n"
                 "  \"layers\": [ { \"primitive\": \"static\",\n"
                 "    \"palette\": [ { \"pos\": 0.0,\n"
                 "      \"color\": \"#445566\" } ] } ]\n"
                 "}\n";
        }
        {
            std::ofstream f(bad, std::ios::binary | std::ios::trunc);
            f << "{ \"schema_version\": 1, \"id\": \"WRONG\","
                 "  \"layers\": [] }";   /* id != filename */
        }
        {
            std::ofstream f(pers, std::ios::binary | std::ios::trunc);
            f << "{\n"
                 "  \"schema_version\": 1,\n"
                 "  \"id\": \"neon\",\n"
                 "  \"name\": \"Neon Personal\",\n"
                 "  \"layers\": [ { \"primitive\": \"gradient\",\n"
                 "    \"direction\": [1.0, 0.0, 0.0],\n"
                 "    \"palette\": [ { \"pos\": 0.0, \"color\": \"#FF00FF\" },\n"
                 "      { \"pos\": 0.5, \"color\": \"#00FFFF\" } ] } ]\n"
                 "}\n";
        }
        std::vector<std::string> errs;
        CHECK(!reg.LoadDirectory(dir.string(), &errs),
              "bad sibling reported");
        CHECK(reg.Find("mylook") != nullptr
              && reg.Find("mylook")->name == "File Mylook",
              "file overrides default");
        CHECK(reg.Find("other") == nullptr,
              "id != filename file excluded");
        CHECK(reg.Find("neon") != nullptr
              && reg.Find("neon")->name == "Neon Personal",
              "personal look loads");
        CHECK(!reg.FileErrors().empty(), "per-file errors recorded");
        /* The excluded file's error names it. */
        bool names_other = false;
        for(const auto& kv : reg.FileErrors())
        {
            if(kv.first.find("other") != std::string::npos)
            {
                names_other = true;
            }
        }
        CHECK(names_other, "error names the bad file");

        /* Build resolves through the file layer. */
        std::vector<EffectLayer> layers;
        CHECK(reg.Build("neon", 3, layers, &errs) && layers.size() == 1
              && layers[0].primitive == "gradient",
              "personal look builds");
    }

    /* Missing-file fallback: the default still answers when the
       file layer never saw the id. */
    {
        EffectRegistry reg2;
        EffectDocument d;
        d.id = "base"; d.name = "Base";
        nlohmann::ordered_json l;
        l["primitive"] = "static";
        nlohmann::ordered_json s0;
        s0["pos"] = 0.0; s0["color"] = "#101010";
        l["palette"] = nlohmann::ordered_json::array({ s0 });
        d.layers.push_back(l);
        reg2.SetDefaults({ d });
        const std::filesystem::path empty =
            std::filesystem::temp_directory_path() / "dls_fx_empty";
        std::filesystem::create_directories(empty, ec);
        CHECK(reg2.LoadDirectory(empty.string(), nullptr),
              "empty dir loads clean");
        std::vector<EffectLayer> layers;
        CHECK(reg2.Build("base", 0, layers, nullptr)
              && layers.size() == 1,
              "missing file falls back to default");
        /* A nonexistent dir is not an error either. */
        CHECK(reg2.LoadDirectory(
                  (empty / "no_such_dir").string(), nullptr),
              "missing dir is not an error");
    }

    /* LoadFile: id != filename rejected, registry unchanged. */
    {
        EffectRegistry reg3;
        const std::filesystem::path d3 =
            std::filesystem::temp_directory_path() / "dls_fx_one";
        std::filesystem::create_directories(d3, ec);
        const std::string p = (d3 / "named.effect.json").string();
        {
            std::ofstream f(p, std::ios::binary | std::ios::trunc);
            f << "{ \"schema_version\": 1, \"id\": \"other\","
                 "  \"layers\": [] }";
        }
        std::vector<std::string> errs;
        CHECK(!reg3.LoadFile(p, &errs), "LoadFile id mismatch rejected");
        CHECK(reg3.Ids().empty(), "failed LoadFile changes nothing");
    }

    std::filesystem::remove_all(dir, ec);
}

/*---------------------------------------------------------*\
||| Workspace effects.layers — typed authored stack.       |
\*---------------------------------------------------------*/
static void TestWorkspaceLayers()
{
    /* Authored stack + preset provenance round-trips. */
    StudioDocument w;
    w.effect.preset    = "aurora";       /* provenance            */
    w.effect.seed      = 77;
    w.effect.playing   = true;
    EffectLayer l;
    l.primitive = "wave";
    l.space     = CoordSpace::Local;
    l.blend     = BlendMode::Screen;
    l.opacity   = 0.4f;
    l.speed     = 0.25f;
    l.scale     = 0.5f;
    l.density   = 1.7f;
    l.direction = { 0.0f, 0.0f, 1.0f };
    l.targets   = { "keyboard_body" };
    l.seed      = 0xDEADBEEFu;
    l.enabled   = false;
    l.palette   = MakePalette({ MakeSceneColor(255, 0, 0),
                                MakeSceneColor(0, 0, 255) });
    w.effect.layers.push_back(l);

    const json j = ToJson(w);
    CHECK(j["effects"]["layers"].is_array()
          && j["effects"]["layers"].size() == 1,
          "inline stack serializes");
    CHECK(j["effects"]["layers"][0]["primitive"] == "wave"
          && j["effects"]["layers"][0]["enabled"] == false
          && j["effects"]["layers"][0]["seed"] == 0xDEADBEEFu,
          "layer fields serialize");

    StudioDocument back;
    std::vector<std::string> errs;
    CHECK(FromJson(j, back, &errs), "workspace with layers parses");
    CHECK(back.effect.preset == "aurora"
          && back.effect.layers.size() == 1,
          "layers + preset provenance round-trip");
    if(back.effect.layers.size() == 1)
    {
        const EffectLayer& g = back.effect.layers[0];
        CHECK(g.primitive == "wave" && g.blend == BlendMode::Screen
              && g.space == CoordSpace::Local
              && Near(g.opacity, 0.4f) && Near(g.speed, 0.25f)
              && Near(g.density, 1.7f) && g.seed == 0xDEADBEEFu
              && g.enabled == false
              && g.targets.size() == 1 && g.targets[0] == "keyboard_body"
              && g.palette.stops.size() == 2,
              "layer fields round-trip");
    }

    /* Preset-only documents still work: layers serializes as []
       and resolves to an empty stack. */
    StudioDocument w2;
    w2.effect.preset = "comet";
    const json j2 = ToJson(w2);
    CHECK(j2["effects"]["layers"].is_array()
          && j2["effects"]["layers"].empty(),
          "preset-only serializes empty layers");
    StudioDocument back2;
    CHECK(FromJson(j2, back2, &errs)
          && back2.effect.preset == "comet"
          && back2.effect.layers.empty(),
          "preset-only workspace resolves");

    /* A malformed authored layer rejects the document, not a
       silent drop. */
    json j3 = j;
    j3["effects"]["layers"][0]["primitive"] = "nope";
    StudioDocument back3;
    errs.clear();
    CHECK(!FromJson(j3, back3, &errs) && !errs.empty(),
          "bad inline layer rejected");
}

/*---------------------------------------------------------*\
||| Runtime seam — the expanded scene document carries the |
||| stack; the v2 scene format round-trips it.             |
\*---------------------------------------------------------*/
static void TestSceneDocLayers()
{
    SceneDocument doc = BuildDefaultDesk();
    EffectLayer l;
    l.primitive = "pulse";
    l.origin    = { 0.4f, 0.2f, -0.1f };
    l.palette   = MakePalette({ MakeSceneColor(0, 255, 128) });
    doc.effect.layers.push_back(l);
    doc.effect.preset = "portal";   /* provenance */

    SceneDocument back;
    std::vector<std::string> errs;
    CHECK(FromJson(ToJson(doc), back, &errs),
          "scene doc with layers parses");
    CHECK(back.effect.layers.size() == 1
          && back.effect.layers[0].primitive == "pulse"
          && back.effect.preset == "portal",
          "scene doc layers + preset round-trip");

    /* The engine honors enabled=false. */
    EffectEngine engine;
    std::vector<EffectLayer> one = { l };
    engine.SetLayers(one);
    FrameColors f1;
    engine.Evaluate(doc, 1.0, f1);
    one[0].enabled = false;
    engine.SetLayers(one);
    FrameColors f2;
    engine.Evaluate(doc, 1.0, f2);
    CHECK(!f1.empty() && f2.empty(), "enabled=false composites nothing");
}

/*---------------------------------------------------------*\
|||| Task 5.2 — the personal-look file: a resolved inline  |
|||| stack serialized as an .effect.json lands in a fresh  |
|||| registry dir, re-parses, and builds the same stack.   |
|||| WriteEffectFile itself is Qt-side; this pins the      |
|||| grammar contract it relies on.                        |
\*---------------------------------------------------------*/
static void TestPersonalLookFile()
{
    std::error_code ec;
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "dls_fx_personal";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    /* Build the look the way saveLookAs does: resolved literals
       only, canonical field order. */
    std::vector<EffectLayer> stack;
    {
        EffectLayer l;
        l.primitive = "static";
        l.palette   = MakePalette({ MakeSceneColor(8, 12, 32) });
        stack.push_back(l);
        EffectLayer w;
        w.primitive = "wave";
        w.blend     = BlendMode::Screen;
        w.opacity   = 0.8f;
        w.speed     = 0.22f;
        w.density   = 1.4f;
        w.direction = { 0.3f, 0.0f, -0.9f };
        w.targets   = { "desk" };
        w.seed      = 12345;
        w.enabled   = true;
        w.palette   = MakePalette({ MakeSceneColor(0, 200, 180),
                                    MakeSceneColor(112, 64, 224) });
        stack.push_back(w);
        EffectLayer r;
        r.primitive = "ripple";
        r.source    = "key";
        r.origin    = { 0.1f, 0.05f, -0.2f };
        r.path      = { { 0.0f, 0.0f, 0.0f }, { 0.4f, 0.0f, 0.4f } };
        stack.push_back(r);
    }
    EffectDocument d;
    d.id    = "mylook";
    d.name  = "My Look";
    d.needs = "key";
    d.layers = nlohmann::ordered_json::array();
    for(const EffectLayer& l : stack)
    {
        d.layers.push_back(
            nlohmann::ordered_json::parse(EffectLayerToJson(l).dump()));
    }

    /* Serialize -> parse -> build: the file the save path writes
       must validate and resolve the same layers. */
    const std::string path = (dir / "mylook.effect.json").string();
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << EffectDocumentToJson(d).dump(2) << "\n";
    }
    EffectDocument landed;
    std::vector<std::string> errs;
    CHECK(EffectDocumentFromJsonFile(path, landed, &errs),
          "personal look file re-parses");
    CHECK(landed.id == "mylook" && landed.needs == "key"
          && landed.layers.size() == 3,
          "personal look fields survive the file");

    EffectRegistry reg;
    CHECK(reg.LoadDirectory(dir.string(), &errs),
          "registry picks up the personal look");
    /* Keep the List() result alive — a range-for over the
       temporary would leave `info` dangling after the loop. */
    const std::vector<EffectRegistry::EffectInfo> infos =
        reg.List();
    const EffectRegistry::EffectInfo* info = nullptr;
    for(const EffectRegistry::EffectInfo& i : infos)
    {
        if(i.id == "mylook")
        {
            info = &i;
        }
    }
    CHECK(info != nullptr && info->from_file && info->name == "My Look"
          && info->needs == "key",
          "personal look lists as a file look");
    std::vector<EffectLayer> built;
    CHECK(reg.Build("mylook", 9, built, &errs) && built.size() == 3,
          "personal look builds");
    if(built.size() == 3)
    {
        CHECK(built[0].primitive == "static"
              && built[1].primitive == "wave"
              && built[1].blend == BlendMode::Screen
              && Near(built[1].opacity, 0.8f)
              && built[1].seed == 12345
              && built[1].targets.size() == 1
              && built[2].primitive == "ripple"
              && built[2].source == "key"
              && built[2].path.size() == 2,
              "saved stack resolves identically");
    }

    /* Grammar facts the save path relies on: an EMPTY layers array
       is a valid look (it contributes nothing), while a present-
       but-empty palette is not. */
    {
        nlohmann::ordered_json j;
        j["schema_version"] = 1;
        j["id"]    = "emptylook";
        j["layers"] = nlohmann::ordered_json::array();
        EffectDocument e;
        CHECK(EffectDocumentFromJson(j, e, &errs),
              "empty layers array is a valid look");
    }

    std::filesystem::remove_all(dir, ec);
}

int main()
{
    std::printf("== effect_json_test ==\n");
    TestFixtureParity();
    TestPackagedDocs();
    TestValidation();
    TestDrawOrder();
    TestRegistry();
    TestWorkspaceLayers();
    TestSceneDocLayers();
    TestPersonalLookFile();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
