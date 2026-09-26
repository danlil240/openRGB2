/*---------------------------------------------------------*\
||| DevicePreset.cpp                                          |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "DevicePreset.h"
#include "../scene/EmitterLayout.h"
#include "../scene/JsonFields.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

namespace studio
{

bool IsPresetId(const std::string& s)
{
    if(s.empty())
    {
        return false;
    }
    for(char c : s)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                     || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if(!ok)
        {
            return false;
        }
    }
    return true;
}

bool IsPresetAssetPath(const std::string& s)
{
    if(s.empty() || s.front() == '/')
    {
        return false;
    }
    bool segment_start = true;
    for(char c : s)
    {
        /* Forward slashes only — '\' would be a Windows-only path
           and ':' a drive letter or URI scheme. Control chars are
           never legitimate in a file name. */
        if((unsigned char)c < 0x20 || c == 0x7f
           || c == '\\' || c == ':')
        {
            return false;
        }
        if(c == '/')
        {
            segment_start = true;
            continue;
        }
        if(c == '.' && segment_start)
        {
            /* Reject "."/".." (and dotfile) segments — a preset
               must never reach outside the workspace assets dir.
               Embedded dots like "fan-a.glb" stay legal. */
            return false;
        }
        segment_start = false;
    }
    /* Trailing slash (empty last segment) is not a file. */
    return s.back() != '/';
}

/*---------------------------------------------------------*\
||| Serialization                                           |
\*---------------------------------------------------------*/
static nlohmann::json Vec3Array(const Vec3& v)
{
    return nlohmann::json::array({ v.x, v.y, v.z });
}

static nlohmann::json LayoutToJson(const ZoneLayout& l)
{
    nlohmann::json j;
    j["type"] = l.type;
    if(l.type == "ring")
    {
        j["radius_m"]        = l.radius_m;
        j["start_angle_deg"] = l.start_angle_deg;
        j["face_y_m"]        = l.face_y_m;
        j["reverse"]         = l.reverse;
    }
    else if(l.type == "strip")
    {
        j["spacing_m"] = l.spacing_m;
        j["origin"]    = Vec3Array(l.origin);
    }
    else if(l.type == "matrix")
    {
        j["dynamic"]   = l.dynamic;
        j["rows"]      = l.rows;
        j["cols"]      = l.cols;
        j["pitch_x_m"] = l.pitch_x_m;
        j["pitch_z_m"] = l.pitch_z_m;
        j["origin"]    = Vec3Array(l.origin);
        j["empty"]     = l.empty_cell;
        nlohmann::json map = nlohmann::json::array();
        for(unsigned int c : l.map)
        {
            map.push_back(c);
        }
        j["map"] = map;
    }
    else /* points */
    {
        nlohmann::json pts = nlohmann::json::array();
        for(const Vec3& v : l.points)
        {
            pts.push_back(Vec3Array(v));
        }
        j["points"] = pts;
        if(!l.addresses.empty())
        {
            nlohmann::json addr = nlohmann::json::array();
            for(int a : l.addresses)
            {
                addr.push_back(a);
            }
            j["addresses"] = addr;
        }
    }
    return j;
}

nlohmann::json ToJson(const DevicePreset& p)
{
    nlohmann::json j;
    j["$schema"]        = "../../schemas/device.schema.json";
    j["schema_version"] = p.schema_version;
    j["id"]             = p.id;
    j["name"]           = p.name;
    j["category"]       = p.category;

    nlohmann::json entities = nlohmann::json::object();
    for(const auto& kv : p.entities)
    {
        const PresetEntity& e = kv.second;
        nlohmann::json je;
        if(!e.type.empty())
        {
            je["type"] = e.type;
        }
        else
        {
            if(!e.geometry.empty())
            {
                je["geometry"] = e.geometry;
            }
            je["size_m"] = Vec3Array(e.size_m);
        }
        je["x"]  = e.position.x;
        je["y"]  = e.position.y;
        je["z"]  = e.position.z;
        je["rx"] = e.rotation_deg.x;
        je["ry"] = e.rotation_deg.y;
        je["rz"] = e.rotation_deg.z;
        if(!e.parent.empty())
        {
            je["parent"] = e.parent;
        }
        if(!e.zone.empty())
        {
            je["zone"] = e.zone;
        }
        if(e.appearance.is_object() && !e.appearance.empty())
        {
            je["appearance"] = e.appearance;
        }
        entities[kv.first] = je;
    }
    j["entities"] = entities;

    nlohmann::json zones = nlohmann::json::array();
    for(const DeviceZone& z : p.zones)
    {
        zones.push_back({
            {"id",        z.id},
            {"entity",    z.entity},
            {"led_count", z.led_count},
            {"layout",    LayoutToJson(z.layout)},
        });
    }
    j["zones"] = zones;

    if(!p.binding_hints.empty())
    {
        nlohmann::json hints = nlohmann::json::array();
        for(const BindingHint& h : p.binding_hints)
        {
            nlohmann::json jh;
            if(!h.controller_name.empty())
            {
                jh["controller_name"] = h.controller_name;
            }
            if(!h.vendor.empty())
            {
                jh["vendor"] = h.vendor;
            }
            if(!h.zone_name.empty())
            {
                jh["zone_name"] = h.zone_name;
            }
            hints.push_back(jh);
        }
        j["binding_hints"] = hints;
    }
    return j;
}

/*---------------------------------------------------------*\
||| Parsing + validation                                    |
\*---------------------------------------------------------*/
namespace
{

/* Read an [x,y,z] array; tolerates {x,y,z} objects too. */
static Vec3 Vec3Field(const nlohmann::json& j, const Vec3& def,
                      const std::string& path, std::vector<std::string>& errs)
{
    if(j.is_null())
    {
        return def;
    }
    Vec3 v = def;
    if(j.is_array())
    {
        if(j.size() != 3)
        {
            AddErr(errs, path, "expected [x,y,z] array");
            return def;
        }
        for(int i = 0; i < 3; i++)
        {
            if(!j[i].is_number())
            {
                AddErr(errs, path, "expected [x,y,z] numbers");
                return def;
            }
        }
        v.x = (float)j[0].get<double>();
        v.y = (float)j[1].get<double>();
        v.z = (float)j[2].get<double>();
    }
    else if(j.is_object())
    {
        v.x = (float)FieldNum(j, "x", def.x, path, errs);
        v.y = (float)FieldNum(j, "y", def.y, path, errs);
        v.z = (float)FieldNum(j, "z", def.z, path, errs);
    }
    else
    {
        AddErr(errs, path, "expected [x,y,z] array");
        return def;
    }
    if(!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
    {
        AddErr(errs, path, "expected finite numbers");
        return def;
    }
    return v;
}

static ZoneLayout LayoutFromJson(const nlohmann::json& j,
                                 const std::string& path,
                                 std::vector<std::string>& errs)
{
    ZoneLayout l;
    if(!j.is_object())
    {
        AddErr(errs, path, "expected object");
        return l;
    }
    l.type = FieldStr(j, "type", std::string(), path, errs);
    if(l.type == "ring")
    {
        l.radius_m        = (float)FieldNum(j, "radius_m", 0.0, path, errs);
        l.start_angle_deg = (float)FieldNum(j, "start_angle_deg", 0.0, path, errs);
        l.face_y_m        = (float)FieldNum(j, "face_y_m", 0.0, path, errs);
        l.reverse         = FieldBool(j, "reverse", false, path, errs);
        if(l.radius_m <= 0.0f)
        {
            AddErr(errs, path + ".radius_m", "expected number > 0");
        }
    }
    else if(l.type == "strip")
    {
        l.spacing_m = (float)FieldNum(j, "spacing_m", 0.0, path, errs);
        l.origin    = Vec3Field(j.value("origin", nlohmann::json()),
                                Vec3{}, path + ".origin", errs);
        if(l.spacing_m <= 0.0f)
        {
            AddErr(errs, path + ".spacing_m", "expected number > 0");
        }
    }
    else if(l.type == "matrix")
    {
        l.dynamic    = FieldBool(j, "dynamic", false, path, errs);
        l.origin     = Vec3Field(j.value("origin", nlohmann::json()),
                                 Vec3{}, path + ".origin", errs);
        l.pitch_x_m  = (float)FieldNum(j, "pitch_x_m", 0.019, path, errs);
        l.pitch_z_m  = (float)FieldNum(j, "pitch_z_m", 0.019, path, errs);
        const long long rows = FieldInt(j, "rows", 0, path, errs);
        const long long cols = FieldInt(j, "cols", 0, path, errs);
        if(rows < 0 || cols < 0 || rows > 256 || cols > 256)
        {
            AddErr(errs, path, "rows/cols expected in 0..256");
        }
        else
        {
            l.rows = (unsigned int)rows;
            l.cols = (unsigned int)cols;
        }
        l.empty_cell = FieldU32(j, "empty", 0xFFFFFFFFu, path, errs);
        if(j.contains("map"))
        {
            if(!j["map"].is_array())
            {
                AddErr(errs, path + ".map", "expected array");
            }
            else
            {
                int mi = 0;
                for(const nlohmann::json& c : j["map"])
                {
                    const std::string mp = path + ".map["
                                           + std::to_string(mi++) + "]";
                    if(!IsInt(c))
                    {
                        AddErr(errs, mp, "expected integer");
                        continue;
                    }
                    /* Full-width read — get<unsigned int> wraps
                       negatives and >32-bit values silently. */
                    const long long v = c.get<long long>();
                    if(v < 0 || v > 4294967295ll)
                    {
                        AddErr(errs, mp, "expected unsigned integer"
                                       " in 0..4294967295");
                        continue;
                    }
                    /* Emitter addresses are stored int — a mapped
                       value >= 2^31 wraps negative at generation.
                       Only the empty sentinel may reach the top
                       of the u32 range. */
                    if(v >= 2147483648ll
                       && v != (long long)l.empty_cell)
                    {
                        AddErr(errs, mp, "LED index must be"
                                       " < 2147483648 (or the empty"
                                       " sentinel)");
                        continue;
                    }
                    l.map.push_back((unsigned int)v);
                }
            }
        }
        if(!l.dynamic)
        {
            if(l.rows == 0 || l.cols == 0 || l.map.empty())
            {
                AddErr(errs, path, "static matrix needs rows/cols/map"
                                   " (or \"dynamic\": true)");
            }
            /* The generator indexes map[row*cols+col] — a short map
               must be a file error, not an out-of-bounds read; a
               long map is a stale hand edit whose tail would be
               silently dropped. */
            else if(l.map.size() != (size_t)l.rows * l.cols)
            {
                AddErr(errs, path + ".map",
                       "has " + std::to_string(l.map.size())
                       + " entries, needs rows*cols ("
                       + std::to_string((size_t)l.rows * l.cols) + ")");
            }
        }
    }
    else if(l.type == "points")
    {
        if(j.contains("points") && j["points"].is_array())
        {
            int i = 0;
            for(const nlohmann::json& jp : j["points"])
            {
                l.points.push_back(Vec3Field(jp, Vec3{},
                    path + ".points[" + std::to_string(i++) + "]", errs));
            }
        }
        else
        {
            AddErr(errs, path + ".points", "expected array");
        }
        if(j.contains("addresses"))
        {
            if(!j["addresses"].is_array())
            {
                AddErr(errs, path + ".addresses", "expected array");
            }
            else
            {
                for(const nlohmann::json& ja : j["addresses"])
                {
                    if(!IsInt(ja))
                    {
                        AddErr(errs, path + ".addresses", "expected integers");
                        break;
                    }
                    /* -1 is the render-only sentinel (Emitter::
                       address); anything lower is a wrapped or
                       invalid index. */
                    const long long a = ja.get<long long>();
                    if(a < -1 || a > 2147483647ll)
                    {
                        AddErr(errs, path + ".addresses",
                               "expected addresses >= -1"
                               " (-1 = render-only)");
                        break;
                    }
                    l.addresses.push_back((int)a);
                }
                if(!l.addresses.empty() && l.addresses.size() != l.points.size())
                {
                    AddErr(errs, path + ".addresses",
                           "count must match points");
                }
                /* Two emitters on one hardware LED: the later write
                   silently shadows the first at push time. -1 pairs
                   may repeat — render-only emitters share the
                   sentinel legitimately. */
                std::set<int> seen;
                for(int a : l.addresses)
                {
                    if(a >= 0 && !seen.insert(a).second)
                    {
                        AddErr(errs, path + ".addresses",
                               "duplicate LED address "
                               + std::to_string(a));
                        break;
                    }
                }
            }
        }
        if(l.points.empty())
        {
            AddErr(errs, path + ".points", "expected at least one point");
        }
        if(l.points.size() > PRESET_MAX_POINTS)
        {
            AddErr(errs, path + ".points", "count exceeds cap "
                   + std::to_string(PRESET_MAX_POINTS));
        }
    }
    else
    {
        AddErr(errs, path + ".type",
               "expected ring|strip|matrix|points, got '" + l.type + "'");
    }
    return l;
}

/* Sane hint text: printable, no control characters. Hints are
   human-facing hardware names — they get compared case-
   insensitively at match time, so the file just has to carry
   clean text. */
static bool IsHintText(const std::string& s)
{
    for(char c : s)
    {
        if((unsigned char)c < 0x20 || c == 0x7f)
        {
            return false;
        }
    }
    return true;
}

static void BindingHintsFromJson(const nlohmann::json& j,
                                 const std::string& path,
                                 std::vector<BindingHint>& out,
                                 std::vector<std::string>& errs)
{
    if(!j.is_array())
    {
        AddErr(errs, path, "expected array");
        return;
    }
    static const char* const known[] = {
        "controller_name", "vendor", "zone_name",
    };
    int i = 0;
    for(const nlohmann::json& jh : j)
    {
        const std::string hp = path + "[" + std::to_string(i++) + "]";
        if(!jh.is_object())
        {
            AddErr(errs, hp, "expected object");
            continue;
        }
        bool bad = false;
        for(auto it = jh.begin(); it != jh.end(); ++it)
        {
            bool is_known = false;
            for(const char* k : known)
            {
                if(it.key() == k) { is_known = true; break; }
            }
            if(!is_known)
            {
                AddErr(errs, hp + "." + it.key(),
                       "unknown binding_hints key (typo?)");
                bad = true;
            }
        }
        if(bad)
        {
            continue;
        }
        BindingHint h;
        h.controller_name = FieldStr(jh, "controller_name",
                                     std::string(), hp, errs);
        h.vendor          = FieldStr(jh, "vendor",
                                     std::string(), hp, errs);
        h.zone_name       = FieldStr(jh, "zone_name",
                                     std::string(), hp, errs);
        if(!IsHintText(h.controller_name) || !IsHintText(h.vendor)
           || !IsHintText(h.zone_name))
        {
            AddErr(errs, hp, "expected printable text");
            continue;
        }
        out.push_back(h);
    }
}

} /* anonymous namespace */

bool DevicePresetFromJson(const nlohmann::json& j, DevicePreset& p,
                          std::vector<std::string>* errors)
{
    /* `errors` is an out-param — stale entries from an earlier call
       must not poison this parse. */
    if(errors)
    {
        errors->clear();
    }
    std::vector<std::string> local;
    std::vector<std::string>& errs = errors ? *errors : local;

    if(!j.is_object())
    {
        AddErr(errs, "root", "expected object");
        return false;
    }

    /* schema_version — newer preset files are rejected, not
       silently downgraded. */
    if(!j.contains("schema_version") || !IsInt(j["schema_version"]))
    {
        AddErr(errs, "schema_version", "expected integer");
        return false;
    }
    const long long version = j["schema_version"].get<long long>();
    if(version != DEVICE_PRESET_SCHEMA_VERSION)
    {
        AddErr(errs, "schema_version", "expected "
               + std::to_string(DEVICE_PRESET_SCHEMA_VERSION)
               + ", got " + std::to_string(version));
        return false;
    }

    DevicePreset out;
    out.id       = FieldStr(j, "id", std::string(), "root", errs);
    out.name     = FieldStr(j, "name", std::string(), "root", errs);
    out.category = FieldStr(j, "category", std::string(), "root", errs);
    if(!IsPresetId(out.id))
    {
        AddErr(errs, "id", "expected non-empty [A-Za-z0-9_-] id");
    }

    /*------------------------------------------------*\
    || entities                                        ||
    \*------------------------------------------------*/
    if(j.contains("entities"))
    {
        if(!j["entities"].is_object())
        {
            AddErr(errs, "entities", "expected object");
        }
        else
        {
            for(auto it = j["entities"].begin(); it != j["entities"].end(); ++it)
            {
                const std::string eid  = it.key();
                const nlohmann::json& je = it.value();
                const std::string path = "entities." + eid;
                if(!IsPresetId(eid))
                {
                    AddErr(errs, path, "bad entity id '" + eid + "'");
                    continue;
                }
                if(!je.is_object())
                {
                    AddErr(errs, path, "expected object");
                    continue;
                }
                PresetEntity e;
                e.id           = eid;
                e.type         = FieldStr(je, "type", std::string(), path, errs);
                e.position.x   = (float)FieldNum(je, "x", 0.0, path, errs);
                e.position.y   = (float)FieldNum(je, "y", 0.0, path, errs);
                e.position.z   = (float)FieldNum(je, "z", 0.0, path, errs);
                e.rotation_deg.x = (float)FieldNum(je, "rx", 0.0, path, errs);
                e.rotation_deg.y = (float)FieldNum(je, "ry", 0.0, path, errs);
                e.rotation_deg.z = (float)FieldNum(je, "rz", 0.0, path, errs);
                e.parent       = FieldStr(je, "parent", std::string(), path, errs);
                if(!e.parent.empty() && !IsPresetId(e.parent))
                {
                    AddErr(errs, path + ".parent", "bad entity id '"
                           + e.parent + "'");
                }
                if(!e.type.empty())
                {
                    /* Child device ref — transform + parent only. */
                    if(!IsPresetId(e.type))
                    {
                        AddErr(errs, path + ".type", "bad type id '"
                               + e.type + "'");
                    }
                    if(je.contains("geometry") || je.contains("size_m")
                       || je.contains("zone") || je.contains("appearance"))
                    {
                        AddErr(errs, path,
                               "child type ref must not carry"
                               " geometry/size_m/zone/appearance");
                    }
                }
                else
                {
                    e.geometry = FieldStr(je, "geometry", std::string(),
                                          path, errs);
                    e.size_m   = Vec3Field(je.value("size_m", nlohmann::json()),
                                           Vec3{}, path + ".size_m", errs);
                    if(e.size_m.x < 0.0f || e.size_m.y < 0.0f
                       || e.size_m.z < 0.0f)
                    {
                        AddErr(errs, path + ".size_m",
                               "expected components >= 0");
                    }
                    e.zone = FieldStr(je, "zone", std::string(), path, errs);
                    if(je.contains("appearance"))
                    {
                        if(!je["appearance"].is_object())
                        {
                            AddErr(errs, path + ".appearance",
                                   "expected object");
                        }
                        else
                        {
                            e.appearance = je["appearance"];
                            /* model/image are asset references —
                               relative portable paths only. A
                               preset that ships "../etc/passwd" or
                               "C:\..." as a model path is rejected
                               here, at parse, not at render. */
                            for(const char* key : { "model", "image" })
                            {
                                const nlohmann::json& a =
                                    e.appearance.value(key,
                                                       nlohmann::json());
                                if(a.is_string()
                                   && !IsPresetAssetPath(
                                       a.get<std::string>()))
                                {
                                    AddErr(errs, path + ".appearance."
                                           + key,
                                           "expected a relative"
                                           " asset path (no drive"
                                           " letters, no '..')");
                                }
                            }
                        }
                    }
                }
                out.entities[eid] = e;
            }
            if(out.entities.size() > PRESET_MAX_ENTITIES)
            {
                AddErr(errs, "entities", "count exceeds cap "
                       + std::to_string(PRESET_MAX_ENTITIES));
            }
        }
    }

    /*------------------------------------------------*\
    || zones                                           ||
    \*------------------------------------------------*/
    if(j.contains("zones"))
    {
        if(!j["zones"].is_array())
        {
            AddErr(errs, "zones", "expected array");
        }
        else
        {
            std::set<std::string> zids;
            int i = 0;
            for(const nlohmann::json& jz : j["zones"])
            {
                const std::string path = "zones[" + std::to_string(i++) + "]";
                if(!jz.is_object())
                {
                    AddErr(errs, path, "expected object");
                    continue;
                }
                DeviceZone z;
                z.id        = FieldStr(jz, "id", std::string(), path, errs);
                z.entity    = FieldStr(jz, "entity", std::string(), path, errs);
                const long long leds = FieldInt(jz, "led_count", 0, path, errs);
                if(leds < 0 || leds > (long long)PRESET_MAX_POINTS)
                {
                    AddErr(errs, path + ".led_count",
                           "expected 0.." + std::to_string(PRESET_MAX_POINTS));
                }
                else
                {
                    z.led_count = (unsigned int)leds;
                }
                z.layout = LayoutFromJson(jz.value("layout", nlohmann::json()),
                                          path + ".layout", errs);
                /* For a points layout the emitter count comes from
                   points, not led_count — when both are declared they
                   must agree (a mismatch is a stale hand edit that
                   misreports the zone's size). */
                if(z.layout.type == "points" && z.led_count != 0
                   && z.led_count != z.layout.points.size())
                {
                    AddErr(errs, path + ".led_count",
                           "declares " + std::to_string(z.led_count)
                           + " LEDs but layout.points has "
                           + std::to_string(z.layout.points.size()));
                }
                /* led_count 0 is reserved for layouts whose emitter
                   count is hardware-decided (dynamic matrix) or
                   point-derived (points). A ring/strip with zero
                   LEDs is a dead zone — almost certainly a file
                   that lost its count in a hand edit. */
                if((z.layout.type == "ring" || z.layout.type == "strip")
                   && z.led_count == 0)
                {
                    AddErr(errs, path + ".led_count",
                           "ring/strip zones need led_count > 0"
                           " (0 is reserved for dynamic"
                           " matrix/points layouts)");
                }
                /* A static matrix's emitter count is its non-empty
                   cell count — led_count must agree, like points. */
                if(z.layout.type == "matrix" && !z.layout.dynamic
                   && z.layout.rows > 0 && z.layout.cols > 0
                   && z.layout.map.size()
                      == (size_t)z.layout.rows * z.layout.cols)
                {
                    unsigned int live = 0;
                    for(unsigned int c : z.layout.map)
                    {
                        if(c != z.layout.empty_cell)
                        {
                            ++live;
                        }
                    }
                    if(z.led_count != live)
                    {
                        AddErr(errs, path + ".led_count",
                               "declares " + std::to_string(z.led_count)
                               + " LEDs but the static matrix map"
                                 " has " + std::to_string(live)
                               + " live cells");
                    }
                }
                if(!IsPresetId(z.id))
                {
                    AddErr(errs, path + ".id", "bad zone id '" + z.id + "'");
                }
                else if(!zids.insert(z.id).second)
                {
                    AddErr(errs, path + ".id", "duplicate zone id '" + z.id + "'");
                }
                out.zones.push_back(z);
            }
            if(out.zones.size() > PRESET_MAX_ZONES)
            {
                AddErr(errs, "zones", "count exceeds cap "
                       + std::to_string(PRESET_MAX_ZONES));
            }
        }
    }

    /*------------------------------------------------*\
    || binding_hints — informational hardware hints    ||
    \*------------------------------------------------*/
    if(j.contains("binding_hints"))
    {
        BindingHintsFromJson(j["binding_hints"], "binding_hints",
                             out.binding_hints, errs);
    }

    /*------------------------------------------------*\
    || Cross references                                ||
    \*------------------------------------------------*/
    for(const DeviceZone& z : out.zones)
    {
        const auto eit = out.entities.find(z.entity);
        if(eit == out.entities.end())
        {
            AddErr(errs, "zones." + z.id + ".entity",
                   "unknown entity '" + z.entity + "'");
        }
        else
        {
            const PresetEntity& e = eit->second;
            if(!e.type.empty())
            {
                AddErr(errs, "zones." + z.id + ".entity",
                       "'" + z.entity + "' is a child type ref,"
                       " not a local entity");
            }
            else if(e.zone != z.id)
            {
                AddErr(errs, "zones." + z.id + ".entity",
                       "entity '" + z.entity + "' does not name zone '"
                       + z.id + "'");
            }
        }
    }
    for(const auto& kv : out.entities)
    {
        const PresetEntity& e = kv.second;
        if(!e.zone.empty())
        {
            bool found = false;
            for(const DeviceZone& z : out.zones)
            {
                if(z.id == e.zone) { found = true; break; }
            }
            if(!found)
            {
                AddErr(errs, "entities." + e.id + ".zone",
                       "unknown zone '" + e.zone + "'");
            }
        }
        if(!e.parent.empty() && out.entities.find(e.parent) == out.entities.end())
        {
            AddErr(errs, "entities." + e.id + ".parent",
                   "unknown entity '" + e.parent + "'");
        }
    }

    /* Entity parent cycles. */
    for(const auto& kv : out.entities)
    {
        std::set<std::string> seen;
        std::string cur = kv.first;
        while(!cur.empty())
        {
            if(!seen.insert(cur).second)
            {
                AddErr(errs, "entities." + kv.first + ".parent",
                       "parent cycle through '" + cur + "'");
                break;
            }
            const auto it = out.entities.find(cur);
            cur = (it == out.entities.end()) ? std::string() : it->second.parent;
        }
    }

    if(!errs.empty())
    {
        return false;
    }
    p = out;
    return true;
}

bool DevicePresetFromJsonFile(const std::string& path, DevicePreset& p,
                              std::vector<std::string>* errors)
{
    std::ifstream f(path, std::ios::binary);
    if(!f)
    {
        if(errors)
        {
            errors->push_back(path + ": cannot open");
        }
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    nlohmann::json j;
    try
    {
        j = nlohmann::json::parse(ss.str());
    }
    catch(const std::exception& e)
    {
        if(errors)
        {
            errors->push_back(path + ": invalid JSON (" + e.what() + ")");
        }
        return false;
    }
    return DevicePresetFromJson(j, p, errors);
}

/*---------------------------------------------------------*\
||| Emitter generation                                      |
\*---------------------------------------------------------*/
std::vector<Emitter> GenerateZoneEmitters(const DeviceZone& z,
                                        const std::string& group,
                                        int addr_base)
{
    const ZoneLayout& l = z.layout;
    if(l.type == "ring")
    {
        return layout::Ring(z.led_count, l.radius_m, l.start_angle_deg,
                            l.reverse, group, addr_base, l.face_y_m);
    }
    if(l.type == "strip")
    {
        return layout::Strip(z.led_count, l.spacing_m, l.origin,
                             group, addr_base);
    }
    if(l.type == "matrix")
    {
        if(l.dynamic)
        {
            /* Resolved at runtime from the bound zone's matrix map
               (SceneBridge::rebuildMatrixLayouts). */
            return {};
        }
        return layout::KeyboardMatrix(l.rows, l.cols, l.map,
                                      l.empty_cell, l.pitch_x_m,
                                      l.pitch_z_m, l.origin, group);
    }
    /* points */
    std::vector<Emitter> out;
    out.reserve(l.points.size());
    for(size_t i = 0; i < l.points.size(); i++)
    {
        Emitter e;
        e.local_pos = l.points[i];
        e.group     = group;
        e.address   = (i < l.addresses.size()) ? l.addresses[i]
                                             : addr_base + (int)i;
        out.push_back(e);
    }
    return out;
}

bool ZoneLayoutToPoints(DeviceZone& z)
{
    if(z.layout.type == "points")
    {
        return false;   /* already explicit */
    }
    /* group "" + addr_base 0 bakes the type-local layout; the
       generated addresses pin the LED order (ring reverse swaps
       address<->position, not geometry). */
    const std::vector<Emitter> em = GenerateZoneEmitters(z, "", 0);
    if(em.empty())
    {
        return false;   /* dynamic matrix / empty zone: nothing to bake */
    }
    ZoneLayout baked;
    baked.type = "points";
    baked.points.reserve(em.size());
    baked.addresses.reserve(em.size());
    for(const Emitter& e : em)
    {
        baked.points.push_back(e.local_pos);
        baked.addresses.push_back(e.address);
    }
    z.layout    = baked;
    z.led_count = (unsigned int)baked.points.size();
    return true;
}

std::vector<std::string> PresetDependencies(const DevicePreset& p)
{
    std::vector<std::string> deps;
    for(const auto& kv : p.entities)
    {
        if(!kv.second.type.empty())
        {
            deps.push_back(kv.second.type);
        }
    }
    return deps;
}

std::vector<std::string> PresetAssetRefs(const DevicePreset& p)
{
    std::vector<std::string> refs;
    for(const auto& kv : p.entities)
    {
        const nlohmann::json& a = kv.second.appearance;
        if(!a.is_object())
        {
            continue;
        }
        for(const char* key : { "model", "image" })
        {
            const nlohmann::json& v = a.value(key, nlohmann::json());
            if(v.is_string())
            {
                const std::string s = v.get<std::string>();
                if(std::find(refs.begin(), refs.end(), s) == refs.end())
                {
                    refs.push_back(s);
                }
            }
        }
    }
    return refs;
}

} /* namespace studio */
