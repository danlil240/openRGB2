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
|||   - preview_begin / preview_update / preview_end: the  ||
|||     stable surface is defined now and answers          ||
|||     Unavailable until the Task-7 preview engine lands  ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "IpcService.h"
#include "WorkspaceTransfer.h"

#include "WorkspaceService.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

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

    WorkspaceService*        service_;
    WorkspaceTransfer        transfers_;
};

} /* namespace ipc */
} /* namespace openrgb2 */
