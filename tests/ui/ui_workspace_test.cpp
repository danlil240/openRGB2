// SPDX-License-Identifier: GPL-2.0-or-later
//
// Shared workspace draft + recovery tests (studio-import
// Task 4) — three layers:
//
//   1. WorkspaceEditSession (Qt-free): the ONE draft —
//      baseline/working separation, one committed gesture =
//      one undo record, gesture cancel leaves no history,
//      undo/redo through EditorEdit inversion and whole-doc
//      snapshots, the 32-entry / 32-MiB bounds, remote
//      baseline moves preserving a dirty draft.
//
//   2. WorkspaceDraftStore: per-workspace durable records —
//      debounced Stage + Flush persistence, baseline
//      revision + content hash retention, discard, and
//      corrupt-record honesty.
//
//   3. WorkspaceController over the in-process pipe against
//      a scripted engine: capability gate + fallback, doc
//      fetch through the transfer mechanism, save (commit
//      job) correlation by request id -> job id -> revision,
//      save rejection preserving the draft, edits during
//      save staying dirty, revision_conflict -> conflict,
//      stale echoes never clobbering, reconnect reconcile by
//      content hash, recovery offer (matching AND moved
//      revisions — never auto-applied), library import via
//      upload + import_commit, and a missing engine still
//      permitting local draft editing/recovery.
//
// CHECK-macro main like the other UI suites — no framework.

#include "EngineBridge.h"
#include "WorkspaceController.h"
#include "WorkspaceDraftStore.h"
#include "WorkspaceEditSession.h"
#include "ProfileLibrary.h"
#include "SceneEditController.h"
#include "LayoutController.h"
#include "JsonVariant.h"

#include "config/StudioConfig.h"
#include "editor/EditorController.h"
#include "editor/TransformCommands.h"

#include "test_pipe.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QVariantMap>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace openrgb2;
using namespace openrgb2::ipc;
using namespace openrgb2::ui;
using namespace ui_test;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

static std::string Sha256(const std::string& s)
{
    return QCryptographicHash::hash(
        QByteArray::fromStdString(s),
        QCryptographicHash::Sha256).toHex().toStdString();
}

/*---------------------------------------------------------*\
|| A minimal valid compact v3 workspace document.           ||
\*---------------------------------------------------------*/
static nlohmann::json CompactDoc(const std::string& name = "desk")
{
    return nlohmann::json::object({
        { "schema_version", 3 },
        { "name",           name },
        { "devices", nlohmann::json::object({
            { "fan0", nlohmann::json::object({
                { "type", "slw_fan" }, { "x", 0.1 },
                { "y", 0.0 }, { "z", 0.2 },
                { "rx", 0.0 }, { "ry", 0.0 }, { "rz", 0.0 } }) },
            { "fan1", nlohmann::json::object({
                { "type", "slw_fan" }, { "x", 0.3 },
                { "y", 0.0 }, { "z", 0.2 } }) },
        }) },
        { "bindings", nlohmann::json::object({
            { "slw0", nlohmann::json::object({
                { "controller_name", "UNI FAN" },
                { "zone_name", "Fan 1" }, { "zone_leds", 40 } }) },
        }) },
        { "device_settings", nlohmann::json::object({
            { "fan0", nlohmann::json::object({
                { "zones", nlohmann::json::object({
                    { "ring", nlohmann::json::object({
                        { "binding", "slw0" }, { "addr_base", 0 },
                        { "verified", true } }) } }) } }) },
        }) },
        { "colors", nlohmann::json::object({
            { "objects",  nlohmann::json::object(
                              {{ "fan0", "#00B4A0" }}) },
            { "emitters", nlohmann::json::object() },
        }) },
        { "effects", nlohmann::json::object({
            { "preset", "aurora" }, { "seed", 0 },
            { "speed", 1.0 }, { "intensity", 1.0 },
            { "playing", true },
            { "layers", nlohmann::json::array() } }) },
    });
}

static studio::StudioDocument ParseCompact(
    const nlohmann::json& j)
{
    studio::StudioDocument d;
    std::vector<std::string> errs;
    const bool ok = studio::FromJson(j, d, &errs);
    CHECK(ok);
    if(!ok)
    {
        std::printf("  FromJson errors: %s\n",
                    errs.empty() ? "?" : errs.front().c_str());
    }
    return d;
}

static std::string DocBytes(const studio::StudioDocument& d)
{
    return studio::ToJson(d).dump(2);
}

/*---------------------------------------------------------*\
|| Scripted engine for the workspace surface.               ||
\*---------------------------------------------------------*/
struct WsEngine
{
    explicit WsEngine(ServerPeer p) : srv(std::move(p)) {}

    ServerPeer    srv;
    bool          cap       = true;    /* studio_workspace_v1 */
    bool          managed   = true;
    std::string   ws_id     = "ws0";
    std::string   ws_name   = "desk";
    uint64_t      ws_rev    = 1;
    uint64_t      res_rev   = 1;
    nlohmann::json doc      = CompactDoc();
    uint64_t      next_job  = 1;
    bool          auto_complete_jobs = true;
    /* Audit U-7: the terminal job event can overtake the accept
       reply on the wire (the job runs on a worker thread). */
    bool          terminal_before_accept = false;
    /* job_status answers {found:false} (record evicted). */
    bool          job_status_not_found   = false;
    bool          fail_next_commit   = false;
    std::string   commit_fail_detail = "invalid_document";
    /* Count of import_commit jobs to fail before succeeding
       — exercises the serial import queue's failure path. */
    int           fail_imports       = 0;
    /* The one held (accepted, not completed) job. */
    uint64_t      held_job   = 0;
    std::string   held_verb;

    /* Legacy-mode GET_SCENE payload — non-null serves a
       loaded scene ({loaded,revision,document}). */
    nlohmann::json scene_doc;
    uint64_t      scene_rev = 1;

    /* captures */
    int           commits       = 0;
    nlohmann::json last_commit_doc;
    uint64_t      last_base_rev  = 0;
    uint64_t      last_res_rev   = 0;
    int           download_begins = 0;
    int           upload_begins   = 0;
    int           import_commits  = 0;
    int           reload_jobs     = 0;
    std::string   upload_bytes;
    std::map<std::string, std::string> staged_uploads;
    std::vector<uint64_t>         terminal_jobs;
    int           set_scene_calls = 0;
    int           layout_calls    = 0;

    std::string DocBytesFor() const { return doc.dump(2); }
    std::string DocSha() const      { return Sha256(DocBytesFor()); }

    nlohmann::json Snapshot() const
    {
        nlohmann::json o = nlohmann::json::object({
            { "managed",            managed },
        });
        if(managed)
        {
            o["id"]                 = ws_id;
            o["name"]               = ws_name;
            o["workspace_revision"] = ws_rev;
            o["resource_revision"]  = res_rev;
            o["generation"]         = ws_rev;
            o["content_sha256"]     = DocSha();
        }
        return o;
    }

    nlohmann::json JobRecord(uint64_t id, const std::string& verb,
                             const std::string& state,
                             const std::string& detail = "")
    {
        nlohmann::json o = nlohmann::json::object({
            { "found",              true  },
            { "job_id",             id    },
            { "session_id",         1     },
            { "verb",               verb  },
            /* The job's target workspace — the wire field a
               foreign-workspace job must not move revisions
               under (F07/F06 round). */
            { "id",                 ws_id },
            { "state",              state },
            { "committed",          state == "succeeded" &&
                                    (verb == "commit" ||
                                     verb == "save_copy" ||
                                     verb == "import_commit") },
            { "applied",            state == "succeeded" &&
                                    verb == "commit" },
            { "generation",         ws_rev },
            { "workspace_revision", ws_rev },
            { "resource_revision",  res_rev },
            { "result", nlohmann::json::object() },
        });
        if(state == "failed")
        {
            o["error"]  = "job failed";
            o["detail"] = detail;
        }
        return o;
    }

    void SendEvent(const nlohmann::json& data)
    {
        srv.SendMsg(MsgType::Event, 0, nlohmann::json::object({
            { "kind", "workspace" }, { "data", data } }));
    }

    void CompleteHeld(const std::string& state,
                      const std::string& detail = "")
    {
        if(held_job == 0)
        {
            return;
        }
        if(state == "succeeded" &&
           (held_verb == "commit" || held_verb == "save_copy" ||
            held_verb == "import_commit"))
        {
            ++ws_rev;
        }
        SendEvent(JobRecord(held_job, held_verb, state, detail));
        terminal_jobs.push_back(held_job);
        held_job = 0;
    }
};

static bool ServeWs(WsEngine& e, uint32_t wait_ms = 15)
{
    Message m;
    if(!e.srv.RecvMsg(m, wait_ms))
    {
        return false;
    }
    const nlohmann::json body = m.Body();
    if(getenv("WS_DEBUG"))
    {
        std::printf("  srv: type=%s id=%u body=%.120s\n",
                    MsgTypeName(m.type), m.request_id,
                    m.body_raw.c_str());
    }
    switch(m.type)
    {
    case MsgType::Hello:
    {
        nlohmann::json w = nlohmann::json::object({
            { "server_version",     1 },
            { "negotiated_version", 1 },
            { "channel",            "command" },
            { "engine",             "openrgb2-engine" },
            { "running",            true },
            { "state", nlohmann::json::object({
                { "engine", nlohmann::json::object(
                    {{ "running", true }, { "scene_clock_hz", 60 }}) },
                { "scene",  nlohmann::json::object(
                    {{ "loaded", false }}) },
                { "sdk",    nlohmann::json::object(
                    {{ "state", "connected" },
                     { "reconnect_attempt", 0 }}) },
            }) },
        });
        if(e.cap)
        {
            w["capabilities"] =
                nlohmann::json::array({ "studio_workspace_v1" });
        }
        e.srv.SendMsg(MsgType::Welcome, 0, w);
        break;
    }
    case MsgType::Subscribe:
    {
        e.srv.SendMsg(MsgType::Ok, m.request_id,
            nlohmann::json::object(
                { { "subscribed", nlohmann::json::array({
                      "workspace", "scene" }) } }));
        if(e.cap)
        {
            /* A legacy engine never emits workspace events. */
            e.SendEvent(e.Snapshot());
        }
        break;
    }
    case MsgType::GetScene:
        if(e.scene_doc.is_null())
        {
            e.srv.SendMsg(MsgType::Scene, m.request_id,
                nlohmann::json::object({{ "loaded", false }}));
        }
        else
        {
            e.srv.SendMsg(MsgType::Scene, m.request_id,
                nlohmann::json::object({
                    { "loaded",   true },
                    { "revision", e.scene_rev },
                    { "document", e.scene_doc } }));
        }
        break;
    case MsgType::SetScene:
        ++e.set_scene_calls;
        e.srv.SendMsg(MsgType::Error, m.request_id,
            nlohmann::json::object({
                { "code", (uint32_t)ErrCode::BadRequest },
                { "error",   "bad_request" },
                { "message", "workspace required" },
                { "detail",  "workspace_required" } }));
        break;
    case MsgType::LayoutCommand:
        ++e.layout_calls;
        e.srv.SendMsg(MsgType::Error, m.request_id,
            nlohmann::json::object({
                { "code", (uint32_t)ErrCode::BadRequest },
                { "error",   "bad_request" },
                { "message", "workspace required" },
                { "detail",  "workspace_required" } }));
        break;
    case MsgType::GetSnapshots:
        e.srv.SendMsg(MsgType::Snapshots, m.request_id,
                      nlohmann::json::object());
        break;
    case MsgType::GetState:
        e.srv.SendMsg(MsgType::State, m.request_id,
            nlohmann::json::object({
                { "engine", nlohmann::json::object(
                    {{ "running", true }}) },
            }));
        break;
    case MsgType::GetPresets:
        e.srv.SendMsg(MsgType::Presets, m.request_id,
            nlohmann::json::object(
                {{ "presets", nlohmann::json::array() }}));
        break;
    case MsgType::GetPlayback:
        e.srv.SendMsg(MsgType::Playback, m.request_id,
                      nlohmann::json::object());
        break;
    case MsgType::GetDeliveryStats:
        e.srv.SendMsg(MsgType::DeliveryStats, m.request_id,
                      nlohmann::json::object());
        break;
    case MsgType::Ping:
        e.srv.SendMsg(MsgType::Pong, m.request_id,
                      nlohmann::json::object());
        break;
    case MsgType::WorkspaceCommand:
    {
        const std::string verb =
            body.value("verb", std::string());
        if(verb == "get")
        {
            e.srv.SendMsg(MsgType::Ok, m.request_id,
                          e.Snapshot());
        }
        else if(verb == "list")
        {
            e.srv.SendMsg(MsgType::Ok, m.request_id,
                nlohmann::json::object({
                    { "workspaces", nlohmann::json::array(
                                        { e.ws_id }) },
                    { "total",  1 },
                    { "offset", 0 },
                    { "limit",  256 },
                    { "active", e.ws_id },
                }));
        }
        else if(verb == "transfer_begin")
        {
            const std::string dir =
                body.value("direction", std::string("upload"));
            if(dir == "download")
            {
                ++e.download_begins;
                const std::string bytes = e.DocBytesFor();
                e.srv.SendMsg(MsgType::Ok, m.request_id,
                    nlohmann::json::object({
                        { "transfer_id",     "dl1" },
                        { "length",          (uint64_t)bytes.size() },
                        { "sha256",          e.DocSha() },
                        { "max_chunk_bytes", (uint64_t)400 },
                    }));
            }
            else
            {
                ++e.upload_begins;
                e.upload_bytes.clear();
                e.srv.SendMsg(MsgType::Ok, m.request_id,
                    nlohmann::json::object({
                        { "transfer_id",     "ul1" },
                        { "max_chunk_bytes", (uint64_t)400 },
                    }));
            }
        }
        else if(verb == "transfer_chunk")
        {
            const std::string tid =
                body.value("transfer_id", std::string());
            const uint64_t off =
                body.value("offset", (uint64_t)0);
            if(body.contains("data") &&
               body.value("direction", std::string()) !=
                   "download")
            {
                /* upload chunk: {transfer_id,offset,data} */
            }
            if(body.contains("data"))
            {
                e.upload_bytes.append(
                    QByteArray::fromBase64(QByteArray::fromStdString(
                        body.value("data", std::string())))
                        .toStdString());
                e.srv.SendMsg(MsgType::Ok, m.request_id,
                    nlohmann::json::object({
                        { "offset",   off },
                        { "received", (uint64_t)e.upload_bytes.size() },
                    }));
            }
            else
            {
                /* download chunk */
                const std::string bytes = e.DocBytesFor();
                const uint64_t mx =
                    body.value("max_bytes", (uint64_t)400);
                const uint64_t n =
                    off < bytes.size()
                        ? std::min<uint64_t>(mx,
                                             bytes.size() - off)
                        : 0;
                const bool eof = off + n >= bytes.size();
                e.srv.SendMsg(MsgType::Ok, m.request_id,
                    nlohmann::json::object({
                        { "offset", off },
                        { "data",   QByteArray(
                                        bytes.data() + off,
                                        (qsizetype)n)
                                        .toBase64().toStdString() },
                        { "eof",    eof },
                    }));
            }
            (void)tid;
        }
        else if(verb == "transfer_finish")
        {
            const std::string tok =
                "stg" + std::to_string(e.next_job);
            e.staged_uploads[tok] = e.upload_bytes;
            e.srv.SendMsg(MsgType::Ok, m.request_id,
                nlohmann::json::object({
                    { "finished", true },
                    { "staged",   tok },
                    { "bytes",    (uint64_t)e.upload_bytes.size() },
                }));
        }
        else if(verb == "commit" || verb == "save_copy" ||
                verb == "reload" || verb == "import_commit" ||
                verb == "validate" || verb == "export")
        {
            const uint64_t job = e.next_job++;
            if(verb == "commit" || verb == "save_copy")
            {
                e.last_commit_doc = body.value("document",
                                               nlohmann::json());
                e.last_base_rev   =
                    body.value("base_revision", (uint64_t)0);
                e.last_res_rev    =
                    body.value("resource_revision", (uint64_t)0);
            }
            if(verb == "import_commit")
            {
                ++e.import_commits;
                const std::string tok =
                    body.value("staged", std::string());
                const auto it = e.staged_uploads.find(tok);
                if(it != e.staged_uploads.end())
                {
                    e.last_commit_doc = nlohmann::json::parse(
                        it->second, nullptr, false);
                }
            }
            if(verb == "reload")
            {
                ++e.reload_jobs;
            }
            const uint32_t accept_req = m.request_id;
            auto send_accept = [&e, accept_req, job] {
                e.srv.SendMsg(MsgType::Ok, accept_req,
                    nlohmann::json::object({
                        { "accepted", true },
                        { "job_id",   job  },
                    }));
            };
            const bool late_accept =
                e.terminal_before_accept && e.auto_complete_jobs;
            if(!late_accept)
            {
                send_accept();
            }
            if(e.auto_complete_jobs)
            {
                if(verb == "commit" && e.fail_next_commit)
                {
                    e.fail_next_commit = false;
                    e.SendEvent(e.JobRecord(
                        job, verb, "failed", e.commit_fail_detail));
                }
                else if(verb == "import_commit" &&
                        e.fail_imports > 0)
                {
                    --e.fail_imports;
                    e.SendEvent(e.JobRecord(
                        job, verb, "failed", "import_failed"));
                }
                else
                {
                    if(verb == "commit")
                    {
                        /* The commit lands: adopt the submitted
                           document (reserialized like the real
                           engine — parse + re-emit). */
                        ++e.commits;
                        studio::StudioDocument d;
                        std::vector<std::string> errs;
                        if(studio::FromJson(e.last_commit_doc,
                                            d, &errs))
                        {
                            e.doc = studio::ToJson(d);
                        }
                        ++e.ws_rev;
                    }
                    else if(verb == "save_copy")
                    {
                        ++e.commits;   /* committed, not adopted */
                    }
                    e.SendEvent(e.JobRecord(job, verb,
                                            "succeeded"));
                }
                e.terminal_jobs.push_back(job);
            }
            else
            {
                e.held_job  = job;
                e.held_verb = verb;
            }
            if(late_accept)
            {
                send_accept();
            }
        }
        else if(verb == "job_status")
        {
            const uint64_t j =
                body.value("job_id", (uint64_t)0);
            if(e.job_status_not_found)
            {
                e.srv.SendMsg(MsgType::Ok, m.request_id,
                    nlohmann::json::object({
                        { "found", false }, { "job_id", j } }));
                break;
            }
            nlohmann::json st = e.JobRecord(
                j, e.held_verb.empty() ? "commit" : e.held_verb,
                e.held_job == j ? "running" : "succeeded");
            if(e.held_job != j)
            {
                st["committed"] = true;
                st["applied"]   = true;
            }
            e.srv.SendMsg(MsgType::Ok, m.request_id, st);
        }
        else
        {
            e.srv.SendMsg(MsgType::Error, m.request_id,
                nlohmann::json::object({
                    { "code", (uint32_t)ErrCode::BadRequest },
                    { "error",   "bad_request" },
                    { "message", "unknown verb" },
                    { "detail",  "bad_request" } }));
        }
        break;
    }
    default:
        e.srv.SendMsg(MsgType::Error, m.request_id,
            nlohmann::json::object({
                { "code", (uint32_t)ErrCode::BadRequest },
                { "error",   "bad_request" },
                { "message", "unexpected request" },
            }));
        break;
    }
    return true;
}

template <typename Pred>
static bool PumpBoth(WsEngine& e, Pred pred, int ms = 12000)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(ms);
    while(std::chrono::steady_clock::now() < deadline)
    {
        if(pred())
        {
            return true;
        }
        ServeWs(e, 10);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return pred();
}

/* A wired bridge+controller pair. Every connection attempt
   mints a fresh in-process pipe (a closed PipeEnd stays
   closed, exactly like a real pipe) and rebinds the scripted
   engine's peer — so engine.srv always talks to the CURRENT
   session after a reconnect. */
struct WsRig
{
    std::vector<std::shared_ptr<TestPipe>> pipes;
    WsEngine   engine{ TestPipe().Peer() };
    EngineBridge bridge;
    WorkspaceController ctl{ &bridge };

    WsRig()
    {
        auto p = std::make_shared<TestPipe>();
        pipes.push_back(p);
        engine.srv = p->Peer();
        bridge.setChannelFactoryForTesting(
            [this](std::string& err)
                -> std::unique_ptr<IIpcChannel>
            {
                (void)err;
                auto np = std::make_shared<TestPipe>();
                pipes.push_back(np);
                engine.srv = np->Peer();
                return np->ClientChannel();
            });
        EngineWireClient::Options wopts;
        wopts.keepalive_ms = 0;
        bridge.setWireOptionsForTesting(wopts);
        bridge.setBackoffForTesting({ 25 });
    }
    template <typename F> bool PumpUntil(F f, int ms = 12000)
    {
        return PumpBoth(engine, f, ms);
    }
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    /*========================================================*\
    || 1. WorkspaceEditSession (Qt-free)                      ||
    \*========================================================*/
    std::printf("-- session: load/dirty/edit\n");
    {
        WorkspaceEditSession s;
        CHECK(!s.HasWorking());
        CHECK(!s.Dirty());
        s.LoadApplied(ParseCompact(CompactDoc()), 5, 2);
        CHECK(s.HasWorking());
        CHECK(!s.Dirty());
        CHECK(s.BaseRevision() == 5);
        CHECK(s.ResourceRevision() == 2);
        CHECK(s.Working().meta.name == "desk");

        /* One committed gesture = one undo record. */
        CHECK(s.Editor().Select("fan0"));
        CHECK(s.Editor().BeginTransform());
        s.Editor().PreviewTranslate({ 0.05f, 0.0f, 0.0f });
        s.Editor().PreviewTranslate({ 0.10f, 0.0f, 0.0f });
        s.Editor().PreviewTranslate({ 0.15f, 0.0f, 0.0f });
        auto e = s.Editor().Commit();
        CHECK(e.has_value());
        CHECK(s.CommitEdit(*e));
        CHECK(s.UndoCount() == 1);
        CHECK(s.Dirty());
        CHECK(s.Working().devices.at("fan0").position.x >
              0.2f);

        CHECK(s.Undo());
        CHECK(!s.Dirty());
        CHECK(s.CanRedo());
        CHECK(s.UndoLabel() == "" && s.RedoLabel() != "");
        CHECK(s.Redo());
        CHECK(s.Dirty());
    }

    std::printf("-- session: gesture cancel leaves no history\n");
    {
        WorkspaceEditSession s;
        s.LoadApplied(ParseCompact(CompactDoc()), 1, 1);
        CHECK(s.Editor().Select("fan0"));
        CHECK(s.Editor().BeginTransform());
        s.Editor().PreviewTranslate({ 0.4f, 0, 0 });
        s.Editor().Cancel();
        CHECK(s.UndoCount() == 0);
        CHECK(!s.Dirty());
        CHECK(s.Working().devices.at("fan0").position.x ==
              0.1f);
    }

    std::printf("-- session: doc edits + discard\n");
    {
        WorkspaceEditSession s;
        s.LoadApplied(ParseCompact(CompactDoc()), 1, 1);
        CHECK(s.CommitDocEdit("rename scene",
            [](studio::StudioDocument& d) {
                d.meta.name = "renamed";
                return true;
            }));
        CHECK(s.Dirty());
        CHECK(s.Working().meta.name == "renamed");
        CHECK(s.UndoCount() == 1);
        /* A no-op mutation leaves no record. */
        CHECK(!s.CommitDocEdit("noop",
            [](studio::StudioDocument&) { return false; }));
        CHECK(s.UndoCount() == 1);
        CHECK(s.Undo());
        CHECK(s.Working().meta.name == "desk");
        s.Discard();
        CHECK(!s.Dirty());
        CHECK(s.UndoCount() == 0);
    }

    std::printf("-- session: remote baseline move under dirty\n");
    {
        WorkspaceEditSession s;
        s.LoadApplied(ParseCompact(CompactDoc()), 1, 1);
        CHECK(s.CommitDocEdit("rename", [](studio::StudioDocument& d) {
            d.meta.name = "mine"; return true; }));
        studio::StudioDocument remote =
            ParseCompact(CompactDoc("remote-name"));
        s.ApplyRemote(remote, 2, 3);
        /* Draft kept; baseline moved; dirty vs new baseline. */
        CHECK(s.Working().meta.name == "mine");
        CHECK(s.Baseline().meta.name == "remote-name");
        CHECK(s.Dirty());
        CHECK(s.BaseRevision() == 2);
        CHECK(s.ResourceRevision() == 3);
    }

    std::printf("-- session: own commit adoption keeps "
                "post-save edits\n");
    {
        WorkspaceEditSession s;
        s.LoadApplied(ParseCompact(CompactDoc()), 1, 1);
        /* edit -> commit snapshot taken -> MORE edits land */
        s.CommitDocEdit("rename", [](studio::StudioDocument& d) {
            d.meta.name = "saved-name"; return true; });
        const studio::StudioDocument committed = s.Working();
        s.CommitDocEdit("post", [](studio::StudioDocument& d) {
            d.brightness = 0.5f; return true; });
        s.AdoptCommitted(committed, 2, 1);
        CHECK(s.BaseRevision() == 2);
        CHECK(s.Baseline().meta.name == "saved-name");
        CHECK(s.Working().meta.name == "saved-name");
        CHECK(s.Working().brightness == 0.5f);
        CHECK(s.Dirty());   /* brightness edit still pending */
    }

    std::printf("-- session: history entry bound\n");
    {
        WorkspaceEditSession s;
        s.LoadApplied(ParseCompact(CompactDoc()), 1, 1);
        for(int i = 0; i < 40; ++i)
        {
            const std::string name =
                "n" + std::to_string(i);
            CHECK(s.CommitDocEdit("edit", [=](
                studio::StudioDocument& d) {
                    d.meta.name = name; return true; }));
        }
        CHECK(s.UndoCount() ==
              WorkspaceEditSession::kHistoryMaxEntries);
    }

    std::printf("-- session: history byte bound\n");
    {
        WorkspaceEditSession s;
        s.LoadApplied(ParseCompact(CompactDoc()), 1, 1);
        s.SetHistoryBoundsForTesting(64, 20000);
        for(int i = 0; i < 64; ++i)
        {
            const std::string blob(200, 'a' + (i % 26));
            CHECK(s.CommitDocEdit("big", [=](
                studio::StudioDocument& d) {
                    d.meta.extensions["blob"] = blob;
                    return true; }));
        }
        CHECK(s.HistoryBytes() <= 20000);
        CHECK(s.UndoCount() < 64);
    }

    /*========================================================*\
    || 2. WorkspaceDraftStore                                  ||
    \*========================================================*/
    std::printf("-- draft store: stage/flush/read/discard\n");
    {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        const studio::StudioDocument doc =
            ParseCompact(CompactDoc("drafted"));

        {
            WorkspaceDraftStore store;
            store.setRootForTesting(dir.path());
            store.setDebounceForTesting(25);
            CHECK(store.recordedIds().isEmpty());
            store.Stage(QStringLiteral("ws0"), 5, 2,
                        QString::fromStdString(
                            Sha256(DocBytes(doc))),
                        studio::ToJson(doc));
            /* Debounced: nothing durable until flush. */
            CHECK(store.Read(QStringLiteral("ws0")).exists
                  == false);
            store.Flush();
            const WorkspaceDraftRecord r =
                store.Read(QStringLiteral("ws0"));
            CHECK(r.exists);
            CHECK(r.baseline_revision == 5);
            CHECK(r.resource_revision == 2);
            CHECK(r.document.value("name", std::string()) ==
                  "drafted");
            CHECK(!r.baseline_sha256.isEmpty());
        }

        /* A fresh instance sees the durable record — the
           crash-survival path. */
        {
            WorkspaceDraftStore store2;
            store2.setRootForTesting(dir.path());
            CHECK(store2.recordedIds().contains(
                QStringLiteral("ws0")));
            const WorkspaceDraftRecord r =
                store2.Read(QStringLiteral("ws0"));
            CHECK(r.exists);
            store2.Discard(QStringLiteral("ws0"));
            CHECK(!store2.Read(QStringLiteral("ws0")).exists);
        }

        /* Corrupt record: reported + set aside, never guessed. */
        {
            WorkspaceDraftStore store3;
            store3.setRootForTesting(dir.path());
            store3.Stage(QStringLiteral("ws1"), 1, 1,
                         QString(), studio::ToJson(doc));
            store3.Flush();
            const QString f = dir.path() +
                QStringLiteral("/ws1.draft.json");
            CHECK(QFileInfo::exists(f));
            QFile bad(f);
            CHECK(bad.open(QIODevice::WriteOnly |
                           QIODevice::Truncate));
            bad.write("{ not json ");
            bad.close();
            const WorkspaceDraftRecord r =
                store3.Read(QStringLiteral("ws1"));
            CHECK(!r.exists);
            CHECK(!r.error.isEmpty());
            CHECK(QFileInfo::exists(f +
                  QStringLiteral(".rejected")));
        }
    }

    /*========================================================*\
    || 3. WorkspaceController over the wire                    ||
    \*========================================================*/
    std::printf("-- controller: capability, fetch, edit, save\n");
    {
        WsRig rig;
        QTemporaryDir dir;
        rig.ctl.setDraftDirForTesting(dir.path());
        rig.bridge.start();
        const bool attached = rig.PumpUntil([&] {
            return rig.ctl.managed() && rig.ctl.ready(); });
        if(!attached)
        {
            std::printf("  DBG avail=%d managed=%d ready=%d "
                        "conn=%d loading=%d err=%s dl_begins=%d\n",
                (int)rig.ctl.available(), (int)rig.ctl.managed(),
                (int)rig.ctl.ready(),
                (int)rig.bridge.connected(),
                (int)rig.ctl.loading(),
                rig.ctl.error().toStdString().c_str(),
                rig.engine.download_begins);
        }
        CHECK(attached);
        CHECK(rig.ctl.available());
        CHECK(rig.ctl.workspaceId() ==
              QStringLiteral("ws0"));
        CHECK(rig.ctl.workspaceRevision() == 1);
        CHECK(rig.engine.download_begins >= 1);
        CHECK(!rig.ctl.dirty());
        CHECK(rig.ctl.Session().Working().meta.name == "desk");

        /* Local edit -> dirty; nothing reaches the engine. */
        CHECK(rig.ctl.Session().CommitDocEdit("rename",
            [](studio::StudioDocument& d) {
                d.meta.name = "edited"; return true; }));
        rig.PumpUntil([&] { return rig.ctl.dirty(); });
        CHECK(rig.ctl.dirty());
        CHECK(rig.engine.commits == 0);
        CHECK(rig.engine.set_scene_calls == 0);

        /* Save -> commit job -> terminal event -> clean. */
        rig.ctl.save();
        CHECK(rig.PumpUntil([&] { return rig.ctl.saving(); }));
        CHECK(rig.PumpUntil([&] {
            return !rig.ctl.saving(); }));
        CHECK(!rig.ctl.dirty());
        CHECK(rig.ctl.error().isEmpty());
        CHECK(rig.engine.commits == 1);
        CHECK(rig.engine.last_base_rev == 1);
        CHECK(rig.ctl.workspaceRevision() == 2);
        CHECK(rig.ctl.Session().Working().meta.name ==
              "edited");
        /* committed baseline adopted — undo history intact */
        CHECK(rig.ctl.canUndo());
        /* The draft record cleared on a clean save. */
        rig.PumpUntil([&] {
            return rig.ctl.Drafts().recordedIds().isEmpty(); });
    }

    std::printf("-- controller: save rejection preserves draft\n");
    {
        WsRig rig;
        QTemporaryDir dir;
        rig.ctl.setDraftDirForTesting(dir.path());
        rig.engine.fail_next_commit = true;
        rig.engine.commit_fail_detail = "invalid_document";
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] { return rig.ctl.ready(); }));
        rig.ctl.Session().CommitDocEdit("rename",
            [](studio::StudioDocument& d) {
                d.meta.name = "kept"; return true; });
        rig.PumpUntil([&] { return rig.ctl.dirty(); });
        rig.ctl.save();
        CHECK(rig.PumpUntil([&] {
            return !rig.ctl.saving() &&
                   !rig.ctl.error().isEmpty(); }));
        CHECK(rig.ctl.dirty());
        CHECK(rig.ctl.Session().Working().meta.name == "kept");
    }

    std::printf("-- controller: revision_conflict -> conflict\n");
    {
        WsRig rig;
        QTemporaryDir dir;
        rig.ctl.setDraftDirForTesting(dir.path());
        rig.engine.fail_next_commit = true;
        rig.engine.commit_fail_detail = "revision_conflict";
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] { return rig.ctl.ready(); }));
        rig.ctl.Session().CommitDocEdit("rename",
            [](studio::StudioDocument& d) {
                d.meta.name = "mine"; return true; });
        rig.PumpUntil([&] { return rig.ctl.dirty(); });
        rig.ctl.save();
        CHECK(rig.PumpUntil([&] {
            return !rig.ctl.saving() && rig.ctl.conflict(); }));
        CHECK(rig.ctl.dirty());          /* draft preserved */
        /* Another client's commit arrives as a workspace
           event — the draft is still not clobbered. */
        rig.engine.doc["name"] = "other-client";
        ++rig.engine.ws_rev;
        rig.engine.SendEvent(rig.engine.Snapshot());
        CHECK(rig.PumpUntil([&] {
            return rig.ctl.workspaceRevision() ==
                   rig.engine.ws_rev; }));
        CHECK(rig.ctl.Session().Working().meta.name == "mine");
        CHECK(rig.ctl.conflict());
        /* Reload resolves it: engine doc wins, draft cleared. */
        rig.ctl.reload();
        CHECK(rig.PumpUntil([&] {
            return !rig.ctl.conflict() && !rig.ctl.dirty(); }));
        CHECK(rig.ctl.Session().Working().meta.name ==
              "other-client");
    }

    std::printf("-- controller: resource_install rebases the "
                "draft base silently\n");
    {
        /* F07: a resource_install job bumps resource_revision
           while the document bytes stand — the draft's commit
           base must advance so the NEXT save doesn't resubmit
           the stale pair and trip a phantom
           revision_conflict. */
        WsRig rig;
        QTemporaryDir dir;
        rig.ctl.setDraftDirForTesting(dir.path());
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] {
            return rig.ctl.managed() && rig.ctl.ready(); }));
        rig.ctl.Session().CommitDocEdit("rename",
            [](studio::StudioDocument& d) {
                d.meta.name = "kept"; return true; });
        rig.PumpUntil([&] { return rig.ctl.dirty(); });
        CHECK(rig.ctl.dirty());
        const int begins = rig.engine.download_begins;

        /* The terminal install record reports {ws 1, res 2}
           with committed:false — the controller rebases; no
           document fetch is issued (the bytes never moved). */
        ++rig.engine.res_rev;
        rig.engine.SendEvent(rig.engine.JobRecord(
            900, "resource_install", "succeeded"));
        CHECK(rig.PumpUntil([&] {
            return rig.ctl.resourceRevision() == 2; }));
        CHECK(rig.engine.download_begins == begins);
        CHECK(rig.ctl.dirty());
        CHECK(!rig.ctl.conflict());
        CHECK(rig.ctl.Session().ResourceRevision() == 2);

        /* The durable shadow re-staged with the rebased pair —
           a recovered draft is not born stale. */
        rig.ctl.FlushDrafts();
        const WorkspaceDraftRecord rec =
            rig.ctl.Drafts().Read(QStringLiteral("ws0"));
        CHECK(rec.exists);
        CHECK(rec.baseline_revision == 1);
        CHECK(rec.resource_revision == 2);

        /* The next save submits {1,2} and lands cleanly. */
        rig.ctl.save();
        CHECK(rig.PumpUntil([&] {
            return !rig.ctl.saving() &&
                   rig.engine.commits == 1; }));
        CHECK(rig.engine.last_base_rev == 1);
        CHECK(rig.engine.last_res_rev == 2);
        CHECK(!rig.ctl.conflict());
        CHECK(rig.ctl.error().isEmpty());
        CHECK(!rig.ctl.dirty());
    }

    std::printf("-- controller: a foreign-workspace job never "
                "moves our revisions\n");
    {
        /* A terminal record carrying another workspace's id
           reports THAT workspace's revisions — adopting them
           would corrupt this session's commit base and drive
           a bogus fetch. */
        WsRig rig;
        QTemporaryDir dir;
        rig.ctl.setDraftDirForTesting(dir.path());
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] {
            return rig.ctl.managed() && rig.ctl.ready(); }));
        rig.ctl.Session().CommitDocEdit("rename",
            [](studio::StudioDocument& d) {
                d.meta.name = "ours"; return true; });
        rig.PumpUntil([&] { return rig.ctl.dirty(); });
        const int begins = rig.engine.download_begins;

        nlohmann::json fj =
            rig.engine.JobRecord(901, "commit", "succeeded");
        fj["id"]                = "other-ws";
        fj["workspace_revision"] = 9;
        fj["resource_revision"]  = 9;
        rig.engine.SendEvent(fj);
        {
            /* Pump a beat — a wrongly-scoped fetch would reach
               the server inside this window. */
            const auto until = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(300);
            while(std::chrono::steady_clock::now() < until)
            {
                ServeWs(rig.engine, 15);
                QCoreApplication::processEvents(
                    QEventLoop::AllEvents, 10);
            }
        }
        CHECK(rig.ctl.workspaceRevision() == 1);
        CHECK(rig.ctl.resourceRevision() == 1);
        CHECK(rig.ctl.Session().BaseRevision() == 1);
        CHECK(rig.ctl.Session().ResourceRevision() == 1);
        CHECK(rig.engine.download_begins == begins);
        CHECK(!rig.ctl.conflict());
        CHECK(rig.ctl.dirty());
        CHECK(rig.ctl.Session().Working().meta.name == "ours");
    }

    std::printf("-- controller: reload after a failed save "
                "clears the error\n");
    {
        /* F07: a rejected save leaves its error up; taking the
           engine copy (reload -> adopted document) must clear
           it — the resolution the user asked for landed. */
        WsRig rig;
        QTemporaryDir dir;
        rig.ctl.setDraftDirForTesting(dir.path());
        rig.engine.fail_next_commit   = true;
        rig.engine.commit_fail_detail = "revision_conflict";
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] {
            return rig.ctl.managed() && rig.ctl.ready(); }));
        rig.ctl.Session().CommitDocEdit("rename",
            [](studio::StudioDocument& d) {
                d.meta.name = "mine"; return true; });
        rig.PumpUntil([&] { return rig.ctl.dirty(); });
        rig.ctl.save();
        CHECK(rig.PumpUntil([&] {
            return !rig.ctl.saving() && rig.ctl.conflict() &&
                   !rig.ctl.error().isEmpty(); }));

        /* Hold the reload, then resolve it with the engine's
           real terminal spelling — "succeeded". */
        rig.engine.auto_complete_jobs = false;
        rig.ctl.reload();
        CHECK(rig.PumpUntil([&] {
            return rig.engine.held_job != 0; }));
        rig.engine.CompleteHeld("succeeded");
        CHECK(rig.PumpUntil([&] {
            return !rig.ctl.conflict() && !rig.ctl.dirty(); }));
        CHECK(rig.ctl.error().isEmpty());
    }

    std::printf("-- controller: connected-but-unmanaged save "
                "is honest, not 'unavailable'\n");
    {
        /* F06: the engine is LIVE but manages nothing (e.g. a
           pre-fix restart). Save/reload must say so — the
           remedy is re-opening the workspace, and the draft
           stays local. */
        WsRig rig;
        QTemporaryDir dir;
        rig.ctl.setDraftDirForTesting(dir.path());
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] {
            return rig.ctl.managed() && rig.ctl.ready(); }));
        rig.ctl.Session().CommitDocEdit("rename",
            [](studio::StudioDocument& d) {
                d.meta.name = "kept"; return true; });
        rig.PumpUntil([&] { return rig.ctl.dirty(); });

        /* The engine reports managed:false — the workspace id
           is still known to the session. */
        rig.engine.managed = false;
        rig.engine.SendEvent(rig.engine.Snapshot());
        CHECK(rig.PumpUntil([&] { return !rig.ctl.managed(); }));

        rig.ctl.save();
        CHECK(rig.ctl.error().contains(
            QStringLiteral("reopen 'ws0'")));
        CHECK(!rig.ctl.error().contains(
            QStringLiteral("unavailable")));
        rig.ctl.reload();
        CHECK(rig.ctl.error().contains(
            QStringLiteral("reopen 'ws0'")));
        CHECK(rig.ctl.dirty());
    }

    std::printf("-- controller: foreign-open latch blocks a "
                "stale-id fetch\n");
    {
        /* Task 10 fix round 1: while a foreign open is latched
           (pending_switch_id_), a moved-revisions snapshot for
           the OLD workspace — another client's commit landing
           mid-switch — must not drive FetchDocument under the
           stale id. Without the guard the download would adopt
           under force_adopt_ and clean-Discard the staged
           draft record the replace latch promised survives. */
        WsRig rig;
        QTemporaryDir dir;
        rig.ctl.setDraftDirForTesting(dir.path());
        rig.ctl.setDebounceForTesting(10);
        rig.engine.auto_complete_jobs = false;   /* hold reload */
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] {
            return rig.ctl.managed() && rig.ctl.ready(); }));
        rig.ctl.Session().CommitDocEdit("rename",
            [](studio::StudioDocument& d) {
                d.meta.name = "keep"; return true; });
        rig.ctl.NoteDraftEdited();       /* stage the shadow */
        rig.PumpUntil([&] { return rig.ctl.dirty(); });
        rig.ctl.FlushDrafts();
        CHECK(!rig.ctl.Drafts().recordedIds().isEmpty());

        /* Confirm the foreign open — the held reload job
           keeps pending_switch_id_ latched on "shelf". */
        rig.ctl.openWorkspace(QStringLiteral("shelf"));
        CHECK(rig.ctl.replaceOffered());
        rig.ctl.confirmReplace();
        CHECK(rig.PumpUntil([&] {
            return rig.engine.held_job != 0; }));
        const int begins = rig.engine.download_begins;

        /* The OLD id's revisions move mid-switch. */
        rig.engine.doc["name"] = "other-client";
        ++rig.engine.ws_rev;
        rig.engine.SendEvent(rig.engine.Snapshot());
        {
            /* Pump a beat — a wrongly-issued transfer_begin
               would reach the server inside this window. */
            const auto until = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(300);
            while(std::chrono::steady_clock::now() < until)
            {
                ServeWs(rig.engine, 15);
                QCoreApplication::processEvents(
                    QEventLoop::AllEvents, 10);
            }
        }
        CHECK(rig.engine.download_begins == begins);
        CHECK(rig.ctl.dirty());
        CHECK(rig.ctl.Session().Working().meta.name == "keep");
        CHECK(rig.ctl.Drafts().Read(
            QStringLiteral("ws0")).exists);

        /* The switch resolves — the shelf snapshot drives the
           one legitimate fetch; the ws0 record survives. */
        rig.engine.ws_id  = "shelf";
        rig.engine.ws_rev = 1;
        rig.engine.doc    = CompactDoc("shelf-doc");
        rig.engine.CompleteHeld("succeeded");
        CHECK(rig.PumpUntil([&] {
            return rig.ctl.workspaceId() ==
                       QLatin1String("shelf") &&
                   rig.ctl.Session().HasWorking() &&
                   rig.ctl.Session().Working().meta.name ==
                       "shelf-doc"; }));
        CHECK(rig.ctl.Drafts().Read(
            QStringLiteral("ws0")).exists);
    }

    std::printf("-- controller: edits during save stay dirty\n");
    {
        WsRig rig;
        QTemporaryDir dir;
        rig.ctl.setDraftDirForTesting(dir.path());
        rig.engine.auto_complete_jobs = false;
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] { return rig.ctl.ready(); }));
        rig.ctl.Session().CommitDocEdit("first",
            [](studio::StudioDocument& d) {
                d.meta.name = "v1"; return true; });
        rig.PumpUntil([&] { return rig.ctl.dirty(); });
        rig.ctl.save();
        CHECK(rig.PumpUntil([&] { return rig.ctl.saving(); }));
        /* Wait for the commit to actually reach the engine —
           saving() flips synchronously at send time. */
        CHECK(rig.PumpUntil([&] { return rig.engine.held_job != 0; }));
        /* Edit while the job is in flight. */
        rig.ctl.Session().CommitDocEdit("second",
            [](studio::StudioDocument& d) {
                d.brightness = 0.5f; return true; });
        rig.engine.CompleteHeld("succeeded");
        CHECK(rig.PumpUntil([&] { return !rig.ctl.saving(); }));
        CHECK(rig.ctl.dirty());
        CHECK(rig.ctl.Session().Working().brightness ==
              0.5f);
        CHECK(rig.ctl.Session().Baseline().meta.name == "v1");
    }

    std::printf("-- controller: stale echo after reconnect "
                "cannot clobber\n");
    {
        /* A commit accepted but never resolved; the pipe drops;
           reconnect lands a snapshot that already carries the
           committed revision + matching content hash — the UI
           adopts it rather than resubmitting or losing it. */
        WsRig rig;
        QTemporaryDir dir;
        rig.ctl.setDraftDirForTesting(dir.path());
        rig.engine.auto_complete_jobs = false;
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] { return rig.ctl.ready(); }));
        rig.ctl.Session().CommitDocEdit("rename",
            [](studio::StudioDocument& d) {
                d.meta.name = "landed"; return true; });
        rig.PumpUntil([&] { return rig.ctl.dirty(); });
        rig.ctl.save();
        CHECK(rig.PumpUntil([&] { return rig.ctl.saving(); }));
        /* The engine actually committed it (canonical store
           bytes — the real engine re-serializes the accepted
           candidate), then the pipe dropped before the
           terminal event arrived. */
        CHECK(rig.PumpUntil([&] {
            return rig.engine.held_job != 0; }));
        {
            studio::StudioDocument cd;
            std::vector<std::string> errs;
            CHECK(studio::FromJson(rig.engine.last_commit_doc,
                                   cd, &errs));
            rig.engine.doc = studio::ToJson(cd);
        }
        ++rig.engine.ws_rev;
        rig.engine.srv.Close();       /* engine vanished */
        CHECK(rig.PumpUntil([&] {
            return !rig.ctl.saving(); }));   /* honest drop */
        CHECK(rig.ctl.dirty());              /* draft kept   */
        /* Reconnect: WELCOME+snapshot carry the NEW revision
           with our committed hash — reconcile adopts it. */
        CHECK(rig.PumpUntil([&] {
            return rig.ctl.managed() && rig.ctl.ready() &&
                   !rig.ctl.dirty(); }));
        CHECK(rig.ctl.Session().Working().meta.name ==
              "landed");
        CHECK(!rig.ctl.conflict());
    }

    /* Audit U-7: a terminal event that overtakes the accept
       reply (unknown job id at the time) must still resolve the
       save once the reply names it — never latch saving. */
    std::printf("-- controller: terminal before accept "
                "resolves the save (U-7)\n");
    {
        WsRig rig;
        QTemporaryDir dir;
        rig.ctl.setDraftDirForTesting(dir.path());
        rig.engine.terminal_before_accept = true;
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] {
            return rig.ctl.managed() && rig.ctl.ready(); }));
        rig.ctl.Session().CommitDocEdit("rename",
            [](studio::StudioDocument& d) {
                d.meta.name = "early"; return true; });
        rig.PumpUntil([&] { return rig.ctl.dirty(); });
        const int begins = rig.engine.download_begins;
        rig.ctl.save();
        CHECK(rig.PumpUntil([&] { return rig.engine.commits == 1; }));
        CHECK(rig.PumpUntil([&] { return !rig.ctl.saving(); }, 3000));
        rig.PumpUntil([&] { return false; }, 200);   /* drain */
        CHECK(!rig.ctl.dirty());
        CHECK(rig.ctl.error().isEmpty());
        CHECK(rig.ctl.Session().BaseRevision() == 2);
        /* Our own job: never treated as a foreign commit. */
        CHECK(rig.engine.download_begins == begins);
    }
    std::printf("-- controller: lost terminal event — the "
                "watchdog polls job_status (U-7)\n");
    {
        WsRig rig;
        QTemporaryDir dir;
        rig.ctl.setDraftDirForTesting(dir.path());
        rig.ctl.setJobWatchdogForTesting(60);
        rig.engine.auto_complete_jobs = false;
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] {
            return rig.ctl.managed() && rig.ctl.ready(); }));
        rig.ctl.Session().CommitDocEdit("rename",
            [](studio::StudioDocument& d) {
                d.meta.name = "lost"; return true; });
        rig.PumpUntil([&] { return rig.ctl.dirty(); });
        rig.ctl.save();
        CHECK(rig.PumpUntil([&] { return rig.engine.held_job != 0; }));
        /* The job completes but its event never arrives. */
        ++rig.engine.ws_rev;
        rig.engine.held_job = 0;
        CHECK(rig.PumpUntil([&] { return !rig.ctl.saving(); }, 4000));
        CHECK(!rig.ctl.dirty());
        CHECK(rig.ctl.Session().BaseRevision() == 2);
        CHECK(rig.ctl.Session().Baseline().meta.name == "lost");
    }
    std::printf("-- controller: a job the engine no longer "
                "knows fails honestly (U-7)\n");
    {
        WsRig rig;
        QTemporaryDir dir;
        rig.ctl.setDraftDirForTesting(dir.path());
        rig.ctl.setJobWatchdogForTesting(60);
        rig.engine.auto_complete_jobs = false;
        rig.engine.job_status_not_found = true;
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] {
            return rig.ctl.managed() && rig.ctl.ready(); }));
        rig.ctl.Session().CommitDocEdit("rename",
            [](studio::StudioDocument& d) {
                d.meta.name = "gone"; return true; });
        rig.PumpUntil([&] { return rig.ctl.dirty(); });
        bool finished_ok = true;
        int  finished    = 0;
        QObject::connect(&rig.ctl, &WorkspaceController::saveFinished,
                         [&](bool ok) { finished_ok = ok; ++finished; });
        rig.ctl.save();
        CHECK(rig.PumpUntil([&] { return rig.engine.held_job != 0; }));
        CHECK(rig.PumpUntil([&] { return !rig.ctl.saving(); }, 4000));
        CHECK(finished == 1);
        CHECK(!finished_ok);
        CHECK(!rig.ctl.error().isEmpty());
        CHECK(rig.ctl.dirty());   /* the draft is kept */
    }

    std::printf("-- controller: stale snapshot echo ignored\n");
    {
        WsRig rig;
        QTemporaryDir dir;
        rig.ctl.setDraftDirForTesting(dir.path());
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] { return rig.ctl.ready(); }));
        rig.ctl.Session().CommitDocEdit("e",
            [](studio::StudioDocument& d) {
                d.meta.name = "new"; return true; });
        rig.PumpUntil([&] { return rig.ctl.dirty(); });
        /* An out-of-order snapshot with the OLD revisions —
           must not touch the draft or the baseline. */
        rig.engine.ws_rev = 1;
        rig.engine.SendEvent(rig.engine.Snapshot());
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        CHECK(rig.ctl.dirty());
        CHECK(rig.ctl.Session().Working().meta.name == "new");
        CHECK(rig.ctl.Session().BaseRevision() == 1);
    }

    std::printf("-- controller: crash recovery, matching revs\n");
    {
        QTemporaryDir dir;
        const studio::StudioDocument crashed =
            ParseCompact(CompactDoc("crashed-draft"));
        {
            /* Simulated earlier session: a record flushed by
               debounce/normal close survives the crash. */
            WorkspaceDraftStore seed;
            seed.setRootForTesting(dir.path());
            seed.Stage(QStringLiteral("ws0"), 1, 1,
                QString::fromStdString(Sha256(DocBytes(crashed))),
                studio::ToJson(crashed));
            seed.Flush();
        }
        WsRig rig;
        rig.ctl.setDraftDirForTesting(dir.path());
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] {
            return rig.ctl.ready() && rig.ctl.recoveryOffered(); }));
        /* Offered — never applied: the engine saw nothing. */
        CHECK(rig.engine.commits == 0);
        CHECK(!rig.ctl.recoveryStale());
        CHECK(!rig.ctl.dirty());
        rig.ctl.recoverDraft();
        rig.PumpUntil([&] { return rig.ctl.dirty(); });
        CHECK(rig.ctl.Session().Working().meta.name ==
              "crashed-draft");
        CHECK(rig.engine.commits == 0);   /* still not sent */
    }

    std::printf("-- controller: crash recovery, moved revs\n");
    {
        QTemporaryDir dir;
        const studio::StudioDocument crashed =
            ParseCompact(CompactDoc("old-draft"));
        {
            WorkspaceDraftStore seed;
            seed.setRootForTesting(dir.path());
            seed.Stage(QStringLiteral("ws0"), 1, 1,
                QString::fromStdString(Sha256(DocBytes(crashed))),
                studio::ToJson(crashed));
            seed.Flush();
        }
        WsRig rig;
        rig.engine.ws_rev = 4;            /* engine moved on */
        rig.ctl.setDraftDirForTesting(dir.path());
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] {
            return rig.ctl.ready() && rig.ctl.recoveryOffered(); }));
        CHECK(rig.ctl.recoveryStale());
        rig.ctl.recoverDraft();
        rig.PumpUntil([&] { return rig.ctl.dirty(); });
        /* The draft recovers as pending edits over the NEW
           baseline — flagged, never silently applied. */
        CHECK(rig.ctl.Session().Working().meta.name ==
              "old-draft");
        CHECK(rig.ctl.Session().BaseRevision() == 4);
        CHECK(rig.engine.commits == 0);
    }

    std::printf("-- controller: missing engine — local draft "
                "editing + recovery\n");
    {
        QTemporaryDir dir;
        const studio::StudioDocument local =
            ParseCompact(CompactDoc("offline-draft"));
        {
            WorkspaceDraftStore seed;
            seed.setRootForTesting(dir.path());
            seed.Stage(QStringLiteral("wsx"), 3, 1,
                QString::fromStdString(Sha256(DocBytes(local))),
                studio::ToJson(local));
            seed.Flush();
        }
        /* No engine at all — the factory never supplies a
           channel. */
        std::shared_ptr<TestPipe> pipe =
            std::make_shared<TestPipe>();
        EngineBridge bridge;
        bridge.setChannelFactoryForTesting(
            [](std::string& err)
                -> std::unique_ptr<IIpcChannel>
            {
                err = "no engine";
                return nullptr;
            });
        bridge.setBackoffForTesting({ 30 });
        WorkspaceController ctl(&bridge);
        ctl.setDraftDirForTesting(dir.path());
        ctl.setDebounceForTesting(10);
        bridge.start();
        /* The durable record is offered without an engine. */
        {
            const auto deadline =
                std::chrono::steady_clock::now() +
                std::chrono::milliseconds(4000);
            while(std::chrono::steady_clock::now() < deadline &&
                  !ctl.recoveryOffered())
            {
                QCoreApplication::processEvents(
                    QEventLoop::AllEvents, 10);
            }
        }
        CHECK(ctl.recoveryOffered());
        ctl.recoverDraft();
        CHECK(ctl.Session().HasWorking());
        CHECK(ctl.Session().Working().meta.name ==
              "offline-draft");
        CHECK(ctl.dirty());
        /* A recovered draft is also LAYOUT-editable with no
           engine — the compact rows come from the working
           doc (no baseline exists to gate on), and ops
           commit into the same draft. */
        LayoutController lay(&bridge);
        lay.setWorkspace(&ctl);
        CHECK(ctl.ready());
        CHECK(!lay.items().isEmpty());
        CHECK(lay.moveObject(QStringLiteral("fan0"), 0.4, 0.3));
        CHECK(ctl.Session().Working()
                  .devices.at("fan0").position.x > 0.39f);
        /* Local edits keep working — and Save reports the
           honest unavailable state, never a fake ok. */
        CHECK(ctl.Session().CommitDocEdit("local",
            [](studio::StudioDocument& d) {
                d.brightness = 0.3f; return true; }));
        ctl.save();
        QCoreApplication::processEvents();
        CHECK(!ctl.error().isEmpty());
        CHECK(ctl.dirty());
        bridge.stop();
    }

    /* A draft recovered BEFORE the engine attaches (the UI starts
       first in a managed launch) must survive the first attach:
       on its own workspace it stays as pending edits over the
       engine's copy; on a different workspace its record is kept
       under its own id instead of being overwritten. */
    for(const bool same_ws : { true, false })
    {
        std::printf("-- controller: offline-recovered draft survives "
                    "attach (%s workspace)\n",
                    same_ws ? "same" : "other");
        QTemporaryDir dir;
        const studio::StudioDocument local =
            ParseCompact(CompactDoc("offline-draft"));
        {
            WorkspaceDraftStore seed;
            seed.setRootForTesting(dir.path());
            seed.Stage(QStringLiteral("wsx"), 1, 1,
                QString::fromStdString(Sha256(DocBytes(local))),
                studio::ToJson(local));
            seed.Flush();
        }
        WsRig rig;
        rig.engine.ws_id = same_ws ? "wsx" : "ws0";
        bool online = false;
        rig.bridge.setChannelFactoryForTesting(
            [&rig, &online](std::string& err)
                -> std::unique_ptr<IIpcChannel>
            {
                if(!online)
                {
                    err = "no engine yet";
                    return nullptr;
                }
                auto np = std::make_shared<TestPipe>();
                rig.pipes.push_back(np);
                rig.engine.srv = np->Peer();
                return np->ClientChannel();
            });
        rig.ctl.setDraftDirForTesting(dir.path());
        rig.ctl.setDebounceForTesting(10);
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] {
            return rig.ctl.recoveryOffered(); }, 4000));
        rig.ctl.recoverDraft();
        CHECK(rig.ctl.dirty());
        CHECK(rig.ctl.Session().Working().meta.name ==
              "offline-draft");

        online = true;
        CHECK(rig.PumpUntil([&] {
            return rig.ctl.managed() &&
                   rig.ctl.Session().HasBaseline() &&
                   !rig.ctl.loading(); }));
        rig.PumpUntil([] { return false; }, 200);   /* settle */
        rig.ctl.FlushDrafts();
        if(same_ws)
        {
            /* Pending edits over the engine baseline — not
               replaced, not a false "changed under you". */
            CHECK(rig.ctl.dirty());
            CHECK(rig.ctl.Session().Working().meta.name ==
                  "offline-draft");
            CHECK(rig.ctl.Session().Baseline().meta.name == "desk");
            CHECK(!rig.ctl.conflict());
        }
        else
        {
            /* The engine's own workspace loads; the foreign
               draft is never staged into it ... */
            CHECK(!rig.ctl.dirty());
            CHECK(rig.ctl.Session().Working().meta.name == "desk");
        }
        /* ... and in both cases the edits are on disk under the
           workspace they belong to. */
        WorkspaceDraftRecord rec =
            rig.ctl.Drafts().Read(QStringLiteral("wsx"));
        studio::StudioDocument kept;
        CHECK(rec.exists &&
              studio::FromJson(rec.document, kept) &&
              kept.meta.name == "offline-draft");
        CHECK(rig.engine.commits == 0);
        rig.bridge.stop();
    }

    std::printf("-- controller: library import retains "
                "originals + names\n");
    {
        QTemporaryDir dir;
        ProfileLibrary lib;
        lib.setPathForTesting(dir.filePath("lib.json"));
        lib.saveProfile("Evening",
            JsonToVariant(CompactDoc("evening-doc")).toMap());
        lib.saveProfile("Desk",
            JsonToVariant(CompactDoc("desk-doc")).toMap());

        WsRig rig;
        rig.ctl.setDraftDirForTesting(
            dir.filePath("drafts"));
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] { return rig.ctl.ready(); }));
        const int n = rig.ctl.importProfiles(&lib);
        CHECK(n == 2);
        CHECK(rig.PumpUntil([&] {
            return rig.engine.import_commits == 2; }));
        /* originals retained — the library is untouched */
        CHECK(lib.entries().size() == 2);
        CHECK(lib.profileDocument("Evening")
                  .value("name").toString() ==
              QStringLiteral("evening-doc"));
    }

    std::printf("-- controller: legacy engine fallback\n");
    {
        WsRig rig;
        rig.engine.cap = false;      /* no studio_workspace_v1 */
        QTemporaryDir dir;
        rig.ctl.setDraftDirForTesting(dir.path());
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] { return rig.bridge.connected(); }));
        CHECK(!rig.ctl.available());
        CHECK(!rig.ctl.managed());
        CHECK(!rig.ctl.ready());
        rig.ctl.save();              /* honest no-op */
        QCoreApplication::processEvents();
        CHECK(rig.engine.commits == 0);
        CHECK(rig.engine.download_begins == 0);
    }

    /*========================================================*\
    || 4. Page adapters over the shared draft                 ||
    ||    Lighting + Layout mutate the SAME session — one     ||
    ||    dirty flag, one undo timeline, one commit path;     ||
    ||    legacy engines fall back to SET_SCENE/              ||
    ||    LAYOUT_COMMAND verbatim.                            ||
    \*========================================================*/
    std::printf("-- adapters: lighting+layout share the "
                "draft\n");
    {
        WsRig rig;
        QTemporaryDir dir;
        rig.ctl.setDraftDirForTesting(dir.path());
        SceneEditController scene(&rig.bridge);
        LayoutController    layout(&rig.bridge);
        scene.setWorkspace(&rig.ctl);
        layout.setWorkspace(&rig.ctl);
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] { return rig.ctl.ready(); }));
        CHECK(scene.hasWorking());
        CHECK(!scene.dirty());
        CHECK(!scene.conflict());

        /* A Lighting edit marks the shared draft — the
           workspace + layout see the same truth. */
        scene.setBrightness(0.5);
        rig.PumpUntil([&] { return rig.ctl.dirty(); });
        CHECK(scene.dirty());
        CHECK(rig.ctl.dirty());
        CHECK(rig.engine.set_scene_calls == 0);
        CHECK(rig.engine.layout_calls == 0);
        CHECK(scene.brightness() > 0.49 &&
              scene.brightness() < 0.51);
        CHECK(scene.sceneName() == QStringLiteral("desk"));

        /* Layout op commits through the same session — one
           undo timeline across both pages. */
        CHECK(layout.moveObject(QStringLiteral("fan0"),
                                0.4, 0.3));
        CHECK(rig.ctl.Session().Working()
                  .devices.at("fan0").position.x > 0.39f);
        CHECK(rig.ctl.Session().UndoCount() == 2);
        CHECK(layout.canUndo());
        CHECK(layout.undoLabel() == QStringLiteral("move fan0"));
        CHECK(rig.engine.layout_calls == 0);

        /* Layout undo reverts the shared draft — the lighting
           edit stays pending. */
        layout.undo();
        CHECK(rig.ctl.Session().UndoCount() == 1);
        CHECK(rig.ctl.Session().Working()
                  .devices.at("fan0").position.x < 0.11f);
        CHECK(scene.dirty());

        /* Save via the Lighting page's path commits through
           WORKSPACE_COMMAND — never SET_SCENE. */
        scene.save();
        CHECK(rig.PumpUntil([&] {
            return !rig.ctl.saving() && !rig.ctl.dirty(); }));
        CHECK(rig.engine.commits == 1);
        CHECK(rig.engine.set_scene_calls == 0);
        CHECK(rig.engine.last_commit_doc.value(
                  "output", nlohmann::json())
                  .value("brightness", 1.0) == 0.5);
        CHECK(!scene.dirty());
        CHECK(scene.saveError().isEmpty());

        /* Layout undo across the save boundary — history
           survives adoption. */
        CHECK(layout.canUndo());
        layout.undo();
        CHECK(rig.ctl.dirty());
        CHECK(rig.ctl.Session().Working().brightness > 0.99f);
    }

    std::printf("-- adapters: remote move surfaces on both "
                "pages\n");
    {
        WsRig rig;
        QTemporaryDir dir;
        rig.ctl.setDraftDirForTesting(dir.path());
        SceneEditController scene(&rig.bridge);
        LayoutController    layout(&rig.bridge);
        scene.setWorkspace(&rig.ctl);
        layout.setWorkspace(&rig.ctl);
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] { return rig.ctl.ready(); }));
        scene.setEffectIntensity(0.4);
        rig.PumpUntil([&] { return rig.ctl.dirty(); });
        /* Another client commits — the draft is preserved and
           BOTH pages report the same conflict. */
        rig.engine.doc["name"] = "other";
        ++rig.engine.ws_rev;
        rig.engine.SendEvent(rig.engine.Snapshot());
        CHECK(rig.PumpUntil([&] {
            return rig.ctl.workspaceRevision() ==
                   rig.engine.ws_rev &&
                   rig.ctl.conflict(); }));
        CHECK(scene.conflict());
        CHECK(scene.dirty());
        /* Layout rows still come from the DRAFT, not the
           remote baseline. */
        bool saw_fan0 = false;
        for(const QVariant& v : layout.items())
        {
            if(v.toMap().value("id").toString() ==
               QLatin1String("fan0"))
            {
                saw_fan0 = true;
            }
        }
        CHECK(saw_fan0);
    }

    std::printf("-- adapters: legacy engine keeps legacy "
                "paths\n");
    {
        WsRig rig;
        rig.engine.cap = false;
        /* A loaded expanded scene — the shape GET_SCENE serves
           under `document` (scene schema v2). */
        const nlohmann::json pos = nlohmann::json::object(
            {{ "x", 0.1 }, { "y", 0.0 }, { "z", 0.2 }});
        rig.engine.scene_doc = nlohmann::json::object({
            { "version",    2 },
            { "name",       "test-scene" },
            { "brightness", 1.0 },
            { "bindings", nlohmann::json::array({
                nlohmann::json::object({
                    { "id", "slw0" },
                    { "controller_name", "UNI FAN" },
                    { "zone_name", "Fan 1" },
                    { "zone_leds", 40 } }) }) },
            { "objects", nlohmann::json::array({
                nlohmann::json::object({
                    { "id", "fan0" }, { "kind", "device" },
                    { "position", pos },
                    { "binding",  "slw0" },
                    { "verified", true },
                    { "emitters", nlohmann::json::array() } }),
            }) },
            { "object_colors", nlohmann::json::object() },
            { "effect", nlohmann::json::object({
                { "preset", "aurora" }, { "speed", 1.0 },
                { "intensity", 1.0 }, { "playing", true },
                { "layers", nlohmann::json::array() } }) },
        });
        QTemporaryDir dir;
        rig.ctl.setDraftDirForTesting(dir.path());
        SceneEditController scene(&rig.bridge);
        LayoutController    layout(&rig.bridge);
        scene.setWorkspace(&rig.ctl);
        layout.setWorkspace(&rig.ctl);
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] {
            return rig.bridge.connected(); }));
        CHECK(!rig.ctl.available());
        /* The model path is untouched — GET_SCENE loads the
           working copy, edits stage in it, and save rides
           SET_SCENE (never WORKSPACE_COMMAND). */
        scene.reloadFromEngine();
        CHECK(rig.PumpUntil([&] { return scene.hasWorking(); }));
        scene.setBrightness(0.5);
        CHECK(scene.dirty());
        scene.save();
        CHECK(rig.PumpUntil([&] {
            return rig.engine.set_scene_calls >= 1 &&
                   !scene.saving(); }));
        CHECK(rig.engine.set_scene_calls == 1);
        CHECK(rig.engine.commits == 0);
        /* The refused save keeps the edit + surfaces the
           engine's error. */
        CHECK(scene.dirty());
        CHECK(!scene.saveError().isEmpty());
        /* A layout op still takes the legacy LAYOUT_COMMAND
           path on the loaded scene doc. */
        CHECK(layout.moveObject(QStringLiteral("fan0"),
                                0.4, 0.3));
        CHECK(rig.PumpUntil([&] {
            return rig.engine.layout_calls >= 1; }));
        CHECK(rig.engine.commits == 0);
        CHECK(!rig.ctl.Session().HasWorking());
    }

    std::printf("-- recovery: foreign-workspace record is "
                "never offered under a managed session\n");
    {
        QTemporaryDir dir;
        const studio::StudioDocument other =
            ParseCompact(CompactDoc("other-doc"));
        {
            /* A crash record for a DIFFERENT workspace —
               staged under its own id. */
            WorkspaceDraftStore seed;
            seed.setRootForTesting(dir.path());
            seed.Stage(QStringLiteral("ws_foreign"), 1, 1,
                QString::fromStdString(Sha256(DocBytes(other))),
                studio::ToJson(other));
            seed.Flush();
        }
        WsRig rig;
        rig.ctl.setDraftDirForTesting(dir.path());
        rig.bridge.start();
        /* Attach completes: managed ws0, doc fetched clean.
           The foreign record must never be offered — staging
           it would commit another workspace's document as
           ws0's content on the next save. */
        CHECK(rig.PumpUntil([&] { return rig.ctl.ready(); }));
        for(int i = 0; i < 20; ++i)
        {
            ServeWs(rig.engine, 5);
            QCoreApplication::processEvents();
        }
        CHECK(!rig.ctl.recoveryOffered());
        CHECK(!rig.ctl.dirty());
        CHECK(rig.ctl.Session().Working().meta.name == "desk");

        /* Its own record still surfaces normally. */
        const studio::StudioDocument mine =
            ParseCompact(CompactDoc("mine-doc"));
        {
            WorkspaceDraftStore seed;
            seed.setRootForTesting(dir.path());
            seed.Stage(QStringLiteral("ws0"), 1, 1,
                QString::fromStdString(Sha256(DocBytes(mine))),
                studio::ToJson(mine));
            seed.Flush();
        }
        /* Re-check is driven by another snapshot. */
        rig.engine.SendEvent(rig.engine.Snapshot());
        CHECK(rig.PumpUntil([&] {
            return rig.ctl.recoveryOffered(); }));
        rig.ctl.recoverDraft();
        CHECK(rig.ctl.Session().Working().meta.name ==
              "mine-doc");
    }

    std::printf("-- controller: failed import keeps the "
                "queue moving\n");
    {
        QTemporaryDir dir;
        ProfileLibrary lib;
        lib.setPathForTesting(dir.filePath("lib.json"));
        lib.saveProfile("First",
            JsonToVariant(CompactDoc("first-doc")).toMap());
        lib.saveProfile("Second",
            JsonToVariant(CompactDoc("second-doc")).toMap());

        WsRig rig;
        rig.ctl.setDraftDirForTesting(dir.filePath("drafts"));
        rig.engine.fail_imports = 1;   /* first commit fails */
        int finished = 0;
        int ok_count = 0;
        QStringList names;
        QObject::connect(&rig.ctl,
            &WorkspaceController::importFinished, &rig.ctl,
            [&](const QString& n, bool ok, const QString& e) {
                (void)e;
                ++finished;
                if(ok)
                {
                    ++ok_count;
                }
                names.push_back(n);
            });
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] { return rig.ctl.ready(); }));
        CHECK(rig.ctl.importProfiles(&lib) == 2);
        /* Both jobs reach the engine — the failed first never
           stalls the second. */
        CHECK(rig.PumpUntil([&] {
            return rig.engine.import_commits == 2; }));
        CHECK(rig.PumpUntil([&] { return finished >= 2; }));
        CHECK(ok_count == 1);
        /* Both entries reported BY NAME — an empty name is
           the regression being guarded against. */
        CHECK(names.size() == 2);
        CHECK(!names[0].isEmpty());
        CHECK(!names[1].isEmpty());
        CHECK(lib.entries().size() == 2);   /* originals kept */
    }

    std::printf("-- adapters: page switching preserves draft "
                "+ selection\n");
    {
        WsRig rig;
        QTemporaryDir dir;
        rig.ctl.setDraftDirForTesting(dir.path());
        SceneEditController scene(&rig.bridge);
        LayoutController    layout(&rig.bridge);
        scene.setWorkspace(&rig.ctl);
        layout.setWorkspace(&rig.ctl);
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] { return rig.ctl.ready(); }));

        /* Lighting edits, Layout selects — then the user flips
           pages. Both live in shared controller state: the
           draft and the selection are never per-view. */
        scene.setBrightness(0.5);
        rig.PumpUntil([&] { return rig.ctl.dirty(); });
        layout.select(QStringLiteral("fan0"));
        /* Layout "page" binds: rows + selection intact. */
        CHECK(layout.items().size() == 2);
        CHECK(layout.selectedId() == QStringLiteral("fan0"));
        /* Lighting "page" re-binds: the same draft projects
           back — dirty flag and edited field both live. */
        CHECK(scene.dirty());
        CHECK(scene.workingDocument()
                  .value(QStringLiteral("brightness"))
                  .toDouble() > 0.49);
        /* Round-trip back: nothing refetched, nothing lost. */
        CHECK(layout.selectedId() == QStringLiteral("fan0"));
        CHECK(rig.ctl.dirty());
        CHECK(rig.engine.download_begins == 1);
        CHECK(rig.ctl.Session().UndoCount() == 1);
    }

    std::printf("-- adapters: emitter rows + render-only "
                "calibration in ws mode\n");
    {
        WsRig rig;
        QTemporaryDir dir;
        rig.ctl.setDraftDirForTesting(dir.path());
        LayoutController layout(&rig.bridge);
        layout.setWorkspace(&rig.ctl);
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] { return rig.ctl.ready(); }));
        /* Feed the resolved applied scene — emitter data is
           engine truth the compact doc doesn't carry. */
        const nlohmann::json pos = nlohmann::json::object(
            {{ "x", 0.1 }, { "y", 0.0 }, { "z", 0.2 }});
        const nlohmann::json scene_doc = nlohmann::json::object({
            { "version",    2 },
            { "name",       "test-scene" },
            { "brightness", 1.0 },
            { "bindings", nlohmann::json::array({
                nlohmann::json::object({
                    { "id", "slw0" },
                    { "controller_name", "UNI FAN" },
                    { "zone_name", "Fan 1" },
                    { "zone_leds", 40 } }) }) },
            { "objects", nlohmann::json::array({
                nlohmann::json::object({
                    { "id", "fan0" }, { "kind", "device" },
                    { "position", pos },
                    { "binding",  "slw0" },
                    { "verified", true },
                    { "emitters", nlohmann::json::array({
                        nlohmann::json::object(
                            {{ "address", 0 }}),
                        nlohmann::json::object(
                            {{ "address", 1 }}),
                        nlohmann::json::object(
                            {{ "address", -1 }}),
                    }) } }),
            }) },
            { "object_colors", nlohmann::json::object() },
            { "effect", nlohmann::json::object({
                { "preset", "aurora" }, { "speed", 1.0 },
                { "intensity", 1.0 }, { "playing", true },
                { "layers", nlohmann::json::array() } }) },
        });
        rig.engine.srv.SendMsg(MsgType::Event, 0,
            nlohmann::json::object({
                { "kind", "scene" },
                { "data", nlohmann::json::object({
                    { "loaded",   true },
                    { "revision", 1 },
                    { "document", scene_doc } }) } }));
        /* Pump until the event lands — row counts then come
           from the resolved doc: 3 emitters, 2 addressed. */
        auto row_for_fan0 = [&]() -> QVariantMap {
            for(const QVariant& v : layout.items())
            {
                const QVariantMap m = v.toMap();
                if(m.value("id").toString() ==
                   QLatin1String("fan0"))
                {
                    return m;
                }
            }
            return QVariantMap();
        };
        CHECK(rig.PumpUntil([&] {
            return row_for_fan0().value("emitter_count")
                       .toInt() == 3; }));
        const QVariantMap fan0 = row_for_fan0();
        CHECK(!fan0.isEmpty());
        CHECK(fan0.value("emitter_count").toInt() == 3);
        CHECK(fan0.value("addressed_count").toInt() == 2);
        /* -1 = render-only is a legal calibration input in ws
           mode — expressed honestly as unbinding the
           attributed zone row. */
        CHECK(layout.calibrateEmitter(QStringLiteral("fan0"),
                                      0, -1));
        CHECK(rig.ctl.Session().Working()
                  .device_settings.at("fan0")
                  .zones.empty());
        CHECK(rig.ctl.dirty());
        /* Undo restores the bound row. */
        layout.undo();
        CHECK(rig.ctl.Session().Working()
                  .device_settings.at("fan0")
                  .zones.count("ring") == 1);
    }

    std::printf("ui_workspace_test: %d checks, %d failures\n",
                checks, failures);
    return failures ? 1 : 0;
}
