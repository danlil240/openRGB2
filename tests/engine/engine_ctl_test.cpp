// SPDX-License-Identifier: GPL-2.0-or-later
//
// openrgb2-ctl session tests (audit I-10): the CLI's CtlSession
// and its composite workspace flows against the REAL
// EngineService + WorkspaceService + IpcServer, served over REAL
// named pipes (command + diagnostics listeners on per-process
// names):
//   - `diag` follows --pipe: DiagPipeFor derives "<pipe>-diag"
//     and that pipe answers DIAG_GET on the diagnostics surface
//   - a job verb waits for its terminal status in the SAME
//     session (a per-request connection used to cancel it)
//   - upload -> commit with the staged token in one session
//   - export -> download -> length + SHA-256 verified bundle
//
// CHECK-macro main like the other engine suites.

#include "CtlSession.h"

#include "EngineService.h"
#include "IpcServer.h"
#include "NamedPipe.h"
#include "WorkspaceTransfer.h"

#include "Engine.h"
#include "WorkspaceService.h"

#include "presets/PresetRegistry.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>

using namespace openrgb2;
using namespace openrgb2::ipc;
using nlohmann::json;
namespace fs = std::filesystem;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

static fs::path g_tmp;

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

static json WorkspaceDoc(const std::string& name)
{
    return json::object({
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
}

/* The real engine-side services (as in ipc_workspace_test). */
struct Rig
{
    openrgb2::Engine                  engine;
    studio::PresetRegistry            devices;
    WorkspaceService::Deps            wdeps;
    EngineService::Deps               edeps;
    EngineServiceOptions              eopts;
    IpcServerOptions                  sopts;
    std::unique_ptr<WorkspaceService> ws;
    std::unique_ptr<EngineService>    svc;
    std::unique_ptr<IpcServer>        server;

    explicit Rig(const fs::path& root)
    {
        std::vector<std::string> errs;
        devices.LoadDirectory((g_tmp / "types").string(), &errs);
        CHECK(errs.empty());
        wdeps.devices = devices;
        wdeps.apply_scene =
            [this](std::shared_ptr<const WorkspaceCandidate> cand,
                   std::string& err) -> bool
            {
                return svc && cand &&
                       svc->ApplyWorkspaceCandidate(*cand, err);
            };
        ws = std::make_unique<WorkspaceService>(root.string(), wdeps);
        edeps.workspace = ws.get();
        svc    = std::make_unique<EngineService>(engine, edeps, eopts);
        server = std::make_unique<IpcServer>(*svc, sopts);
        svc->SetEventSink(
            [this](const IpcEvent& ev) { server->Emit(ev); });
    }
};

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    g_tmp = fs::temp_directory_path() /
            ("o2-ctl-" + std::to_string(GetCurrentProcessId()) + "-" +
             std::to_string((uint64_t)std::chrono::steady_clock::now()
                                .time_since_epoch().count()));
    fs::create_directories(g_tmp);
    WriteTestType(g_tmp / "types", 8);

    /*---- DiagPipeFor -----------------------------------------*/
    CHECK(ctl::DiagPipeFor(kCommandPipeName) == kDiagnosticsPipeName);
    CHECK(ctl::DiagPipeFor("\\\\.\\pipe\\custom") ==
          "\\\\.\\pipe\\custom-diag");

    {
        Rig rig(g_tmp / "root");
        const std::string cmd_pipe =
            "\\\\.\\pipe\\openrgb2-ctltest-" +
            std::to_string(GetCurrentProcessId());
        const std::string diag_pipe = ctl::DiagPipeFor(cmd_pipe);
        /* Never touch a real engine's pipes: a derivation that
           fell back to the default name would reach the
           installed engine's diagnostics channel. */
        const bool own_diag = diag_pipe != kDiagnosticsPipeName;
        CHECK(own_diag);
        NamedPipeListener cmd(*rig.server, ChannelKind::Command,
                              cmd_pipe);
        NamedPipeListener diag(*rig.server, ChannelKind::Diagnostics,
                               own_diag ? diag_pipe
                                        : cmd_pipe + "-unused");
        std::string err;
        CHECK(cmd.Start(err));
        CHECK(diag.Start(err));

        /*---- diag follows --pipe -------------------------------*/
        if(own_diag)
        {
            ctl::CtlSession s;
            CHECK(s.Connect(ctl::DiagPipeFor(cmd_pipe), 3000, err));
            CHECK(s.Welcome().value("channel", "") == "diagnostics");
            Message m;
            CHECK(s.Request(MsgType::DiagGet, json::object(), m, err));
            CHECK(m.type != MsgType::Error);
        }

        ctl::CtlSession s;
        CHECK(s.Connect(cmd_pipe, 3000, err));

        /*---- job verb waits in-session -----------------------*/
        {
            json out;
            CHECK(ctl::WorkspaceRun(s, json::object({
                      { "verb",              "validate"            },
                      { "id",                "desk"                },
                      { "base_revision",     0                     },
                      { "resource_revision", 0                     },
                      { "document",          WorkspaceDoc("probe") },
                  }), 20000, out, err));
            /* the terminal job record, not the {accepted,job_id}
               acknowledgement */
            CHECK(out.value("found", false));
            CHECK(out.value("verb", "") == "validate");
            CHECK(out.value("state", "") == "succeeded");
        }

        /*---- upload -> commit with the staged token ----------*/
        {
            std::string staged;
            CHECK(ctl::UploadBytes(s, "document",
                                   WorkspaceDoc("desk").dump(), staged,
                                   err));
            CHECK(!staged.empty());
            json out;
            CHECK(ctl::WorkspaceRun(s, json::object({
                      { "verb",              "commit" },
                      { "id",                "desk"   },
                      { "base_revision",     0        },
                      { "resource_revision", 0        },
                      { "staged",            staged   },
                  }), 20000, out, err));
            CHECK(out.value("state", "") == "succeeded");
            CHECK(out.value("committed", false));
            CHECK(out.value("workspace_revision", 0ull) == 1);
        }

        /*---- export -> verified bundle -----------------------*/
        {
            std::string bundle;
            json        status;
            CHECK(ctl::ExportWorkspace(s, "desk", 20000, bundle, status,
                                       err));
            if(!err.empty())
            {
                std::printf("export: %s\n", err.c_str());
            }
            CHECK(status.value("state", "") == "succeeded");
            const json res = status.value("result", json::object());
            CHECK(!bundle.empty());
            CHECK(bundle.size() == res.value("bytes", 0ull));
            CHECK(Sha256Hex(bundle) == res.value("sha256", ""));
            const json bj = json::parse(bundle, nullptr, false);
            CHECK(bj.is_object() &&
                  bj.value("format", "") == "openrgb2-bundle-files");
            CHECK(bj.is_object() && bj.value("files", json()).is_object() &&
                  bj["files"].contains("studio.json"));
        }

        cmd.Stop();
        diag.Stop();
    }

    std::error_code ec;
    fs::remove_all(g_tmp, ec);
    std::printf("engine_ctl_test: %d checks, %d failures\n", checks,
                failures);
    return failures == 0 ? 0 : 1;
}
