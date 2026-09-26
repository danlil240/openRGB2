/*---------------------------------------------------------*\
|| BindingResolver.cpp                                       |
||                                                           |
||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "BindingResolver.h"

#include <algorithm>
#include <cctype>

namespace studio
{

static std::string Lower(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return out;
}

static bool NameMatches(const std::string& wanted, const std::string& actual)
{
    if(wanted.empty())
    {
        return true;
    }
    const std::string w = Lower(wanted);
    const std::string a = Lower(actual);
    return a == w || a.find(w) != std::string::npos;
}

ResolvedBinding ResolveBinding(const DeviceBinding& binding,
                               const std::vector<ControllerSnapshot>& controllers)
{
    ResolvedBinding result;

    std::vector<int> candidates;
    for(size_t i = 0; i < controllers.size(); i++)
    {
        const ControllerSnapshot& c = controllers[i];

        if(!binding.serial.empty())
        {
            if(c.serial == binding.serial && NameMatches(binding.controller_name, c.name))
            {
                candidates.push_back((int)i);
            }
            continue;
        }

        if(!NameMatches(binding.controller_name, c.name))
        {
            continue;
        }
        if(binding.device_type >= 0 && c.device_type != binding.device_type)
        {
            continue;
        }
        if(!binding.vendor.empty() && Lower(binding.vendor) != Lower(c.vendor))
        {
            continue;
        }
        candidates.push_back((int)i);
    }

    if(candidates.size() > 1 && !binding.location.empty())
    {
        std::vector<int> narrowed;
        for(int idx : candidates)
        {
            if(controllers[idx].location == binding.location)
            {
                narrowed.push_back(idx);
            }
        }
        if(!narrowed.empty())
        {
            candidates = narrowed;
        }
    }

    if(candidates.empty())
    {
        result.reason = "no controller matches " + binding.controller_name;
        return result;
    }
    if(candidates.size() > 1)
    {
        result.status = BindingStatus::Ambiguous;
        result.reason = std::to_string(candidates.size())
                      + " controllers match " + binding.controller_name
                      + " — identification needed";
        return result;
    }

    const ControllerSnapshot& ctrl = controllers[candidates[0]];
    if(binding.zone_name.empty())
    {
        result.status           = BindingStatus::Resolved;
        result.controller_index = candidates[0];
        result.reason           = "controller matched (no zone required)";
        return result;
    }

    for(size_t z = 0; z < ctrl.zones.size(); z++)
    {
        if(ctrl.zones[z].name == binding.zone_name)
        {
            result.status           = BindingStatus::Resolved;
            result.controller_index = candidates[0];
            result.zone_index       = (int)z;
            if(binding.zone_leds > 0 && ctrl.zones[z].leds_count != binding.zone_leds)
            {
                result.reason = "zone matched but LED count "
                              + std::to_string(ctrl.zones[z].leds_count)
                              + " != expected " + std::to_string(binding.zone_leds);
            }
            return result;
        }
    }

    result.controller_index = candidates[0];
    result.reason           = "controller matched but zone '" + binding.zone_name + "' not found";
    return result;
}

std::vector<ResolvedBinding> ResolveBindings(
    const std::vector<DeviceBinding>& bindings,
    const std::vector<ControllerSnapshot>& controllers)
{
    std::vector<ResolvedBinding> out;
    out.reserve(bindings.size());
    for(const DeviceBinding& b : bindings)
    {
        out.push_back(ResolveBinding(b, controllers));
    }
    return out;
}

} /* namespace studio */
