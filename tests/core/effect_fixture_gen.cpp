/*---------------------------------------------------------*\
|| effect_fixture_gen.cpp                                    |
||                                                           |
||   Captures deterministic effect fixtures: for each       |
||   shipped look, resolve BuildPreset(id, seed) at a       |
||   fixed seed set, dump the resolved layer stack, and     |
||   evaluate frames at fixed times on the default-desk     |
||   scene (plus a synthetic reactive InputState for the    |
||   audio/key/screen looks). Output:                       |
||   tests/fixtures/effects/<id>.fixture.json               |
||                                                           |
||   These files are the parity oracle for the JSON effect  |
||   migration (task 5.1): the first capture ran while      |
||   BuildPreset was still the C++ builder, so they record  |
||   the pre-migration ground truth. Re-run only when a     |
||   look intentionally changes, and only for that look.    |
||                                                           |
||   Usage: effect_fixture_gen.exe [out_dir [id...]]        |
||   (no ids = every look). Run from tests/core with        |
||   DLS_EFFECT_DIR=../../src/core/presets/effects, e.g.    |
||   effect_fixture_gen fixtures/effects wave               |
||   Built on demand only (EXCLUDE_FROM_ALL):               |
||   cmake --build build --target effect_fixture_gen        |
||                                                           |
||   Imported from openrgb-unified tests/effect_fixture_gen |
||   (unchanged capture logic); wave added for audit C-11.  |
||                                                           |
||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "scene/SceneTypes.h"
#include "scene/SceneJson.h"
#include "scene/DefaultDesk.h"
#include "effects/EffectTypes.h"
#include "effects/EffectEngine.h"
#include "effects/Presets.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace studio;
using nlohmann::json;

/*---------------------------------------------------------*\
|| Canonical resolved-layer JSON — this is the format      |
|| effects/EffectJson.cpp writes for effects.layers and    |
|| the shape fixtures compare against.                     |
\*---------------------------------------------------------*/
static json Vec3Json(const Vec3& v)
{
    return json::array({ v.x, v.y, v.z });
}

static const char* SpaceName(CoordSpace s)
{
    return s == CoordSpace::Local ? "local" : "world";
}

static const char* BlendName(BlendMode b)
{
    switch(b)
    {
    case BlendMode::Add:    return "add";
    case BlendMode::Screen: return "screen";
    default:                return "replace";
    }
}

static json PaletteJson(const Palette& p)
{
    json arr = json::array();
    for(const PaletteStop& s : p.stops)
    {
        arr.push_back({{"pos",   s.pos},
                       {"color", SceneColorHex(ToSceneColor(s.color))}});
    }
    return arr;
}

static json LayerJson(const EffectLayer& L)
{
    json j;
    j["primitive"] = L.primitive;
    j["space"]     = SpaceName(L.space);
    j["blend"]     = BlendName(L.blend);
    j["opacity"]   = L.opacity;
    j["speed"]     = L.speed;
    j["scale"]     = L.scale;
    j["phase"]     = L.phase;
    j["density"]   = L.density;
    j["origin"]    = Vec3Json(L.origin);
    j["direction"] = Vec3Json(L.direction);
    json path = json::array();
    for(const Vec3& v : L.path)
    {
        path.push_back(Vec3Json(v));
    }
    j["path"] = path;
    /* palette is omitted when empty — a present-but-empty palette
       is a validation error in the format. */
    if(!L.palette.stops.empty())
    {
        j["palette"] = PaletteJson(L.palette);
    }
    json targets = json::array();
    for(const std::string& t : L.targets)
    {
        targets.push_back(t);
    }
    j["targets"] = targets;
    j["source"]  = L.source;
    j["seed"]    = L.seed;
    /* The format reserves "enabled"; every shipped layer is
       enabled, so the oracle pins true. */
    j["enabled"] = true;
    return j;
}

/*---------------------------------------------------------*\
|| Frames                                                  |
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

/* FNV-1a 64 over the canonical frames dump — a stable frame hash
   so diffs localize to one case instead of one wall of colors. */
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

/*---------------------------------------------------------*\
|| Synthetic reactive input — one fixed state exercising   |
|| audio onsets (positioned + position-less), key events    |
|| (positioned + position-less), the level signal and a     |
|| 4x2 screen grid. Event times are relative to eval t so   |
|| ages are deterministic per case.                        |
\*---------------------------------------------------------*/
static InputState ReactiveInput(double t)
{
    InputState in;
    in.audio_level = 0.7f;

    InputEvent a1;
    a1.t = t - 0.40; a1.source = "audio"; a1.strength = 1.0f;
    a1.has_pos = false;
    in.events.push_back(a1);

    InputEvent a2;
    a2.t = t - 1.10; a2.source = "audio"; a2.strength = 0.6f;
    a2.has_pos = true; a2.pos = { 0.47f, 0.22f, -0.10f };
    in.events.push_back(a2);

    InputEvent k1;
    k1.t = t - 0.15; k1.source = "key"; k1.strength = 0.9f;
    k1.has_pos = true; k1.pos = { -0.10f, 0.045f, 0.24f }; k1.code = 65;
    in.events.push_back(k1);

    InputEvent k2;
    k2.t = t - 0.75; k2.source = "key"; k2.strength = 0.7f;
    k2.has_pos = false; k2.code = 32;
    in.events.push_back(k2);

    in.screen_cols = 4;
    in.screen_rows = 2;
    static const float cells[8][3] = {
        { 0.90f, 0.10f, 0.20f }, { 0.55f, 0.20f, 0.75f },
        { 0.20f, 0.60f, 0.90f }, { 0.10f, 0.80f, 0.40f },
        { 0.95f, 0.70f, 0.15f }, { 0.30f, 0.30f, 0.35f },
        { 0.05f, 0.15f, 0.60f }, { 0.80f, 0.40f, 0.10f },
    };
    for(const auto& c : cells)
    {
        in.screen_cells.push_back({ c[0], c[1], c[2], 1.0f });
    }
    return in;
}

static json InputJson(const InputState& in)
{
    json j;
    j["audio_level"] = in.audio_level;
    json ev = json::array();
    for(const InputEvent& e : in.events)
    {
        ev.push_back({{"t",        e.t},
                      {"source",   e.source},
                      {"has_pos",  e.has_pos},
                      {"pos",      Vec3Json(e.pos)},
                      {"strength", e.strength},
                      {"code",     e.code}});
    }
    j["events"]      = ev;
    j["screen_cols"] = in.screen_cols;
    j["screen_rows"] = in.screen_rows;
    json cells = json::array();
    for(const ColorF& c : in.screen_cells)
    {
        cells.push_back(json::array({ c.r, c.g, c.b, c.a }));
    }
    j["screen_cells"] = cells;
    return j;
}

/*---------------------------------------------------------*\
|| Capture                                                 |
\*---------------------------------------------------------*/
static bool EmitPreset(const std::string& id, bool reactive,
                       const std::filesystem::path& dir)
{
    static const unsigned int seeds[] = { 0u, 1u, 42u, 777u };
    static const double       times[] = { 0.0, 0.5, 2.35 };

    const SceneDocument doc = BuildDefaultDesk();
    EffectEngine        engine;

    json out;
    out["format"]  = "dls-effect-fixture";
    out["version"] = 1;
    out["preset"]  = id;
    out["scene"]   = "default-desk";

    /* Resolved layer stacks — one per seed; cases reference them. */
    std::vector<std::vector<EffectLayer>> built;
    json stacks = json::object();
    for(unsigned int seed : seeds)
    {
        built.push_back(BuildPreset(id, seed));
        if(built.back().empty())
        {
            std::fprintf(stderr, "BuildPreset(%s) returned no layers\n",
                         id.c_str());
            return false;
        }
        json arr = json::array();
        for(const EffectLayer& L : built.back())
        {
            arr.push_back(LayerJson(L));
        }
        stacks[std::to_string(seed)] = arr;
    }
    out["stacks"] = stacks;

    json cases = json::array();
    for(size_t si = 0; si < 4; si++)
    {
        const unsigned int seed = seeds[si];
        engine.SetLayers(built[si]);
        /* input variant 0 = no input; variant 1 = synthetic reactive */
        const int variants = reactive ? 2 : 1;
        for(int v = 0; v < variants; v++)
        {
            for(double t : times)
            {
                const InputState in = ReactiveInput(t);
                FrameColors frame;
                engine.Evaluate(doc, t, frame, v == 0 ? nullptr : &in);

                json c;
                c["seed"]       = seed;
                c["t"]          = t;
                c["input"]      = v == 0 ? json() : InputJson(in);
                c["frame_hash"] = HashFrame(frame);
                c["frames"]     = FramesJson(frame);
                cases.push_back(c);
            }
        }
    }
    out["cases"] = cases;

    const std::filesystem::path path = dir / (id + ".fixture.json");
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if(!f.is_open())
    {
        std::fprintf(stderr, "cannot write %s\n", path.string().c_str());
        return false;
    }
    f << out.dump(2) << "\n";
    std::printf("wrote %s (%d cases)\n", path.filename().string().c_str(),
                (int)cases.size());
    return true;
}

int main(int argc, char** argv)
{
    std::filesystem::path dir = argc > 1 ? argv[1] : "fixtures/effects";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    static const char* const order[] = {
        "aurora", "reactor", "comet", "chrome", "portal",
        "embers", "shockwave", "keyripple", "ambient", "wave",
    };
    std::vector<std::string> ids;
    for(int i = 2; i < argc; i++)
    {
        ids.push_back(argv[i]);
    }
    if(ids.empty())
    {
        ids.assign(order, order + sizeof(order) / sizeof(order[0]));
    }
    for(const std::string& s : ids)
    {
        const bool reactive = s == "shockwave" || s == "keyripple"
                              || s == "ambient";
        if(!EmitPreset(s, reactive, dir))
        {
            return 1;
        }
    }
    std::printf("fixtures complete\n");
    return 0;
}
