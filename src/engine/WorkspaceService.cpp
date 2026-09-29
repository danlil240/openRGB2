/*---------------------------------------------------------*\
|||| WorkspaceService.cpp                                    ||
||||                                                           |
||||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "WorkspaceService.h"

#include "config/StudioConfig.h"
#include "presets/DevicePreset.h"
#include "presets/PresetBundle.h"
#include "effects/EffectJson.h"

#include <algorithm>
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

/* Job-result report arrays ride the 1 MiB reply envelope — a
   pathological bundle (thousands of bundled presets or parse
   errors) must never push a result over it. Reports keep the
   first entries and carry the dropped count honestly. */
constexpr size_t kMaxReportItems = 256;

void CapReportArray(nlohmann::json& report, const char* key)
{
    const auto it = report.find(key);
    if(it == report.end() || !it->is_array() ||
       it->size() <= kMaxReportItems)
    {
        return;
    }
    const size_t dropped = it->size() - kMaxReportItems;
    it->erase(it->begin() + (ptrdiff_t)kMaxReportItems,
              it->end());
    report[std::string(key) + "_truncated"] = dropped;
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
    /* One bounded worker — IPC handlers only validate, enqueue,
       or read snapshots; every file-touching workspace op runs
       here so PING and the legacy control surface never block
       on a slow commit. */
    worker_ = std::thread([this] { WorkerMain(); });
}

WorkspaceService::~WorkspaceService()
{
    Shutdown();
}

void WorkspaceService::Shutdown()
{
    {
        std::lock_guard<std::mutex> g(jobs_mutex_);
        jobs_stop_.store(true);
    }
    jobs_cv_.notify_all();
    if(worker_.joinable())
    {
        worker_.join();
    }
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
    current_.generation         = lr.generation;
    PublishLocked();
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
    bool apply_after,
    std::shared_ptr<const WorkspaceCandidate>* apply_cand)
{
    Result res;
    /* Service-level stale-writer fast path for the workspace
       currently open — the store's marker check below is the
       authoritative gate either way. */
    if(!current_.id.empty() && current_.id == id &&
       expected_rev != current_.workspace_revision)
    {
        res.error  = "stale workspace revision: expected " +
                     std::to_string(expected_rev) + " but '" + id +
                     "' is at " +
                     std::to_string(current_.workspace_revision);
        res.detail = "revision_conflict";
        return res;
    }

    const WorkspaceStore::CommitResult cr =
        store_.Commit(id, cand, original_bytes, expected_rev);
    res.warnings = cr.warnings;
    if(!cr.ok)
    {
        res.error = cr.error;
        if(cr.error.find("revision") != std::string::npos ||
           cr.error.find("stale")    != std::string::npos)
        {
            res.detail = "revision_conflict";
        }
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
    current_.generation         = cr.generation;
    PublishLocked();

    /* Publish AFTER the durable commit — and AFTER the caller
       releases the service mutex. The apply emits the scene
       event whose payload re-reads this service (the managed
       flag), so running it here under the lock is a same-
       thread self-deadlock; a failed apply never rolls the
       generation back, it is reported honestly. */
    if(apply_after && apply_cand && deps_.apply_scene &&
       cr.candidate && cr.candidate->resolved)
    {
        *apply_cand = cr.candidate;
    }
    res.ok = true;
    return res;
}

void WorkspaceService::ApplyAfterCommit(
    const std::shared_ptr<const WorkspaceCandidate>& cand,
    uint64_t generation, Result& res)
{
    if(!cand || !deps_.apply_scene)
    {
        return;
    }
    std::string aerr;
    bool applied_ok = false;
    try
    {
        applied_ok = deps_.apply_scene(cand, aerr);
    }
    catch(const std::exception& e)
    {
        aerr = std::string("apply threw: ") + e.what();
    }
    catch(...)
    {
        aerr = "apply threw";
    }
    if(applied_ok)
    {
        res.applied = true;
    }
    else
    {
        res.error = "committed generation " +
                    std::to_string(generation) +
                    " but the scene apply failed: " + aerr;
    }
}

WorkspaceService::Result WorkspaceService::Save(
    const std::string& id,
    const nlohmann::json& document,
    uint64_t expected_workspace_revision)
{
    std::shared_ptr<const WorkspaceCandidate> apply_cand;
    Result res;
    {
        std::lock_guard<std::mutex> g(mutex_);
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
                res.error = "document failed to parse as a "
                            "workspace candidate";
            }
            return res;
        }
        res = CommitCommon(id, cand, document.dump(2),
                           expected_workspace_revision,
                           /*apply_after=*/true, &apply_cand);
    }
    /* The apply runs after the lock is released — a scene
       publish emits events that re-enter this service. */
    ApplyAfterCommit(apply_cand, res.generation, res);
    return res;
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
                        /*apply_after=*/false, nullptr);
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
                        /*apply_after=*/false, nullptr);
}

std::shared_ptr<const WorkspaceCandidate>
WorkspaceService::CurrentCandidate() const
{
    std::lock_guard<std::mutex> g(mutex_);
    return current_.candidate;
}

void WorkspaceService::PublishLocked()
{
    /* current_ -> snap_ copy; snap_mutex_ is held for the
       copy only — readers never queue behind commit I/O. */
    PublishedState s;
    s.managed            = !current_.id.empty();
    s.id                 = current_.id;
    s.workspace_revision = current_.workspace_revision;
    s.resource_revision  = current_.resource_revision;
    s.generation         = current_.generation;
    if(current_.candidate)
    {
        s.name        = current_.candidate->document.meta.name;
        s.source_form =
            WorkspaceSourceFormName(current_.candidate->source_form);
    }
    std::lock_guard<std::mutex> g(snap_mutex_);
    snap_ = std::move(s);
}

WorkspaceService::PublishedState
WorkspaceService::Snapshot() const
{
    std::lock_guard<std::mutex> g(snap_mutex_);
    return snap_;
}

std::string WorkspaceService::CurrentId() const
{
    return Snapshot().id;
}

uint64_t WorkspaceService::WorkspaceRevision() const
{
    return Snapshot().workspace_revision;
}

uint64_t WorkspaceService::ResourceRevision() const
{
    return Snapshot().resource_revision;
}

uint64_t WorkspaceService::CurrentGeneration() const
{
    return Snapshot().generation;
}

bool WorkspaceService::ManagedActive() const
{
    return Snapshot().managed;
}

/*---------------------------------------------------------*\
||| IPC job machinery (Task 3) — one bounded worker; the      ||
||| handler layer enqueues only.                              ||
\*---------------------------------------------------------*/

void WorkspaceService::SetJobSurface(JobSurface surface)
{
    std::lock_guard<std::mutex> g(jobs_mutex_);
    surface_ = std::move(surface);
}

void WorkspaceService::SetJobEventHook(
    std::function<void(const JobStatus&)> hook)
{
    std::lock_guard<std::mutex> g(jobs_mutex_);
    surface_.job_event = std::move(hook);
}

void WorkspaceService::SetPreJobHookForTesting(
    std::function<void(const std::string&)> hook)
{
    std::lock_guard<std::mutex> g(jobs_mutex_);
    pre_job_hook_ = std::move(hook);
}

namespace
{

bool JsonU64(const nlohmann::json& j, const char* key,
             uint64_t& out)
{
    const auto it = j.find(key);
    if(it == j.end() || !it->is_number_unsigned())
    {
        return false;
    }
    out = it->get<uint64_t>();
    return true;
}

bool IsJobVerb(const std::string& v)
{
    return v == "validate"       || v == "commit"       ||
           v == "save_copy"    || v == "import_inspect" ||
           v == "import_commit" || v == "export"      ||
           v == "resource_install" ||
           v == "reload";
}

} /* anonymous namespace */

WorkspaceService::JobResult WorkspaceService::Enqueue(
    uint64_t session_id, const std::string& verb,
    const nlohmann::json& body)
{
    JobResult r;
    if(!IsJobVerb(verb))
    {
        r.error  = "unknown job verb '" + verb + "'";
        r.detail = "bad_request";
        return r;
    }

    /* Cheap synchronous validation — the worker re-checks the
       durable revisions; enqueue never does file I/O. `reload`
       may omit id: the worker falls back to the current
       workspace. */
    const bool needs_id = verb != "export" &&
                          verb != "import_inspect" &&
                          verb != "reload";
    const std::string id = body.value("id", std::string());
    if(needs_id && id.empty())
    {
        r.error  = "'" + verb + "' requires an id";
        r.detail = "bad_request";
        return r;
    }
    if(!id.empty() && !WorkspaceStore::IsWorkspaceId(id))
    {
        r.error  = "invalid workspace id '" + id + "'";
        r.detail = "bad_request";
        return r;
    }
    const bool needs_revisions =
        verb == "commit" || verb == "save_copy" ||
        verb == "validate" || verb == "resource_install";
    if(needs_revisions)
    {
        uint64_t unused = 0;
        if(!JsonU64(body, "base_revision", unused) ||
           !JsonU64(body, "resource_revision", unused))
        {
            r.error  = "'" + verb + "' requires base_revision and "
                       "resource_revision";
            r.detail = "bad_request";
            return r;
        }
    }
    /* resource_install carries the effect document inline —
       bounded by the job-body cap like any other verb. */
    if(verb == "resource_install" &&
       (!body.contains("effect") || !body["effect"].is_object()))
    {
        r.error  = "'resource_install' requires an effect object";
        r.detail = "bad_request";
        return r;
    }
    const bool needs_source = verb == "commit" || verb == "save_copy" ||
                              verb == "validate";
    if(needs_source && !body.contains("document") &&
       !body.contains("staged"))
    {
        r.error  = "'" + verb + "' requires document or staged";
        r.detail = "bad_request";
        return r;
    }
    if((verb == "import_inspect" || verb == "import_commit") &&
       !body.value("staged", std::string()).size())
    {
        r.error  = "'" + verb + "' requires a staged token";
        r.detail = "bad_request";
        return r;
    }

    {
        std::lock_guard<std::mutex> g(jobs_mutex_);
        if(jobs_pending_.size() >= deps_.max_pending_jobs)
        {
            r.error  = "too many pending workspace jobs";
            r.detail = "busy";
            return r;
        }
        Job job;
        job.st.found      = true;
        job.st.job_id     = next_job_id_++;
        job.st.session_id = session_id;
        job.st.verb       = verb;
        job.st.state      = JobState::Queued;
        job.body          = body;
        job.seq           = next_job_seq_++;
        const uint64_t jid = job.st.job_id;
        jobs_[jid] = std::move(job);
        jobs_pending_.push_back(jid);
        r.accepted = true;
        r.job_id   = jid;
    }
    jobs_cv_.notify_one();
    return r;
}

WorkspaceService::JobStatus WorkspaceService::Status(
    uint64_t session_id, uint64_t job_id) const
{
    std::lock_guard<std::mutex> g(jobs_mutex_);
    JobStatus st;
    const auto it = jobs_.find(job_id);
    if(it == jobs_.end() || it->second.st.session_id != session_id)
    {
        return st;     /* found:false — foreign/unknown job */
    }
    return it->second.st;
}

WorkspaceService::JobResult WorkspaceService::Cancel(
    uint64_t session_id, uint64_t job_id)
{
    JobResult r;
    std::lock_guard<std::mutex> g(jobs_mutex_);
    auto it = jobs_.find(job_id);
    if(it == jobs_.end() || it->second.st.session_id != session_id)
    {
        r.error  = "unknown job";
        r.detail = "bad_request";
        return r;
    }
    Job& job = it->second;
    if(job.st.state != JobState::Queued)
    {
        /* a running commit past the durable marker is never
           rolled back; terminal jobs are already done */
        r.error  = "job is not queued";
        r.detail = "bad_request";
        return r;
    }
    job.st.state = JobState::Cancelled;
    jobs_pending_.erase(
        std::remove(jobs_pending_.begin(), jobs_pending_.end(),
                    job_id),
        jobs_pending_.end());
    r.accepted = true;
    r.job_id   = job_id;
    return r;
}

void WorkspaceService::CancelSession(uint64_t session_id)
{
    std::vector<JobStatus> finished;
    {
        std::lock_guard<std::mutex> g(jobs_mutex_);
        for(auto& kv : jobs_)
        {
            Job& job = kv.second;
            if(job.st.session_id == session_id &&
               job.st.state == JobState::Queued)
            {
                job.st.state = JobState::Cancelled;
                job.st.error = "session ended";
                finished.push_back(job.st);
            }
        }
        jobs_pending_.erase(
            std::remove_if(jobs_pending_.begin(),
                           jobs_pending_.end(),
                           [&](uint64_t jid)
                           {
                               const auto it = jobs_.find(jid);
                               return it == jobs_.end() ||
                                      it->second.st.state !=
                                          JobState::Queued;
                           }),
            jobs_pending_.end());
        /* terminal records of a dead session are useless —
           reconcile is by revision, not job id */
        for(auto it = jobs_.begin(); it != jobs_.end();)
        {
            if(it->second.st.session_id == session_id &&
               it->second.st.state != JobState::Queued &&
               it->second.st.state != JobState::Running)
            {
                it = jobs_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
    for(const JobStatus& st : finished)
    {
        JobSurface surface;
        {
            std::lock_guard<std::mutex> g(jobs_mutex_);
            surface = surface_;
        }
        if(surface.job_event)
        {
            surface.job_event(st);
        }
    }
}

void WorkspaceService::WorkerMain()
{
    for(;;)
    {
        uint64_t jid = 0;
        {
            std::unique_lock<std::mutex> l(jobs_mutex_);
            jobs_cv_.wait(l, [&] {
                return jobs_stop_.load() || !jobs_pending_.empty();
            });
            if(jobs_stop_.load())
            {
                return;
            }
            jid = jobs_pending_.front();
            jobs_pending_.pop_front();
            jobs_[jid].st.state = JobState::Running;
        }

        std::function<void(const std::string&)> hook;
        Job  snapshot;
        {
            std::lock_guard<std::mutex> g(jobs_mutex_);
            snapshot = jobs_[jid];
            hook     = pre_job_hook_;
        }
        JobStatus done;
        try
        {
            if(hook)
            {
                hook(snapshot.st.verb);
            }
            done = RunJob(snapshot);
        }
        catch(const std::exception& e)
        {
            /* a throwing job fails honestly — the worker must
               never take the process down */
            done                = snapshot.st;
            done.state          = JobState::Failed;
            done.error          = "job threw";
            done.detail         = e.what();
        }
        catch(...)
        {
            done                = snapshot.st;
            done.state          = JobState::Failed;
            done.error          = "job threw";
            done.detail         = "internal";
        }

        JobSurface surface;
        {
            std::lock_guard<std::mutex> g(jobs_mutex_);
            auto it = jobs_.find(jid);
            if(it != jobs_.end())
            {
                it->second.st = done;
                /* Terminal records stay queryable for
                   reconcile — the request body (a full staged
                   or inline document, up to ~1 MiB) does not:
                   retention keeps status, never payload. */
                it->second.body = nlohmann::json();
            }
            /* bounded retention — oldest terminal records evict */
            while(jobs_.size() > kMaxJobRecords)
            {
                auto oldest = jobs_.end();
                for(auto it = jobs_.begin(); it != jobs_.end(); ++it)
                {
                    const JobState s = it->second.st.state;
                    const bool terminal =
                        s == JobState::Succeeded ||
                        s == JobState::Failed    ||
                        s == JobState::Cancelled;
                    if(terminal &&
                       (oldest == jobs_.end() ||
                        it->second.seq < oldest->second.seq))
                    {
                        oldest = it;
                    }
                }
                if(oldest == jobs_.end())
                {
                    break;
                }
                jobs_.erase(oldest);
            }
            surface = surface_;
        }
        /* the completion EVENT — emitted OFF the job mutex so a
           slow subscriber never stalls the worker */
        if(surface.job_event)
        {
            try { surface.job_event(done); } catch(...) {}
        }
    }
}

WorkspaceService::JobStatus WorkspaceService::RunJob(
    const Job& job)
{
    JobStatus st = job.st;      /* carries id/session/verb    */
    const nlohmann::json& body = job.body;
    const std::string&    verb = job.st.verb;

    auto fail = [&](std::string err, std::string detail)
        -> JobStatus
    {
        st.state  = JobState::Failed;
        st.error  = std::move(err);
        st.detail = std::move(detail);
        return st;
    };
    auto done = [&](const Result& res) -> JobStatus
    {
        st.state              = res.ok ? JobState::Succeeded
                                       : JobState::Failed;
        st.committed          = res.committed;
        st.applied            = res.applied;
        st.recovered          = res.recovered;
        st.generation         = res.generation;
        st.workspace_revision = res.workspace_revision;
        st.resource_revision  = res.resource_revision;
        st.error              = res.error;
        st.detail             = res.detail;
        if(!res.warnings.empty())
        {
            st.result["warnings"] = res.warnings;
            CapReportArray(st.result, "warnings");
        }
        return st;
    };

    JobSurface surface;
    {
        std::lock_guard<std::mutex> g(jobs_mutex_);
        surface = surface_;
    }

    /* The staged payload the job consumes — read on the worker
       thread, never inside the service mutex. */
    auto staged_doc = [&](nlohmann::json& doc, std::string& raw)
        -> std::string
    {
        const auto sit = body.find("staged");
        if(sit == body.end())
        {
            doc = body.value("document", nlohmann::json());
            raw = doc.is_discarded() ? std::string() : doc.dump(2);
            return {};
        }
        if(!sit->is_string())
        {
            return "bad_request";
        }
        if(!surface.read_staged)
        {
            return "staged_unknown";
        }
        std::string err;
        if(!surface.read_staged(sit->get<std::string>(), raw, err))
        {
            return err.empty() ? "staged_unknown" : err;
        }
        doc = nlohmann::json::parse(raw, nullptr, false);
        if(doc.is_discarded() || !doc.is_object())
        {
            return "invalid_document";
        }
        return {};
    };

    if(verb == "validate" || verb == "commit" || verb == "save_copy")
    {
        nlohmann::json doc;
        std::string    raw;
        if(const std::string d = staged_doc(doc, raw); !d.empty())
        {
            return fail(d == "invalid_document"
                            ? "staged bytes are not a JSON object"
                            : d == "staged_unknown"
                                  ? "staged token is not consumable"
                                  : d,
                        d);
        }
        const std::string id = body.value("id", std::string());
        uint64_t base_rev = 0, res_rev = 0;
        JsonU64(body, "base_revision",     base_rev);
        JsonU64(body, "resource_revision", res_rev);

        if(verb == "validate")
        {
            /* Bounded introspection — never commits. The
               candidate form the commit would store is the
               reported validity. */
            WorkspaceCandidate cand;
            std::vector<std::string> errors, warnings;
            {
                std::lock_guard<std::mutex> g(mutex_);
                ParseWorkspaceCandidate(
                    doc, store_.EffectiveDeviceRegistry(),
                    store_.EffectiveEffectRegistry(),
                    cand, errors, warnings);
            }
            st.state = JobState::Succeeded;
            st.result["valid"]    = errors.empty();
            st.result["errors"]   = errors;
            st.result["warnings"] = warnings;
            /* Same 1 MiB reply bound as the import report. */
            CapReportArray(st.result, "errors");
            CapReportArray(st.result, "warnings");
            return st;
        }
        /* commit applies + adopts; save_copy only commits — the
           open workspace is untouched. */
        const Result res = CommitJson(
            id, doc, raw, base_rev, res_rev,
            /*apply_after=*/verb == "commit",
            /*adopt=*/      verb == "commit");
        return done(res);
    }

    if(verb == "import_inspect")
    {
        nlohmann::json doc;
        std::string    raw;
        if(const std::string d = staged_doc(doc, raw); !d.empty())
        {
            return fail("staged bundle is not readable", d);
        }
        std::vector<std::string> errors, warnings;
        nlohmann::json report;
        {
            std::lock_guard<std::mutex> g(mutex_);
            report = InspectCandidateJson(
                doc, store_.EffectiveDeviceRegistry(),
                store_.EffectiveEffectRegistry(),
                errors, warnings);
        }
        st.state   = errors.empty() ? JobState::Succeeded
                                    : JobState::Failed;
        st.error   = errors.empty() ? std::string()
                                    : errors.front();
        st.result  = std::move(report);
        return st;
    }

    if(verb == "import_commit")
    {
        nlohmann::json doc;
        std::string    raw;
        if(const std::string d = staged_doc(doc, raw); !d.empty())
        {
            return fail("staged bundle is not readable", d);
        }
        const std::string id = body.value("id", std::string());
        uint64_t base_rev = 0, res_rev = 0;
        JsonU64(body, "base_revision",     base_rev);
        JsonU64(body, "resource_revision", res_rev);
        const Result res =
            ImportStagedJson(id, doc, raw, base_rev, res_rev);
        return done(res);
    }

    if(verb == "resource_install")
    {
        /* Task 6 personal-look install: ONE validated effect
           document into the managed library — collision-safe
           (never overwrites), revision-gated on BOTH the
           workspace and resource revisions, and returning the
           landing id + refreshed revisions. */
        studio::EffectDocument fx;
        {
            std::vector<std::string> errs;
            if(!studio::EffectDocumentFromJson(
                   body["effect"], fx, &errs))
            {
                return fail(
                    "effect document invalid: " +
                    (errs.empty() ? std::string("bad shape")
                                  : errs.front()),
                    "invalid_document");
            }
        }
        const std::string id = body.value("id", std::string());
        uint64_t base_rev = 0, res_rev = 0;
        JsonU64(body, "base_revision",     base_rev);
        JsonU64(body, "resource_revision", res_rev);

        WorkspaceStore::ResourceInstallResult ir;
        {
            std::lock_guard<std::mutex> g(mutex_);
            ir = store_.InstallEffectResource(id, fx,
                                              base_rev, res_rev);
            if(ir.ok && id == current_.id)
            {
                /* Keep the published snapshot coherent — a
                   landed resource is part of the current
                   workspace's resource set immediately. */
                current_.resource_revision = ir.resource_revision;
                PublishLocked();
            }
        }
        if(!ir.ok)
        {
            Result res;
            res.error = ir.error;
            if(ir.error.find("stale") != std::string::npos ||
               ir.error.find("revision") != std::string::npos)
            {
                res.detail = "revision_conflict";
            }
            res.warnings = ir.warnings;
            return done(res);
        }
        Result res;
        res.ok                = true;
        /* No generation commits — the marker's resource
           revision bumped in place; `committed` stays false
           and the result carries the landing id. */
        res.workspace_revision = ir.workspace_revision;
        res.resource_revision  = ir.resource_revision;
        res.warnings          = ir.warnings;
        st = done(res);
        st.result["landed_id"] = ir.landed_id;
        st.result["reused"]    = ir.reused;
        return st;
    }

    if(verb == "export")
    {
        /* The managed workspace's portable form — sanitized,
           bundled with the resources it actually uses, staged
           for download on the OWNING session. */
        std::shared_ptr<const WorkspaceCandidate> cand;
        {
            std::lock_guard<std::mutex> g(mutex_);
            if(body.contains("id"))
            {
                const WorkspaceStore::LoadResult lr =
                    store_.Load(body.value("id", std::string()));
                if(lr.status == WorkspaceStore::LoadStatus::Loaded ||
                   lr.status == WorkspaceStore::LoadStatus::Recovered)
                {
                    cand = lr.candidate;
                }
            }
            else
            {
                cand = current_.candidate;
            }
        }
        if(!cand)
        {
            return fail("no managed workspace to export",
                        "bad_request");
        }
        nlohmann::json devs = nlohmann::json::array();
        std::vector<std::string> used, uerrs;
        studio::CollectUsedTypes(cand->document,
                                 cand->device_registry,
                                 used, &uerrs);
        for(const std::string& tid : used)
        {
            if(const studio::DevicePreset* p =
                   cand->device_registry.Find(tid))
            {
                devs.push_back(studio::ToJson(*p));
            }
        }
        nlohmann::json effs = nlohmann::json::array();
        if(!cand->document.effect.preset.empty())
        {
            if(const studio::EffectDocument* ed =
                   cand->effect_registry.Find(
                       cand->document.effect.preset))
            {
                effs.push_back(studio::EffectDocumentToJson(*ed));
            }
        }
        const nlohmann::json bundle = nlohmann::json::object({
            { "format",    "openrgb2-workspace-bundle" },
            { "version",   1                           },
            { "workspace", studio::ToJson(
                  studio::SanitizeForExport(cand->document)) },
            { "resources", nlohmann::json::object({
                  { "devices", devs },
                  { "effects", effs } }) },
        });
        const std::string bytes = bundle.dump(2);
        if(!surface.stage_download)
        {
            return fail("download staging is not wired",
                        "internal");
        }
        std::string token, sha, err;
        if(!surface.stage_download(job.st.session_id, "export",
                                   bytes, token, sha, err))
        {
            return fail("export staging failed: " + err,
                        "internal");
        }
        st.state = JobState::Succeeded;
        st.result = nlohmann::json::object({
            { "transfer_id", token                  },
            { "bytes",       (uint64_t)bytes.size() },
            { "sha256",      sha                    },
            { "warnings",    uerrs                  },
        });
        CapReportArray(st.result, "warnings");
        return st;
    }

    if(verb == "reload")
    {
        std::string id = body.value("id", std::string());
        if(id.empty())
        {
            std::lock_guard<std::mutex> g(mutex_);
            id = current_.id;
        }
        if(id.empty())
        {
            return fail("no managed workspace is active",
                        "bad_request");
        }
        Result res = Open(id);
        if(res.ok && deps_.apply_scene)
        {
            std::shared_ptr<const WorkspaceCandidate> cand;
            {
                std::lock_guard<std::mutex> g(mutex_);
                cand = current_.candidate;
            }
            std::string aerr;
            if(cand && cand->resolved &&
               deps_.apply_scene(cand, aerr))
            {
                res.applied = true;
            }
        }
        return done(res);
    }

    return fail("unknown job verb '" + verb + "'", "bad_request");
}

WorkspaceService::Result WorkspaceService::CommitJson(
    const std::string& id, const nlohmann::json& document,
    const std::string& original_bytes,
    uint64_t expected_workspace_revision,
    uint64_t expected_resource_revision,
    bool apply_after, bool adopt)
{
    Result res;
    std::shared_ptr<const WorkspaceCandidate> apply_cand;
    {
        std::lock_guard<std::mutex> g(mutex_);
        if(!WorkspaceStore::IsWorkspaceId(id))
        {
            res.error  = "invalid workspace id '" + id + "'";
            res.detail = "bad_request";
            return res;
        }
        /* Authoring + resource revisions re-checked against the
           durable marker at COMMIT time — a base accepted at
           enqueue must lose when an intervening commit landed. */
        const WorkspaceStore::Revisions rev =
            store_.LoadRevisions(id);
        if(rev.exists && !rev.error.empty())
        {
            res.error  = "workspace marker unreadable: " +
                         rev.error;
            res.detail = "marker_unreadable";
            return res;
        }
        if(rev.workspace_revision != expected_workspace_revision ||
           rev.resource_revision  != expected_resource_revision)
        {
            res.error = "stale revision: expected {" +
                std::to_string(expected_workspace_revision) + "," +
                std::to_string(expected_resource_revision) +
                "} but '" + id + "' is at {" +
                std::to_string(rev.workspace_revision) + "," +
                std::to_string(rev.resource_revision) + "}";
            res.detail = "revision_conflict";
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
                res.error = "document failed to parse as a "
                            "workspace candidate";
            }
            res.detail = "invalid_document";
            return res;
        }
        if(adopt)
        {
            res = CommitCommon(id, cand, original_bytes,
                               expected_workspace_revision,
                               apply_after, &apply_cand);
        }
        else
        {
            /* save_copy — commit under the OTHER id without
               adopting: the current workspace is untouched. */
            const WorkspaceStore::CommitResult cr =
                store_.Commit(id, cand, original_bytes,
                              expected_workspace_revision);
            res.warnings = cr.warnings;
            if(!cr.ok)
            {
                res.error = cr.error;
                if(cr.error.find("revision") != std::string::npos ||
                   cr.error.find("stale")    != std::string::npos)
                {
                    res.detail = "revision_conflict";
                }
            }
            else
            {
                res.ok                 = true;
                res.committed          = true;
                res.generation         = cr.generation;
                res.workspace_revision = cr.workspace_revision;
                res.resource_revision  = cr.resource_revision;
            }
        }
    }
    /* The scene publish runs AFTER the commit lock releases —
       the emitted event's payload re-reads this service (the
       managed flag), so applying under the lock is a same-
       thread self-deadlock. */
    ApplyAfterCommit(apply_cand, res.generation, res);
    return res;
}

WorkspaceService::Result WorkspaceService::ImportStagedJson(
    const std::string& id, const nlohmann::json& staged,
    const std::string& original_bytes,
    uint64_t expected_workspace_revision,
    uint64_t expected_resource_revision)
{
    std::lock_guard<std::mutex> g(mutex_);
    Result res;
    if(!WorkspaceStore::IsWorkspaceId(id))
    {
        res.error  = "invalid workspace id '" + id + "'";
        res.detail = "bad_request";
        return res;
    }
    const WorkspaceStore::Revisions rev = store_.LoadRevisions(id);
    if(rev.exists && !rev.error.empty())
    {
        res.error  = "workspace marker unreadable: " + rev.error;
        res.detail = "marker_unreadable";
        return res;
    }
    if(rev.workspace_revision != expected_workspace_revision ||
       rev.resource_revision  != expected_resource_revision)
    {
        res.error  = "stale revision for '" + id + "'";
        res.detail = "revision_conflict";
        return res;
    }

    /* Bundle form: {format, workspace:{...}, resources:{devices,
       effects}} — a bare compact document is equally valid. */
    nlohmann::json doc = staged;
    std::map<std::string, studio::DevicePreset>   bundled_dev;
    std::map<std::string, studio::EffectDocument> bundled_fx;
    studio::PresetRegistry devreg = store_.EffectiveDeviceRegistry();
    studio::EffectRegistry effreg = store_.EffectiveEffectRegistry();
    const bool bundle = staged.is_object() &&
                        staged.contains("workspace") &&
                        staged["workspace"].is_object();
    if(bundle)
    {
        doc = staged["workspace"];
        const nlohmann::json resj =
            staged.value("resources", nlohmann::json::object());
        for(const nlohmann::json& pj :
                resj.value("devices", nlohmann::json::array()))
        {
            studio::DevicePreset p;
            std::vector<std::string> perrs;
            if(!studio::DevicePresetFromJson(pj, p, &perrs))
            {
                res.error  = "bundled device preset invalid: " +
                             (perrs.empty() ? "?" : perrs.front());
                res.detail = "invalid_document";
                return res;
            }
            bundled_dev[p.id] = p;
        }
        for(const nlohmann::json& ej :
                resj.value("effects", nlohmann::json::array()))
        {
            studio::EffectDocument d;
            std::vector<std::string> eerrs;
            if(!studio::EffectDocumentFromJson(ej, d, &eerrs))
            {
                res.error  = "bundled effect look invalid: " +
                             (eerrs.empty() ? "?" : eerrs.front());
                res.detail = "invalid_document";
                return res;
            }
            bundled_fx[d.id] = d;
        }
        /* Scratch overlay — bundled types resolve inter-type
           refs the same way the file layer does. */
        std::vector<studio::DevicePreset> todo;
        for(const auto& kv : bundled_dev)
        {
            todo.push_back(kv.second);
        }
        bool grew = true;
        while(grew && !todo.empty())
        {
            grew = false;
            for(auto it = todo.begin(); it != todo.end();)
            {
                std::vector<std::string> aerrs;
                if(devreg.Add(*it, &aerrs))
                {
                    it   = todo.erase(it);
                    grew = true;
                }
                else
                {
                    ++it;
                }
            }
        }
        if(!todo.empty())
        {
            res.error  = "bundled device presets have "
                         "unresolvable dependencies";
            res.detail = "invalid_document";
            return res;
        }
        for(const auto& kv : bundled_fx)
        {
            effreg.Add(kv.second, nullptr);
        }
    }

    WorkspaceCandidate cand;
    std::vector<std::string> errors;
    if(!ParseWorkspaceCandidate(doc, devreg, effreg, cand,
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
            res.error = "staged content failed to parse as a "
                        "workspace candidate";
        }
        res.detail = "invalid_document";
        return res;
    }

    /* Bundled types/looks the document actually uses become
       extracted resources — the commit installs them with
       collision-aware rename + reference remap. */
    {
        std::set<std::string> have;
        for(const studio::DevicePreset& p : cand.extracted_types)
        {
            have.insert(p.id);
        }
        std::vector<std::string> used, uerrs;
        studio::CollectUsedTypes(cand.document, cand.device_registry,
                                 used, &uerrs);
        for(const std::string& tid : used)
        {
            if(have.count(tid) || !bundled_dev.count(tid))
            {
                continue;
            }
            cand.extracted_types.push_back(bundled_dev[tid]);
            have.insert(tid);
        }
        if(bundled_fx.count(cand.document.effect.preset))
        {
            bool have_fx = false;
            for(const studio::EffectDocument& d :
                    cand.extracted_effects)
            {
                have_fx |= d.id == cand.document.effect.preset;
            }
            if(!have_fx)
            {
                cand.extracted_effects.push_back(
                    bundled_fx[cand.document.effect.preset]);
            }
        }
    }

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
    return CommitCommon(id, cand, original_bytes,
                        expected_workspace_revision,
                        /*apply_after=*/false, nullptr);
}

nlohmann::json WorkspaceService::InspectCandidateJson(
    const nlohmann::json& document_or_bundle,
    const studio::PresetRegistry& devices,
    const studio::EffectRegistry& effects,
    std::vector<std::string>& errors,
    std::vector<std::string>& warnings)
{
    nlohmann::json out = nlohmann::json::object({
        { "valid",     false                 },
        { "conflicts", nlohmann::json::array() },
        { "missing",   nlohmann::json::array() },
        { "warnings",  nlohmann::json::array() },
        { "errors",    nlohmann::json::array() },
    });

    nlohmann::json doc = document_or_bundle;
    studio::PresetRegistry devreg = devices;
    studio::EffectRegistry effreg = effects;
    const bool bundle = document_or_bundle.is_object() &&
                        document_or_bundle.contains("workspace") &&
                        document_or_bundle["workspace"].is_object();
    if(bundle)
    {
        doc = document_or_bundle["workspace"];
        const nlohmann::json resj =
            document_or_bundle.value("resources",
                                     nlohmann::json::object());
        for(const nlohmann::json& pj :
                resj.value("devices", nlohmann::json::array()))
        {
            studio::DevicePreset p;
            std::vector<std::string> perrs;
            if(!studio::DevicePresetFromJson(pj, p, &perrs))
            {
                errors.push_back("bundled device preset invalid: " +
                                 (perrs.empty() ? "?" : perrs.front()));
                continue;
            }
            /* new | identical | conflict — the inspect contract
               the client resolves BEFORE committing. */
            const studio::DevicePreset* local = devreg.Find(p.id);
            std::string status = "new";
            if(local)
            {
                status = studio::ToJson(*local) == studio::ToJson(p)
                             ? "identical" : "conflict";
            }
            out["conflicts"].push_back(nlohmann::json::object({
                { "id",     p.id   },
                { "status", status },
            }));
            devreg.Add(p, nullptr);
        }
        for(const nlohmann::json& ej :
                resj.value("effects", nlohmann::json::array()))
        {
            studio::EffectDocument d;
            if(studio::EffectDocumentFromJson(ej, d, nullptr))
            {
                effreg.Add(d, nullptr);
            }
        }
    }

    WorkspaceCandidate cand;
    std::vector<std::string> perrs;
    if(!ParseWorkspaceCandidate(doc, devreg, effreg, cand,
                                perrs, warnings))
    {
        for(const std::string& e : perrs)
        {
            errors.push_back(e);
        }
    }
    else
    {
        out["valid"] = true;
        out["name"]  = cand.document.meta.name;
        /* types the document uses but no layer resolves */
        std::vector<std::string> used, uerrs;
        studio::CollectUsedTypes(cand.document, cand.device_registry,
                                 used, &uerrs);
        for(const std::string& e : uerrs)
        {
            out["missing"].push_back(e);
        }
    }
    out["errors"]   = errors;
    out["warnings"] = warnings;
    /* Every array field is capped — the report rides the 1 MiB
       job-result envelope; a pathological bundle truncates with
       an honest dropped count, never an oversized reply. */
    for(const char* key :
            { "conflicts", "missing", "warnings", "errors" })
    {
        CapReportArray(out, key);
    }
    return out;
}

bool WorkspaceService::DocumentBytes(const std::string& id,
                                     std::string& bytes,
                                     std::string& err) const
{
    /* Raw document.json — byte-identical to re-serializing a
       loaded candidate, but a marker + single file read rather
       than a full manifest-verify + registry-overlay + parse.
       Reader-thread metadata stays bounded and cheap; the IPC
       layer caches per revision on top of this. */
    return store_.ReadActiveDocumentBytes(id, bytes, err);
}

/*---------------------------------------------------------*\
||| Job serialization (the wire shares one shape)             ||
\*---------------------------------------------------------*/

const char* JobStateName(WorkspaceService::JobState s)
{
    switch(s)
    {
    case WorkspaceService::JobState::Queued:    return "queued";
    case WorkspaceService::JobState::Running:   return "running";
    case WorkspaceService::JobState::Succeeded: return "succeeded";
    case WorkspaceService::JobState::Failed:    return "failed";
    case WorkspaceService::JobState::Cancelled: return "cancelled";
    }
    return "?";
}

nlohmann::json JobStatusToJson(const WorkspaceService::JobStatus& st)
{
    nlohmann::json out = nlohmann::json::object({
        { "found",              st.found                  },
        { "job_id",             st.job_id                 },
        { "session_id",         st.session_id             },
        { "verb",               st.verb                   },
        { "state",              JobStateName(st.state)    },
        { "committed",          st.committed              },
        { "applied",            st.applied                },
        { "recovered",          st.recovered              },
        { "generation",         st.generation             },
        { "workspace_revision", st.workspace_revision     },
        { "resource_revision",  st.resource_revision      },
        { "result",             st.result                 },
    });
    if(!st.error.empty())
    {
        out["error"] = st.error;
    }
    if(!st.detail.empty())
    {
        out["detail"] = st.detail;
    }
    return out;
}

} /* namespace openrgb2 */
