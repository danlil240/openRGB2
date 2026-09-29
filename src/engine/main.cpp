/*---------------------------------------------------------*\
|| main.cpp — openrgb2-engine                                ||
||                                                           ||
||   Standalone Qt-free lighting-engine process. The UI is ||
||   a separate process (Milestone 4); the engine keeps     ||
||   running when no UI is attached, so this entry point   ||
||   never depends on a client connection.                 ||
||                                                           ||
||   The whole composition — ownership checks, SDK attach,||
||   playback, profile, power, scene, IPC, delivery,       ||
||   ordered shutdown — lives in EngineHost (Task 3.9) so ||
||   the offline end-to-end suite drives the SAME path     ||
||   against a fake backend. main() here is only argument  ||
||   parsing, signals, and the run loop.                    ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "EngineHost.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>

namespace
{

openrgb2::EngineHost* g_host = nullptr;

void OnSignal(int)
{
    if(g_host != nullptr)
    {
        g_host->RequestStop();
    }
}

void Usage(const char* argv0)
{
    std::fprintf(stderr,
        "usage: %s [--scene <file>] [--effects-dir <dir>]\n"
        "          [--devices-dir <dir>] [--state <file>]\n"
        "          [--sdk [host:port]] [--ipc | --no-ipc]\n"
        "          [--backend-config <dir>] [--backend-exe <path>]\n"
        "          [--no-scan]\n"
        "          [--no-instance-guard]\n"
        "\n"
        "  Runs the lighting engine until SIGINT/SIGTERM. No UI is\n"
        "  required or expected. Without --scene the engine idles at\n"
        "  the 60 Hz scene clock (no frames are produced).\n"
        "  --scene accepts any persisted form the profile layer\n"
        "  resolves: an expanded SceneDocument JSON ('version'),\n"
        "  a compact workspace studio.json ('schema_version'), or a\n"
        "  legacy host-settings blob ('scene'). Workspace docs need\n"
        "  --devices-dir (default: presets/devices or\n"
        "  src/core/presets/devices relative to the working\n"
        "  directory). --effects-dir loads effect-look JSON files so\n"
        "  scenes that name a preset resolve through the registry.\n"
        "  --sdk connects submitLatest to the OpenRGB backend SDK\n"
        "  (default 127.0.0.1:6744); without it frames go to a null\n"
        "  sink. The backend is launched EXTERNALLY with --server\n"
        "  --server-host 127.0.0.1 --server-port 6744 --noautoconnect\n"
        "  --localconfig and a private configuration directory as its\n"
        "  working dir — this process attaches, it never spawns the\n"
        "  backend. --backend-config points at that private config\n"
        "  dir so startup can verify the legacy Studio/Effects/\n"
        "  HardwareSync scene producers are not enabled there\n"
        "  (sole-scene-writer check, report only). --backend-exe\n"
        "  declares the managed backend's image path so an\n"
        "  explicit QUIT {stop_backend:true} can close ONLY a\n"
        "  process whose image matches it exactly (graceful\n"
        "  WM_CLOSE, same verified-own standard as\n"
        "  scripts\\stop-openrgb2.ps1). Without it the handoff\n"
        "  never signals anything. Startup also\n"
        "  scans for competing RGB owners (other OpenRGB\n"
        "  instances, L-Connect, GCC/RGB Fusion, Synapse, G Hub,\n"
        "  iCUE, SignalRGB) and reports them -- never kills\n"
        "  (--no-scan disables). Only one engine may own the\n"
        "  managed configuration: a second instance refuses to\n"
        "  start (--no-instance-guard opts out, for tests).\n"
        "  --state sets the durable engine profile (default\n"
        "  profiles/local/engine-state.json — gitignored user state\n"
        "  holding per-scene playback selections and resolved\n"
        "  binding identities; atomic writes + last-known-good).\n"
        "  IPC is on by default: the command pipe %s and the\n"
        "  read-only diagnostics pipe %s (restricted to the\n"
        "  creating user; see docs/ipc-protocol.md). --no-ipc\n"
        "  runs headless with no pipes.\n",
        argv0, openrgb2::ipc::kCommandPipeName,
        openrgb2::ipc::kDiagnosticsPipeName);
}

} /* anonymous namespace */

int main(int argc, char** argv)
{
    openrgb2::EngineHostOptions opts;
    bool sdk_addr_set = false;
    std::string sdk_addr;

    for(int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if((arg == "--scene") && i + 1 < argc)
        {
            opts.scene_path = argv[++i];
        }
        else if((arg == "--effects-dir") && i + 1 < argc)
        {
            opts.effects_dir = argv[++i];
        }
        else if((arg == "--devices-dir") && i + 1 < argc)
        {
            opts.devices_dir = argv[++i];
        }
        else if((arg == "--state") && i + 1 < argc)
        {
            opts.state_path = argv[++i];
        }
        else if((arg == "--backend-config") && i + 1 < argc)
        {
            opts.backend_config_dir = argv[++i];
        }
        else if((arg == "--backend-exe") && i + 1 < argc)
        {
            opts.backend_exe = argv[++i];
        }
        else if(arg == "--sdk")
        {
            /* Optional host:port; defaults to the pinned backend
               endpoint (plan section 4). */
            opts.sdk_enabled = true;
            sdk_addr_set     = true;
            sdk_addr         = "127.0.0.1:6744";
            if(i + 1 < argc && argv[i + 1][0] != '-')
            {
                sdk_addr = argv[++i];
            }
        }
        else if(arg == "--ipc")
        {
            opts.ipc_enabled = true;
        }
        else if(arg == "--no-ipc")
        {
            opts.ipc_enabled = false;
        }
        else if(arg == "--no-scan")
        {
            opts.scan_owners = false;
        }
        else if(arg == "--no-instance-guard")
        {
            opts.single_instance = false;
        }
        else if(arg == "--help" || arg == "-h")
        {
            Usage(argv[0]);
            return 0;
        }
        else
        {
            std::fprintf(stderr, "openrgb2-engine: unknown argument %s\n",
                         arg.c_str());
            Usage(argv[0]);
            return 2;
        }
    }

    if(sdk_addr_set)
    {
        const std::string::size_type colon = sdk_addr.rfind(':');
        if(colon != std::string::npos)
        {
            opts.sdk_host = sdk_addr.substr(0, colon);
            opts.sdk_port = (uint16_t)std::strtoul(
                sdk_addr.substr(colon + 1).c_str(), nullptr, 10);
        }
        else
        {
            opts.sdk_host = sdk_addr;
        }
    }

    openrgb2::EngineHost host(std::move(opts));
    g_host = &host;

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
#ifdef SIGBREAK
    std::signal(SIGBREAK, OnSignal);
#endif

    std::string err;
    if(!host.Start(err))
    {
        std::fprintf(stderr, "openrgb2-engine: %s\n", err.c_str());
        return 1;
    }
    std::fprintf(stderr,
        "openrgb2-engine: running — Ctrl+C to stop\n");

    const auto started = openrgb2::Clock::now();
    host.Run();

    /* Ordered, bounded shutdown (plan section 4 "Quit All shuts
       down owned processes in order" + section 6 bounded
       shutdown). */
    const openrgb2::OrderlyShutdown::Result& sd = host.Stop();
    for(const openrgb2::OrderlyShutdown::StepResult& s : sd.steps)
    {
        if(!s.within)
        {
            std::fprintf(stderr,
                "openrgb2-engine: shutdown step '%s' overran its "
                "bound (%lld ms > %lld ms)\n",
                s.name.c_str(),
                (long long)std::chrono::duration_cast<
                    std::chrono::milliseconds>(s.elapsed).count(),
                (long long)std::chrono::duration_cast<
                    std::chrono::milliseconds>(s.bound).count());
        }
    }

    const double elapsed =
        std::chrono::duration<double>(openrgb2::Clock::now() - started)
            .count();
    std::fprintf(stderr,
        "openrgb2-engine: stopped after %.2f s, %llu scene frames "
        "(shutdown %lld ms)\n",
        elapsed, (unsigned long long)host.FramesProduced(),
        (long long)std::chrono::duration_cast<
            std::chrono::milliseconds>(sd.elapsed).count());
    return 0;
}
