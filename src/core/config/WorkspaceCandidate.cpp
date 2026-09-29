/*---------------------------------------------------------*\
|||| WorkspaceCandidate.cpp                                  ||
||||                                                           |
||||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "WorkspaceCandidate.h"

#include "ConfigMigration.h"
#include "../scene/SceneJson.h"
#include "../scene/SceneResolver.h"

#include <utility>

namespace openrgb2
{

namespace
{

/* The preference sections a v1/v2 expanded workspace file carried
   alongside its scene — the migrated compact document knows
   nothing about editor prefs, so the file's own copies overlay it
   (ConfigStore::LoadParsed parity). */
const char* const kCarriedSections[] = {
    "ui", "camera", "controls", "render", "inputs", "extensions",
};

/* Re-validate a migrated document through the strict v3 reader —
   the same "re-validated as a normal v3 candidate" discipline
   ConfigStore applied before a migration was allowed to land. */
bool Revalidate(const studio::StudioDocument& doc,
                std::vector<std::string>& errors,
                std::vector<std::string>& warnings,
                studio::StudioDocument& out)
{
    const nlohmann::json wj = studio::ToJson(doc);
    return studio::FromJson(wj, out, &errors, &warnings);
}

bool ResolveInto(const studio::StudioDocument& doc,
                 const studio::PresetRegistry& reg,
                 WorkspaceCandidate& c,
                 std::vector<std::string>& errors,
                 std::vector<std::string>& warnings)
{
    auto resolved = std::make_shared<studio::SceneDocument>();
    std::vector<std::string> rerrs;
    if(!studio::ResolveScene(doc, reg, *resolved, &rerrs, &warnings))
    {
        errors.push_back("workspace failed to resolve:");
        for(const std::string& e : rerrs)
        {
            errors.push_back("  " + e);
        }
        return false;
    }
    c.resolved = std::move(resolved);
    return true;
}

/* Layer extracted migration types over a copy of the supplied
   registry. A type that fails to register (bad deps, invalid id)
   is a warning, not a failure — the resolve step reports any
   resulting dangling reference honestly. */
void OverlayTypes(studio::PresetRegistry& reg,
                  const std::vector<studio::DevicePreset>& types,
                  std::vector<std::string>& warnings)
{
    for(const studio::DevicePreset& t : types)
    {
        std::vector<std::string> terrs;
        if(!reg.Add(t, &terrs))
        {
            std::string msg = "extracted type '" + t.id +
                              "' not registered:";
            for(const std::string& e : terrs)
            {
                msg += " " + e;
            }
            warnings.push_back(msg);
        }
    }
}

void EffectNote(const studio::StudioDocument& doc,
                const studio::EffectRegistry& effects,
                std::vector<std::string>& warnings)
{
    /* A named preset that resolves nowhere in the supplied
       look registry is a note, not an error — playback preset
       names legitimately carry no registry stack, and a missing
       look can be fixed without re-importing the workspace. */
    if(doc.effect.layers.empty() && !doc.effect.preset.empty() &&
       !effects.Contains(doc.effect.preset))
    {
        warnings.push_back("effects.preset: '" + doc.effect.preset +
                           "' is not in the supplied effect-look "
                           "registry");
    }
}

} /* anonymous namespace */

const char* WorkspaceSourceFormName(WorkspaceCandidate::SourceForm f)
{
    switch(f)
    {
    case WorkspaceCandidate::SourceForm::CompactWorkspace:
        return "compact-workspace";
    case WorkspaceCandidate::SourceForm::LegacySettings:
        return "legacy-settings";
    case WorkspaceCandidate::SourceForm::ExpandedWorkspace:
        return "expanded-workspace";
    case WorkspaceCandidate::SourceForm::ExpandedScene:
        return "expanded-scene";
    }
    return "unknown";
}

bool ParseWorkspaceCandidate(const nlohmann::json& j,
                             const studio::PresetRegistry& devices,
                             const studio::EffectRegistry& effects,
                             WorkspaceCandidate& out,
                             std::vector<std::string>& errors,
                             std::vector<std::string>& warnings)
{
    errors.clear();
    warnings.clear();
    if(!j.is_object())
    {
        errors.push_back("root: expected a JSON object");
        return false;
    }

    WorkspaceCandidate c;

    /*---- Anything with an object "scene" member is an expanded
    ||   source — the host-settings blob {"scene":...,"inputs":
    ||   ...} OR the on-disk v1/v2 expanded workspace
    ||   {"schema_version":<3, "scene":{...}, "definitions":...}.
    ||   StudioConfig::FromJson hard-rejects both, so detection
    ||   must route them through ConfigMigration first — the same
    ||   ordering the engine's SceneLoader used. A migrated scene
    ||   carries no playback selection: it lands as Live effects,
    ||   never uploaded playback.
    \*---------------------------------------------------------*/
    if(studio::HasLegacySettings(j))
    {
        studio::SceneDocument scene;
        {
            std::vector<std::string> serrs;
            if(!studio::FromJson(j["scene"], scene, &serrs))
            {
                for(const std::string& e : serrs)
                {
                    errors.push_back("scene: " + e);
                }
                return false;
            }
        }

        const auto vit = j.find("schema_version");
        if(vit != j.end())
        {
            /* v1/v2 expanded workspace on disk. */
            if(!vit->is_number_integer())
            {
                errors.push_back("schema_version: expected integer");
                return false;
            }
            const long long v = vit->get<long long>();
            if(v >= studio::STUDIO_SCHEMA_VERSION)
            {
                errors.push_back(
                    "scene: expanded scenes must not be serialized "
                    "in schema v3");
                return false;
            }
            if(v < 1)
            {
                errors.push_back("schema_version: expected >= 1");
                return false;
            }
            std::vector<std::string> merrs;
            if(!studio::MigrateExpandedScene(scene, c.document,
                                             c.extracted_types,
                                             &merrs, &warnings))
            {
                for(const std::string& e : merrs)
                {
                    errors.push_back("migration: " + e);
                }
                return false;
            }
            /* The file's own preference sections win over the
               migrated scene's defaults — ConfigStore parity.
               live_on_startup is always cleared. */
            nlohmann::json wj = studio::ToJson(c.document);
            for(const char* k : kCarriedSections)
            {
                if(j.contains(k))
                {
                    wj[k] = j[k];
                }
            }
            if(j.contains("name"))
            {
                wj["name"] = j["name"];
            }
            const auto oit = j.find("output");
            if(oit != j.end() && oit->is_object() &&
               oit->contains("brightness"))
            {
                wj["output"]["brightness"] = (*oit)["brightness"];
            }
            const auto eit = j.find("effects");
            if(eit != j.end() && eit->is_object())
            {
                for(auto it = eit->begin(); it != eit->end(); ++it)
                {
                    wj["effects"][it.key()] = it.value();
                }
            }
            wj["output"]["live_on_startup"] = false;

            studio::StudioDocument   checked;
            std::vector<std::string> cerrs;
            if(!studio::FromJson(wj, checked, &cerrs, &warnings))
            {
                for(const std::string& e : cerrs)
                {
                    errors.push_back("migrated document: " + e);
                }
                return false;
            }
            c.document    = std::move(checked);
            c.source_form =
                WorkspaceCandidate::SourceForm::ExpandedWorkspace;
            c.migrated    = true;
        }
        else
        {
            /* Legacy host-settings blob. */
            std::vector<std::string> merrs;
            if(!studio::MigrateLegacySettings(j, c.document,
                                              &c.extracted_types,
                                              &merrs))
            {
                for(const std::string& e : merrs)
                {
                    errors.push_back(e);
                }
                return false;
            }
            c.source_form =
                WorkspaceCandidate::SourceForm::LegacySettings;
            c.migrated    = true;
        }

        c.device_registry = devices;
        OverlayTypes(c.device_registry, c.extracted_types, warnings);
        c.effect_registry = effects;
        if(!ResolveInto(c.document, c.device_registry, c,
                        errors, warnings))
        {
            return false;
        }
        EffectNote(c.document, c.effect_registry, warnings);
        out = std::move(c);
        return true;
    }

    /*---- Compact workspace (studio.json, schema v3). Only
    ||   reached with no "scene" member — expanded workspaces
    ||   were claimed above. Newer versions are rejected and
    ||   NEVER rewritten.
    \*---------------------------------------------------------*/
    if(j.contains("schema_version"))
    {
        if(!studio::FromJson(j, c.document, &errors, &warnings))
        {
            return false;
        }
        c.source_form =
            WorkspaceCandidate::SourceForm::CompactWorkspace;
        c.device_registry = devices;
        c.effect_registry = effects;
        if(!ResolveInto(c.document, c.device_registry, c,
                        errors, warnings))
        {
            return false;
        }
        EffectNote(c.document, c.effect_registry, warnings);
        out = std::move(c);
        return true;
    }

    /*---- Expanded SceneDocument (core schema, v1/v2). The
    ||   resolved scene is the parsed document itself —
    ||   expanded-scene compatibility is verbatim — while the
    ||   migrated compact document keeps the authoring data
    ||   durable instead of discarding it.
    \*---------------------------------------------------------*/
    if(j.contains("version"))
    {
        auto scene = std::make_shared<studio::SceneDocument>();
        if(!studio::FromJson(j, *scene, &errors))
        {
            return false;
        }
        std::vector<std::string> merrs;
        if(!studio::MigrateExpandedScene(*scene, c.document,
                                         c.extracted_types,
                                         &merrs, &warnings))
        {
            for(const std::string& e : merrs)
            {
                errors.push_back("migration: " + e);
            }
            return false;
        }
        /* Re-validate the migrated document through the strict
           v3 reader — an expansion the validator would reject
           never becomes a candidate. */
        studio::StudioDocument checked;
        if(!Revalidate(c.document, errors, warnings, checked))
        {
            return false;
        }
        c.document    = std::move(checked);
        c.source_form = WorkspaceCandidate::SourceForm::ExpandedScene;
        c.migrated    = true;
        c.device_registry = devices;
        OverlayTypes(c.device_registry, c.extracted_types, warnings);
        c.effect_registry = effects;
        c.resolved        = std::move(scene);
        EffectNote(c.document, c.effect_registry, warnings);
        out = std::move(c);
        return true;
    }

    errors.push_back(
        "unrecognised document — expected a scene ('version'), "
        "a workspace ('schema_version'), or legacy settings "
        "('scene')");
    return false;
}

void SanitizeImportedWorkspace(studio::StudioDocument& doc)
{
    /* An import never automatically enables output, input
       capture, or verified hardware ownership (plan section
       3.2). Identity hints (serial/location) stay for review —
       portable export is where SanitizeForExport strips them. */
    doc.meta.live_on_startup = false;
    doc.inputs.audio         = false;
    doc.inputs.keys          = false;
    doc.inputs.screen        = false;
    for(auto& kv : doc.device_settings)
    {
        for(auto& zkv : kv.second.zones)
        {
            zkv.second.verified = false;
        }
    }
    doc.effect.playing = false;
}

void SanitizeImportedScene(studio::SceneDocument& doc)
{
    for(studio::SceneObject& o : doc.objects)
    {
        o.verified = false;
    }
}

} /* namespace openrgb2 */
