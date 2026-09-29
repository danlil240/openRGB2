// SPDX-License-Identifier: GPL-2.0-or-later
//
// WORKSPACE_COMMAND tests (studio-import Task 3, plan section
// 3.3): the real EngineService + WorkspaceService over real
// IpcServer sessions on in-process channels.
//
// Covers the brief's acceptance list:
//   - capability negotiation ("studio_workspace_v1" in
//     WELCOME) + missing-capability fallback (no workspace
//     service -> no capability, WORKSPACE_COMMAND -> unavailable;
//     old verbs keep working on a new server)
//   - staged-candidate commit with base/resource revision
//     checks; stale -> "revision_conflict"; two-client and
//     out-of-order writers never overwrite newer work
//   - session-bound bounded transfers: ordered chunks, hash +
//     length verification, per-session and aggregate caps,
//     idle expiry, no client-chosen paths
//   - one bounded job worker: queued jobs + staging cancelled
//     on disconnect; a commit past the durable marker is
//     reconciled by revision/content hash after reconnect —
//     never rolled back, never blindly resubmitted
//   - bounded staging under load never stalls basic controls:
//     PING answers alongside a slow workspace job
//   - diagnostics channel stays read-only (WORKSPACE_COMMAND
//     -> read_only)
//   - managed-workspace gate: SET_SCENE and layout mutations
//     are refused with "workspace_required" once a workspace
//     is active; GET_SCENE, identify, telemetry stay
//   - preview verbs (Task 7 wired them for real) answer with
//     honest pre-session errors — the full bounded-preview
//     surface lives in preview_session_test

#include "IpcProtocol.h"
#include "IpcServer.h"
#include "EngineService.h"
#include "WorkspaceTransfer.h"

#include "Engine.h"
#include "WorkspaceService.h"

#include "config/StudioConfig.h"
#include "presets/PresetRegistry.h"
#include "presets/EffectRegistry.h"

#include "fake_ipc_channel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace openrgb2;
using namespace openrgb2::ipc;
using namespace ipc_test;
using nlohmann::json;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

namespace fs = std::filesystem;

static fs::path g_tmp;

/*---------------------------------------------------------*\
||| Fixtures                                                 ||
\*---------------------------------------------------------*/

/* One device type on disk so compact documents resolve. */
static void WriteTestType(const fs::path& dir, int leds)
{
    fs::create_directories(dir);
    const json ent = json::object({
        { "geometry", "fan_body" },
        { "zone",     "ring"     },
    });
    const json doc = json::object({
        { "schema_version", 1          },
        { "id",             "test-fan" },
        { "name",           "Test Fan" },
        { "category",       "fan"      },
        { "entities", json::object({{ "body", ent }}) },
        { "zones", json::array({ json::object({
              { "id",        "ring" },
              { "entity",    "body" },
              { "led_count", leds   },
              { "layout", json::object({
                    { "type",     "ring" },
                    { "radius_m", 0.05  } }) } }) }) },
    });
    std::ofstream(dir / "test-fan.device.json") << doc.dump(2);
}

/* A minimal compact v3 workspace document. `pad` lands in the
   verbatim-retained extensions section so byte-size tests can
   grow the document without changing its meaning. */
static json WorkspaceDoc(const std::string& name,
                         size_t pad = 0)
{
    json doc = json::object({
        { "schema_version", 3    },
        { "name",           name },
        { "devices", json::object({
              { "fan", json::object({
                    { "type", "test-fan" },
                    { "x", 0.1 }, { "y", 0.05 }, { "z", -0.02 },
                    { "rx", 0.0 }, { "ry", 30.0 }, { "rz", 0.0 } }) } }) },
        { "bindings", json::object({
              { "b1", json::object({
                    { "controller_name", "Test Controller" },
                    { "zone_name",       "ring"            },
                    { "zone_leds",       8                 } }) } }) },
        { "device_settings", json::object({
              { "fan", json::object({
                    { "zones", json::object({
                          { "ring", json::object({
                                { "binding",   "b1"   },
                                { "addr_base", 0      },
                                { "verified",  true   } }) } }) } }) } }) },
    });
    if(pad)
    {
        doc["extensions"] = json::object(
            {{ "pad", std::string(pad, 'p') }});
    }
    return doc;
}

struct Rig
{
    openrgb2::Engine                 engine;
    studio::PresetRegistry           devices;
    WorkspaceService::Deps           wdeps;
    EngineService::Deps              edeps;
    EngineServiceOptions             eopts;
    IpcServerOptions                 sopts;
    std::unique_ptr<WorkspaceService> ws;
    std::unique_ptr<EngineService>    svc;
    std::unique_ptr<IpcServer>        server;

    Rig(const fs::path& root)
    {
        std::vector<std::string> errs;
        devices.LoadDirectory((g_tmp / "types").string(), &errs);
        CHECK(errs.empty());
        wdeps.devices = devices;
        /* The workspace publish seam is the SAME scene-apply
           path as SET_SCENE — wired lazily because the service
           object is constructed after the workspace service. */
        wdeps.apply_scene =
            [this](std::shared_ptr<const WorkspaceCandidate> cand,
                   std::string& err) -> bool
            {
                return svc && cand &&
                       svc->ApplyWorkspaceCandidate(*cand, err);
            };
        ws = std::make_unique<WorkspaceService>(root.string(),
                                                wdeps);
        edeps.workspace = ws.get();
    }

    void Start()
    {
        svc    = std::make_unique<EngineService>(engine, edeps,
                                                 eopts);
        server = std::make_unique<IpcServer>(*svc, sopts);
        svc->SetEventSink(
            [this](const IpcEvent& ev) { server->Emit(ev); });
    }
};

static json Welcome(TestClient& c, IpcServer& server,
                    ChannelKind kind = ChannelKind::Command)
{
    CHECK(server.Serve(c.MakeChannel(), kind));
    c.Hello();
    Message m;
    CHECK(c.RecvMsg(m, 5000));
    CHECK(m.type == MsgType::Welcome);
    return m.Body();
}

/* Send a WORKSPACE_COMMAND; seq carries the request id. */
static void WsSend(TestClient& c, uint32_t& seq, const json& body)
{
    c.SendMsg(MsgType::WorkspaceCommand, ++seq, body);
}

/* Read until the reply for `id` arrives; EVENTs are collected
   in `events` when given. Returns the reply message. */
static bool RecvReply(TestClient& c, uint32_t id, Message& out,
                      std::vector<json>* events = nullptr,
                      uint32_t timeout_ms = 8000)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeout_ms);
    while(std::chrono::steady_clock::now() < deadline)
    {
        Message m;
        if(!c.RecvMsg(m, 1000))
        {
            continue;
        }
        if(m.type == MsgType::Event)
        {
            if(events)
            {
                events->push_back(m.Body());
            }
            continue;
        }
        if(m.request_id == id)
        {
            out = m;
            return true;
        }
    }
    return false;
}

static json WsCall(TestClient& c, uint32_t& seq, const json& body,
                   std::vector<json>* events = nullptr)
{
    WsSend(c, seq, body);
    Message m;
    CHECK(RecvReply(c, seq, m, events));
    return m.Body();
}

static bool IsErr(const Message& m, ErrCode code,
                  const char* detail = nullptr)
{
    if(m.type != MsgType::Error)
    {
        return false;
    }
    const json b = m.Body();
    if(b.is_discarded())
    {
        return false;
    }
    if(b.value("code", (uint32_t)0xFFFFFFFF) != (uint32_t)code)
    {
        return false;
    }
    if(detail && b.value("detail", std::string()) != detail)
    {
        return false;
    }
    return true;
}

static json WsCallErr(TestClient& c, uint32_t& seq,
                      const json& body, ErrCode code,
                      const char* detail = nullptr)
{
    WsSend(c, seq, body);
    Message m;
    CHECK(RecvReply(c, seq, m));
    CHECK(IsErr(m, code, detail));
    return m.Body();
}

/* Poll job_status until the job reaches a terminal state;
   returns the last status body. EVENTs arriving mid-poll are
   collected into `events` when given — never dropped, so the
   completion event can't be lost to polling. */
static json WaitJob(TestClient& c, uint32_t& seq, uint64_t job_id,
                    uint32_t timeout_ms = 10000,
                    std::vector<json>* events = nullptr)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeout_ms);
    json last;
    while(std::chrono::steady_clock::now() < deadline)
    {
        const json r = WsCall(c, seq, json::object(
            {{ "verb",   "job_status" },
             { "job_id", job_id       }}), events);
        last = r;
        const std::string st = r.value("state", std::string());
        if(st == "succeeded" || st == "failed" ||
           st == "cancelled")
        {
            return r;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::printf("FAIL: job %llu never reached a terminal state\n",
                (unsigned long long)job_id);
    ++checks; ++failures;
    return last;
}

/* Full upload: begin -> ordered chunks -> finish. Returns the
   staged token ("" on any step failure). */
static std::string Upload(TestClient& c, uint32_t& seq,
                          const std::string& kind,
                          const std::string& bytes)
{
    const json r = WsCall(c, seq, json::object({
        { "verb",      "transfer_begin"              },
        { "direction", "upload"                      },
        { "kind",      kind                          },
        { "length",    (uint64_t)bytes.size()        },
        { "sha256",    Sha256Hex(bytes)              },
    }));
    if(!r.value("transfer_id", std::string()).size())
    {
        return "";
    }
    const std::string tid = r["transfer_id"].get<std::string>();
    size_t off = 0;
    while(off < bytes.size())
    {
        const size_t n = std::min<size_t>(bytes.size() - off,
                                          120 * 1024);
        const json cr = WsCall(c, seq, json::object({
            { "verb",        "transfer_chunk" },
            { "transfer_id", tid              },
            { "offset",      (uint64_t)off    },
            { "data",        Base64Encode(
                                 bytes.data() + off, n) },
        }));
        CHECK(cr.value("received", 0ull) == off + n);
        off += n;
    }
    const json f = WsCall(c, seq, json::object({
        { "verb",        "transfer_finish" },
        { "transfer_id", tid               },
    }));
    return f.value("staged", std::string());
}

static bool HasCaps(const json& welcome)
{
    const json caps = welcome.value("capabilities",
                                    nlohmann::json::array());
    for(const json& cap : caps)
    {
        if(cap.is_string() &&
           cap.get<std::string>() == "studio_workspace_v1")
        {
            return true;
        }
    }
    return false;
}

/*---------------------------------------------------------*\
||| main                                                     ||
\*---------------------------------------------------------*/
int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    g_tmp = fs::temp_directory_path() /
            ("o2-ipcws-" + std::to_string((uint64_t)
                 std::chrono::steady_clock::now()
                     .time_since_epoch().count()));
    fs::create_directories(g_tmp);
    WriteTestType(g_tmp / "types", 8);

    uint32_t seq = 0;

    /*---- capability negotiation ----------------------------*/
    {
        Rig rig(g_tmp / "cap");
        rig.Start();
        TestClient c;
        const json w = Welcome(c, *rig.server);
        CHECK(HasCaps(w));
        /* An unmanaged service still answers metadata. */
        const json g = WsCall(c, seq, json::object(
            {{ "verb", "get" }}));
        CHECK(g.value("managed", true) == false);
    }

    /*---- missing-capability fallback ------------------------*/
    {
        Rig rig(g_tmp / "nocap");
        rig.edeps.workspace = nullptr;    /* unwired: no surface */
        rig.Start();
        TestClient c;
        const json w = Welcome(c, *rig.server);
        CHECK(!HasCaps(w));
        WsCallErr(c, seq, json::object({{ "verb", "get" }}),
                  ErrCode::Unavailable);
        /* Old verbs keep working unchanged on the same server. */
        c.SendMsg(MsgType::Ping, ++seq, json::object());
        Message m;
        CHECK(RecvReply(c, seq, m));
        CHECK(m.type == MsgType::Pong);
    }

    /*---- subscribe "workspace" + initial snapshot -----------*/
    {
        Rig rig(g_tmp / "sub");
        rig.Start();
        TestClient c;
        Welcome(c, *rig.server);
        c.SendMsg(MsgType::Subscribe, ++seq,
                  json::object(
                      {{ "events", json::array({"workspace"}) }}));
        Message m;
        CHECK(RecvReply(c, seq, m));
        CHECK(m.type == MsgType::Ok);
        /* the initial snapshot EVENT for the kind arrives */
        bool got = false;
        for(int i = 0; i < 4 && !got; ++i)
        {
            Message ev;
            if(!c.RecvMsg(ev, 3000)) continue;
            if(ev.type == MsgType::Event &&
               ev.Body().value("kind", std::string()) == "workspace")
            {
                got = true;
                CHECK(ev.Body()["data"].value("managed", true)
                      == false);
            }
        }
        CHECK(got);
    }

    /*---- diagnostics channel stays read-only ----------------*/
    {
        Rig rig(g_tmp / "diag");
        rig.Start();
        TestClient c;
        Welcome(c, *rig.server, ChannelKind::Diagnostics);
        c.SendMsg(MsgType::WorkspaceCommand, ++seq,
                  json::object({{ "verb", "get" }}));
        Message m;
        CHECK(RecvReply(c, seq, m));
        CHECK(IsErr(m, ErrCode::ReadOnly));
    }

    /*---- 1 MiB envelope boundary ----------------------------*/
    {
        Rig rig(g_tmp / "envelope");
        rig.Start();
        TestClient c;
        Welcome(c, *rig.server);

        /* Exactly at the cap: an ignored "pad" field makes a
           valid request whose serialized body is 1 MiB. */
        json base = json::object(
            {{ "verb", "get" }, { "pad", "" }});
        const size_t over = base.dump().size();
        base["pad"] = std::string(kMaxBodyBytes - over, 'x');
        CHECK(base.dump().size() == kMaxBodyBytes);
        WsSend(c, seq, base);
        Message m;
        CHECK(RecvReply(c, seq, m));
        CHECK(m.type == MsgType::Ok);

        /* Past the cap: too_large, then the connection stays
           alive (PING still answers). */
        std::vector<uint8_t> big;
        auto put = [&](uint32_t v, int bytes)
        { for(int i = 0; i < bytes; ++i)
              big.push_back((uint8_t)(v >> (8 * i))); };
        put(kMagic, 4); put(kProtocolVersion, 2);
        put((uint16_t)MsgType::WorkspaceCommand, 2);
        put(++seq, 4); put(kMaxBodyBytes + 1, 4);
        c.SendBytes(big);
        c.SendRaw(std::string(128, '#'));
        c.SendMsg(MsgType::Ping, ++seq, json::object());
        CHECK(RecvReply(c, seq - 1, m));
        CHECK(IsErr(m, ErrCode::TooLarge));
        CHECK(RecvReply(c, seq, m));
        CHECK(m.type == MsgType::Pong);
    }

    /*---- commit a staged candidate -> managed + applied -----*/
    Rig* mainrig = nullptr;
    {
            Rig* rig = new Rig(g_tmp / "main");
        /* Many sequential test sessions share this server — a
           TestClient's session dies only when its reader sees
           the close, so the connection cap must outlast the
           whole suite, not just live clients. */
        rig->sopts.max_connections = 64;
        rig->Start();
        mainrig = rig;
        TestClient c;
        Welcome(c, *rig->server);
        std::vector<json> events;
        c.SendMsg(MsgType::Subscribe, ++seq,
                  json::object({{ "events", "all" }}));
        Message m;
        CHECK(RecvReply(c, seq, m, &events));

        const std::string bytes = WorkspaceDoc("managed-a").dump(2);
        const std::string tok =
            Upload(c, seq, "document", bytes);
        CHECK(!tok.empty());

        const json r = WsCall(c, seq, json::object({
            { "verb",              "commit" },
            { "id",                "w1"     },
            { "base_revision",     0        },
            { "resource_revision", 0        },
            { "staged",            tok      },
        }), &events);
        CHECK(r.value("accepted", false));
        const uint64_t job = r.value("job_id", 0ull);
        CHECK(job != 0);
        const json st = WaitJob(c, seq, job, 10000, &events);
        CHECK(st.value("state", "") == "succeeded");
        CHECK(st.value("committed", false));
        CHECK(st.value("applied", false) == true);
        CHECK(st.value("workspace_revision", 0ull) == 1);

        /* The completion EVENT carries job id + revision +
           success — the documented reconcile surface. It may
           have arrived while WaitJob was polling, so the
           collected events are scanned first. */
        auto ev_match = [&](const json& b)
        {
            return b.value("kind", std::string()) == "workspace" &&
                   b.contains("data") && b["data"].is_object() &&
                   b["data"].value("job_id", 0ull) == job;
        };
        bool ev_ok = false;
        for(const json& b : events)
        {
            if(ev_match(b))
            {
                ev_ok = true;
                CHECK(b["data"].value("state", "") == "succeeded");
                CHECK(b["data"].value("workspace_revision", 0ull)
                      == 1);
            }
        }
        for(int i = 0; i < 8 && !ev_ok; ++i)
        {
            Message ev;
            if(!c.RecvMsg(ev, 2000)) continue;
            const json b = ev.Body();
            if(ev.type == MsgType::Event && ev_match(b))
            {
                ev_ok = true;
                CHECK(b["data"].value("state", "") == "succeeded");
                CHECK(b["data"].value("workspace_revision", 0ull)
                      == 1);
            }
        }
        CHECK(ev_ok);

        const json g = WsCall(c, seq, json::object(
            {{ "verb", "get" }}));
        CHECK(g.value("managed", false));
        CHECK(g.value("id", "") == "w1");
        CHECK(g.value("workspace_revision", 0ull) == 1);
        CHECK(g.value("content_sha256", "").size() == 64);
        /* applied through the shared scene-apply path */
        CHECK(rig->svc->SceneRevision() == 1);
        CHECK(rig->svc->Scene()->name == "managed-a");
        }

    /*---- managed workspace gates lossy legacy mutations ----*/
    {
        Rig& rig = *mainrig;
        TestClient c;
        Welcome(c, *rig.server);
        const json doc = json::object({
            { "version", 2 },
            { "name",    "legacy" },
            { "objects", json::array() },
        });
        c.SendMsg(MsgType::SetScene, ++seq,
                  json::object({{ "document", doc }}));
        Message m;
        CHECK(RecvReply(c, seq, m));
        CHECK(IsErr(m, ErrCode::BadRequest, "workspace_required"));

        c.SendMsg(MsgType::LayoutCommand, ++seq, json::object({
            { "verb",      "set_position"              },
            { "object_id", "o1"                        },
            { "position", json::object(
                {{ "x", 1 }, { "y", 2 }, { "z", 3 }}) },
        }));
        CHECK(RecvReply(c, seq, m));
        CHECK(IsErr(m, ErrCode::BadRequest, "workspace_required"));

        /* identify is NOT a document mutation — it must not be
           workspace-gated (no mapper wired -> honest
           unavailable, not workspace_required). */
        c.SendMsg(MsgType::LayoutCommand, ++seq, json::object({
            { "verb",       "identify" },
            { "controller", json::object(
                {{ "device", "x" }, { "serial", "y" },
                 { "location", "z" }, { "zone", "" }}) },
        }));
        CHECK(RecvReply(c, seq, m));
        CHECK(IsErr(m, ErrCode::Unavailable));

        /* GET_SCENE and telemetry stay available. */
        c.SendMsg(MsgType::GetScene, ++seq, json::object());
        CHECK(RecvReply(c, seq, m));
        CHECK(m.type == MsgType::Scene);
        CHECK(m.Body().value("workspace", json::object())
                  .value("managed", false));
        c.SendMsg(MsgType::GetState, ++seq, json::object());
        CHECK(RecvReply(c, seq, m));
        CHECK(m.type == MsgType::State);
    }

    /*---- two-client conflict: stale writer never wins -------*/
    {
        Rig& rig = *mainrig;
        TestClient a, b;
        Welcome(a, *rig.server);
        Welcome(b, *rig.server);

        const json ga = WsCall(a, seq, json::object(
            {{ "verb", "get" }}));
        const uint64_t rev_a = ga["workspace_revision"].get<uint64_t>();
        const uint64_t res_a = ga["resource_revision"].get<uint64_t>();
        CHECK(rev_a == 1);

        /* Client B commits first (revision 2). */
        const json rb = WsCall(b, seq, json::object({
            { "verb",              "commit"                    },
            { "id",                "w1"                        },
            { "base_revision",     rev_a                       },
            { "resource_revision", res_a                       },
            { "document",          WorkspaceDoc("managed-b")   },
        }));
        const json stb = WaitJob(b, seq, rb["job_id"].get<uint64_t>());
        CHECK(stb.value("state", "") == "succeeded");
        CHECK(stb.value("workspace_revision", 0ull) == 2);

        /* Client A replays a stale base — out-of-order reply
           must NOT overwrite B's newer work. */
        const json ra = WsCall(a, seq, json::object({
            { "verb",              "commit"                    },
            { "id",                "w1"                        },
            { "base_revision",     rev_a                       },
            { "resource_revision", res_a                       },
            { "document",          WorkspaceDoc("stale-a")     },
        }));
        CHECK(ra.value("accepted", false));
        const json sta = WaitJob(a, seq, ra["job_id"].get<uint64_t>());
        CHECK(sta.value("state", "") == "failed");
        CHECK(sta.value("detail", "") == "revision_conflict");

        const json g = WsCall(a, seq, json::object(
            {{ "verb", "get" }}));
        CHECK(g.value("workspace_revision", 0ull) == 2);
        CHECK(g.value("name", "") == "managed-b");
    }

    /*---- stale resource revision ----------------------------*/
    {
        Rig& rig = *mainrig;
        TestClient c;
        Welcome(c, *rig.server);
        const json g = WsCall(c, seq, json::object(
            {{ "verb", "get" }}));
        const json r = WsCall(c, seq, json::object({
            { "verb",              "commit"                          },
            { "id",                "w1"                              },
            { "base_revision",     g["workspace_revision"]           },
            { "resource_revision",
              g["resource_revision"].get<uint64_t>() + 7             },
            { "document",          WorkspaceDoc("res-stale")         },
        }));
        const json st = WaitJob(c, seq, r["job_id"].get<uint64_t>());
        CHECK(st.value("state", "") == "failed");
        CHECK(st.value("detail", "") == "revision_conflict");
    }

    /*---- path traversal / absolute ids ----------------------*/
    {
        Rig rig(g_tmp / "paths");
        rig.Start();
        TestClient c;
        Welcome(c, *rig.server);
        WsCallErr(c, seq, json::object({
            { "verb", "commit" }, { "id", "../escape" },
            { "base_revision", 0 }, { "resource_revision", 0 },
            { "document", WorkspaceDoc("x") },
        }), ErrCode::BadRequest);
        WsCallErr(c, seq, json::object({
            { "verb", "commit" }, { "id", "C:/abs" },
            { "base_revision", 0 }, { "resource_revision", 0 },
            { "document", WorkspaceDoc("x") },
        }), ErrCode::BadRequest);
        /* a staged token is an opaque id — traversal never
           reaches the filesystem */
        WsCallErr(c, seq, json::object({
            { "verb", "commit" }, { "id", "w1" },
            { "base_revision", 0 }, { "resource_revision", 0 },
            { "staged", "../../secret" },
        }), ErrCode::BadRequest);
        /* client-chosen paths are never accepted */
        WsCallErr(c, seq, json::object({
            { "verb", "transfer_begin" }, { "direction", "upload" },
            { "kind", "document" }, { "length", 10 },
            { "sha256", std::string(64, '0') },
            { "path", "C:/evil.json" },
        }), ErrCode::BadRequest);
    }

    /*---- transfer bounds: order/hash/length/chunk -----------*/
    {
        Rig rig(g_tmp / "tbounds");
        rig.Start();
        TestClient c;
        Welcome(c, *rig.server);

        const std::string bytes = WorkspaceDoc("x").dump(2);
        const json r = WsCall(c, seq, json::object({
            { "verb", "transfer_begin" }, { "direction", "upload" },
            { "kind", "document" },
            { "length", (uint64_t)bytes.size() },
            { "sha256", Sha256Hex(bytes) },
        }));
        const std::string tid = r["transfer_id"].get<std::string>();
        CHECK(!tid.empty());

        /* out-of-order offset -> chunk_order */
        WsCallErr(c, seq, json::object({
            { "verb", "transfer_chunk" }, { "transfer_id", tid },
            { "offset", 5 },
            { "data", Base64Encode(bytes.data(), 16) },
        }), ErrCode::BadRequest, "chunk_order");

        /* chunk past the decoded bound -> too_large */
        WsCallErr(c, seq, json::object({
            { "verb", "transfer_chunk" }, { "transfer_id", tid },
            { "offset", 0 },
            { "data", Base64Encode(
                          std::string(200 * 1024, 'z').data(),
                          200 * 1024) },
        }), ErrCode::BadRequest, "too_large");

        /* finish before the declared length -> length_mismatch */
        WsCallErr(c, seq, json::object({
            { "verb", "transfer_finish" }, { "transfer_id", tid },
        }), ErrCode::BadRequest, "length_mismatch");

        /* correct bytes but wrong declared hash -> hash_mismatch */
        const json r2 = WsCall(c, seq, json::object({
            { "verb", "transfer_begin" }, { "direction", "upload" },
            { "kind", "document" },
            { "length", (uint64_t)bytes.size() },
            { "sha256", Sha256Hex("different content") },
        }));
        const std::string tid2 = r2["transfer_id"].get<std::string>();
        WsCall(c, seq, json::object({
            { "verb", "transfer_chunk" }, { "transfer_id", tid2 },
            { "offset", 0 }, { "data", Base64Encode(bytes.data(),
                                                    bytes.size()) },
        }));
        WsCallErr(c, seq, json::object({
            { "verb", "transfer_finish" }, { "transfer_id", tid2 },
        }), ErrCode::BadRequest, "hash_mismatch");

        /* third open transfer on the session -> transfer_limit */
        WsCallErr(c, seq, json::object({
            { "verb", "transfer_begin" }, { "direction", "upload" },
            { "kind", "document" }, { "length", 4 },
            { "sha256", Sha256Hex("1234") },
        }), ErrCode::BadRequest, "transfer_limit");

        /* a second session is independent */
        TestClient c2;
        Welcome(c2, *rig.server);
        const json r3 = WsCall(c2, seq, json::object({
            { "verb", "transfer_begin" }, { "direction", "upload" },
            { "kind", "document" }, { "length", 4 },
            { "sha256", Sha256Hex("1234") },
        }));
        CHECK(!r3.value("transfer_id", "").empty());
        /* ... but foreign-session tokens don't cross */
        WsCallErr(c, seq, json::object({
            { "verb", "transfer_finish" },
            { "transfer_id", r3["transfer_id"] },
        }), ErrCode::BadRequest, "unknown_transfer");
    }

    /*---- aggregate staging cap -------------------------------*/
    {
        Rig rig(g_tmp / "aggcap");
        rig.eopts.transfers.max_staged_total_bytes = 300 * 1024;
        rig.Start();
        TestClient c;
        Welcome(c, *rig.server);
        const json r1 = WsCall(c, seq, json::object({
            { "verb", "transfer_begin" }, { "direction", "upload" },
            { "kind", "bundle" }, { "length", 200 * 1024 },
            { "sha256", std::string(64, 'a') },
        }));
        CHECK(!r1.value("transfer_id", "").empty());
        WsCallErr(c, seq, json::object({
            { "verb", "transfer_begin" }, { "direction", "upload" },
            { "kind", "bundle" }, { "length", 200 * 1024 },
            { "sha256", std::string(64, 'b') },
        }), ErrCode::BadRequest, "aggregate_cap");
    }

    /*---- expired transfer ------------------------------------*/
    {
        Rig rig(g_tmp / "expiry");
        rig.eopts.transfers.idle_expiry_ms = 80;
        rig.Start();
        TestClient c;
        Welcome(c, *rig.server);
        const json r = WsCall(c, seq, json::object({
            { "verb", "transfer_begin" }, { "direction", "upload" },
            { "kind", "document" }, { "length", 4 },
            { "sha256", Sha256Hex("1234") },
        }));
        const std::string tid = r["transfer_id"].get<std::string>();
        CHECK(!tid.empty());
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        /* any command lazily sweeps; the dead transfer is gone */
        WsCall(c, seq, json::object({{ "verb", "list" }}));
        WsCallErr(c, seq, json::object({
            { "verb", "transfer_chunk" }, { "transfer_id", tid },
            { "offset", 0 }, { "data", Base64Encode("1234", 4) },
        }), ErrCode::BadRequest, "unknown_transfer");
    }

    /*---- >1 MiB document through chunked transfer -----------*/
    {
        Rig rig(g_tmp / "bigdoc");
        rig.Start();
        TestClient c;
        Welcome(c, *rig.server);
        const std::string bytes =
            WorkspaceDoc("big", /*pad*/ (size_t)(1.4 * 1024 * 1024))
                .dump(2);
        CHECK(bytes.size() > kMaxBodyBytes);
        CHECK(bytes.size() < 4ull * 1024 * 1024);
        const std::string tok = Upload(c, seq, "document", bytes);
        CHECK(!tok.empty());
        const json r = WsCall(c, seq, json::object({
            { "verb", "commit" }, { "id", "big1" },
            { "base_revision", 0 }, { "resource_revision", 0 },
            { "staged", tok },
        }));
        const json st = WaitJob(c, seq, r["job_id"].get<uint64_t>());
        CHECK(st.value("state", "") == "succeeded");

        /* a declared upload past the document cap refuses up
           front — the 1 MiB envelope is never stretched */
        WsCallErr(c, seq, json::object({
            { "verb", "transfer_begin" }, { "direction", "upload" },
            { "kind", "document" }, { "length", 5ull * 1024 * 1024 },
            { "sha256", std::string(64, 'c') },
        }), ErrCode::BadRequest, "too_large");
    }

    /*---- bounded document download ---------------------------*/
    {
        Rig& rig = *mainrig;
        TestClient c;
        Welcome(c, *rig.server);
        const json r = WsCall(c, seq, json::object({
            { "verb", "transfer_begin" }, { "direction", "download" },
            { "kind", "document" }, { "id", "w1" },
        }));
        const std::string tid = r["transfer_id"].get<std::string>();
        CHECK(!tid.empty());
        CHECK(r.value("length", 0ull) > 0);
        CHECK(r.value("sha256", "").size() == 64);
        std::string got;
        uint64_t off = 0;
        bool eof = false;
        for(int i = 0; i < 64 && !eof; ++i)
        {
            const json cr = WsCall(c, seq, json::object({
                { "verb", "transfer_chunk" }, { "transfer_id", tid },
                { "offset", off },
            }));
            std::vector<uint8_t> dec;
            CHECK(Base64Decode(cr.value("data", ""), dec,
                               200 * 1024));
            got.append((const char*)dec.data(), dec.size());
            CHECK(cr.value("offset", 0ull) == off);
            off += dec.size();
            eof = cr.value("eof", false);
        }
        CHECK(eof);
        CHECK(Sha256Hex(got) == r["sha256"].get<std::string>());
        const json doc = json::parse(got, nullptr, false);
        CHECK(doc.is_object());
        CHECK(doc.value("name", "") == "managed-b");
        WsCall(c, seq, json::object({
            { "verb", "transfer_finish" }, { "transfer_id", tid },
        }));
    }

    /*---- lost completion event: reconcile by job_status -----*/
    {
        Rig& rig = *mainrig;
        TestClient c;   /* never subscribes — event is "lost" */
        Welcome(c, *rig.server);
        const json g = WsCall(c, seq, json::object(
            {{ "verb", "get" }}));
        const json r = WsCall(c, seq, json::object({
            { "verb", "commit" }, { "id", "w1" },
            { "base_revision",     g["workspace_revision"] },
            { "resource_revision", g["resource_revision"]  },
            { "document", WorkspaceDoc("managed-c") },
        }));
        const json st = WaitJob(c, seq, r["job_id"].get<uint64_t>());
        CHECK(st.value("state", "") == "succeeded");
        CHECK(st.value("workspace_revision", 0ull) == 3);
    }

    /*---- disconnect during commit + reconnect reconcile -----*/
    {
        Rig rig(g_tmp / "disco");
        rig.Start();
        std::atomic<bool> release{ false };
        std::atomic<int>  hook_hits{ 0 };
        rig.ws->SetPreJobHookForTesting(
            [&](const std::string&) {
                ++hook_hits;
                while(!release.load())
                {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(1));
                }
            });
        TestClient c1;
        Welcome(c1, *rig.server);
        const json r1 = WsCall(c1, seq, json::object({
            { "verb", "commit" }, { "id", "w1" },
            { "base_revision", 0 }, { "resource_revision", 0 },
            { "document", WorkspaceDoc("disco-a") },
        }));
        const uint64_t job1 = r1["job_id"].get<uint64_t>();
        CHECK(WaitFor([&] { return hook_hits.load() >= 1; }));
        /* queue a second job on the same session */
        const json r2 = WsCall(c1, seq, json::object({
            { "verb", "commit" }, { "id", "w1" },
            { "base_revision", 1 }, { "resource_revision", 0 },
            { "document", WorkspaceDoc("disco-b") },
        }));
        CHECK(r2.value("accepted", false));

        /* client vanishes mid-commit; the running job's commit
           may still land durably — never rolled back — while
           the QUEUED job is cancelled with the session. */
        c1.CloseInbound();
        CHECK(WaitFor([&] {
            return rig.server->Stats().sessions_active == 0;
        }));
        release.store(true);

        TestClient c2;
        Welcome(c2, *rig.server);
        /* reconcile by revision: poll `get` until the accepted
           commit is visible — the dead session's job id is
           meaningless on the new session. */
        json g;
        CHECK(WaitFor([&] {
            g = WsCall(c2, seq, json::object({{ "verb", "get" }}));
            return g.value("managed", false) &&
                   g.value("name", "") == "disco-a";
        }, 8000));
        CHECK(g.value("workspace_revision", 0ull) == 1);
        /* the queued second commit never ran — the revision
           never advanced past the accepted one */
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        g = WsCall(c2, seq, json::object({{ "verb", "get" }}));
        CHECK(g.value("workspace_revision", 0ull) == 1);
        /* the dead session's job ids are not visible to the
           reconnecting session — reconcile is by revision */
        const json st2 = WsCall(c2, seq, json::object({
            { "verb", "job_status" }, { "job_id", job1 },
        }));
        CHECK(st2.value("found", true) == false);
    }

    /*---- PING stays responsive beside a slow workspace job --*/
    {
        Rig rig(g_tmp / "slowjob");
        rig.Start();
        std::atomic<bool> release{ false };
        rig.ws->SetPreJobHookForTesting(
            [&](const std::string&) {
                while(!release.load())
                {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(1));
                }
            });
        TestClient c1, c2;
        Welcome(c1, *rig.server);
        Welcome(c2, *rig.server);
        const json r = WsCall(c1, seq, json::object({
            { "verb", "commit" }, { "id", "w1" },
            { "base_revision", 0 }, { "resource_revision", 0 },
            { "document", WorkspaceDoc("slow") },
        }));
        CHECK(r.value("accepted", false));
        /* same session + foreign session both answer PING while
           the worker sits in the job */
        Message m;
        c1.SendMsg(MsgType::Ping, ++seq, json::object());
        CHECK(RecvReply(c1, seq, m, nullptr, 3000));
        CHECK(m.type == MsgType::Pong);
        c2.SendMsg(MsgType::Ping, ++seq, json::object());
        CHECK(RecvReply(c2, seq, m, nullptr, 3000));
        CHECK(m.type == MsgType::Pong);
        release.store(true);
        WaitJob(c1, seq, r["job_id"].get<uint64_t>());
    }

    /*---- pending-job bound (8) + job_cancel ------------------*/
    {
        Rig rig(g_tmp / "jobcap");
        rig.Start();
        std::atomic<bool> release{ false };
        std::atomic<int>  hits{ 0 };
        rig.ws->SetPreJobHookForTesting(
            [&](const std::string&) {
                ++hits;
                while(!release.load())
                {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(1));
                }
            });
        TestClient c;
        Welcome(c, *rig.server);
        auto commit = [&](uint64_t base)
        {
            return WsCall(c, seq, json::object({
                { "verb", "commit" }, { "id", "w1" },
                { "base_revision", base },
                { "resource_revision", 0 },
                { "document", WorkspaceDoc("q") },
            }));
        };
        json first = commit(0);
        CHECK(first.value("accepted", false));
        CHECK(WaitFor([&] { return hits.load() >= 1; }));
        /* 1 running + 8 pending is the bound — the 10th job is
           refused, never queued unboundedly */
        std::vector<uint64_t> queued;
        for(int i = 0; i < 8; ++i)
        {
            const json r = commit(0);
            CHECK(r.value("accepted", false));
            queued.push_back(r["job_id"].get<uint64_t>());
        }
        const json over = commit(0);
        CHECK(!over.value("accepted", true));
        CHECK(over.value("detail", "") == "busy");

        /* cancel a queued job; the running one cannot be
           cancelled */
        const json cancel = WsCall(c, seq, json::object({
            { "verb", "job_cancel" }, { "job_id", queued.back() },
        }));
        CHECK(cancel.value("cancelled", false) == true);
        const json busy_cancel = WsCallErr(c, seq, json::object({
            { "verb", "job_cancel" },
            { "job_id", first["job_id"] },
        }), ErrCode::BadRequest);
        (void)busy_cancel;
        WsCallErr(c, seq, json::object({
            { "verb", "job_cancel" }, { "job_id", 9999 },
        }), ErrCode::BadRequest);
        release.store(true);
        WaitJob(c, seq, first["job_id"].get<uint64_t>());
        /* the cancelled job reports cancelled when polled */
        const json cs = WsCall(c, seq, json::object({
            { "verb", "job_status" },
            { "job_id", queued.back() },
        }));
        CHECK(cs.value("state", "") == "cancelled");
    }

    /*---- preview verbs: honest errors before any session ---*/
    {
        /* Task 7 replaced the Unavailable stubs with the real
           bounded draft preview (preview_session_test covers
           the full surface). Here we only pin the honest
           pre-session failures: begin needs a document or a
           staged token, update needs an existing session, end
           is idempotent {active:false}. */
        Rig rig(g_tmp / "preview");
        rig.Start();
        TestClient c;
        Welcome(c, *rig.server);
        WsCallErr(c, seq, json::object({{ "verb",
            "preview_begin" }}), ErrCode::BadRequest);
        WsCallErr(c, seq, json::object({{ "verb",
            "preview_update" }}), ErrCode::BadRequest,
            "no_preview");
        const json end = WsCall(c, seq, json::object({
            { "verb", "preview_end" } }));
        CHECK(end.value("active", true) == false);
    }

    /*---- validate verb ----------------------------------------*/
    {
        Rig& rig = *mainrig;
        TestClient c;
        Welcome(c, *rig.server);
        json g = WsCall(c, seq, json::object({{ "verb", "get" }}));
        const json r = WsCall(c, seq, json::object({
            { "verb", "validate" }, { "id", "w1" },
            { "base_revision",     g["workspace_revision"] },
            { "resource_revision", g["resource_revision"]  },
            { "document", WorkspaceDoc("validated") },
        }));
        const json st = WaitJob(c, seq, r["job_id"].get<uint64_t>());
        CHECK(st.value("state", "") == "succeeded");
        CHECK(st["result"].value("valid", false) == true);
        /* validate never commits */
        g = WsCall(c, seq, json::object({{ "verb", "get" }}));
        CHECK(g.value("name", "") == "managed-c");
        CHECK(g.value("workspace_revision", 0ull) == 3);
    }

    /*---- save_copy commits another workspace w/o adopting ----*/
    {
        Rig& rig = *mainrig;
        TestClient c;
        Welcome(c, *rig.server);
        const json r = WsCall(c, seq, json::object({
            { "verb", "save_copy" }, { "id", "w2" },
            { "base_revision", 0 }, { "resource_revision", 0 },
            { "document", WorkspaceDoc("copy") },
        }));
        const json st = WaitJob(c, seq, r["job_id"].get<uint64_t>());
        CHECK(st.value("state", "") == "succeeded");
        CHECK(st.value("committed", false));
        const json g = WsCall(c, seq, json::object(
            {{ "verb", "get" }}));
        /* current workspace unchanged */
        CHECK(g.value("id", "") == "w1");
        CHECK(g.value("name", "") == "managed-c");
        const json l = WsCall(c, seq, json::object(
            {{ "verb", "list" }}));
        bool seen_w2 = false;
        for(const json& e : l["workspaces"])
        {
            seen_w2 |= e.get<std::string>() == "w2";
        }
        CHECK(seen_w2);
    }

    /*---- catalog + bounded page ------------------------------*/
    {
        Rig& rig = *mainrig;
        TestClient c;
        Welcome(c, *rig.server);
        const json cat = WsCall(c, seq, json::object({
            { "verb", "catalog" }, { "kind", "devices" },
            { "offset", 0 }, { "limit", 8 },
        }));
        CHECK(cat.value("kind", "") == "devices");
        CHECK(cat["entries"].is_array());
        bool found_fan = false;
        for(const json& e : cat["entries"])
        {
            found_fan |= e.value("id", "") == "test-fan";
        }
        CHECK(found_fan);
        WsCallErr(c, seq, json::object({
            { "verb", "catalog" }, { "kind", "bogus" },
        }), ErrCode::BadRequest);
    }

    /*---- effects catalog: byte-budgeted pages ---------------*/
    {
        /* A look library whose inlined document_texts would
           overflow the 1 MiB body cap must page instead of
           tearing the session down (fix round 2 — the round-1
           document inline made the reply byte-unbounded). */
        Rig& rig = *mainrig;
        const fs::path elib = rig.ws->Store().LibraryEffectsDir();
        fs::create_directories(elib);
        auto write_look = [&elib](const std::string& id,
                                  size_t desc_bytes)
        {
            nlohmann::ordered_json d;
            d["schema_version"] = studio::EFFECT_SCHEMA_VERSION;
            d["id"]   = id;
            d["name"] = id;
            d["description"] = std::string(desc_bytes, 'x');
            d["needs"] = "";
            d["layers"] = nlohmann::ordered_json::array({
                nlohmann::ordered_json::object(
                    {{ "primitive", "static" }}),
            });
            std::ofstream(elib / (id + ".effect.json"))
                << d.dump();
        };
        /* ~100 KiB descriptions ride the metadata AND the
           document_text (~200 KiB per entry) → ~3 per 768 KiB
           page → pages. */
        for(int i = 0; i < 6; ++i)
        {
            write_look("cat_fat_" + std::to_string(i),
                       100 * 1024);
        }
        /* One look whose doc AND whose metadata both exceed the
           budget — it degrades twice (document flag, then a
           bounded description) but must still emit a row: an
           empty page with next_offset == offset would stall
           the walk forever. */
        write_look("cat_huge", 900 * 1024);

        TestClient c;
        Welcome(c, *rig.server);
        std::set<std::string> seen;
        bool omitted = false;
        uint64_t off = 0, last = 0;
        for(int page = 0; page < 16; ++page)
        {
            const json r = WsCall(c, seq, json::object({
                { "verb", "catalog" }, { "kind", "effects" },
                { "offset", off },
            }));
            /* Every page fits the wire envelope — the reply
               itself is the bound that was missing. */
            CHECK(r.dump().size() < kMaxBodyBytes);
            CHECK(r.value("total", 0ull) == 7);
            /* No empty progress-free page: a truncated page
               must advance the offset or the walk stalls. */
            if(r.value("more", false))
            {
                CHECK(!r["entries"].empty());
                CHECK(r.value("next_offset", 0ull) > off);
            }
            for(const json& e : r["entries"])
            {
                seen.insert(e.value("id", ""));
                if(e.value("document_omitted", false))
                {
                    omitted = true;
                    CHECK(e.value("id", "") == "cat_huge");
                    CHECK(!e.contains("document_text"));
                }
            }
            if(!r.value("more", false))
            {
                break;
            }
            last = off;
            off = r.value("next_offset", off);
            CHECK(off > last);
        }
        /* Every look is reachable across the page walk. */
        for(int i = 0; i < 6; ++i)
        {
            CHECK(seen.count("cat_fat_" + std::to_string(i)) == 1);
        }
        CHECK(seen.count("cat_huge") == 1);
        CHECK(omitted);
        /* Session is alive — the catalog loop never tore it
           down (WsCall itself fails if no reply arrives). */
        WsCallErr(c, seq, json::object({
            { "verb", "catalog" }, { "kind", "bogus" },
        }), ErrCode::BadRequest);
    }

    /*---- export -> staged download; import inspect/commit ----*/
    {
        Rig& rig = *mainrig;
        TestClient c;
        Welcome(c, *rig.server);
        const json r = WsCall(c, seq, json::object({
            { "verb", "export" },
        }));
        const json st = WaitJob(c, seq, r["job_id"].get<uint64_t>());
        CHECK(st.value("state", "") == "succeeded");
        const std::string tid =
            st["result"].value("transfer_id", "");
        CHECK(!tid.empty());
        CHECK(st["result"].value("bytes", 0ull) > 0);
        CHECK(st["result"].value("sha256", "").size() == 64);

        std::string bundle;
        uint64_t off = 0; bool eof = false;
        for(int i = 0; i < 64 && !eof; ++i)
        {
            const json cr = WsCall(c, seq, json::object({
                { "verb", "transfer_chunk" },
                { "transfer_id", tid }, { "offset", off },
            }));
            std::vector<uint8_t> dec;
            CHECK(Base64Decode(cr.value("data", ""), dec,
                               200 * 1024));
            bundle.append((const char*)dec.data(), dec.size());
            off += dec.size();
            eof = cr.value("eof", false);
        }
        CHECK(eof);
        const json bj = json::parse(bundle, nullptr, false);
        CHECK(bj.is_object());
        CHECK(bj.value("format", "") ==
              "openrgb2-workspace-bundle");
        /* export hygiene: the portable form strips verified
           ownership (SanitizeForExport) — the key is absent or
           false, never true */
        CHECK(bj["workspace"]["device_settings"]["fan"]["zones"]
              ["ring"].value("verified", false) == false);

        /* inspect the exported bundle via a fresh upload */
        Rig rig2(g_tmp / "imported");
        rig2.Start();
        TestClient c2;
        Welcome(c2, *rig2.server);
        const std::string tok =
            Upload(c2, seq, "bundle", bundle);
        CHECK(!tok.empty());
        const json ri = WsCall(c2, seq, json::object({
            { "verb", "import_inspect" }, { "staged", tok },
        }));
        const json sti = WaitJob(c2, seq, ri["job_id"].get<uint64_t>());
        CHECK(sti.value("state", "") == "succeeded");
        CHECK(sti["result"].value("valid", false) == true);
        CHECK(sti["result"].value("conflicts", json()).is_array());

        const json rc = WsCall(c2, seq, json::object({
            { "verb", "import_commit" }, { "id", "imp1" },
            { "staged", tok },
        }));
        const json stc = WaitJob(c2, seq, rc["job_id"].get<uint64_t>());
        CHECK(stc.value("state", "") == "succeeded");
        CHECK(stc.value("committed", false));
        /* imports never auto-apply */
        CHECK(stc.value("applied", true) == false);
        const json g2 = WsCall(c2, seq, json::object(
            {{ "verb", "get" }}));
        CHECK(g2.value("id", "") == "imp1");

        /* reload re-reads the durable state — revision survives */
        const json rr = WsCall(c2, seq, json::object({
            { "verb", "reload" }, { "id", "imp1" },
        }));
        const json str_ = WaitJob(c2, seq, rr["job_id"].get<uint64_t>());
        CHECK(str_.value("state", "") == "succeeded");
    }

    std::printf("\n%d checks, %d failures\n", checks, failures);
    std::error_code ec;
    fs::remove_all(g_tmp, ec);
    delete mainrig;
    return failures == 0 ? 0 : 1;
}
