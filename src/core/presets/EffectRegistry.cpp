/*---------------------------------------------------------*\
|||| EffectRegistry.cpp                                        |
||||                                                           |
||||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "EffectRegistry.h"

#include "DevicePreset.h"    /* IsPresetId — id == filename */

#include <filesystem>

namespace studio
{
namespace
{

/* "<id>.effect.json" -> "<id>"; "" when the name doesn't carry
   the suffix. */
std::string BasenameEffect(const std::string& path)
{
    const std::string name =
        std::filesystem::path(path).filename().string();
    const std::string suffix = ".effect.json";
    if(name.size() > suffix.size()
       && name.compare(name.size() - suffix.size(),
                       suffix.size(), suffix) == 0)
    {
        return name.substr(0, name.size() - suffix.size());
    }
    return std::string();
}

/* Load + validate one file; enforces id == basename and a valid
   id charset. `into` is the target map (file layer or defaults).
   Records per-file errors under the file path. */
bool LoadOne(const std::string& path,
             std::map<std::string, EffectDocument>& into,
             std::map<std::string, std::string>& file_errors,
             std::vector<std::string>* errors)
{
    EffectDocument d;
    std::vector<std::string> perrs;
    auto fail = [&](const std::string& msg) {
        file_errors[path] = msg;
        if(errors)
        {
            errors->push_back(path + ": " + msg);
        }
        return false;
    };
    if(!EffectDocumentFromJsonFile(path, d, &perrs))
    {
        const std::string msg = perrs.empty() ? "invalid"
                                              : perrs.front();
        if(errors)
        {
            for(const std::string& e : perrs)
            {
                errors->push_back(e);
            }
        }
        file_errors[path] = msg;
        return false;
    }
    const std::string base = BasenameEffect(path);
    /* id must match the file name — never silently pick whichever
       file was found first. */
    if(!base.empty() && d.id != base)
    {
        return fail("id '" + d.id + "' does not match file name '"
                    + base + "'");
    }
    if(!IsPresetId(d.id))
    {
        return fail("id '" + d.id + "' is not a valid identifier");
    }
    into[d.id] = d;
    file_errors.erase(path);
    return true;
}

} /* anonymous namespace */

void EffectRegistry::SetDefaults(std::vector<EffectDocument> d)
{
    defaults.clear();
    for(EffectDocument& e : d)
    {
        defaults[e.id] = e;
    }
}

bool EffectRegistry::LoadDefaultsDirectory(const std::string& dir,
                                           std::vector<std::string>* errors)
{
    bool ok = true;
    std::error_code ec;
    if(!std::filesystem::is_directory(dir, ec))
    {
        return true;
    }
    for(const auto& entry : std::filesystem::directory_iterator(dir, ec))
    {
        if(!entry.is_regular_file())
        {
            continue;
        }
        const std::string path = entry.path().string();
        if(BasenameEffect(path).empty())
        {
            continue;
        }
        ok &= LoadOne(path, defaults, file_errors, errors);
    }
    return ok;
}

void EffectRegistry::ClearFiles()
{
    effects.clear();
    file_errors.clear();
}

bool EffectRegistry::LoadDirectory(const std::string& dir,
                                   std::vector<std::string>* errors)
{
    bool ok = true;
    std::error_code ec;
    if(!std::filesystem::is_directory(dir, ec))
    {
        return true;   /* no look dir = only defaults; not an error */
    }
    for(const auto& entry : std::filesystem::directory_iterator(dir, ec))
    {
        if(!entry.is_regular_file())
        {
            continue;
        }
        const std::string path = entry.path().string();
        if(BasenameEffect(path).empty())
        {
            continue;
        }
        ok &= LoadOne(path, effects, file_errors, errors);
    }
    return ok;
}

bool EffectRegistry::LoadFile(const std::string& path,
                              std::vector<std::string>* errors)
{
    EffectDocument d;
    if(!EffectDocumentFromJsonFile(path, d, errors))
    {
        return false;
    }
    const std::string base = BasenameEffect(path);
    if(!base.empty() && d.id != base)
    {
        if(errors)
        {
            errors->push_back(path + ": id '" + d.id
                + "' does not match file name '" + base + "'");
        }
        return false;
    }
    if(!IsPresetId(d.id))
    {
        if(errors)
        {
            errors->push_back(path + ": id '" + d.id
                + "' is not a valid identifier");
        }
        return false;
    }
    effects[d.id] = d;
    file_errors.erase(path);
    return true;
}

bool EffectRegistry::Add(const EffectDocument& d,
                         std::vector<std::string>* errors)
{
    if(!IsPresetId(d.id))
    {
        if(errors)
        {
            errors->push_back("effect: id '" + d.id
                + "' is not a valid identifier");
        }
        return false;
    }
    effects[d.id] = d;
    return true;
}

const EffectDocument* EffectRegistry::Find(const std::string& id) const
{
    const auto it = effects.find(id);
    if(it != effects.end())
    {
        return &it->second;
    }
    const auto dit = defaults.find(id);
    if(dit != defaults.end())
    {
        return &dit->second;
    }
    return nullptr;
}

std::vector<std::string> EffectRegistry::Ids() const
{
    std::vector<std::string> ids;
    for(const auto& kv : defaults)
    {
        ids.push_back(kv.first);
    }
    for(const auto& kv : effects)
    {
        if(defaults.find(kv.first) == defaults.end())
        {
            ids.push_back(kv.first);
        }
    }
    return ids;
}

bool EffectRegistry::Build(const std::string& id, unsigned int seed,
                           std::vector<EffectLayer>& out,
                           std::vector<std::string>* errors) const
{
    const EffectDocument* d = Find(id);
    if(d == nullptr)
    {
        if(errors)
        {
            errors->push_back("unknown effect look '" + id + "'");
        }
        return false;
    }
    return ResolveEffectLayers(*d, seed, out, errors);
}

std::vector<EffectRegistry::EffectInfo> EffectRegistry::List(
    const std::vector<std::string>& known_order) const
{
    /* Merge file-over-default into one ordered map first. */
    std::map<std::string, std::pair<const EffectDocument*, bool>> merged;
    for(const auto& kv : defaults)
    {
        merged[kv.first] = { &kv.second, false };
    }
    for(const auto& kv : effects)
    {
        merged[kv.first] = { &kv.second, true };
    }

    auto to_info = [](const std::pair<const EffectDocument*, bool>& e)
    {
        EffectInfo i;
        i.id          = e.first->id;
        i.name        = e.first->name;
        i.description = e.first->description;
        i.needs       = e.first->needs;
        i.from_file   = e.second;
        return i;
    };

    std::vector<EffectInfo> out;
    /* Known-order entries keep their card position even when a
       file overrides the packaged look. */
    for(const std::string& id : known_order)
    {
        const auto it = merged.find(id);
        if(it != merged.end())
        {
            out.push_back(to_info(it->second));
            merged.erase(it);
        }
    }
    for(const auto& kv : merged)
    {
        out.push_back(to_info(kv.second));
    }
    return out;
}

} /* namespace studio */
