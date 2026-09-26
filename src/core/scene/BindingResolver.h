/*---------------------------------------------------------*\
|| BindingResolver.h                                         |
||                                                           |
||   Resolves DeviceBindings (persistent identities) to     |
||   live controller snapshots. Snapshots are a neutral     |
||   copy of what RGBControllerInterface exposes, so this   |
||   module stays Qt-free and unit-testable.                |
||                                                           |
||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include "SceneTypes.h"

namespace studio
{

struct ZoneSnapshot
{
    std::string  name;
    unsigned int leds_count  = 0;
    unsigned int start_idx   = 0;
    int          type        = 0;
    bool         has_per_led = false;   /* active mode has per-LED color */
};

struct ControllerSnapshot
{
    std::string              name;
    std::string              vendor;
    std::string              serial;
    std::string              location;
    int                      device_type = -1;
    std::vector<ZoneSnapshot> zones;
};

enum class BindingStatus
{
    Resolved,
    Unresolved,
    Ambiguous,
};

struct ResolvedBinding
{
    BindingStatus status           = BindingStatus::Unresolved;
    int           controller_index = -1;    /* snapshot list position —   */
    int           zone_index       = -1;    /* runtime only, never saved  */
    std::string   reason;
};

/*---------------------------------------------------------*\
|| Match rules (in order):                                 |
||   1. binding.serial non-empty  -> serial + name must    |
||      match; serial is the disambiguator.                |
||   2. Otherwise candidates match on controller_name      |
||      (exact, else substring) AND device_type if set.    |
||   3. >1 candidate -> filter by non-empty location;      |
||      still >1 -> Ambiguous (never silently pick one).   |
||   4. Zone matched by zone_name; zone_leds mismatch      |
||      resolves but reports the count difference.         |
\*---------------------------------------------------------*/
ResolvedBinding ResolveBinding(const DeviceBinding& binding,
                               const std::vector<ControllerSnapshot>& controllers);

std::vector<ResolvedBinding> ResolveBindings(
    const std::vector<DeviceBinding>& bindings,
    const std::vector<ControllerSnapshot>& controllers);

} /* namespace studio */
