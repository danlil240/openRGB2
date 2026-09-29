// SPDX-License-Identifier: GPL-2.0-or-later
//
// WorkspaceCommands — the WORKSPACE_COMMAND verb surface
// (studio-import Task 3, plan section 3.3). Handlers run on the
// session's reader thread and may only VALIDATE, ENQUEUE, or
// READ SNAPSHOTS: every file-touching workspace operation is a
// job on WorkspaceService's single bounded worker; large
// content moves through WorkspaceTransfer's bounded,
// session-bound, server-named disk staging.
//
// Wire details live in docs/ipc-protocol.md. The stable
// contract points exercised here:
//   - staged candidate verbs carry base_revision +
//     resource_revision; a stale base fails the JOB with
//     detail "revision_conflict" — never silently wins
//   - every failure carries a stable machine `detail` token
//   - jobs/transfers bind to the calling session; teardown
//     cancels them (a commit already past the durable marker
//     stays durable — reconnect reconciles by revision)
//   - preview_* is a stable Unavailable until Task 7

#include "WorkspaceCommands.h"

#include "config/StudioConfig.h"       /* studio::ToJson(doc)  */
#include "presets/PresetRegistry.h"
#include "presets/EffectRegistry.h"

#include <filesystem>
#include <string>

namespace openrgb2 { namespace ipc {

namespace
{

/* Every failure reply carries a stable machine token the
   client switches on — the human message is for logs. */
IpcReply FailDetail(ErrCode code, std::string message,
                    std::string detail)
{
    IpcReply r = IpcReply::Fail(code, std::move(message));
    r.body["detail"] = std::move(detail);
    return r;
}

bool BodyU64(const nlohmann::json& b, const char* key,
             uint64_t& out)
{
    const auto it = b.find(key);
    if(it == b.end() || !it->is_number_unsigned())
    {
        return false;
    }
    out = it->get<uint64_t>();
    return true;
}

/* Bounded page control — {offset,limit}; limit clamps to the
   documented ceiling so a caller cannot ask for an unbounded
   reply (the 1 MiB envelope is the other bound). */
constexpr uint64_t kMaxPageLimit = 256;

struct Page
{
    uint64_t offset = 0;
    uint64_t limit  = kMaxPageLimit;
};

bool ReadPage(const nlohmann::json& body, Page& p,
              IpcReply& err)
{
    if(body.contains("offset") &&
       !BodyU64(body, "offset", p.offset))
    {
        err = FailDetail(ErrCode::BadRequest,
                         "offset must be an unsigned integer",
                         "bad_request");
        return false;
    }
    if(body.contains("limit"))
    {
        if(!BodyU64(body, "limit", p.limit))
        {
            err = FailDetail(ErrCode::BadRequest,
                             "limit must be an unsigned integer",
                             "bad_request");
            return false;
        }
        if(p.limit > kMaxPageLimit)
        {
            p.limit = kMaxPageLimit;
        }
    }
    return true;
}

template <typename T, typename F>
nlohmann::json Paged(const std::vector<T>& items, const Page& p,
                     const F& to_json)
{
    nlohmann::json out = nlohmann::json::array();
    const uint64_t total = items.size();
    for(uint64_t i = p.offset;
        i < total && out.size() < p.limit; ++i)
    {
        out.push_back(to_json(items[(size_t)i]));
    }
    return out;
}

} /* anonymous namespace */

WorkspaceCommands::WorkspaceCommands(
    WorkspaceService* service,
    WorkspaceTransfer::Options transfer_options)
    : service_(service)
      /* staging lives INSIDE the verified managed root — the
         store already owns the containment discipline */
    , transfers_(service ? service->Store().Root() +
                               std::string("/transfers")
                         : std::string(),
                 transfer_options)
{
    if(service_)
    {
        /* The worker's staging seam: staged-upload reads and
           session-bound download staging go through the same
           bounded transfer table the handlers drive. The
           completion-EVENT emitter is wired separately by
           EngineService (SetJobEventHook) — this surface is
           staging only. */
        WorkspaceService::JobSurface surf;
        surf.read_staged =
            [this](const std::string& token, std::string& bytes,
                   std::string& err)
            {
                return transfers_.ReadStaged(token, bytes, err);
            };
        surf.stage_download =
            [this](uint64_t session_id, const std::string& kind,
                   const std::string& bytes, std::string& token,
                   std::string& sha256_hex, std::string& err)
            {
                const WorkspaceTransfer::BeginResult r =
                    transfers_.StageDownload(session_id, kind,
                                             bytes);
                if(!r.ok)
                {
                    err = r.error.empty() ? r.detail : r.error;
                    return false;
                }
                token      = r.transfer_id;
                sha256_hex = r.sha256;
                return true;
            };
        service_->SetJobSurface(std::move(surf));
    }
}

/*---------------------------------------------------------*\
||| Metadata                                                  ||
\*---------------------------------------------------------*/

nlohmann::json WorkspaceCommands::SnapshotJson() const
{
    nlohmann::json out = nlohmann::json::object(
        {{ "managed", false }});
    if(!service_)
    {
        return out;
    }
    const std::string id = service_->CurrentId();
    if(id.empty())
    {
        return out;
    }
    const std::shared_ptr<const WorkspaceCandidate> cand =
        service_->CurrentCandidate();
    out["managed"]            = true;
    out["id"]                 = id;
    out["workspace_revision"] = service_->WorkspaceRevision();
    out["resource_revision"]  = service_->ResourceRevision();
    out["generation"]         = service_->CurrentGeneration();
    if(cand)
    {
        out["name"] = cand->document.meta.name;
        /* The deterministic serialization is what a document
           download streams — the hash is the reconnect-reconcile
           token (plan 3.3). In-memory only: bounded CPU, never
           file I/O on a reader thread. */
        out["content_sha256"] =
            Sha256Hex(studio::ToJson(cand->document).dump(2));
        out["source_form"] =
            WorkspaceSourceFormName(cand->source_form);
    }
    return out;
}

IpcReply WorkspaceCommands::HandleGet(uint64_t session_id,
                                      const nlohmann::json& body)
{
    (void)session_id;
    if(body.contains("id"))
    {
        /* A non-current workspace's metadata reads the durable
           marker — never guesses. */
        const std::string id = body.value("id", std::string());
        if(!WorkspaceStore::IsWorkspaceId(id))
        {
            return FailDetail(ErrCode::BadRequest,
                              "invalid workspace id",
                              "bad_request");
        }
        const WorkspaceStore::Revisions rev =
            service_->Store().LoadRevisions(id);
        nlohmann::json out = nlohmann::json::object({
            { "managed",            rev.exists            },
            { "id",                 id                    },
            { "workspace_revision", rev.workspace_revision },
            { "resource_revision",  rev.resource_revision  },
        });
        if(!rev.error.empty())
        {
            out["marker_error"] = rev.error;
        }
        return IpcReply::Success(MsgType::Ok, out);
    }
    return IpcReply::Success(MsgType::Ok, SnapshotJson());
}

IpcReply WorkspaceCommands::HandleList(const nlohmann::json& body)
{
    Page p;
    IpcReply err;
    if(!ReadPage(body, p, err))
    {
        return err;
    }
    const std::vector<std::string> all = service_->Store().List();
    nlohmann::json ids = nlohmann::json::array();
    for(uint64_t i = p.offset;
        i < all.size() && ids.size() < p.limit; ++i)
    {
        ids.push_back(all[(size_t)i]);
    }
    return IpcReply::Success(MsgType::Ok, nlohmann::json::object({
        { "workspaces", ids                    },
        { "total",      (uint64_t)all.size()   },
        { "offset",     p.offset               },
        { "limit",      p.limit                },
        { "active",     service_->CurrentId()  },
    }));
}

IpcReply WorkspaceCommands::HandleCatalog(const nlohmann::json& body)
{
    Page p;
    IpcReply err;
    if(!ReadPage(body, p, err))
    {
        return err;
    }
    const std::string kind = body.value("kind", std::string());
    nlohmann::json entries = nlohmann::json::array();
    uint64_t total = 0;
    if(kind == "devices")
    {
        const auto items =
            service_->Store().EffectiveDeviceRegistry().List();
        total = items.size();
        entries = Paged(items, p,
            [](const studio::PresetRegistry::PresetInfo& i)
            {
                return nlohmann::json::object({
                    { "id",         i.id         },
                    { "name",       i.name       },
                    { "category",   i.category   },
                    { "zone_count", i.zone_count },
                    { "led_total",  i.led_total  },
                    { "from_file",  i.from_file  },
                });
            });
    }
    else if(kind == "effects")
    {
        const auto items =
            service_->Store().EffectiveEffectRegistry().List();
        total = items.size();
        entries = Paged(items, p,
            [](const studio::EffectRegistry::EffectInfo& i)
            {
                return nlohmann::json::object({
                    { "id",          i.id          },
                    { "name",        i.name        },
                    { "description", i.description },
                    { "needs",       i.needs       },
                    { "from_file",   i.from_file   },
                });
            });
    }
    else if(kind == "workspaces")
    {
        const std::vector<std::string> all =
            service_->Store().List();
        total = all.size();
        nlohmann::json ids = nlohmann::json::array();
        for(uint64_t i = p.offset;
            i < all.size() && ids.size() < p.limit; ++i)
        {
            ids.push_back(all[(size_t)i]);
        }
        entries = ids;
    }
    else
    {
        return FailDetail(ErrCode::BadRequest,
            "catalog kind must be devices|effects|workspaces",
            "bad_request");
    }
    return IpcReply::Success(MsgType::Ok, nlohmann::json::object({
        { "kind",    kind    },
        { "entries", entries },
        { "total",   total   },
        { "offset",  p.offset },
        { "limit",   p.limit  },
    }));
}

/*---------------------------------------------------------*\
||| Job verbs                                                 ||
\*---------------------------------------------------------*/

IpcReply WorkspaceCommands::HandleJobVerb(uint64_t session_id,
                                          const std::string& verb,
                                          const nlohmann::json& body)
{
    /* A `staged` token only ever belongs to the session that
       produced it — a foreign or expired token fails before
       the job exists. */
    const auto sit = body.find("staged");
    if(sit != body.end())
    {
        if(!sit->is_string())
        {
            return FailDetail(ErrCode::BadRequest,
                              "staged must be a token string",
                              "bad_request");
        }
        if(!transfers_.StagedFor(session_id,
                                 sit->get<std::string>()))
        {
            return FailDetail(ErrCode::BadRequest,
                "staged token is not consumable by this session",
                "bad_request");
        }
    }

    const WorkspaceService::JobResult r =
        service_->Enqueue(session_id, verb, body);
    if(r.accepted)
    {
        return IpcReply::Success(MsgType::Ok, nlohmann::json::object({
            { "accepted", true      },
            { "job_id",   r.job_id  },
        }));
    }
    if(r.detail == "busy")
    {
        /* Bounded, not broken — the pending queue is full; the
           caller retries after a terminal event/status. */
        return IpcReply::Success(MsgType::Ok, nlohmann::json::object({
            { "accepted", false   },
            { "detail",   "busy"  },
        }));
    }
    return FailDetail(ErrCode::BadRequest, r.error,
                      r.detail.empty() ? "bad_request" : r.detail);
}

IpcReply WorkspaceCommands::HandleJobStatus(uint64_t session_id,
                                            const nlohmann::json& body)
{
    uint64_t job_id = 0;
    if(!BodyU64(body, "job_id", job_id))
    {
        return FailDetail(ErrCode::BadRequest,
                          "job_status requires {job_id}",
                          "bad_request");
    }
    return IpcReply::Success(MsgType::Ok,
        JobStatusToJson(service_->Status(session_id, job_id)));
}

IpcReply WorkspaceCommands::HandleJobCancel(uint64_t session_id,
                                            const nlohmann::json& body)
{
    uint64_t job_id = 0;
    if(!BodyU64(body, "job_id", job_id))
    {
        return FailDetail(ErrCode::BadRequest,
                          "job_cancel requires {job_id}",
                          "bad_request");
    }
    const WorkspaceService::JobResult r =
        service_->Cancel(session_id, job_id);
    if(!r.accepted)
    {
        return FailDetail(ErrCode::BadRequest,
                          r.error.empty() ? "job cannot be cancelled"
                                          : r.error,
                          r.detail.empty() ? "bad_request" : r.detail);
    }
    return IpcReply::Success(MsgType::Ok,
        nlohmann::json::object({{ "cancelled", true },
                                { "job_id",    job_id }}));
}

/*---------------------------------------------------------*\
||| Transfers                                                 ||
\*---------------------------------------------------------*/

IpcReply WorkspaceCommands::HandleTransferBegin(
    uint64_t session_id, const nlohmann::json& body)
{
    /* Clients never name paths — staging is server-generated
       ids inside the managed root. */
    if(body.contains("path") || body.contains("file"))
    {
        return FailDetail(ErrCode::BadRequest,
            "client-supplied paths are never accepted",
            "bad_request");
    }
    const std::string direction =
        body.value("direction", std::string("upload"));
    const std::string kind =
        body.value("kind", std::string("document"));

    if(direction == "upload")
    {
        uint64_t length = 0;
        const std::string sha =
            body.value("sha256", std::string());
        if(!BodyU64(body, "length", length) || sha.empty())
        {
            return FailDetail(ErrCode::BadRequest,
                "upload requires {kind,length,sha256}",
                "bad_request");
        }
        const WorkspaceTransfer::BeginResult r =
            transfers_.BeginUpload(session_id, kind, length, sha);
        if(!r.ok)
        {
            return FailDetail(ErrCode::BadRequest, r.error, r.detail);
        }
        return IpcReply::Success(MsgType::Ok, nlohmann::json::object({
            { "transfer_id",     r.transfer_id                    },
            { "max_chunk_bytes", transfers_.TransferOptions()
                                     .max_chunk_decoded_bytes   },
        }));
    }

    if(direction == "download")
    {
        /* Bounded document read materialized for staged
           download — document/type/look content rides the
           transfer mechanism, not an inline body. */
        const std::string id = body.value("id", std::string());
        if(id.empty() || !WorkspaceStore::IsWorkspaceId(id))
        {
            return FailDetail(ErrCode::BadRequest,
                "download requires {id} naming a managed "
                "workspace", "bad_request");
        }
        std::string bytes, err;
        if(!service_->DocumentBytes(id, bytes, err))
        {
            return FailDetail(ErrCode::BadRequest, err,
                              "bad_request");
        }
        const WorkspaceTransfer::BeginResult r =
            transfers_.StageDownload(session_id, kind, bytes);
        if(!r.ok)
        {
            return FailDetail(ErrCode::BadRequest, r.error, r.detail);
        }
        return IpcReply::Success(MsgType::Ok, nlohmann::json::object({
            { "transfer_id",     r.transfer_id                    },
            { "length",          r.length                         },
            { "sha256",          r.sha256                         },
            { "max_chunk_bytes", transfers_.TransferOptions()
                                     .max_chunk_decoded_bytes   },
        }));
    }

    return FailDetail(ErrCode::BadRequest,
                      "direction must be upload|download",
                      "bad_request");
}

IpcReply WorkspaceCommands::HandleTransferChunk(
    uint64_t session_id, const nlohmann::json& body)
{
    const std::string tid =
        body.value("transfer_id", std::string());
    uint64_t offset = 0;
    if(tid.empty() || !BodyU64(body, "offset", offset))
    {
        return FailDetail(ErrCode::BadRequest,
            "transfer_chunk requires {transfer_id,offset}",
            "bad_request");
    }

    const auto dit = body.find("data");
    if(dit != body.end())
    {
        /* upload chunk — base64, <=192 KiB DECODED. The encoded
           bound rejects oversized payloads before decode. */
        if(!dit->is_string())
        {
            return FailDetail(ErrCode::BadRequest,
                              "data must be base64",
                              "bad_request");
        }
        const uint64_t cap =
            transfers_.TransferOptions().max_chunk_decoded_bytes;
        const uint64_t enc_cap = ((cap + 2) / 3) * 4;
        if((uint64_t)dit->get<std::string>().size() > enc_cap)
        {
            return FailDetail(ErrCode::BadRequest,
                "chunk exceeds the decoded-byte bound",
                "too_large");
        }
        std::vector<uint8_t> decoded;
        if(!Base64Decode(dit->get<std::string>(), decoded,
                         (size_t)cap))
        {
            return FailDetail(ErrCode::BadRequest,
                              "data is not valid base64",
                              "bad_request");
        }
        const WorkspaceTransfer::ChunkResult r =
            transfers_.WriteChunk(session_id, tid, offset, decoded);
        if(!r.ok)
        {
            return FailDetail(ErrCode::BadRequest, r.error, r.detail);
        }
        return IpcReply::Success(MsgType::Ok, nlohmann::json::object({
            { "offset",   r.offset   },
            { "received", r.received },
        }));
    }

    /* download chunk — ordered reads */
    uint64_t max_bytes = 0;
    if(body.contains("max_bytes") &&
       !BodyU64(body, "max_bytes", max_bytes))
    {
        return FailDetail(ErrCode::BadRequest,
                          "max_bytes must be an unsigned integer",
                          "bad_request");
    }
    const WorkspaceTransfer::ChunkResult r =
        transfers_.ReadChunk(session_id, tid, offset, max_bytes);
    if(!r.ok)
    {
        return FailDetail(ErrCode::BadRequest, r.error, r.detail);
    }
    return IpcReply::Success(MsgType::Ok, nlohmann::json::object({
        { "offset", r.offset   },
        { "data",   r.data_b64 },
        { "eof",    r.eof      },
    }));
}

IpcReply WorkspaceCommands::HandleTransferFinish(
    uint64_t session_id, const nlohmann::json& body)
{
    const std::string tid =
        body.value("transfer_id", std::string());
    if(tid.empty())
    {
        return FailDetail(ErrCode::BadRequest,
                          "transfer_finish requires {transfer_id}",
                          "bad_request");
    }
    const WorkspaceTransfer::FinishResult r =
        transfers_.Finish(session_id, tid);
    if(!r.ok)
    {
        return FailDetail(ErrCode::BadRequest, r.error, r.detail);
    }
    nlohmann::json out = nlohmann::json::object(
        {{ "finished", true }});
    if(!r.staged_token.empty())
    {
        out["staged"] = r.staged_token;
        out["bytes"]  = r.bytes;
    }
    return IpcReply::Success(MsgType::Ok, out);
}

IpcReply WorkspaceCommands::HandleTransferCancel(
    uint64_t session_id, const nlohmann::json& body)
{
    const std::string tid =
        body.value("transfer_id", std::string());
    if(tid.empty() ||
       !transfers_.Cancel(session_id, tid))
    {
        return FailDetail(ErrCode::BadRequest,
                          "unknown transfer", "unknown_transfer");
    }
    return IpcReply::Success(MsgType::Ok,
        nlohmann::json::object({{ "cancelled", true }}));
}

/*---------------------------------------------------------*\
||| Dispatch                                                  ||
\*---------------------------------------------------------*/

IpcReply WorkspaceCommands::Handle(uint64_t session_id,
                                   const nlohmann::json& body)
{
    if(!service_)
    {
        return FailDetail(ErrCode::Unavailable,
            "the workspace capability is not wired",
            "unavailable");
    }
    if(!body.is_object() ||
       !body.value("verb", nlohmann::json()).is_string())
    {
        return FailDetail(ErrCode::BadRequest,
            "WORKSPACE_COMMAND requires {verb:<string>}",
            "bad_request");
    }
    /* Idle transfers die lazily — every command sweeps, so
       expiry never depends on a ticking caller. */
    transfers_.SweepExpired();

    const std::string verb = body["verb"].get<std::string>();

    if(verb == "get")     return HandleGet(session_id, body);
    if(verb == "list")    return HandleList(body);
    if(verb == "catalog") return HandleCatalog(body);

    if(verb == "validate"       || verb == "commit"      ||
       verb == "save_copy"      || verb == "import_inspect" ||
       verb == "import_commit"  || verb == "export"      ||
       verb == "reload")
    {
        return HandleJobVerb(session_id, verb, body);
    }

    if(verb == "transfer_begin")  return HandleTransferBegin(session_id, body);
    if(verb == "transfer_chunk")  return HandleTransferChunk(session_id, body);
    if(verb == "transfer_finish") return HandleTransferFinish(session_id, body);
    if(verb == "transfer_cancel") return HandleTransferCancel(session_id, body);

    if(verb == "job_status") return HandleJobStatus(session_id, body);
    if(verb == "job_cancel") return HandleJobCancel(session_id, body);

    /* The preview surface is stable NOW; the Task-7 preview
       engine is what answers Unavailable honestly. */
    if(verb == "preview_begin" || verb == "preview_update" ||
       verb == "preview_end")
    {
        return FailDetail(ErrCode::Unavailable,
            "the preview engine lands with Task 7 — the verb "
            "surface is stable",
            "unavailable");
    }

    return FailDetail(ErrCode::BadRequest,
                      "unknown workspace verb '" + verb + "'",
                      "bad_request");
}

void WorkspaceCommands::OnSessionEnd(uint64_t session_id)
{
    /* Queued jobs + staged transfers of the dead session are
       cancelled — a commit already past the durable marker is
       NEVER rolled back; the reconnect reconciles by
       revision/content hash. */
    if(service_)
    {
        service_->CancelSession(session_id);
    }
    transfers_.CancelSession(session_id);
}

void WorkspaceCommands::SweepExpired()
{
    transfers_.SweepExpired();
}

bool WorkspaceCommands::ReadStaged(const std::string& token,
                                   std::string& bytes,
                                   std::string& err)
{
    return transfers_.ReadStaged(token, bytes, err);
}

bool WorkspaceCommands::StageDownload(uint64_t session_id,
                                      const std::string& kind,
                                      const std::string& bytes,
                                      std::string& token,
                                      std::string& err)
{
    std::string sha;
    const WorkspaceTransfer::BeginResult r =
        transfers_.StageDownload(session_id, kind, bytes);
    if(!r.ok)
    {
        err = r.error.empty() ? r.detail : r.error;
        return false;
    }
    token = r.transfer_id;
    return true;
}

}} // namespace openrgb2::ipc
