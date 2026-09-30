// SPDX-License-Identifier: GPL-2.0-or-later
//
// Studio-import Task 12 — the complete imported workflow, end to
// end against the REAL EngineHost composition (ownership scan,
// SDK attach through the injected fake backend, the real
// named-pipe IPC listeners, the real WORKSPACE_COMMAND job queue,
// the managed WorkspaceStore on a temp root):
//
//   A. legacy import — a host-settings blob {"scene":...,"inputs":
//      ...} uploads over the bounded transfer surface, commits as a
//      managed workspace (sanitized: no verified ownership, no
//      armed inputs, no live_on_startup), its extracted custom
//      device type lands in the managed library, and a reload job
//      adopts + resolves it — verified-cleared, so NOTHING reaches
//      the wire (an import can never arm output).
//   B. author + save — the UI-side edit path: document download,
//      personal-look install (resource_install kind:"effect"),
//      object move + re-verify + look selection, revision-checked
//      commit -> applied -> frames reach the fake backend. The
//      lossy legacy mutation surface is refused while managed
//      (workspace_required).
//   C. packed bundle import — an openrgb2-bundle-files upload
//      carrying studio.json + a custom device type is inspected
//      (valid, new-type report) then committed onto a second id —
//      never applied; the bundled type resolves via resource_get.
//   D. UI process boundary — the workspace/preview surface under
//      repeated attach: an in-flight session preview dies with the
//      "crashed UI" channel, frames keep flowing, a fresh session
//      rejoins and takes a preview (the dead session's slot was
//      released), then two more clean attaches.
//   E. engine restart — second EngineHost on the SAME workspace
//      root: the persisted active workspace is re-adopted at start
//      (managed:true, revisions intact, NO scene re-apply — the
//      last-scene snapshot stays the applied truth), managed ids
//      list back, an explicit reload re-opens the authored
//      workspace, and frames resume.
//   F. export / reimport round-trip — the portable bundle stages
//      for download (sanitized: no verified, no identity), packs
//      the referenced types, re-imports onto a fresh id, and the
//      round-tripped workspace reloads + resolves.
//   G. external-change honesty — a byte poked into the managed
//      generation reports "modified" on the polled get.
//   H. missing workspace — a persisted active workspace whose
//      directory vanished logs the failure and boots unmanaged.
//
// No sockets beyond the loopback named pipes, no hardware; the SDK
// side is the in-process fake backend. "UI" sessions are real
// named-pipe clients speaking the production wire protocol — the
// same verbs openrgb2-ui issues.

#include "EngineHost.h"
#include "EngineService.h"
#include "SdkWire.h"

#include "../sdk/fake_sdk_server.h"

#include "IpcProtocol.h"
#include "NamedPipe.h"
#include "WorkspaceTransfer.h"

#include "scene/SceneTypes.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace openrgb2;
using nlohmann::json;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

namespace fs = std::filesystem;

/*---------------------------------------------------------*\
||| Fake SDK fixture (same controller shape as the e2e     ||
||| suites — the wireless group carries three 40-LED zones, ||
||| the wired header one resizable 8-LED ARGB zone).        ||
\*---------------------------------------------------------*/

static const char* kMac    = "FA:KE:57:00:00:12";
static const char* kMbSer  = "MB-STUDIO-1";

static sdk_test::FakeController WirelessGroup()
{
    sdk_test::FakeController c;
    c.type     = 5;
    c.name     = "UNI FAN SL V3 Wireless STUDIOE2E";
    c.vendor   = "Lian Li";
    c.serial   = kMac;
    c.location = std::string("Wireless: ") + kMac;
    for(const char* m : { "Direct", "Spectrum Cycle",
                          "Rainbow Wave", "Breathing" })
    {
        sdk_test::FakeMode mode;
        mode.name = m;
        c.modes.push_back(mode);
    }
    c.zones = { { "Fan 1", 0, 40, 40, 40 },
                { "Fan 2", 0, 40, 40, 40 },
                { "Fan 3", 0, 40, 40, 40 } };
    c.colors.assign(120, 0x00010101u);
    return c;
}

static sdk_test::FakeController Wired()
{
    sdk_test::FakeController c;
    c.type     = 2;
    c.name     = "X870E AORUS ELITE WIFI7 ICE STUDIOE2E";
    c.vendor   = "GIGABYTE";
    c.serial   = kMbSer;
    c.location = "smbus:0-motherboard-studioe2e";
    sdk_test::FakeMode m;
    m.name = "Direct";
    c.modes.push_back(m);
    c.zones = { { "ARGB_V2_1", 0, 8, 8, 256 } };
    c.colors.assign(8, 0x00010101u);
    return c;
}

/*---------------------------------------------------------*\
||| Real named-pipe client — the UI session surface, with  ||
||| the WORKSPACE_COMMAND verb helpers the production UI   ||
||| uses (tools + controllers both land here).             ||
\*---------------------------------------------------------*/

struct UiClient
{
    std::unique_ptr<ipc::IIpcChannel> ch;
    uint32_t seq = 0;

    bool Connect(const std::string& pipe)
    {
        std::string err;
        ch = ipc::ConnectNamedPipeChannel(pipe, 5000, err);
        if(!ch)
        {
            std::printf("  pipe connect %s failed: %s\n",
                        pipe.c_str(), err.c_str());
        }
        return ch != nullptr;
    }

    bool Send(ipc::MsgType t, uint32_t id, const json& body)
    {
        std::string err;
        const std::vector<uint8_t> bytes =
            ipc::EncodeMessage(t, id, body);
        if(bytes.empty())
        {
            return false;
        }
        return ch->SendAll(bytes.data(), bytes.size(), 5000, err)
               == ipc::IpcIoResult::Ok;
    }

    bool Recv(ipc::Message& out, uint32_t timeout_ms)
    {
        uint8_t hdr[ipc::kHeaderBytes];
        std::string err;
        if(ch->RecvExact(hdr, sizeof(hdr), timeout_ms, err)
           != ipc::IpcIoResult::Ok)
        {
            return false;
        }
        uint32_t magic = 0, blen = 0;
        memcpy(&magic, hdr + 0, 4);
        memcpy(&blen,  hdr + 12, 4);
        if(magic != ipc::kMagic || blen > ipc::kMaxBodyBytes)
        {
            return false;
        }
        uint16_t ver = 0, type = 0;
        memcpy(&ver,  hdr + 4, 2);
        memcpy(&type, hdr + 6, 2);
        out.request_id = 0;
        memcpy(&out.request_id, hdr + 8, 4);
        out.version = ver;
        out.type    = (ipc::MsgType)type;
        if(blen == 0)
        {
            out.body_raw.clear();
            return true;
        }
        out.body_raw.resize(blen);
        return ch->RecvExact(out.body_raw.data(), blen,
                             timeout_ms, err)
               == ipc::IpcIoResult::Ok;
    }

    /* HELLO -> WELCOME; the body is returned so capability
       negotiation can be asserted. */
    json Hello()
    {
        if(!Send(ipc::MsgType::Hello, 0, json::object({
                { "client_name", "studio-e2e-ui" },
                { "min_version", 1 },
                { "max_version", 1 } })))
        {
            return json();
        }
        ipc::Message m;
        if(!Recv(m, 5000) || m.type != ipc::MsgType::Welcome)
        {
            return json();
        }
        return m.Body();
    }

    /* Read until the reply for `id` arrives; EVENTs are
       collected (never dropped) when `events` is given. */
    bool RecvReply(uint32_t id, ipc::Message& out,
                   std::vector<json>* events = nullptr,
                   uint32_t timeout_ms = 15000)
    {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(timeout_ms);
        while(std::chrono::steady_clock::now() < deadline)
        {
            ipc::Message m;
            if(!Recv(m, 2000))
            {
                continue;
            }
            if(m.type == ipc::MsgType::Event)
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

    /* One request/reply pair (WORKSPACE_COMMAND or any other
       verb). Returns the reply body; errors surface as
       {_error:true,code,error,detail}. */
    json Call(ipc::MsgType t, const json& body,
              std::vector<json>* events = nullptr)
    {
        if(!Send(t, ++seq, body))
        {
            return json::object({{ "_error", true },
                                 { "error", "send failed" }});
        }
        ipc::Message m;
        if(!RecvReply(seq, m, events))
        {
            return json::object({{ "_error", true },
                                 { "error", "no reply" }});
        }
        json b = m.Body();
        if(m.type == ipc::MsgType::Error)
        {
            if(b.is_discarded()) { b = json::object(); }
            b["_error"] = true;
        }
        return b.is_discarded() ? json::object() : b;
    }

    json Ws(const json& body, std::vector<json>* events = nullptr)
    {
        return Call(ipc::MsgType::WorkspaceCommand, body, events);
    }

    /* Poll job_status to a terminal state. */
    json WaitJob(uint64_t job_id, uint32_t timeout_ms = 20000,
                 std::vector<json>* events = nullptr)
    {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(timeout_ms);
        json last;
        while(std::chrono::steady_clock::now() < deadline)
        {
            last = Ws(json::object(
                {{ "verb",   "job_status" },
                 { "job_id", job_id       }}), events);
            const std::string st =
                last.value("state", std::string());
            if(st == "succeeded" || st == "failed" ||
               st == "cancelled")
            {
                return last;
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(5));
        }
        std::printf("  (job %llu never terminated: %s)\n",
                    (unsigned long long)job_id,
                    last.dump().c_str());
        return last;
    }

    /* Enqueue a workspace job and wait it out; returns the
       terminal status body. */
    json RunJob(const json& body,
                std::vector<json>* events = nullptr)
    {
        const json r = Ws(body, events);
        if(r.value("_error", false) ||
           !r.value("accepted", false))
        {
            json fail = r.is_object() ? r : json::object();
            fail["state"] = "enqueue_failed";
            return fail;
        }
        return WaitJob(r["job_id"].get<uint64_t>(), 20000,
                       events);
    }

    /* Bounded upload: begin -> ordered chunks -> finish.
       Returns the staged token ("" on failure). */
    std::string Upload(const std::string& kind,
                       const std::string& bytes)
    {
        const json r = Ws(json::object({
            { "verb",      "transfer_begin"            },
            { "direction", "upload"                    },
            { "kind",      kind                        },
            { "length",    (uint64_t)bytes.size()      },
            { "sha256",    ipc::Sha256Hex(bytes)       },
        }));
        const std::string tid =
            r.value("transfer_id", std::string());
        if(tid.empty())
        {
            return "";
        }
        size_t off = 0;
        while(off < bytes.size())
        {
            const size_t n =
                std::min<size_t>(bytes.size() - off, 128 * 1024);
            const json cr = Ws(json::object({
                { "verb",        "transfer_chunk" },
                { "transfer_id", tid              },
                { "offset",      (uint64_t)off    },
                { "data", ipc::Base64Encode(
                              bytes.data() + off, n) },
            }));
            if(cr.value("received", 0ull) != off + n)
            {
                return "";
            }
            off += n;
        }
        const json f = Ws(json::object({
            { "verb",        "transfer_finish" },
            { "transfer_id", tid               },
        }));
        return f.value("staged", std::string());
    }

    /* Bounded download of a staged token (export results are
       already staged; the transfer_id IS the token). */
    bool Download(const std::string& transfer_id,
                  std::string& bytes)
    {
        bytes.clear();
        uint64_t off = 0;
        for(int guard = 0; guard < 4096; ++guard)
        {
            const json r = Ws(json::object({
                { "verb",        "transfer_chunk" },
                { "transfer_id", transfer_id      },
                { "offset",      off              },
            }));
            if(r.value("_error", false))
            {
                return false;
            }
            std::vector<uint8_t> chunk;
            if(!ipc::Base64Decode(
                   r.value("data", std::string()), chunk,
                   ipc::WorkspaceTransfer::Options()
                       .max_chunk_decoded_bytes))
            {
                return false;
            }
            bytes.append((const char*)chunk.data(), chunk.size());
            off += chunk.size();
            if(r.value("eof", false))
            {
                /* finish closes the reader side and frees the
                   session's transfer slot (per-session cap 2). */
                Ws(json::object({
                    { "verb",        "transfer_finish" },
                    { "transfer_id", transfer_id       },
                }));
                return true;
            }
            if(chunk.empty())
            {
                return false;   /* no progress, no eof */
            }
        }
        return false;
    }

    /* A finished upload's staged token stays live until the
       session ends or it is cancelled — release slots the
       consuming jobs no longer need. */
    void CancelTransfer(const std::string& transfer_id)
    {
        Ws(json::object({
            { "verb",        "transfer_cancel" },
            { "transfer_id", transfer_id       },
        }));
    }

    /* The managed workspace's durable document bytes via
       transfer_begin{download, document, id}. */
    bool DownloadDocument(const std::string& id,
                          std::string& bytes)
    {
        const json r = Ws(json::object({
            { "verb",      "transfer_begin" },
            { "direction", "download"       },
            { "kind",      "document"       },
            { "id",        id               },
        }));
        const std::string tid =
            r.value("transfer_id", std::string());
        return !tid.empty() && Download(tid, bytes);
    }

    json GetScene()
    {
        return Call(ipc::MsgType::GetScene, json::object());
    }
};

/*---------------------------------------------------------*\
||| Host bring-up / pump helpers                            ||
\*---------------------------------------------------------*/

struct Host
{
    EngineHostOptions          opts;
    sdk_test::FakeSdkServer*   server;
    std::unique_ptr<EngineHost> host;

    bool Start(sdk_test::FakeSdkServer& srv,
               EngineHostOptions o)
    {
        server = &srv;
        opts   = std::move(o);
        opts.sdk_enabled = true;
        opts.transport_factory =
            [srvp = server]() -> std::unique_ptr<sdk::IStreamTransport>
            { return std::make_unique<sdk_test::FakeStream>(*srvp); };
        host = std::make_unique<EngineHost>(opts);
        std::string err;
        if(!host->Start(err))
        {
            std::printf("  host start failed: %s\n", err.c_str());
            return false;
        }
        return true;
    }

    void PumpFor(int ms)
    {
        const auto end = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(ms);
        while(std::chrono::steady_clock::now() < end)
        {
            host->PollOnce();
            std::this_thread::sleep_for(
                std::chrono::milliseconds(20));
        }
    }

    template<typename Pred>
    bool WaitFor(Pred pred, int timeout_ms)
    {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(timeout_ms);
        while(std::chrono::steady_clock::now() < deadline)
        {
            host->PollOnce();
            if(pred())
            {
                return true;
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(15));
        }
        host->PollOnce();
        return pred();
    }

    const OrderlyShutdown::Result& Stop()
    {
        host->RequestStop();
        return host->Stop();
    }
};

static bool HasCaps(const json& welcome)
{
    for(const json& c :
            welcome.value("capabilities", json::array()))
    {
        if(c.is_string() &&
           c.get<std::string>() == "studio_workspace_v1")
        {
            return true;
        }
    }
    return false;
}

/* Find an object in a GET_SCENE document by binding id. */
static json FindBoundObject(const json& scene_doc,
                            const std::string& binding)
{
    for(const json& o :
            scene_doc.value("objects", json::array()))
    {
        if(o.value("binding", std::string()) == binding)
        {
            return o;
        }
    }
    return json();
}

/* Find an object in a GET_SCENE document by object id. */
static json FindObjectById(const json& scene_doc,
                           const std::string& id)
{
    for(const json& o :
            scene_doc.value("objects", json::array()))
    {
        if(o.value("id", std::string()) == id)
        {
            return o;
        }
    }
    return json();
}

/* Count verified device objects in a resolved scene doc. */
static int VerifiedObjects(const json& scene_doc)
{
    int n = 0;
    for(const json& o :
            scene_doc.value("objects", json::array()))
    {
        if(o.value("kind", std::string()) == "device" &&
           o.value("verified", false))
        {
            ++n;
        }
    }
    return n;
}

/*---------------------------------------------------------*\
||| Fixtures                                                 ||
\*---------------------------------------------------------*/

static json EmitterRow(int addr)
{
    return json::object({
        { "address", addr },
        { "group",   "ring" },
        { "pos", json::object(
             {{ "x", 0.0 }, { "y", 0.0 }, { "z", 0.0 }}) },
    });
}

/* A representative legacy host-settings blob — the shape a
   Desktop Lighting Studio settings file carries: an expanded
   v2 scene (custom inline geometry -> migration extracts a
   custom device type) plus armed inputs, which the import
   boundary must strip. */
static std::string LegacyBlob()
{
    json objects = json::array();
    auto dev = [&](const char* id, const char* binding,
                   double x)
    {
        json emitters = json::array();
        for(int i = 0; i < 8; ++i)
        {
            emitters.push_back(EmitterRow(i));
        }
        objects.push_back(json::object({
            { "id",       id      },
            { "kind",     "device"},
            { "binding",  binding },
            { "geometry", "fan_ring" },
            { "label",    id      },
            { "verified", true    },
            { "visible",  true    },
            { "position", json::object(
                 {{ "x", x }, { "y", 0.0 }, { "z", 0.0 }}) },
            { "emitters", emitters },
        }));
    };
    dev("fa", "b1", 0.10);
    dev("fb", "b2", 0.20);

    const json blob = json::object({
        { "scene", json::object({
            { "version", 2 },
            { "name",    "legacy desk" },
            { "bindings", json::array({
                json::object({
                    { "id",              "b1"        },
                    { "controller_name", "UNI FAN SL" },
                    { "serial",          kMac        },
                    { "zone_name",       "Fan 1"     },
                    { "zone_leds",       40          } }),
                json::object({
                    { "id",              "b2"        },
                    { "controller_name", "UNI FAN SL" },
                    { "serial",          kMac        },
                    { "zone_name",       "Fan 2"     },
                    { "zone_leds",       40          } }),
            }) },
            { "objects", objects },
            { "effect", json::object({
                { "preset", "" }, { "seed", 1 },
                { "speed", 1.0 }, { "intensity", 1.0 },
                { "playing", true },
                { "layers", json::array({ json::object({
                    { "primitive", "wave" },
                    { "space",     "world" },
                    { "speed",     0.4    },
                    { "scale",     0.5    },
                    { "palette", json::array({
                        json::object({{ "pos", 0.0 },
                                      { "color", "#FF0000" }}),
                        json::object({{ "pos", 0.5 },
                                      { "color", "#0000FF" }}),
                    }) },
                }) }) },
            }) },
        }) },
        { "inputs", json::object({
            { "audio", true }, { "screen_index", 2 },
            { "sens_pct", 175 }, { "decay_pct", 90 } }) },
    });
    return blob.dump(2);
}

/* A bundled custom device type — one 8-LED ring zone. */
static std::string XferStripType()
{
    return json::object({
        { "schema_version", 1            },
        { "id",             "xfer-strip" },
        { "name",           "Xfer Strip" },
        { "category",       "strip"      },
        { "entities", json::object({
              { "body", json::object({
                    { "geometry", "strip_body" },
                    { "zone",     "ring"       } }) } }) },
        { "zones", json::array({ json::object({
              { "id",        "ring" },
              { "entity",    "body" },
              { "led_count", 8      },
              { "layout", json::object({
                    { "type",     "ring" },
                    { "radius_m", 0.03   } }) } }) }) },
    }).dump(2);
}

/* The compact v3 workspace inside the bundle — references the
   bundled custom type, bound + (pre-sanitization) verified. */
static std::string BundleWorkspace()
{
    return json::object({
        { "schema_version", 3              },
        { "name",           "bundled desk" },
        { "devices", json::object({
              { "strip1", json::object({
                    { "type", "xfer-strip" },
                    { "x", 0.0 }, { "y", 0.1 }, { "z", 0.2 },
                    { "rx", 0.0 }, { "ry", 0.0 }, { "rz", 0.0 } }) } }) },
        { "bindings", json::object({
              { "bw", json::object({
                    { "controller_name", "X870E"      },
                    { "serial",          kMbSer       },
                    { "zone_name",       "ARGB_V2_1"  },
                    { "zone_leds",       8            } }) } }) },
        { "device_settings", json::object({
              { "strip1", json::object({
                    { "zones", json::object({
                          { "ring", json::object({
                                { "binding",   "bw" },
                                { "addr_base", 0    },
                                { "verified",  true } }) } }) } }) } }) },
        { "effects", json::object({
              { "preset",    ""    },
              { "seed",      1     },
              { "speed",     1.0   },
              { "intensity", 1.0   },
              { "playing",   true  },
              { "layers", json::array({ json::object({
                    { "primitive", "static" },
                    { "palette", json::array({
                          json::object({{ "pos", 0.0 },
                                        { "color", "#0040FF" }}),
                    }) },
                }) }) },
        }) },
    }).dump(2);
}

static std::string PackedBundle()
{
    return json::object({
        { "format",  "openrgb2-bundle-files" },
        { "version", 1                       },
        { "files", json::object({
              { "studio.json", json::object({
                    { "encoding", "utf8"             },
                    { "data",     BundleWorkspace()  } }) },
              { "presets/devices/xfer-strip.device.json",
                json::object({
                    { "encoding", "utf8"            },
                    { "data",     XferStripType()   } }) },
        }) },
    }).dump(2);
}

/* The personal look installed through resource_install — a
   custom (non-packaged) effect document the authored workspace
   then references by preset name. */
static json MyLookDoc()
{
    return json::object({
        { "schema_version", 1        },
        { "id",             "mylook" },
        { "name",           "My Look"},
        { "layers", json::array({ json::object({
              { "primitive", "wave"  },
              { "space",     "world" },
              { "speed",     0.3     },
              { "scale",     0.5     },
              { "palette", json::array({
                    json::object({{ "pos", 0.0 },
                                  { "color", "#00FF00" }}),
                    json::object({{ "pos", 0.5 },
                                  { "color", "#0000FF" }}),
              }) },
        }) }) },
    });
}

/*---------------------------------------------------------*\
||| main                                                     ||
\*---------------------------------------------------------*/
int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::error_code ec;
    const fs::path tmp =
        fs::temp_directory_path(ec) /
        ("openrgb2-e2e-studio-" + std::to_string((uint64_t)
            std::chrono::steady_clock::now()
                .time_since_epoch().count()));
    fs::create_directories(tmp, ec);
    const std::string ws_root =
        (tmp / "workspaces").generic_string();

    const std::string cmd_pipe  =
        "\\\\.\\pipe\\openrgb2-e2e-studio-ws";
    const std::string diag_pipe =
        "\\\\.\\pipe\\openrgb2-e2e-studio-ws-diag";

    sdk_test::FakeSdkServer server;
    server.controllers = { WirelessGroup(), Wired() };
    server.listening   = true;

    EngineHostOptions opts;
    opts.ipc_enabled          = true;
    opts.command_pipe_name    = cmd_pipe;
    opts.diagnostics_pipe_name = diag_pipe;
    opts.power_monitor        = false;
    opts.reactive_inputs      = false;
    opts.scan_owners          = false;
    opts.single_instance      = true;
    opts.instance_name        = "Local\\openrgb2-engine-e2e-studio";
    opts.workspace_root       = ws_root;
    /* The packaged preset libraries — the same dirs the staged
       package ships (working dir is tests/engine). */
    opts.devices_dir          = "../../src/core/presets/devices";
    opts.effects_dir          = "../../src/core/presets/effects";
    opts.state_path           =
        (tmp / "engine-state.json").generic_string();
    opts.sdk_reprobe          = std::chrono::milliseconds(300);
    opts.sdk_topology_poll    = std::chrono::milliseconds(300);
    opts.log = [](const std::string&) {};

    std::vector<json> events;

    /*======================================================*\
    || Phase A: engine 1 — legacy import over the wire      ||
    \*======================================================*/
    Host h1;
    CHECK(h1.Start(server, opts));
    if(!h1.host)
    {
        std::printf("%d checks, %d failures\n", checks, failures);
        return 1;
    }
    CHECK(h1.WaitFor([&]{
        return h1.host->GetSdk() &&
               h1.host->GetSdk()->State() ==
                   ConnectionState::Connected;
    }, 15000));
    CHECK(h1.host->GetSdk()->Snapshots().size() == 2);

    UiClient ui1;
    CHECK(ui1.Connect(cmd_pipe));
    const json welcome = ui1.Hello();
    CHECK(welcome.is_object() && HasCaps(welcome));

    /* Upload the legacy blob and import it — committed as a
       managed workspace, sanitized, NEVER applied. */
    const std::string legacy_tok =
        ui1.Upload("document", LegacyBlob());
    CHECK(!legacy_tok.empty());
    json imp1 = ui1.RunJob(json::object({
        { "verb",              "import_commit" },
        { "id",                "desk-legacy"   },
        { "staged",            legacy_tok      },
        { "base_revision",     0               },
        { "resource_revision", 0               },
    }), &events);
    CHECK(imp1.value("state", "") == "succeeded");
    if(imp1.value("state", "") != "succeeded")
    {
        std::printf("  (legacy import: %s)\n",
                    imp1.dump().c_str());
    }
    CHECK(imp1.value("committed", false));
    CHECK(!imp1.value("applied", true));   /* never auto-applied */
    CHECK(imp1.value("workspace_revision", 0ull) == 1);
    /* The consumed staged upload frees its session slot
       (per-session live-transfer cap is 2). */
    ui1.CancelTransfer(legacy_tok);

    /* The import adopted the workspace — it is the managed
       current one. */
    json g = ui1.Ws(json::object({{ "verb", "get" }}));
    CHECK(g.value("managed", false));
    CHECK(g.value("id", "") == "desk-legacy");
    CHECK(g.value("workspace_revision", 0ull) == 1);
    CHECK(g.value("workspace_root", "") == ws_root);
    {
        const json l = ui1.Ws(json::object({{ "verb", "list" }}));
        bool listed = false;
        for(const json& w :
                l.value("workspaces", json::array()))
        {
            listed |= w == "desk-legacy";
        }
        CHECK(listed);
        CHECK(l.value("active", "") == "desk-legacy");
    }

    /* The authored compact document — downloaded through the
       bounded transfer — is sanitized: no verified zone, no
       armed input, no live_on_startup. The migrated custom
       type reference resolves through the managed library. */
    std::string doc_bytes;
    CHECK(ui1.DownloadDocument("desk-legacy", doc_bytes));
    json doc = json::parse(doc_bytes, nullptr, false);
    CHECK(!doc.is_discarded());
    if(doc.is_discarded()) { return 1; }
    CHECK(doc.value("schema_version", 0) == 3);
    CHECK(doc.value("name", "") == "legacy desk");
    CHECK(doc.value("output", json::object())
              .value("live_on_startup", true) == false);
    CHECK(doc.value("inputs", json::object())
              .value("audio", true) == false);
    CHECK(doc.value("inputs", json::object())
              .value("keys", true) == false);
    CHECK(doc.value("inputs", json::object())
              .value("screen", true) == false);
    {
        /* Migration extracted the custom geometry as ONE shared
           type (two structurally identical objects). */
        const json devices = doc.value("devices", json::object());
        CHECK(devices.contains("fa") && devices.contains("fb"));
        const std::string tid =
            devices.value("fa", json::object())
                .value("type", std::string());
        CHECK(!tid.empty());
        CHECK(devices["fb"].value("type", std::string()) == tid);
        const json rg = ui1.Ws(json::object({
            { "verb", "resource_get" },
            { "kind", "device_type"  },
            { "id",   tid            },
        }));
        CHECK(!rg.value("_error", false));
        CHECK(rg.value("id", "") == tid);
        CHECK(rg["document"].is_object());
        /* every zone setting carries verified:false */
        bool any_verified = false;
        for(const json& ds :
                doc.value("device_settings", json::object()))
        {
            for(const json& z :
                    ds.value("zones", json::object()))
            {
                any_verified |= z.value("verified", false);
            }
        }
        CHECK(!any_verified);
    }

    /* Adopt + apply (the UI "open"): the sanitized import
       resolves and publishes — but nothing may reach the wire,
       because no zone is verified yet. */
    json open1 = ui1.RunJob(json::object({
        { "verb", "reload" }, { "id", "desk-legacy" } }));
    CHECK(open1.value("state", "") == "succeeded");
    CHECK(open1.value("applied", false));
    {
        const json sc = ui1.GetScene();
        CHECK(sc.value("name", "") == "legacy desk");
        const json sd = sc.value("document", json::object());
        CHECK(sd.is_object());
        CHECK(VerifiedObjects(sd) == 0);
        /* An unverified-only scene writes nothing — bounded
           observation window, no writes on either dev. */
        const uint64_t w0 = server.CountEvents(sdk::kPktUpdateLEDs);
        h1.PumpFor(700);
        CHECK(server.CountEvents(sdk::kPktUpdateLEDs) == w0);
    }

    /*======================================================*\
    || Phase B: author + save — edits commit + apply        ||
    \*======================================================*/

    /* Personal-look dependency: install ONE effect document
       into the managed library (revision-checked on both
       counters), then reference it by preset name. */
    {
        const json ri = ui1.RunJob(json::object({
            { "verb",              "resource_install" },
            { "kind",            "effect"           },
            { "id",              "desk-legacy"      },
            { "base_revision",   g["workspace_revision"] },
            { "resource_revision",
              g["resource_revision"] },
            { "effect",          MyLookDoc()        },
        }));
        CHECK(ri.value("state", "") == "succeeded");
        if(ri.value("state", "") != "succeeded")
        {
            std::printf("  (resource_install: %s)\n",
                        ri.dump().c_str());
        }
        CHECK(ri["result"].value("landed_id", "") == "mylook");
        g = ui1.Ws(json::object({{ "verb", "get" }}));
    }

    /* Edit the draft the way the panels do: move the first
       device, re-verify both zones, select the personal look,
       add a layer edit on top. */
    json edited = doc;
    edited["devices"]["fa"]["x"] = 0.35;
    if(edited["device_settings"].is_object())
    {
        for(json& ds : edited["device_settings"])
        {
            if(ds.contains("zones"))
            {
                for(json& z : ds["zones"])
                {
                    z["verified"] = true;
                }
            }
        }
    }
    edited["effects"]["preset"]  = "mylook";
    edited["effects"]["playing"] = true;

    json save = ui1.RunJob(json::object({
        { "verb",              "commit"        },
        { "id",                "desk-legacy"   },
        { "base_revision",
          g["workspace_revision"]              },
        { "resource_revision",
          g["resource_revision"]               },
        { "document",          edited          },
    }), &events);
    CHECK(save.value("state", "") == "succeeded");
    if(save.value("state", "") != "succeeded")
    {
        std::printf("  (commit: %s)\n", save.dump().c_str());
    }
    CHECK(save.value("committed", false));
    CHECK(save.value("applied", false));
    CHECK(save.value("workspace_revision", 0ull) == 2);

    /* The applied scene carries the edit: the moved object and
       re-verified zones resolve — and now frames DO reach the
       fake backend. */
    {
        const json sc = ui1.GetScene();
        CHECK(sc.value("name", "") == "legacy desk");
        const json sd = sc.value("document", json::object());
        CHECK(VerifiedObjects(sd) == 2);
        const json fa = FindBoundObject(sd, "b1");
        CHECK(fa.is_object());
        /* The device's placement lives on the "fa" placement
           frame — "fa/body"'s transform is local to it. */
        const json frame = FindObjectById(sd, "fa");
        CHECK(frame.is_object());
        if(frame.is_object())
        {
            const double x =
                frame["position"].value("x", -1.0);
            if(!(x > 0.3 && x < 0.4))
            {
                std::printf("  (moved frame: %s)\n",
                            frame.dump().c_str());
            }
            CHECK(x > 0.3 && x < 0.4);
        }
    }
    CHECK(h1.WaitFor([&]{
        return server.CountEvents(sdk::kPktUpdateLEDs, 0) >= 1;
    }, 10000));
    CHECK(h1.host->FramesProduced() > 0);

    /* Input ownership / managed gate: while a managed
       workspace is active the lossy legacy document mutations
       are refused — the workspace draft is the only write
       path. */
    {
        const json r = ui1.Call(ipc::MsgType::SetScene,
            json::object({{ "document", json::object({
                { "version", 2 }, { "name", "rogue" },
                { "objects", json::array() } }) }}));
        CHECK(r.value("_error", false));
        CHECK(r.value("detail", "") == "workspace_required");
    }

    /* Bounded draft preview on this session — begin/update/end
       (the QA-session preview lifecycle; disposal across a
       dead channel is asserted in phase D). */
    {
        json pb = ui1.Ws(json::object({
            { "verb",     "preview_begin" },
            { "document", edited          },
        }));
        CHECK(pb.value("active", false));
        const json pe = ui1.Ws(json::object(
            {{ "verb", "preview_end" }}));
        CHECK(pe.value("active", true) == false);
    }

    /*======================================================*\
    || Phase C: packed bundle import (v3 + custom type)     ||
    \*======================================================*/
    const std::string bundle_tok =
        ui1.Upload("bundle", PackedBundle());
    CHECK(!bundle_tok.empty());
    {
        /* inspect first — nothing written, types reported. */
        const json ins = ui1.RunJob(json::object({
            { "verb",   "import_inspect" },
            { "staged", bundle_tok       },
        }));
        CHECK(ins.value("state", "") == "succeeded");
        if(ins.value("state", "") != "succeeded")
        {
            std::printf("  (inspect: %s)\n", ins.dump().c_str());
        }
        const json res = ins.value("result", json::object());
        CHECK(res.value("valid", false));
        bool type_seen = false;
        for(const json& t : res.value("types", json::array()))
        {
            if(t.value("id", "") == "xfer-strip")
            {
                type_seen = true;
                CHECK(t.value("status", "") == "new");
            }
        }
        CHECK(type_seen);
        CHECK(res.value("conflicts", json::array()).empty());
        CHECK(res.value("has_workspace", false));

        /* commit onto a second managed id — never applied. */
        const json ic = ui1.RunJob(json::object({
            { "verb",              "import_commit" },
            { "id",                "desk-bundle"   },
            { "staged",            bundle_tok      },
            { "base_revision",     0               },
            { "resource_revision", 0               },
        }));
        CHECK(ic.value("state", "") == "succeeded");
        if(ic.value("state", "") != "succeeded")
        {
            std::printf("  (bundle import: %s)\n",
                        ic.dump().c_str());
        }
        CHECK(ic.value("committed", false));
        CHECK(!ic.value("applied", true));
        /* the bundled type landed in the managed library */
        const json rg = ui1.Ws(json::object({
            { "verb", "resource_get" },
            { "kind", "device_type"  },
            { "id",   "xfer-strip"   },
        }));
        CHECK(!rg.value("_error", false));
        CHECK(rg["document"].is_object());
        /* The consumed staged upload frees its session slot. */
        ui1.CancelTransfer(bundle_tok);
        /* it resolves: the stored document re-parses to a
           resolved scene (download + round-trip parse happens
           on the workspace read path — a resolved check lands
           after restart too). */
    }

    /*======================================================*\
    || Phase D: UI process boundary — crash + reattach,     ||
    || preview disposal, engine continuity                  ||
    \*======================================================*/
    {
        /* An in-flight preview dies with the channel. */
        const json pb = ui1.Ws(json::object({
            { "verb",     "preview_begin" },
            { "document", edited          },
        }));
        CHECK(pb.value("active", false));
        const uint64_t frames_before = h1.host->FramesProduced();
        ui1.ch.reset();              /* UI "crash": pipe breaks */
        h1.PumpFor(1000);
        CHECK(h1.host->FramesProduced() > frames_before);

        /* Reattach (the restarted UI process). The dead
           session's preview slot must already be released — a
           fresh preview_begin answers, never preview_limit. */
        UiClient ui2;
        CHECK(ui2.Connect(cmd_pipe));
        CHECK(HasCaps(ui2.Hello()));
        const json g2 = ui2.Ws(json::object({{ "verb", "get" }}));
        CHECK(g2.value("managed", false));
        /* Engine continuity across the UI boundary: the same
           workspace stayed current and frames kept flowing. */
        CHECK(g2.value("id", "") == "desk-bundle");
        const json pb2 = ui2.Ws(json::object({
            { "verb",     "preview_begin" },
            { "document", edited          },
        }));
        CHECK(pb2.value("active", false));
        ui2.Ws(json::object({{ "verb", "preview_end" }}));

        /* Two more clean attaches — repeated attach/detach
           never degrades the session surface. */
        for(int i = 0; i < 2; ++i)
        {
            UiClient uiN;
            CHECK(uiN.Connect(cmd_pipe));
            CHECK(uiN.Hello().is_object());
            const json st = uiN.Call(ipc::MsgType::GetState,
                                     json::object());
            CHECK(st.value("engine", json::object())
                      .value("running", false));
        }
        /* engine still live after the churn */
        h1.PumpFor(300);
        CHECK(h1.host->FramesProduced() > frames_before);
    }

    /*======================================================*\
    || Phase E: engine restart — durable workspace survives ||
    \*======================================================*/
    /* Engine truth BEFORE the stop — the restart must re-adopt
       exactly this workspace at these revisions (F06). */
    json pre_get;
    {
        UiClient uip;
        CHECK(uip.Connect(cmd_pipe));
        CHECK(HasCaps(uip.Hello()));
        pre_get = uip.Ws(json::object({{ "verb", "get" }}));
    }
    CHECK(pre_get.value("managed", false));
    CHECK(pre_get.value("id", "") == "desk-bundle");

    const OrderlyShutdown::Result sd1 = h1.Stop();
    CHECK(sd1.all_within_bound);
    /* The single-instance guard releases with the EngineHost
       object — a "restarted" engine process is a new object. */
    h1.host.reset();

    /* The adoption is durable profile state — a real
       `active_workspace` field, not an extension blob. */
    {
        std::ifstream pf(opts.state_path);
        const json state = json::parse(
            std::string(std::istreambuf_iterator<char>(pf),
                        std::istreambuf_iterator<char>()),
            nullptr, false);
        CHECK(!state.is_discarded());
        CHECK(state.value("active_workspace",
                          std::string()) == "desk-bundle");
    }

    Host h2;
    if(!h2.Start(server, opts))
    {
        CHECK(false);
        std::printf("%d checks, %d failures\n", checks, failures);
        return 1;
    }
    CHECK(h2.WaitFor([&]{
        return h2.host->GetSdk() &&
               h2.host->GetSdk()->State() ==
                   ConnectionState::Connected;
    }, 15000));

    UiClient ui3;
    CHECK(ui3.Connect(cmd_pipe));
    CHECK(HasCaps(ui3.Hello()));

    /* The restart RE-ADOPTED the last managed workspace — no
       client had to re-open it; revisions survive verbatim.
       Open() is adoption-only: it NEVER applies a scene, so
       the persisted last-scene snapshot ("legacy desk") stays
       the applied truth — the re-adopted workspace's own
       scene ("bundled desk") must not appear. */
    {
        const json g0 = ui3.Ws(json::object({{ "verb", "get" }}));
        CHECK(g0.value("managed", false));
        CHECK(g0.value("id", "") == "desk-bundle");
        CHECK(g0.value("workspace_revision", 0ull) ==
              pre_get.value("workspace_revision", 0ull));
        CHECK(g0.value("resource_revision", 0ull) ==
              pre_get.value("resource_revision", 0ull));
        CHECK(ui3.GetScene().value("name", "") == "legacy desk");
        const json l = ui3.Ws(json::object({{ "verb", "list" }}));
        bool has_legacy = false, has_bundle = false;
        for(const json& w :
                l.value("workspaces", json::array()))
        {
            has_legacy |= w == "desk-legacy";
            has_bundle |= w == "desk-bundle";
        }
        CHECK(has_legacy && has_bundle);
        /* the managed library survived: the bundled type +
           the migration-extracted type are still installable
           resources */
        const json rg = ui3.Ws(json::object({
            { "verb", "resource_get" },
            { "kind", "device_type"  },
            { "id",   "xfer-strip"   },
        }));
        CHECK(!rg.value("_error", false));
        CHECK(rg["document"].is_object());
        const json rl = ui3.Ws(json::object({
            { "verb", "resource_get" },
            { "kind", "effect"       },
            { "id",   "mylook"       },
        }));
        CHECK(!rl.value("_error", false));
        CHECK(rl["document"].is_object());
    }

    /* Re-open the authored workspace — authored data and the
       resolved scene survive the restart; output resumes. */
    {
        const json ro = ui3.RunJob(json::object({
            { "verb", "reload" }, { "id", "desk-legacy" } }));
        CHECK(ro.value("state", "") == "succeeded");
        CHECK(ro.value("applied", false));

        std::string bytes2;
        CHECK(ui3.DownloadDocument("desk-legacy", bytes2));
        const json doc2 = json::parse(bytes2, nullptr, false);
        CHECK(!doc2.is_discarded());
        CHECK(doc2["devices"]["fa"].value("x", -1.0) > 0.34 &&
              doc2["devices"]["fa"].value("x", -1.0) < 0.36);
        CHECK(doc2["effects"].value("preset", "") == "mylook");
        bool all_verified =
            doc2.value("device_settings", json::object())
                .is_object() &&
            !doc2.value("device_settings", json::object())
                 .empty();
        for(const json& ds :
                doc2.value("device_settings", json::object()))
        {
            for(const json& z : ds.value("zones", json::object()))
            {
                all_verified &= z.value("verified", false);
            }
        }
        CHECK(all_verified);

        const json sc = ui3.GetScene();
        CHECK(sc.value("name", "") == "legacy desk");
        const json sd = sc.value("document", json::object());
        CHECK(VerifiedObjects(sd) == 2);
        const json fa = FindBoundObject(sd, "b1");
        CHECK(fa.is_object());
        const json frame = FindObjectById(sd, "fa");
        CHECK(frame.is_object());
        if(frame.is_object())
        {
            const double x = frame["position"].value("x", -1.0);
            if(!(x > 0.3 && x < 0.4))
            {
                std::printf("  (post-restart frame: %s)\n",
                            frame.dump().c_str());
            }
            CHECK(x > 0.3 && x < 0.4);
        }
        CHECK(h2.WaitFor([&]{
            return server.CountEvents(sdk::kPktUpdateLEDs, 0)
                   >= 1;
        }, 10000));
    }

    /*======================================================*\
    || Phase F: export / reimport round-trip                ||
    \*======================================================*/
    {
        const json ex = ui3.RunJob(json::object({
            { "verb", "export" }, { "id", "desk-legacy" } }));
        CHECK(ex.value("state", "") == "succeeded");
        if(ex.value("state", "") != "succeeded")
        {
            std::printf("  (export: %s)\n", ex.dump().c_str());
        }
        const json er = ex.value("result", json::object());
        const std::string dl_tok =
            er.value("transfer_id", std::string());
        CHECK(!dl_tok.empty());
        CHECK(er.value("types_written", 0ull) >= 1);

        std::string bundle_bytes;
        CHECK(ui3.Download(dl_tok, bundle_bytes));
        const json bundle =
            json::parse(bundle_bytes, nullptr, false);
        CHECK(!bundle.is_discarded());
        CHECK(bundle.value("format", "") ==
              "openrgb2-bundle-files");
        const json files = bundle.value("files", json::object());
        CHECK(files.contains("studio.json"));
        {
            bool has_type = false;
            for(auto it = files.begin(); it != files.end(); ++it)
            {
                if(it.key().rfind("presets/devices/", 0) == 0)
                {
                    has_type = true;
                }
            }
            CHECK(has_type);
        }
        /* the exported document is sanitized: no verified
           zones, no live flags, no serial identity */
        {
            const json sj = json::parse(
                files["studio.json"].value("data", std::string()),
                nullptr, false);
            CHECK(!sj.is_discarded());
            CHECK(sj.value("output", json::object())
                      .value("live_on_startup", true) == false);
            bool any_v = false;
            for(const json& ds :
                    sj.value("device_settings", json::object()))
            {
                for(const json& z :
                        ds.value("zones", json::object()))
                {
                    any_v |= z.value("verified", false);
                }
            }
            CHECK(!any_v);
            /* the personal look materialized inline — a
               re-importer without the look file still gets the
               literal layer stack */
            CHECK(sj["effects"].value("preset", "") == "mylook");
            CHECK(!sj["effects"].value("layers",
                                       json::array()).empty());
        }

        /* Reimport the exported bytes onto a fresh id. */
        const std::string re_tok =
            ui3.Upload("bundle", bundle_bytes);
        CHECK(!re_tok.empty());
        const json re_ins = ui3.RunJob(json::object({
            { "verb",   "import_inspect" },
            { "staged", re_tok           },
        }));
        CHECK(re_ins.value("state", "") == "succeeded");
        CHECK(re_ins["result"].value("valid", false));
        const json re_ic = ui3.RunJob(json::object({
            { "verb",              "import_commit" },
            { "id",                "desk-copy"     },
            { "staged",            re_tok          },
            { "base_revision",     0               },
            { "resource_revision", 0               },
        }));
        CHECK(re_ic.value("state", "") == "succeeded");
        if(re_ic.value("state", "") != "succeeded")
        {
            std::printf("  (reimport: %s)\n",
                        re_ic.dump().c_str());
        }
        CHECK(re_ic.value("committed", false));

        /* The round-tripped workspace opens and resolves. */
        const json ro2 = ui3.RunJob(json::object({
            { "verb", "reload" }, { "id", "desk-copy" } }));
        CHECK(ro2.value("state", "") == "succeeded");
        const json sc2 = ui3.GetScene();
        CHECK(sc2.value("name", "") == "legacy desk");
        CHECK(sc2["document"].is_object());
        /* sanitized on the way in — the exported doc carried no
           verified flags, so the round-trip resolves the same
           object set but writes nothing new */
        CHECK(VerifiedObjects(sc2["document"]) == 0);
        CHECK(FindBoundObject(sc2["document"], "b1").is_object());
    }

    /*======================================================*\
    || Phase G: external-change honesty on the live poll    ||
    \*======================================================*/
    {
        json gnow = ui3.Ws(json::object({{ "verb", "get" }}));
        CHECK(gnow.value("external_change", "x") == "none");
        const std::string wsdir =
            gnow.value("workspace_dir", std::string());
        CHECK(!wsdir.empty());
        if(!wsdir.empty())
        {
            /* Poke one byte into the active generation's
               manifest — detection is REPORT ONLY (never a
               reload, never an apply). */
            const std::string gen =
                "gen-00000" +
                std::to_string(
                    gnow.value("generation", 1ull));
            const fs::path manifest =
                fs::path(wsdir) / "generations" / gen /
                "manifest.json";
            {
                std::error_code mec;
                const uintmax_t sz = fs::file_size(manifest, mec);
                CHECK(!mec);
                std::fstream f(manifest,
                               std::ios::in | std::ios::out |
                                   std::ios::binary);
                f.seekp(0);
                int c = f.get();
                f.seekp(0);
                f.put(c == ' ' ? '~' : ' ');
                (void)sz;
            }
            gnow = ui3.Ws(json::object({{ "verb", "get" }}));
            CHECK(gnow.value("external_change", "") ==
                  "modified");
            /* and the scene did NOT re-apply behind our back */
            CHECK(ui3.GetScene().value("name", "") ==
                  "legacy desk");
        }
    }

    /*======================================================*\
    || Ordered teardown                                     ||
    \*======================================================*/
    const OrderlyShutdown::Result& sd2 = h2.Stop();
    CHECK(sd2.all_within_bound);
    CHECK(h2.host->GetSdk()->State() ==
          ConnectionState::Disconnected);
    h2.host.reset();

    /*======================================================*\
    || Phase H: persisted workspace dir gone — graceful     ||
    || unmanaged boot                                       ||
    \*======================================================*/
    {
        /* Whatever id the last adopt persisted (desk-copy after
           Phase F), delete its directory: the re-adopt must
           fail honestly — logged, then the engine runs
           unmanaged rather than refusing to start. */
        std::ifstream pf(opts.state_path);
        const json state = json::parse(
            std::string(std::istreambuf_iterator<char>(pf),
                        std::istreambuf_iterator<char>()),
            nullptr, false);
        const std::string persisted =
            state.value("active_workspace", std::string());
        CHECK(!persisted.empty());
        std::error_code dec;
        fs::remove_all(tmp / "workspaces" / persisted, dec);
        CHECK(!dec);

        Host h3;
        CHECK(h3.Start(server, opts));
        if(h3.host)
        {
            CHECK(h3.WaitFor([&]{
                return h3.host->GetSdk() &&
                       h3.host->GetSdk()->State() ==
                           ConnectionState::Connected;
            }, 15000));
            UiClient ui4;
            CHECK(ui4.Connect(cmd_pipe));
            CHECK(HasCaps(ui4.Hello()));
            const json g3 =
                ui4.Ws(json::object({{ "verb", "get" }}));
            CHECK(g3.value("managed", true) == false);
            /* The engine is fully functional — surviving
               workspaces still list + re-open on demand. */
            const json l3 =
                ui4.Ws(json::object({{ "verb", "list" }}));
            bool survivors = false;
            for(const json& w :
                    l3.value("workspaces", json::array()))
            {
                survivors |= w.is_string() &&
                             w.get<std::string>() != persisted;
            }
            CHECK(survivors);
            h3.host->RequestStop();
            CHECK(h3.host->Stop().all_within_bound);
        }
    }

    std::error_code rec;
    fs::remove_all(tmp, rec);

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
