/*---------------------------------------------------------*\
|| engine_ctl.cpp — openrgb2-ctl                             ||
||                                                           ||
||   Minimal command-line driver for the engine's IPC      ||
||   surface (Task 3.9 — the milestone's verification      ||
||   driver; the real UI is Milestone 4). Speaks the       ||
||   versioned protocol from docs/ipc-protocol.md over the ||
||   restricted named pipes: attach, request, print the    ||
||   reply JSON, detach. `subscribe` holds a session to    ||
||   watch push events; attach/detach NEVER restarts       ||
||   hardware state (UI independence, plan section 6).      ||
||                                                           ||
||   Also carries the smoke-test save/restore path:        ||
||   `sdk-save`/`sdk-restore` capture and re-apply each    ||
||   controller's active mode + LED colors directly over   ||
||   the backend SDK — the same save/restore discipline    ||
||   tools/bench (lianli_bench) uses.                       ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "IpcChannel.h"
#include "IpcProtocol.h"
#include "NamedPipe.h"

#include "EngineTypes.h"
#include "OpenRgbClient.h"
#include "TcpSocket.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace openrgb2;

namespace
{

void Usage(const char* argv0)
{
    std::fprintf(stderr,
        "usage: %s [--pipe <name>] <command> [args]\n"
        "\n"
        "IPC commands (command pipe, default %s):\n"
        "  ping | state | scene | snapshots | playback | stats\n"
        "  presets | quit [--stop-backend]\n"
        "  diag                       DIAG_GET on the diagnostics pipe\n"
        "  set-scene <file>           apply a SceneDocument JSON file\n"
        "  set-playback <mode> [control=playing|paused|stopped]\n"
        "                 [speed=N] [reverse] [color=#RRGGBB]\n"
        "  subscribe [seconds]        SUBSCRIBE all; print EVENTs\n"
        "  raw <type-hex> [json]      arbitrary verb (escape hatch)\n"
        "\n"
        "Backend SDK commands (--sdk-host/--sdk-port, default\n"
        "127.0.0.1:6744) — save/restore like lianli_bench:\n"
        "  sdk-list                   enumerate controllers\n"
        "  sdk-save <file>            capture active mode + LED colors\n"
        "  sdk-restore <file>         re-apply the saved state\n",
        argv0, ipc::kCommandPipeName);
}

/*---------------------------------------------------------*\
|| IPC client plumbing                                      ||
\*---------------------------------------------------------*/
class IpcClient
{
public:
    bool Connect(const std::string& pipe, uint32_t timeout_ms,
                 std::string& err)
    {
        channel_ = ipc::ConnectNamedPipeChannel(pipe, timeout_ms, err);
        if(!channel_)
        {
            return false;
        }
        reader_ = std::make_unique<ipc::MessageReader>(*channel_);
        const std::vector<uint8_t> hello = ipc::EncodeMessage(
            ipc::MsgType::Hello, 0,
            nlohmann::json::object({
                { "client_name", "openrgb2-ctl" },
                { "min_version", 1 },
                { "max_version", 1 },
            }));
        if(hello.empty() ||
           channel_->SendAll(hello.data(), hello.size(), 5000, err)
               != ipc::IpcIoResult::Ok)
        {
            return false;
        }
        ipc::Message msg;
        std::string detail;
        if(reader_->Read(msg, 5000, detail) != ipc::ReadResult::Ok ||
           msg.type != ipc::MsgType::Welcome)
        {
            err = "handshake failed: " + detail +
                  " (reply type 0x" +
                  ([&]{ char b[8]; std::snprintf(b, sizeof b, "%04x",
                       (unsigned)msg.type); return std::string(b); })() +
                  ")";
            return false;
        }
        return true;
    }

    /* Sends one request; skips EVENT pushes until the matching
       reply arrives. */
    bool Request(ipc::MsgType type, const nlohmann::json& body,
                 ipc::Message& reply, std::string& err,
                 uint32_t deadline_ms = 5000)
    {
        const uint32_t id = ++req_id_;
        const std::vector<uint8_t> wire =
            ipc::EncodeMessage(type, id, body);
        if(wire.empty())
        {
            err = "encode failed (body over 1 MiB bound)";
            return false;
        }
        if(channel_->SendAll(wire.data(), wire.size(), deadline_ms,
                             err) != ipc::IpcIoResult::Ok)
        {
            return false;
        }
        for(;;)
        {
            std::string detail;
            const ipc::ReadResult rr =
                reader_->Read(reply, deadline_ms, detail);
            if(rr != ipc::ReadResult::Ok)
            {
                err = "read failed: " + detail;
                return false;
            }
            if(reply.request_id == id)
            {
                return true;
            }
            /* An EVENT push between request and reply — surface
               it and keep waiting for our reply. */
            if(reply.type == ipc::MsgType::Event)
            {
                std::printf("EVENT %s\n",
                            reply.Body().dump().c_str());
            }
        }
    }

    /* Reads the next message (event pump for subscribe). */
    ipc::ReadResult Read(ipc::Message& msg, uint32_t deadline_ms,
                         std::string& detail)
    {
        return reader_->Read(msg, deadline_ms, detail);
    }

private:
    std::unique_ptr<ipc::IIpcChannel>    channel_;
    std::unique_ptr<ipc::MessageReader>  reader_;
    uint32_t                             req_id_ = 0;
};

int PrintReply(const ipc::Message& reply)
{
    if(reply.type == ipc::MsgType::Error)
    {
        const nlohmann::json b = reply.Body();
        std::fprintf(stderr, "ERROR %s: %s\n",
                     b.value("error", std::string("?")).c_str(),
                     b.value("message", std::string()).c_str());
        return 1;
    }
    std::printf("%s\n", reply.Body().dump(2).c_str());
    return 0;
}

/*---------------------------------------------------------*\
|| SDK save/restore (lianli_bench discipline)               ||
\*---------------------------------------------------------*/
std::string HexColor(const RgbColor& c)
{
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02X%02X%02X", c.r, c.g, c.b);
    return buf;
}

bool ParseHex(const std::string& s, RgbColor& out)
{
    unsigned r = 0, g = 0, b = 0;
    if(s.size() == 7 && s[0] == '#' &&
       std::sscanf(s.c_str() + 1, "%02x%02x%02x", &r, &g, &b) == 3)
    {
        out.r = (uint8_t)r; out.g = (uint8_t)g; out.b = (uint8_t)b;
        return true;
    }
    return false;
}

std::string HexPacked(uint32_t c)   /* 0x00BBGGRR -> #RRGGBB */
{
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02X%02X%02X",
                  c & 0xFFu, (c >> 8) & 0xFFu, (c >> 16) & 0xFFu);
    return buf;
}

uint32_t PackedFromHex(const std::string& s)
{
    RgbColor c;
    if(!ParseHex(s, c))
    {
        return 0;
    }
    return ((uint32_t)c.b << 16) | ((uint32_t)c.g << 8) | c.r;
}

bool SameKey(const DeviceKey& a, const nlohmann::json& k)
{
    return a.device   == k.value("device",   std::string()) &&
           a.serial   == k.value("serial",   std::string()) &&
           a.location == k.value("location", std::string());
}

int SdkList(sdk::OpenRgbClient& client)
{
    const std::vector<DeviceSnapshot> snaps = client.Snapshots();
    std::printf("%zu controllers:\n", snaps.size());
    for(const DeviceSnapshot& s : snaps)
    {
        std::printf("  %s | vendor=%s serial=%s\n"
                    "    location=%s | %u LEDs | zones:",
                    s.name.c_str(), s.vendor.c_str(),
                    s.key.serial.c_str(), s.key.location.c_str(),
                    s.LedCount());
        for(const ZoneSnapshot& z : s.zones)
        {
            std::printf(" %s(%u)", z.name.c_str(), z.led_count);
        }
        std::printf("\n    modes:");
        for(const std::string& m : s.modes)
        {
            std::printf(" %s", m.c_str());
        }
        std::string active;
        int32_t idx = -1;
        sdk::SdkMode m;
        if(client.ActiveMode(s.key, idx, &m))
        {
            active = m.name;
        }
        std::printf("\n    active: %s | connection: %s\n\n",
                    active.c_str(),
                    s.connection == ConnectionState::Connected
                        ? "connected" : "disconnected");
    }
    return 0;
}

int SdkSave(sdk::OpenRgbClient& client, const std::string& path)
{
    nlohmann::json out;
    out["tool"] = "openrgb2-ctl sdk-save";
    nlohmann::json ctrls = nlohmann::json::array();
    for(const DeviceSnapshot& s : client.Snapshots())
    {
        if(s.connection != ConnectionState::Connected ||
           s.LedCount() == 0)
        {
            continue;
        }
        nlohmann::json c;
        c["key"] = {
            { "device",   s.key.device   },
            { "serial",   s.key.serial   },
            { "location", s.key.location },
        };
        c["name"]          = s.name;
        c["vendor"]        = s.vendor;
        c["active_mode"]   = s.active_mode;
        int32_t idx = -1;
        sdk::SdkMode m;
        if(client.ActiveMode(s.key, idx, &m))
        {
            c["mode"] = {
                { "name",      m.name      },
                { "speed",     m.speed     },
                { "direction", m.direction },
            };
            nlohmann::json mc = nlohmann::json::array();
            for(uint32_t col : m.colors)
            {
                mc.push_back(HexPacked(col));
            }
            c["mode"]["colors"] = mc;
        }
        nlohmann::json leds = nlohmann::json::array();
        for(const RgbColor& col : s.colors)
        {
            leds.push_back(HexColor(col));
        }
        c["led_colors"] = leds;
        ctrls.push_back(c);
    }
    out["controllers"] = ctrls;
    std::ofstream f(path);
    if(!f)
    {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return 1;
    }
    f << out.dump(2) << "\n";
    std::printf("saved %zu controller state(s) to %s\n",
                ctrls.size(), path.c_str());
    return 0;
}

int SdkRestore(sdk::OpenRgbClient& client, const std::string& path)
{
    std::ifstream f(path);
    if(!f)
    {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        return 1;
    }
    const nlohmann::json doc = nlohmann::json::parse(f, nullptr, false);
    if(doc.is_discarded() || !doc.contains("controllers"))
    {
        std::fprintf(stderr, "%s is not an sdk-save file\n",
                     path.c_str());
        return 1;
    }
    int restored = 0;
    int failed   = 0;
    const std::vector<DeviceSnapshot> snaps = client.Snapshots();
    for(const nlohmann::json& c : doc["controllers"])
    {
        const DeviceSnapshot* snap = nullptr;
        for(const DeviceSnapshot& s : snaps)
        {
            if(SameKey(s.key, c.value("key", nlohmann::json())))
            {
                snap = &s;
                break;
            }
        }
        if(snap == nullptr)
        {
            std::fprintf(stderr, "restore: %s not enumerated — "
                         "skipped (missing-device profiles are "
                         "preserved, never reassigned)\n",
                         c.value("name", std::string("?")).c_str());
            ++failed;
            continue;
        }
        /* Re-apply the saved mode first, then colors — the same
           ordering lianli_bench restores with. "Direct" maps to
           SETCUSTOMMODE (the live-control mode). */
        const std::string mode_name =
            c.value("mode", nlohmann::json::object())
                .value("name", std::string());
        bool ok = true;
        if(!mode_name.empty())
        {
            if(mode_name == "Direct" || mode_name == "Custom")
            {
                ok = client.SetCustomMode(snap->key);
            }
            else
            {
                sdk::ModePatch patch;
                const nlohmann::json mj =
                    c.value("mode", nlohmann::json::object());
                if(mj.contains("speed"))
                {
                    patch.has_speed = true;
                    patch.speed = mj.value("speed", 0u);
                }
                if(mj.contains("direction"))
                {
                    patch.has_direction = true;
                    patch.direction = mj.value("direction", 0u);
                }
                if(mj.contains("colors") && mj["colors"].is_array())
                {
                    patch.has_colors = true;
                    for(const nlohmann::json& col : mj["colors"])
                    {
                        patch.colors.push_back(
                            PackedFromHex(col.get<std::string>()));
                    }
                }
                ok = client.SelectMode(snap->key, mode_name, patch);
            }
            if(!ok)
            {
                std::fprintf(stderr, "restore: %s mode '%s' "
                             "failed: %s\n", snap->name.c_str(),
                             mode_name.c_str(),
                             client.LastError().c_str());
            }
        }
        if(ok && c.contains("led_colors") &&
           c["led_colors"].is_array())
        {
            RgbFrame frame;
            frame.controller          = snap->key;
            frame.topology_generation = snap->topology_generation;
            frame.sequence            = 1;
            frame.sample_time         = Clock::now();
            frame.synthetic           = true;
            for(const nlohmann::json& col : c["led_colors"])
            {
                RgbColor cc;
                if(ParseHex(col.get<std::string>(), cc))
                {
                    frame.colors.push_back(cc);
                }
            }
            if(frame.colors.size() == snap->LedCount())
            {
                const SubmitResult r = client.SubmitLatest(frame);
                ok = r.status != SubmitStatus::Rejected;
                if(!ok)
                {
                    std::fprintf(stderr, "restore: %s color write "
                                 "rejected: %s\n", snap->name.c_str(),
                                 r.reason.c_str());
                }
            }
            else
            {
                std::fprintf(stderr, "restore: %s saved LED count "
                             "%zu != enumerated %u — colors skipped\n",
                             snap->name.c_str(), frame.colors.size(),
                             snap->LedCount());
            }
        }
        ok ? ++restored : ++failed;
    }
    std::printf("restored %d controller(s), %d skipped/failed\n",
                restored, failed);
    return failed == 0 ? 0 : 1;
}

} /* anonymous namespace */

int main(int argc, char** argv)
{
    std::string pipe      = ipc::kCommandPipeName;
    std::string sdk_host  = "127.0.0.1";
    uint16_t    sdk_port  = 6744;
    std::vector<std::string> args;
    for(int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if(a == "--pipe" && i + 1 < argc)
        {
            pipe = argv[++i];
        }
        else if(a == "--sdk-host" && i + 1 < argc)
        {
            sdk_host = argv[++i];
        }
        else if(a == "--sdk-port" && i + 1 < argc)
        {
            sdk_port = (uint16_t)std::strtoul(argv[++i], nullptr, 10);
        }
        else if(a == "--help" || a == "-h")
        {
            Usage(argv[0]);
            return 0;
        }
        else
        {
            args.push_back(a);
        }
    }
    if(args.empty())
    {
        Usage(argv[0]);
        return 2;
    }

    const std::string cmd = args[0];

    /*---- Direct backend SDK commands ------------------------*/
    if(cmd == "sdk-list" || cmd == "sdk-save" || cmd == "sdk-restore")
    {
        if((cmd == "sdk-save" || cmd == "sdk-restore") &&
           args.size() < 2)
        {
            std::fprintf(stderr, "%s needs a file path\n",
                         cmd.c_str());
            return 2;
        }
        sdk::OpenRgbClientOptions opts;
        opts.host        = sdk_host;
        opts.port        = sdk_port;
        opts.client_name = "openrgb2-ctl";
        sdk::OpenRgbClient client(opts,
            std::make_unique<sdk::TcpSocket>());
        if(!client.Connect())
        {
            std::fprintf(stderr, "SDK connect %s:%u failed: %s\n",
                         sdk_host.c_str(), (unsigned)sdk_port,
                         client.LastError().c_str());
            return 1;
        }
        int rc = 0;
        if(cmd == "sdk-list")
        {
            rc = SdkList(client);
        }
        else if(cmd == "sdk-save")
        {
            rc = SdkSave(client, args[1]);
        }
        else
        {
            rc = SdkRestore(client, args[1]);
        }
        client.Disconnect();
        return rc;
    }

    /*---- IPC commands ----------------------------------------*/
    ipc::MsgType type;
    nlohmann::json body = nlohmann::json::object();
    bool wants_diag_pipe = false;
    bool subscribe_mode  = false;
    uint32_t subscribe_s = 5;

    if(cmd == "ping")           { type = ipc::MsgType::Ping; }
    else if(cmd == "state")     { type = ipc::MsgType::GetState; }
    else if(cmd == "scene")     { type = ipc::MsgType::GetScene; }
    else if(cmd == "snapshots") { type = ipc::MsgType::GetSnapshots; }
    else if(cmd == "playback")  { type = ipc::MsgType::GetPlayback; }
    else if(cmd == "stats")     { type = ipc::MsgType::GetDeliveryStats; }
    else if(cmd == "presets")   { type = ipc::MsgType::GetPresets; }
    else if(cmd == "quit")
    {
        type = ipc::MsgType::Quit;
        for(size_t i = 1; i < args.size(); ++i)
        {
            if(args[i] == "--stop-backend")
            {
                /* Quit-all semantics: ask the engine to also
                   close the VERIFIED managed backend (graceful
                   WM_CLOSE after the transport drains). */
                body["stop_backend"] = true;
            }
        }
    }
    else if(cmd == "diag")
    {
        type            = ipc::MsgType::DiagGet;
        wants_diag_pipe = true;
    }
    else if(cmd == "set-scene")
    {
        if(args.size() < 2)
        {
            std::fprintf(stderr, "set-scene needs a file path\n");
            return 2;
        }
        std::ifstream f(args[1]);
        const nlohmann::json doc =
            nlohmann::json::parse(f, nullptr, false);
        if(doc.is_discarded())
        {
            std::fprintf(stderr, "%s is not valid JSON\n",
                         args[1].c_str());
            return 1;
        }
        type = ipc::MsgType::SetScene;
        body = { { "document", doc } };
    }
    else if(cmd == "set-playback")
    {
        if(args.size() < 2)
        {
            std::fprintf(stderr,
                "set-playback needs live_effects|animation_playback\n");
            return 2;
        }
        type = ipc::MsgType::SetPlaybackMode;
        body["mode"] = args[1];
        nlohmann::json params = nlohmann::json::object();
        for(size_t i = 2; i < args.size(); ++i)
        {
            const std::string& a = args[i];
            if(a.rfind("control=", 0) == 0)
            {
                body["control"] = a.substr(8);
            }
            else if(a.rfind("speed=", 0) == 0)
            {
                params["speed"] = std::strtol(a.c_str() + 6,
                                              nullptr, 10);
            }
            else if(a == "reverse")
            {
                params["reverse"] = true;
            }
            else if(a.rfind("color=", 0) == 0)
            {
                params["color"] = a.substr(6);
            }
            else
            {
                std::fprintf(stderr, "unknown set-playback arg %s\n",
                             a.c_str());
                return 2;
            }
        }
        if(!params.empty())
        {
            body["params"] = params;
        }
    }
    else if(cmd == "subscribe")
    {
        subscribe_mode = true;
        if(args.size() >= 2)
        {
            subscribe_s =
                (uint32_t)std::strtoul(args[1].c_str(), nullptr, 10);
            if(subscribe_s == 0) subscribe_s = 5;
        }
        type = ipc::MsgType::Subscribe;
        body = { { "events", "all" } };
    }
    else if(cmd == "raw")
    {
        if(args.size() < 2)
        {
            std::fprintf(stderr, "raw needs a type code (hex)\n");
            return 2;
        }
        type = (ipc::MsgType)std::strtoul(args[1].c_str(),
                                         nullptr, 16);
        if(args.size() >= 3)
        {
            body = nlohmann::json::parse(args[2], nullptr, false);
            if(body.is_discarded())
            {
                std::fprintf(stderr, "bad JSON body\n");
                return 1;
            }
        }
    }
    else
    {
        std::fprintf(stderr, "unknown command '%s'\n", cmd.c_str());
        Usage(argv[0]);
        return 2;
    }

    const std::string target_pipe =
        wants_diag_pipe ? ipc::kDiagnosticsPipeName : pipe;

    IpcClient client;
    std::string err;
    if(!client.Connect(target_pipe, 3000, err))
    {
        std::fprintf(stderr, "connect %s failed: %s\n",
                     target_pipe.c_str(), err.c_str());
        return 1;
    }

    ipc::Message reply;
    if(!client.Request(type, body, reply, err))
    {
        std::fprintf(stderr, "request failed: %s\n", err.c_str());
        return 1;
    }
    int rc = PrintReply(reply);
    if(rc != 0)
    {
        return rc;
    }

    if(subscribe_mode)
    {
        /* Hold the session and print pushes — the attach/detach
           reconnection check for Task 3.9. */
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(subscribe_s);
        std::fprintf(stderr,
            "--- subscribed %u s; events below ---\n", subscribe_s);
        while(std::chrono::steady_clock::now() < deadline)
        {
            ipc::Message ev;
            std::string detail;
            const uint32_t left = (uint32_t)std::max<int64_t>(1,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now())
                    .count());
            const ipc::ReadResult rr = client.Read(ev, left, detail);
            if(rr == ipc::ReadResult::Ok)
            {
                std::printf("EVENT %s\n", ev.Body().dump().c_str());
                std::fflush(stdout);
            }
            else if(rr == ipc::ReadResult::Timeout ||
                    rr == ipc::ReadResult::TooLarge)
            {
                continue;
            }
            else
            {
                std::fprintf(stderr, "event read ended: %s\n",
                             detail.c_str());
                break;
            }
        }
    }
    return rc;
}
