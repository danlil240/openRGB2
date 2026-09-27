/*---------------------------------------------------------*\
|| OwnershipScan.cpp                                         ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "OwnershipScan.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>
#include <iphlpapi.h>
#endif

namespace openrgb2
{

namespace
{

std::string Lower(std::string s)
{
    for(char& c : s)
    {
        c = (char)std::tolower((unsigned char)c);
    }
    return s;
}

std::string BaseName(const std::string& path)
{
    const std::string::size_type p = path.find_last_of("/\\");
    return p == std::string::npos ? path : path.substr(p + 1);
}

bool Contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

/* Lowercase alphanumerics only: real image names arrive as
   "GigabyteControlCenter.exe", "RGBFusion.exe",
   "LogiOptionsPlus.exe", "LianLiLightControl.exe" while the
   product names carry spaces and punctuation — matching on
   the compact form catches both spellings. */
std::string Compact(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for(char c : s)
    {
        if(std::isalnum((unsigned char)c))
        {
            out += (char)std::tolower((unsigned char)c);
        }
    }
    return out;
}

#ifdef _WIN32
std::string Narrow(const std::wstring& w)
{
    if(w.empty())
    {
        return std::string();
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                      (int)w.size(), nullptr, 0,
                                      nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                        s.data(), n, nullptr, nullptr);
    return s;
}
#endif

} /* anonymous namespace */

/*---------------------------------------------------------*\
|| Platform edges                                           ||
\*---------------------------------------------------------*/

std::vector<ProcessEntry> EnumerateProcesses()
{
    std::vector<ProcessEntry> out;
#ifdef _WIN32
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if(snap == INVALID_HANDLE_VALUE)
    {
        return out;
    }
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if(Process32FirstW(snap, &pe))
    {
        do
        {
            ProcessEntry e;
            e.pid = pe.th32ProcessID;
            e.exe = Narrow(pe.szExeFile);
            out.push_back(std::move(e));
        } while(Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
#endif
    return out;
}

uint32_t FindTcpListenerPid(uint16_t port)
{
#ifdef _WIN32
    DWORD size = 0;
    if(GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET,
                           TCP_TABLE_OWNER_PID_LISTENER, 0)
       != ERROR_INSUFFICIENT_BUFFER)
    {
        return 0;
    }
    std::vector<uint8_t> buf(size);
    PMIB_TCPTABLE_OWNER_PID table =
        reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(buf.data());
    if(GetExtendedTcpTable(table, &size, FALSE, AF_INET,
                           TCP_TABLE_OWNER_PID_LISTENER, 0)
       != NO_ERROR)
    {
        return 0;
    }
    /* dwLocalPort is stored in network byte order — compare
       the host-order value after ntohs. */
    for(DWORD i = 0; i < table->dwNumEntries; ++i)
    {
        const MIB_TCPROW_OWNER_PID& row = table->table[i];
        if(row.dwState == MIB_TCP_STATE_LISTEN &&
           ntohs((u_short)row.dwLocalPort) == port)
        {
            return row.dwOwningPid;
        }
    }
#else
    (void)port;
#endif
    return 0;
}

std::string ProcessImagePath(uint32_t pid)
{
    std::string out;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                           FALSE, pid);
    if(h == nullptr)
    {
        return out;
    }
    wchar_t buf[4096];
    DWORD n = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    if(QueryFullProcessImageNameW(h, 0, buf, &n) && n > 0)
    {
        out = Narrow(std::wstring(buf, n));
    }
    CloseHandle(h);
#else
    (void)pid;
#endif
    return out;
}

/*---------------------------------------------------------*\
|| Classifier (pure)                                        ||
\*---------------------------------------------------------*/

/* PARITY CONTRACT: packaging/windows/start-openrgb2.ps1 mirrors
   these exact rules (rule tokens + owner labels). The packaging
   suite mechanically enforces it — T1 in
   tests/packaging/package_lifecycle_tests.ps1 extracts the token
   and owner sets from BOTH files and fails on any drift. Keep the
   literal spellings (`Contains(...)`, `name == "..."`,
   `f.owner = "..."`) in the shapes the test extracts. */
std::vector<OwnerFinding> ClassifyOwners(
    const std::vector<ProcessEntry>& processes,
    uint32_t self_pid,
    uint32_t backend_pid)
{
    std::vector<OwnerFinding> out;
    for(const ProcessEntry& p : processes)
    {
        if(p.pid == 0 || p.pid == self_pid)
        {
            continue;
        }
        const std::string exe     = BaseName(p.exe);
        const std::string name    = Lower(exe);
        const std::string compact = Compact(exe);

        OwnerFinding f;
        f.exe = exe;
        f.pid = p.pid;

        if(name == "openrgb.exe")
        {
            if(p.pid == backend_pid && backend_pid != 0)
            {
                f.owner    = "OpenRGB (managed backend)";
                f.conflict = false;
                f.detail   = "owns the SDK listener — the "
                             "expected sole hardware owner";
            }
            else
            {
                f.owner    = "OpenRGB (other instance)";
                f.conflict = true;
                f.detail   = "a second OpenRGB may own RGB "
                             "hardware or host the legacy "
                             "Studio/Effects producers — report "
                             "only, never killed";
            }
            out.push_back(f);
        }
        else if(Contains(compact, "lconnect") ||
                Contains(compact, "lianli"))
        {
            f.owner    = "L-Connect";
            f.conflict = true;
            f.detail   = "competing RGB owner (Lian Li)";
            out.push_back(f);
        }
        else if(name == "gcc.exe" ||
                Contains(compact, "gigabytecontrolcenter") ||
                Contains(compact, "rgbfusion") ||
                Contains(compact, "aorusengine"))
        {
            f.owner    = "Gigabyte Control Center / RGB Fusion";
            f.conflict = true;
            f.detail   = "competing RGB owner (motherboard/GPU)";
            out.push_back(f);
        }
        else if(Contains(name, "synapse") ||
                (Contains(name, "razer") &&
                 (Contains(name, "central") ||
                  Contains(name, "cortex"))))
        {
            f.owner    = "Razer Synapse";
            f.conflict = true;
            f.detail   = "competing RGB owner (Razer "
                         "peripherals)";
            out.push_back(f);
        }
        else if(Contains(name, "lghub") ||
                name == "lcore.exe" ||
                Contains(compact, "logioptions"))
        {
            f.owner    = "Logitech G Hub / Options";
            f.conflict = true;
            f.detail   = "competing RGB owner (Logitech "
                         "peripherals)";
            out.push_back(f);
        }
        else if(Contains(name, "icue") ||
                name == "corsair.service.exe")
        {
            f.owner    = "Corsair iCUE";
            f.conflict = true;
            f.detail   = "competing RGB owner (Corsair)";
            out.push_back(f);
        }
        else if(Contains(name, "signalrgb"))
        {
            f.owner    = "SignalRGB";
            f.conflict = true;
            f.detail   = "competing RGB owner";
            out.push_back(f);
        }
        else if(name == "fancontrol.exe")
        {
            /* Expected and required to stay running: owns
               cooling, never RGB. A notice so the report shows
               it was checked, not a conflict. */
            f.owner    = "FanControl";
            f.conflict = false;
            f.detail   = "cooling owner — expected; must keep "
                         "running untouched";
            out.push_back(f);
        }
    }
    return out;
}

/*---------------------------------------------------------*\
|| Managed-config sole-writer check (pure over the dir)     ||
\*---------------------------------------------------------*/

bool IsLegacySceneProducerName(const std::string& name)
{
    /* Compact form (alnum only) — catches "Desktop Lighting
       Studio.dll", "DesktopLightingStudio.dll", and
       "HardwareSync.dll" alike. */
    const std::string n = Compact(name);
    return Contains(n, "desktoplightingstudio") ||
           Contains(n, "lightingstudio") ||
           Contains(n, "hardwaresync") ||
           Contains(n, "effectsplugin");
}

namespace
{

/* Loadable plugin binaries under <plugins_dir>, recursively —
   mirrors PluginManager::ScanAndLoadPluginsFrom's extension
   filter (.dll/.so/.dylib). Paths are recorded relative to
   `base` so findings read "plugins/<sub>/<name>.dll". */
void ScanPluginFiles(const std::filesystem::path& plugins_dir,
                     const std::filesystem::path& base,
                     std::vector<std::string>& out)
{
    std::error_code ec;
    for(std::filesystem::recursive_directory_iterator
            it(plugins_dir, ec), end;
        !ec && it != end; it.increment(ec))
    {
        const std::filesystem::path& p = it->path();
        if(!it->is_regular_file(ec))
        {
            continue;
        }
        const std::string ext = Lower(p.extension().string());
        if(ext != ".dll" && ext != ".so" && ext != ".dylib")
        {
            continue;
        }
        out.push_back(p.lexically_relative(base).generic_string());
    }
}

} /* anonymous namespace */

ManagedConfigReport CheckManagedBackendConfig(
    const std::string& dir,
    const std::string& backend_exe_dir)
{
    ManagedConfigReport rep;
    std::error_code ec;

    if(dir.empty() && backend_exe_dir.empty())
    {
        rep.findings.push_back(
            "no backend config or exe dir known — sole-writer "
            "check skipped");
        return rep;
    }

    if(!dir.empty())
    {
        const std::filesystem::path root(dir);
        rep.config_dir_exists = std::filesystem::is_directory(root, ec);
        if(!rep.config_dir_exists)
        {
            rep.findings.push_back("backend config dir '" + dir +
                "' not found — cannot verify sole scene writer");
        }
        else
        {
            /* Persisted plugin enable set: OpenRGB.json ->
               "Plugins" -> "plugins"[{name, enabled}]. */
            const std::filesystem::path settings =
                root / "OpenRGB.json";
            if(std::filesystem::is_regular_file(settings, ec))
            {
                rep.settings_found = true;
                std::ifstream in(settings);
                nlohmann::json j =
                    nlohmann::json::parse(in, nullptr, false);
                if(j.is_discarded())
                {
                    rep.findings.push_back(
                        "OpenRGB.json is not parseable — plugin "
                        "enable state unverified");
                }
                else
                {
                    const auto pit = j.find("Plugins");
                    if(pit != j.end() && pit->is_object() &&
                       pit->contains("plugins") &&
                       (*pit)["plugins"].is_array())
                    {
                        for(const nlohmann::json& e :
                            (*pit)["plugins"])
                        {
                            const std::string name =
                                e.value("name", std::string());
                            if(e.value("enabled", false) &&
                               !name.empty())
                            {
                                rep.enabled_plugins.push_back(name);
                            }
                        }
                    }
                }
            }

            /* User plugin binaries under <dir>/plugins. */
            const std::filesystem::path pdir = root / "plugins";
            rep.plugins_dir_exists =
                std::filesystem::is_directory(pdir, ec);
            if(rep.plugins_dir_exists)
            {
                ScanPluginFiles(pdir, root, rep.plugin_binaries);
            }
        }
    }
    else
    {
        rep.findings.push_back(
            "no backend config dir configured — OpenRGB.json "
            "enable set unverified");
    }

    /* Windows SYSTEM plugin path: <backend exe dir>/plugins is
       loaded by PluginManager::ScanAndLoadPlugins
       unconditionally, and plugins not yet persisted in
       OpenRGB.json default to ENABLED on first run — a legacy
       producer dropped beside OpenRGB.exe is a scene writer
       before the config file ever records it. */
    if(!backend_exe_dir.empty())
    {
        const std::filesystem::path exedir(backend_exe_dir);
        if(std::filesystem::is_directory(exedir, ec))
        {
            rep.exe_dir_scanned = true;
            const std::filesystem::path spdir = exedir / "plugins";
            if(std::filesystem::is_directory(spdir, ec))
            {
                ScanPluginFiles(spdir, exedir,
                                rep.system_plugin_binaries);
            }
        }
        else
        {
            rep.findings.push_back("backend exe dir '" +
                backend_exe_dir + "' not found — system plugin "
                "scan skipped");
        }
    }

    /* Assemble honest findings. */
    for(const std::string& n : rep.enabled_plugins)
    {
        rep.findings.push_back("enabled plugin in managed config: " + n);
        if(IsLegacySceneProducerName(n))
        {
            rep.legacy_producers.push_back(n);
            rep.findings.push_back("WARNING: '" + n +
                "' is a legacy Studio/Effects scene producer — "
                "disable it: the engine is the sole scene writer "
                "in the managed configuration (plan section 4)");
        }
    }
    for(const std::string& b : rep.plugin_binaries)
    {
        rep.findings.push_back("plugin binary present: " + b);
        if(IsLegacySceneProducerName(b))
        {
            rep.legacy_producers.push_back(b);
            rep.findings.push_back("WARNING: plugin binary '" + b +
                "' matches a legacy Studio/Effects producer — "
                "remove it from the managed config dir");
        }
    }
    for(const std::string& b : rep.system_plugin_binaries)
    {
        rep.findings.push_back(
            "system plugin binary (backend exe dir): " + b);
        if(IsLegacySceneProducerName(b))
        {
            rep.legacy_producers.push_back("system:" + b);
            rep.findings.push_back("WARNING: system plugin '" + b +
                "' matches a legacy Studio/Effects producer — "
                "remove it from the backend's exe-dir plugins "
                "folder: it loads unconditionally at backend "
                "startup and defaults to enabled");
        }
    }
    /* "Verified" only when the managed config scope was
       actually inspected (a missing/unconfigured config dir
       leaves the OpenRGB.json enable set unverified — the
       findings above already carry that honestly). */
    if(rep.config_dir_exists &&
       rep.enabled_plugins.empty() &&
       rep.plugin_binaries.empty() &&
       rep.system_plugin_binaries.empty())
    {
        rep.findings.push_back(
            "sole scene writer verified: no enabled or loadable "
            "plugins in the managed backend configuration or "
            "the backend's exe-dir plugins folder");
    }
    return rep;
}

} /* namespace openrgb2 */
