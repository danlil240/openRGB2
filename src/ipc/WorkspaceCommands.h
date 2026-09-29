/*---------------------------------------------------------*\
||| WorkspaceCommands.h                                       ||
|||                                                           ||
|||   The WORKSPACE_COMMAND verb surface (studio-import    ||
|||   Task 3, plan section 3.3; docs/ipc-protocol.md).     ||
|||   Thin layer between the IPC session machinery and     ||
|||   WorkspaceService: handlers only VALIDATE, enqueue,   ||
|||   or read snapshots — file work runs on the service's  ||
|||   single bounded job worker and staging lives in       ||
|||   WorkspaceTransfer (disk staging inside the managed   ||
|||   root, never under a client-chosen path).             ||
|||                                                           ||
|||   Verb families (plan 3.3 verbatim):                   ||
|||   - get / list / catalog: metadata + bounded pages;    ||
|||     document/type/look content downloads ride the      ||
|||     transfer mechanism                                 ||
|||   - validate / commit / save_copy: staged-candidate    ||
|||     jobs carrying base_revision + resource_revision;   ||
|||     stale -> job fails with detail "revision_conflict" ||
|||   - import_inspect / import_commit / export / reload:  ||
|||     async jobs                                         ||
|||   - transfer_begin / transfer_chunk / transfer_finish  ||
|||     / transfer_cancel: session-bound bounded staging   ||
|||   - job_status / job_cancel: poll/cancel by id within  ||
|||     the owning session                                 ||
|||   - preview_begin / preview_update / preview_end:     ||
|||     bounded draft preview (Task 7, plan 3.4) — one    ||
|||     PreviewSession per IPC session, a global two-     ||
|||     session bound, 30 Hz chunked publishing to the    ||
|||     owning session only. NO hardware path exists in   ||
|||     this code — the preview feed is pure evaluation.  ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "IpcService.h"
#include "WorkspaceTransfer.h"

#include "PreviewSession.h"
#include "PowerLifecycle.h"
#include "ReactiveInputs.h"
#include "WorkspaceService.h"
#include "presets/PresetRegistry.h"
#include "presets/EffectRegistry.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace openrgb2
{
namespace ipc
{

class WorkspaceCommands
{
public:
    /* `service` outlives this object (EngineService holds the
       WorkspaceService pointer as a dep). Transfer bounds are
       injectable so the offline suite exercises expiry and
       caps in milliseconds/kibibytes. */
    WorkspaceCommands(WorkspaceService* service,
                      WorkspaceTransfer::Options transfer_options =
                          WorkspaceTransfer::Options{});

    /* WORKSPACE_COMMAND dispatch — runs on the session's
       reader thread; handlers never take WorkspaceService's
       commit mutex. */
    IpcReply       Handle(uint64_t session_id,
                          const nlohmann::json& body);

    /* The "workspace" event kind's initial-snapshot body —
       metadata only, same shape as the `get` reply. */
    nlohmann::json SnapshotJson() const;

    /* Session teardown: cancel the session's queued jobs and
       drop its staged transfers (a commit past the durable
       marker boundary is unaffected — reconnect reconciles by
       revision/content hash). */
    void           OnSessionEnd(uint64_t session_id);

    /* Idle-transfer expiry — the owner loop's PollEvents calls
       this; Handle() also sweeps lazily so expiry never needs
       a ticking caller. */
    void           SweepExpired();

    /*-----------------------------------------------------*\
    || studio-import Task 7 — bounded draft preview.        ||
    ||                                                      ||
    ||   EngineService wires these deps once the event     ||
    ||   sink exists. While deps.emit is empty the         ||
    ||   preview_* verbs keep answering Unavailable —      ||
    ||   honestly, never half-active.                       ||
    \*-----------------------------------------------------*/
    struct PreviewDeps
    {
        /* Read-only topology for candidate application —
           the same provider EngineService's layout surface
           uses. */
        std::function<std::vector<DeviceSnapshot>()> snapshots;
        /* Display-off gating: nullptr = display always on. */
        const PowerLifecycle*  power    = nullptr;
        /* Transient input snapshots ONLY — preview consumes
           Snapshot() and NEVER starts/enables a source. */
        ReactiveInputs*        reactive = nullptr;
        /* The output clock's epoch: preview samples the SAME
           scene-time origin the worker uses, so a draft and
           its committed form animate identically. */
        std::function<TimePoint()>     scene_epoch;
        /* Targeted delivery: (session_id, event, coalesce
           key) -> IpcServer::EmitTo. */
        std::function<void(uint64_t, const IpcEvent&,
                           const std::string&)> emit;

        size_t   max_sessions        = 2;
        uint32_t min_interval_ms     = 33;  /* 30 Hz cap    */
        size_t   chunk_bytes         = 768u * 1024u;
        size_t   display_budget_leds = 4096;
    };
    void           SetPreviewDeps(PreviewDeps deps);

    /* The owner-loop publish tick — called from
       EngineService::PollFrameStatus (~20 ms cadence); the
       per-session 30 Hz cap lives inside. Bounded work per
       call: one Sample per due session. */
    void           PollPreviews(TimePoint now = Clock::now());

    /* The "preview" event's session-bound snapshot — always a
       status record; {active:false} for a session with no
       preview. */
    nlohmann::json PreviewStatusJson(uint64_t session_id) const;

    /* Job-surface wiring consumed by EngineService (the job
       worker reads staged uploads and stages downloads
       through these). */
    bool ReadStaged(const std::string& token,
                    std::string& bytes, std::string& err);
    bool StageDownload(uint64_t session_id, const std::string& kind,
                       const std::string& bytes,
                       std::string& token, std::string& err);

private:
    IpcReply HandleGet(uint64_t session_id,
                       const nlohmann::json& body);
    IpcReply HandleList(const nlohmann::json& body);
    IpcReply HandleCatalog(const nlohmann::json& body);
    IpcReply HandleJobVerb(uint64_t session_id,
                           const std::string& verb,
                           const nlohmann::json& body);
    IpcReply HandleTransferBegin(uint64_t session_id,
                                 const nlohmann::json& body);
    IpcReply HandleTransferChunk(uint64_t session_id,
                                 const nlohmann::json& body);
    IpcReply HandleTransferFinish(uint64_t session_id,
                                  const nlohmann::json& body);
    IpcReply HandleTransferCancel(uint64_t session_id,
                                  const nlohmann::json& body);
    IpcReply HandleJobStatus(uint64_t session_id,
                             const nlohmann::json& body);
    IpcReply HandleJobCancel(uint64_t session_id,
                             const nlohmann::json& body);
    IpcReply HandlePreviewBegin(uint64_t session_id,
                                const nlohmann::json& body);
    IpcReply HandlePreviewUpdate(uint64_t session_id,
                                 const nlohmann::json& body);
    IpcReply HandlePreviewEnd(uint64_t session_id);

    /* Reader-side caches — handlers must not re-run file work
       per request (plan 3.3). The library cache keys on the
       managed library dirs' write times (a catalog never
       re-parses a unchanged library), the workspace list on
       the root dir's write time (the id set only changes when
       an id dir appears/vanishes), and document bytes on the
       durable marker revisions. Each rebuilds once per
       observed change; all are shared_ptr snapshots so a
       concurrent rebuild never mutates an in-flight reply. */
    std::shared_ptr<const studio::PresetRegistry>
                         CachedDevices() const;
    std::shared_ptr<const studio::EffectRegistry>
                         CachedEffects() const;
    std::vector<std::string>
                         CachedList() const;
    std::shared_ptr<const std::string>
                         CachedDocumentBytes(
                             const std::string& id,
                             uint64_t workspace_revision,
                             uint64_t resource_revision,
                             std::string& err) const;

    WorkspaceService*        service_;
    WorkspaceTransfer        transfers_;

    mutable std::mutex       cache_mutex_;
    mutable uint64_t         lib_dev_mtime_ = 0;
    mutable uint64_t         lib_eff_mtime_ = 0;
    mutable std::shared_ptr<const studio::PresetRegistry>
                             lib_dev_;
    mutable std::shared_ptr<const studio::EffectRegistry>
                             lib_eff_;
    mutable uint64_t         list_mtime_ = 0;
    mutable std::vector<std::string> list_ids_;
    struct DocCacheEntry
    {
        uint64_t    workspace_revision = 0;
        uint64_t    resource_revision  = 0;
        uint64_t    seq                = 0;
        std::shared_ptr<const std::string> bytes;
    };
    mutable std::map<std::string, DocCacheEntry> doc_cache_;
    mutable uint64_t         doc_cache_seq_ = 1;
    /* Document cache bound: 4 entries x the 4 MiB document cap. */
    static constexpr size_t kMaxDocCacheEntries = 4;

    /* Task 7 preview state — one PreviewSession per IPC
       session, bounded by PreviewDeps::max_sessions. The map
       mutex guards membership only; a PreviewSession's own
       mutex guards its slots (Sample runs under this lock —
       bounded, single-owner-thread). */
    struct PreviewSlot
    {
        std::unique_ptr<PreviewSession> session;
        TimePoint    last_emit{};              /* 30 Hz cap    */
        std::string  paused_reason;            /* "" = running */
        std::string  reported_error;           /* dedupe       */
    };
    PreviewDeps                       preview_deps_;
    mutable std::mutex                preview_mutex_;
    std::map<uint64_t, PreviewSlot>   previews_;

    /* Shared tail of begin/update: parse the document body
       (inline or staged-upload token) into an immutable
       candidate the session then owns read-only. */
    IpcReply PreviewCandidate(uint64_t session_id,
                              const nlohmann::json& body,
                              std::shared_ptr<WorkspaceCandidate>& out,
                              uint64_t& draft_revision);
};

} /* namespace ipc */
} /* namespace openrgb2 */
