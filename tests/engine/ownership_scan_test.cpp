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

static const OwnerFinding* FindPid(const std::vector<OwnerFinding>& v,
                                   uint32_t pid)
{
    for(const OwnerFinding& f : v)
    {
        if(f.pid == pid) return &f;
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
        { 401, "L-Connect 3.exe" },
        { 402, "LianLiLightControl.exe" },     /* real Lian Li  */
        { 500, "GCC.exe" },
        { 501, "GigabyteControlCenter.exe" },  /* no-space name */
        { 502, "RGBFusion.exe" },
        { 503, "AORUSEngine.exe" },
        { 600, "Razer Synapse 3.exe" },
        { 700, "lghub_agent.exe" },
        { 701, "LogiOptions.exe" },            /* Options+      */
        { 702, "LogiOptionsPlus.exe" },
        { 800, "FanControl.exe" },
        { 900, "explorer.exe" },               /* ignored       */
        { 901, "svchost.exe" },                /* ignored       */
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

    /* Real (no-space/punctuation) image names must classify the
       same — the regression coverage for space-bearing needles
       missing "GigabyteControlCenter.exe"-style names. */
    CHECK(FindPid(out, 401) && FindPid(out, 401)->owner == "L-Connect");
    CHECK(FindPid(out, 402) && FindPid(out, 402)->owner == "L-Connect" &&
          FindPid(out, 402)->conflict);
    CHECK(FindPid(out, 501) &&
          FindPid(out, 501)->owner == "Gigabyte Control Center / RGB Fusion");
    CHECK(FindPid(out, 502) &&
          FindPid(out, 502)->owner == "Gigabyte Control Center / RGB Fusion");
    CHECK(FindPid(out, 503) &&
          FindPid(out, 503)->owner == "Gigabyte Control Center / RGB Fusion");
    CHECK(FindPid(out, 701) &&
          FindPid(out, 701)->owner == "Logitech G Hub / Options");
    CHECK(FindPid(out, 702) &&
          FindPid(out, 702)->owner == "Logitech G Hub / Options");

    /* FanControl is a notice — cooling owner, never a conflict. */
    const OwnerFinding* fc = Find(out, "FanControl");
    CHECK(fc != nullptr && !fc->conflict);

    /* The engine itself and unlisted processes produce nothing. */
    for(const OwnerFinding& f : out)
    {
        CHECK(f.pid != 100);
        CHECK(f.pid != 900);
        CHECK(f.pid != 901);
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
    ManagedConfigReport clean =
        CheckManagedBackendConfig(dir.string(), "");
    CHECK(clean.config_dir_exists);
    CHECK(!clean.settings_found);
    CHECK(clean.legacy_producers.empty());

    /* A managed config with NO plugins verifies the sole
       scene writer. */
    {
        std::ofstream f(dir / "OpenRGB.json");
        f << "{ \"Detectors\": {}, \"Server\": {} }";
    }
    ManagedConfigReport r1 =
        CheckManagedBackendConfig(dir.string(), "");
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
    ManagedConfigReport r2 =
        CheckManagedBackendConfig(dir.string(), "");
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

    /* Windows SYSTEM plugin path: <backend exe dir>/plugins is
       loaded unconditionally (PluginManager::ScanAndLoadPlugins)
       and defaults new plugins to enabled — a legacy producer
       beside OpenRGB.exe must be flagged even before it is
       persisted into OpenRGB.json. */
    const std::filesystem::path exedir =
        std::filesystem::temp_directory_path(ec) /
        "openrgb2-owncfg-exedir";
    std::filesystem::remove_all(exedir, ec);
    std::filesystem::create_directories(
        exedir / "plugins" / "system", ec);
    {
        std::ofstream f(exedir / "plugins" / "system" /
                        "LightingStudioFX.dll");
        f << "fake";
        std::ofstream f2(exedir / "plugins" / "Benign.dll");
        f2 << "fake";
        std::ofstream f3(exedir / "OpenRGB.exe");
        f3 << "fake";                        /* exe dir marker */
    }
    ManagedConfigReport r4 =
        CheckManagedBackendConfig(dir.string(), exedir.string());
    CHECK(r4.exe_dir_scanned);
    CHECK(r4.system_plugin_binaries.size() == 2);
    /* settings(1) + config dll(1) + system dll(1) = 3. */
    CHECK(r4.legacy_producers.size() == 3);
    warned = false;
    for(const std::string& s : r4.findings)
    {
        if(s.find("system plugin") != std::string::npos &&
           s.find("WARNING") != std::string::npos)
        {
            warned = true;
        }
    }
    CHECK(warned);

    /* The exe-dir scan also runs WITHOUT a config dir — the
       OpenRGB.json enable set is then honestly unverified. */
    ManagedConfigReport r5 =
        CheckManagedBackendConfig("", exedir.string());
    CHECK(r5.exe_dir_scanned);
    CHECK(r5.system_plugin_binaries.size() == 2);
    CHECK(!r5.config_dir_exists);

    /* Neither scope known: the check reports the skip. */
    ManagedConfigReport r6 = CheckManagedBackendConfig("", "");
    CHECK(!r6.findings.empty() &&
          r6.legacy_producers.empty());

    /* A missing dir reports honestly. */
    ManagedConfigReport r3 =
        CheckManagedBackendConfig((dir / "nope").string(), "");
    CHECK(!r3.config_dir_exists && !r3.findings.empty());

    std::filesystem::remove_all(exedir, ec);

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
