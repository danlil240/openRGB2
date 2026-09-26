/*---------------------------------------------------------*\
||| DefaultDesk.h                                             |
|||                                                           |
|||   Builds the default desk scene from the verified        |
|||   hardware inventory (docs/hardware-inventory.md) and    |
|||   zone imports (configs/zone-imports/). Placement is a   |
|||   reasonable guess — the plan requires user confirmation |
|||   of desk dimensions and component positions.            |
|||                                                           |
|||   Two surfaces:                                          |
|||   - BuildDefaultWorkspace() produces the v3 compact      |
|||     workspace referencing the packaged device-type ids   |
|||     (presets/devices/*.device.json — the authoritative   |
|||     library; DefaultDevicePresets() is only the minimal  |
|||     C++ fallback set for BuildFallbackWorkspace()).      |
|||   - BuildDefaultDesk() keeps emitting the expanded       |
|||     SceneDocument directly — the v2 shape migration      |
|||     tests still exercise.                                |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include "SceneTypes.h"
#include "../config/StudioConfig.h"
#include "../presets/DevicePreset.h"

#include <vector>

namespace studio
{

/* Expanded v2 scene — retained for migration tests. */
SceneDocument BuildDefaultDesk();

/* Minimal C++ fallback type set — desk + group only. The bundled
   presets/devices/*.device.json files are the AUTHORITATIVE type
   library; SceneBridge loads them from the qrc and uses this set
   only when no packaged file is readable. */
std::vector<DevicePreset> DefaultDevicePresets();

/* The v3 default desk: compact placements referencing the
   packaged type ids. Resolving it must produce the same scene
   BuildDefaultDesk() produces (the parity test). */
StudioDocument BuildDefaultWorkspace();

/* The recoverable desk used when no packaged type file is
   readable — resolves on DefaultDevicePresets() alone. */
StudioDocument BuildFallbackWorkspace();

} /* namespace studio */
