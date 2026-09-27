/*---------------------------------------------------------*\
||| SceneLoader.h                                             ||
|||                                                           ||
|||   The ONE file->SceneDocument path for the engine      ||
|||   (Task 3.8 — "--scene workspace-doc resolution"):     ||
|||   scene loading resolves through the profile/          ||
|||   workspace layer, not only raw expanded JSON.         ||
|||                                                           ||
|||   Accepted forms, in detection order:                  ||
|||   - any document with an object "scene" member: the    ||
|||     legacy host-settings blob {"scene":..., "inputs":  ||
|||     ...} AND the on-disk v1/v2 expanded workspace      ||
|||     {"schema_version":<3, "scene":{...},               ||
|||     "definitions":...} — StudioConfig::FromJson        ||
|||     hard-rejects both, so ConfigMigration::            ||
|||     MigrateLegacySettings runs FIRST, then the same    ||
|||     workspace->scene resolve                          ||
|||   - compact workspace studio.json: "schema_version"    ||
|||     present -> StudioConfig::FromJson + SceneResolver  ||
|||     through the device-type registry                   ||
|||   - expanded SceneDocument: "version" 1/2 ->           ||
|||     SceneJson::FromJson (the core schema; v1 upgrades  ||
|||     in place like the file store did)                  ||
|||                                                           ||
|||   Migration rule (plan section 3.4): none of these     ||
|||   forms carries a playback selection — a migrated or   ||
|||   resolved scene has NO per-scene playback entry and   ||
|||   therefore defaults to LiveEffects. Old profiles are  ||
|||   preserved as Live effects, never silently converted  ||
|||   to uploaded playback.                                ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "presets/PresetRegistry.h"
#include "scene/SceneTypes.h"

#include <memory>
#include <string>
#include <vector>

namespace openrgb2
{

/* Load `path` into a runtime SceneDocument.
   - `registry` supplies the device types a compact workspace
     expands against (PresetRegistry::LoadDirectory); expanded
     and legacy-expanded documents don't consult it — a
     legacy blob's extracted types are layered over a COPY so
     the caller's registry is never mutated.
   - On success `out` receives the document and the function
     returns true; `warnings`, when given, collects non-fatal
     notes (migration notes, unknown fields).
   - On failure `out` is untouched and `err` carries the
     field messages (never a blank or guessed scene). */
bool LoadSceneDocument(const std::string& path,
                       const studio::PresetRegistry& registry,
                       std::shared_ptr<studio::SceneDocument>& out,
                       std::string& err,
                       std::vector<std::string>* warnings = nullptr);

} /* namespace openrgb2 */
