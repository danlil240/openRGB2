/*---------------------------------------------------------*\
||| ConfigMigration.cpp                                       |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "ConfigMigration.h"
#include "../scene/SceneJson.h"
#include "../scene/SceneResolver.h"
#include "../presets/PresetRegistry.h"

#include <limits>
#include <map>
#include <set>

namespace studio
{
namespace
{

std::string SanitizeId(const std::string& s)
{
    std::string out;
    for(char c : s)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                     || (c >= '0' && c <= '9') || c == '_' || c == '-';
        out += ok ? c : '_';
    }
    return out.empty() ? "type" : out;
}

/* The object whose STRUCTURE defines the instance's type: a
   Linked copy carries no emitters — its definition is the
   mirror owner's. */
const SceneObject* StructuralSource(const SceneDocument& scene,
                                    const SceneObject& o)
{
    if(o.kind == ObjectKind::Linked)
    {
        const SceneObject* owner = FindObject(scene, o.mirror_of);
        if(owner != nullptr && owner->kind != ObjectKind::Linked)
        {
            return owner;
        }
    }
    return &o;
}

/* Emitter addresses contiguous from `base`? Then the zone can use
   addr_base instead of explicit per-point addresses. */
bool ContiguousAddresses(const std::vector<Emitter>& emitters, int& base)
{
    if(emitters.empty())
    {
        base = 0;
        return true;
    }
    base = std::numeric_limits<int>::max();
    std::set<int> seen;
    for(const Emitter& e : emitters)
    {
        if(e.address < 0 || !seen.insert(e.address).second)
        {
            return false;   /* render-only or duplicate address */
        }
        if(e.address < base)
        {
            base = e.address;
        }
    }
    for(size_t i = 0; i < emitters.size(); i++)
    {
        if(seen.find(base + (int)i) == seen.end())
        {
            return false;
        }
    }
    return seen.size() == emitters.size();
}

/* Structural signature — placement (position/rotation) and
   physical identity (binding, verified, group names) are
   deliberately absent so equivalent definitions deduplicate.
   Emitter addresses: a CONTIGUOUS run normalizes to its base
   (the instance carries it as zone addr_base — same strip on a
   different address window is the same type). A sparse or mixed
   map keeps ABSOLUTE addresses instead: layout.addresses are
   absolute, so collapsing {0,2,4} and {10,12,14} into one type
   would hand the second instance the first's physical LEDs. */
nlohmann::json Signature(const SceneDocument& scene, const SceneObject& o)
{
    if(o.kind == ObjectKind::Group)
    {
        return "group";
    }
    const SceneObject* src = StructuralSource(scene, o);
    int base = 0;
    const bool contiguous = ContiguousAddresses(src->emitters, base);
    nlohmann::json sig;
    sig["g"]  = src->geometry;
    sig["s"]  = { src->size_m.x, src->size_m.y, src->size_m.z };
    sig["sc"] = { src->transform.scale.x, src->transform.scale.y,
                  src->transform.scale.z };
    sig["l"]  = src->layout;
    nlohmann::json em = nlohmann::json::array();
    for(const Emitter& e : src->emitters)
    {
        em.push_back({ e.local_pos.x, e.local_pos.y, e.local_pos.z,
                       e.address < 0 ? -1
                       : contiguous  ? e.address - base
                                     : e.address });
    }
    sig["e"] = em;
    return sig;
}

/* Build the DevicePreset for one structural source object. The
   local transform stays at identity — placement lives on the
   instance. A non-unit scale folds into size_m and emitter
   positions (the signature separated those variants already). */
DevicePreset TypeFromObject(const std::string& id, const SceneObject& src)
{
    DevicePreset p;
    p.id       = id;
    p.name     = src.label.empty() ? id : src.label;
    p.category = "migrated";

    PresetEntity e;
    e.id           = "body";
    e.geometry     = src.geometry;
    e.size_m.x     = src.size_m.x * src.transform.scale.x;
    e.size_m.y     = src.size_m.y * src.transform.scale.y;
    e.size_m.z     = src.size_m.z * src.transform.scale.z;

    if(!src.emitters.empty() || src.layout == "matrix_map")
    {
        DeviceZone z;
        z.id     = "main";
        z.entity = "body";
        e.zone   = "main";
        if(src.layout == "matrix_map")
        {
            z.layout.type    = "matrix";
            z.layout.dynamic = true;
        }
        else
        {
            int base = 0;
            const bool contiguous = ContiguousAddresses(src.emitters, base);
            z.layout.type = "points";
            z.led_count   = (unsigned int)src.emitters.size();
            for(const Emitter& em : src.emitters)
            {
                Vec3 v;
                v.x = em.local_pos.x * src.transform.scale.x;
                v.y = em.local_pos.y * src.transform.scale.y;
                v.z = em.local_pos.z * src.transform.scale.z;
                z.layout.points.push_back(v);
            }
            if(!contiguous)
            {
                /* Sparse/odd addressing — keep the exact map. */
                for(const Emitter& em : src.emitters)
                {
                    z.layout.addresses.push_back(em.address);
                }
            }
            /* contiguous: addresses = instance addr_base + index */
        }
        p.zones.push_back(z);
    }
    p.entities["body"] = e;
    return p;
}

} /* anonymous namespace */

bool HasLegacySettings(const nlohmann::json& legacy)
{
    return legacy.is_object() && !legacy.empty()
           && legacy.contains("scene") && legacy["scene"].is_object();
}

bool MigrateExpandedScene(const SceneDocument& scene,
                          StudioDocument& doc,
                          std::vector<DevicePreset>& types,
                          std::vector<std::string>* errors,
                          std::vector<std::string>* warnings)
{
    std::vector<std::string> errs, warns;

    StudioDocument w;
    std::vector<DevicePreset> out_types;
    w.meta.name            = scene.name;
    w.brightness           = scene.brightness;
    w.effect               = scene.effect;
    w.meta.live_on_startup = false;   /* migration never arms output */

    for(const DeviceBinding& b : scene.bindings)
    {
        if(w.bindings.find(b.id) != w.bindings.end())
        {
            warns.push_back("bindings." + b.id + ": duplicate id —"
                            " last wins");
        }
        w.bindings[b.id] = b;
    }

    /*------------------------------------------------*\
    || Pass 1 — type extraction + structural dedupe.   ||
    \*------------------------------------------------*/
    std::map<std::string, std::string> sig_to_type;   /* sig -> type id */
    std::map<std::string, std::string> type_sig;      /* type id -> sig */
    std::map<std::string, std::string> obj_type;      /* obj id -> type id */
    std::set<std::string> used_ids;

    for(const SceneObject& o : scene.objects)
    {
        const SceneObject* src = StructuralSource(scene, o);
        const std::string  sig = Signature(scene, o).dump();
        const auto it = sig_to_type.find(sig);
        if(it != sig_to_type.end())
        {
            obj_type[o.id] = it->second;
            continue;
        }

        /* New type — name from geometry when available so the desk
           gets readable ids like "fan_body"; collision with a
           DIFFERENT signature gets a numeric suffix (a preserved
           variant). */
        std::string base = (o.kind == ObjectKind::Group)
            ? "group"
            : (src->geometry.empty() ? SanitizeId(o.id)
                                     : SanitizeId(src->geometry));
        std::string id = base;
        for(int n = 2; used_ids.find(id) != used_ids.end(); n++)
        {
            id = base + "-" + std::to_string(n);
        }
        used_ids.insert(id);

        DevicePreset p;
        if(o.kind == ObjectKind::Group)
        {
            p.id       = id;
            p.name     = "Placement group";
            p.category = "group";
        }
        else
        {
            p = TypeFromObject(id, *src);
            p.id = id;   /* TypeFromObject used the same id */
        }
        std::vector<std::string> perrs;
        DevicePreset check;
        if(!DevicePresetFromJson(ToJson(p), check, &perrs))
        {
            for(const std::string& e : perrs)
            {
                errs.push_back("extracted type '" + id + "': " + e);
            }
            continue;
        }
        sig_to_type[sig] = id;
        type_sig[id]     = sig;
        obj_type[o.id]   = id;
        out_types.push_back(p);
    }

    /*------------------------------------------------*\
    || Pass 2 — compact instances + per-instance       ||
    || state. Every expanded object is one instance;   ||
    || parent_id maps to the optional instance parent. ||
    \*------------------------------------------------*/
    for(const SceneObject& o : scene.objects)
    {
        const auto tit = obj_type.find(o.id);
        if(tit == obj_type.end())
        {
            continue;   /* its type failed extraction — reported */
        }
        DeviceInstance d;
        d.type         = tit->second;
        d.position     = o.transform.position;
        d.rotation_deg = o.transform.rotation_deg;
        d.parent       = o.parent_id;
        w.devices[o.id] = d;

        DeviceSettings s;
        bool have = false;
        if(!o.visible)
        {
            s.visible = false;
            have = true;
        }
        if(o.kind == ObjectKind::Linked)
        {
            s.mirror_of = o.mirror_of;
            have = true;
        }
        else if(o.kind != ObjectKind::Group)
        {
            const SceneObject* src = &o;
            const bool has_zone = !src->emitters.empty()
                                  || src->layout == "matrix_map";
            ZoneSetting z;
            if(has_zone)
            {
                z.binding  = src->binding;
                z.verified = src->verified;
                int base = 0;
                if(ContiguousAddresses(src->emitters, base))
                {
                    z.addr_base = base;
                }
                s.zones["main"] = z;
                have = true;
            }
            else if(!src->binding.empty())
            {
                warns.push_back("devices." + o.id + ": binding '"
                                + src->binding + "' dropped — object"
                                " has no zone to carry it");
            }
        }
        if(have)
        {
            w.device_settings[o.id] = s;
        }
    }

    /*------------------------------------------------*\
    || Colors — re-key from object id to               ||
    || <instance>/<entity>. Group instances have no    ||
    || entities; a color keyed at one is dropped with  ||
    || a note rather than breaking the migration.      ||
    \*------------------------------------------------*/
    const auto rekey = [&](const std::string& key) -> std::string
    {
        const auto it = obj_type.find(key);
        if(it == obj_type.end() || it->second == "group"
           || type_sig[it->second] == "\"group\"")
        {
            return std::string();
        }
        return key + "/body";
    };
    for(const auto& kv : scene.object_colors)
    {
        const std::string nk = rekey(kv.first);
        if(nk.empty())
        {
            warns.push_back("colors.objects." + kv.first
                            + ": dropped (no emitters on a group)");
        }
        else
        {
            w.object_colors[nk] = kv.second;
        }
    }
    for(const auto& kv : scene.emitter_colors)
    {
        const std::string nk = rekey(kv.first);
        if(nk.empty())
        {
            warns.push_back("colors.emitters." + kv.first
                            + ": dropped (no emitters on a group)");
        }
        else
        {
            w.emitter_colors[nk] = kv.second;
        }
    }

    /*------------------------------------------------*\
    || Self-checks: the extracted types must form a    ||
    || valid registry and the workspace must round-    ||
    || trip + resolve — a migrated desk that can't     ||
    || rebuild its scene is a failed migration.        ||
    \*------------------------------------------------*/
    if(errs.empty())
    {
        PresetRegistry reg;
        std::vector<std::string> rerrs;
        for(const DevicePreset& p : out_types)
        {
            if(!reg.Add(p, &rerrs))
            {
                errs.insert(errs.end(), rerrs.begin(), rerrs.end());
            }
        }
        StudioDocument check;
        std::vector<std::string> cerrs;
        if(!FromJson(ToJson(w), check, &cerrs))
        {
            for(const std::string& e : cerrs)
            {
                errs.push_back("migrated workspace: " + e);
            }
        }
        else
        {
            SceneDocument resolved;
            std::vector<std::string> serrs;
            if(!ResolveScene(check, reg, resolved, &serrs))
            {
                for(const std::string& e : serrs)
                {
                    errs.push_back("migrated scene: " + e);
                }
            }
        }
    }

    if(!errs.empty())
    {
        if(errors)   { *errors   = errs;  }
        if(warnings) { *warnings = warns; }
        return false;
    }
    if(errors)   { errors->clear();   }
    if(warnings) { *warnings = warns; }
    doc   = w;
    types = out_types;
    return true;
}

bool MigrateLegacySettings(const nlohmann::json& legacy,
                           StudioDocument& doc,
                           std::vector<DevicePreset>* types,
                           std::vector<std::string>* errors)
{
    if(!HasLegacySettings(legacy))
    {
        if(errors)
        {
            errors->push_back("legacy: no saved scene in host settings");
        }
        return false;
    }

    /* Parse the legacy scene through the normal scene validator —
       v1 blobs get the v1->v2 upgrade inside SceneJson::FromJson.
       A malformed legacy save can never half-apply. */
    SceneDocument scene;
    std::vector<std::string> errs;
    if(!FromJson(legacy["scene"], scene, &errs))
    {
        if(errors)
        {
            for(std::string& e : errs)
            {
                errors->push_back("legacy.scene: " + e);
            }
        }
        return false;
    }

    std::vector<DevicePreset> t;
    std::vector<std::string> warns;
    if(!MigrateExpandedScene(scene, doc, t, &errs, &warns))
    {
        if(errors)
        {
            for(std::string& e : errs)
            {
                errors->push_back("legacy." + e);
            }
        }
        return false;
    }
    if(types != nullptr)
    {
        *types = t;
    }

    /* Input settings carry the same field names in both formats. */
    if(legacy.contains("inputs") && legacy["inputs"].is_object())
    {
        const nlohmann::json& ji = legacy["inputs"];
        const auto getb = [&](const char* k, bool def) {
            return ji.contains(k) && ji[k].is_boolean()
                ? ji[k].get<bool>() : def;
        };
        const auto geti = [&](const char* k, int def) {
            return ji.contains(k) && ji[k].is_number_integer()
                ? ji[k].get<int>() : def;
        };
        doc.inputs.audio        = getb("audio",        doc.inputs.audio);
        doc.inputs.keys         = getb("keys",         doc.inputs.keys);
        doc.inputs.screen       = getb("screen",       doc.inputs.screen);
        doc.inputs.screen_index = geti("screen_index", doc.inputs.screen_index);
        doc.inputs.sens_pct     = geti("sens_pct",     doc.inputs.sens_pct);
        doc.inputs.decay_pct    = geti("decay_pct",    doc.inputs.decay_pct);
    }

    /* Migration never turns live output on. */
    doc.meta.live_on_startup = false;
    return true;
}

} /* namespace studio */
