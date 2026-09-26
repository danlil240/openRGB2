/*---------------------------------------------------------*\
||| SceneResolver.h                                           |
|||                                                           |
|||   The v3 runtime adapter:                                |
|||     compact workspace + device-type library              |
|||       -> expanded SceneDocument                          |
|||       -> renderer / effects / output                     |
|||                                                           |
|||   The SceneDocument stays the internal representation    |
|||   (it is what QML, the effect engine and the output      |
|||   adapter already consume); it is produced on load and   |
|||   NEVER serialized back into studio.json.                |
|||                                                           |
|||   Expansion rules:                                       |
|||   - Every device instance (root and nested child refs)   |
|||     becomes a placement-only Group object whose id is    |
|||     the instance path; root instances take their         |
|||     transform from devices.<id> (type/x/y/z/rx/ry/rz),   |
|||     nested ones from the child entity's local transform. |
|||   - Each local part entity expands to                  |
|||     "<instance path>/<entity id>" parented to its        |
|||     entity-parent chain or the instance group.           |
|||   - A device_settings entry attaches per-instance state: |
|||     zones.<id>.binding / addr_base / verified wire the   |
|||     zone's generated emitters to a physical binding;     |
|||     mirror_of marks every expanded object under the      |
|||     source instance path Linked to the corresponding     |
|||     object under the target path (same type required,    |
|||     no chains) — output ownership, never placement.      |
|||   - World emitter placement is                            |
|||     instance transform × entity-parent chain ×           |
|||     emitter local position — by construction, since      |
|||     SceneGraph composes parent world × local.            |
|||                                                           |
|||   Qt-free; covered by the fast test suite.               |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include "../config/StudioConfig.h"
#include "../presets/PresetRegistry.h"

#include <string>
#include <vector>

namespace studio
{

/* Expand `ws` into the runtime scene. On success `out` receives the
   document (replacing whatever it held) and the function returns
   true; on failure `out` is untouched and `errors` holds
   "<section>.<path>: ..." messages. Non-fatal notes (e.g. a color
   keyed at an entity the type no longer has) go to `warnings` when
   given. Never throws — a rejected workspace must leave the active
   scene alone. */
bool ResolveScene(const StudioDocument& ws, const PresetRegistry& reg,
                  SceneDocument& out,
                  std::vector<std::string>* errors   = nullptr,
                  std::vector<std::string>* warnings = nullptr);

} /* namespace studio */
