/*---------------------------------------------------------*\
|||| SceneLoader.cpp                                           ||
||||                                                           |
||||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "SceneLoader.h"

#include "effects/Presets.h"    /* studio::EffectLooks() */

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

} /* anonymous namespace */

bool LoadWorkspaceCandidate(const std::string& path,
                            const studio::PresetRegistry& devices,
                            const studio::EffectRegistry& effects,
                            WorkspaceCandidate& cand,
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

    /* The ONE parse/migrate/validate/resolve path — the same
       funnel the managed store and the import service use. */
    std::vector<std::string> errors, warns;
    if(!ParseWorkspaceCandidate(j, devices, effects, cand,
                                errors, warns))
    {
        err = path + ": " + JoinErrors(errors);
        return false;
    }
    if(warnings)
    {
        warnings->insert(warnings->end(), warns.begin(),
                         warns.end());
    }
    return true;
}

bool LoadSceneDocument(const std::string& path,
                       const studio::PresetRegistry& registry,
                       std::shared_ptr<studio::SceneDocument>& out,
                       std::string& err,
                       std::vector<std::string>* warnings)
{
    WorkspaceCandidate cand;
    if(!LoadWorkspaceCandidate(path, registry,
                               studio::EffectLooks(),
                               cand, err, warnings))
    {
        return false;
    }
    /* The resolved scene is immutable on the candidate — the
       compatibility surface hands out an owned copy. */
    out = std::make_shared<studio::SceneDocument>(*cand.resolved);
    return true;
}

} /* namespace openrgb2 */
