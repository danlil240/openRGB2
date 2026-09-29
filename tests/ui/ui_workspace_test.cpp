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
    bool          fail_next_commit   = false;
    std::string   commit_fail_detail = "invalid_document";
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
            e.srv.SendMsg(MsgType::Ok, m.request_id,
                nlohmann::json::object({
                    { "accepted", true },
                    { "job_id",   job  },
                }));
            if(e.auto_complete_jobs)
            {
                if(verb == "commit" && e.fail_next_commit)
                {
                    e.fail_next_commit = false;
                    e.SendEvent(e.JobRecord(
                        job, verb, "failed", e.commit_fail_detail));
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
        }
        else if(verb == "job_status")
        {
            const uint64_t j =
                body.value("job_id", (uint64_t)0);
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

    std::printf("ui_workspace_test: %d checks, %d failures\n",
                checks, failures);
    return failures ? 1 : 0;
}
