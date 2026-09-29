/*---------------------------------------------------------*\
|||| WorkspaceCandidate.h                                    ||
||||                                                           |
||||   The canonical workspace candidate (studio-import    ||
||||   Task 2, plan section 3.2): the ONE                  ||
||||   parse/migrate/validate/resolve path every persisted ||
||||   workspace form funnels through — compact v3         ||
||||   studio.json, legacy host-settings blob, expanded    ||
||||   v1/v2 workspace, and the expanded OpenRGB2          ||
||||   SceneDocument.                                      ||
||||                                                           |
||||   A candidate is immutable once produced: it carries  ||
||||   the compact authoring document, SNAPSHOTS of the    ||
||||   registries it resolved against (the caller's        ||
||||   registries are copied, never mutated by extracted   ||
||||   types), the migration's extracted device types, and ||
||||   the resolved runtime scene the engine applies.      ||
||||                                                           |
||||   Accepted forms, in detection order (the same order  ||
||||   engine SceneLoader used):                           ||
||||   - object "scene" member + "schema_version" 1..2:    ||
||||     the on-disk v1/v2 expanded workspace —            ||
||||     MigrateExpandedScene + the file's own preference  ||
||||     sections overlaid (ConfigStore::LoadParsed        ||
||||     parity), resolved via the expanded registry       ||
||||   - object "scene" member alone: the legacy host-     ||
||||     settings blob — MigrateLegacySettings             ||
||||   - "schema_version" == 3: StudioConfig::FromJson +   ||
||||     SceneResolver; newer versions are REJECTED and    ||
||||     never rewritten                                   ||
||||   - "version" 1/2: expanded SceneDocument — resolved  ||
||||     verbatim (expanded-scene compatibility), with the ||
||||     migrated compact document retained for authoring  ||
||||                                                           |
||||   Migration rule (plan section 3.2): none of these    ||
||||   forms carries a playback selection — a migrated or  ||
||||   resolved scene defaults to Live effects, never      ||
||||   uploaded playback. Migration never arms             ||
||||   live_on_startup.                                    ||
||||                                                           ||
||||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "StudioConfig.h"
#include "../presets/DevicePreset.h"
#include "../effects/EffectJson.h"   /* studio::EffectDocument */
#include "../presets/EffectRegistry.h"
#include "../presets/PresetRegistry.h"
#include "../scene/SceneTypes.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

namespace openrgb2
{

struct WorkspaceCandidate
{
    /* Which persisted form the candidate was produced from —
       diagnostics and the generation manifest record it. */
    enum class SourceForm
    {
        CompactWorkspace,   /* schema_version == 3 studio.json         */
        LegacySettings,     /* host-settings blob {"scene":...}        */
        ExpandedWorkspace,  /* schema_version 1/2 + "scene" on disk    */
        ExpandedScene,      /* OpenRGB2 SceneDocument "version" 1/2    */
    };
    SourceForm source_form = SourceForm::CompactWorkspace;
    /* True when the source predated v3 and passed through
       migration. */
    bool       migrated = false;

    /* The compact v3 authoring document — ALWAYS populated.
       Expanded and legacy sources migrate through
       ConfigMigration so downstream code handles exactly one
       authoring form. */
    studio::StudioDocument             document;

    /* Device types extracted by migration (legacy/expanded
       sources). Already layered into `device_registry`; kept
       separately so the store can persist them as managed
       resource files. Empty for compact v3 sources. */
    std::vector<studio::DevicePreset>  extracted_types;

    /* Effect looks carried alongside the candidate (bundle or
       folder-of-files imports). Installed into the managed
       effect library on commit with the same collision-aware
       rename + reference-remap discipline as device types.
       Empty for plain document parses. */
    std::vector<studio::EffectDocument> extracted_effects;

    /* Registry snapshots taken at parse time — the versioned
       resource set this workspace resolved against. Copied from
       the supplied registries plus extracted types; the caller's
       registries are never mutated. */
    studio::PresetRegistry             device_registry;
    studio::EffectRegistry             effect_registry;

    /* The runtime scene the engine applies. For compact and
       migrated sources this is SceneResolver's expansion of
       `document` against `device_registry`. For an expanded
       SceneDocument source it is the parsed document itself —
       expanded-scene compatibility is preserved verbatim. */
    std::shared_ptr<const studio::SceneDocument> resolved;
};

const char* WorkspaceSourceFormName(WorkspaceCandidate::SourceForm f);

/* Parse/migrate/validate/resolve one workspace document body.
   - `devices` / `effects` are the libraries the candidate
     resolves against; they are COPIED — extracted legacy types
     layer over a private snapshot only.
   - On success `out` receives the candidate and the function
     returns true; on failure `out` is untouched and `errors`
     holds field-path messages (never a blank or guessed scene).
   - `warnings` collects non-fatal notes: migration notes,
     unknown effect presets, unregistered extracted types.
   Never throws. */
bool ParseWorkspaceCandidate(const nlohmann::json& j,
                             const studio::PresetRegistry& devices,
                             const studio::EffectRegistry& effects,
                             WorkspaceCandidate& out,
                             std::vector<std::string>& errors,
                             std::vector<std::string>& warnings);

/* Import-boundary hygiene (plan section 3.2): an imported
   workspace never automatically enables output, input capture,
   or verified hardware ownership — the import clears the arming
   fields while local identity hints (serial/location) stay for
   review. Distinct from studio::SanitizeForExport, which is the
   PORTABLE-export rule and additionally strips identity. */
void SanitizeImportedWorkspace(studio::StudioDocument& doc);

/* The resolved-scene half of the import boundary: clears every
   object's verified flag in place so an applied imported scene
   can never write hardware before local re-verification. */
void SanitizeImportedScene(studio::SceneDocument& doc);

} /* namespace openrgb2 */
