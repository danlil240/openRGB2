/*---------------------------------------------------------*\
||| SceneLoader.cpp                                           ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "SceneLoader.h"

#include "config/ConfigMigration.h"
#include "config/StudioConfig.h"
#include "scene/SceneJson.h"
#include "scene/SceneResolver.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

namespace openrgb2
{

namespace
{

std::string JoinErrors(const std::vector<std::string>& errs)
{
    std::string out;
    for(const std::string& e : errs)
    {
        if(!out.empty())
        {
            out += "; ";
        }
        out += e;
    }
    return out;
}

/* Compact workspace -> runtime scene. `ws` is already a
   validated candidate; expansion runs against `reg`. */
bool ResolveWorkspace(const studio::StudioDocument& ws,
                      const studio::PresetRegistry& reg,
                      std::shared_ptr<studio::SceneDocument>& out,
                      std::string& err,
                      std::vector<std::string>* warnings)
{
    auto doc = std::make_shared<studio::SceneDocument>();
    std::vector<std::string> errors;
    if(!studio::ResolveScene(ws, reg, *doc, &errors, warnings))
    {
        err = "workspace failed to resolve: " + JoinErrors(errors);
        return false;
    }
    out = std::move(doc);
    return true;
}

} /* anonymous namespace */

bool LoadSceneDocument(const std::string& path,
                       const studio::PresetRegistry& registry,
                       std::shared_ptr<studio::SceneDocument>& out,
                       std::string& err,
                       std::vector<std::string>* warnings)
{
    std::ifstream in(path, std::ios::binary);
    if(!in)
    {
        err = "cannot open " + path;
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const nlohmann::json j =
        nlohmann::json::parse(ss.str(), nullptr, false);
    if(j.is_discarded())
    {
        err = path + " is not valid JSON";
        return false;
    }
    if(!j.is_object())
    {
        err = path + ": expected a JSON object";
        return false;
    }

    std::vector<std::string> errors;

    /*---- Compact workspace (studio.json, schema v3) --------*/
    if(j.contains("schema_version"))
    {
        studio::StudioDocument ws;
        if(!studio::FromJson(j, ws, &errors, warnings))
        {
            err = "workspace document failed to load: " +
                  JoinErrors(errors);
            return false;
        }
        return ResolveWorkspace(ws, registry, out, err, warnings);
    }

    /*---- Legacy host-settings blob {"scene":...,"inputs":...}*\
    || The old project's settings form — migrated forward       ||
    || through the same ConfigMigration path the file store    ||
    || ran (extracted types layer over a COPY of the device    ||
    || registry). A migrated scene has no playback selection:  ||
    || it lands as Live effects, never uploaded playback.      ||
    \*---------------------------------------------------------*/
    if(studio::HasLegacySettings(j))
    {
        studio::StudioDocument ws;
        std::vector<studio::DevicePreset> types;
        if(!studio::MigrateLegacySettings(j, ws, &types, &errors))
        {
            err = "legacy settings failed to migrate: " +
                  JoinErrors(errors);
            return false;
        }
        studio::PresetRegistry reg = registry;   /* scratch copy */
        for(const studio::DevicePreset& t : types)
        {
            std::vector<std::string> terrs;
            if(!reg.Add(t, &terrs) && warnings)
            {
                warnings->push_back("legacy type '" + t.id +
                                    "' not registered: " +
                                    JoinErrors(terrs));
            }
        }
        return ResolveWorkspace(ws, reg, out, err, warnings);
    }

    /*---- Expanded SceneDocument (core schema, v1/v2) --------*\
    || v1 upgrades in place inside SceneJson::FromJson — the    ||
    || same treatment the file store gave old scene files.     ||
    \*---------------------------------------------------------*/
    if(j.contains("version"))
    {
        auto doc = std::make_shared<studio::SceneDocument>();
        if(!studio::FromJson(j, *doc, &errors))
        {
            err = "scene document failed to load: " +
                  JoinErrors(errors);
            return false;
        }
        out = std::move(doc);
        return true;
    }

    err = path + ": unrecognised document — expected a scene "
          "('version'), a workspace ('schema_version'), or "
          "legacy settings ('scene')";
    return false;
}

} /* namespace openrgb2 */
