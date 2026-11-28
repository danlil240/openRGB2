/*---------------------------------------------------------*\
||| ProfileStore.cpp                                          ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "ProfileStore.h"

#include "scene/SceneJson.h"  /* studio::ParseSceneColor */

#include <windows.h>          /* MoveFileExA              */
#include <io.h>               /* _commit                  */

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace openrgb2
{

namespace
{

constexpr const char* kKind = "openrgb2-engine-state";

/* Content caps — a malformed state file must not be able to
   allocate unboundedly (same discipline as the workspace
   document caps). Generous vs. real usage. */
constexpr size_t kMaxSceneEntries   = 256;
constexpr size_t kMaxBindingEntries = 1024;
constexpr size_t kMaxReactiveEntries = 16;   /* 3 real ids —
   the bound only exists so a corrupt file can't allocate  */

void AddErr(std::vector<std::string>& errs, const std::string& path,
            const std::string& msg)
{
    errs.push_back(path + ": " + msg);
}

bool FieldStr(const nlohmann::json& j, const char* key,
              std::string& out)
{
    const auto it = j.find(key);
    if(it == j.end())
    {
        return false;
    }
    if(!it->is_string())
    {
        return false;
    }
    out = it->get<std::string>();
    return true;
}

/* Lenient string read for legacy documents — wrong-typed
   fields read empty instead of erroring. */
std::string LooseStr(const nlohmann::json& j, const char* key)
{
    std::string out;
    FieldStr(j, key, out);
    return out;
}

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string ColorHex(RgbColor c)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X",
                  c.r, c.g, c.b);
    return buf;
}

bool ParseColor(const nlohmann::json& v, RgbColor& out)
{
    studio::SceneColor c = 0;
    if(!studio::ParseSceneColor(v, c))
    {
        return false;
    }
    out = FromSceneColor(c);
    return true;
}

nlohmann::json BindingToJson(const studio::DeviceBinding& b)
{
    return nlohmann::json::object({
        { "controller_name", b.controller_name },
        { "vendor",          b.vendor          },
        { "serial",          b.serial          },
        { "location",        b.location        },
        { "device_type",     b.device_type     },
        { "zone_name",       b.zone_name       },
        { "zone_leds",       b.zone_leds       },
    });
}

/* Strict binding read (current schema): wrong types error. */
bool BindingFromJson(const nlohmann::json& jb, const std::string& path,
                     studio::DeviceBinding& out,
                     std::vector<std::string>& errs)
{
    if(!jb.is_object())
    {
        AddErr(errs, path, "expected object");
        return false;
    }
    bool ok = true;
    const auto str = [&](const char* key, std::string& dst)
    {
        const auto it = jb.find(key);
        if(it != jb.end())
        {
            if(it->is_string())
            {
                dst = it->get<std::string>();
            }
            else
            {
                AddErr(errs, path + "." + key, "expected string");
                ok = false;
            }
        }
    };
    str("controller_name", out.controller_name);
    str("vendor",          out.vendor);
    str("serial",          out.serial);
    str("location",        out.location);
    str("zone_name",       out.zone_name);
    const auto dt = jb.find("device_type");
    if(dt != jb.end())
    {
        if(dt->is_number_integer())
        {
            out.device_type = dt->get<int>();
        }
        else
        {
            AddErr(errs, path + ".device_type", "expected integer");
            ok = false;
        }
    }
    const auto zl = jb.find("zone_leds");
    if(zl != jb.end())
    {
        if(zl->is_number_integer() && zl->get<long long>() >= 0)
        {
            out.zone_leds = (unsigned int)zl->get<long long>();
        }
        else
        {
            AddErr(errs, path + ".zone_leds", "expected integer >= 0");
            ok = false;
        }
    }
    return ok;
}

/* Lenient binding read (legacy schema): fields best-effort. */
studio::DeviceBinding BindingLoose(const nlohmann::json& jb,
                                 const std::string& id)
{
    studio::DeviceBinding b;
    b.id = id;
    if(!jb.is_object())
    {
        return b;
    }
    b.controller_name = LooseStr(jb, "controller_name");
    b.vendor          = LooseStr(jb, "vendor");
    b.serial          = LooseStr(jb, "serial");
    b.location        = LooseStr(jb, "location");
    b.zone_name       = LooseStr(jb, "zone_name");
    const auto dt = jb.find("device_type");
    if(dt != jb.end() && dt->is_number_integer())
    {
        b.device_type = dt->get<int>();
    }
    const auto zl = jb.find("zone_leds");
    if(zl != jb.end() && zl->is_number_integer() &&
       zl->get<long long>() > 0)
    {
        b.zone_leds = (unsigned int)zl->get<long long>();
    }
    return b;
}

/*---------------------------------------------------------*\
||| Atomic file primitives                                  ||
||\*--------------------------------------------------------*/

bool WriteAtomic(const std::string& tmp, const std::string& dst,
                 const std::string& bytes, std::string& err)
{
    FILE* f = nullptr;
    if(fopen_s(&f, tmp.c_str(), "wb") != 0 || f == nullptr)
    {
        err = "cannot open " + tmp + " for writing";
        return false;
    }
    bool ok = true;
    if(!bytes.empty() &&
       fwrite(bytes.data(), 1, bytes.size(), f) != bytes.size())
    {
        err = "short write to " + tmp;
        ok  = false;
    }
    if(ok && fflush(f) != 0)
    {
        err = "flush failed for " + tmp;
        ok  = false;
    }
    /* Push the bytes to stable storage BEFORE the rename —
       a crash after rename must not expose a zero-filled
       directory entry. */
    if(ok && _commit(_fileno(f)) != 0)
    {
        err = "commit failed for " + tmp;
        ok  = false;
    }
    fclose(f);
    if(!ok)
    {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return false;
    }
    if(!MoveFileExA(tmp.c_str(), dst.c_str(),
                    MOVEFILE_REPLACE_EXISTING |
                    MOVEFILE_WRITE_THROUGH))
    {
        err = "rename of " + tmp + " over " + dst +
              " failed (win32 " + std::to_string(GetLastError()) + ")";
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

bool ReadFile(const std::string& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if(!in)
    {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

/* Parse + validate (+migrate) one document body. Non-fatal
   notes (e.g. an unrecognised legacy mode mapped to
   live_effects) are collected in `warns` — never dropped. */
bool ParseState(const std::string& bytes, EngineState& out,
                std::vector<std::string>& errs,
                std::vector<std::string>& warns, bool& migrated)
{
    const nlohmann::json j =
        nlohmann::json::parse(bytes, nullptr, false);
    if(j.is_discarded())
    {
        errs.push_back("root: not valid JSON");
        return false;
    }
    return EngineStateFromJson(j, out, &errs, &warns, &migrated);
}

} /* anonymous namespace */

/*---------------------------------------------------------*\
||| Serialization                                           ||
\*---------------------------------------------------------*/

nlohmann::json EngineStateToJson(const EngineState& state)
{
    nlohmann::json j = nlohmann::json::object();
    /* Retained unknown members first — the known fields below
       always win on collision. */
    if(state.extensions.is_object())
    {
        for(auto it = state.extensions.begin();
            it != state.extensions.end(); ++it)
        {
            j[it.key()] = it.value();
        }
    }
    j["kind"]           = kKind;
    j["schema_version"] = kEngineStateSchemaVersion;

    nlohmann::json scenes = nlohmann::json::array();
    for(const PersistedSceneMode& m : state.scenes)
    {
        scenes.push_back(nlohmann::json::object({
            { "scene",   m.scene_key                   },
            { "mode",    PlaybackModeName(m.mode)      },
            { "speed",   m.params.speed                },
            { "reverse", m.params.reverse              },
            { "color",   ColorHex(m.params.color)      },
        }));
    }
    j["scenes"] = std::move(scenes);

    nlohmann::json bindings = nlohmann::json::object();
    for(const auto& kv : state.bindings)
    {
        bindings[kv.first] = BindingToJson(kv.second);
    }
    j["bindings"] = std::move(bindings);

    /* Task 4.6 reactive inputs — the enabled selection +
       per-input params as recorded by the manager. */
    nlohmann::json reactive = nlohmann::json::array();
    for(const ReactiveInputs::PersistedInput& p : state.reactive)
    {
        reactive.push_back(nlohmann::json::object({
            { "input",        p.id           },
            { "enabled",      p.enabled      },
            { "sensitivity",  p.sensitivity  },
            { "screen_index", p.screen_index },
        }));
    }
    j["reactive"] = std::move(reactive);

    /* The last managed workspace id — the engine re-adopts it
       at start (adoption only, never a scene re-apply). */
    j["active_workspace"] = state.active_workspace;

    /* R-1: the runtime output control's persisted half — only
       written once something set it (an older file stays
       byte-for-byte free of the member). Unknown members a
       newer build put in `output` are written back first; the
       known fields win on collision. */
    if(state.output)
    {
        nlohmann::json out = nlohmann::json::object();
        if(state.output_extensions.is_object())
        {
            for(auto it = state.output_extensions.begin();
                it != state.output_extensions.end(); ++it)
            {
                out[it.key()] = it.value();
            }
        }
        out["master"] =
            std::round(double(state.output->master) * 10000.0) /
            10000.0;
        out["blackout"] = state.output->blackout;
        j["output"] = std::move(out);
    }

    /* E-12b exit/idle output policy (OutputPolicy.h shape). */
    j["output_policy"] = OutputPolicyToJson(state.output_policy);
    return j;
}

bool EngineStateFromJson(const nlohmann::json& j,
                         EngineState& out,
                         std::vector<std::string>* errors,
                         std::vector<std::string>* warnings,
                         bool*                    migrated)
{
    std::vector<std::string> local_errs, local_warns;
    std::vector<std::string>& errs = errors   ? *errors   : local_errs;
    std::vector<std::string>& warn = warnings ? *warnings : local_warns;

    if(!j.is_object())
    {
        AddErr(errs, "root", "expected object");
        return false;
    }

    /* Version gate — older documents migrate forward, newer
       ones are rejected (never a silent downgrade). */
    long long version = 0;
    const auto        vit = j.find("schema_version");
    if(vit != j.end())
    {
        if(!vit->is_number_integer())
        {
            AddErr(errs, "schema_version", "expected integer");
            return false;
        }
        version = vit->get<long long>();
    }
    if(version < 0)
    {
        AddErr(errs, "schema_version", "expected >= 0");
        return false;
    }
    if(version > kEngineStateSchemaVersion)
    {
        AddErr(errs, "schema_version",
               std::to_string(version) + " is newer than supported " +
               std::to_string(kEngineStateSchemaVersion));
        return false;
    }
    const bool legacy = version < kEngineStateSchemaVersion;
    if(migrated)
    {
        *migrated = legacy;
    }
    if(const auto kit = j.find("kind");
       kit != j.end() && kit->is_string() &&
       kit->get<std::string>() != kKind)
    {
        warn.push_back("kind: '" + kit->get<std::string>() +
                       "' is not '" + kKind + "' (loading anyway)");
    }

    EngineState candidate;

    /*---- scenes --------------------------------------------*\
    || v1: array of {scene, mode, speed, reverse, color}.      ||
    || v0: the same, or an object map name->selection, with    ||
    || looser mode spellings; an absent/unrecognised mode      ||
    || migrates to LiveEffects — old profiles are preserved    ||
    || as Live effects, NEVER silently uploaded playback.      ||
    \*---------------------------------------------------------*/
    const auto sit = j.find("scenes");
    if(sit != j.end() && !sit->is_null())
    {
        bool                 ok   = true;
        std::vector<PersistedSceneMode> parsed;
        const auto read_entry = [&](const nlohmann::json& je,
                                    const std::string& map_key,
                                    const std::string& path)
        {
            PersistedSceneMode m;
            m.scene_key = map_key;
            if(je.is_string())
            {
                /* v0 shorthand: "<name>": "animation" */
                if(legacy)
                {
                    const std::string t = Lower(je.get<std::string>());
                    if(t == "animation" || t == "animation_playback" ||
                       t == "uploaded" || t == "playback")
                    {
                        m.mode = PlaybackMode::AnimationPlayback;
                    }
                    else
                    {
                        m.mode = PlaybackMode::LiveEffects;
                    }
                    parsed.push_back(m);
                }
                else
                {
                    AddErr(errs, path, "expected object");
                    ok = false;
                }
                return;
            }
            if(!je.is_object())
            {
                AddErr(errs, path, "expected object");
                ok = false;
                return;
            }
            std::string key;
            if(FieldStr(je, "scene", key) ||
               FieldStr(je, "scene_key", key) ||
               FieldStr(je, "name", key))
            {
                m.scene_key = key;
            }
            std::string mode_name;
            if(!FieldStr(je, "mode", mode_name) &&
               !FieldStr(je, "playback_mode", mode_name) &&
               !FieldStr(je, "playback", mode_name))
            {
                mode_name.clear();
            }
            if(mode_name.empty())
            {
                /* Old profiles carried no playback mode — the
                   migrated selection is Live effects. */
                m.mode = PlaybackMode::LiveEffects;
                if(!legacy)
                {
                    AddErr(errs, path + ".mode", "missing or empty");
                    ok = false;
                    return;
                }
            }
            else if(!PlaybackModeFromName(mode_name, m.mode))
            {
                if(legacy)
                {
                    const std::string t = Lower(mode_name);
                    if(t == "animation" || t == "uploaded" ||
                       t == "playback")
                    {
                        m.mode = PlaybackMode::AnimationPlayback;
                    }
                    else
                    {
                        warn.push_back(path + ".mode: '" + mode_name +
                                       "' unrecognised — migrated to "
                                       "live_effects");
                        m.mode = PlaybackMode::LiveEffects;
                    }
                }
                else
                {
                    AddErr(errs, path + ".mode",
                           "unknown playback mode '" + mode_name + "'");
                    ok = false;
                    return;
                }
            }
            if(m.scene_key.empty())
            {
                if(legacy)
                {
                    return;   /* a scene-less entry is skipped */
                }
                AddErr(errs, path + ".scene", "missing or empty");
                ok = false;
                return;
            }
            const auto sp = je.find("speed");
            if(sp != je.end())
            {
                if(sp->is_number_integer())
                {
                    m.params.speed =
                        ClampPlaybackSpeed(sp->get<int32_t>());
                }
                else if(!legacy)
                {
                    AddErr(errs, path + ".speed", "expected integer");
                    ok = false;
                }
            }
            const auto rv = je.find("reverse");
            if(rv != je.end())
            {
                if(rv->is_boolean())
                {
                    m.params.reverse = rv->get<bool>();
                }
                else if(!legacy)
                {
                    AddErr(errs, path + ".reverse", "expected boolean");
                    ok = false;
                }
            }
            const auto co = je.find("color");
            if(co != je.end())
            {
                if(!ParseColor(*co, m.params.color) && !legacy)
                {
                    AddErr(errs, path + ".color",
                           "expected \"#RRGGBB\" or packed integer");
                    ok = false;
                }
            }
            parsed.push_back(m);
        };
        if(sit->is_array())
        {
            int i = 0;
            for(const nlohmann::json& je : *sit)
            {
                read_entry(je, std::string(),
                           "scenes[" + std::to_string(i++) + "]");
            }
        }
        else if(legacy && sit->is_object())
        {
            for(auto e = sit->begin(); e != sit->end(); ++e)
            {
                read_entry(e.value(), e.key(), "scenes." + e.key());
            }
        }
        else
        {
            AddErr(errs, "scenes", "expected array");
            ok = false;
        }
        if(parsed.size() > kMaxSceneEntries)
        {
            AddErr(errs, "scenes",
                   "exceeds the " + std::to_string(kMaxSceneEntries) +
                   "-entry bound");
            ok = false;
        }
        if(ok)
        {
            candidate.scenes = std::move(parsed);
        }
        else
        {
            return false;
        }
    }

    /*---- bindings ------------------------------------------*\
    || Object map binding-id -> identity fields.               ||
    \*---------------------------------------------------------*/
    const auto bit = j.find("bindings");
    if(bit != j.end() && !bit->is_null())
    {
        if(!bit->is_object())
        {
            AddErr(errs, "bindings", "expected object");
            return false;
        }
        if(bit->size() > kMaxBindingEntries)
        {
            AddErr(errs, "bindings",
                   "exceeds the " + std::to_string(kMaxBindingEntries) +
                   "-entry bound");
            return false;
        }
        for(auto b = bit->begin(); b != bit->end(); ++b)
        {
            if(b.key().empty())
            {
                AddErr(errs, "bindings", "empty binding id");
                return false;
            }
            if(legacy)
            {
                candidate.bindings[b.key()] =
                    BindingLoose(b.value(), b.key());
            }
            else
            {
                studio::DeviceBinding d;
                d.id = b.key();
                if(!BindingFromJson(b.value(),
                                    "bindings." + b.key(), d, errs))
                {
                    return false;
                }
                candidate.bindings[b.key()] = d;
            }
        }
    }

    /*---- reactive inputs (Task 4.6) ----------------------*\
    || v1: array of {input, enabled, sensitivity?,           ||
    || screen_index?}. Unknown input ids are warnings — a    ||
    || newer document may know ids this build doesn't; skip  ||
    || them, never fail the whole profile over one entry.    ||
    \*---------------------------------------------------------*/
    const auto rit = j.find("reactive");
    if(rit != j.end() && !rit->is_null())
    {
        if(!rit->is_array())
        {
            if(legacy)
            {
                warn.push_back("reactive: not an array — ignored");
            }
            else
            {
                AddErr(errs, "reactive", "expected array");
                return false;
            }
        }
        else if(rit->size() > kMaxReactiveEntries)
        {
            AddErr(errs, "reactive",
                   "exceeds the " +
                   std::to_string(kMaxReactiveEntries) +
                   "-entry bound");
            return false;
        }
        else
        {
            bool ok = true;
            int  i  = 0;
            for(const nlohmann::json& je : *rit)
            {
                const std::string path =
                    "reactive[" + std::to_string(i++) + "]";
                if(!je.is_object())
                {
                    if(legacy)
                    {
                        continue;
                    }
                    AddErr(errs, path, "expected object");
                    ok = false;
                    continue;
                }
                std::string id;
                if(!FieldStr(je, "input", id) || id.empty())
                {
                    if(legacy)
                    {
                        continue;
                    }
                    AddErr(errs, path + ".input",
                           "missing or empty");
                    ok = false;
                    continue;
                }
                bool known = false;
                for(const std::string& k :
                        ReactiveInputs::InputIds())
                {
                    if(k == id)
                    {
                        known = true;
                        break;
                    }
                }
                if(!known)
                {
                    warn.push_back(path + ".input: '" + id +
                                   "' is not a reactive input "
                                   "this build knows — skipped");
                    continue;
                }
                ReactiveInputs::PersistedInput p;
                p.id = id;
                const auto en = je.find("enabled");
                if(en != je.end())
                {
                    if(en->is_boolean())
                    {
                        p.enabled = en->get<bool>();
                    }
                    else if(!legacy)
                    {
                        AddErr(errs, path + ".enabled",
                               "expected boolean");
                        ok = false;
                    }
                }
                const auto se = je.find("sensitivity");
                if(se != je.end())
                {
                    if(se->is_number())
                    {
                        p.sensitivity = (float)se->get<double>();
                    }
                    else if(!legacy)
                    {
                        AddErr(errs, path + ".sensitivity",
                               "expected number");
                        ok = false;
                    }
                }
                const auto si = je.find("screen_index");
                if(si != je.end())
                {
                    if(si->is_number_integer() &&
                       si->get<long long>() >= 0)
                    {
                        p.screen_index = (int)si->get<long long>();
                    }
                    else if(!legacy)
                    {
                        AddErr(errs, path + ".screen_index",
                               "expected integer >= 0");
                        ok = false;
                    }
                }
                candidate.reactive.push_back(p);
            }
            if(!ok)
            {
                return false;
            }
        }
    }

    /*---- active workspace ----------------------------------*\
    || The id the engine re-adopts at start. A real field —   ||
    || never an extension passthrough — so a write validates. ||
    \*---------------------------------------------------------*/
    const auto wit = j.find("active_workspace");
    if(wit != j.end() && !wit->is_null())
    {
        if(wit->is_string())
        {
            candidate.active_workspace =
                wit->get<std::string>();
        }
        else if(legacy)
        {
            /* Legacy documents read leniently — a wrong-typed
               member just means "no workspace to re-open". */
            warn.push_back("active_workspace: not a string — "
                           "ignored");
        }
        else
        {
            AddErr(errs, "active_workspace", "expected string");
            return false;
        }
    }

    /*---- runtime output control (R-1) ----------------------*\
    || {master: 0..1, blackout: bool}. Read leniently in every ||
    || schema: a malformed member is dropped with a warning —  ||
    || failing the document would cost the bindings to LKG     ||
    || recovery over a tray brightness. Out-of-range masters   ||
    || clamp; unknown members ride output_extensions.          ||
    \*---------------------------------------------------------*/
    const auto oit = j.find("output");
    if(oit != j.end() && !oit->is_null())
    {
        const nlohmann::json& o = *oit;
        const auto mit = o.is_object() ? o.find("master") : o.end();
        const auto bit = o.is_object() ? o.find("blackout") : o.end();
        const bool master_ok =
            o.is_object() &&
            (mit == o.end() || (mit->is_number() &&
                                std::isfinite(mit->get<double>())));
        const bool blackout_ok =
            o.is_object() && (bit == o.end() || bit->is_boolean());
        if(master_ok && blackout_ok)
        {
            PersistedOutput po;
            if(mit != o.end())
            {
                const double m = mit->get<double>();
                po.master = float(m < 0.0 ? 0.0 : (m > 1.0 ? 1.0 : m));
            }
            if(bit != o.end())
            {
                po.blackout = bit->get<bool>();
            }
            candidate.output = po;
            for(auto it = o.begin(); it != o.end(); ++it)
            {
                if(it.key() != "master" && it.key() != "blackout")
                {
                    candidate.output_extensions[it.key()] = it.value();
                }
            }
        }
        else
        {
            warn.push_back("output: expected {master:number, "
                           "blackout:bool} — ignored");
        }
    }

    /*---- E-12b exit/idle output policy --------------------*\
    || A real field, validated by the same OutputPolicyFrom-  ||
    || Json the set_policy verb uses. Absent -> hold/hold.    ||
    \*---------------------------------------------------------*/
    const auto pit = j.find("output_policy");
    if(pit != j.end() && !pit->is_null())
    {
        std::string perr;
        if(!OutputPolicyFromJson(*pit, candidate.output_policy, perr))
        {
            if(legacy)
            {
                warn.push_back("output_policy: " + perr +
                               " — ignored (hold)");
            }
            else
            {
                AddErr(errs, "output_policy", perr);
                return false;
            }
        }
    }

    /* Unknown top-level members are retained verbatim — a
       migrated document never silently drops fields that
       still apply. */
    for(auto it = j.begin(); it != j.end(); ++it)
    {
        if(it.key() != "kind" && it.key() != "schema_version" &&
           it.key() != "scenes" && it.key() != "bindings" &&
           it.key() != "reactive" &&
           it.key() != "active_workspace" &&
           it.key() != "output" &&
           it.key() != "output_policy")
        {
            candidate.extensions[it.key()] = it.value();
        }
    }

    candidate.schema_version = kEngineStateSchemaVersion;
    out = std::move(candidate);
    return true;
}

/*---------------------------------------------------------*\
||| Persistence-boundary validation                         ||
\*---------------------------------------------------------*/

bool ValidateSceneLedAddresses(const studio::SceneDocument& doc,
                               std::vector<std::string>* errors)
{
    std::vector<std::string> local;
    std::vector<std::string>& errs = errors ? *errors : local;

    std::map<std::string, const studio::DeviceBinding*> by_id;
    for(const studio::DeviceBinding& b : doc.bindings)
    {
        by_id[b.id] = &b;
    }

    for(const studio::SceneObject& o : doc.objects)
    {
        const std::string path = "objects." + o.id;
        int i = 0;
        for(const studio::Emitter& e : o.emitters)
        {
            const std::string ep =
                path + ".emitters[" + std::to_string(i++) + "]";
            if(e.address < -1)
            {
                /* -1 is the render-only sentinel; anything lower
                   is a wrapped/invalid index. */
                AddErr(errs, ep + ".address",
                       std::to_string(e.address) +
                       " is invalid (must be >= -1)");
            }
        }
        if(o.binding.empty() || o.kind != studio::ObjectKind::Device)
        {
            continue;
        }
        const auto it = by_id.find(o.binding);
        if(it == by_id.end() || it->second->zone_leds == 0)
        {
            continue;   /* 0 = unchecked, same as the resolver */
        }
        const studio::DeviceBinding& b = *it->second;
        i = 0;
        for(const studio::Emitter& e : o.emitters)
        {
            const std::string ep =
                path + ".emitters[" + std::to_string(i++) + "]";
            if(e.address >= (int)b.zone_leds)
            {
                AddErr(errs, ep + ".address",
                       std::to_string(e.address) +
                       " out of range (zone '" + b.zone_name +
                       "' has " + std::to_string(b.zone_leds) +
                       " LEDs)");
            }
        }
    }
    return errs.empty();
}

/*---------------------------------------------------------*\
||| Offline bindings                                        ||
\*---------------------------------------------------------*/

bool AugmentSceneBindings(studio::SceneDocument& doc,
    const std::map<std::string, studio::DeviceBinding>& resolved)
{
    bool changed = false;
    for(studio::DeviceBinding& b : doc.bindings)
    {
        const auto it = resolved.find(b.id);
        if(it == resolved.end())
        {
            continue;
        }
        const studio::DeviceBinding& rec = it->second;
        /* Same logical attachment only: the recorded identity
           must still describe the binding's controller —
           a re-purposed id is never rewritten. */
        if(!rec.controller_name.empty() &&
           !b.controller_name.empty() &&
           Lower(rec.controller_name) != Lower(b.controller_name))
        {
            continue;
        }
        if(b.controller_name.empty() && !rec.controller_name.empty())
        {
            b.controller_name = rec.controller_name;
            changed = true;
        }
        if(b.vendor.empty() && !rec.vendor.empty())
        {
            b.vendor   = rec.vendor;
            changed = true;
        }
        if(b.serial.empty() && !rec.serial.empty())
        {
            b.serial   = rec.serial;
            changed = true;
        }
        if(b.location.empty() && !rec.location.empty())
        {
            b.location = rec.location;
            changed = true;
        }
        if(b.device_type < 0 && rec.device_type >= 0)
        {
            b.device_type = rec.device_type;
            changed = true;
        }
        if(b.zone_name.empty() && !rec.zone_name.empty())
        {
            b.zone_name = rec.zone_name;
            changed = true;
        }
        if(b.zone_leds == 0 && rec.zone_leds > 0)
        {
            b.zone_leds = rec.zone_leds;
            changed = true;
        }
    }
    return changed;
}

studio::DeviceBinding ResolvedBindingIdentity(
    const studio::DeviceBinding& binding,
    const DeviceSnapshot&        snap,
    const std::string&           resolved_zone_name,
    uint32_t                     resolved_zone_leds)
{
    studio::DeviceBinding out;
    out.id              = binding.id;
    out.controller_name = snap.name;
    out.vendor          = snap.vendor;
    out.serial          = snap.key.serial;
    out.location        = snap.key.location;
    out.device_type     = snap.device_type;
    out.zone_name       = !binding.zone_name.empty()
                              ? binding.zone_name
                              : resolved_zone_name;
    out.zone_leds       = resolved_zone_leds;
    return out;
}

/*---------------------------------------------------------*\
||| ProfileStore                                            ||
\*---------------------------------------------------------*/

ProfileStore::ProfileStore(std::string path)
    : path_(std::move(path))
{
}

ProfileStore::LoadResult ProfileStore::Load() const
{
    LoadResult result;
    std::error_code ec;
    if(!std::filesystem::exists(path_, ec))
    {
        result.status = LoadStatus::Empty;
        return result;
    }

    std::string              bytes;
    EngineState              state;
    std::vector<std::string> errs, warns;
    bool                     migrated = false;
    if(ReadFile(path_, bytes) &&
       ParseState(bytes, state, errs, warns, migrated))
    {
        result.status   = migrated ? LoadStatus::Migrated
                                   : LoadStatus::Loaded;
        result.state    = std::move(state);
        result.warnings = std::move(warns);
        return result;
    }
    if(bytes.empty() && errs.empty())
    {
        errs.push_back("root: cannot read file");
    }
    std::string primary_err = "primary profile failed to load:";
    for(const std::string& e : errs)
    {
        primary_err += " " + e + ";";
    }

    /* Last-known-good recovery — the corrupt primary never
       silently becomes a blank profile. */
    if(std::filesystem::exists(LkgPath(), ec))
    {
        EngineState              backup;
        std::vector<std::string> berrs, bwarns;
        std::string              bbytes;
        bool                     bmig = false;
        if(ReadFile(LkgPath(), bbytes) &&
           ParseState(bbytes, backup, berrs, bwarns, bmig))
        {
            result.status   = LoadStatus::Recovered;
            result.state    = std::move(backup);
            result.warnings = std::move(bwarns);
            result.error    = primary_err +
                              " restored the last-known-good copy";
            return result;
        }
        result.error = primary_err +
                       " last-known-good recovery also failed:";
        for(const std::string& e : berrs)
        {
            result.error += " " + e + ";";
        }
    }
    else
    {
        result.error = primary_err + " no last-known-good copy exists";
    }
    result.status = LoadStatus::Failed;
    return result;
}

bool ProfileStore::StillOurs(const std::string& path, const Written& w)
{
    if(!w.valid)
    {
        return false;
    }
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if(ec || size != w.size)
    {
        return false;
    }
    const auto mtime = std::filesystem::last_write_time(path, ec);
    return !ec && mtime == w.mtime;
}

void ProfileStore::Remember(const std::string& path,
                            const std::string& bytes, Written& w)
{
    std::error_code ec1, ec2;
    w.size  = std::filesystem::file_size(path, ec1);
    w.mtime = std::filesystem::last_write_time(path, ec2);
    w.valid = !ec1 && !ec2 && w.size == bytes.size();
    w.bytes = w.valid ? bytes : std::string();
}

bool ProfileStore::Save(const EngineState& state, std::string& err) const
{
    const nlohmann::json       j  = EngineStateToJson(state);
    const std::string bytes = j.dump(2) + "\n";

    std::lock_guard<std::mutex> lock(io_mutex_);
    const bool ours = StillOurs(path_, primary_written_);
    if(ours && bytes == primary_written_.bytes)
    {
        return true;    /* byte-identical to the file on disk */
    }

    /* Serialize + re-validate the candidate — the store never
       lands a document its own validation rejects. */
    EngineState                check;
    std::vector<std::string>   errs;
    if(!EngineStateFromJson(j, check, &errs))
    {
        err = "candidate failed validation:";
        for(const std::string& e : errs)
        {
            err += " " + e + ";";
        }
        return false;
    }

    const std::filesystem::path p(path_);
    std::error_code ec;
    if(p.has_parent_path() &&
       !std::filesystem::exists(p.parent_path(), ec))
    {
        if(!std::filesystem::create_directories(p.parent_path(), ec))
        {
            err = "cannot create " + p.parent_path().string() +
                  ": " + ec.message();
            return false;
        }
    }

    /* Preserve the outgoing file before the overwrite: a VALID
       one becomes the last-known-good; an INVALID one is set
       aside as .rejected (reported, never destroyed). */
    if(std::filesystem::exists(path_, ec))
    {
        std::string              cur;
        EngineState              cur_state;
        std::vector<std::string> cerrs, cwarns;
        bool                     cmig = false;
        /* A file still exactly as this store wrote it was
           validated before that write — no re-read/re-parse. */
        const bool cur_valid =
            ours ? (cur = primary_written_.bytes, true)
                 : (ReadFile(path_, cur) &&
                    ParseState(cur, cur_state, cerrs, cwarns, cmig));
        if(cur_valid)
        {
            if(!WriteAtomic(LkgPath() + ".tmp", LkgPath(), cur, err))
            {
                err = "last-known-good backup failed: " + err;
                return false;
            }
        }
        else
        {
            if(!WriteAtomic(RejectedPath() + ".tmp", RejectedPath(),
                            cur, err))
            {
                err = "could not set the corrupt file aside: " + err;
                return false;
            }
        }
    }

    primary_written_.valid = false;
    if(!WriteAtomic(TempPath(), path_, bytes, err))
    {
        return false;
    }
    Remember(path_, bytes, primary_written_);
    return true;
}

/*---------------------------------------------------------*\
|||| Last-scene snapshot                                   ||
\*---------------------------------------------------------*/

std::string ProfileStore::SceneSnapshotPath() const
{
    return (std::filesystem::path(path_).parent_path() /
            "last-scene.json").string();
}

std::string ProfileStore::SceneSnapshotRejectedPath() const
{
    return SceneSnapshotPath() + ".rejected";
}

ProfileStore::SceneSnapshot ProfileStore::LoadSceneSnapshot() const
{
    SceneSnapshot r;
    std::error_code ec;
    const std::string p = SceneSnapshotPath();
    if(!std::filesystem::exists(p, ec))
    {
        return r;   /* absent — a fresh profile, not an error */
    }
    if(!ReadFile(p, r.bytes))
    {
        r.error = "cannot read " + p;
        return r;
    }
    const nlohmann::json j =
        nlohmann::json::parse(r.bytes, nullptr, false);
    if(j.is_discarded() || !j.is_object())
    {
        /* Honest failure: the bytes stay on disk (a read never
           destroys user state); the NEXT successful save sets
           the corrupt file aside as .rejected. */
        r.bytes.clear();
        r.error = p + ": not a scene document (invalid JSON)";
        return r;
    }
    r.present = true;
    return r;
}

bool ProfileStore::SaveSceneSnapshot(const std::string& bytes,
                                     std::string& err) const
{
    /* The store never lands a document it can't itself parse —
       same discipline as Save()'s serialize + re-validate. */
    const nlohmann::json j =
        nlohmann::json::parse(bytes, nullptr, false);
    if(j.is_discarded() || !j.is_object())
    {
        err = "scene snapshot candidate is not a JSON object";
        return false;
    }
    std::lock_guard<std::mutex> lock(io_mutex_);
    return WriteSnapshotLocked(bytes, err);
}

bool ProfileStore::SaveSceneSnapshotJson(const nlohmann::json& doc,
                                     std::string& err) const
{
    if(!doc.is_object())
    {
        err = "scene snapshot candidate is not a JSON object";
        return false;
    }
    const std::string bytes = doc.dump(2) + "\n";
    std::lock_guard<std::mutex> lock(io_mutex_);
    return WriteSnapshotLocked(bytes, err);
}

bool ProfileStore::WriteSnapshotLocked(const std::string& bytes,
                                       std::string& err) const
{
    const std::filesystem::path p(SceneSnapshotPath());
    const bool ours = StillOurs(p.string(), snapshot_written_);
    if(ours && bytes == snapshot_written_.bytes)
    {
        return true;    /* byte-identical to the file on disk */
    }
    std::error_code ec;
    if(p.has_parent_path() &&
       !std::filesystem::exists(p.parent_path(), ec))
    {
        if(!std::filesystem::create_directories(p.parent_path(),
                                               ec))
        {
            err = "cannot create " + p.parent_path().string() +
                  ": " + ec.message();
            return false;
        }
    }

    /* A corrupt outgoing file is preserved as .rejected —
       reported, never silently destroyed. Only a file we
       actually READ is set aside; an unreadable one keeps its
       bytes and the write below reports any real error. */
    if(!ours && std::filesystem::exists(p, ec))
    {
        std::string cur;
        if(ReadFile(p.string(), cur))
        {
            const nlohmann::json cj =
                nlohmann::json::parse(cur, nullptr, false);
            if(cj.is_discarded() || !cj.is_object())
            {
                if(!WriteAtomic(
                       SceneSnapshotRejectedPath() + ".tmp",
                       SceneSnapshotRejectedPath(), cur, err))
                {
                    err = "could not set the corrupt scene "
                          "snapshot aside: " + err;
                    return false;
                }
            }
        }
    }

    snapshot_written_.valid = false;
    if(!WriteAtomic(p.string() + ".tmp", p.string(), bytes, err))
    {
        return false;
    }
    Remember(p.string(), bytes, snapshot_written_);
    return true;
}

} /* namespace openrgb2 */
