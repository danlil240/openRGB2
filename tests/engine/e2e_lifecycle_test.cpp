/*---------------------------------------------------------*\
||| e2e_lifecycle_test.cpp                                  ||
|||                                                           ||
|||   Task 5.3 lifecycle matrix — the simulable rows,       ||
|||   driven against the REAL EngineHost production         ||
|||   composition (ownership scan -> SDK attach ->          ||
|||   enumerate -> bind -> 60 Hz frames -> delivery lanes   ||
|||   -> real named-pipe IPC listeners) with the in-proc    ||
|||   fake SDK backend standing in for OpenRGB:             ||
|||                                                           ||
|||   A. cold start with the backend DOWN then up — the     ||
|||      engine reports "not attached" honestly, keeps      ||
|||      retrying on the bounded schedule, and attaches +   ||
|||      enumerates + starts frames when the backend        ||
|||      arrives late. The competing-owner scan also runs   ||
|||      here: a renamed sleeper process posing as a second ||
|||      OpenRGB.exe must be REPORTED as a conflict and     ||
|||      must still be alive at teardown (never killed).    ||
|||   B. UI crash — a real named-pipe client session        ||
|||      (HELLO/STATE) is severed mid-run; the engine keeps ||
|||      producing frames and a fresh session rejoins       ||
|||      honestly.                                          ||
|||   C. unplug/replug while live — RemoveController        ||
|||      mid-stream must be detected by the periodic        ||
|||      topology refresh (misrouting bounded to the poll   ||
|||      interval), and the replugged group must re-resolve ||
|||      to the same DeviceKey at its new positional index. ||
|||   D. display sleep (injected DisplayOff/On with an      ||
|||      uploaded loop active) — all output gated, exactly  ||
|||      one freeze write, revalidation before restore.     ||
|||   E. system suspend/resume (injected Suspend/Resume     ||
|||      with the transport dying across the boundary and   ||
|||      the backend losing the uploaded loop) — parked,    ||
|||      then revalidated restore.                          ||
|||   F. backend outage longer than the reconnect backoff — ||
|||      the client latches Disconnected; the owner-loop    ||
|||      re-probe (Task 5.3 fix) must re-arm it so a        ||
|||      backend that returns late still re-attaches.       ||
|||                                                           ||
|||   Display/suspend events arrive through the same        ||
|||   PostEvent surface the production WinPowerMonitor      ||
|||   feeds — the state machine, gate, freeze, and restore  ||
|||   paths under test are the production ones; only the    ||
|||   OS event source is injected. Physical-rig evidence    ||
|||   for the non-simulable rows (real suspend, physical    ||
|||   unplug, real display power) is documented in          ||
|||   docs/benchmarks/lifecycle-matrix.md.                  ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
||\*---------------------------------------------------------*/

#include "EngineHost.h"
#include "EngineService.h"
#include "SdkWire.h"
#include "../sdk/fake_sdk_server.h"

#include "scene/SceneJson.h"
#include "scene/SceneTypes.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace openrgb2;
using nlohmann::json;

#define CHECK(cond) do { if(!(cond)) { \
    fprintf(stderr, "FAIL %d: %s\n", __LINE__, #cond); \
    return 1; } } while(0)
#define CHECKV(cond, ...) do { if(!(cond)) { \
    fprintf(stderr, "FAIL %d: %s\n", __LINE__, #cond); \
    fprintf(stderr, "  " __VA_ARGS__); fputc('\n', stderr); \
    return 1; } } while(0)

/*---------------------------------------------------------*\
||| Fake SDK fixture (same shape as the e2e suites)        ||
\*---------------------------------------------------------*/

static const char* kMac = "FA:KE:53:00:00:01";

static sdk_test::FakeController WirelessGroup()
{
    sdk_test::FakeController c;
    c.type     = 5;
    c.name     = "UNI FAN SL V3 Wireless LIFECYCLE";
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
    c.name     = "X870E AORUS ELITE WIFI7 ICE LIFECYCLE";
    c.vendor   = "GIGABYTE";
    c.serial   = "MB-LIFECYCLE-1";
    c.location = "smbus:0-motherboard-lifecycle";
    sdk_test::FakeMode m;
    m.name = "Direct";
    c.modes.push_back(m);
    c.zones = { { "ARGB_V2_1", 0, 8, 8, 8 } };
    c.colors.assign(8, 0x00010101u);
    return c;
}

/*---------------------------------------------------------*\
||| Scene documents (version-2 schema, e2e shape)          ||
\*---------------------------------------------------------*/

static json EmitterArray(int first, int count)
{
    json e = json::array();
    for(int i = 0; i < count; ++i)
    {
        e.push_back({ { "pos", { {"x",0},{"y",0},{"z",0} } },
                      { "group", "" },
                      { "address", first + i } });
    }
    return e;
}

static json Bindings()
{
    json bindings = json::array();
    auto bind = [&](const char* id, const char* zone, int leds)
    {
        bindings.push_back({
            { "id",              id     },
            { "controller_name", "UNI FAN SL" },
            { "serial",          kMac   },
            { "zone_name",       zone   },
            { "zone_leds",       leds   },
        });
    };
    bind("b-fan1", "Fan 1", 40);
    bind("b-fan2", "Fan 2", 40);
    bind("b-fan3", "Fan 3", 40);
    bindings.push_back({
        { "id",              "b-argb"    },
        { "controller_name", "X870E"     },
        { "zone_name",       "ARGB_V2_1" },
        { "zone_leds",       8           },
    });
    return bindings;
}

static json Objects()
{
    json objects = json::array();
    auto obj = [&](const char* id, const char* binding, int n)
    {
        objects.push_back({
            { "id", id }, { "kind", "device" },
            { "binding", binding }, { "verified", true },
            { "visible", true },
            { "emitters", EmitterArray(0, n) },
        });
    };
    obj("o-fan1", "b-fan1", 40);
    obj("o-fan2", "b-fan2", 40);
    obj("o-fan3", "b-fan3", 40);
    obj("o-argb", "b-argb", 8);
    return objects;
}

static json LiveSceneDoc()
{
    return {
        { "version", 2 },
        { "name",    "lifecycle-live" },
        { "brightness", 1.0 },
        { "bindings", Bindings() },
        { "objects",  Objects() },
        { "effect", {
            { "preset",    "" },
            { "seed",      1 },
            { "speed",     1.0 },
            { "intensity", 1.0 },
            { "playing",   true },
            { "layers", json::array({ {
                { "primitive", "wave" },
                { "space",     "world" },
                { "speed",     0.4 },
                { "scale",     0.5 },
                { "palette", json::array({
                    { { "pos", 0.0 }, { "color", "#FF0000" } },
                    { { "pos", 0.5 }, { "color", "#0000FF" } },
                }) },
            } }) },
        } },
    };
}

static json PlaybackSceneDoc()
{
    return {
        { "version", 2 },
        { "name",    "lifecycle-playback" },
        { "brightness", 1.0 },
        { "bindings", Bindings() },
        { "objects",  Objects() },
        { "effect", {
            { "preset",    "rainbow_wave" },
            { "seed",      1 },
            { "speed",     1.0 },
            { "intensity", 1.0 },
            { "playing",   true },
            { "layers",    json::array() },
        } },
    };
}

/*---------------------------------------------------------*\
||| Helpers                                                ||
\*---------------------------------------------------------*/

static bool ApplyScene(ipc::EngineService* svc, const json& j,
                       std::string& err)
{
    auto doc = std::make_shared<studio::SceneDocument>();
    std::vector<std::string> errs;
    if(!studio::FromJson(j, *doc, &errs))
    {
        err = errs.empty() ? "scene parse failed" : errs.front();
        return false;
    }
    return svc->ApplySceneDocument(doc, err);
}

static void PumpFor(EngineHost& host, int ms)
{
    const auto end = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(ms);
    while(std::chrono::steady_clock::now() < end)
    {
        host.PollOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

template<class Pred>
static bool WaitFor(EngineHost& host, Pred pred, int timeout_ms)
{
    const auto end = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(timeout_ms);
    while(std::chrono::steady_clock::now() < end)
    {
        host.PollOnce();
        if(pred())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    host.PollOnce();
    return pred();
}

static const DeviceSnapshot* FindByName(
    const std::vector<DeviceSnapshot>& snaps,
    const char* needle)
{
    for(const DeviceSnapshot& s : snaps)
    {
        if(s.name.find(needle) != std::string::npos)
        {
            return &s;
        }
    }
    return nullptr;
}

/* Current positional dev index of a controller (the SDK
   wire's dev_id is positional — unplug/replug shifts it). */
static int IndexOf(const std::vector<DeviceSnapshot>& snaps,
                   const char* needle)
{
    for(size_t i = 0; i < snaps.size(); ++i)
    {
        if(snaps[i].name.find(needle) != std::string::npos)
        {
            return (int)i;
        }
    }
    return -1;
}

/*---------------------------------------------------------*\
||| Real named-pipe client (the UI session surface)        ||
\*---------------------------------------------------------*/

struct PipeClient
{
    std::unique_ptr<ipc::IIpcChannel> ch;

    bool Connect(const std::string& pipe)
    {
        std::string err;
        ch = ipc::ConnectNamedPipeChannel(pipe, 3000, err);
        if(!ch)
        {
            fprintf(stderr, "pipe connect to %s failed: %s\n",
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
        return ch->SendAll(bytes.data(), bytes.size(), 3000, err)
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

    bool Hello()
    {
        if(!Send(ipc::MsgType::Hello, 0, json::object({
                { "client_name", "lifecycle-ui" },
                { "min_version", 1 },
                { "max_version", 1 } })))
        {
            return false;
        }
        ipc::Message m;
        return Recv(m, 3000) && m.type == ipc::MsgType::Welcome;
    }

    bool GetState(json& out)
    {
        if(!Send(ipc::MsgType::GetState, 7, json::object()))
        {
            return false;
        }
        ipc::Message m;
        if(!Recv(m, 3000) || m.type != ipc::MsgType::State)
        {
            return false;
        }
        out = m.Body();
        return !out.is_discarded();
    }
};

/*---------------------------------------------------------*\
||| Competing-owner fixture: this very test binary copied  ||
||| to a file named "OpenRGB.exe" and spawned with         ||
||| --sleeper — a real process whose image name collides   ||
||| with the managed backend's, classified by the REAL     ||
||| toolhelp scan at engine Start().                       ||
\*---------------------------------------------------------*/

#ifdef _WIN32
struct FakeOwner
{
    HANDLE proc = nullptr;
    DWORD  pid  = 0;

    bool Spawn(const std::filesystem::path& self,
               const std::filesystem::path& dir)
    {
        std::error_code ec;
        const std::filesystem::path dst = dir / "OpenRGB.exe";
        std::filesystem::copy_file(
            self, dst,
            std::filesystem::copy_options::overwrite_existing, ec);
        if(ec)
        {
            return false;
        }
        const std::string cmd =
            "\"" + dst.string() + "\" --sleeper 180";
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::string mutable_cmd = cmd;
        if(!CreateProcessA(nullptr, mutable_cmd.data(), nullptr,
                           nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr,
                           &si, &pi))
        {
            return false;
        }
        CloseHandle(pi.hThread);
        proc = pi.hProcess;
        pid  = pi.dwProcessId;
        return true;
    }

    bool Alive() const
    {
        return proc != nullptr &&
               WaitForSingleObject(proc, 0) == WAIT_TIMEOUT;
    }
    void Kill()
    {
        if(proc != nullptr)
        {
            TerminateProcess(proc, 0);   /* the TEST may kill its
                                          own fixture process —
                                          the engine never does */
            CloseHandle(proc);
            proc = nullptr;
        }
    }
    ~FakeOwner() { Kill(); }
};
#endif

/*---------------------------------------------------------*\
||| Main                                                   ||
\*---------------------------------------------------------*/

static int TestBody(const std::filesystem::path& self_exe);
int main(int argc, char** argv)
{
#ifdef _WIN32
    /* Competing-owner fixture mode: launched as a file named
       "OpenRGB.exe"; just sleeps so the ownership scan has a
       real process to classify. */
    for(int i = 1; i < argc; ++i)
    {
        if(std::strcmp(argv[i], "--sleeper") == 0)
        {
            const int secs = i + 1 < argc ? atoi(argv[i + 1]) : 60;
            Sleep((DWORD)secs * 1000u);
            return 0;
        }
    }
    try
    {
        wchar_t self[4096];
        DWORD n = GetModuleFileNameW(nullptr, self, 4096);
        return TestBody(std::filesystem::path(
            std::wstring(self, n)));
    }
    catch(const std::exception& e)
    {
        fprintf(stderr, "UNCAUGHT EXCEPTION: %s\n", e.what());
        return 1;
    }
    catch(...)
    {
        fprintf(stderr, "UNCAUGHT UNKNOWN EXCEPTION\n");
        return 1;
    }
#else
    (void)argc; (void)argv;
    fprintf(stderr, "Windows-only suite\n");
    return 0;
#endif
}

static int TestBody(const std::filesystem::path& self_exe)
{
    std::error_code ec;
    const std::filesystem::path tmpdir =
        std::filesystem::temp_directory_path(ec) /
        "openrgb2-lifecycle-state";
    std::filesystem::create_directories(tmpdir, ec);
    std::filesystem::remove(tmpdir / "engine-state.json", ec);

    /*------------------------------------------------*/
    /* Row: competing owner — spawn BEFORE the engine   */
    /* so the REAL startup scan classifies it.          */
    /*------------------------------------------------*/
    FakeOwner competitor;
    CHECKV(competitor.Spawn(self_exe, tmpdir),
           "competing-owner fixture failed to spawn");
    const DWORD competitor_pid = competitor.pid;
    fprintf(stderr, "competing owner fixture: OpenRGB.exe pid %lu\n",
            (unsigned long)competitor_pid);

    /*------------------------------------------------*/
    /* Row: cold start, backend DOWN first              */
    /*------------------------------------------------*/
    sdk_test::FakeSdkServer server;
    server.controllers = { WirelessGroup(), Wired() };
    server.listening   = false;      /* backend not up yet  */
    sdk_test::FakeStream* stream = nullptr;

    EngineHostOptions opts;
    opts.sdk_enabled        = true;
    opts.ipc_enabled        = true;
    /* Saved for the UI-session rows — opts is moved into the
       host below. */
    const std::string cmd_pipe =
        "\\\\.\\pipe\\openrgb2-e2e-lifecycle";
    opts.command_pipe_name  = cmd_pipe;
    opts.diagnostics_pipe_name =
        "\\\\.\\pipe\\openrgb2-e2e-lifecycle-diag";
    opts.power_monitor      = false; /* events are injected  */
    opts.reactive_inputs    = false;
    opts.scan_owners        = true;
    opts.single_instance    = true;
    opts.instance_name      = "Local\\openrgb2-engine-lifecycle-test";
    opts.state_path         =
        (tmpdir / "engine-state.json").generic_string();
    opts.backend_config_dir = "";
    /* An unused port keeps the ownership scan's listener
       probe honest on rigs where a REAL backend is already
       serving 127.0.0.1:6744 — the fake stream injects the
       transport, so the port only feeds the scan/log text. */
    opts.sdk_port           = 64998;
    /* Fast exhaustion + fast re-probe so the backoff-latch
       row runs in milliseconds, not minutes. */
    opts.sdk_backoff.initial_ms   = 50;
    opts.sdk_backoff.max_ms       = 500;
    opts.sdk_backoff.max_attempts = 4;
    opts.sdk_reprobe              = std::chrono::milliseconds(300);
    opts.sdk_topology_poll        = std::chrono::milliseconds(300);
    opts.transport_factory =
        [&server, &stream]() -> std::unique_ptr<sdk::IStreamTransport>
        {
            auto s = std::make_unique<sdk_test::FakeStream>(server);
            stream = s.get();
            return s;
        };
    std::mutex              log_mutex;
    std::vector<std::string> log_lines;
    opts.log = [&log_lines, &log_mutex](const std::string& l)
    {
        std::lock_guard<std::mutex> g(log_mutex);
        log_lines.push_back(l);
    };

    const auto t_start = std::chrono::steady_clock::now();
    EngineHost host(std::move(opts));
    std::string err;
    CHECKV(host.Start(err), "Start: %s", err.c_str());
    sdk::OpenRgbClient*   client   = host.GetSdk();
    DeliveryPipeline*     pipeline = host.GetPipeline();
    PlaybackController*   playback = host.GetPlayback();
    ipc::EngineService*   service  = host.GetService();
    PowerLifecycle*       lifecycle = host.GetLifecycle();
    CHECK(client && pipeline && playback && service && lifecycle);

    /*---- competing-owner scan (row: existing competing owner)*/
    CHECK(host.Ownership().scanned);
    CHECK(!host.Ownership().backend_attached);
    {
        bool reported = false, conflicting = false;
        for(const OwnerFinding& f : host.Ownership().findings)
        {
            if(f.pid == competitor_pid)
            {
                reported    = true;
                conflicting = f.conflict;
                fprintf(stderr,
                        "scan classified fixture: owner='%s' "
                        "conflict=%d detail='%s'\n",
                        f.owner.c_str(), (int)f.conflict,
                        f.detail.c_str());
            }
        }
        CHECKV(reported,
               "competing OpenRGB.exe not reported by the scan");
        CHECKV(conflicting,
               "competing OpenRGB.exe not flagged as a conflict");
        /* Report-only contract: the competitor must be alive —
           the engine may never kill it. */
        CHECKV(competitor.Alive(),
               "competing owner was killed by engine startup");
    }

    /* Backend absent: the engine stays honest — never claims
       attached, keeps retrying on the bounded schedule. */
    PumpFor(host, 1200);
    CHECK(client->State() != ConnectionState::Connected);
    CHECK(host.Started());
    {
        bool retry_seen = false;
        std::lock_guard<std::mutex> g(log_mutex);
        for(const std::string& l : log_lines)
        {
            if(l.find("no backend detected") != std::string::npos)
            {
                retry_seen = true;
            }
        }
        CHECKV(retry_seen,
               "absent backend was not reported honestly");
    }

    /* The backend arrives LATE: the bounded reconnect (or the
       re-probe) must attach without a restart. */
    server.listening = true;
    const auto t_listen = std::chrono::steady_clock::now();
    CHECK(WaitFor(host, [&]{
        return client->State() == ConnectionState::Connected;
    }, 15000));
    const auto t_attach = std::chrono::steady_clock::now();
    CHECK(client->Snapshots().size() == 2);
    CHECK(server.CountEvents(sdk::kPktSetClientName) >= 1);

    /* Startup scene -> bindings resolve -> frames on the wire:
       the cold-start row's attach/enumerate/scene-restore. */
    CHECKV(ApplyScene(service, LiveSceneDoc(), err),
           "apply live scene: %s", err.c_str());
    CHECK(WaitFor(host, [&]{
        return server.CountEvents(sdk::kPktUpdateLEDs, 0) >= 1;
    }, 10000));
    const auto t_first_frame = std::chrono::steady_clock::now();
    CHECK(host.FramesProduced() > 0);
    {
        const double ms_attach =
            std::chrono::duration<double, std::milli>(
                t_attach - t_listen).count();
        const double ms_total =
            std::chrono::duration<double, std::milli>(
                t_first_frame - t_start).count();
        fprintf(stderr,
                "cold start: backend-late attach %.0f ms; "
                "start->first UPDATELEDS %.0f ms "
                "(includes %.0f ms of injected backend outage)\n",
                ms_attach, ms_total,
                std::chrono::duration<double, std::milli>(
                    t_listen - t_start).count());
    }

    /*------------------------------------------------*/
    /* Row: UI crash — a real pipe session severed      */
    /*------------------------------------------------*/
    {
        PipeClient ui1;
        CHECKV(ui1.Connect(cmd_pipe),
               "UI session 1 could not connect");
        CHECKV(ui1.Hello(), "UI session 1 HELLO failed");
        json st;
        CHECKV(ui1.GetState(st), "UI session 1 STATE failed");

        /* Crash: the channel dies with no goodbye — the
           session thread sees the pipe break. */
        ui1.ch.reset();
        const uint64_t frames_at_crash = host.FramesProduced();
        const uint64_t wire_at_crash =
            server.CountEvents(sdk::kPktUpdateLEDs);
        PumpFor(host, 1000);

        /* The engine is undisturbed: frames keep flowing and a
           restarted UI rejoins on a fresh session. */
        CHECKV(host.FramesProduced() > frames_at_crash,
               "engine stopped producing after UI crash");
        CHECKV(server.CountEvents(sdk::kPktUpdateLEDs) >
               wire_at_crash,
               "output stopped after UI crash");
        PipeClient ui2;
        CHECKV(ui2.Connect(cmd_pipe),
               "restarted UI could not connect");
        CHECKV(ui2.Hello(), "restarted UI HELLO failed");
        CHECKV(ui2.GetState(st),
               "restarted UI STATE failed — no honest rejoin");
    }

    /*------------------------------------------------*/
    /* Row: unplug/replug while LIVE                    */
    /*------------------------------------------------*/
    std::vector<DeviceSnapshot> snaps = client->Snapshots();
    const DeviceSnapshot* wireless0 = FindByName(snaps, "Wireless");
    CHECK(wireless0);
    const DeviceKey wireless_key = wireless0->key;

    /* The wireless group vanishes (dongle unplug): positional
       enumeration shifts the wired controller to index 0. */
    server.RemoveController(0);
    CHECK(WaitFor(host, [&]{
        return client->Snapshots().size() == 1;
    }, 15000));
    {
        /* After detection the shifted wired controller sits at
           index 0 — and after the re-enumeration no further
           120-LED writes may reach it (a stale positional
           index would keep misrouting the group's frames
           there). Live frames for the vanished key are
           rejected honestly at the lane. */
        const int widx = IndexOf(client->Snapshots(), "X870E");
        CHECK(widx == 0);
        const uint64_t base =
            server.CountEvents(sdk::kPktUpdateLEDs, 0);
        PumpFor(host, 800);
        /* Walk only the dev-0 UPDATELEDS events recorded AFTER
           detection — earlier ones legitimately carried the
           wireless group's 120 LEDs while it owned index 0. */
        uint64_t ordinal = 0;
        bool     misroute = false;
        for(const sdk_test::ServerEvent& e :
            server.SnapshotEvents())
        {
            if(e.pkt_id != sdk::kPktUpdateLEDs || e.dev_id != 0 ||
               e.body.size() < 6)
            {
                continue;
            }
            if(ordinal++ < base)
            {
                continue;
            }
            uint16_t count = 0;
            memcpy(&count, e.body.data() + 4, 2);
            if(count == 120)
            {
                misroute = true;
            }
        }
        CHECKV(!misroute,
               "wireless frames misrouted to the shifted wired "
               "controller after unplug detection");
        RgbFrame f;
        f.controller          = wireless_key;
        f.topology_generation = wireless0->topology_generation;
        f.sequence            = 7000;
        f.colors.assign(120, RgbColor{ 9, 9, 9 });
        const SubmitResult sr = pipeline->SubmitLatest(f);
        CHECKV(sr.status == SubmitStatus::Rejected,
               "frame for unplugged group was not rejected");
    }
    /* Replug: the group returns at a NEW positional index; the
       same DeviceKey must re-resolve and output resumes. */
    server.AddController(WirelessGroup());
    CHECK(WaitFor(host, [&]{
        return client->Snapshots().size() == 2 &&
               IndexOf(client->Snapshots(), "Wireless") >= 0;
    }, 15000));
    snaps = client->Snapshots();
    const DeviceSnapshot* wireless1 = FindByName(snaps, "Wireless");
    CHECK(wireless1);
    CHECKV(wireless1->key == wireless_key,
           "wireless identity shifted across replug");
    const int new_widx = IndexOf(snaps, "Wireless");
    CHECK(new_widx >= 0);
    CHECK(WaitFor(host, [&]{
        return server.CountEvents(sdk::kPktUpdateLEDs,
                                  (uint32_t)new_widx) >= 1;
    }, 10000));

    /*------------------------------------------------*/
    /* Row: display sleep — uploaded loop freeze-once   */
    /*------------------------------------------------*/
    CHECKV(ApplyScene(service, PlaybackSceneDoc(), err),
           "apply playback scene: %s", err.c_str());
    PlaybackParams params;
    params.speed = 100;
    {
        PlaybackController::Result r =
            playback->Request(PlaybackMode::AnimationPlayback,
                              PlaybackControl::Playing, params);
        CHECKV(r.accepted, "animation request rejected: %s",
               r.reason.c_str());
    }
    CHECK(WaitFor(host, [&]{
        const PlaybackStatus s = playback->Status();
        return s.mode == PlaybackMode::AnimationPlayback &&
               s.upload == UploadState::Active;
    }, 10000));
    const int widx2 = IndexOf(client->Snapshots(), "Wireless");
    CHECK(widx2 >= 0);

    {
        const uint64_t upd0 =
            server.CountEvents(sdk::kPktUpdateLEDs, (uint32_t)widx2);
        const uint64_t scm0 =
            server.CountEvents(sdk::kPktSetCustomMode, (uint32_t)widx2);
        const uint64_t all_leds0 =
            server.CountEvents(sdk::kPktUpdateLEDs);
        const uint64_t all_mode0 =
            server.CountEvents(sdk::kPktUpdateMode);

        lifecycle->PostEvent(PowerLifecycle::Event::DisplayOff);
        PumpFor(host, 700);
        CHECK(lifecycle->OutputPaused());
        CHECK(lifecycle->PowerHold());

        /* The freeze: exactly ONE mode write (Direct) + ONE
           sampled-hold frame — then silence. The gate covers
           ALL output: no frame may reach the wire while the
           display sleeps (the 6.4 no-recurring-writes rule —
           nothing here could wake a sleeping monitor). */
        PumpFor(host, 1200);
        CHECKV(server.CountEvents(sdk::kPktSetCustomMode,
                                  (uint32_t)widx2) == scm0 + 1,
               "freeze was not a single Direct select");
        CHECKV(server.CountEvents(sdk::kPktUpdateLEDs,
                                  (uint32_t)widx2) == upd0 + 1,
               "freeze wrote != 1 hold frame");
        CHECKV(server.CountEvents(sdk::kPktUpdateLEDs) ==
               all_leds0 + 1,
               "frames leaked to the wire while display off");
        CHECKV(server.CountEvents(sdk::kPktUpdateMode) ==
               all_mode0,
               "a second loop upload ran while display off");
        /* The lane stays owned — live producers never regain
           the group mid-freeze. */
        CHECK(pipeline->IsSuppressed(wireless_key));
    }

    /* Display on: revalidation (backend reports the Direct
       the freeze left) then ONE restore upload — never a
       blind resume. */
    lifecycle->PostEvent(PowerLifecycle::Event::DisplayOn);
    {
        const uint64_t mode0 =
            server.CountEvents(sdk::kPktUpdateMode);
        CHECK(WaitFor(host, [&]{
            const PlaybackStatus s = playback->Status();
            return s.upload == UploadState::Active &&
                   s.control == PlaybackControl::Playing;
        }, 15000));
        CHECK(!lifecycle->OutputPaused());
        CHECKV(server.CountEvents(sdk::kPktUpdateMode) ==
               mode0 + 1,
               "wake restore did not re-upload exactly once");
        /* Live output resumes on the wired lane. */
        const int xidx = IndexOf(client->Snapshots(), "X870E");
        const uint64_t xw = server.CountEvents(
            sdk::kPktUpdateLEDs, (uint32_t)xidx);
        CHECK(WaitFor(host, [&]{
            return server.CountEvents(sdk::kPktUpdateLEDs,
                                      (uint32_t)xidx) > xw;
        }, 5000));
    }

    /*------------------------------------------------*/
    /* Row: suspend/resume — transport dies mid-sleep   */
    /*------------------------------------------------*/
    {
        const uint64_t leds0 =
            server.CountEvents(sdk::kPktUpdateLEDs);
        const uint64_t mode0 =
            server.CountEvents(sdk::kPktUpdateMode);
        const uint64_t scm0 =
            server.CountEvents(sdk::kPktSetCustomMode);

        lifecycle->PostEvent(PowerLifecycle::Event::Suspend);
        PumpFor(host, 500);
        CHECK(lifecycle->Suspended());
        CHECK(lifecycle->OutputPaused());

        /* While suspended: the SDK socket drops (driver/power
           loss across S3) and the restarted-side device loses
           the uploaded loop — the classic "device state
           diverged across sleep" case. */
        stream->SetPeerClosed(true);
        server.RemoveController(0);
        server.RemoveController(0);
        server.AddController(WirelessGroup());
        server.AddController(Wired());

        PumpFor(host, 1000);
        /* NO write may be attempted across the sleep boundary
           — the engine never promises the loop stops, and it
           never writes blind either. */
        CHECKV(server.CountEvents(sdk::kPktUpdateLEDs) == leds0,
               "frame written across the suspend boundary");
        CHECKV(server.CountEvents(sdk::kPktUpdateMode) == mode0,
               "mode write attempted across suspend");
        CHECKV(server.CountEvents(sdk::kPktSetCustomMode) == scm0,
               "custom-mode write attempted across suspend");

        lifecycle->PostEvent(PowerLifecycle::Event::Resume);
        /* Reconnect -> revalidate (the backend reports Direct,
           not the uploaded loop) -> restore the still-selected
           animation. */
        CHECK(WaitFor(host, [&]{
            return client->State() == ConnectionState::Connected;
        }, 15000));
        CHECK(WaitFor(host, [&]{
            const PlaybackStatus s = playback->Status();
            return s.upload == UploadState::Active &&
                   s.control == PlaybackControl::Playing;
        }, 15000));
        CHECK(!lifecycle->Suspended());
        CHECK(!lifecycle->OutputPaused());
        CHECKV(playback->Detail().empty(),
               "resume restore still reports: %s",
               playback->Detail().c_str());
        /* The restore re-uploaded — never assumed state. */
        CHECKV(server.CountEvents(sdk::kPktUpdateMode) > mode0,
               "no restore UPDATEMODE after resume");
    }

    /*------------------------------------------------*/
    /* Row: backend RESTART mid-session                 */
    /*------------------------------------------------*/
    {
        /* The backend process dies; the "new" backend comes up
           having lost the uploaded loop (fresh controller
           state) — the engine must re-attach, re-negotiate,
           and restore the still-selected playback state. */
        const uint64_t mode0 =
            server.CountEvents(sdk::kPktUpdateMode);
        server.RemoveController(0);
        server.RemoveController(0);
        server.AddController(WirelessGroup());
        server.AddController(Wired());
        stream->SetPeerClosed(true);

        CHECK(WaitFor(host, [&]{
            return client->State() == ConnectionState::Connected;
        }, 15000));
        /* Re-negotiated: a fresh handshake hit the wire. */
        CHECK(WaitFor(host, [&]{
            const PlaybackStatus s = playback->Status();
            return s.upload == UploadState::Active &&
                   s.control == PlaybackControl::Playing;
        }, 15000));
        CHECKV(server.CountEvents(sdk::kPktUpdateMode) > mode0,
               "no restore upload after backend restart");
    }

    /*------------------------------------------------*/
    /* Row: outage LONGER than the reconnect backoff    */
    /* (the Task 5.3 re-probe fix)                      */
    /*------------------------------------------------*/
    {
        server.listening = false;
        stream->SetPeerClosed(true);
        /* With max_attempts=4 and ~50-500 ms backoff the
           schedule exhausts in well under a second — the
           client latches Disconnected and the owner-loop
           re-probe re-arms it (the Task 5.3 fix). The re-arm
           happens inside the same poll that observes the
           latch, so the transient Disconnected state is not
           externally visible — the re-arm LOG LINE is the
           honest observable: it only exists post-latch. */
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(15);
        bool reprobe_seen = false;
        while(std::chrono::steady_clock::now() < deadline &&
              !reprobe_seen)
        {
            host.PollOnce();
            {
                std::lock_guard<std::mutex> g(log_mutex);
                for(const std::string& l : log_lines)
                {
                    if(l.find("re-arming the bounded SDK "
                              "reconnect") != std::string::npos)
                    {
                        reprobe_seen = true;
                    }
                }
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(5));
        }
        CHECKV(reprobe_seen,
               "no owner-loop re-probe after backoff latch — "
               "the client is permanently latched");
        /* While the backend stays down the engine keeps
           honestly reporting a non-connected state — never
           wedged, never claiming attach. */
        CHECK(client->State() != ConnectionState::Connected);
        CHECK(host.Started());
        /* The backend returns well after exhaustion: without
           the re-probe this would never re-attach. */
        server.listening = true;
        CHECK(WaitFor(host, [&]{
            return client->State() == ConnectionState::Connected;
        }, 15000));
        CHECK(WaitFor(host, [&]{
            const PlaybackStatus s = playback->Status();
            return s.upload == UploadState::Active;
        }, 15000));
    }

    /*------------------------------------------------*/
    /* Teardown — the competing owner outlived us       */
    /*------------------------------------------------*/
    CHECKV(competitor.Alive(),
           "competing owner died during the run — the engine "
           "must never kill it");
    competitor.Kill();

    host.RequestStop();
    const OrderlyShutdown::Result& sd = host.Stop();
    CHECKV(sd.all_within_bound, "stop overrun");
    CHECK(client->State() == ConnectionState::Disconnected);

    fprintf(stderr, "e2e lifecycle matrix OK\n");
    {
        std::lock_guard<std::mutex> g(log_mutex);
        for(const std::string& l : log_lines)
        {
            fprintf(stderr, "  | %s\n", l.c_str());
        }
    }
    return 0;
}
