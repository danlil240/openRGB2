/*---------------------------------------------------------*\
|||| Presets.cpp                                               |
||||                                                           |
||||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "Presets.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>

namespace studio
{

/*---------------------------------------------------------*\
|||| RemixRng — splitmix32                                   |
\*---------------------------------------------------------*/
unsigned int RemixRng::Next()
{
    state += 0x9E3779B9u;
    unsigned int z = state;
    z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
    z = (z ^ (z >> 13)) * 0xC2B2AE35u;
    return z ^ (z >> 16);
}

float RemixRng::Next01() { return (float)(Next() >> 8) / (float)0x1000000; }
float RemixRng::Range(float lo, float hi) { return lo + (hi - lo) * Next01(); }
float RemixRng::Angle() { return Range(0.0f, 6.2831853f); }

Vec3 RotateYaw(const Vec3& dir, float radians)
{
    const float c = std::cos(radians), s = std::sin(radians);
    return { dir.x * c + dir.z * s, dir.y, -dir.x * s + dir.z * c };
}

/*---------------------------------------------------------*\
|||| The effect-look registry                               ||
||||                                                           |
||||   presets/effects/<id>.effect.json is the source of    ||
||||   truth for the nine shipped looks — no C++ builder    ||
||||   remains. The Qt bridge fills the defaults layer from ||
||||   bundled qrc resources at startup; the Qt-free side   ||
||||   (tests, tools) lazily probes the source-tree         ||
||||   packaged dir on first access so the same JSON        ||
||||   answers BuildPreset there. Only when NOTHING is      ||
||||   readable does one minimal built-in look stand in.    |
\*---------------------------------------------------------*/
namespace
{

/* Fixed card order for the scene strip — kept here (not in the
   JSON files) so file overrides and personal looks slot around
   it predictably. */
const char* const kKnownOrder[] = {
    "aurora", "reactor", "comet", "chrome", "portal",
    "embers", "shockwave", "keyripple", "ambient",
};
constexpr unsigned int kKnownCount =
    sizeof(kKnownOrder) / sizeof(kKnownOrder[0]);

/* The ONE built-in fallback: a soft static wash. Deliberately not
   a table of the nine looks — when the packaged files are all
   unreadable the strip shows this single card instead of a
   silently diverging in-code copy. */
EffectDocument FallbackEffectDoc()
{
    EffectDocument d;
    d.id          = "fallback";
    d.name        = "Fallback wash";
    d.description = "Packaged effect files unreadable — static wash";
    nlohmann::ordered_json l;
    l["primitive"] = "static";
    l["opacity"]   = 1.0;
    nlohmann::ordered_json stop;
    stop["pos"]   = 0.0;
    stop["color"] = "#2A3A55";
    l["palette"] = nlohmann::ordered_json::array({ stop });
    d.layers.push_back(l);
    return d;
}

/* Probe the packaged effects dir for the Qt-free side. The Qt
   path never relies on this — the bridge feeds qrc contents via
   SetDefaults. $DLS_EFFECT_DIR wins so tests can point at a
   fixture dir; otherwise a few relative candidates cover the
   usual working dirs (tests/, repo root, the plugin dir). */
void ProbePackagedEffects(EffectRegistry& reg)
{
    if(const char* env = std::getenv("DLS_EFFECT_DIR"))
    {
        if(*env != '\0')
        {
            reg.LoadDefaultsDirectory(env, nullptr);
        }
    }
    static const char* const candidates[] = {
        "presets/effects",
        "../plugins/DesktopLightingStudio/presets/effects",
        "plugins/DesktopLightingStudio/presets/effects",
    };
    for(const char* c : candidates)
    {
        std::error_code ec;
        if(std::filesystem::is_directory(c, ec))
        {
            reg.LoadDefaultsDirectory(c, nullptr);
        }
    }
    if(reg.Ids().empty())
    {
        reg.SetDefaults({ FallbackEffectDoc() });
    }
}

bool g_probed = false;

} /* anonymous namespace */

EffectRegistry& EffectLooks()
{
    static EffectRegistry reg;
    if(!g_probed)
    {
        g_probed = true;
        ProbePackagedEffects(reg);
    }
    return reg;
}

void ResetEffectLooks()
{
    EffectLooks().SetDefaults({});
    EffectLooks().ClearFiles();
    g_probed = false;
}

/*---------------------------------------------------------*\
|||| Listing / resolution                                    |
\*---------------------------------------------------------*/
const std::vector<PresetInfo>& PresetList()
{
    /* Rebuilt per call — registry contents change on file
       reload, so a stale static table would lie. Callers
       consume the list immediately (same contract as the old
       fixed table). */
    static std::vector<PresetInfo> cache;
    cache.clear();
    const std::vector<std::string> order(kKnownOrder,
                                         kKnownOrder + kKnownCount);
    for(const EffectRegistry::EffectInfo& i : EffectLooks().List(order))
    {
        cache.push_back({ i.id, i.name, i.description, i.needs,
                          i.from_file });
    }
    return cache;
}

const PresetInfo* FindPreset(const std::string& id)
{
    for(const PresetInfo& p : PresetList())
    {
        if(p.id == id)
        {
            return &p;
        }
    }
    return nullptr;
}

std::vector<EffectLayer> BuildPreset(const std::string& id, unsigned int seed)
{
    std::vector<EffectLayer> layers;
    if(EffectLooks().Build(id, seed, layers, nullptr))
    {
        return layers;
    }
    /* Unknown ids resolve empty — EXCEPT in the fully degraded
       world (the fallback doc is the only thing registered),
       where the one built-in look answers any id so the desk
       still lights. */
    if(EffectLooks().Ids().size() == 1
       && EffectLooks().Find("fallback") != nullptr
       && EffectLooks().Build("fallback", seed, layers, nullptr))
    {
        return layers;
    }
    return {};
}

void ApplyGlobalParams(std::vector<EffectLayer>& layers, float speed, float intensity)
{
    for(EffectLayer& L : layers)
    {
        L.speed   *= speed;
        L.opacity *= intensity;
        if(L.opacity > 1.0f) L.opacity = 1.0f;
        if(L.opacity < 0.0f) L.opacity = 0.0f;
    }
}

} /* namespace studio */
