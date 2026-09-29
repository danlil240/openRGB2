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
#include "config/WorkspaceCandidate.h" /* ParseWorkspaceCandidate */
#include "presets/PresetRegistry.h"
#include "presets/EffectRegistry.h"

#include <chrono>
#include <filesystem>
#include <string>

namespace openrgb2 { namespace ipc {

namespace fs = std::filesystem;

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

/* Cheap invalidation key for the reader caches — a bounded
   entry walk (names + sizes + write times), never a parse.
   Mixing entry mtimes catches content rewrites the dir's own
   timestamp does not report. 0 = unreadable/absent (caches
   still build once and re-check each call). */
uint64_t DirStamp(const std::string& dir)
{
    std::error_code ec;
    if(!fs::is_directory(dir, ec))
    {
        return 0;
    }
    uint64_t h = 1469598103934665603ull;    /* FNV-1a 64 offset */
    auto mix = [&](uint64_t v)
    {
        h ^= v;
        h *= 1099511628211ull;
    };
    for(const fs::directory_entry& e :
            fs::directory_iterator(dir, ec))
    {
        const std::string name = e.path().filename().string();
        for(unsigned char c : name)
        {
            mix(c);
        }
        mix((uint64_t)e.file_size(ec));
        const fs::file_time_type t = e.last_write_time(ec);
        mix((uint64_t)std::chrono::duration_cast<
            std::chrono::nanoseconds>(
                t.time_since_epoch()).count());
    }
    const fs::file_time_type dt = fs::last_write_time(dir, ec);
    mix((uint64_t)std::chrono::duration_cast<
        std::chrono::nanoseconds>(
            dt.time_since_epoch()).count());
    return ec ? 0 : h;
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
||| Reader caches — file work is never re-run per request    ||
||| (library/list key on dir write times; document bytes on  ||
||| durable marker revisions).                               ||
\*---------------------------------------------------------*/

std::shared_ptr<const studio::PresetRegistry>
WorkspaceCommands::CachedDevices() const
{
    const uint64_t stamp =
        DirStamp(service_->Store().LibraryDevicesDir());
    std::lock_guard<std::mutex> g(cache_mutex_);
    if(!lib_dev_ || lib_dev_mtime_ != stamp)
    {
        lib_dev_ = std::make_shared<const studio::PresetRegistry>(
            service_->Store().EffectiveDeviceRegistry());
        lib_dev_mtime_ = stamp;
    }
    return lib_dev_;
}

std::shared_ptr<const studio::EffectRegistry>
WorkspaceCommands::CachedEffects() const
{
    const uint64_t stamp =
        DirStamp(service_->Store().LibraryEffectsDir());
    std::lock_guard<std::mutex> g(cache_mutex_);
    if(!lib_eff_ || lib_eff_mtime_ != stamp)
    {
        lib_eff_ = std::make_shared<const studio::EffectRegistry>(
            service_->Store().EffectiveEffectRegistry());
        lib_eff_mtime_ = stamp;
    }
    return lib_eff_;
}

std::vector<std::string> WorkspaceCommands::CachedList() const
{
    /* The id set changes only when a workspace dir appears or
       vanishes under the root — the root's own write time is
       the invalidation key (a generation write inside an id
       dir does not change the LIST). */
    const uint64_t stamp = DirStamp(service_->Store().Root());
    std::lock_guard<std::mutex> g(cache_mutex_);
    if(list_mtime_ != stamp)
    {
        list_ids_   = service_->Store().List();
        list_mtime_ = stamp;
    }
    return list_ids_;
}

std::shared_ptr<const std::string>
WorkspaceCommands::CachedDocumentBytes(const std::string& id,
                                       uint64_t workspace_revision,
                                       uint64_t resource_revision,
                                       std::string& err) const
{
    std::lock_guard<std::mutex> g(cache_mutex_);
    const auto it = doc_cache_.find(id);
    if(it != doc_cache_.end() &&
       it->second.workspace_revision == workspace_revision &&
       it->second.resource_revision == resource_revision)
    {
        return it->second.bytes;
    }
    std::string bytes;
    /* One bounded load per (id,revisions) — repeat download
       begins and content_sha256 reads hit the cache. */
    if(!service_->DocumentBytes(id, bytes, err))
    {
        return nullptr;
    }
    DocCacheEntry e;
    e.workspace_revision = workspace_revision;
    e.resource_revision  = resource_revision;
    e.seq    = doc_cache_seq_++;
    e.bytes  = std::make_shared<const std::string>(
                   std::move(bytes));
    doc_cache_[id] = std::move(e);
    while(doc_cache_.size() > kMaxDocCacheEntries)
    {
        auto oldest = doc_cache_.begin();
        for(auto i = doc_cache_.begin(); i != doc_cache_.end(); ++i)
        {
            if(i->second.seq < oldest->second.seq)
            {
                oldest = i;
            }
        }
        doc_cache_.erase(oldest);
    }
    return doc_cache_[id].bytes;
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
    /* One coherent published snapshot — never torn across a
       commit and never queued behind worker file I/O. */
    const WorkspaceService::PublishedState snap = service_->Snapshot();
    if(!snap.managed)
    {
        return out;
    }
    out["managed"]            = true;
    out["id"]                 = snap.id;
    out["workspace_revision"] = snap.workspace_revision;
    out["resource_revision"]  = snap.resource_revision;
    out["generation"]         = snap.generation;
    if(!snap.name.empty())
    {
        out["name"] = snap.name;
    }
    if(!snap.source_form.empty())
    {
        out["source_form"] = snap.source_form;
    }
    /* The deterministic serialization is what a document
       download streams — same bytes the doc cache serves, so
       the hash is the reconnect-reconcile token (plan 3.3).
       Cached per revision: at most one bounded store read per
       commit, amortized across every get/download. */
    std::string err;
    if(const std::shared_ptr<const std::string> bytes =
           CachedDocumentBytes(snap.id, snap.workspace_revision,
                               snap.resource_revision, err))
    {
        out["content_sha256"] = Sha256Hex(*bytes);
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
        /* The current id answers from the published snapshot —
           no marker read at all. */
        const WorkspaceService::PublishedState snap = service_->Snapshot();
        if(snap.managed && snap.id == id)
        {
            return IpcReply::Success(MsgType::Ok,
                nlohmann::json::object({
                    { "managed",            true                  },
                    { "id",                 id                    },
                    { "workspace_revision",
                      snap.workspace_revision                     },
                    { "resource_revision",
                      snap.resource_revision                      },
                }));
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
    /* Cached on the root dir's write time — the id set, not
       the file system, is what this answer is about. */
    const std::vector<std::string> all = CachedList();
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
    bool more = false;
    uint64_t next_offset = 0;
    if(kind == "devices")
    {
        /* Cached effective library — an unchanged library is
           never re-parsed per request. */
        const auto items = CachedDevices()->List();
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
        const std::shared_ptr<const studio::EffectRegistry> reg =
            CachedEffects();
        const auto items = reg->List();
        total = items.size();
        /* Byte-budgeted page: `from_file` entries inline their
           document, and a heavy look library must not push the
           reply past kMaxBodyBytes (EncodeMessage would drop the
           send and the session would tear down — a reconnecting
           client would then loop). Entries accumulate while the
           serialized page stays under ~3/4 of the 1 MiB body cap
           (envelope headroom); the remainder rides the next page
           via more/next_offset. */
        constexpr size_t kCatalogPageBytes = 768u * 1024u;
        size_t bytes = 2;                    /* "[]" */
        uint64_t i = p.offset;
        for(; i < items.size() && entries.size() < p.limit; ++i)
        {
            const auto& info = items[(size_t)i];
            nlohmann::json e = nlohmann::json::object({
                { "id",          info.id          },
                { "name",        info.name        },
                { "description", info.description },
                { "needs",       info.needs       },
                { "from_file",   info.from_file   },
            });
            /* File-supplied looks carry their document so the
               UI session can resolve them without a second
               round-trip — a saved look stays usable across
               restarts. document_text keeps the ordered_json
               byte order (remix draws consume the RNG stream in
               field order — a re-keyed JSON object would
               diverge). Packaged looks resolve from the
               client's compiled-in registry already. */
            if(info.from_file)
            {
                if(const studio::EffectDocument* d =
                       reg->Find(info.id))
                {
                    e["document_text"] =
                        studio::EffectDocumentToJson(*d).dump();
                }
            }
            std::string ser = e.dump();
            if(bytes + ser.size() + 1 > kCatalogPageBytes)
            {
                /* An entry that ALONE exceeds the budget degrades
                   to metadata + an honest flag — never a dropped
                   session. */
                if(entries.empty() && e.contains("document_text"))
                {
                    e.erase("document_text");
                    e["document_omitted"] = true;
                    ser = e.dump();
                }
                /* The metadata itself can still exceed the page
                   (a giant description rides it AND the doc) —
                   bound it too. A lone entry always emits
                   SOMETHING, so the walk can never stall on
                   next_offset == offset with an empty page. */
                if(entries.empty() &&
                   bytes + ser.size() + 1 > kCatalogPageBytes)
                {
                    e["description"] = std::string();
                    e["description_omitted"] = true;
                    ser = e.dump();
                }
                if(bytes + ser.size() + 1 > kCatalogPageBytes)
                {
                    break;
                }
            }
            bytes += ser.size() + 1;
            entries.push_back(std::move(e));
        }
        if(i < items.size())
        {
            more        = true;
            next_offset = i;
        }
    }
    else if(kind == "workspaces")
    {
        const std::vector<std::string> all = CachedList();
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
    /* For the bounded kinds, `more`+`next_offset` continue the
       walk when a count- or byte-limited page truncated early. */
    if(!more && p.offset + entries.size() < total)
    {
        more        = true;
        next_offset = p.offset + entries.size();
    }
    return IpcReply::Success(MsgType::Ok, nlohmann::json::object({
        { "kind",        kind        },
        { "entries",     entries     },
        { "total",       total       },
        { "offset",      p.offset    },
        { "limit",       p.limit     },
        { "more",        more        },
        { "next_offset", next_offset },
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
           transfer mechanism, not an inline body. Revisions
           are the cheap staleness key: repeat begins serve
           the cached bytes; a commit invalidates honestly. */
        const std::string id = body.value("id", std::string());
        if(id.empty() || !WorkspaceStore::IsWorkspaceId(id))
        {
            return FailDetail(ErrCode::BadRequest,
                "download requires {id} naming a managed "
                "workspace", "bad_request");
        }
        const WorkspaceStore::Revisions rev =
            service_->Store().LoadRevisions(id);
        if(!rev.exists || !rev.error.empty())
        {
            return FailDetail(ErrCode::BadRequest,
                rev.error.empty()
                    ? "no managed workspace '" + id + "'"
                    : rev.error,
                "bad_request");
        }
        std::string err;
        const std::shared_ptr<const std::string> bytes =
            CachedDocumentBytes(id, rev.workspace_revision,
                                rev.resource_revision, err);
        if(!bytes)
        {
            return FailDetail(ErrCode::BadRequest, err,
                              "bad_request");
        }
        const WorkspaceTransfer::BeginResult r =
            transfers_.StageDownload(session_id, kind, *bytes);
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
       verb == "resource_install" ||
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

    /* Task 7 draft preview — session-bound, bounded, and
       never a hardware path. */
    if(verb == "preview_begin")  return HandlePreviewBegin(session_id, body);
    if(verb == "preview_update") return HandlePreviewUpdate(session_id, body);
    if(verb == "preview_end")    return HandlePreviewEnd(session_id);

    return FailDetail(ErrCode::BadRequest,
                      "unknown workspace verb '" + verb + "'",
                      "bad_request");
}

void WorkspaceCommands::OnSessionEnd(uint64_t session_id)
{
    /* Queued jobs + staged transfers of the dead session are
       cancelled — a commit already past the durable marker is
       NEVER rolled back; the reconnect reconciles by
       revision/content hash. Its draft preview dies too (plan
       3.4: "release on disconnect"). */
    if(service_)
    {
        service_->CancelSession(session_id);
    }
    transfers_.CancelSession(session_id);
    {
        std::lock_guard<std::mutex> lock(preview_mutex_);
        previews_.erase(session_id);
    }
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

/*---------------------------------------------------------*\
|| Task 7 — bounded draft preview                          ||
||                                                          ||
||   preview_begin/update/end manage ONE PreviewSession    ||
||   per IPC session (global bound PreviewDeps::max_       ||
||   sessions). Frames publish ONLY to the owning session  ||
||   through deps.emit -> IpcServer::EmitTo, chunked below ||
||   the body cap, latest-wins per (sequence,chunk). No    ||
||   sink, no SDK, no capture: the engine-side object is   ||
||   pure evaluation over the shared prepared scene.       ||
\*---------------------------------------------------------*/

void WorkspaceCommands::SetPreviewDeps(PreviewDeps deps)
{
    std::lock_guard<std::mutex> lock(preview_mutex_);
    preview_deps_ = std::move(deps);
}

/* The "preview" event's initial snapshot for a subscribing
   session — a status record, so a UI that subscribes BEFORE
   beginning still learns the honest inactive state. */
nlohmann::json WorkspaceCommands::PreviewStatusJson(
    uint64_t session_id) const
{
    nlohmann::json out = nlohmann::json::object({
        { "type",   "status" },
        { "active", false    },
        { "paused", false    },
    });
    std::lock_guard<std::mutex> lock(preview_mutex_);
    const auto it = previews_.find(session_id);
    if(it != previews_.end() && it->second.session)
    {
        out["active"]         = it->second.session->Active();
        out["paused"]         = !it->second.paused_reason.empty();
        if(!it->second.paused_reason.empty())
        {
            out["reason"] = it->second.paused_reason;
        }
        out["draft_revision"] = it->second.session->DraftRevision();
        out["sequence"]       = it->second.session->Sequence();
        const std::string err = it->second.session->Error();
        if(!err.empty())
        {
            out["error"] = err;
        }
    }
    return out;
}

/* Parse the document half of begin/update — inline
   {document} or a session-bound staged upload token
   {staged}. The candidate is immutable; the session owns it
   read-only. */
IpcReply WorkspaceCommands::PreviewCandidate(
    uint64_t session_id, const nlohmann::json& body,
    std::shared_ptr<WorkspaceCandidate>& out,
    uint64_t& draft_revision)
{
    nlohmann::json doc;
    const auto dit = body.find("document");
    const auto sit = body.find("staged");
    if(dit != body.end())
    {
        doc = *dit;
    }
    else if(sit != body.end())
    {
        if(!sit->is_string())
        {
            return FailDetail(ErrCode::BadRequest,
                              "staged must be a token string",
                              "bad_request");
        }
        /* Session-bound consumption, same rule the job verbs
           enforce. */
        if(!transfers_.StagedFor(session_id,
                                 sit->get<std::string>()))
        {
            return FailDetail(ErrCode::BadRequest,
                "staged token is not consumable by this session",
                "bad_request");
        }
        std::string bytes, err;
        if(!transfers_.ReadStaged(sit->get<std::string>(),
                                  bytes, err))
        {
            return FailDetail(ErrCode::BadRequest,
                              err.empty() ? "staged token unreadable"
                                          : err,
                              "bad_request");
        }
        doc = nlohmann::json::parse(bytes, nullptr, false);
        if(doc.is_discarded())
        {
            return FailDetail(ErrCode::BadRequest,
                              "staged document is not valid JSON",
                              "invalid_document");
        }
    }
    else
    {
        return FailDetail(ErrCode::BadRequest,
            "preview requires {document} or {staged}",
            "bad_request");
    }

    draft_revision = 0;
    if(body.contains("draft_revision") &&
       !BodyU64(body, "draft_revision", draft_revision))
    {
        return FailDetail(ErrCode::BadRequest,
            "draft_revision must be an unsigned integer",
            "bad_request");
    }

    /* The ONE candidate path — identical to validate/commit:
       the same registry snapshots, migration, and resolve
       feed the draft the workspace would commit. */
    auto cand = std::make_shared<WorkspaceCandidate>();
    std::vector<std::string> errors, warnings;
    if(!ParseWorkspaceCandidate(doc, *CachedDevices(),
                                *CachedEffects(), *cand,
                                errors, warnings))
    {
        return FailDetail(ErrCode::BadRequest,
            errors.empty() ? "document failed to parse"
                           : errors.front(),
            "invalid_document");
    }
    out = std::move(cand);
    return IpcReply::Success(MsgType::Ok, nlohmann::json::object());
}

IpcReply WorkspaceCommands::HandlePreviewBegin(
    uint64_t session_id, const nlohmann::json& body)
{
    if(!preview_deps_.emit)
    {
        /* Unwired preview engine — the stable stub answer. */
        return FailDetail(ErrCode::Unavailable,
            "the preview engine is not wired on this server",
            "unavailable");
    }
    std::shared_ptr<WorkspaceCandidate> cand;
    uint64_t rev = 0;
    const IpcReply parsed =
        PreviewCandidate(session_id, body, cand, rev);
    if(parsed.code != ErrCode::Ok)
    {
        return parsed;
    }

    std::lock_guard<std::mutex> lock(preview_mutex_);
    auto it = previews_.find(session_id);
    if(it == previews_.end())
    {
        /* One preview per session; a global bound across
           sessions (plan 3.4). A re-begin replaces the
           session's OWN preview and never consumes a second
           slot. */
        if(previews_.size() >= preview_deps_.max_sessions)
        {
            return FailDetail(ErrCode::Busy,
                "the concurrent preview limit is reached",
                "preview_limit");
        }
        PreviewSession::Options popts;
        popts.display_budget_leds =
            preview_deps_.display_budget_leds;
        PreviewSession::Deps pdeps;
        pdeps.snapshots = preview_deps_.snapshots;
        auto slot = std::make_unique<PreviewSession>(
            std::move(pdeps), popts);
        it = previews_.emplace(session_id,
                               PreviewSlot{ std::move(slot) })
                 .first;
    }
    std::string err;
    if(!it->second.session->SetCandidate(std::move(cand), rev,
                                         err))
    {
        return FailDetail(ErrCode::BadRequest, err,
                          "invalid_document");
    }
    if(body.contains("visible"))
    {
        if(!body["visible"].is_boolean())
        {
            return FailDetail(ErrCode::BadRequest,
                              "visible must be a boolean",
                              "bad_request");
        }
        it->second.session->SetVisible(
            body["visible"].get<bool>());
    }
    it->second.paused_reason.clear();
    it->second.reported_error.clear();
    it->second.last_emit = TimePoint{};

    /* Honest status push — the client lands on real state. */
    preview_deps_.emit(session_id,
        IpcEvent{ event_kind::kPreview,
                  nlohmann::json::object({
                      { "type",   "status" },
                      { "active", true     },
                      { "paused", false    },
                      { "draft_revision", rev },
                  }) },
        "status");
    return IpcReply::Success(MsgType::Ok, nlohmann::json::object({
        { "active",         true },
        { "draft_revision", rev  },
        { "rate_hz",        1000 / preview_deps_.min_interval_ms },
    }));
}

IpcReply WorkspaceCommands::HandlePreviewUpdate(
    uint64_t session_id, const nlohmann::json& body)
{
    if(!preview_deps_.emit)
    {
        return FailDetail(ErrCode::Unavailable,
            "the preview engine is not wired on this server",
            "unavailable");
    }
    std::lock_guard<std::mutex> lock(preview_mutex_);
    auto it = previews_.find(session_id);
    if(it == previews_.end())
    {
        return FailDetail(ErrCode::BadRequest,
            "no preview session is active — begin first",
            "no_preview");
    }
    if(body.contains("document") || body.contains("staged"))
    {
        std::shared_ptr<WorkspaceCandidate> cand;
        uint64_t rev = it->second.session->DraftRevision();
        const IpcReply parsed =
            PreviewCandidate(session_id, body, cand, rev);
        if(parsed.code != ErrCode::Ok)
        {
            return parsed;
        }
        std::string err;
        /* Latest-wins: the newer draft replaces the held
           candidate outright — nothing queues. */
        if(!it->second.session->SetCandidate(std::move(cand),
                                             rev, err))
        {
            return FailDetail(ErrCode::BadRequest, err,
                              "invalid_document");
        }
        it->second.reported_error.clear();
    }
    if(body.contains("visible"))
    {
        if(!body["visible"].is_boolean())
        {
            return FailDetail(ErrCode::BadRequest,
                              "visible must be a boolean",
                              "bad_request");
        }
        it->second.session->SetVisible(
            body["visible"].get<bool>());
    }
    return IpcReply::Success(MsgType::Ok, nlohmann::json::object({
        { "active",         true },
        { "draft_revision",
          it->second.session->DraftRevision() },
    }));
}

IpcReply WorkspaceCommands::HandlePreviewEnd(uint64_t session_id)
{
    bool had = false;
    {
        std::lock_guard<std::mutex> lock(preview_mutex_);
        had = previews_.erase(session_id) > 0;
    }
    if(had && preview_deps_.emit)
    {
        preview_deps_.emit(session_id,
            IpcEvent{ event_kind::kPreview,
                      nlohmann::json::object({
                          { "type",   "status" },
                          { "active", false    },
                          { "paused", false    },
                      }) },
            "status");
    }
    /* Idempotent: ending a non-existent preview is still a
       truthful {active:false}. */
    return IpcReply::Success(MsgType::Ok,
        nlohmann::json::object({{ "active", false }}));
}

/* Object -> wire form. Geometry rides every chunk so a frame
   needs no second source of truth to paint. */
static nlohmann::json PreviewObjectJson(
    const PreviewObjectColors& o)
{
    nlohmann::json j = nlohmann::json::object({
        { "id",       o.object_id  },
        { "kind",     o.kind       },
        { "x",  o.position.x }, { "y",  o.position.y },
        { "z",  o.position.z },
        { "rx", o.rotation_deg.x }, { "ry", o.rotation_deg.y },
        { "rz", o.rotation_deg.z },
        { "w",  o.size_m.x }, { "h",  o.size_m.y },
        { "d",  o.size_m.z },
        { "bound",         o.bound    },
        { "verified",      o.verified },
        { "emitter_total", o.emitter_total },
    });
    if(!o.parent_id.empty())
    {
        j["parent"] = o.parent_id;
    }
    nlohmann::json colors = nlohmann::json::array();
    for(const RgbColor& c : o.colors)
    {
        char hex[8];
        std::snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                      c.r, c.g, c.b);
        colors.push_back(hex);
    }
    j["colors"] = std::move(colors);
    if(!o.indices.empty())
    {
        /* Reduced frames carry the sampled indices — a sparse
           array can never pass for a complete one. */
        j["indices"] = o.indices;
    }
    return j;
}

void WorkspaceCommands::PollPreviews(TimePoint now)
{
    if(!preview_deps_.emit)
    {
        return;
    }
    /* Display-off pauses every preview without ending the
       session (plan 3.4) — evaluation and publishing stop;
       the state survives for the display's return. */
    const bool display_on =
        preview_deps_.power == nullptr ||
        preview_deps_.power->DisplayOn();
    const TimePoint epoch =
        preview_deps_.scene_epoch
            ? preview_deps_.scene_epoch() : TimePoint{};
    const double t =
        std::chrono::duration<double>(now - epoch).count();

    std::lock_guard<std::mutex> lock(preview_mutex_);
    for(auto& kv : previews_)
    {
        const uint64_t sid = kv.first;
        PreviewSlot&   sl  = kv.second;
        PreviewSession& ps = *sl.session;

        /* Hidden view and display-off are the two pause
           reasons — a transition publishes ONE status event,
           never a stream. */
        std::string reason;
        if(!ps.Visible())
        {
            reason = "hidden";
        }
        else if(!display_on)
        {
            reason = "display_off";
        }
        if(reason != sl.paused_reason)
        {
            sl.paused_reason = reason;
            nlohmann::json st = nlohmann::json::object({
                { "type",           "status"               },
                { "active",         ps.Active()            },
                { "paused",         !reason.empty()        },
                { "draft_revision", ps.DraftRevision()     },
                { "sequence",       ps.Sequence()          },
            });
            if(!reason.empty())
            {
                st["reason"] = reason;
            }
            preview_deps_.emit(sid,
                IpcEvent{ event_kind::kPreview, std::move(st) },
                "status");
        }
        if(!reason.empty())
        {
            continue;
        }

        /* 30 Hz publish cap — the tick may arrive faster;
           due-ness, not the loop, decides. */
        if(sl.last_emit != TimePoint{})
        {
            const auto since =
                std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                        now - sl.last_emit).count();
            if(since < preview_deps_.min_interval_ms)
            {
                continue;
            }
        }

        /* Transient input snapshot — consumed read-only;
           preview NEVER starts a capture source. */
        std::shared_ptr<const studio::InputState> input;
        if(preview_deps_.reactive)
        {
            input = preview_deps_.reactive->Snapshot();
        }

        PreviewFrame frame;
        if(!ps.Sample(t, input.get(), frame))
        {
            const std::string err = ps.Error();
            if(!err.empty() && err != sl.reported_error)
            {
                /* A malformed draft says so ONCE — not per
                   tick. */
                sl.reported_error = err;
                preview_deps_.emit(sid,
                    IpcEvent{ event_kind::kPreview,
                        nlohmann::json::object({
                            { "type",   "status" },
                            { "active", true     },
                            { "paused", false    },
                            { "error",  err      },
                            { "draft_revision",
                              ps.DraftRevision() },
                        }) },
                    "status");
            }
            continue;
        }
        sl.reported_error.clear();
        sl.last_emit = now;

        /* Chunked publish: object records accumulate while the
           serialized budget lasts; every chunk carries the
           frame's completeness metadata so the client can
           reassemble strictly by sequence and never present a
           partial set as complete. */
        const size_t budget = preview_deps_.chunk_bytes;
        nlohmann::json objects = nlohmann::json::array();
        size_t bytes = 2;
        int    chunk = 0;
        /* Two passes: count chunks first so every chunk knows
           `chunks` — a single-object overflow still emits (the
           1 MiB envelope bound applies at encode, honestly). */
        std::vector<nlohmann::json> chunks;
        for(const PreviewObjectColors& o : frame.objects)
        {
            nlohmann::json jo = PreviewObjectJson(o);
            const size_t n = jo.dump().size() + 1;
            if(bytes + n > budget && !objects.empty())
            {
                chunks.push_back(std::move(objects));
                objects = nlohmann::json::array();
                bytes = 2;
            }
            objects.push_back(std::move(jo));
            bytes += n;
        }
        chunks.push_back(std::move(objects));
        const int total = (int)chunks.size();
        for(nlohmann::json& objs : chunks)
        {
            nlohmann::json data = nlohmann::json::object({
                { "type",           "frame"              },
                { "sequence",       frame.sequence       },
                { "draft_revision", frame.draft_revision },
                { "scene_time_s",   frame.scene_time_s   },
                { "chunk",          chunk                },
                { "chunks",         total                },
                { "objects",        std::move(objs)      },
                { "reduced",        frame.reduced        },
                { "emitter_total",  frame.emitter_total  },
                { "emitter_sent",   frame.emitter_sent   },
                { "object_total",   frame.object_total   },
            });
            preview_deps_.emit(sid,
                IpcEvent{ event_kind::kPreview, std::move(data) },
                "frame/" + std::to_string(frame.sequence)
                       + "/" + std::to_string(chunk));
            ++chunk;
        }
    }
}

}} // namespace openrgb2::ipc
