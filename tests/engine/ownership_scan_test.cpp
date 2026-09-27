// SPDX-License-Identifier: GPL-2.0-or-later
//
// Task 3.9 ownership tests — the section-4 ownership rules:
//   - competing RGB owners are classified and REPORTED,
//     never killed (the classifier is a pure function over a
//     process list — no real processes touched)
//   - the managed backend's own PID is exempted when it owns
//     the SDK listener
//   - the managed config's sole-writer check flags the legacy
//     Studio/Effects/HardwareSync producers and reports any
//     enabled plugin honestly
//   - the single-instance guard refuses a second owner of the
//     same name and releases cleanly
//   - FindTcpListenerPid resolves a real loopback listener's
//     PID (self-contained winsock listener, closed before exit)

#include "OwnershipScan.h"
#include "SingleInstance.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

using namespace openrgb2;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

static const OwnerFinding* Find(const std::vector<OwnerFinding>& v,
                                const std::string& owner)
{
    for(const OwnerFinding& f : v)
    {
        if(f.owner == owner) return &f;
    }
    return nullptr;
}

/*---------------------------------------------------------*\
|| Classifier                                               ||
\*---------------------------------------------------------*/
static void TestClassifier()
{
    const std::vector<ProcessEntry> procs = {
        { 100, "openrgb2-engine.exe" },        /* self          */
        { 200, "OpenRGB.exe" },                /* managed       */
        { 300, "OpenRGB.exe" },                /* OTHER install */
        { 400, "LConnectService.exe" },
        { 500, "GCC.exe" },
        { 600, "Razer Synapse 3.exe" },
        { 700, "lghub_agent.exe" },
        { 800, "FanControl.exe" },
        { 900, "explorer.exe" },               /* ignored       */
        { 999, "SignalRGB.exe" },
    };

    const std::vector<OwnerFinding> out =
        ClassifyOwners(procs, /*self*/ 100, /*backend*/ 200);

    CHECK(Find(out, "OpenRGB (managed backend)") != nullptr);
    CHECK(Find(out, "OpenRGB (managed backend)") &&
          !Find(out, "OpenRGB (managed backend)")->conflict);

    const OwnerFinding* other = Find(out, "OpenRGB (other instance)");
    CHECK(other != nullptr && other->conflict && other->pid == 300);

    CHECK(Find(out, "L-Connect") && Find(out, "L-Connect")->conflict);
    CHECK(Find(out, "Gigabyte Control Center / RGB Fusion") &&
          Find(out, "Gigabyte Control Center / RGB Fusion")->conflict);
    CHECK(Find(out, "Razer Synapse") &&
          Find(out, "Razer Synapse")->conflict);
    CHECK(Find(out, "Logitech G Hub / Options") &&
          Find(out, "Logitech G Hub / Options")->conflict);
    CHECK(Find(out, "SignalRGB") &&
          Find(out, "SignalRGB")->conflict);

    /* FanControl is a notice — cooling owner, never a conflict. */
    const OwnerFinding* fc = Find(out, "FanControl");
    CHECK(fc != nullptr && !fc->conflict);

    /* The engine itself and unlisted processes produce nothing. */
    for(const OwnerFinding& f : out)
    {
        CHECK(f.pid != 100);
        CHECK(f.pid != 900);
    }

    /* With no managed backend listening (pid 0), every OpenRGB
       instance is a potential owner — none is exempted. */
    const std::vector<OwnerFinding> none =
        ClassifyOwners({ { 200, "OpenRGB.exe" } }, 100, 0);
    CHECK(none.size() == 1 && none[0].conflict &&
          none[0].owner == "OpenRGB (other instance)");
}

/*---------------------------------------------------------*\
|| Managed-config sole-writer check                         ||
\*---------------------------------------------------------*/
static void TestManagedConfig()
{
    std::error_code ec;
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path(ec) /
        "openrgb2-owncfg-test";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    /* Empty dir: no settings, nothing enabled — clean verdict. */
    ManagedConfigReport clean = CheckManagedBackendConfig(dir.string());
    CHECK(clean.config_dir_exists);
    CHECK(!clean.settings_found);
    CHECK(clean.legacy_producers.empty());

    /* A managed config with NO plugins verifies the sole
       scene writer. */
    {
        std::ofstream f(dir / "OpenRGB.json");
        f << "{ \"Detectors\": {}, \"Server\": {} }";
    }
    ManagedConfigReport r1 = CheckManagedBackendConfig(dir.string());
    CHECK(r1.settings_found);
    CHECK(r1.enabled_plugins.empty());
    CHECK(r1.legacy_producers.empty());

    /* An enabled legacy producer is flagged by name. */
    {
        std::ofstream f(dir / "OpenRGB.json");
        f << nlohmann::json::object({
            { "Plugins", {
                { "plugins", nlohmann::json::array({
                    { { "name", "DesktopLightingStudio" },
                      { "enabled", true } },
                    { { "name", "Some Other Plugin" },
                      { "enabled", false } },
                }) },
            } },
        }).dump();
    }
    /* Plugin binaries under <dir>/plugins are detected too. */
    std::filesystem::create_directories(dir / "plugins" / "dls", ec);
    {
        std::ofstream f(dir / "plugins" / "dls" /
                        "HardwareSyncProducer.dll");
        f << "fake";
        std::ofstream f2(dir / "plugins" / "readme.txt");
        f2 << "not a plugin";
    }
    ManagedConfigReport r2 = CheckManagedBackendConfig(dir.string());
    CHECK(r2.enabled_plugins.size() == 1 &&
          r2.enabled_plugins[0] == "DesktopLightingStudio");
    CHECK(r2.plugin_binaries.size() == 1);
    CHECK(r2.legacy_producers.size() == 2);   /* settings + dll */
    bool warned = false;
    for(const std::string& s : r2.findings)
    {
        if(s.find("WARNING") != std::string::npos) warned = true;
    }
    CHECK(warned);

    /* A missing dir reports honestly. */
    ManagedConfigReport r3 =
        CheckManagedBackendConfig((dir / "nope").string());
    CHECK(!r3.config_dir_exists && !r3.findings.empty());

    /* Name matcher itself. */
    CHECK(IsLegacySceneProducerName("DesktopLightingStudio"));
    CHECK(IsLegacySceneProducerName("hardwaresync.dll"));
    CHECK(!IsLegacySceneProducerName("OpenRGBSettingsPlugin"));

    std::filesystem::remove_all(dir, ec);
}

/*---------------------------------------------------------*\
|| Single-instance guard                                    ||
\*---------------------------------------------------------*/
static void TestSingleInstance()
{
    const std::string name = "Local\\openrgb2-ownership-test";
    std::string err;

    SingleInstanceGuard first(name);
    CHECK(first.Acquire(err));
    CHECK(first.Held());

    /* A second owner of the same name is refused — never
       taken over. */
    SingleInstanceGuard second(name);
    CHECK(!second.Acquire(err));
    CHECK(!err.empty());
    CHECK(!second.Held());

    /* Release frees the claim — a later guard acquires. */
    first.Release();
    CHECK(!first.Held());
    SingleInstanceGuard third(name);
    err.clear();
    CHECK(third.Acquire(err));

    /* A different name never collides. */
    SingleInstanceGuard other(name + "-other");
    err.clear();
    CHECK(other.Acquire(err));
}

/*---------------------------------------------------------*\
|| TCP listener PID lookup (real loopback socket)           ||
\*---------------------------------------------------------*/
static void TestListenerPid()
{
#ifdef _WIN32
    WSADATA wsa;
    if(WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        std::printf("SKIP: WSAStartup failed\n");
        return;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(s != INVALID_SOCKET);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;                    /* ephemeral */
    CHECK(bind(s, (sockaddr*)&addr, sizeof(addr)) == 0);
    CHECK(listen(s, 1) == 0);
    sockaddr_in bound{};
    int blen = sizeof(bound);
    CHECK(getsockname(s, (sockaddr*)&bound, &blen) == 0);
    const uint16_t port = ntohs(bound.sin_port);

    const uint32_t pid = FindTcpListenerPid(port);
    CHECK(pid == GetCurrentProcessId());

    /* An unbound port reports nothing. */
    CHECK(FindTcpListenerPid(1) == 0 ||
          FindTcpListenerPid(1) != GetCurrentProcessId());

    closesocket(s);
    WSACleanup();
#else
    std::printf("SKIP: listener PID lookup is Windows-only\n");
#endif
}

int main()
{
    TestClassifier();
    TestManagedConfig();
    TestSingleInstance();
    TestListenerPid();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
