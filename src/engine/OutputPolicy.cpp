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

/*---------------------------------------------------------*\
||| Per-key executor                                        ||
\*---------------------------------------------------------*/

std::vector<OutputPolicyOutcome> RunOutputPolicy(
    const std::vector<DeviceSnapshot>&    targets,
    OutputPhase                           phase,
    const OutputPolicy&                   policy,
    const std::map<DeviceKey, PriorMode>& priors,
    const OutputPolicyDeps&               deps)
{
    std::vector<OutputPolicyOutcome> out(targets.size());
    for(size_t i = 0; i < targets.size(); ++i)
    {
        out[i].key    = targets[i].key;
        out[i].policy = phase == OutputPhase::Exit
            ? policy.ExitFor(targets[i].key)
            : policy.IdleFor(targets[i].key);
    }

    /* 1. The power gate: a write would wake a sleeping display
          or cross a suspend boundary — the upkeep's own rule. */
    if(deps.output_paused && deps.output_paused())
    {
        for(OutputPolicyOutcome& o : out)
        {
            o.outcome = "skipped_power_gate";
        }
        return out;
    }

    const long long idle_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deps.lane_idle_timeout).count();
    std::vector<size_t> owned_black, hold_keys, black_keys,
                        restore_keys;
    std::vector<const PriorMode*> restore_modes;

    /* 2. Classify. */
    for(size_t i = 0; i < targets.size(); ++i)
    {
        const DeviceSnapshot& t = targets[i];
        OutputPolicyOutcome&  o = out[i];
        if(deps.calibration_owns && deps.calibration_owns(t.key))
        {
            o.outcome = "skipped_calibration";
            continue;
        }
        if(deps.playback_owns && deps.playback_owns(t.key))
        {
            if(o.policy == OutputAction::Black)
            {
                owned_black.push_back(i);
            }
            else if(o.policy == OutputAction::Restore)
            {
                o.outcome = "loop_kept";
                o.detail  = "the uploaded loop is the hardware mode";
            }
            else
            {
                o.outcome = "held";
            }
            continue;
        }
        if(o.policy == OutputAction::Hold)
        {
            o.outcome = "held";
            hold_keys.push_back(i);
            continue;
        }
        if(o.policy == OutputAction::Restore)
        {
            const auto pit = priors.find(t.key);
            if(IsWirelessGroupController(t))
            {
                o.outcome = "black_fallback";
                o.detail  = "wireless group: restoring a hardware "
                            "mode is not verified there";
            }
            else if(pit == priors.end())
            {
                o.outcome = "black_fallback";
                o.detail  = "no recorded hardware mode (the engine "
                            "never switched it this run)";
            }
            else if(IsDirectModeName(pit->second.name))
            {
                o.outcome = "black_fallback";
                o.detail  = "recorded mode '" + pit->second.name +
                            "' was left by an earlier run";
            }
            else
            {
                restore_keys.push_back(i);
                restore_modes.push_back(&pit->second);
                continue;
            }
        }
        else
        {
            o.outcome = "black";
        }
        black_keys.push_back(i);
    }

    /* 3. The loop owner's write (stop-black) — once. It also
          queues black on the wired peers' lanes, so a hold key
          drops that frame if it is still unsent. */
    if(!owned_black.empty())
    {
        std::string err;
        const bool ok = deps.playback_stop_black
            ? deps.playback_stop_black(err) : false;
        if(!ok && err.empty())
        {
            err = "playback stop-black unavailable";
        }
        for(size_t i : owned_black)
        {
            out[i].outcome = ok ? "loop_stopped" : "failed";
            out[i].detail  = ok ? "" : err;
        }
        if(ok && deps.drop_pending)
        {
            for(size_t i : hold_keys)
            {
                deps.drop_pending(targets[i].key);
            }
        }
    }

    /* 4. Queue every black frame first: the lane workers then
          deliver in parallel while we wait below. A pending
          live frame is dropped so it cannot follow the black. */
    std::vector<size_t> queued;
    for(size_t i : black_keys)
    {
        if(deps.drop_pending)
        {
            deps.drop_pending(targets[i].key);
        }
        std::string err;
        const bool ok = deps.submit_black
            ? deps.submit_black(targets[i], err) : false;
        if(!ok)
        {
            out[i].outcome = "failed";
            out[i].detail  = err.empty() ? "black write unavailable"
                                         : err;
            continue;
        }
        queued.push_back(i);
    }
    for(size_t i : queued)
    {
        if(deps.wait_idle &&
           !deps.wait_idle(targets[i].key, deps.lane_idle_timeout))
        {
            out[i].detail += (out[i].detail.empty() ? "" : "; ") +
                std::string("not confirmed sent within ") +
                std::to_string(idle_ms) + " ms";
        }
    }

    /* 5. Restore: the lane goes quiet first (the mode change is
          serialized behind the last live send), then UPDATEMODE
          straight to the transport. */
    for(size_t n = 0; n < restore_keys.size(); ++n)
    {
        const size_t     i = restore_keys[n];
        const PriorMode& m = *restore_modes[n];
        if(deps.drop_pending)
        {
            deps.drop_pending(targets[i].key);
        }
        std::string note;
        if(deps.wait_idle &&
           !deps.wait_idle(targets[i].key, deps.lane_idle_timeout))
        {
            note = "lane not idle within " + std::to_string(idle_ms) +
                   " ms";
        }
        std::string err;
        const bool ok = deps.select_mode
            ? deps.select_mode(targets[i].key, m, err) : false;
        if(ok)
        {
            out[i].outcome = "restored";
            out[i].detail  = note.empty() ? m.name
                                          : m.name + "; " + note;
        }
        else
        {
            out[i].outcome = "failed";
            out[i].detail  = err.empty() ? "mode select unavailable"
                                         : err;
        }
    }
    return out;
}

nlohmann::json OutputPolicyOutcomesToJson(
    const std::vector<OutputPolicyOutcome>& outcomes)
{
    nlohmann::json arr = nlohmann::json::array();
    for(const OutputPolicyOutcome& o : outcomes)
    {
        arr.push_back(nlohmann::json::object({
            { "key", nlohmann::json::object({
                { "device",   o.key.device   },
                { "serial",   o.key.serial   },
                { "location", o.key.location },
            }) },
            { "policy",  OutputActionName(o.policy) },
            { "outcome", o.outcome                  },
            { "detail",  o.detail                   },
        }));
    }
    return arr;
}

} /* namespace openrgb2 */
