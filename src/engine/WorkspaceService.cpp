/*---------------------------------------------------------*\
|||| WorkspaceService.cpp                                    ||
||||                                                           |
||||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "WorkspaceService.h"

#include "config/StudioConfig.h"
#include "presets/DevicePreset.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>

namespace openrgb2
{

namespace
{

namespace fs = std::filesystem;

bool ReadBytes(const std::string& path, std::string& out,
               uint64_t cap)
{
    std::ifstream in(path, std::ios::binary);
    if(!in)
    {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return out.size() <= cap;
}

/* Layer the types/looks a folder-of-files import carries under
   <dir>/presets/{devices,effects} — a scratch overlay only; the
   caller's registries are never mutated (matching the
   candidate's snapshot discipline). */
void LoadSiblingPresets(const fs::path& file_dir,
                        studio::PresetRegistry& devices,
                        studio::EffectRegistry& effects,
                        std::vector<std::string>& warnings)
{
    const fs::path pdev = file_dir / "presets" / "devices";
    const fs::path pfx  = file_dir / "presets" / "effects";
    std::error_code ec;
    std::vector<std::string> lerrs;
    if(fs::is_directory(pdev, ec))
    {
        devices.LoadDirectory(pdev.string(), &lerrs);
    }
    if(fs::is_directory(pfx, ec))
    {
        effects.LoadDirectory(pfx.string(), &lerrs);
    }
    for(std::string& e : lerrs)
    {
        warnings.push_back("sibling presets: " + e);
    }
}

/* Pull the sibling-file types/looks the document actually
   references into the candidate's extracted sets so the commit
   installs them into the managed library — only what is used,
   only under a free id. */
void HarvestUsedSiblings(const fs::path& file_dir,
                         WorkspaceCandidate& cand)
{
    const fs::path pdev = file_dir / "presets" / "devices";
    const fs::path pfx  = file_dir / "presets" / "effects";
    std::error_code ec;

    if(fs::is_directory(pdev, ec))
    {
        std::set<std::string> used;
        for(const auto& kv : cand.document.devices)
        {
            used.insert(kv.second.type);
        }
        /* Transitive deps inside sibling types — walk to fixpoint
           over the sibling registry. */
        studio::PresetRegistry sreg;
        sreg.LoadDirectory(pdev.string(), nullptr);
        bool grew = true;
        while(grew)
        {
            grew = false;
            std::vector<std::string> snapshot(used.begin(),
                                              used.end());
            for(const std::string& id : snapshot)
            {
                const studio::DevicePreset* p = sreg.Find(id);
                if(p == nullptr)
                {
                    continue;
                }
                for(const std::string& dep :
                        studio::PresetDependencies(*p))
                {
                    if(used.insert(dep).second)
                    {
                        grew = true;
                    }
                }
            }
        }
        std::set<std::string> have;
        for(const studio::DevicePreset& p : cand.extracted_types)
        {
            have.insert(p.id);
        }
        for(const std::string& id : used)
        {
            if(have.count(id))
            {
                continue;
            }
            if(const studio::DevicePreset* p = sreg.Find(id))
            {
                cand.extracted_types.push_back(*p);
                have.insert(id);
            }
        }
    }

    if(fs::is_directory(pfx, ec) &&
       !cand.document.effect.preset.empty())
    {
        studio::EffectRegistry sreg;
        sreg.LoadDirectory(pfx.string(), nullptr);
        const std::string& want = cand.document.effect.preset;
        bool have = false;
        for(const studio::EffectDocument& d : cand.extracted_effects)
        {
            have |= d.id == want;
        }
        if(!have)
        {
            if(const studio::EffectDocument* d = sreg.Find(want))
            {
                cand.extracted_effects.push_back(*d);
            }
        }
    }
}

} /* anonymous namespace */

WorkspaceService::WorkspaceService(std::string root, Deps deps)
    : store_(std::move(root),
             WorkspaceStore::Deps{ deps.devices, deps.effects }),
      deps_(std::move(deps))
{
}

WorkspaceService::Result WorkspaceService::Open(const std::string& id)
{
    std::lock_guard<std::mutex> g(mutex_);
    Result res;
    if(!WorkspaceStore::IsWorkspaceId(id))
    {
        res.error = "invalid workspace id '" + id + "'";
        return res;
    }
    const WorkspaceStore::LoadResult lr = store_.Load(id);
    res.warnings = lr.warnings;
    switch(lr.status)
    {
    case WorkspaceStore::LoadStatus::Empty:
        res.error = "no managed workspace '" + id +
                    "' under " + store_.Root();
        return res;
    case WorkspaceStore::LoadStatus::Failed:
        res.error = lr.error;
        return res;
    case WorkspaceStore::LoadStatus::Recovered:
        /* Honest recovery: the last-valid generation adopted,
           the primary failure reported as a warning. */
        res.recovered = true;
        res.warnings.push_back("recovered: " + lr.error);
        break;
    case WorkspaceStore::LoadStatus::Loaded:
        break;
    }
    current_.id                = id;
    current_.candidate         = lr.candidate;
    current_.workspace_revision = lr.workspace_revision;
    current_.resource_revision  = lr.resource_revision;
    res.ok                 = true;
    res.generation         = lr.generation;
    res.workspace_revision = lr.workspace_revision;
    res.resource_revision  = lr.resource_revision;
    return res;
}

WorkspaceService::Result WorkspaceService::CommitCommon(
    const std::string& id,
    WorkspaceCandidate& cand,
    const std::string& original_bytes,
    uint64_t expected_rev,
    bool apply_after)
{
    Result res;
    /* Service-level stale-writer fast path for the workspace
       currently open — the store's marker check below is the
       authoritative gate either way. */
    if(!current_.id.empty() && current_.id == id &&
       expected_rev != current_.workspace_revision)
    {
        res.error = "stale workspace revision: expected " +
                    std::to_string(expected_rev) + " but '" + id +
                    "' is at " +
                    std::to_string(current_.workspace_revision);
        return res;
    }

    const WorkspaceStore::CommitResult cr =
        store_.Commit(id, cand, original_bytes, expected_rev);
    res.warnings = cr.warnings;
    if(!cr.ok)
    {
        res.error = cr.error;
        return res;
    }
    res.committed          = true;
    res.generation         = cr.generation;
    res.workspace_revision = cr.workspace_revision;
    res.resource_revision  = cr.resource_revision;

    current_.id                = id;
    current_.candidate         = cr.candidate;
    current_.workspace_revision = cr.workspace_revision;
    current_.resource_revision  = cr.resource_revision;

    /* Publish AFTER the durable commit — a failed apply never
       rolls the generation back; it is reported honestly. */
    if(apply_after && deps_.apply_scene &&
       cr.candidate && cr.candidate->resolved)
    {
        std::string aerr;
        if(deps_.apply_scene(cr.candidate->resolved, aerr))
        {
            res.applied = true;
        }
        else
        {
            res.error = "committed generation " +
                        std::to_string(cr.generation) +
                        " but the scene apply failed: " + aerr;
        }
    }
    res.ok = true;
    return res;
}

WorkspaceService::Result WorkspaceService::Save(
    const std::string& id,
    const nlohmann::json& document,
    uint64_t expected_workspace_revision)
{
    std::lock_guard<std::mutex> g(mutex_);
    Result res;
    if(!WorkspaceStore::IsWorkspaceId(id))
    {
        res.error = "invalid workspace id '" + id + "'";
        return res;
    }
    /* The saved document parses as a candidate against the
       effective libraries — the same candidate contract the
       store and CLI loader share. */
    WorkspaceCandidate cand;
    std::vector<std::string> errors;
    if(!ParseWorkspaceCandidate(document,
                                store_.EffectiveDeviceRegistry(),
                                store_.EffectiveEffectRegistry(),
                                cand, errors, res.warnings))
    {
        for(const std::string& e : errors)
        {
            if(!res.error.empty())
            {
                res.error += "; ";
            }
            res.error += e;
        }
        if(res.error.empty())
        {
            res.error = "document failed to parse as a workspace "
                        "candidate";
        }
        return res;
    }
    return CommitCommon(id, cand, document.dump(2),
                        expected_workspace_revision,
                        /*apply_after=*/true);
}

WorkspaceService::Result WorkspaceService::Import(
    const std::string& id,
    const nlohmann::json& document,
    uint64_t expected_workspace_revision)
{
    std::lock_guard<std::mutex> g(mutex_);
    Result res;
    if(!WorkspaceStore::IsWorkspaceId(id))
    {
        res.error = "invalid workspace id '" + id + "'";
        return res;
    }
    WorkspaceCandidate cand;
    std::vector<std::string> errors;
    if(!ParseWorkspaceCandidate(document,
                                store_.EffectiveDeviceRegistry(),
                                store_.EffectiveEffectRegistry(),
                                cand, errors, res.warnings))
    {
        for(const std::string& e : errors)
        {
            if(!res.error.empty())
            {
                res.error += "; ";
            }
            res.error += e;
        }
        if(res.error.empty())
        {
            res.error = "document failed to parse as a workspace "
                        "candidate";
        }
        return res;
    }

    /* Import-boundary hygiene FIRST: an imported workspace never
       automatically enables output, input capture, or verified
       hardware ownership — the stored document and its resolved
       scene are both sanitized before anything commits. */
    SanitizeImportedWorkspace(cand.document);
    if(cand.resolved)
    {
        auto rs = std::make_shared<studio::SceneDocument>(
            *cand.resolved);
        SanitizeImportedScene(*rs);
        cand.resolved = std::move(rs);
    }
    res.warnings.push_back(
        "import sanitized: output/inputs/ownership unarmed; "
        "identity hints kept for review");

    /* Imports NEVER auto-apply — the committed generation waits
       for an explicit open/apply decision. */
    return CommitCommon(id, cand, document.dump(2),
                        expected_workspace_revision,
                        /*apply_after=*/false);
}

WorkspaceService::Result WorkspaceService::ImportFile(
    const std::string& id,
    const std::string& path,
    uint64_t expected_workspace_revision)
{
    std::lock_guard<std::mutex> g(mutex_);
    Result res;
    if(!WorkspaceStore::IsWorkspaceId(id))
    {
        res.error = "invalid workspace id '" + id + "'";
        return res;
    }
    std::string bytes;
    if(!ReadBytes(path, bytes, WorkspaceStore::kMaxDocumentBytes))
    {
        res.error = "cannot read workspace file '" + path +
                    "' (missing or oversized)";
        return res;
    }
    const nlohmann::json j =
        nlohmann::json::parse(bytes, nullptr, false);
    if(j.is_discarded() || !j.is_object())
    {
        res.error = path + " is not a valid workspace document";
        return res;
    }

    /* Scratch registries: base + managed library + the file's
       sibling presets/ dirs — the folder-of-files import the
       source ConfigStore's File->Open flow served. */
    studio::PresetRegistry devreg = store_.EffectiveDeviceRegistry();
    studio::EffectRegistry effreg = store_.EffectiveEffectRegistry();
    const fs::path dir = fs::path(path).parent_path();
    LoadSiblingPresets(dir, devreg, effreg, res.warnings);

    WorkspaceCandidate cand;
    std::vector<std::string> errors;
    if(!ParseWorkspaceCandidate(j, devreg, effreg, cand,
                                errors, res.warnings))
    {
        for(const std::string& e : errors)
        {
            if(!res.error.empty())
            {
                res.error += "; ";
            }
            res.error += e;
        }
        if(res.error.empty())
        {
            res.error = path +
                        " failed to parse as a workspace candidate";
        }
        return res;
    }
    /* A compact source carries no extracted types of its own —
       pull the sibling files it actually references so the
       commit installs them (collision-aware) and the
       generation snapshots them. */
    HarvestUsedSiblings(dir, cand);

    SanitizeImportedWorkspace(cand.document);
    if(cand.resolved)
    {
        auto rs = std::make_shared<studio::SceneDocument>(
            *cand.resolved);
        SanitizeImportedScene(*rs);
        cand.resolved = std::move(rs);
    }
    res.warnings.push_back(
        "import sanitized: output/inputs/ownership unarmed; "
        "identity hints kept for review");

    /* The original imported bytes are retained verbatim in the
       generation — recovery never needs to re-derive them. */
    return CommitCommon(id, cand, bytes,
                        expected_workspace_revision,
                        /*apply_after=*/false);
}

std::shared_ptr<const WorkspaceCandidate>
WorkspaceService::CurrentCandidate() const
{
    std::lock_guard<std::mutex> g(mutex_);
    return current_.candidate;
}

std::string WorkspaceService::CurrentId() const
{
    std::lock_guard<std::mutex> g(mutex_);
    return current_.id;
}

uint64_t WorkspaceService::WorkspaceRevision() const
{
    std::lock_guard<std::mutex> g(mutex_);
    return current_.workspace_revision;
}

uint64_t WorkspaceService::ResourceRevision() const
{
    std::lock_guard<std::mutex> g(mutex_);
    return current_.resource_revision;
}

} /* namespace openrgb2 */
