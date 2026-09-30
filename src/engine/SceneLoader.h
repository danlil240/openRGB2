/*---------------------------------------------------------*\
|||| SceneLoader.h                                             ||
||||                                                           |
||||   The ONE file->SceneDocument path for the engine      ||
||||   (Task 3.8 — "--scene workspace-doc resolution"),    ||
||||   now funnelling through the canonical candidate      ||
||||   (studio-import Task 2): every persisted form —      ||
||||   compact v3 studio.json, legacy host-settings blob,  ||
||||   expanded v1/v2 workspace, expanded SceneDocument —  ||
||||   resolves through ParseWorkspaceCandidate, so CLI    ||
||||   loading, the managed store, and imports all share   ||
||||   one parse/migrate/validate/resolve path.            ||
||||                                                           ||
||||   Accepted forms, in detection order (see             ||
||||   core/config/WorkspaceCandidate.h):                  ||
||||   - object "scene" member + "schema_version" 1..2:    ||
||||     the on-disk expanded workspace —                  ||
||||     MigrateExpandedScene + carried preference         ||
||||     sections                                          ||
||||   - object "scene" member alone: the legacy host-     ||
||||     settings blob — MigrateLegacySettings             ||
||||   - "schema_version" == 3: StudioConfig::FromJson +   ||
||||     SceneResolver through the device-type registry    ||
||||   - "version" 1/2: expanded SceneDocument — kept      ||
||||     verbatim as the resolved scene                    ||
||||                                                           ||
||||   Migration rule (plan section 3.4): none of these     ||
||||   forms carries a playback selection — a migrated or  ||
||||   resolved scene has NO per-scene playback entry and  ||
||||   therefore defaults to LiveEffects. Old profiles are  ||
||||   preserved as Live effects, never silently converted  ||
||||   to uploaded playback.                                ||
||||                                                           ||
||||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "config/WorkspaceCandidate.h"
#include "presets/EffectRegistry.h"
#include "presets/PresetRegistry.h"
#include "scene/SceneTypes.h"

#include <memory>
#include <string>
#include <vector>

namespace openrgb2
{

/* Load `path` into the canonical workspace candidate — the
   caller receives the compact authoring document, registry
   snapshots, and the resolved runtime scene together.
   - `devices` / `effects` are the libraries the candidate
     resolves against (they are COPIED — extracted types layer
     over a private snapshot, never the caller's).
   - On success `cand` receives the candidate and the function
     returns true; on failure `cand` is untouched and `err`
     carries the field messages (never a blank or guessed
     scene). `warnings`, when given, collects non-fatal notes. */
bool LoadWorkspaceCandidate(const std::string& path,
                            const studio::PresetRegistry& devices,
                            const studio::EffectRegistry& effects,
                            WorkspaceCandidate& cand,
                            std::string& err,
                            std::vector<std::string>* warnings = nullptr);

/* The compatibility surface: same contract as before, resolved
   through the candidate path — `out` receives a copy of the
   candidate's resolved scene. `effects` defaults to the shared
   look library (studio::EffectLooks()). */
bool LoadSceneDocument(const std::string& path,
                       const studio::PresetRegistry& registry,
                       std::shared_ptr<studio::SceneDocument>& out,
                       std::string& err,
                       std::vector<std::string>* warnings = nullptr);

} /* namespace openrgb2 */
