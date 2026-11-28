/*---------------------------------------------------------*\
||| OutputPolicy.cpp — see OutputPolicy.h (audit E-12b).     ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "OutputPolicy.h"

#include "DeviceMapper.h"   /* IsWirelessGroupController */

#include <algorithm>
#include <cctype>

namespace openrgb2
{

/*---------------------------------------------------------*\
||| Names                                                   ||
\*---------------------------------------------------------*/

const char* OutputActionName(OutputAction a)
{
    switch(a)
    {
    case OutputAction::Hold:    return "hold";
    case OutputAction::Black:   return "black";
    case OutputAction::Restore: return "restore";
    }
    return "hold";
}

bool ParseOutputAction(const std::string& s, OutputAction& out)
{
    if(s == "hold")    { out = OutputAction::Hold;    return true; }
    if(s == "black")   { out = OutputAction::Black;   return true; }
    if(s == "restore") { out = OutputAction::Restore; return true; }
    return false;
}

/*---------------------------------------------------------*\
||| Matching + resolution                                   ||
\*---------------------------------------------------------*/

bool OutputPolicyMatch::Empty() const
{
    return device.empty() && location_prefix.empty() &&
           serial.empty();
}

bool OutputPolicyMatch::Matches(const DeviceKey& key) const
{
    if(Empty())
    {
        return false;
    }
    if(!device.empty() && key.device != device)
    {
        return false;
    }
    if(!location_prefix.empty() &&
       key.location.rfind(location_prefix, 0) != 0)
    {
        return false;
    }
    if(!serial.empty() && key.serial != serial)
    {
        return false;
    }
    return true;
}

OutputAction OutputPolicy::ExitFor(const DeviceKey& key) const
{
    for(const OutputPolicyRule& r : per_controller)
    {
        if(r.on_exit && r.match.Matches(key))
        {
            return *r.on_exit;
        }
    }
    return on_exit;
}

OutputAction OutputPolicy::IdleFor(const DeviceKey& key) const
{
    for(const OutputPolicyRule& r : per_controller)
    {
        if(r.on_idle && r.match.Matches(key))
        {
            return *r.on_idle;
        }
    }
    return on_idle;
}

bool OutputPolicy::IsDefault() const
{
    return on_exit == OutputAction::Hold &&
           on_idle == OutputAction::Hold &&
           per_controller.empty();
}

bool operator==(const OutputPolicyMatch& a, const OutputPolicyMatch& b)
{
    return a.device == b.device &&
           a.location_prefix == b.location_prefix &&
           a.serial == b.serial;
}

bool operator==(const OutputPolicyRule& a, const OutputPolicyRule& b)
{
    return a.match == b.match && a.on_exit == b.on_exit &&
           a.on_idle == b.on_idle;
}

bool operator==(const OutputPolicy& a, const OutputPolicy& b)
{
    return a.on_exit == b.on_exit && a.on_idle == b.on_idle &&
           a.per_controller == b.per_controller;
}

/*---------------------------------------------------------*\
||| JSON                                                    ||
\*---------------------------------------------------------*/

nlohmann::json OutputPolicyToJson(const OutputPolicy& p)
{
    nlohmann::json rules = nlohmann::json::array();
    for(const OutputPolicyRule& r : p.per_controller)
    {
        nlohmann::json m = nlohmann::json::object();
        if(!r.match.device.empty())
        {
            m["device"] = r.match.device;
        }
        if(!r.match.location_prefix.empty())
        {
            m["location_prefix"] = r.match.location_prefix;
        }
        if(!r.match.serial.empty())
        {
            m["serial"] = r.match.serial;
        }
        nlohmann::json rj = nlohmann::json::object();
        rj["match"] = std::move(m);
        if(r.on_exit)
        {
            rj["on_exit"] = OutputActionName(*r.on_exit);
        }
        if(r.on_idle)
        {
            rj["on_idle"] = OutputActionName(*r.on_idle);
        }
        rules.push_back(std::move(rj));
    }
    return nlohmann::json::object({
        { "on_exit",        OutputActionName(p.on_exit) },
        { "on_idle",        OutputActionName(p.on_idle) },
        { "per_controller", std::move(rules)            },
    });
}

namespace
{

bool ReadAction(const nlohmann::json& obj, const char* field,
                const std::string& path,
                std::optional<OutputAction>& out, std::string& err)
{
    const auto it = obj.find(field);
    if(it == obj.end() || it->is_null())
    {
        return true;                     /* absent: keep default */
    }
    OutputAction a = OutputAction::Hold;
    if(!it->is_string() || !ParseOutputAction(it->get<std::string>(), a))
    {
        err = path + field +
              ": expected \"hold\", \"black\" or \"restore\"";
        return false;
    }
    out = a;
    return true;
}

bool ReadMatchField(const nlohmann::json& m, const char* field,
                    const std::string& path, std::string& out,
                    std::string& err)
{
    const auto it = m.find(field);
    if(it == m.end() || it->is_null())
    {
        return true;
    }
    if(!it->is_string())
    {
        err = path + "." + field + ": expected string";
        return false;
    }
    out = it->get<std::string>();
    return true;
}

} /* anonymous namespace */

bool OutputPolicyFromJson(const nlohmann::json& j, OutputPolicy& out,
                          std::string& err)
{
    if(!j.is_object())
    {
        err = "output_policy: expected object";
        return false;
    }
    OutputPolicy cand;
    std::optional<OutputAction> a;
    if(!ReadAction(j, "on_exit", "", a, err))
    {
        return false;
    }
    if(a)
    {
        cand.on_exit = *a;
    }
    a.reset();
    if(!ReadAction(j, "on_idle", "", a, err))
    {
        return false;
    }
    if(a)
    {
        cand.on_idle = *a;
    }

    const auto rit = j.find("per_controller");
    if(rit != j.end() && !rit->is_null())
    {
        if(!rit->is_array())
        {
            err = "per_controller: expected array";
            return false;
        }
        if(rit->size() > kMaxOutputPolicyRules)
        {
            err = "per_controller: more than " +
                  std::to_string(kMaxOutputPolicyRules) + " rules";
            return false;
        }
        size_t i = 0;
        for(const nlohmann::json& rj : *rit)
        {
            const std::string path =
                "per_controller[" + std::to_string(i++) + "]";
            if(!rj.is_object())
            {
                err = path + ": expected object";
                return false;
            }
            OutputPolicyRule rule;
            const auto mit = rj.find("match");
            if(mit == rj.end() || !mit->is_object())
            {
                err = path + ".match: expected object";
                return false;
            }
            const std::string mpath = path + ".match";
            if(!ReadMatchField(*mit, "device", mpath,
                               rule.match.device, err) ||
               !ReadMatchField(*mit, "location_prefix", mpath,
                               rule.match.location_prefix, err) ||
               !ReadMatchField(*mit, "serial", mpath,
                               rule.match.serial, err))
            {
                return false;
            }
            if(rule.match.Empty())
            {
                err = mpath + ": needs device, location_prefix or "
                              "serial";
                return false;
            }
            if(!ReadAction(rj, "on_exit", path + ".", rule.on_exit,
                           err) ||
               !ReadAction(rj, "on_idle", path + ".", rule.on_idle,
                           err))
            {
                return false;
            }
            cand.per_controller.push_back(std::move(rule));
        }
    }
    out = std::move(cand);
    return true;
}

bool IsDirectModeName(const std::string& name)
{
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return lower == "direct" || lower == "custom";
}

} /* namespace openrgb2 */
