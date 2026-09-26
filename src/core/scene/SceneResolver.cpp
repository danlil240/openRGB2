/*---------------------------------------------------------*\
||| SceneResolver.cpp                                         |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "SceneResolver.h"
#include "SceneGraph.h"

#include <map>
#include <set>

namespace studio
{
namespace
{

struct ResolveCtx
{
    const StudioDocument&            ws;
    const PresetRegistry&            reg;
    SceneDocument                    out;
    std::vector<std::string>&        errs;
    /* instance path -> type id (filled during expansion; the mirror
       pass needs it for the same-type rule). */
    std::map<std::string, std::string> inst_types;
    /* Expansion-size accounting — hostile workspaces (many devices
       x fat types) must not allocate unboundedly. `capped` stops
       the expansion once a limit trips. */
    size_t                           emitters = 0;
    bool                             capped   = false;
};

void AddErr(ResolveCtx& c, const std::string& path, const std::string& msg)
{
    c.errs.push_back(path + ": " + msg);
}

const DeviceSettings* SettingsFor(const ResolveCtx& c, const std::string& path)
{
    const auto it = c.ws.device_settings.find(path);
    return it == c.ws.device_settings.end() ? nullptr : &it->second;
}

/* Expansion caps — the v2 scene validator's bounds, now enforced
   as instances unfold. */
void NoteObject(ResolveCtx& c, const std::string& inst_path)
{
    if(c.out.objects.size() > STUDIO_MAX_OBJECTS && !c.capped)
    {
        AddErr(c, inst_path, "expanded object count exceeds cap "
               + std::to_string(STUDIO_MAX_OBJECTS));
        c.capped = true;
    }
}

void NoteEmitters(ResolveCtx& c, const std::string& inst_path,
                  size_t count)
{
    c.emitters += count;
    if(c.emitters > STUDIO_MAX_EMITTERS && !c.capped)
    {
        AddErr(c, inst_path, "expanded emitter count exceeds cap "
               + std::to_string(STUDIO_MAX_EMITTERS));
        c.capped = true;
    }
}

/* Expand one type under an instance path. `group_id` is the object
   entities without an internal parent attach to. */
void ExpandType(ResolveCtx& c, const std::string& inst_path,
                const DevicePreset& p, const std::string& group_id,
                int depth)
{
    if(depth > 32)
    {
        AddErr(c, inst_path, "child-device nesting too deep");
        return;
    }
    const DeviceSettings* settings = SettingsFor(c, inst_path);

    for(const auto& kv : p.entities)
    {
        if(c.capped)
        {
            return;
        }
        const PresetEntity& e   = kv.second;
        const std::string   oid = inst_path + "/" + e.id;
        const std::string   parent = e.parent.empty()
            ? group_id
            : inst_path + "/" + e.parent;

        if(!e.type.empty())
        {
            /* Nested child-device reference — its own instance path
               and group node; the child type's entities expand under
               it (never copied into this type). */
            const DevicePreset* child = c.reg.Find(e.type);
            if(child == nullptr)
            {
                AddErr(c, "devices." + inst_path,
                       "unknown type '" + e.type + "' (entity '"
                       + e.id + "')");
                continue;
            }
            SceneObject g;
            g.id                   = inst_path + "/" + e.id;
            g.label                = e.id;
            g.kind                 = ObjectKind::Group;
            g.parent_id            = parent;
            g.transform.position   = e.position;
            g.transform.rotation_deg = e.rotation_deg;
            if(settings != nullptr)
            {
                g.visible = settings->visible;
            }
            c.out.objects.push_back(g);
            NoteObject(c, inst_path);
            c.inst_types[g.id] = e.type;
            ExpandType(c, g.id, *child, g.id, depth + 1);
            continue;
        }

        SceneObject o;
        o.id                   = oid;
        o.label                = e.id;
        o.parent_id            = parent;
        o.transform.position   = e.position;
        o.transform.rotation_deg = e.rotation_deg;
        o.geometry             = e.geometry;
        o.size_m               = e.size_m;
        if(settings != nullptr)
        {
            o.visible = settings->visible;
        }

        /* Zones on this entity generate emitters and carry the
           physical binding. One object binds at most one zone —
           distinct bindings on one entity can't be represented on a
           SceneObject and are a type-authoring error. */
        std::string bound;
        for(const DeviceZone& z : p.zones)
        {
            if(z.entity != e.id)
            {
                continue;
            }
            int         addr_base = 0;
            std::string zbinding;
            bool        zverified = false;
            if(settings != nullptr)
            {
                const auto zt = settings->zones.find(z.id);
                if(zt != settings->zones.end())
                {
                    addr_base = zt->second.addr_base;
                    zbinding  = zt->second.binding;
                    zverified = zt->second.verified;
                }
            }
            if(!zbinding.empty())
            {
                if(!bound.empty() && bound != zbinding)
                {
                    AddErr(c, "device_settings." + inst_path
                           + ".zones." + z.id,
                           "entity '" + e.id + "' carries zones bound"
                           " to different bindings — one object can"
                           " only bind one zone");
                }
                else
                {
                    bound = zbinding;
                }
            }
            if(z.layout.type == "matrix" && z.layout.dynamic)
            {
                /* Emitters are rebuilt from the bound zone's matrix
                   map at runtime (rebuildMatrixLayouts). */
                o.layout = "matrix_map";
            }
            else
            {
                std::vector<Emitter> em =
                    GenerateZoneEmitters(z, o.id, addr_base);
                /* Address sanity — -1 is the render-only sentinel
                   (Emitter::address); anything lower is a wrapped or
                   invalid index the output push would silently drop. */
                for(const Emitter& e : em)
                {
                    if(e.address < -1)
                    {
                        AddErr(c, "device_settings." + inst_path
                               + ".zones." + z.id,
                               "address " + std::to_string(e.address)
                               + " is invalid (must be >= -1)");
                    }
                }
                /* LED bounds — the v2 workspace contract: every
                   address must be < the bound zone's led count
                   (zone_leds 0 = unchecked). addr_base + led_count
                   (or explicit points.addresses) is validated here,
                   where the generated addresses meet the binding. */
                if(!zbinding.empty())
                {
                    const auto bit = c.ws.bindings.find(zbinding);
                    if(bit != c.ws.bindings.end()
                       && bit->second.zone_leds > 0)
                    {
                        for(const Emitter& e : em)
                        {
                            if(e.address >= (int)bit->second.zone_leds)
                            {
                                AddErr(c, "device_settings." + inst_path
                                       + ".zones." + z.id,
                                       "address "
                                       + std::to_string(e.address)
                                       + " out of range (zone '"
                                       + bit->second.zone_name + "' has "
                                       + std::to_string(bit->second.zone_leds)
                                       + " LEDs)");
                            }
                        }
                    }
                }
                NoteEmitters(c, inst_path, em.size());
                o.emitters.insert(o.emitters.end(), em.begin(), em.end());
            }
            if(zverified)
            {
                o.verified = true;
            }
        }
        o.binding = bound;
        /* Kind: bound or emitter-carrying objects are devices; pure
           bodies are decor. Mirror pass may downgrade to Linked. */
        o.kind = (o.emitters.empty() && o.layout.empty())
            ? ObjectKind::Decor
            : ObjectKind::Device;
        c.out.objects.push_back(o);
        NoteObject(c, inst_path);
    }
}

/* Settings-zone ids must name real zones of the resolved type. */
void ValidateZoneKeys(ResolveCtx& c, const std::string& inst_path,
                      const DevicePreset& p, const DeviceSettings& s)
{
    for(const auto& kv : s.zones)
    {
        bool found = false;
        for(const DeviceZone& z : p.zones)
        {
            if(z.id == kv.first) { found = true; break; }
        }
        if(!found)
        {
            AddErr(c, "device_settings." + inst_path + ".zones."
                   + kv.first, "type '" + p.id + "' has no zone '"
                   + kv.first + "'");
        }
    }
}

} /* anonymous namespace */

bool ResolveScene(const StudioDocument& ws, const PresetRegistry& reg,
                  SceneDocument& out,
                  std::vector<std::string>* errors,
                  std::vector<std::string>* warnings)
{
    std::vector<std::string> errs, warns;
    ResolveCtx c{ ws, reg, SceneDocument{}, errs, {} };

    c.out.version    = 2;
    c.out.name       = ws.meta.name;
    c.out.brightness = ws.brightness;
    c.out.effect     = ws.effect;
    for(const auto& kv : ws.bindings)
    {
        c.out.bindings.push_back(kv.second);
    }
    c.out.object_colors  = ws.object_colors;
    c.out.emitter_colors = ws.emitter_colors;

    /*------------------------------------------------*\
    || Expand every root device instance.              ||
    \*------------------------------------------------*/
    for(const auto& kv : ws.devices)
    {
        if(c.capped)
        {
            break;
        }
        const std::string&    iid = kv.first;
        const DeviceInstance& d   = kv.second;
        const DevicePreset*   p   = reg.Find(d.type);
        if(p == nullptr)
        {
            AddErr(c, "devices." + iid + ".type",
                   "unknown type '" + d.type + "'");
            continue;
        }
        if(!d.parent.empty()
           && ws.devices.find(d.parent) == ws.devices.end())
        {
            AddErr(c, "devices." + iid + ".parent",
                   "unknown instance '" + d.parent + "'");
            continue;
        }

        SceneObject g;
        g.id                   = iid;
        g.label                = iid;
        g.kind                 = ObjectKind::Group;
        g.parent_id            = d.parent;
        g.transform.position   = d.position;
        g.transform.rotation_deg = d.rotation_deg;
        const DeviceSettings* settings = SettingsFor(c, iid);
        if(settings != nullptr)
        {
            g.visible = settings->visible;
        }
        c.out.objects.push_back(g);
        NoteObject(c, iid);
        c.inst_types[iid] = d.type;
        ExpandType(c, iid, *p, iid, 0);
    }

    /* Nested instance paths must exist before settings/mirror checks
       (expansion above already recorded them). */
    for(const auto& kv : ws.device_settings)
    {
        if(c.inst_types.find(kv.first) == c.inst_types.end())
        {
            AddErr(c, "device_settings." + kv.first,
                   "unknown instance path");
            continue;
        }
        ValidateZoneKeys(c, kv.first,
                         *reg.Find(c.inst_types[kv.first]), kv.second);
    }

    /*------------------------------------------------*\
    || Instance-level mirrors: every expanded object   ||
    || under the source path becomes Linked to the      ||
    || matching object under the target path. Both      ||
    || paths must be instances of the SAME type — only  ||
    || then do entity ids correspond 1:1. Placement is  ||
    || untouched: mirror_of is output ownership.        ||
    \*------------------------------------------------*/
    for(const auto& kv : ws.device_settings)
    {
        const std::string&    src = kv.first;
        const DeviceSettings& s   = kv.second;
        if(s.mirror_of.empty())
        {
            continue;
        }
        const std::string& dst = s.mirror_of;
        const std::string  sp  = "device_settings." + src + ".mirror_of";

        const auto st = c.inst_types.find(src);
        const auto dt = c.inst_types.find(dst);
        if(st == c.inst_types.end())
        {
            /* already reported as unknown instance path */
            continue;
        }
        if(dt == c.inst_types.end())
        {
            AddErr(c, sp, "'" + dst + "' is not a device instance");
            continue;
        }
        if(st->second != dt->second)
        {
            AddErr(c, sp, "'" + src + "' (" + st->second + ") and '"
                   + dst + "' (" + dt->second
                   + ") are different types — a mirror needs the same"
                   " definition so its entities correspond 1:1");
            continue;
        }
        const auto ds = ws.device_settings.find(dst);
        if(ds != ws.device_settings.end() && !ds->second.mirror_of.empty())
        {
            AddErr(c, sp, "'" + dst + "' is itself mirrored — mirror"
                   " chains are not allowed");
            continue;
        }

        for(SceneObject& o : c.out.objects)
        {
            /* Nested instance group nodes stay placement-only —
               their descendant part objects are linked instead. */
            if(o.kind == ObjectKind::Group
               || o.id.size() <= src.size()
               || o.id.compare(0, src.size(), src) != 0
               || o.id[src.size()] != '/')
            {
                continue;
            }
            const std::string target = dst + o.id.substr(src.size());
            if(FindObject(c.out, target) == nullptr)
            {
                AddErr(c, sp, "target object '" + target
                       + "' missing (types differ)");
                continue;
            }
            o.kind      = ObjectKind::Linked;
            o.mirror_of = target;
            o.binding.clear();
            o.layout.clear();
            o.emitters.clear();
            o.verified  = false;
        }
    }

    /*------------------------------------------------*\
    || Whole-document structural validation — the same ||
    || rules every scene must satisfy.                 ||
    \*------------------------------------------------*/
    if(errs.empty())
    {
        std::vector<std::string> gerrs;
        if(!ValidateSceneGraph(c.out, &gerrs))
        {
            for(std::string& e : gerrs)
            {
                errs.push_back("scene: " + e);
            }
        }
    }

    /*------------------------------------------------*\
    || Non-fatal notes: colors keyed at ids the        ||
    || resolved scene no longer has (stale after a     ||
    || type edit). Kept in the document — the data     ||
    || isn't lost if the type regains the entity.      ||
    \*------------------------------------------------*/
    std::set<std::string> ids;
    for(const SceneObject& o : c.out.objects)
    {
        ids.insert(o.id);
    }
    for(const auto& kv : ws.object_colors)
    {
        if(ids.find(kv.first) == ids.end())
        {
            warns.push_back("colors.objects." + kv.first
                            + ": no such resolved object");
        }
    }
    for(const auto& kv : ws.emitter_colors)
    {
        if(ids.find(kv.first) == ids.end())
        {
            warns.push_back("colors.emitters." + kv.first
                            + ": no such resolved object");
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
    out = c.out;
    return true;
}

} /* namespace studio */
