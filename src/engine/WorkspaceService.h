/*---------------------------------------------------------*\
|||| WorkspaceService.h                                      ||
||||                                                           |
||||   The engine-internal open/save/import surface over   ||
||||   the managed workspace store (studio-import Task 2,  ||
||||   plan section 3.2). THE ONLY COMPONENT that commits  ||
||||   accepted generations — every workspace-changing     ||
||||   operation funnels through here so revision checks,  ||
||||   sanitizing, and the serialized commit boundary are  ||
||||   impossible to skip.                                 ||
||||                                                           |
||||   Surface:                                             ||
||||   - Open(id):   load the workspace's active           ||
||||     generation and make it current; recovers the      ||
||||     last-valid generation honestly (a recovery is an  ||
||||     ok result with the failure reported in warnings)  ||
||||   - Save(id, doc, expected_rev):  author/save — the   ||
||||     document is committed verbatim (its authored      ||
||||     arming/verified state is the user's own), then    ||
||||     the resolved scene is published through the       ||
||||     injected apply_scene seam                         ||
||||   - Import(id, doc, expected_rev): untrusted-source   ||
||||     path — the candidate is sanitized FIRST (no      ||
||||     live-on-startup, no input capture, no verified    ||
||||     ownership), committed, and NEVER auto-applied     ||
||||   - ImportFile(id, path, expected_rev): Import with   ||
||||     the file's original bytes retained verbatim in    ||
||||     the generation, plus sibling presets/devices and  ||
||||     presets/effects directories consulted as scratch  ||
||||     registries (and installed into the managed        ||
||||     library when the workspace actually references    ||
||||     them)                                             ||
||||                                                           |
||||   All operations are revision-checked against the     ||
||||   durable marker — a stale writer is refused before   ||
||||   anything is staged. Everything is serialized on a   ||
||||   mutex; the store's crash contract does the rest.    ||
||||                                                           |
||||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "WorkspaceStore.h"

#include "config/WorkspaceCandidate.h"
#include "scene/SceneTypes.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace openrgb2
{

class WorkspaceService
{
public:
    /*---- Workspace jobs (studio-import Task 3, plan 3.3) ----\
    | One bounded worker runs every file-touching operation;   |
    | the IPC handler layer only validates + enqueues.         |
    \*---------------------------------------------------------*/
    enum class JobState
    {
        Queued, Running, Succeeded, Failed, Cancelled
    };

    /* Enqueue/Cancel return: accepted carries the accepted job
       id; a rejection carries the human reason plus a stable
       machine detail token ("busy", "bad_request"). */
    struct JobResult
    {
        bool        accepted = false;
        uint64_t    job_id   = 0;
        std::string error;
        std::string detail;
    };

    /* Serializable job status — the job_status reply body AND
       the "workspace" EVENT payload are JobStatusToJson of
       this (docs/ipc-protocol.md). */
    struct JobStatus
    {
        bool        found      = false;
        uint64_t    job_id     = 0;
        /* Session binding — a job_status poll or EVENT for a
           foreign session reports found:false; reconnect
           reconciles by workspace revision / content hash. */
        uint64_t    session_id = 0;
        std::string verb;
        JobState    state      = JobState::Queued;
        bool        committed = false;
        bool        applied   = false;
        bool        recovered = false;
        uint64_t    generation         = 0;
        uint64_t    workspace_revision = 0;
        uint64_t    resource_revision  = 0;
        nlohmann::json result = nlohmann::json::object();
        std::string error;
        /* Stable machine token: "revision_conflict",
           "invalid_document", "staged_unknown", ... */
        std::string detail;
    };

    struct Deps
    {
        /* Base libraries the managed root overlays. */
        studio::PresetRegistry devices;
        studio::EffectRegistry effects;
        /* Post-commit publish seam — nullptr is legal (the save
           still commits; `applied` reports false). Runs on the
           caller's thread AFTER the marker moved and AFTER the
           service mutex is released (a scene apply emits events
           whose payloads re-enter this service for the managed
           flag — running it under the lock self-deadlocks); a
           failure is reported honestly — the committed
           generation stays durable. */
        /* Task 6: the committed CANDIDATE is handed over, not
           just its resolved document — scene preparation needs
           the authoring document's inputs (decay) and the
           registry snapshot for runtime layer resolution. */
        std::function<bool(
            std::shared_ptr<const WorkspaceCandidate>,
            std::string&)> apply_scene;
        /* Pending-job bound across ALL clients (plan 3.3: at
           most eight). */
        uint32_t max_pending_jobs = 8;
    };

    /* The session-bound staging surface the IPC layer supplies:
       staged upload reads + download staging + the job-event
       emitter. Set once before Enqueue is used (the IPC
       WorkspaceCommands wires it on construction). */
    struct JobSurface
    {
        /* staged token -> committed bytes (worker thread;
           bounded by the declared transfer kind). */
        std::function<bool(const std::string& token,
                           std::string& bytes,
                           std::string& err)> read_staged;
        /* session-bound download staging (export results):
           returns the opaque transfer token + the content
           SHA-256 the client verifies the download against. */
        std::function<bool(uint64_t session_id,
                           const std::string& kind,
                           const std::string& bytes,
                           std::string& token,
                           std::string& sha256_hex,
                           std::string& err)> stage_download;
        /* Terminal-state emitter — invoked OFF the job mutex
           on the worker thread (IPC event fanout hooks here). */
        std::function<void(const JobStatus&)> job_event;
    };

    WorkspaceService(std::string root, Deps deps);
    ~WorkspaceService();   /* Shutdown() */
    /* Idempotent worker stop + join. EngineHost runs it inside
       OrderlyShutdown — BEFORE EngineService teardown — so an
       in-flight job can never call deps_.apply_scene on a
       destructing IPC service. */
    void      Shutdown();

    struct Result
    {
        bool        ok                 = false;
        bool        committed          = false; /* generation landed */
        bool        applied            = false; /* resolved published */
        bool        recovered          = false; /* LKG path taken     */
        uint64_t    generation         = 0;
        uint64_t    workspace_revision = 0;
        uint64_t    resource_revision  = 0;
        std::string error;
        /* Stable machine token for wire surfaces
           ("revision_conflict" for a stale base/resource
           revision) — empty means "unclassified". */
        std::string detail;
        std::vector<std::string> warnings;
    };

    Result Open(const std::string& id);
    Result Save(const std::string& id,
                const nlohmann::json& document,
                uint64_t expected_workspace_revision);
    Result Import(const std::string& id,
                  const nlohmann::json& document,
                  uint64_t expected_workspace_revision);
    Result ImportFile(const std::string& id,
                      const std::string& path,
                      uint64_t expected_workspace_revision);

    /* Current-workspace introspection (empty until an
       Open/Save/Import adopts one). */
    const WorkspaceStore& Store() const { return store_; }
    /* Coherent, commit-free read of the current workspace —
       refreshed at each commit boundary inside snap_mutex_
       (held for the field copy only, NEVER across store I/O),
       so metadata reads and the legacy-mutation gate never
       stall behind a multi-MiB worker commit (plan 3.3:
       handlers read snapshots). */
    struct PublishedState
    {
        bool        managed            = false;
        std::string id;
        std::string name;
        std::string source_form;
        uint64_t    workspace_revision = 0;
        uint64_t    resource_revision  = 0;
        uint64_t    generation         = 0;
    };
    PublishedState Snapshot() const;
    /* Worker/owner paths only — takes the commit mutex. IPC
       metadata reads use Snapshot() instead. */
    std::shared_ptr<const WorkspaceCandidate> CurrentCandidate() const;
    std::string  CurrentId()          const;
    uint64_t     WorkspaceRevision()  const;
    uint64_t     ResourceRevision()   const;
    uint64_t     CurrentGeneration()  const;
    /* The deterministic document bytes for `id` (identical to
       what a document download streams — the store writes
       ToJson(document).dump(2)); the IPC layer hashes these for
       get.content_sha256. Runs file I/O on the caller's
       thread — never from the job worker's own commit path. */
    bool         DocumentBytes(const std::string& id,
                               std::string& bytes,
                               std::string& err) const;
    /* A managed workspace is ACTIVE once an id is adopted —
       the IPC surface uses this to refuse lossy legacy scene
       mutations (plan 3.3: detail "workspace_required"). */
    bool         ManagedActive()      const;

    /*---- External-change detection (studio-import Task 10) --*\
    |  CheckExternal re-scans the active generation's tracked   |
    |  files and compares against the signature captured at the |
    |  last adopt/commit. state is "none" | "modified" |        |
    |  "deleted" — REPORTED ONLY: detection never loads, never  |
    |  writes, and NEVER reaches deps_.apply_scene. A           |
    |  byte-identical rewrite reproduces the same signature     |
    |  (same-content suppression). A commit racing the scan     |
    |  simply reports "none" this round.                        |
    \*---------------------------------------------------------*/
    struct ExternalCheck
    {
        bool        managed = false;
        std::string state;      /* "none" when clean/absent */
        std::string detail;
    };
    ExternalCheck CheckExternal();

    /* Restore Backup (job verb "restore_backup"): loads the
       generation the marker BACKUP (active.lkg) names and
       commits it forward as a NEW immutable generation —
       revision-gated like every commit, never write-in-place.
       The restored candidate applies through the normal
       post-commit seam (this is an explicit user action). */
    Result RestoreBackup(const std::string& id,
                         uint64_t expected_workspace_revision,
                         uint64_t expected_resource_revision);

    /*---- Job queue (session-bound; plan 3.3) --------------
       Enqueue validates cheaply (verb, required fields, the
       pending bound) and returns the accepted job id — job
       acceptance is NOT a successful save; revisions are
       re-checked when the worker commits. Status polls by
       (session, job id) — jobs are session-bound, so a
       foreign session sees found:false and a reconnecting
       client reconciles through the workspace revision /
       content hash instead. CancelSession drops the session's
       QUEUED jobs (a running commit past the atomic marker
       stays durable — never falsely rolled back). */
    void      SetJobSurface(JobSurface surface);
    /* Terminal-job event hook — the IPC layer wires Emit
       here (the worker invokes it OFF the job mutex so a
       slow subscriber never stalls workspace work). */
    void      SetJobEventHook(
                  std::function<void(const JobStatus&)> hook);
    /* Instrumentation seam for the offline suite: invoked on
       the worker before each job runs. Production never sets
       it. */
    void      SetPreJobHookForTesting(
                  std::function<void(const std::string& verb)> hook);
    JobResult Enqueue(uint64_t session_id,
                      const std::string& verb,
                      const nlohmann::json& body);
    JobStatus Status(uint64_t session_id, uint64_t job_id) const;
    JobResult Cancel(uint64_t session_id, uint64_t job_id);
    void      CancelSession(uint64_t session_id);

private:
    /* The serialized commit boundary — every mutation runs
       under this mutex. */
    struct Current
    {
        std::string                               id;
        std::shared_ptr<const WorkspaceCandidate> candidate;
        uint64_t workspace_revision = 0;
        uint64_t resource_revision  = 0;
        uint64_t generation         = 0;
        /* External-change baseline — the active generation's
           signature captured by ExternalScan at adopt/commit.
           CheckExternal compares a fresh scan against it; a
           byte-identical rewrite reproduces the same token. */
        std::string external_sig;
    };
    /* Serialized-commit core — CALLER HOLDS mutex_. When
       apply_after is set the committed candidate is handed back
       through apply_cand (never applied inside the lock — the
       apply emits events whose payloads re-enter this
       service). */
    Result CommitCommon(const std::string& id,
                        WorkspaceCandidate& cand,
                        const std::string& original_bytes,
                        uint64_t expected_rev,
                        bool apply_after,
                        std::shared_ptr<const WorkspaceCandidate>*
                            apply_cand);
    /* Runs deps_.apply_scene OFF the service mutex and folds
       the outcome into `res` (committed stays durable; an
       apply failure is reported, never rolled back). */
    void   ApplyAfterCommit(
             const std::shared_ptr<const WorkspaceCandidate>& cand,
             uint64_t generation, Result& res);
    /* IPC job paths (worker thread): the workspace AND resource
       revisions are re-checked against the durable marker at
       commit time — acceptance at enqueue is never a successful
       save (plan 3.3). `adopt` distinguishes commit (becomes
       current + applies) from save_copy (commits only). */
    Result CommitJson(const std::string& id,
                      const nlohmann::json& document,
                      const std::string& original_bytes,
                      uint64_t expected_workspace_revision,
                      uint64_t expected_resource_revision,
                      bool apply_after,
                      bool adopt);
    /* Parse + sanitize staged import bytes then commit — never
       applies. Three staged shapes are accepted:
         - packed bundle  {format:"openrgb2-bundle-files",
                           files:{rel:{encoding,data}}} — unpacked
           into the workspace's staging dir, run through
           studio::InspectBundle + ApplyImport INSIDE staging,
           and only then committed (assets land under
           <root>/assets via the candidate's extracted_assets);
         - flat bundle    {format:"openrgb2-workspace-bundle",
                           workspace:{...}, resources:{...}};
         - bare document  a compact v3 workspace.
       `choices` carries the conflict-resolution map ApplyImport
       needs (conflict id -> import-as id); `mode` may request
       "preset_import" (types-only bundle: installs the types
       into the managed library WITHOUT touching the active
       document). Bundled types/looks become extracted
       resources (the store's collision-aware install owns
       rename/remap). */
    Result ImportStagedJson(const std::string& id,
                            const nlohmann::json& bytes_json,
                            const std::string& original_bytes,
                            const std::map<std::string,
                                           std::string>& choices,
                            const std::string& mode,
                            uint64_t expected_workspace_revision,
                            uint64_t expected_resource_revision);
    /* Packed-bundle inspection (worker path) — unpacks the
       staged files into a scratch dir inside the managed root
       and runs studio::InspectBundle; report carries per-type
       status, conflict ids + suggested import ids, bundled
       assets, warnings, and the same bounded arrays the flat
       path reports. Never writes outside the scratch dir. */
    nlohmann::json InspectPackedBundle(
        const nlohmann::json& bundle_doc,
        std::vector<std::string>& errors,
        std::vector<std::string>& warnings);
    /* Bounded candidate introspection (the worker's
       validate/import_inspect path — never commits). */
    static nlohmann::json InspectCandidateJson(
        const nlohmann::json& document_or_bundle,
        const studio::PresetRegistry& devices,
        const studio::EffectRegistry& effects,
        std::vector<std::string>& errors,
        std::vector<std::string>& warnings);

    /*---- Job worker state (jobs_mutex_) --------------------*/
    struct Job
    {
        JobStatus      st;
        nlohmann::json body;
        uint64_t       seq = 0;      /* eviction order         */
    };
    void      WorkerMain();
    /* The job dispatcher — worker thread only; returns the
       terminal status the worker records + emits. */
    JobStatus RunJob(const Job& job);

    mutable std::mutex      jobs_mutex_;
    std::condition_variable jobs_cv_;
    std::deque<uint64_t>    jobs_pending_;
    std::map<uint64_t, Job> jobs_;        /* live + terminal     */
    uint64_t                next_job_id_  = 1;
    uint64_t                next_job_seq_ = 1;
    std::thread             worker_;
    std::atomic<bool>       jobs_stop_{ false };
    JobSurface              surface_;     /* under jobs_mutex_   */
    std::function<void(const std::string&)> pre_job_hook_;
    /* Terminal-job retention bound — recent completions stay
       queryable for lost-event reconcile; oldest terminal
       records evict past the cap. */
    static constexpr size_t kMaxJobRecords = 64;

    WorkspaceStore  store_;
    Deps            deps_;
    mutable std::mutex mutex_;
    Current            current_;
    /* Copy current_ -> snap_ for lock-light readers —
       CALLER HOLDS mutex_. snap_mutex_ is only ever held for
       the field copy: the worker never stores I/O under it,
       so readers contend on microseconds, not commits. */
    void                PublishLocked();
    mutable std::mutex  snap_mutex_;
    PublishedState      snap_;
};

const char*        JobStateName(WorkspaceService::JobState s);
nlohmann::json     JobStatusToJson(
                       const WorkspaceService::JobStatus& st);

} /* namespace openrgb2 */
