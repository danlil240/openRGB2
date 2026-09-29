/*---------------------------------------------------------*\
|||| SceneEditModel.cpp                                        ||
||||                                                           ||
||||   SPDX-License-Identifier: GPL-2.0-or-later               ||
||\*---------------------------------------------------------*/

#include "SceneEditModel.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace openrgb2
{
namespace ui
{

namespace
{

/* docs/ipc-protocol.md §5 — the uploaded preset set and the
   params each accepts. */
const std::vector<PlaybackPresetInfo> kPlaybackPresets =
{
    { "spectrum_cycle",     "Spectrum Cycle",     true,  false, false },
    { "rainbow_wave",        "Rainbow Wave",       true,  true,  false },
    { "breathing",          "Breathing",          true,  false, true  },
    { "perimeter_wave",      "Perimeter Wave",     true,  true,  false },
    { "perimeter_comet",    "Perimeter Comet",    true,  true,  true  },
    { "perimeter_chase",    "Perimeter Chase",    true,  true,  false },
    { "perimeter_ping_pong","Perimeter Ping Pong",true,  false, true  },
    { "stack_wave",         "Stack Wave",         true,  false, false },
    { "stack_rainbow",      "Stack Rainbow",      true,  true,  false },
};

constexpr int kSpeedMin = 10;
constexpr int kSpeedMax = 200;

/* effect.speed is a multiplier on the resolved layers — bound
   the UI's domain well inside anything the core accepts. */
constexpr double kEffectSpeedMin = 0.05;
constexpr double kEffectSpeedMax = 8.0;

double Clamp(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Wrong-typed fields in a hand-restored document must not throw
   (documents the engine emitted are clean; library files are
   user-touchable storage). */
std::string JStr(const nlohmann::json& j, const char* key)
{
    const auto it = j.find(key);
    return (it != j.end() && it->is_string())
               ? it->get<std::string>()
               : std::string();
}

bool JBool(const nlohmann::json& j, const char* key)
{
    const auto it = j.find(key);
    return it != j.end() && it->is_boolean() && it->get<bool>();
}

} /* anonymous namespace */

/*---------------------------------------------------------*\
||| Playback preset catalog (wire contract)                ||
\*---------------------------------------------------------*/

const std::vector<PlaybackPresetInfo>& PlaybackPresetCatalog()
{
    return kPlaybackPresets;
}

std::string NormalizePresetId(const std::string& name)
{
    std::string out;
    out.reserve(name.size());
    for(const unsigned char ch : name)
    {
        if(ch == ' ' || ch == '_' || ch == '-')
        {
            continue;
        }
        out.push_back((char)std::tolower(ch));
    }
    return out;
}

bool PlaybackPresetFromName(const std::string& name,
                            PlaybackPresetInfo& out)
{
    const std::string n = NormalizePresetId(name);
    for(const PlaybackPresetInfo& p : kPlaybackPresets)
    {
        if(n == NormalizePresetId(p.id) ||
           n == NormalizePresetId(p.sdk_name))
        {
            out = p;
            return true;
        }
    }
    return false;
}

int ClampPlaybackSpeed(int speed)
{
    return speed < kSpeedMin ? kSpeedMin
         : speed > kSpeedMax ? kSpeedMax : speed;
}

/*---------------------------------------------------------*\
||| Field helpers                                          ||
\*---------------------------------------------------------*/

bool SceneEditModel::IsHexColor(const std::string& s)
{
    if(s.size() != 7 || s[0] != '#')
    {
        return false;
    }
    for(size_t i = 1; i < 7; ++i)
    {
        const char c = s[i];
        if(!std::isxdigit((unsigned char)c))
        {
            return false;
        }
    }
    return true;
}

std::string SceneEditModel::NormalizeHexColor(const std::string& s)
{
    std::string out = s;
    for(size_t i = 1; i < out.size(); ++i)
    {
        out[i] = (char)std::toupper((unsigned char)out[i]);
    }
    return out;
}

nlohmann::json SceneEditModel::NewDocument()
{
    /* Version must be a JSON INTEGER — the engine's loader
       (studio::FromJson) rejects floats. */
    return nlohmann::json::object({
        { "version",    2        },
        { "name",       std::string() },
        { "brightness", 1.0      },
        { "bindings",   nlohmann::json::array() },
        { "objects",    nlohmann::json::array() },
        { "object_colors",  nlohmann::json::object() },
        { "emitter_colors", nlohmann::json::object() },
        { "effect", nlohmann::json::object({
            { "preset",    std::string() },
            { "seed",      0    },
            { "speed",     1.0  },
            { "intensity", 1.0  },
            { "playing",   true },
            { "layers",    nlohmann::json::array() },
        }) },
    });
}

/*---------------------------------------------------------*\
||| Baseline / working state                               ||
\*---------------------------------------------------------*/

void SceneEditModel::LoadApplied(const nlohmann::json& doc_or_null)
{
    /* Dirty MUST be sampled against the OLD baseline —
       checking after the overwrite would read every clean
       working copy as dirty (it can't equal a baseline it
       never saw), and a remote apply would always surface
       as a conflict instead of being adopted. */
    const bool was_dirty = Dirty();
    const bool changed   = (baseline_ != doc_or_null);
    baseline_ = doc_or_null;
    if(!was_dirty)
    {
        /* No pending edits — the applied document IS the
           working copy. */
        working_ = baseline_;
        conflict_ = false;
    }
    else if(changed)
    {
        /* The applied scene moved under the user's pending
           edits (another client, a re-apply, an engine-side
           augmentation). Keep the edits — the conflict flag
           makes the UI say so rather than silently choosing
           a side. */
        conflict_ = true;
    }
}

void SceneEditModel::LoadWorking(const nlohmann::json& doc)
{
    working_ = doc.is_object() ? doc : nlohmann::json(nullptr);
    conflict_ = false;
}

void SceneEditModel::Commit()
{
    baseline_ = working_;
    conflict_ = false;
    preset_replaced_layers_ = false;
}

void SceneEditModel::DiscardEdits()
{
    working_ = baseline_;
    conflict_ = false;
    preset_replaced_layers_ = false;
}

void SceneEditModel::AdoptApplied(const nlohmann::json& doc)
{
    /* The working copy only differs from the committed
       baseline when the user edited again after the save —
       keep those pending edits (dirty recomputes against the
       new baseline). Otherwise adopt the normalized document
       verbatim so the reserialization can't read as an edit. */
    if(working_ == baseline_)
    {
        working_ = doc;
    }
    baseline_ = doc;
    conflict_ = false;
}

bool SceneEditModel::HasBaseline() const
{
    return baseline_.is_object();
}

bool SceneEditModel::HasWorking() const
{
    return working_.is_object();
}

bool SceneEditModel::Dirty() const
{
    return working_.is_object() && working_ != baseline_;
}

void SceneEditModel::EnsureWorking()
{
    if(!working_.is_object())
    {
        working_ = baseline_.is_object() ? baseline_
                                         : NewDocument();
    }
}

/*---------------------------------------------------------*\
||| Edits                                                  ||
\*---------------------------------------------------------*/

bool SceneEditModel::SetPreset(const std::string& id,
                               std::string& err)
{
    if(id.empty())
    {
        err = "empty preset id";
        return false;
    }
    EnsureWorking();
    nlohmann::json& e = working_["effect"];
    if(!e.is_object())
    {
        e = nlohmann::json::object();
    }
    /* Choosing a named preset replaces any inline layer stack:
       the engine resolves layers FIRST, so a stale stack would
       silently keep the old effect. The flag lets the UI say
       so when the baseline authored one. */
    preset_replaced_layers_ =
        e.contains("layers") && e["layers"].is_array() &&
        !e["layers"].empty();
    e["preset"] = id;
    e["layers"] = nlohmann::json::array();
    /* A picked look is meant to run. */
    e["playing"] = true;
    return true;
}

bool SceneEditModel::SetName(const std::string& name,
                             std::string& err)
{
    /* Trim — a name is what the user sees, and leading or
       trailing whitespace would silently hide it in every
       list it appears in. Reject empties: a nameless scene
       can't be found or spoken about (Task 4.8 minor). */
    std::string trimmed = name;
    const std::size_t b = trimmed.find_first_not_of(" \t\r\n");
    const std::size_t e = trimmed.find_last_not_of(" \t\r\n");
    trimmed = (b == std::string::npos)
                  ? std::string()
                  : trimmed.substr(b, e - b + 1);
    if(trimmed.empty())
    {
        err = "scene name is empty";
        return false;
    }
    if(trimmed.size() > 128)
    {
        err = "scene name over 128 characters";
        return false;
    }
    EnsureWorking();
    working_["name"] = trimmed;
    return true;
}

bool SceneEditModel::SetBrightness(double v)
{
    if(!std::isfinite(v))
    {
        return false;
    }
    EnsureWorking();
    working_["brightness"] = Clamp(v, 0.0, 1.0);
    return true;
}

bool SceneEditModel::SetEffectSpeed(double v)
{
    if(!std::isfinite(v))
    {
        return false;
    }
    EnsureWorking();
    nlohmann::json& e = working_["effect"];
    if(!e.is_object())
    {
        e = nlohmann::json::object();
    }
    e["speed"] = Clamp(v, kEffectSpeedMin, kEffectSpeedMax);
    return true;
}

bool SceneEditModel::SetEffectIntensity(double v)
{
    if(!std::isfinite(v))
    {
        return false;
    }
    EnsureWorking();
    nlohmann::json& e = working_["effect"];
    if(!e.is_object())
    {
        e = nlohmann::json::object();
    }
    e["intensity"] = Clamp(v, 0.0, 1.0);
    return true;
}

bool SceneEditModel::SetObjectColor(const std::string& object_id,
                                    const std::string& hex,
                                    std::string& err)
{
    if(object_id.empty())
    {
        err = "no object selected";
        return false;
    }
    if(!IsHexColor(hex))
    {
        err = "color must be \"#RRGGBB\"";
        return false;
    }
    EnsureWorking();
    nlohmann::json& c = working_["object_colors"];
    if(!c.is_object())
    {
        c = nlohmann::json::object();
    }
    c[object_id] = NormalizeHexColor(hex);
    return true;
}

bool SceneEditModel::ClearObjectColor(const std::string& object_id)
{
    EnsureWorking();
    if(working_["object_colors"].is_object())
    {
        working_["object_colors"].erase(object_id);
    }
    return true;
}

/*---------------------------------------------------------*\
||| Group selection                                        ||
\*---------------------------------------------------------*/

void SceneEditModel::SetSelectedGroups(
    const std::vector<std::string>& ids)
{
    selected_ = ids;
    ApplyTargetsToLayers();
}

bool SceneEditModel::GroupsTargetable() const
{
    return working_.is_object() &&
           working_.contains("effect") &&
           working_["effect"].is_object() &&
           working_["effect"].contains("layers") &&
           working_["effect"]["layers"].is_array() &&
           !working_["effect"]["layers"].empty();
}

void SceneEditModel::ApplyTargetsToLayers()
{
    if(!GroupsTargetable())
    {
        return;
    }
    nlohmann::json targets = nlohmann::json::array();
    for(const std::string& id : selected_)
    {
        targets.push_back(id);
    }
    for(nlohmann::json& layer : working_["effect"]["layers"])
    {
        if(layer.is_object())
        {
            layer["targets"] = targets;
        }
    }
}

/*---------------------------------------------------------*\
||| Views                                                  ||
\*---------------------------------------------------------*/

std::vector<DeviceGroup> SceneEditModel::DeviceGroups() const
{
    std::vector<DeviceGroup> out;
    if(!working_.is_object() ||
       !working_.contains("objects") ||
       !working_["objects"].is_array())
    {
        return out;
    }

    const auto find_binding =
        [this](const std::string& id) -> const nlohmann::json*
        {
            if(!working_.contains("bindings") ||
               !working_["bindings"].is_array())
            {
                return nullptr;
            }
            for(const nlohmann::json& b : working_["bindings"])
            {
                if(b.is_object() && JStr(b, "id") == id)
                {
                    return &b;
                }
            }
            return nullptr;
        };

    for(const nlohmann::json& o : working_["objects"])
    {
        if(!o.is_object() || JStr(o, "kind") != "device")
        {
            continue;
        }
        DeviceGroup g;
        g.id       = JStr(o, "id");
        g.label    = JStr(o, "label");
        if(g.label.empty())
        {
            g.label = g.id;
        }
        g.binding  = JStr(o, "binding");
        g.verified = JBool(o, "verified");
        if(o.contains("emitters") && o["emitters"].is_array())
        {
            g.emitter_count = o["emitters"].size();
        }
        if(!g.binding.empty())
        {
            if(const nlohmann::json* b = find_binding(g.binding))
            {
                g.controller_name = JStr(*b, "controller_name");
                g.vendor          = JStr(*b, "vendor");
                g.serial          = JStr(*b, "serial");
                g.location        = JStr(*b, "location");
                g.zone_name       = JStr(*b, "zone_name");
                const auto dt = b->find("device_type");
                if(dt != b->end() && dt->is_number())
                {
                    g.device_type = (int)dt->get<double>();
                }
                const auto zl = b->find("zone_leds");
                if(zl != b->end() && zl->is_number())
                {
                    const double v = zl->get<double>();
                    g.zone_leds = v > 0 ? (uint32_t)v : 0;
                }
            }
        }
        if(!g.id.empty())
        {
            out.push_back(std::move(g));
        }
    }
    /* Stable display order — the document order, not a
       reserialized-map order. */
    return out;
}

std::string SceneEditModel::PresetId() const
{
    if(working_.is_object() && working_.contains("effect") &&
       working_["effect"].is_object())
    {
        return JStr(working_["effect"], "preset");
    }
    return std::string();
}

nlohmann::json SceneEditModel::BuildSetSceneBody() const
{
    return nlohmann::json::object(
        { { "document", working_.is_object()
                            ? working_
                            : nlohmann::json(nullptr) } });
}

} /* namespace ui */
} /* namespace openrgb2 */
