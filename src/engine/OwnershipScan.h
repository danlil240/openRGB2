/*---------------------------------------------------------*\
|| OwnershipScan.h                                           ||
||                                                           ||
||   Competing-owner detection and the managed-config       ||
||   sole-writer check (Task 3.9; plan section 4, process   ||
||   ownership).                                             ||
||                                                           ||
||   The engine never kills or takes over another process: ||
||   every finding here is REPORT ONLY — the conflict is    ||
||   surfaced to the operator/log, and an explicit takeover ||
||   remains the operator's decision (plan section 4:      ||
||   "do not kill arbitrary matching processes").            ||
||                                                           ||
||   Two surfaces:                                           ||
||   1. Process ownership — other OpenRGB instances (each   ||
||      may own RGB hardware or host the legacy Studio/     ||
||      Effects/HardwareSync plugin producers), L-Connect,  ||
||      Gigabyte Control Center/RGB Fusion, Razer Synapse,  ||
||      Logitech G Hub, iCUE, SignalRGB. FanControl is a    ||
||      NOTICE, not a conflict: it owns cooling, not RGB,   ||
||      and must keep running.                              ||
||   2. Managed-config check — the backend's private        ||
||      configuration directory must not enable the old     ||
||      Studio/Effects/HardwareSync scene producers: the    ||
||      new engine is the sole scene writer in the managed  ||
||      configuration.                                      ||
||                                                           ||
||   The classifier and config check are pure functions     ||
||   over injected data — fully offline-testable. Process   ||
||   and TCP-listener enumeration are the only Win32        ||
||   edges.                                                 ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace openrgb2
{

/*---------------------------------------------------------*\
|| Process inventory                                        ||
\*---------------------------------------------------------*/
struct ProcessEntry
{
    uint32_t    pid = 0;
    std::string exe;          /* image file name, e.g. "OpenRGB.exe" */
};

/* Win32 toolhelp snapshot; empty on other platforms / failure.
   Read-only observation — never a handle to terminate. */
std::vector<ProcessEntry> EnumerateProcesses();

/* The PID that owns a TCP listener bound to `port` (any local
   address — the managed backend binds 127.0.0.1). 0 = nothing
   listening or lookup unavailable. Read-only (iphlpapi). */
uint32_t FindTcpListenerPid(uint16_t port);

/* A process's image path (QueryFullProcessImageName). Empty
   on failure/non-Windows/insufficient access. Read-only —
   used to locate the backend's own exe dir, whose `plugins`
   subfolder Windows loads as SYSTEM plugins unconditionally
   (PluginManager::ScanAndLoadPlugins), i.e. a second
   plugin-load path beyond <config>/plugins. */
std::string ProcessImagePath(uint32_t pid);

/*---------------------------------------------------------*\
|| Findings                                                 ||
\*---------------------------------------------------------*/
struct OwnerFinding
{
    /* conflict=true: the process can write RGB hardware —
       an actual ownership conflict for the managed setup.
       conflict=false: informational only (the managed
       backend itself, FanControl, the engine). */
    bool        conflict = false;
    std::string owner;      /* product label                */
    std::string exe;
    uint32_t    pid = 0;
    std::string detail;
};

/* Classifies a process inventory. `self_pid` is the engine's
   own process; `backend_pid` is the PID owning the managed
   backend's SDK listener when known (0 = no listener — a
   running OpenRGB.exe is then reported as a potential owner
   without the "managed backend" exemption). Pure function. */
std::vector<OwnerFinding> ClassifyOwners(
    const std::vector<ProcessEntry>& processes,
    uint32_t self_pid,
    uint32_t backend_pid);

/*---------------------------------------------------------*\
|| Managed-config sole-writer check                         ||
||                                                          ||
|| Two plugin-load paths are scanned for scene-producing    ||
|| plugins (vendor/OpenRGB PluginManager semantics):         ||
||   - <config dir>/plugins/** — user plugin binaries; the  ||
||     persisted enable set is <config dir>/OpenRGB.json -> ||
||     "Plugins"."plugins"[].enabled.                       ||
||   - <backend exe dir>/plugins/** — Windows SYSTEM        ||
||     plugins, loaded unconditionally and defaulting to    ||
||     enabled on first run: a legacy producer dropped      ||
||     beside OpenRGB.exe writes scenes even before it is   ||
||     persisted into OpenRGB.json.                         ||
|| Any enabled plugin is reported (the managed setup runs    ||
|| none); entries matching the legacy Studio/Effects/        ||
|| HardwareSync producers escalate to explicit findings —   ||
|| those would be a second scene writer.                     ||
\*---------------------------------------------------------*/
struct ManagedConfigReport
{
    bool                     config_dir_exists   = false;
    bool                     settings_found      = false;
    bool                     plugins_dir_exists  = false;
    bool                     exe_dir_scanned     = false;
    std::vector<std::string> plugin_binaries;        /* <cfg>/plugins */
    std::vector<std::string> system_plugin_binaries; /* <exe>/plugins */
    std::vector<std::string> enabled_plugins;      /* settings   */
    std::vector<std::string> legacy_producers;     /* old writers*/
    std::vector<std::string> findings;             /* human text */
};

/* `dir` is the backend's private configuration directory
   (the --localconfig working dir; may be empty). `backend_exe_dir`
   is the directory containing the backend executable — its
   `plugins` subfolder is the Windows SYSTEM plugin location
   (PluginManager::ScanAndLoadPlugins loads it unconditionally
   and defaults new plugins to enabled on first run), i.e. a
   second scene-writer path beyond <config>/plugins. May be
   empty when the backend PID/image cannot be resolved. */
ManagedConfigReport CheckManagedBackendConfig(
    const std::string& dir,
    const std::string& backend_exe_dir);

/* The name match for the legacy scene producers — pure
   function shared by both checks above. */
bool IsLegacySceneProducerName(const std::string& name);

} /* namespace openrgb2 */
