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

/*---------------------------------------------------------*\
|| Classifier (pure)                                        ||
\*---------------------------------------------------------*/

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
        const std::string exe  = BaseName(p.exe);
        const std::string name = Lower(exe);

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
        else if(Contains(name, "lconnect") ||
                Contains(name, "l-connect") ||
                Contains(name, "lian li"))
        {
            f.owner    = "L-Connect";
            f.conflict = true;
            f.detail   = "competing RGB owner (Lian Li)";
            out.push_back(f);
        }
        else if(name == "gcc.exe" ||
                Contains(name, "gigabyte control center") ||
                Contains(name, "rgb fusion") ||
                Contains(name, "aorus engine"))
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
                Contains(name, "logi options"))
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
    const std::string n = Lower(name);
    return Contains(n, "desktoplightingstudio") ||
           Contains(n, "desktop lighting studio") ||
           Contains(n, "lightingstudio") ||
           Contains(n, "hardwaresync") ||
           Contains(n, "hardware sync") ||
           Contains(n, "effectsplugin") ||
           Contains(n, "effects plugin");
}

ManagedConfigReport CheckManagedBackendConfig(const std::string& dir)
{
    ManagedConfigReport rep;
    std::error_code ec;

    if(dir.empty())
    {
        rep.findings.push_back(
            "no backend config dir configured — sole-writer "
            "check skipped");
        return rep;
    }

    const std::filesystem::path root(dir);
    rep.config_dir_exists = std::filesystem::is_directory(root, ec);
    if(!rep.config_dir_exists)
    {
        rep.findings.push_back("backend config dir '" + dir +
            "' not found — cannot verify sole scene writer");
        return rep;
    }

    /* Persisted plugin enable set: OpenRGB.json -> "Plugins" ->
       "plugins"[{name, enabled}]. */
    const std::filesystem::path settings = root / "OpenRGB.json";
    if(std::filesystem::is_regular_file(settings, ec))
    {
        rep.settings_found = true;
        std::ifstream in(settings);
        nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
        if(j.is_discarded())
        {
            rep.findings.push_back(
                "OpenRGB.json is not parseable — plugin enable "
                "state unverified");
        }
        else
        {
            const auto pit = j.find("Plugins");
            if(pit != j.end() && pit->is_object() &&
               pit->contains("plugins") &&
               (*pit)["plugins"].is_array())
            {
                for(const nlohmann::json& e : (*pit)["plugins"])
                {
                    const std::string name =
                        e.value("name", std::string());
                    if(e.value("enabled", false) && !name.empty())
                    {
                        rep.enabled_plugins.push_back(name);
                    }
                }
            }
        }
    }

    /* Loadable plugin binaries under <dir>/plugins (recursive —
       mirrors PluginManager::ScanAndLoadPluginsFrom). */
    const std::filesystem::path pdir = root / "plugins";
    rep.plugins_dir_exists = std::filesystem::is_directory(pdir, ec);
    if(rep.plugins_dir_exists)
    {
        for(std::filesystem::recursive_directory_iterator
                it(pdir, ec), end;
            !ec && it != end; it.increment(ec))
        {
            const std::filesystem::path& p = it->path();
            if(!it->is_regular_file(ec))
            {
                continue;
            }
            const std::string ext = Lower(p.extension().string());
            if(ext != ".dll" && ext != ".so")
            {
                continue;
            }
            rep.plugin_binaries.push_back(
                p.lexically_relative(root).generic_string());
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
    if(rep.enabled_plugins.empty() && rep.plugin_binaries.empty())
    {
        rep.findings.push_back(
            "sole scene writer verified: no enabled plugins in "
            "the managed backend configuration");
    }
    return rep;
}

} /* namespace openrgb2 */
