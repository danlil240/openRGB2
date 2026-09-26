/*---------------------------------------------------------*\
||| StudioConfig.cpp                                          |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "StudioConfig.h"
#include "../scene/SceneJson.h"
#include "../scene/JsonFields.h"
#include "../presets/DevicePreset.h"
#include "../effects/Presets.h"
#include "../effects/EffectJson.h"

#include <set>
#include <vector>

namespace studio
{
namespace
{

/* The checked field readers (IsInt/AddErr/FieldStr/FieldBool/
   FieldNum/FieldInt/FieldI32/FieldU32) are shared in
   scene/JsonFields.h - one copy for the scene and config
   validators; this file keeps only the workspace-specific
   Section/FieldEnum helpers. */

/* If `key` exists on j it must be an object; returns the member or
   nullptr when absent/invalid. */
const nlohmann::json* Section(const nlohmann::json& j, const char* key,
                              std::vector<std::string>& errs)
{
    if(!j.contains(key))
    {
        return nullptr;
    }
    if(!j[key].is_object())
    {
        AddErr(errs, key, "expected object");
        return nullptr;
    }
    return &j[key];
}

std::string FieldEnum(const nlohmann::json& j, const char* key,
                      const std::string& def,
                      std::initializer_list<const char*> allowed,
                      const std::string& path,
                      std::vector<std::string>& errs)
{
    const std::string v = FieldStr(j, key, def, path, errs);
    if(v != def)   /* only re-check values the user actually set */
    {
        bool ok = false;
        for(const char* a : allowed)
        {
            if(v == a) { ok = true; break; }
        }
        if(!ok)
        {
            AddErr(errs, path + "." + key, "unknown value '" + v + "'");
            return def;
        }
    }
    return v;
}

/* Split an instance/entity path on '/' and check every segment is
   a legal id. */
std::vector<std::string> PathSegments(const std::string& path)
{
    std::vector<std::string> segs;
    std::string cur;
    for(char c : path)
    {
        if(c == '/')
        {
            segs.push_back(cur);
            cur.clear();
        }
        else
        {
            cur += c;
        }
    }
    segs.push_back(cur);
    return segs;
}

bool ValidPath(const std::string& path)
{
    for(const std::string& s : PathSegments(path))
    {
        if(!IsPresetId(s))
        {
            return false;
        }
    }
    return true;
}

nlohmann::json BindingToJson(const DeviceBinding& b)
{
    return {
        {"controller_name", b.controller_name},
        {"vendor",          b.vendor},
        {"serial",          b.serial},
        {"location",        b.location},
        {"device_type",     b.device_type},
        {"zone_name",       b.zone_name},
        {"zone_leds",       b.zone_leds},
    };
}

DeviceBinding BindingFromJson(const nlohmann::json& jb,
                              const std::string& id,
                              const std::string& path,
                              std::vector<std::string>& errs)
{
    DeviceBinding b;
    b.id              = id;
    b.controller_name = FieldStr(jb, "controller_name", std::string(), path, errs);
    b.vendor          = FieldStr(jb, "vendor", std::string(), path, errs);
    b.serial          = FieldStr(jb, "serial", std::string(), path, errs);
    b.location        = FieldStr(jb, "location", std::string(), path, errs);
    b.device_type     = FieldI32(jb, "device_type", -1, path, errs);
    b.zone_name       = FieldStr(jb, "zone_name", std::string(), path, errs);
    const long long leds = FieldInt(jb, "zone_leds", 0, path, errs);
    b.zone_leds       = leds <= 0 ? 0u
                      : (leds > 4294967295ll ? 4294967295u
                                             : (unsigned int)leds);
    return b;
}

} /* anonymous namespace */

nlohmann::json ToJson(const StudioDocument& doc)
{
    nlohmann::json effects = {
        {"preset",    doc.effect.preset},
        {"seed",      doc.effect.seed},
        {"speed",     doc.effect.speed},
        {"intensity", doc.effect.intensity},
        {"playing",   doc.effect.playing},
        /* The user's authored inline layer stack — resolved
           literals only (the validated EffectLayer grammar).
           Empty = the named preset resolves through the
           effect-look registry at runtime. */
        {"layers",    EffectLayersToJson(doc.effect.layers)},
    };

    nlohmann::json devices = nlohmann::json::object();
    for(const auto& kv : doc.devices)
    {
        const DeviceInstance& d = kv.second;
        nlohmann::json jd = {
            {"type", d.type},
            {"x",    d.position.x},
            {"y",    d.position.y},
            {"z",    d.position.z},
            {"rx",   d.rotation_deg.x},
            {"ry",   d.rotation_deg.y},
            {"rz",   d.rotation_deg.z},
        };
        if(!d.parent.empty())
        {
            jd["parent"] = d.parent;
        }
        devices[kv.first] = jd;
    }

    nlohmann::json bindings = nlohmann::json::object();
    for(const auto& kv : doc.bindings)
    {
        bindings[kv.first] = BindingToJson(kv.second);
    }

    nlohmann::json settings = nlohmann::json::object();
    for(const auto& kv : doc.device_settings)
    {
        const DeviceSettings& s = kv.second;
        nlohmann::json js = nlohmann::json::object();
        if(s.extra.is_object())
        {
            /* unknown fields first — known keys overwrite below so a
               stale retained field can't shadow live state */
            js = s.extra;
        }
        if(!s.visible)
        {
            js["visible"] = false;
        }
        if(s.locked)
        {
            js["locked"] = true;
        }
        if(!s.mirror_of.empty())
        {
            js["mirror_of"] = s.mirror_of;
        }
        if(!s.zones.empty())
        {
            nlohmann::json zones = nlohmann::json::object();
            for(const auto& zkv : s.zones)
            {
                const ZoneSetting& z = zkv.second;
                nlohmann::json jz = nlohmann::json::object();
                if(!z.binding.empty())
                {
                    jz["binding"] = z.binding;
                }
                if(z.addr_base != 0)
                {
                    jz["addr_base"] = z.addr_base;
                }
                if(z.verified)
                {
                    jz["verified"] = true;
                }
                zones[zkv.first] = jz;
            }
            js["zones"] = zones;
        }
        settings[kv.first] = js;
    }

    nlohmann::json objects = nlohmann::json::object();
    for(const auto& kv : doc.object_colors)
    {
        objects[kv.first] = SceneColorHex(kv.second);
    }
    nlohmann::json emitters = nlohmann::json::object();
    for(const auto& kv : doc.emitter_colors)
    {
        nlohmann::json inner = nlohmann::json::object();
        for(const auto& e : kv.second)
        {
            inner[std::to_string(e.first)] = SceneColorHex(e.second);
        }
        emitters[kv.first] = inner;
    }
    nlohmann::json colors = {
        {"objects",  objects},
        {"emitters", emitters},
    };

    nlohmann::json j;
    j["$schema"]        = "schemas/studio.schema.json";
    j["schema_version"] = doc.schema_version;
    j["name"]           = doc.meta.name;
    j["ui"] = {
        {"theme",          doc.meta.ui.theme},
        {"reduced_motion", doc.meta.ui.reduced_motion},
    };
    if(!doc.meta.ui.favorites.empty())
    {
        nlohmann::json favs = nlohmann::json::array();
        for(const std::string& f : doc.meta.ui.favorites)
        {
            favs.push_back(f);
        }
        j["ui"]["favorites"] = favs;
    }
    j["camera"] = {
        {"view",       doc.meta.camera.view},
        {"projection", doc.meta.camera.projection},
        {"target", {
            {"x", doc.meta.camera.target.x},
            {"y", doc.meta.camera.target.y},
            {"z", doc.meta.camera.target.z},
        }},
        {"yaw_deg",   doc.meta.camera.yaw_deg},
        {"pitch_deg", doc.meta.camera.pitch_deg},
        {"distance",  doc.meta.camera.distance},
        {"span",      doc.meta.camera.span},
    };
    j["controls"] = {
        {"middle_drag",     doc.meta.controls.middle_drag},
        {"move_snap_m",     doc.meta.controls.move_snap_m},
        {"rotate_snap_deg", doc.meta.controls.rotate_snap_deg},
    };
    j["render"] = {
        {"quality", doc.meta.render.quality},
        {"bloom",   doc.meta.render.bloom},
    };
    j["inputs"] = {
        {"audio",        doc.inputs.audio},
        {"keys",         doc.inputs.keys},
        {"screen",       doc.inputs.screen},
        {"screen_index", doc.inputs.screen_index},
        {"sens_pct",     doc.inputs.sens_pct},
        {"decay_pct",    doc.inputs.decay_pct},
    };
    j["output"] = {
        {"brightness",       doc.brightness},
        {"live_on_startup",  doc.meta.live_on_startup},
    };
    j["devices"]         = devices;
    j["bindings"]        = bindings;
    j["device_settings"] = settings;
    j["colors"]          = colors;
    j["effects"]         = effects;
    j["extensions"]      = doc.meta.extensions.is_object()
        ? doc.meta.extensions
        : nlohmann::json::object();
    return j;
}

bool FromJson(const nlohmann::json& j, StudioDocument& doc,
              std::vector<std::string>* errors,
              std::vector<std::string>* warnings)
{
    std::vector<std::string> errs, warns;

    if(!j.is_object())
    {
        errs.push_back("root: expected object");
        if(errors) { *errors = errs; }
        return false;
    }

    /*------------------------------------------------*\
    || schema_version — newer docs are rejected        ||
    || untouched rather than silently downgraded; the  ||
    || expanded v2 format migrates at the file layer   ||
    || (ConfigStore), never inside FromJson.           ||
    \*------------------------------------------------*/
    if(!j.contains("schema_version"))
    {
        errs.push_back("schema_version: missing");
    }
    else if(!IsInt(j["schema_version"]))
    {
        errs.push_back("schema_version: expected integer");
    }
    else
    {
        const long long v = j["schema_version"].get<long long>();
        if(v < STUDIO_SCHEMA_VERSION)
        {
            AddErr(errs, "schema_version",
                   std::to_string(v) + " is the expanded format —"
                   " it migrates on file load (ConfigStore)");
        }
        else if(v > STUDIO_SCHEMA_VERSION)
        {
            AddErr(errs, "schema_version",
                   std::to_string(v) + " is newer than supported "
                   + std::to_string(STUDIO_SCHEMA_VERSION));
        }
    }

    /* Expanded sections must never appear in a v3 workspace —
       entities and embedded definitions live in the type files. */
    if(j.contains("scene"))
    {
        AddErr(errs, "scene",
               "expanded scenes must not be serialized in schema v3");
    }
    if(j.contains("definitions"))
    {
        AddErr(errs, "definitions",
               "embedded device definitions must not be serialized"
               " in schema v3");
    }

    StudioDocument out;

    if(j.contains("name"))
    {
        if(j["name"].is_string())
        {
            out.meta.name = j["name"].get<std::string>();
        }
        else
        {
            errs.push_back("name: expected string");
        }
    }

    /*------------------------------------------------*\
    || Editor preference sections                      ||
    \*------------------------------------------------*/
    if(const nlohmann::json* s = Section(j, "ui", errs))
    {
        out.meta.ui.theme = FieldStr(*s, "theme", out.meta.ui.theme,
                                     "ui", errs);
        out.meta.ui.reduced_motion = FieldBool(*s, "reduced_motion",
                                     out.meta.ui.reduced_motion, "ui", errs);
        /* ui.favorites — recoverable list: malformed entries drop
           with a warning, order is kept, duplicates collapse. */
        if(s->contains("favorites"))
        {
            const nlohmann::json& f = (*s)["favorites"];
            if(!f.is_array())
            {
                warns.push_back("ui.favorites: expected array"
                                " — dropped");
            }
            else
            {
                std::set<std::string> seen;
                int i = 0;
                for(const nlohmann::json& v : f)
                {
                    const std::string fp = "ui.favorites["
                                           + std::to_string(i++) + "]";
                    if(!v.is_string())
                    {
                        warns.push_back(fp + ": dropped non-string"
                                        " entry");
                        continue;
                    }
                    const std::string id = v.get<std::string>();
                    if(!IsPresetId(id))
                    {
                        warns.push_back(fp + ": dropped invalid"
                                        " identifier '" + id + "'");
                        continue;
                    }
                    if(seen.insert(id).second)
                    {
                        out.meta.ui.favorites.push_back(id);
                    }
                }
            }
        }
    }
    if(const nlohmann::json* s = Section(j, "camera", errs))
    {
        out.meta.camera.view = FieldEnum(*s, "view", out.meta.camera.view,
            {"desk", "top", "front", "case", "free"}, "camera", errs);
        out.meta.camera.projection = FieldEnum(*s, "projection",
            out.meta.camera.projection,
            {"orthographic", "perspective"}, "camera", errs);
        /* Pose fields are optional — a camera section that only
           carries view/projection (pre-2.2 files) keeps defaults. */
        if(const nlohmann::json* t = Section(*s, "target", errs))
        {
            CameraPrefs& c = out.meta.camera;
            c.target.x = (float)FieldNum(*t, "x", c.target.x,
                                         "camera.target", errs);
            c.target.y = (float)FieldNum(*t, "y", c.target.y,
                                         "camera.target", errs);
            c.target.z = (float)FieldNum(*t, "z", c.target.z,
                                         "camera.target", errs);
        }
        CameraPrefs& c = out.meta.camera;
        c.yaw_deg   = (float)FieldNum(*s, "yaw_deg",   c.yaw_deg,   "camera", errs);
        c.pitch_deg = (float)FieldNum(*s, "pitch_deg", c.pitch_deg, "camera", errs);
        c.distance  = (float)FieldNum(*s, "distance",  c.distance,  "camera", errs);
        c.span      = (float)FieldNum(*s, "span",      c.span,      "camera", errs);
        if(c.distance <= 0.0f) { c.distance = 1.21f; }
        if(c.span     <= 0.0f) { c.span     = 0.9f;  }
    }
    if(const nlohmann::json* s = Section(j, "controls", errs))
    {
        out.meta.controls.middle_drag = FieldEnum(*s, "middle_drag",
            out.meta.controls.middle_drag, {"pan", "orbit"}, "controls", errs);
        const double snap = FieldNum(*s, "move_snap_m",
            out.meta.controls.move_snap_m, "controls", errs);
        if(snap > 0.0)
        {
            out.meta.controls.move_snap_m = (float)snap;
        }
        else if(s->contains("move_snap_m"))
        {
            AddErr(errs, "controls.move_snap_m", "expected number > 0");
        }
        const double rot = FieldNum(*s, "rotate_snap_deg",
            out.meta.controls.rotate_snap_deg, "controls", errs);
        if(rot > 0.0)
        {
            out.meta.controls.rotate_snap_deg = (float)rot;
        }
        else if(s->contains("rotate_snap_deg"))
        {
            AddErr(errs, "controls.rotate_snap_deg", "expected number > 0");
        }
    }
    if(const nlohmann::json* s = Section(j, "render", errs))
    {
        out.meta.render.quality = FieldEnum(*s, "quality",
            out.meta.render.quality, {"low", "balanced", "high"},
            "render", errs);
        out.meta.render.bloom = FieldBool(*s, "bloom",
            out.meta.render.bloom, "render", errs);
    }

    /*------------------------------------------------*\
    || inputs                                          ||
    \*------------------------------------------------*/
    if(const nlohmann::json* s = Section(j, "inputs", errs))
    {
        out.inputs.audio  = FieldBool(*s, "audio",  out.inputs.audio,
                                      "inputs", errs);
        out.inputs.keys   = FieldBool(*s, "keys",   out.inputs.keys,
                                      "inputs", errs);
        out.inputs.screen = FieldBool(*s, "screen", out.inputs.screen,
                                      "inputs", errs);
        const long long idx = FieldInt(*s, "screen_index",
            out.inputs.screen_index, "inputs", errs);
        if(idx < 0 || idx > 2147483647ll)
        {
            AddErr(errs, "inputs.screen_index",
                   "expected integer in 0..2147483647");
        }
        else
        {
            out.inputs.screen_index = (int)idx;
        }
        const long long sens = FieldInt(*s, "sens_pct",
            out.inputs.sens_pct, "inputs", errs);
        if(sens < 25 || sens > 200)
        {
            AddErr(errs, "inputs.sens_pct", "expected integer in 25..200");
        }
        else
        {
            out.inputs.sens_pct = (int)sens;
        }
        const long long decay = FieldInt(*s, "decay_pct",
            out.inputs.decay_pct, "inputs", errs);
        if(decay < 50 || decay > 300)
        {
            AddErr(errs, "inputs.decay_pct", "expected integer in 50..300");
        }
        else
        {
            out.inputs.decay_pct = (int)decay;
        }
    }

    /*------------------------------------------------*\
    || output                                          ||
    \*------------------------------------------------*/
    if(const nlohmann::json* s = Section(j, "output", errs))
    {
        if(s->contains("brightness"))
        {
            const double b = FieldNum(*s, "brightness", 1.0, "output", errs);
            if(b < 0.0 || b > 1.0)
            {
                AddErr(errs, "output.brightness", "expected number in 0..1");
            }
            else if(s->at("brightness").is_number())
            {
                out.brightness = (float)b;
            }
        }
        out.meta.live_on_startup = FieldBool(*s, "live_on_startup",
            out.meta.live_on_startup, "output", errs);
    }

    /*------------------------------------------------*\
    || effects                                         ||
    \*------------------------------------------------*/
    if(const nlohmann::json* s = Section(j, "effects", errs))
    {
        out.effect.preset    = FieldStr(*s, "preset", std::string(),
                                        "effects", errs);
        out.effect.seed      = FieldU32(*s, "seed", 0, "effects", errs);
        const double speed = FieldNum(*s, "speed", 1.0, "effects", errs);
        if(speed <= 0.0)
        {
            AddErr(errs, "effects.speed", "expected number > 0");
        }
        else
        {
            out.effect.speed = (float)speed;
        }
        const double intensity = FieldNum(*s, "intensity", 1.0,
                                          "effects", errs);
        if(intensity < 0.0 || intensity > 1.0)
        {
            AddErr(errs, "effects.intensity", "expected number in 0..1");
        }
        else
        {
            out.effect.intensity = (float)intensity;
        }
        out.effect.playing = FieldBool(*s, "playing", false,
                                       "effects", errs);
        if(s->contains("layers"))
        {
            /* Typed + validated: the authored inline stack is real
               layer data (resolved literals — a remix spec here is
               an error). An empty array is meaningful: it says "no
               inline stack, resolve the named preset". */
            EffectLayersFromJson(s->at("layers"), out.effect.layers,
                                 &errs, "effects.layers");
        }
    }

    /*------------------------------------------------*\
    || devices — compact placements                    ||
    \*------------------------------------------------*/
    if(j.contains("devices"))
    {
        if(!j["devices"].is_object())
        {
            AddErr(errs, "devices", "expected object");
        }
        else
        {
            for(auto it = j["devices"].begin(); it != j["devices"].end(); ++it)
            {
                const std::string iid  = it.key();
                const nlohmann::json& jd = it.value();
                const std::string path = "devices." + iid;
                if(!IsPresetId(iid))
                {
                    AddErr(errs, path, "bad instance id '" + iid + "'");
                    continue;
                }
                if(!jd.is_object())
                {
                    AddErr(errs, path, "expected object");
                    continue;
                }
                DeviceInstance d;
                d.type           = FieldStr(jd, "type", std::string(),
                                            path, errs);
                if(!IsPresetId(d.type))
                {
                    AddErr(errs, path + ".type", "expected a device-type"
                           " id ([A-Za-z0-9_-])");
                }
                d.position.x     = (float)FieldNum(jd, "x", 0.0, path, errs);
                d.position.y     = (float)FieldNum(jd, "y", 0.0, path, errs);
                d.position.z     = (float)FieldNum(jd, "z", 0.0, path, errs);
                d.rotation_deg.x = (float)FieldNum(jd, "rx", 0.0, path, errs);
                d.rotation_deg.y = (float)FieldNum(jd, "ry", 0.0, path, errs);
                d.rotation_deg.z = (float)FieldNum(jd, "rz", 0.0, path, errs);
                d.parent         = FieldStr(jd, "parent", std::string(),
                                            path, errs);
                out.devices[iid] = d;
            }
            if(out.devices.size() > STUDIO_MAX_DEVICES)
            {
                AddErr(errs, "devices", "count exceeds cap "
                       + std::to_string(STUDIO_MAX_DEVICES));
            }
            /* parent refs + cycles */
            for(const auto& kv : out.devices)
            {
                const std::string& pid = kv.second.parent;
                if(pid.empty())
                {
                    continue;
                }
                if(!IsPresetId(pid) || out.devices.find(pid) == out.devices.end())
                {
                    AddErr(errs, "devices." + kv.first + ".parent",
                           "unknown instance '" + pid + "'");
                }
            }
            for(const auto& kv : out.devices)
            {
                std::set<std::string> seen;
                std::string cur = kv.first;
                while(!cur.empty())
                {
                    if(!seen.insert(cur).second)
                    {
                        AddErr(errs, "devices." + kv.first + ".parent",
                               "parent cycle through '" + cur + "'");
                        break;
                    }
                    const auto it = out.devices.find(cur);
                    cur = (it == out.devices.end()) ? std::string()
                                                  : it->second.parent;
                }
            }
        }
    }

    /*------------------------------------------------*\
    || bindings                                        ||
    \*------------------------------------------------*/
    if(j.contains("bindings"))
    {
        if(!j["bindings"].is_object())
        {
            AddErr(errs, "bindings", "expected object");
        }
        else
        {
            for(auto it = j["bindings"].begin(); it != j["bindings"].end(); ++it)
            {
                const std::string path = "bindings." + it.key();
                if(!IsPresetId(it.key()))
                {
                    AddErr(errs, path, "bad binding id '" + it.key() + "'");
                    continue;
                }
                if(!it.value().is_object())
                {
                    AddErr(errs, path, "expected object");
                    continue;
                }
                out.bindings[it.key()] =
                    BindingFromJson(it.value(), it.key(), path, errs);
            }
            if(out.bindings.size() > STUDIO_MAX_BINDINGS)
            {
                AddErr(errs, "bindings", "count exceeds cap "
                       + std::to_string(STUDIO_MAX_BINDINGS));
            }
        }
    }

    /*------------------------------------------------*\
    || device_settings                                 ||
    \*------------------------------------------------*/
    if(j.contains("device_settings"))
    {
        if(!j["device_settings"].is_object())
        {
            AddErr(errs, "device_settings", "expected object");
        }
        else
        {
            for(auto it = j["device_settings"].begin();
                it != j["device_settings"].end(); ++it)
            {
                const std::string spath = it.key();
                const nlohmann::json& js = it.value();
                const std::string path = "device_settings." + spath;
                if(!ValidPath(spath))
                {
                    AddErr(errs, path, "bad instance path '" + spath + "'");
                    continue;
                }
                /* The first segment must be a root device instance;
                   deeper segments are validated on resolution. */
                const std::string root = PathSegments(spath).front();
                if(out.devices.find(root) == out.devices.end())
                {
                    AddErr(errs, path,
                           "unknown device instance '" + root + "'");
                    continue;
                }
                if(!js.is_object())
                {
                    AddErr(errs, path, "expected object");
                    continue;
                }
                DeviceSettings s;
                s.visible   = FieldBool(js, "visible", true, path, errs);
                s.locked    = FieldBool(js, "locked", false, path, errs);
                s.mirror_of = FieldStr(js, "mirror_of", std::string(),
                                       path, errs);
                if(!s.mirror_of.empty())
                {
                    if(!ValidPath(s.mirror_of))
                    {
                        AddErr(errs, path + ".mirror_of",
                               "bad instance path '" + s.mirror_of + "'");
                    }
                    else
                    {
                        const std::string mroot =
                            PathSegments(s.mirror_of).front();
                        if(out.devices.find(mroot) == out.devices.end())
                        {
                            AddErr(errs, path + ".mirror_of",
                                   "unknown device instance '" + mroot + "'");
                        }
                    }
                    if(s.mirror_of == spath)
                    {
                        AddErr(errs, path + ".mirror_of", "self mirror");
                    }
                }
                if(js.contains("zones"))
                {
                    if(!js["zones"].is_object())
                    {
                        AddErr(errs, path + ".zones", "expected object");
                    }
                    else
                    {
                        for(auto zt = js["zones"].begin();
                            zt != js["zones"].end(); ++zt)
                        {
                            const std::string zp = path + ".zones." + zt.key();
                            if(!IsPresetId(zt.key()))
                            {
                                AddErr(errs, zp, "bad zone id '" + zt.key() + "'");
                                continue;
                            }
                            if(!zt.value().is_object())
                            {
                                AddErr(errs, zp, "expected object");
                                continue;
                            }
                            ZoneSetting z;
                            z.binding   = FieldStr(zt.value(), "binding",
                                                   std::string(), zp, errs);
                            z.addr_base = FieldI32(zt.value(), "addr_base",
                                                   0, zp, errs);
                            z.verified  = FieldBool(zt.value(), "verified",
                                                    false, zp, errs);
                            if(z.addr_base < 0)
                            {
                                AddErr(errs, zp + ".addr_base",
                                       "expected >= 0");
                            }
                            if(!z.binding.empty()
                               && out.bindings.find(z.binding) == out.bindings.end())
                            {
                                AddErr(errs, zp + ".binding",
                                       "unknown binding '" + z.binding + "'");
                            }
                            s.zones[zt.key()] = z;
                        }
                        if(s.zones.size() > STUDIO_MAX_ZONE_SET)
                        {
                            AddErr(errs, path + ".zones", "count exceeds cap "
                                   + std::to_string(STUDIO_MAX_ZONE_SET));
                        }
                    }
                }
                /* Retain unknown fields verbatim. */
                static const std::set<std::string> known = {
                    "visible", "locked", "mirror_of", "zones",
                };
                for(auto kt = js.begin(); kt != js.end(); ++kt)
                {
                    if(known.find(kt.key()) == known.end())
                    {
                        s.extra[kt.key()] = kt.value();
                    }
                }
                out.device_settings[spath] = s;
            }
            if(out.device_settings.size() > STUDIO_MAX_SETTINGS)
            {
                AddErr(errs, "device_settings", "count exceeds cap "
                       + std::to_string(STUDIO_MAX_SETTINGS));
            }
        }
    }

    /*------------------------------------------------*\
    || colors                                          ||
    \*------------------------------------------------*/
    if(const nlohmann::json* s = Section(j, "colors", errs))
    {
        if(s->contains("objects"))
        {
            const nlohmann::json& oc = (*s)["objects"];
            if(!oc.is_object())
            {
                AddErr(errs, "colors.objects", "expected object");
            }
            else
            {
                for(auto it = oc.begin(); it != oc.end(); ++it)
                {
                    const std::string path = "colors.objects." + it.key();
                    if(!ValidPath(it.key()))
                    {
                        AddErr(errs, path, "bad object path '" + it.key() + "'");
                        continue;
                    }
                    SceneColor c = 0;
                    if(ParseSceneColor(it.value(), c))
                    {
                        out.object_colors[it.key()] = c;
                    }
                    else
                    {
                        AddErr(errs, path,
                               "expected \"#RRGGBB\" or packed integer");
                    }
                }
                if(out.object_colors.size() > STUDIO_MAX_COLOR_KEYS)
                {
                    AddErr(errs, "colors.objects", "count exceeds cap "
                           + std::to_string(STUDIO_MAX_COLOR_KEYS));
                }
            }
        }
        if(s->contains("emitters"))
        {
            const nlohmann::json& ec = (*s)["emitters"];
            if(!ec.is_object())
            {
                AddErr(errs, "colors.emitters", "expected object");
            }
            else
            {
                for(auto it = ec.begin(); it != ec.end(); ++it)
                {
                    const std::string path = "colors.emitters." + it.key();
                    if(!ValidPath(it.key()))
                    {
                        AddErr(errs, path, "bad object path '" + it.key() + "'");
                        continue;
                    }
                    if(!it.value().is_object())
                    {
                        AddErr(errs, path, "expected object");
                        continue;
                    }
                    for(auto e = it.value().begin(); e != it.value().end(); ++e)
                    {
                        int index = -1;
                        try
                        {
                            index = std::stoi(e.key());
                        }
                        catch(...)
                        {
                        }
                        if(index < 0)
                        {
                            AddErr(errs, path,
                                   "bad emitter index '" + e.key() + "'");
                            continue;
                        }
                        SceneColor c = 0;
                        if(ParseSceneColor(e.value(), c))
                        {
                            out.emitter_colors[it.key()][index] = c;
                        }
                        else
                        {
                            AddErr(errs, path + "." + e.key(),
                                   "expected \"#RRGGBB\" or packed integer");
                        }
                    }
                }
                if(out.emitter_colors.size() > STUDIO_MAX_COLOR_KEYS)
                {
                    AddErr(errs, "colors.emitters", "count exceeds cap "
                           + std::to_string(STUDIO_MAX_COLOR_KEYS));
                }
            }
        }
    }

    /*------------------------------------------------*\
    || extensions — retained verbatim                  ||
    \*------------------------------------------------*/
    if(const nlohmann::json* s = Section(j, "extensions", errs))
    {
        out.meta.extensions = *s;
    }

    /*------------------------------------------------*\
    || Non-fatal notes                                 ||
    \*------------------------------------------------*/
    static const std::set<std::string> known = {
        "$schema", "schema_version", "name", "ui", "camera", "controls",
        "render", "inputs", "output", "devices", "bindings",
        "device_settings", "colors", "effects", "extensions",
        /* handled above as hard errors, listed so they don't also
           warn: */ "scene", "definitions",
    };
    for(auto it = j.begin(); it != j.end(); ++it)
    {
        if(known.find(it.key()) == known.end())
        {
            warns.push_back("unknown field '" + it.key()
                            + "' — possible typo");
        }
    }
    if(!out.effect.preset.empty()
       && FindPreset(out.effect.preset) == nullptr)
    {
        warns.push_back("effects.preset: unknown preset '"
                        + out.effect.preset + "'");
    }

    if(!errs.empty())
    {
        if(errors)   { *errors   = errs;  }
        if(warnings) { *warnings = warns; }
        return false;
    }
    if(errors)   { errors->clear();   }
    if(warnings) { *warnings = warns; }
    doc = out;
    return true;
}

} /* namespace studio */
