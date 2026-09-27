/*---------------------------------------------------------*\
||| LayoutEditModel.cpp                                       ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "LayoutEditModel.h"

#include <algorithm>
#include <cmath>

namespace openrgb2
{
namespace ui
{
namespace
{

std::string JStr(const nlohmann::json& j, const char* key)
{
    const auto it = j.find(key);
    return (it != j.end() && it->is_string())
               ? it->get<std::string>()
               : std::string();
}

double JNum(const nlohmann::json& j, const char* key, double dflt)
{
    const auto it = j.find(key);
    return (it != j.end() && it->is_number())
               ? it->get<double>()
               : dflt;
}

bool JBool(const nlohmann::json& j, const char* key, bool dflt)
{
    const auto it = j.find(key);
    return (it != j.end() && it->is_boolean())
               ? it->get<bool>()
               : dflt;
}

double VecAxis(const nlohmann::json& j, const char* axis, double dflt)
{
    if(!j.is_object())
    {
        return dflt;
    }
    return JNum(j, axis, dflt);
}

nlohmann::json VecObj(double x, double y, double z)
{
    return nlohmann::json::object(
        {{"x", x}, {"y", y}, {"z", z}});
}

/* Mirrors studio::NameMatches / SceneEditController's display
   hint — case-insensitive equality or containment; empty
   wanted matches everything. Display only: the engine
   resolves authoritatively. */
bool NameMatches(const std::string& wanted, const std::string& actual)
{
    if(wanted.empty())
    {
        return true;
    }
    std::string w = wanted, a = actual;
    std::transform(w.begin(), w.end(), w.begin(), ::tolower);
    std::transform(a.begin(), a.end(), a.begin(), ::tolower);
    return a == w || a.find(w) != std::string::npos;
}

} /* anonymous namespace */

void LayoutEditModel::LoadApplied(const nlohmann::json& doc_or_null)
{
    applied_ = doc_or_null.is_object() ? doc_or_null
                                       : nlohmann::json(nullptr);
}

void LayoutEditModel::SetSnapshots(
    const std::vector<nlohmann::json>& snaps)
{
    snapshots_ = snaps;
}

const nlohmann::json* LayoutEditModel::FindObject(
    const std::string& id) const
{
    if(!applied_.is_object())
    {
        return nullptr;
    }
    const auto it = applied_.find("objects");
    if(it == applied_.end() || !it->is_array())
    {
        return nullptr;
    }
    for(const nlohmann::json& o : *it)
    {
        if(o.is_object() && JStr(o, "id") == id)
        {
            return &o;
        }
    }
    return nullptr;
}

const nlohmann::json* LayoutEditModel::FindBinding(
    const std::string& id) const
{
    if(!applied_.is_object())
    {
        return nullptr;
    }
    const auto it = applied_.find("bindings");
    if(it == applied_.end() || !it->is_array())
    {
        return nullptr;
    }
    for(const nlohmann::json& b : *it)
    {
        if(b.is_object() && JStr(b, "id") == id)
        {
            return &b;
        }
    }
    return nullptr;
}

int LayoutEditModel::MatchSnapshots(const nlohmann::json& binding,
                                    const nlohmann::json** first) const
{
    int matches = 0;
    if(first)
    {
        *first = nullptr;
    }
    const std::string cname  = JStr(binding, "controller_name");
    const std::string vendor = JStr(binding, "vendor");
    const std::string serial = JStr(binding, "serial");
    const std::string loc    = JStr(binding, "location");
    for(const nlohmann::json& s : snapshots_)
    {
        const nlohmann::json key =
            s.value("key", nlohmann::json::object());
        /* controller_name matches the enumerated name (or the
           key's composite device identity — bindings authored
           against either resolve the same display hint). */
        if(!NameMatches(cname, JStr(s, "name")) &&
           !NameMatches(cname, JStr(key, "device")))
        {
            continue;
        }
        if(!vendor.empty() &&
           !NameMatches(vendor, JStr(s, "vendor")))
        {
            continue;
        }
        if(!serial.empty() && serial != JStr(key, "serial"))
        {
            continue;
        }
        if(!loc.empty() && loc != JStr(key, "location"))
        {
            continue;
        }
        if(matches == 0 && first)
        {
            *first = &s;
        }
        ++matches;
    }
    return matches;
}

std::vector<LayoutItem> LayoutEditModel::Items() const
{
    std::vector<LayoutItem> out;
    if(!applied_.is_object())
    {
        return out;
    }
    const auto it = applied_.find("objects");
    if(it == applied_.end() || !it->is_array())
    {
        return out;
    }
    for(const nlohmann::json& o : *it)
    {
        if(!o.is_object())
        {
            continue;
        }
        LayoutItem it2;
        it2.id        = JStr(o, "id");
        it2.label     = JStr(o, "label");
        if(it2.label.empty())
        {
            it2.label = it2.id;
        }
        it2.kind      = JStr(o, "kind");
        it2.binding   = JStr(o, "binding");
        it2.mirror_of = JStr(o, "mirror_of");
        it2.geometry  = JStr(o, "geometry");
        it2.verified  = JBool(o, "verified", false);
        it2.visible   = JBool(o, "visible", true);
        it2.x = VecAxis(o.value("position", nlohmann::json()), "x", 0);
        it2.y = VecAxis(o.value("position", nlohmann::json()), "y", 0);
        it2.z = VecAxis(o.value("position", nlohmann::json()), "z", 0);
        it2.rx = VecAxis(o.value("rotation", nlohmann::json()), "x", 0);
        it2.ry = VecAxis(o.value("rotation", nlohmann::json()), "y", 0);
        it2.rz = VecAxis(o.value("rotation", nlohmann::json()), "z", 0);

        /* Footprint: authored size_m when positive, else the
           emitter bounding box (real data — linked copies
           carry no emitters, so they keep authored size), else
           a default body size for unlabeled nodes. */
        double min_x = 0, max_x = 0, min_z = 0, max_z = 0;
        int    addressed = 0;
        {
            const auto eit = o.find("emitters");
            if(eit != o.end() && eit->is_array())
            {
                it2.emitter_count = (int)eit->size();
                bool first = true;
                for(const nlohmann::json& e : *eit)
                {
                    const double ex =
                        VecAxis(e.value("pos", nlohmann::json()),
                                "x", 0);
                    const double ez =
                        VecAxis(e.value("pos", nlohmann::json()),
                                "z", 0);
                    if(first)
                    {
                        min_x = max_x = ex;
                        min_z = max_z = ez;
                        first = false;
                    }
                    else
                    {
                        min_x = std::min(min_x, ex);
                        max_x = std::max(max_x, ex);
                        min_z = std::min(min_z, ez);
                        max_z = std::max(max_z, ez);
                    }
                    if(JNum(e, "address", -1) >= 0)
                    {
                        ++addressed;
                    }
                }
            }
        }
        it2.addressed_count = addressed;
        const double sw = VecAxis(o.value("size_m",
                                          nlohmann::json()), "x", 0);
        const double sd = VecAxis(o.value("size_m",
                                          nlohmann::json()), "z", 0);
        it2.w = sw > 0 ? sw
                : (it2.emitter_count > 0
                       ? std::max(0.02, max_x - min_x + 0.02)
                       : 0.10);
        it2.d = sd > 0 ? sd
                : (it2.emitter_count > 0
                       ? std::max(0.02, max_z - min_z + 0.02)
                       : 0.10);

        /* Binding + snapshot join (display hint only). */
        if(it2.kind == "linked")
        {
            it2.state = "linked";
        }
        else if(it2.kind == "device")
        {
            const nlohmann::json* b =
                it2.binding.empty() ? nullptr
                                    : FindBinding(it2.binding);
            if(b == nullptr)
            {
                it2.state = "unbound";
            }
            else
            {
                it2.controller_name = JStr(*b, "controller_name");
                it2.zone_name       = JStr(*b, "zone_name");
                it2.zone_leds       = (uint32_t)std::max<int64_t>(
                    0, (int64_t)JNum(*b, "zone_leds", 0));
                const nlohmann::json* snap = nullptr;
                const int n = MatchSnapshots(*b, &snap);
                if(n == 0)
                {
                    it2.state = "missing";
                }
                else if(n > 1)
                {
                    it2.state = "ambiguous";
                }
                else
                {
                    it2.state     = "resolved";
                    it2.connected =
                        JStr(*snap, "connection") == "connected";
                    if(it2.zone_leds == 0)
                    {
                        /* Enumerated zone count fills the
                           honest bound a declared-0 binding
                           can't give. */
                        const auto zit = snap->find("zones");
                        if(zit != snap->end() && zit->is_array())
                        {
                            for(const nlohmann::json& z : *zit)
                            {
                                if(JStr(z, "name") ==
                                   it2.zone_name)
                                {
                                    it2.zone_leds = (uint32_t)
                                        std::max<int64_t>(0,
                                            (int64_t)JNum(z,
                                                "led_count", 0));
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
        out.push_back(std::move(it2));
    }
    return out;
}

std::vector<std::string> LayoutEditModel::LinkSources(
    const std::string& exclude_id) const
{
    std::vector<std::string> out;
    if(!applied_.is_object())
    {
        return out;
    }
    const auto it = applied_.find("objects");
    if(it == applied_.end() || !it->is_array())
    {
        return out;
    }
    for(const nlohmann::json& o : *it)
    {
        const std::string id = JStr(o, "id");
        if(JStr(o, "kind") == "device" && id != exclude_id)
        {
            out.push_back(id);
        }
    }
    return out;
}

nlohmann::json LayoutEditModel::Bindings() const
{
    if(!applied_.is_object())
    {
        return nlohmann::json::array();
    }
    const auto it = applied_.find("bindings");
    return (it != applied_.end() && it->is_array())
               ? *it
               : nlohmann::json::array();
}

nlohmann::json LayoutEditModel::ObjectEmitters(
    const std::string& id) const
{
    nlohmann::json out = nlohmann::json::array();
    const nlohmann::json* o = FindObject(id);
    if(o == nullptr)
    {
        return out;
    }
    const auto eit = o->find("emitters");
    if(eit == o->end() || !eit->is_array())
    {
        return out;
    }
    int i = 0;
    for(const nlohmann::json& e : *eit)
    {
        out.push_back(nlohmann::json::object({
            { "index",   i++ },
            { "address", (int)JNum(e, "address", -1) },
            { "group",   JStr(e, "group") },
        }));
    }
    return out;
}

uint32_t LayoutEditModel::BoundZoneLeds(const std::string& id) const
{
    const nlohmann::json* o = FindObject(id);
    if(o == nullptr)
    {
        return 0;
    }
    const std::string bid = JStr(*o, "binding");
    const nlohmann::json* b =
        bid.empty() ? nullptr : FindBinding(bid);
    if(b == nullptr)
    {
        return 0;
    }
    const int64_t declared = (int64_t)JNum(*b, "zone_leds", 0);
    if(declared > 0)
    {
        return (uint32_t)declared;
    }
    const nlohmann::json* snap = nullptr;
    if(MatchSnapshots(*b, &snap) == 1 && snap != nullptr)
    {
        const std::string zone = JStr(*b, "zone_name");
        const auto zit = snap->find("zones");
        if(zit != snap->end() && zit->is_array())
        {
            for(const nlohmann::json& z : *zit)
            {
                if(JStr(z, "name") == zone)
                {
                    const int64_t n =
                        (int64_t)JNum(z, "led_count", 0);
                    return n > 0 ? (uint32_t)n : 0;
                }
            }
        }
    }
    return 0;
}

/*---------------------------------------------------------*\
||| Op builders                                              ||
\*---------------------------------------------------------*/

bool LayoutEditModel::BuildSetPosition(const std::string& id,
                                       double x, double y, double z,
                                       LayoutOp& out,
                                       std::string& err) const
{
    const nlohmann::json* o = FindObject(id);
    if(o == nullptr)
    {
        err = "unknown scene object '" + id + "'";
        return false;
    }
    if(!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
    {
        err = "position must be finite meters";
        return false;
    }
    out.label = "move " + (o->contains("label") &&
                           (*o)["label"].is_string()
                               ? JStr(*o, "label") : id);
    out.body  = nlohmann::json::object({
        { "verb",      "set_position" },
        { "object_id", id             },
        { "position",  VecObj(x, y, z) },
    });
    return true;
}

bool LayoutEditModel::BuildSetOrientation(const std::string& id,
                                          double rx, double ry,
                                          double rz, LayoutOp& out,
                                          std::string& err) const
{
    const nlohmann::json* o = FindObject(id);
    if(o == nullptr)
    {
        err = "unknown scene object '" + id + "'";
        return false;
    }
    if(!std::isfinite(rx) || !std::isfinite(ry) || !std::isfinite(rz))
    {
        err = "rotation must be finite degrees";
        return false;
    }
    out.label = "rotate " + id;
    out.body  = nlohmann::json::object({
        { "verb",      "set_orientation" },
        { "object_id", id                },
        { "rotation",  VecObj(rx, ry, rz) },
    });
    return true;
}

bool LayoutEditModel::BuildSetMirror(const std::string& id,
                                     const std::string& target,
                                     LayoutOp& out,
                                     std::string& err) const
{
    const nlohmann::json* o = FindObject(id);
    if(o == nullptr)
    {
        err = "unknown scene object '" + id + "'";
        return false;
    }
    const std::string kind = JStr(*o, "kind");
    if(target.empty())
    {
        if(kind != "linked")
        {
            err = "object '" + id + "' is not linked";
            return false;
        }
        out.label = "unlink " + id;
    }
    else
    {
        if(kind != "device" && kind != "linked")
        {
            err = "only device objects can be mirrored";
            return false;
        }
        if(target == id)
        {
            err = "an object cannot mirror itself";
            return false;
        }
        const nlohmann::json* t = FindObject(target);
        if(t == nullptr || JStr(*t, "kind") != "device")
        {
            err = "mirror target '" + target +
                  "' is not a device object";
            return false;
        }
        out.label = "link " + id + " → " + target;
    }
    out.body = nlohmann::json::object({
        { "verb",      "set_mirror" },
        { "object_id", id           },
        { "mirror_of", target       },
    });
    return true;
}

bool LayoutEditModel::BuildSetBinding(const std::string& id,
                                      const std::string& binding_id,
                                      LayoutOp& out,
                                      std::string& err) const
{
    const nlohmann::json* o = FindObject(id);
    if(o == nullptr)
    {
        err = "unknown scene object '" + id + "'";
        return false;
    }
    if(JStr(*o, "kind") != "device")
    {
        err = "only device objects take bindings";
        return false;
    }
    if(!binding_id.empty() && FindBinding(binding_id) == nullptr)
    {
        err = "unknown binding '" + binding_id + "'";
        return false;
    }
    out.label = binding_id.empty() ? "unbind " + id
                                   : "bind " + id;
    out.body  = nlohmann::json::object({
        { "verb",      "set_binding" },
        { "object_id", id            },
        { "binding",   binding_id    },
    });
    return true;
}

bool LayoutEditModel::BuildSetBindingFromController(
    const std::string& id, const std::string& new_binding_id,
    const nlohmann::json& controller_key,
    LayoutOp& out, std::string& err) const
{
    const nlohmann::json* o = FindObject(id);
    if(o == nullptr)
    {
        err = "unknown scene object '" + id + "'";
        return false;
    }
    if(JStr(*o, "kind") != "device")
    {
        err = "only device objects take bindings";
        return false;
    }
    if(new_binding_id.empty())
    {
        err = "a new binding needs an id";
        return false;
    }
    if(FindBinding(new_binding_id) != nullptr)
    {
        err = "binding '" + new_binding_id + "' already exists";
        return false;
    }
    if(!controller_key.is_object() ||
       JStr(controller_key, "device").empty())
    {
        err = "a new binding needs a controller DeviceKey";
        return false;
    }
    out.label = "bind " + id + " → " +
                JStr(controller_key, "device");
    out.body  = nlohmann::json::object({
        { "verb",       "set_binding"    },
        { "object_id",  id               },
        { "binding",    new_binding_id   },
        { "controller", controller_key   },
    });
    return true;
}

bool LayoutEditModel::BuildCalibrateLed(const std::string& id,
                                        int emitter_index,
                                        int address,
                                        LayoutOp& out,
                                        std::string& err) const
{
    const nlohmann::json* o = FindObject(id);
    if(o == nullptr)
    {
        err = "unknown scene object '" + id + "'";
        return false;
    }
    if(JStr(*o, "kind") != "device")
    {
        err = "only device objects have LED addresses";
        return false;
    }
    size_t emitters = 0;
    {
        const auto eit = o->find("emitters");
        if(eit != o->end() && eit->is_array())
        {
            emitters = eit->size();
        }
    }
    if(emitter_index < 0 || (size_t)emitter_index >= emitters)
    {
        err = "emitter_index out of range (object has " +
              std::to_string(emitters) + " emitters)";
        return false;
    }
    if(address < -1)
    {
        err = "address must be >= -1 (-1 = render-only)";
        return false;
    }
    if(address >= 0)
    {
        const uint32_t bound = BoundZoneLeds(id);
        if(bound == 0)
        {
            err = "object '" + id +
                  "' has no bound zone LED count";
            return false;
        }
        if((uint32_t)address >= bound)
        {
            err = "address " + std::to_string(address) +
                  " out of range (zone has " +
                  std::to_string(bound) + " LEDs)";
            return false;
        }
    }
    out.label = "calibrate " + id + "[" +
                std::to_string(emitter_index) + "]";
    out.body  = nlohmann::json::object({
        { "verb",          "calibrate_led" },
        { "object_id",     id              },
        { "emitter_index", emitter_index   },
        { "address",       address         },
    });
    return true;
}

/*---------------------------------------------------------*\
||| History                                                  ||
\*---------------------------------------------------------*/

void LayoutEditModel::PushUndo(const std::string& label)
{
    undo_.push_back(HistoryEntry{ label, applied_ });
    while(undo_.size() > kHistoryBound)
    {
        undo_.pop_front();   /* bound: oldest edit drops out */
    }
    redo_.clear();           /* a new edit is a new timeline */
}

void LayoutEditModel::DropUndo()
{
    if(!undo_.empty())
    {
        undo_.pop_back();
    }
}

void LayoutEditModel::PushRedo(const std::string& label,
                               const nlohmann::json& doc)
{
    redo_.push_back(HistoryEntry{ label, doc });
    while(redo_.size() > kHistoryBound)
    {
        redo_.pop_front();
    }
}

void LayoutEditModel::PushUndoEntry(const std::string& label,
                                    const nlohmann::json& doc)
{
    undo_.push_back(HistoryEntry{ label, doc });
    while(undo_.size() > kHistoryBound)
    {
        undo_.pop_front();
    }
    /* Symmetric with PushUndo — but this is a REDO replay, so
       the redo stack is left alone (the entry being replayed
       is popped separately). */
}

const LayoutEditModel::HistoryEntry* LayoutEditModel::PeekUndo() const
{
    return undo_.empty() ? nullptr : &undo_.back();
}

const LayoutEditModel::HistoryEntry* LayoutEditModel::PeekRedo() const
{
    return redo_.empty() ? nullptr : &redo_.back();
}

void LayoutEditModel::PopUndo()
{
    if(!undo_.empty())
    {
        undo_.pop_back();
    }
}

void LayoutEditModel::PopRedo()
{
    if(!redo_.empty())
    {
        redo_.pop_back();
    }
}

void LayoutEditModel::ClearHistory()
{
    undo_.clear();
    redo_.clear();
}

} /* namespace ui */
} /* namespace openrgb2 */
