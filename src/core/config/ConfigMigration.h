/*---------------------------------------------------------*\
||| ConfigMigration.h                                         |
|||                                                           |
|||   One-time migrations into the v3 workspace:           ||
|||   - legacy host-settings blob                           ||
|||     { "scene": <SceneJson v1/v2>, "inputs": {...} }     ||
|||   - expanded v2 studio.json on disk                     ||
|||                                                           |
|||   Both reduce to MigrateExpandedScene: every expanded   ||
|||   object becomes a compact instance; structurally       ||
|||   equivalent local definitions (identical geometry,     ||
|||   size, emitter layout and addresses modulo the         ||
|||   address base — placement and physical identity        ||
|||   excluded) deduplicate into one external type;         ||
|||   differences survive as separate variants.             ||
|||                                                           |
|||   Qt-free so the fast test suite covers it.              |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include "StudioConfig.h"
#include "../presets/DevicePreset.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace studio
{

/* True when the host-settings blob actually carries a saved scene
   (non-empty object with an object "scene" member). */
bool HasLegacySettings(const nlohmann::json& legacy);

/* Extract a compact v3 workspace + its type files from an already
   parsed expanded SceneDocument (v1 scenes must first pass through
   SceneJson's v1->v2 upgrade). Preserves: instance ids (= object
   ids), parent hierarchy, world placement, binding identity,
   zone verification, mirror output behavior (instance-level
   mirror_of), colors (re-keyed to <instance>/<entity>) and the
   effect state. On failure `doc`/`types` are untouched and
   `errors` describes the fields; `warnings` collects non-fatal
   notes (e.g. a color keyed at a placement group). Never throws. */
bool MigrateExpandedScene(const SceneDocument& scene,
                          StudioDocument& doc,
                          std::vector<DevicePreset>& types,
                          std::vector<std::string>* errors   = nullptr,
                          std::vector<std::string>* warnings = nullptr);

/* Migrate a legacy settings blob into a workspace candidate + its
   extracted types. Runs the same whole-document validation as a
   file load — on failure `doc`/`types` are untouched and `errors`
   describes the fields. Never enables live output. Never throws. */
bool MigrateLegacySettings(const nlohmann::json& legacy,
                           StudioDocument& doc,
                           std::vector<DevicePreset>* types,
                           std::vector<std::string>* errors = nullptr);

} /* namespace studio */
