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
|| The backend's private configuration directory (the       ||
|| --localconfig working directory, plan section 4) is      ||
|| scanned for scene-producing plugins:                     ||
||   - <dir>/plugins/**: *.dll / *.so are loadable plugin    ||
||     binaries (vendor/OpenRGB PluginManager semantics).    ||
||   - <dir>/OpenRGB.json -> "Plugins"."plugins"[].enabled   ||
||     is the persisted enable set.                          ||
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
    std::vector<std::string> plugin_binaries;      /* rel paths  */
    std::vector<std::string> enabled_plugins;      /* settings   */
    std::vector<std::string> legacy_producers;     /* old writers*/
    std::vector<std::string> findings;             /* human text */
};

ManagedConfigReport CheckManagedBackendConfig(const std::string& dir);

/* The name match for the legacy scene producers — pure
   function shared by both checks above. */
bool IsLegacySceneProducerName(const std::string& name);

} /* namespace openrgb2 */
