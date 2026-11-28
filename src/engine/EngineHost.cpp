/*---------------------------------------------------------*\
|| EngineHost.cpp                                            ||
||                                                           ||
||   The end-to-end engine composition (Task 3.9). Moved    ||
||   verbatim-intent from the former main.cpp wiring — see  ||
||   EngineHost.h for the startup order and the seams.      ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "EngineHost.h"

#include "SceneLoader.h"

#include "WinReactiveInputs.h"

#include "TcpSocket.h"

#include "effects/Presets.h"
#include "presets/PresetRegistry.h"
#include "scene/SceneJson.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace openrgb2
{

namespace
{

/* Per-controller delivery budgets (plan section 4 — budgets are
   qualified per controller, never a global rate). The wireless
   group's measured ceiling is ~33.5 admitted uploads/s
   (docs/benchmarks/wireless-result.md), so it is paced at 30 ms.
   Qualification is device-aware: IsWirelessGroupController keys
   on the wireless service's own transport identity (the
   "Wireless:" location prefix), not a name heuristic. Other
   controllers are unpaced until measured — the 60 Hz scene
   clock already bounds how often producers can submit. */
Clock::duration BudgetFor(const DeviceSnapshot& snap)
{
    if(IsWirelessGroupController(snap))
    {
        return std::chrono::milliseconds(30);
    }
    /* SMBus I2C GPUs (Gigabyte RGB Fusion 2 class): the firmware
       fades toward each newly written direct color, and a new
       color arriving before the fade completes restarts it — a
       60 Hz stream of changing colors turns that into visible
       flicker. 45 ms between writes lets each fade finish (the
       playback mirror's ~47 ms cadence renders steadily; the
       unpaced ~17 ms live stream does not — verified against an
       AORUS RTX 5080 MASTER ICE with a packet-level wire trace).
       OpenRGB device_type 2 = GPU. */
    constexpr int32_t kDeviceTypeGpu = 2;

    if(snap.device_type == kDeviceTypeGpu &&
       snap.key.location.rfind("I2C:", 0) == 0)
    {
        return std::chrono::milliseconds(45);
    }
    return Clock::duration::zero();
}

void SyncLanes(DeliveryPipeline& pipeline,
               const sdk::OpenRgbClient& client)
{
    const std::vector<DeviceSnapshot> snaps = client.Snapshots();
    pipeline.SyncDevices(snaps);
    for(const DeviceSnapshot& snap : snaps)
    {
        /* Only re-apply on change — SetBudget notifies the lane
           even when the value is identical. */
        Clock::duration current;
        const Clock::duration want = BudgetFor(snap);
        if(!pipeline.Budget(snap.key, current) || current != want)
        {
            pipeline.SetBudget(snap.key, want);
        }
    }
}

/* Device-type registry for workspace resolution: the explicit
   --devices-dir, else the conventional probe paths relative to
   the working directory. An empty registry is legal — only
   compact workspace documents actually need it. */
studio::PresetRegistry BuildDeviceRegistry(
    const std::string& dir,
    const std::function<void(const std::string&)>& log)
{
    studio::PresetRegistry reg;
    std::vector<std::string> candidates;
    if(!dir.empty())
    {
        candidates.push_back(dir);
    }
    else
    {
        candidates.push_back("presets/devices");
        candidates.push_back("src/core/presets/devices");
    }
    for(const std::string& c : candidates)
    {
        std::error_code ec;
        if(dir.empty() && !std::filesystem::exists(c, ec))
        {
            continue;   /* probe candidate absent — try the next */
        }
        std::vector<std::string> errors;
        reg.LoadDirectory(c, &errors);
        for(const std::string& e : errors)
        {
            log("device types: " + e);
        }
        if(dir.empty() && reg.FileCount() == 0)
        {
            continue;
        }
        log("device-type registry loaded " +
            std::to_string(reg.FileCount()) + " types from " + c);
        return reg;
    }
    return reg;
}

/* Task 3.8 offline bindings: record the identity each scene
   binding resolved TO (serial/location/zone + LED count) into
   the engine profile. A binding then resolves to the SAME
   device after restart — a missing device keeps its binding
   and a same-named different device can never inherit it.
   Only reports true on change (the owner loop must not
   rewrite the profile every tick). */
bool RecordResolvedBindings(
    DeviceMapper& mapper,
    const std::vector<DeviceSnapshot>& snaps,
    EngineState& state)
{
    const std::shared_ptr<const studio::SceneDocument> doc =
        mapper.Scene();
    if(!doc)
    {
        return false;
    }
    bool changed = false;
    for(const studio::DeviceBinding& b : doc->bindings)
    {
        ZonePlacement zp;
        if(!mapper.BoundZone(b.id, zp))
        {
            continue;   /* unresolved/ambiguous — keep the last
                           recorded identity (missing-device
                           profile preservation). */
        }
        const auto sit = std::find_if(snaps.begin(), snaps.end(),
            [&zp](const DeviceSnapshot& s)
            { return s.key == zp.controller; });
        if(sit == snaps.end())
        {
            continue;
        }
        std::string zone_name = b.zone_name;
        if(zone_name.empty() && zp.zone_index < sit->zones.size())
        {
            zone_name = sit->zones[zp.zone_index].name;
        }
        const studio::DeviceBinding id =
            ResolvedBindingIdentity(b, *sit, zone_name,
                                    zp.led_count);
        studio::DeviceBinding& slot = state.bindings[b.id];
        if(slot.id              != id.id              ||
           slot.controller_name != id.controller_name ||
           slot.vendor          != id.vendor          ||
           slot.serial          != id.serial          ||
           slot.location        != id.location        ||
           slot.device_type     != id.device_type     ||
           slot.zone_name       != id.zone_name       ||
           slot.zone_leds       != id.zone_leds)
        {
            slot    = id;
            changed = true;
        }
    }
    return changed;
}

/*---------------------------------------------------------*\
|| Quit-all managed-backend handoff (plan section 4 — the ||
|| operator's explicit "close everything" is what delegates||
|| teardown; the engine still never kills a foreign        ||
|| process). Verification matches the standard             ||
|| stop-openrgb2.ps1 enforces: a pid is only ours when its ||
|| image path equals the declared --backend-exe EXACTLY.  ||
\*---------------------------------------------------------*/
#ifdef _WIN32

std::string LowerAscii(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string NormalizeImagePath(const std::string& p)
{
    return LowerAscii(
        std::filesystem::path(p).lexically_normal()
            .generic_string());
}

bool IsManagedBackendImage(uint32_t pid,
                           const std::string& expected_norm)
{
    const std::string img = ProcessImagePath(pid);
    return !img.empty() &&
           NormalizeImagePath(img) == expected_norm;
}

struct BackendResolution
{
    uint32_t    pid      = 0;
    const char* decision = "not_found";
};

/* Resolve the managed backend for an operator-requested
   teardown — the same two routes stop-openrgb2.ps1 uses:
   the pid owning the SDK listener when it is verifiably
   ours, else image-path discovery across the process list.
   A listener held by a foreign or unreadable image never
   becomes a target on its own. */
BackendResolution ResolveManagedBackend(const std::string& backend_exe,
                                        uint16_t sdk_port)
{
    BackendResolution r;
    const std::string expected = NormalizeImagePath(backend_exe);
    const uint32_t listener = FindTcpListenerPid(sdk_port);
    if(listener != 0 && IsManagedBackendImage(listener, expected))
    {
        r.pid      = listener;
        r.decision = "closing";
        return r;
    }
    const std::string want_name = LowerAscii(
        std::filesystem::path(backend_exe).filename().string());
    for(const ProcessEntry& p : EnumerateProcesses())
    {
        if(p.pid == 0 || LowerAscii(p.exe) != want_name)
        {
            continue;
        }
        if(IsManagedBackendImage(p.pid, expected))
        {
            r.pid      = p.pid;
            r.decision = "closing";
            return r;
        }
    }
    r.decision = listener != 0 ? "unverifiable" : "not_found";
    return r;
}

/* Graceful close for a VERIFIED-OWN backend (taskkill-without-/F
   semantics): WM_CLOSE posted to every top-level window the pid
   owns — the tray/headless server has no main window, so
   CloseMainWindow alone cannot reach it. Returns the count
   posted; the caller's bounded wait reports the outcome. */
size_t PostCloseToProcessWindows(uint32_t pid)
{
    struct Ctx { DWORD pid; size_t posted; } ctx{ pid, 0 };
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL
        {
            Ctx*  c    = reinterpret_cast<Ctx*>(lp);
            DWORD wpid = 0;
            GetWindowThreadProcessId(hwnd, &wpid);
            if(wpid == c->pid &&
               PostMessageW(hwnd, WM_CLOSE, 0, 0))
            {
                ++c->posted;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.posted;
}

/*---------------------------------------------------------*\
|| R-9 backend supervisor: the live probe, launcher and     ||
|| closer. Same verified-own standard as above — a pid is  ||
|| ours only when its image equals --backend-exe exactly.  ||
\*---------------------------------------------------------*/
BackendProbe LiveBackendProbe(const std::string& backend_exe,
                              uint16_t port)
{
    BackendProbe p;
    const std::string expected = NormalizeImagePath(backend_exe);
    const uint32_t listener = FindTcpListenerPid(port);
    if(listener != 0)
    {
        const std::string img = ProcessImagePath(listener);
        if(img.empty())
        {
            p.port = PortHolder::Unverifiable;
        }
        else if(NormalizeImagePath(img) == expected)
        {
            p.port    = PortHolder::Own;
            p.own_pid = listener;
        }
        else
        {
            p.port = PortHolder::Foreign;
        }
    }
    /* Every process named like the backend: ours by image, or
       "another OpenRGB.exe" (foreign or unreadable) — the
       latter blocks a spawn. */
    const std::string want_name = LowerAscii(
        std::filesystem::path(backend_exe).filename().string());
    for(const ProcessEntry& e : EnumerateProcesses())
    {
        if(e.pid == 0 || LowerAscii(e.exe) != want_name)
        {
            continue;
        }
        const std::string img = ProcessImagePath(e.pid);
        if(!img.empty() && NormalizeImagePath(img) == expected)
        {
            if(p.own_pid == 0)
            {
                p.own_pid = e.pid;
            }
        }
        else
        {
            p.other_backend_running = true;
        }
    }
    return p;
}

std::wstring Widen(const std::string& s)
{
    return std::filesystem::u8path(s).wstring();
}

std::wstring QuoteArg(const std::wstring& a)
{
    if(!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos)
    {
        return a;
    }
    std::wstring out = L"\"";
    for(wchar_t c : a)
    {
        if(c == L'"')
        {
            out += L'\\';
        }
        out += c;
    }
    out += L'"';
    return out;
}

/* The script's Start-Process -WindowStyle Hidden, as
   CreateProcess: hidden, no console, no inherited handles,
   working dir = the private config dir. */
bool LaunchBackendProcess(const BackendLaunch& l, uint32_t& pid,
                          std::string& err)
{
    const std::wstring exe = Widen(l.exe);
    std::wstring cmd = QuoteArg(exe);
    for(const std::string& a : l.args)
    {
        cmd += L' ';
        cmd += QuoteArg(Widen(a));
    }
    const std::wstring cwd = Widen(l.cwd);
    STARTUPINFOW si{};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if(!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
                       nullptr, cwd.empty() ? nullptr : cwd.c_str(),
                       &si, &pi))
    {
        err = "CreateProcess failed (error " +
              std::to_string(GetLastError()) + ")";
        return false;
    }
    pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

/* Graceful close without the teardown's wait: re-verify the
   image, signal the backend's PID-scoped shutdown event and
   post WM_CLOSE (windowed builds). Never a kill — a backend
   that ignores it stays up and the link-down rule never
   doubles it (its pid still verifies as ours). */
bool CloseOwnBackendNoWait(uint32_t pid, const std::string& backend_exe,
                           std::string& err)
{
    if(pid == 0 || backend_exe.empty() ||
       !IsManagedBackendImage(pid, NormalizeImagePath(backend_exe)))
    {
        err = "pid " + std::to_string(pid) + " no longer resolves to " +
              backend_exe + " -- left alone";
        return false;
    }
    const std::wstring ev_name =
        L"Local\\OpenRGB2-Backend-Shutdown-" + std::to_wstring(pid);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, ev_name.c_str());
    if(ev != nullptr)
    {
        SetEvent(ev);
        CloseHandle(ev);
    }
    const size_t posted = PostCloseToProcessWindows(pid);
    if(ev == nullptr && posted == 0)
    {
        err = "backend (pid " + std::to_string(pid) + ") exposes no "
              "shutdown event and owns no windows";
        return false;
    }
    return true;
}

#endif /* _WIN32 */

std::string UtcNowIso()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

} /* anonymous namespace */

EngineHost::EngineHost(EngineHostOptions options)
    : opts_(std::move(options))
{
    if(!opts_.log)
    {
        opts_.log = [](const std::string& line)
        { std::fprintf(stderr, "openrgb2-engine: %s\n", line.c_str()); };
    }
    /* R-9: the backend supervisor runs only in the managed
       layout (the launcher declares both the backend image and
       its private config dir). */
    if(opts_.backend_supervise && opts_.sdk_enabled &&
       !opts_.backend_exe.empty() && !opts_.backend_config_dir.empty())
    {
        supervisor_ =
            std::make_unique<SupervisorPolicy>(opts_.supervisor_policy);
    }
}

EngineHost::~EngineHost()
{
    Stop();
}

void EngineHost::Log(const std::string& line) const
{
    opts_.log(line);
}

uint64_t EngineHost::FramesProduced() const
{
    return frames_.load();
}

void EngineHost::RequestStop()
{
    stop_.store(true);
}

bool EngineHost::StopRequested() const
{
    return stop_.load();
}

bool EngineHost::Started() const
{
    return started_;
}

/*---------------------------------------------------------*\
|| Ownership scan (Task 3.9): report-only.                   ||
\*---------------------------------------------------------*/
void EngineHost::ScanCompetitors(bool initial)
{
    /* Gather outside the lock — process enumeration and the
       registry read are the slow parts. */
    uint32_t backend_pid = 0;
    if(opts_.sdk_enabled)
    {
        backend_pid = FindTcpListenerPid(opts_.sdk_port);
    }
    uint32_t self_pid = 0;
#ifdef _WIN32
    self_pid = GetCurrentProcessId();
#endif
    std::vector<OwnerFinding> findings = ClassifyOwners(
        opts_.process_source ? opts_.process_source()
                             : EnumerateProcesses(),
        self_pid, backend_pid);
    const int dynamic_lighting =
        opts_.dynamic_lighting_source
            ? opts_.dynamic_lighting_source()
            : ReadDynamicLightingEnabled();
    {
        OwnerFinding dl;
        if(ClassifyDynamicLighting(dynamic_lighting, dl))
        {
            findings.push_back(std::move(dl));
        }
    }

    std::vector<OwnerFinding> before;
    {
        std::lock_guard<std::mutex> g(ownership_mutex_);
        before = std::move(ownership_.findings);
        ownership_.scanned           = true;
        ownership_.backend_pid       = backend_pid;
        ownership_.backend_listening = backend_pid != 0;
        ownership_.findings          = findings;
        ownership_.dynamic_lighting  = dynamic_lighting;
    }

    if(!initial)
    {
        const std::vector<std::string> changes =
            DescribeOwnerChanges(before, findings);
        for(const std::string& line : changes)
        {
            Log("ownership re-scan: " + line);
        }
        /* R-10: push the change — one event per scan that
           changed anything. The initial scan runs before the
           IPC surface exists; SUBSCRIBE's snapshot covers it. */
        if(!changes.empty() && service_)
        {
            service_->EmitOwnership();
        }
        return;
    }
    if(findings.empty())
    {
        Log("ownership scan: no competing RGB owners detected");
    }
    for(const OwnerFinding& f : findings)
    {
        Log(std::string("ownership ") +
            (f.conflict ? "CONFLICT" : "notice") + ": " +
            f.owner + " (pid " + std::to_string(f.pid) + ", " +
            f.exe + ") — " + f.detail);
    }
    if(dynamic_lighting < 0)
    {
        Log("ownership: Windows Dynamic Lighting state unknown "
            "(setting not readable)");
    }
}

void EngineHost::ScanOwners()
{
    ScanCompetitors(/*initial*/ true);
    next_owner_scan_ = Clock::now() + opts_.owner_rescan;

    /* The managed config's sole-writer check covers BOTH
       plugin-load paths (plan section 4): <config>/plugins +
       OpenRGB.json and the Windows system-plugin dir beside
       the backend exe — resolved from the listener PID's image
       path. An unreadable image (e.g. the backend runs
       elevated and we do not) is reported honestly, and the
       check still runs on whichever scope is known. */
    std::string backend_exe_dir;
    if(ownership_.backend_pid != 0)
    {
        const std::string img =
            ProcessImagePath(ownership_.backend_pid);
        if(!img.empty())
        {
            const std::string::size_type p =
                img.find_last_of("/\\");
            backend_exe_dir =
                p == std::string::npos ? img : img.substr(0, p);
        }
        else
        {
            Log("ownership: backend image path unreadable (pid " +
                std::to_string(ownership_.backend_pid) +
                ") — exe-dir plugin scan skipped");
        }
    }
    if(!opts_.backend_config_dir.empty() || !backend_exe_dir.empty())
    {
        ownership_.config_checked = true;
        ownership_.config =
            CheckManagedBackendConfig(opts_.backend_config_dir,
                                      backend_exe_dir);
        for(const std::string& f : ownership_.config.findings)
        {
            Log("managed-config: " + f);
        }
    }
}

OwnershipReport EngineHost::Ownership() const
{
    std::lock_guard<std::mutex> g(ownership_mutex_);
    return ownership_;
}

nlohmann::json EngineHost::OwnershipJson() const
{
    /* Findings verbatim — the report-only contract (plan
       section 4) means the UI shows the scan's conflicts,
       never a kill button or a UI-side re-classification. */
    std::lock_guard<std::mutex> g(ownership_mutex_);
    nlohmann::json conflicts = nlohmann::json::array();
    nlohmann::json notices   = nlohmann::json::array();
    for(const OwnerFinding& f : ownership_.findings)
    {
        nlohmann::json e = nlohmann::json::object({
            { "owner",  f.owner  },
            { "key",    f.key    },   /* R-10: stable guidance key */
            { "exe",    f.exe    },
            { "pid",    f.pid    },
            { "detail", f.detail },
        });
        (f.conflict ? conflicts : notices).push_back(
            std::move(e));
    }
    return nlohmann::json::object({
        { "scanned", ownership_.scanned },
        { "backend", nlohmann::json::object({
            { "listening", ownership_.backend_listening },
            { "attached",  ownership_.backend_attached  },
        })},
        { "conflicts", std::move(conflicts) },
        { "notices",   std::move(notices)   },
        { "dynamic_lighting",
          DynamicLightingStateName(ownership_.dynamic_lighting) },
        { "config", nlohmann::json::object({
            { "checked",           ownership_.config_checked },
            { "findings",          ownership_.config.findings },
            { "enabled_plugins",
              ownership_.config.enabled_plugins },
            { "legacy_producers",
              ownership_.config.legacy_producers },
        })},
    });
}

/*---------------------------------------------------------*\
|| Injected deps (Tasks 3.6/3.7)                            ||
\*---------------------------------------------------------*/

/* Playback-mode state machine (Task 3.6): owns the
   LiveEffects <-> AnimationPlayback contract — per-scene
   persisted selection, lane suppression (one producer per
   wireless group), SDK preset select, revision-based
   acknowledgement, pause/stop transport control, and the
   baked wired timeline that follows the validated
   wireless clock (no backend clock channel exists over
   the SDK wire, so clock_sample stays unset and sync is
   honestly reported as unavailable). All backend access
   goes through injected deps. */
PlaybackController::Deps EngineHost::BuildPlaybackDeps()
{
    PlaybackController::Deps pdeps;
    pdeps.snapshots =
        [this]() -> std::vector<DeviceSnapshot>
        {
            return sdk_ ? sdk_->Snapshots()
                        : std::vector<DeviceSnapshot>{};
        };
    pdeps.wired_targets =
        [this]() -> std::vector<DeviceSnapshot>
        {
            return WiredTargets();
        };
    pdeps.select_mode =
        [this](const DeviceKey& k, const std::string& name,
               const PlaybackModePatch& p, std::string& derr)
        {
            if(!sdk_)
            {
                derr = "no SDK transport configured";
                return false;
            }
            sdk::ModePatch mp;
            mp.has_speed     = p.has_speed;     mp.speed     = p.speed;
            mp.has_direction = p.has_direction; mp.direction = p.direction;
            mp.has_colors    = p.has_colors;    mp.colors    = p.colors;
            if(!sdk_->SelectMode(k, name, mp))
            {
                derr = sdk_->LastError();
                return false;
            }
            return true;
        };
    pdeps.active_mode =
        [this](const DeviceKey& k, BackendModeReport& rep)
        {
            if(!sdk_)
            {
                return false;
            }
            int32_t        idx = -1;
            sdk::SdkMode   m;
            if(!sdk_->ActiveMode(k, idx, &m))
            {
                return false;
            }
            rep.valid     = true;
            rep.name      = m.name;
            rep.speed     = m.speed;
            rep.direction = m.direction;
            rep.colors    = m.colors;
            return true;
        };
    pdeps.refresh_status =
        [this](std::string& derr)
        {
            if(!sdk_)
            {
                derr = "no SDK transport configured";
                return false;
            }
            if(!sdk_->RefreshStatus())
            {
                derr = sdk_->LastError();
                return false;
            }
            return true;
        };
    pdeps.set_custom_mode =
        [this](const DeviceKey& k, std::string& derr)
        {
            if(!sdk_)
            {
                derr = "no SDK transport configured";
                return false;
            }
            if(!sdk_->SetCustomMode(k))
            {
                derr = sdk_->LastError();
                return false;
            }
            return true;
        };
    pdeps.direct_submit =
        [this](const RgbFrame& f)
        {
            /* Pause-hold / stop-black bypass the suppressed
               lane — the controller is the one producer. */
            return sdk_
                ? sdk_->SubmitLatest(f)
                : SubmitResult::Reject(
                      "no SDK transport configured");
        };
    pdeps.wired_submit =
        [this](const RgbFrame& f)
        { return pipeline_->SubmitLatest(f); };
    pdeps.set_suppressed =
        [this](const DeviceKey& k, bool s, const std::string& r)
        { pipeline_->SetSuppressed(k, s, r); };
    pdeps.drop_pending =
        [this](const DeviceKey& k)
        { return pipeline_->DropPending(k); };
    pdeps.reset_delivered =
        [this](const DeviceKey& k)
        { pipeline_->ResetDeliveredState(k); };
    pdeps.wait_idle =
        [this](const DeviceKey& k, Clock::duration timeout)
        { return pipeline_->WaitIdle(k, timeout); };
    /* clock_sample — the N01 observability pipe doubles as the
       clock channel: LIANLI_WIRELESS_STATUS carries the group's
       rgb_clock sample, so the playback-clock sync dep is now
       real. The controller's own FindWireless() picks the first
       connected "Wireless:" snapshot; this resolves the same
       predicate so the dep needs no key plumbing. A stock
       backend (no packet 4000) answers false cleanly — sync
       reports honest Unknown/Stale. */
    pdeps.clock_sample =
        [this](PlaybackClockSnapshot& out)
        {
            if(!sdk_)
            {
                return false;
            }
            for(const DeviceSnapshot& snap : sdk_->Snapshots())
            {
                if(IsWirelessGroupController(snap) &&
                   snap.connection == ConnectionState::Connected)
                {
                    return sdk_->WirelessClockSample(snap.key, out);
                }
            }
            return false;
        };
    return pdeps;
}

/* Power lifecycle (Task 3.7): display-off gates ALL RGB
   frame output + reactive capture through the pipeline's
   output pause and freezes an active uploaded loop ONCE
   (explicit sampled-hold, never a write stream and never
   a maintenance stop); suspend discards pending work and
   parks the controller; wake/reconnect revalidates
   identity before any restore write. All hardware-facing
   operations are the injected deps below — the Windows
   monitor only posts events into this machine. */
PowerLifecycle::Deps EngineHost::BuildLifecycleDeps()
{
    PowerLifecycle::Deps ldeps;
    ldeps.set_output_paused =
        [this](bool p, const std::string& r)
        { pipeline_->SetOutputPaused(p, r); };
    ldeps.lane_keys =
        [this]() { return pipeline_->LaneKeys(); };
    ldeps.drop_pending =
        [this](const DeviceKey& k)
        { return pipeline_->DropPending(k); };
    ldeps.reset_delivered =
        [this](const DeviceKey& k)
        { pipeline_->ResetDeliveredState(k); };
    ldeps.last_delivered =
        [this](const DeviceKey& k, RgbFrame& out)
        { return pipeline_->LastDelivered(k, out); };
    ldeps.reliable_submit =
        [this](const RgbFrame& f)
        { return pipeline_->SubmitReliable(f); };
    ldeps.set_capture_paused =
        [this](bool p)
        {
            /* The reactive-input pause seam (Task 4.6): the
               ReactiveInputs manager reads this — paused
               sources skip capture work (the screen sampler
               does NO GPU reads while displays sleep) and the
               scene worker sees an empty input state on the
               next tick. */
            capture_paused_.store(p);
            if(reactive_)
            {
                reactive_->SetPaused(p);
            }
        };
    ldeps.snapshots =
        [this]() -> std::vector<DeviceSnapshot>
        {
            return sdk_ ? sdk_->Snapshots()
                        : std::vector<DeviceSnapshot>{};
        };
    ldeps.playback = playback_.get();
    ldeps.log =
        [this](const std::string& line) { Log(line); };
    return ldeps;
}

/* studio-import Task 11 — physical LED calibration. The
   session reuses the SAME injected seams the playback
   controller owns (lane suppression primitives, the
   transport-bypassing direct submit, SetCustomMode): the
   single-producer rule is enforced by construction — there
   is no second hardware path to write through. The "changed"
   hook publishes the session-bound "calibration" IPC event
   through the service's targeted emitter. */
CalibrationSession::Deps EngineHost::BuildCalibrationDeps()
{
    CalibrationSession::Deps d;
    d.snapshots =
        [this]() -> std::vector<DeviceSnapshot>
        {
            return sdk_ ? sdk_->Snapshots()
                        : std::vector<DeviceSnapshot>{};
        };
    d.playback       = playback_.get();
    d.set_suppressed =
        [this](const DeviceKey& k, bool s, const std::string& r)
        { pipeline_->SetSuppressed(k, s, r); };
    d.is_suppressed  =
        [this](const DeviceKey& k)
        { return pipeline_->IsSuppressed(k); };
    d.drop_pending   =
        [this](const DeviceKey& k)
        { return pipeline_->DropPending(k); };
    d.reset_delivered =
        [this](const DeviceKey& k)
        { pipeline_->ResetDeliveredState(k); };
    d.wait_idle      =
        [this](const DeviceKey& k, Clock::duration t)
        { return pipeline_->WaitIdle(k, t); };
    d.direct_submit  =
        [this](const RgbFrame& f)
        {
            return sdk_
                ? sdk_->SubmitLatest(f)
                : SubmitResult::Reject(
                      "no SDK transport configured");
        };
    d.set_custom_mode =
        [this](const DeviceKey& k, std::string& derr)
        {
            if(!sdk_)
            {
                derr = "no SDK transport configured";
                return false;
            }
            if(!sdk_->SetCustomMode(k))
            {
                derr = sdk_->LastError();
                return false;
            }
            return true;
        };
    d.output_paused =
        [this]() { return pipeline_->OutputPaused(); };
    d.log =
        [this](const std::string& line)
        { Log("calibration: " + line); };
    d.changed =
        [this]
        {
            if(service_)
            {
                service_->EmitCalibrationStatus();
            }
        };
    return d;
}

/*---------------------------------------------------------*\
|| Startup                                                  ||
\*---------------------------------------------------------*/
bool EngineHost::Start(std::string& err)
{
    if(started_)
    {
        err = "already started";
        return false;
    }
    start_attempted_ = true;

    /* Audit E-2: a 1 ms timer resolution for the engine's
       lifetime, before any worker starts — the 60 Hz scene
       clock, the lanes' budget waits and the owner loop all
       wake on the system timer tick (15.6 ms by default).
       Released at the end of Stop(). A refusal is logged, not
       fatal: the engine still runs, just coarser. */
    if(!timer_resolution_.Acquire(1, opts_.timer_api))
    {
        Log("timer resolution: 1 ms request refused — frame "
            "pacing stays on the default system tick");
    }

    /* 1. Single instance (plan section 4): exactly one engine
       owns the managed configuration. Refusing to start is the
       honest answer to a double-own attempt — the holder is
       reported, never taken over or killed. */
    if(opts_.single_instance)
    {
        instance_ = std::make_unique<SingleInstanceGuard>(
            opts_.instance_name);
        if(!instance_->Acquire(err))
        {
            return false;
        }
    }

    /* 2. Competing-owner scan + managed-config sole-writer
       check — report only, both non-fatal. */
    if(opts_.scan_owners)
    {
        ScanOwners();
    }

    /* The delivery pipeline is the engine's submitLatest sink:
       per-controller depth-1 pending lanes with independent
       budgets, unchanged-frame suppression, and the distinct
       reliable convergence path (Task 3.3). It drains into the
       SDK transport below; with no transport configured its
       lanes simply idle. */
    pipeline_ = std::make_shared<DeliveryPipeline>();
    pipeline_->SetKeyframeInterval(opts_.delivery_keyframe);
    engine_.SetFrameSink(pipeline_);

    /* 4. Backend attach: the backend is launched EXTERNALLY
       (plan section 4 — `--server --server-host 127.0.0.1
       --server-port 6744 --noautoconnect --localconfig` with a
       private configuration directory as its working dir) and
       remains the sole hardware owner; this process never
       spawns it. The scan above already resolved whether the
       port is listening; Connect() failing while the port is
       held means a foreign service — reported as a conflict. */
    if(opts_.sdk_enabled)
    {
        sdk::OpenRgbClientOptions copts;
        copts.host    = opts_.sdk_host;
        copts.port    = opts_.sdk_port;
        copts.backoff = opts_.sdk_backoff;
        std::unique_ptr<sdk::IStreamTransport> transport =
            opts_.transport_factory
                ? opts_.transport_factory()
                : std::make_unique<sdk::TcpSocket>();
        sdk_ = std::make_shared<sdk::OpenRgbClient>(
            copts, std::move(transport));
        pipeline_->SetTransport(sdk_);

        if(ownership_.backend_listening)
        {
            Log("backend listener detected on port " +
                std::to_string(opts_.sdk_port) + " (pid " +
                std::to_string(ownership_.backend_pid) +
                ") — attaching via SDK; the engine does not "
                "own the backend process");
        }
        if(sdk_->Connect())
        {
            ownership_.backend_attached = true;
            Log("SDK connected to " + copts.host + ":" +
                std::to_string(copts.port) + " (protocol " +
                std::to_string(sdk_->ProtocolVersion()) + ", " +
                std::to_string(sdk_->Snapshots().size()) +
                " controllers)");
        }
        else if(ownership_.backend_listening)
        {
            Log("ownership CONFLICT: port " +
                std::to_string(opts_.sdk_port) +
                " is held by a process that did not complete "
                "the OpenRGB SDK handshake (" +
                sdk_->LastError() + ") — another owner may be "
                "squatting on the managed endpoint; will keep "
                "retrying");
            sdk_->ReconnectNow();
        }
        else
        {
            /* Not fatal: the sink reports Disconnected and the
               bounded backoff keeps retrying via Poll(). */
            Log("no backend detected at " + copts.host + ":" +
                std::to_string(copts.port) + " (" +
                sdk_->LastError() + ") — launch the pinned "
                "backend with --server --server-host 127.0.0.1 "
                "--server-port 6744 --noautoconnect "
                "--localconfig; will retry");
            sdk_->ReconnectNow();
        }
        SyncLanes(*pipeline_, *sdk_);
    }

    /* Playback-mode state machine (Task 3.6): the deps are
       built first, then the durable-profile block below adds
       save_modes before the controller is constructed. */
    PlaybackController::Deps pdeps = BuildPlaybackDeps();

    /*---- Task 3.8 — the durable engine profile -------------*\
    || profiles/local/ is gitignored user state: resolved      ||
    || identities (serials/locations) live here, never in      ||
    || committed files. The store does atomic temp+rename      ||
    || writes, keeps a last-known-good backup, and migrates    ||
    || older schema versions forward. A corrupt primary is     ||
    || reported honestly and recovered from LKG — never a      ||
    || blank or guessed state.                                 ||
    \*---------------------------------------------------------*/
    profile_ = std::make_unique<ProfileStore>(opts_.state_path);
    {
        const ProfileStore::LoadResult lr = profile_->Load();
        switch(lr.status)
        {
        case ProfileStore::LoadStatus::Empty:
            break;   /* fresh start — not an error */
        case ProfileStore::LoadStatus::Loaded:
            engine_state_ = lr.state;
            break;
        case ProfileStore::LoadStatus::Migrated:
            engine_state_ = lr.state;
            Log("profile " + opts_.state_path +
                " migrated to schema v" +
                std::to_string(kEngineStateSchemaVersion));
            break;
        case ProfileStore::LoadStatus::Recovered:
            engine_state_ = lr.state;
            Log(lr.error);
            break;
        case ProfileStore::LoadStatus::Failed:
            /* Honest report; the engine still runs (the state
               is empty and flagged, never guessed). */
            Log(lr.error + " — running with no profile state");
            break;
        }
        for(const std::string& w : lr.warnings)
        {
            Log("profile: " + w);
        }
    }

    /* Durable per-scene playback selections (Task 3.8): the
       controller hands its whole LRU store on every change —
       write it atomically through the profile store. */
    pdeps.save_modes =
        [this](const std::vector<PersistedSceneMode>& modes,
               std::string& serr)
        {
            std::lock_guard<std::mutex> g(state_mutex_);
            engine_state_.scenes = modes;
            return profile_->Save(engine_state_, serr);
        };

    playback_ = std::make_unique<PlaybackController>(
        std::move(pdeps));
    {
        const size_t restored =
            playback_->RestoreSceneModes(engine_state_.scenes);
        if(restored > 0)
        {
            Log("restored " + std::to_string(restored) +
                " per-scene playback selections from " +
                opts_.state_path);
        }
    }

    /*---- R-1 runtime output control ----------------------------*\
    || master + blackout come back from the profile (hold never   ||
    || does — a restart resumes). Restored BEFORE the startup     ||
    || scene applies, so the first frame is already scaled and a  ||
    || restored Off reaches an uploaded loop's re-apply           ||
    || (PlaybackController::OnSceneApplied keeps the override).   ||
    || Every later change runs OnOutputChanged: worker hold,      ||
    || playback routing, persistence, the "output" event.         ||
    \*---------------------------------------------------------*/
    if(engine_state_.output)
    {
        output_.Restore(*engine_state_.output);
        const OutputState os = output_.State();
        Log("restored output control: master " +
            std::to_string(int(os.master * 100.0f + 0.5f)) + "%" +
            (os.blackout ? ", off" : "") + " from " +
            opts_.state_path);
    }
    playback_->SetUserOverride(false, output_.State().blackout);
    output_.SetChanged(
        [this](const OutputState&) { OnOutputChanged(); });

    /*---- Task 4.6 reactive inputs ----------------------------*\
    || Audio / screen / key capture, Qt-free ports of the        ||
    || preserved plugin's inputs. The manager itself is cheap:   ||
    || nothing captures until an input is enabled through IPC.   ||
    || Three honest wirings:                                     ||
    ||   - the bus stamps events on the scene clock so ages      ||
    ||     evaluate correctly against the tick's scene_time_s    ||
    ||   - the worker PULLS a Snapshot() each tick (the          ||
    ||     InputProvider hook), so pause/disable stops feeding   ||
    ||     the scene on the very next frame                      ||
    ||   - the persisted selection re-arms on start; a source    ||
    ||     that fails to start stays disabled and reports its    ||
    ||     own reason — never a fabricated toggle                ||
    \*---------------------------------------------------------*/
    if(opts_.reactive_inputs)
    {
        ReactiveInputs::Deps rdeps;
        rdeps.source_factory =
            opts_.reactive_factory
                ? opts_.reactive_factory
                : [](const std::string& id)
                  { return platform::MakeReactiveSource(id); };
        rdeps.enumerate_displays =
            [] { return platform::EnumerateDisplays(); };
        rdeps.now =
            [this]
            {
                /* Scene-clock seconds. The worker's epoch is
                   only set once engine_.Start() runs (Restore
                   above races ahead of it) — until then stamp
                   t = 0 so early events age against the same
                   base the first ticks evaluate at. */
                if(!engine_.Worker().Running())
                {
                    return 0.0;
                }
                return std::chrono::duration<double>(
                    Clock::now() - engine_.Worker().Epoch())
                    .count();
            };
        rdeps.changed =
            [this] { OnReactiveChanged(); };
        reactive_ = std::make_unique<ReactiveInputs>(
            std::move(rdeps));
        engine_.Worker().SetInputProvider(
            [this]() { return reactive_->Snapshot(); });
        reactive_->Restore(engine_state_.reactive);
        {
            const auto persisted = engine_state_.reactive;
            for(const auto& p : persisted)
            {
                if(p.enabled)
                {
                    Log("reactive input '" + p.id +
                        "' re-armed from " + opts_.state_path);
                }
            }
        }
    }

    lifecycle_ = std::make_unique<PowerLifecycle>(
        BuildLifecycleDeps());

    /* Task 11: the physical LED-calibration session — built
       after playback_ + lifecycle_ exist (it coordinates
       with both). Its worker is the session's only hardware
       writer; the calibration_* WORKSPACE verbs bind to it
       via ipc_deps.calibration below. */
    calibration_ = std::make_unique<CalibrationSession>(
        BuildCalibrationDeps());

    /* The Windows event source: a dedicated thread + message-
       only window carrying the display-state and suspend/resume
       registrations (Qt-free adaptation of the preserved
       DisplayPowerMonitor). Failure is non-fatal — the engine
       runs unmonitored (output never power-pauses) and the
       reason is logged. */
    if(opts_.power_monitor)
    {
        std::string perr;
        if(!power_monitor_.Start(
            [this](PowerLifecycle::Event e)
            { lifecycle_->PostEvent(e); }, perr))
        {
            Log("power monitoring unavailable: " + perr +
                " (continuing)");
        }
    }

    /* Every evaluated scene frame is split into per-controller
       complete-controller frames and handed to the pipeline via
       the engine's submitLatest sink. While an uploaded loop
       owns the output the evaluate gate below skips evaluation
       and mapping (Tick produces the wired baked frames; the
       owned group's suppressed lane would refuse a live frame).
       A tick that read the gate just before ownership began is
       still mapped: submissions to OTHER controllers are then
       skipped, and one to the suppressed wireless group is
       refused by its lane with a reason — never a silent
       drop. */
    /* While the power lifecycle gates output (display off /
       suspend) every mapped frame would only be rejected at the
       pipeline: skip evaluation + mapping instead of running
       them at 60 Hz just to inflate the `rejected` counters. */
    engine_.Worker().SetOutputGate(
        [this]() { return pipeline_->OutputPaused(); });
    /* E-6: while the uploaded loop owns the output every mapped
       frame would be skipped (wired) or refused by the
       suppressed lane (the owned group) — so skip evaluation
       and mapping entirely. The clock still ticks: Tick bakes
       the wired frames on it. A frame the gate skipped is never
       mapped even if ownership ended mid-tick (its colors are
       empty); the next tick evaluates normally. */
    engine_.Worker().SetEvaluateGate(
        [this]() { return playback_->PlaybackOwnsWired(); });
    /* R-1: the runtime master scale — the mapper multiplies the
       arbitrated OutputControl scale into every live write, so a
       tray brightness/Off change lands on the next tick without
       a document edit (uploaded loops are not scaled). */
    mapper_.SetOutputScaleSource(&output_.EffectiveScaleSource());
    engine_.Worker().SetFrameCallback(
        [this](const EvaluatedFrame& eval)
        {
            ++frames_;
            if(!eval.evaluate_gated)
            {
                const bool owns = playback_->PlaybackOwnsWired();
                for(RgbFrame& frame : mapper_.MapFrame(eval))
                {
                    if(owns &&
                       !playback_->OwnsGroup(frame.controller))
                    {
                        continue;
                    }
                    engine_.SubmitLatest(std::move(frame));
                }
            }
            playback_->Tick(eval.sample_time);
        });

    if(!opts_.effects_dir.empty())
    {
        /* Preset-look files loaded as the registry's defaults
           layer; DLS_EFFECT_DIR and the source-tree probe still
           apply. */
        studio::EffectLooks().LoadDefaultsDirectory(
            opts_.effects_dir, nullptr);
    }

    /* Task 2 (studio-import): the managed workspace store +
       commit service. Its publish seam is the same scene-apply
       path --scene and SET_SCENE use; its libraries are the
       packaged types/looks plus the managed root's own
       library layer (types the workspace actually uses are
       snapshotted per generation regardless). The store is
       purely file state — constructing it never touches
       output. Built BEFORE the IPC service so the
       WORKSPACE_COMMAND surface can bind to it. */
    {
        WorkspaceService::Deps wdeps;
        wdeps.devices = BuildDeviceRegistry(
            opts_.devices_dir,
            [this](const std::string& l) { Log(l); });
        wdeps.effects = studio::EffectLooks();
        /* apply_scene references service_ lazily — the lambda
           fires only from a workspace job, long after the
           service exists. */
        wdeps.apply_scene =
            [this](std::shared_ptr<const WorkspaceCandidate> cand,
                   std::string& aerr) -> bool
            {
                return service_ && cand &&
                       service_->ApplyWorkspaceCandidate(*cand,
                                                         aerr);
            };
        /* The managed workspace is durable engine state: every
           adoption (open/commit/import) persists the id so a
           restart re-adopts it below — engine restart must not
           strand the managed context (the UI reports
           managed:false against a live connection and every
           revision-gated write refuses). Runs off the service
           mutex on the adopting thread; the profile write is
           bounded like every other state_mutex_ persist. */
        wdeps.active_workspace_changed =
            [this](const std::string& id)
            {
                if(!profile_)
                {
                    return;
                }
                std::lock_guard<std::mutex> g(state_mutex_);
                if(engine_state_.active_workspace == id)
                {
                    return;   /* unchanged — never rewrite */
                }
                engine_state_.active_workspace = id;
                std::string serr;
                if(!profile_->Save(engine_state_, serr))
                {
                    Log("profile save failed: " + serr);
                }
            };
        workspaces_ = std::make_unique<WorkspaceService>(
            opts_.workspace_root.empty()
                ? WorkspaceStore::DefaultRoot()
                : opts_.workspace_root,
            std::move(wdeps));
    }

    /* The IPC service owns the scene-apply path (Task 3.5): the
       --scene CLI and SET_SCENE both go through
       ApplySceneDocument, so a UI-applied scene and a
       CLI-loaded scene are indistinguishable to the engine. */
    {
        ipc::EngineService::Deps ipc_deps;
        ipc_deps.pipeline = pipeline_.get();
        ipc_deps.mapper   = &mapper_;
        ipc_deps.sdk      = sdk_.get();
        ipc_deps.playback = playback_.get();
        ipc_deps.power    = lifecycle_.get();
        /* Task 11: the physical calibration session — the
           calibration_* WORKSPACE verbs bind their opaque
           session tokens to it. nullptr keeps the verbs
           answering Unavailable honestly. */
        ipc_deps.calibration = calibration_.get();
        /* Task 4.6: the REACTIVE_COMMAND surface — nullptr is
           legal; the service then reports available=false
           honestly rather than fabricating toggles. */
        ipc_deps.reactive = reactive_.get();
        /* R-1: OUTPUT_COMMAND + the "output" event + the
           output_control_v1 capability. */
        ipc_deps.output = &output_;
        /* Task 4.4 layout verbs: identify and the set_binding
           create path resolve against live topology — the
           same source the mapper syncs from. */
        ipc_deps.snapshots =
            [this]() -> std::vector<DeviceSnapshot>
            {
                return sdk_ ? sdk_->Snapshots()
                            : std::vector<DeviceSnapshot>{};
            };
        /* The "Quit All" hook (plan section 4): QUIT flips the
           owner loop's stop flag; the ordered, bounded teardown
           runs in Stop() (OrderlyShutdown). The opt-in
           stop_backend flag resolves the managed backend NOW so
           the OK reply can honestly report the handoff decision
           — "closing" means a verified-own pid was resolved and
           WM_CLOSE will be posted during teardown, never that
           the process already exited. The pid is stored
           BEFORE the stop flag (audit E-11): Run() returns as
           soon as stop_ is set and Stop() registers the
           backend-handoff step only for a pid it can already
           see, so the old flag-first order could skip the
           handoff while the reply said "closing". */
        ipc_deps.request_shutdown =
            [this](bool stop_backend) -> nlohmann::json
            {
                const nlohmann::json reply =
                    ArmBackendHandoff(stop_backend);
                stop_.store(true);
                return reply;
            };
        /* Task 4.7: the startup ownership scan surfaced
           read-only in STATE/DIAGNOSTICS. ownership_ is
           written during Start() before the IPC surface
           exists — the publish order (scan -> service ->
           listeners) makes the provider race-free. */
        ipc_deps.ownership =
            [this] { return OwnershipJson(); };
        /* Audit E-12b: the exit/idle output policy in STATE and
           the QUIT reply's exit_policy. */
        ipc_deps.output_policy =
            [this] { return OutputPolicyJson(); };
        /* R-9: the backend supervisor's status in STATE and the
           "supervisor" event snapshot. */
        ipc_deps.supervisor =
            [this] { return SupervisorJson(); };
        ipc_deps.set_output_policy =
            [this](const OutputPolicy& p, std::string& e)
            { return SetOutputPolicy(p, e); };
        /* Task 3.8 offline bindings: a persisted resolved
           identity completes name-only scene bindings before
           apply — the same binding id keeps pointing at the
           same physical device across restarts, and a
           same-named different device can never inherit the
           binding. */
        ipc_deps.prepare_scene =
            [this](std::shared_ptr<const studio::SceneDocument>& doc)
            {
                auto copy =
                    std::make_shared<studio::SceneDocument>(*doc);
                {
                    std::lock_guard<std::mutex> g(state_mutex_);
                    if(AugmentSceneBindings(*copy,
                                            engine_state_.bindings))
                    {
                        doc = copy;
                    }
                }
            };
        /* Last-scene persistence: every successful apply that
           carries objects lands the document beside the state
           file, so a restart with no --scene restores what was
           last applied. A write failure is logged, never
           fatal — persistence must not break the apply that
           produced it. */
        ipc_deps.persist_scene =
            [this](const studio::SceneDocument& doc)
            {
                if(!profile_)
                {
                    return;
                }
                std::string serr;
                /* Hand the JSON over as-is: the store
                   serializes it once, no dump-then-reparse. */
                if(!profile_->SaveSceneSnapshotJson(
                       studio::ToJson(doc), serr))
                {
                    Log("last-scene snapshot save failed: " +
                        serr);
                }
            };
        /* studio-import Task 3: binding the workspace service
           makes the WORKSPACE_COMMAND verb surface exist —
           WELCOME advertises "studio_workspace_v1" exactly
           then. */
        ipc_deps.workspace = workspaces_.get();
        service_ = std::make_unique<ipc::EngineService>(
            engine_, std::move(ipc_deps));
    }

    if(opts_.ipc_enabled && !StartIpc())
    {
        /* StartIpc logs the honest reason and continues —
           headless operation is valid (plan section 4: no UI
           required). */
    }

    if(!ApplyStartupScene(err))
    {
        return false;
    }

    /* Re-adopt the last managed workspace — the scene restore
       above already decided what physical output shows;
       Open() only re-adopts the workspace's generation and
       revision truth, it NEVER applies a scene, so the
       restored/--scene output stays the applied truth. A
       workspace whose directory vanished is reported and the
       engine continues unmanaged — a stale pointer is never
       a startup failure. The guard leaves a client-driven
       adopt alone if one raced ahead (IPC listeners are
       already up). Because they are, the persisted id is
       copied under state_mutex_: a client adopt writes it
       from a workspace job through active_workspace_changed
       at the same time (audit E-9). */
    std::string wsid;
    {
        std::lock_guard<std::mutex> g(state_mutex_);
        wsid = engine_state_.active_workspace;
    }
    if(workspaces_ && !wsid.empty() &&
       !workspaces_->ManagedActive())
    {
        /* Open reads records off disk; the store type-checks
           them, but anything that still throws (a damaged
           file, a filesystem error, allocation) must cost the
           managed state only — never the process, and never
           every subsequent launch (audit I-11). */
        WorkspaceService::Result wr;
        try
        {
            wr = workspaces_->Open(wsid);
        }
        catch(const std::exception& e)
        {
            wr       = WorkspaceService::Result{};
            wr.error = std::string("re-adopt threw: ") + e.what();
        }
        catch(...)
        {
            wr       = WorkspaceService::Result{};
            wr.error = "re-adopt threw a non-standard exception";
        }
        if(wr.ok)
        {
            Log("re-adopted managed workspace '" + wsid +
                "' from " + opts_.state_path);
            for(const std::string& w : wr.warnings)
            {
                Log("workspace '" + wsid + "': " + w);
            }
        }
        else
        {
            Log("managed workspace '" + wsid +
                "' could not be re-adopted (" + wr.error +
                ") — starting unmanaged");
        }
    }

    if(!engine_.Start())
    {
        err = "failed to start scene worker";
        return false;
    }
    Log("running (60 Hz scene clock, no UI required)");

    /* Per-lane delivery workers: a lane blocked inside a slow
       transport send cannot stall the others. */
    pipeline_->Start();
    started_ = true;
    return true;
}

bool EngineHost::StartIpc()
{
    bool any = false;
    ipc_server_ = std::make_unique<ipc::IpcServer>(*service_);
    service_->SetEventSink(
        [this](const ipc::IpcEvent& ev) { ipc_server_->Emit(ev); });
    /* Task 7: the session-bound draft preview feed — targeted
       delivery to the owning session, never a broadcast. */
    service_->SetSessionEventSink(
        [this](uint64_t sid, const ipc::IpcEvent& ev,
               const std::string& key)
        { ipc_server_->EmitTo(sid, ev, key); });
    service_->SetServerStatsProvider(
        [this]() { return ipc_server_->Stats(); });
    /* Event bodies nobody subscribed to are never built:
       EngineService asks the server before building one. */
    service_->SetEventInterest(
        [this](const std::string& k)
        { return ipc_server_->HasSubscriber(k); });

    /* Both pipes share the restricted DACL (creating user +
       LocalSystem only). The diagnostics pipe is read-only
       by construction: DiagGet is the only verb it serves. */
    cmd_listener_ = std::make_unique<ipc::NamedPipeListener>(
        *ipc_server_, ipc::ChannelKind::Command,
        opts_.command_pipe_name);
    diag_listener_ = std::make_unique<ipc::NamedPipeListener>(
        *ipc_server_, ipc::ChannelKind::Diagnostics,
        opts_.diagnostics_pipe_name);
    std::string err;
    if(!cmd_listener_->Start(err))
    {
        Log("command pipe unavailable: " + err +
            " (continuing without command IPC)");
        cmd_listener_.reset();
    }
    else
    {
        any = true;
    }
    err.clear();
    if(!diag_listener_->Start(err))
    {
        Log("diagnostics pipe unavailable: " + err +
            " (continuing)");
        diag_listener_.reset();
    }
    else
    {
        any = true;
    }
    return any;
}

bool EngineHost::ApplyStartupScene(std::string& err)
{
    if(!opts_.scene_path.empty())
    {
        /* Task 2: --scene loads through the canonical workspace
           candidate — expanded scene JSON, a compact studio.json
           workspace, a v1/v2 expanded workspace, or a legacy
           host-settings blob all land on the same resolved
           SceneDocument while the compact authoring form survives
           for the workspace layer. */
        const studio::PresetRegistry device_registry =
            BuildDeviceRegistry(opts_.devices_dir,
                [this](const std::string& l) { Log(l); });
        /* A --scene file may legitimately name a personal look —
           resolve against the effective registry (packaged looks
           + the managed root's library/effects), the same set
           ApplySceneDocument validates against. Root-level file
           state needs no open workspace. */
        const studio::EffectRegistry effect_registry =
            workspaces_
                ? workspaces_->Store().EffectiveEffectRegistry()
                : studio::EffectLooks();
        WorkspaceCandidate cand;
        std::vector<std::string> warnings;
        if(!LoadWorkspaceCandidate(opts_.scene_path, device_registry,
                                   effect_registry,
                                   cand, err, &warnings))
        {
            return false;
        }
        for(const std::string& w : warnings)
        {
            Log(opts_.scene_path + ": " + w);
        }
        Log(opts_.scene_path + ": loaded " +
            WorkspaceSourceFormName(cand.source_form) +
            (cand.migrated ? " (migrated)" : ""));
        if(!service_->ApplyWorkspaceCandidate(cand, err))
        {
            err = "scene apply failed: " + err;
            return false;
        }
        return true;
    }

    /* No explicit --scene: restore the last applied scene —
       the snapshot persist_scene wrote on the previous run's
       final non-empty apply. Parse + apply go through the
       SAME path SET_SCENE uses (FromJson + ApplySceneDocument,
       so graph/LED-address validation and binding
       augmentation all run normally). A corrupt or
       unloadable snapshot degrades to the idle state with a
       logged reason — restore failure is never a startup
       failure, and no scene is ever fabricated. */
    if(!opts_.scene_restore || !profile_ || !service_)
    {
        return true;
    }
    const ProfileStore::SceneSnapshot snap =
        profile_->LoadSceneSnapshot();
    if(!snap.error.empty())
    {
        Log("last-scene restore skipped: " + snap.error);
        return true;
    }
    if(!snap.present)
    {
        return true;   /* nothing persisted — idle, as before */
    }
    const nlohmann::json j =
        nlohmann::json::parse(snap.bytes, nullptr, false);
    auto doc = std::make_shared<studio::SceneDocument>();
    std::vector<std::string> errors;
    if(j.is_discarded() ||
       !studio::FromJson(j, *doc, &errors))
    {
        std::string why = "not valid scene JSON";
        if(!errors.empty())
        {
            why = errors.front();
        }
        Log("last-scene restore skipped: " +
            profile_->SceneSnapshotPath() + ": " + why);
        return true;
    }
    if(!service_->ApplySceneDocument(doc, err))
    {
        Log("last-scene restore: apply failed (" + err +
            ") — continuing without a scene");
        err.clear();
        return true;
    }
    Log("restored last applied scene '" + doc->name + "' from " +
        profile_->SceneSnapshotPath());
    return true;
}

/*---------------------------------------------------------*\
|| Owner loop                                               ||
\*---------------------------------------------------------*/
/* R-1 OutputControl change hook. Runs on the writing thread
   (an IPC session for OUTPUT_COMMAND); serialized and always
   applying the CURRENT state, so concurrent writers converge on
   the latest value whatever order their hooks run in. The
   mapper reads the scale atomic directly — nothing to do here
   for brightness/Off on live lanes. */
void EngineHost::OnOutputChanged()
{
    std::lock_guard<std::mutex> g(output_apply_mutex_);
    const OutputState s = output_.State();

    /* Live: freeze/resume the scene clock. A release drops the
       reactive events queued across the hold — they were
       stamped on the pre-shift epoch. */
    if(engine_.Worker().SetHold(s.hold) && !s.hold && reactive_)
    {
        reactive_->Bus()->ClearEvents();
    }
    /* Uploaded loops: hold -> Paused, Off -> Stopped (they are
       never scaled by the master). */
    if(playback_)
    {
        const PlaybackController::Result r =
            playback_->SetUserOverride(s.hold, s.blackout);
        if(!r.accepted)
        {
            Log("output control: uploaded playback did not take "
                "the override (" + r.reason + ")" +
                (r.reason.find("in progress") != std::string::npos
                     ? " — retrying on the next maintenance tick"
                     : ""));
        }
    }

    /* Persist master + blackout only when they moved (a hold
       toggle never rewrites the profile, and a fresh profile
       gets no member until something non-default is set). */
    if(profile_)
    {
        const PersistedOutput p = output_.Persisted();
        std::lock_guard<std::mutex> sg(state_mutex_);
        const bool changed = engine_state_.output
                                 ? *engine_state_.output != p
                                 : p != PersistedOutput{};
        if(changed)
        {
            engine_state_.output = p;
            std::string serr;
            if(!profile_->Save(engine_state_, serr))
            {
                Log("profile save failed: " + serr);
            }
        }
    }

    if(service_)
    {
        service_->EmitOutput();
    }
}

/* Task 4.6 reactive-input change hook: the manager's
   `changed` dep — emits the IPC event and persists the
   selection. Runs on whichever thread applied the change
   (IPC session threads for set_enabled/set_params, the
   owner loop for the pause gate); the profile save is
   bounded like every other state_mutex_ holder. */
void EngineHost::OnReactiveChanged()
{
    if(service_)
    {
        service_->EmitReactive();
    }
    if(!reactive_ || !profile_)
    {
        return;
    }
    /* Persist ONLY when the recorded selection actually
       moved — pause transitions and unchanged-value param
       writes (slider debounce ticks) must not rewrite the
       profile: PersistedState doesn't record pause. */
    const std::vector<ReactiveInputs::PersistedInput>
        persisted = reactive_->PersistedState();
    std::string serr;
    std::lock_guard<std::mutex> g(state_mutex_);
    if(persisted == engine_state_.reactive)
    {
        return;
    }
    engine_state_.reactive = persisted;
    if(!profile_->Save(engine_state_, serr))
    {
        Log("profile save failed: " + serr);
    }
}

std::vector<DeviceSnapshot> EngineHost::WiredTargets()
{
    if(!sdk_)
    {
        return {};
    }
    /* Generation FIRST, then the snapshots: a re-enumeration
       between the two leaves newer snapshots under an older
       key, which simply rebuilds next call — the reverse order
       could cache old snapshots under the new generation. */
    const uint64_t        generation = sdk_->TopologyGeneration();
    const ConnectionState state      = sdk_->State();
    std::vector<DeviceKey> bound     = mapper_.BoundControllers();
    std::lock_guard<std::mutex> lock(wired_cache_mutex_);
    if(wired_cache_.valid &&
       wired_cache_.generation == generation &&
       wired_cache_.state == state &&
       wired_cache_.bound == bound)
    {
        return wired_cache_.targets;
    }
    std::vector<DeviceSnapshot> out;
    for(DeviceSnapshot& s : sdk_->Snapshots())
    {
        if(!IsWirelessGroupController(s) &&
           s.connection == ConnectionState::Connected &&
           std::find(bound.begin(), bound.end(), s.key) !=
               bound.end())
        {
            s.colors.clear();
            s.colors.shrink_to_fit();
            s.led_names.clear();
            s.led_names.shrink_to_fit();
            s.modes.clear();
            s.modes.shrink_to_fit();
            for(ZoneSnapshot& z : s.zones)
            {
                z.matrix_map.clear();
                z.matrix_map.shrink_to_fit();
            }
            out.push_back(std::move(s));
        }
    }
    wired_cache_.valid      = true;
    wired_cache_.generation = generation;
    wired_cache_.state      = state;
    wired_cache_.bound      = std::move(bound);
    wired_cache_.targets    = out;
    return out;
}

void EngineHost::PollOnce()
{
    if(ipc_server_)
    {
        /* Diff-driven push events: topology, connection,
           playback changes and the bounded-rate
           frame_status feed (Task 3.5). */
        service_->PollEvents();
    }
    if(sdk_)
    {
        sdk_->Poll();
        ReprobeSdk(Clock::now());
        /* Task 5.3 topology revalidation (option doc above):
           a bounded RefreshStatus on a slow cadence so a
           mid-session unplug/replug is detected within the
           interval instead of misrouting frames through a
           stale positional index until the socket happens
           to break. Failures mark the connection broken —
           the bounded reconnect path takes over. */
        if(sdk_->State() == ConnectionState::Connected)
        {
            const TimePoint now = Clock::now();
            if(now - last_topology_poll_ >= opts_.sdk_topology_poll)
            {
                last_topology_poll_ = now;
                sdk_->RefreshStatus();
            }
        }
        /* Mirror connection/topology into the lanes so
           validation tracks the live backend state and new
           controllers get lanes without a restart. */
        SyncLanes(*pipeline_, *sdk_);
        /* R-9: restart a dead backend / escalate a terminal
           wireless wedge (bounded; see SupervisorPolicy.h). */
        PollSupervisor();
    }

    /* Power lifecycle (Task 3.7): applies queued display/
       suspend events, diffs connection state for reconnect
       recovery, and runs the deferred wake restore — always
       BEFORE playback maintenance so the restore ordering
       (revalidate -> write) is never pre-empted by a poll-
       side convergence write. */
    /* B-1: competitors that start after the engine (G Hub at
       login, the LampArray service after a Windows Update,
       Dynamic Lighting switched back on) are re-detected. */
    if(opts_.scan_owners &&
       opts_.owner_rescan.count() > 0 &&
       Clock::now() >= next_owner_scan_)
    {
        ScanCompetitors(/*initial*/ false);
        next_owner_scan_ = Clock::now() + opts_.owner_rescan;
    }
    lifecycle_->Poll(Clock::now());
    /* Playback maintenance: acknowledgement polling, clock
       sampling, topology re-resolution. */
    playback_->Poll(Clock::now());
    /* Task 11: calibration maintenance — output-pause ends,
       inactivity timeout, topology re-validation. Runs AFTER
       playback's poll so a released hold and a revalidated
       restore never interleave inside one tick. */
    if(calibration_)
    {
        calibration_->Poll(Clock::now());
    }

    if(sdk_)
    {
        std::vector<DeviceSnapshot> snaps = sdk_->Snapshots();
        mapper_.SyncDevices(snaps);
        /* Zone-size reconciliation BEFORE the persist/upkeep
           below so both see the post-resize topology — a
           resize Enumerate()s and re-syncs the mapper in
           place. */
        ReconcileZoneSizes(snaps);
        /* Task 3.8: persist the identities bindings resolved
           to — the offline binding table survives restarts
           and only writes on change. */
        {
            std::lock_guard<std::mutex> g(state_mutex_);
            if(RecordResolvedBindings(mapper_, snaps,
                                      engine_state_))
            {
                std::string serr;
                if(!profile_->Save(engine_state_, serr))
                {
                    Log("profile save failed: " + serr);
                }
            }
        }
        for(const DeviceKey& key : mapper_.BoundControllers())
        {
            const auto snap_it =
                std::find_if(snaps.begin(), snaps.end(),
                    [&key](const DeviceSnapshot& s)
                    { return s.key == key; });
            if(snap_it == snaps.end() ||
               snap_it->connection != ConnectionState::Connected)
            {
                continue;
            }
            /* An uploaded loop owns this group — asserting
               Direct mode here would stomp it (Task 3.6:
               one producer per wireless group). A calibration
               session owns it the same way (Task 11). */
            if(playback_->OwnsGroup(key) ||
               (calibration_ && calibration_->OwnsGroup(key)))
            {
                continue;
            }
            /* Task 5.3: no upkeep writes while the power gate
               is asserted — SETCUSTOMMODE is a hardware write
               and must not cross a suspend/display-off
               boundary (it bypasses the lane gate the same
               way the controller's own freeze does — but the
               freeze is the only allowed exception, and it
               goes through the playback controller). The
               generation bookkeeping stays stale, so the
               upkeep re-applies once the gate releases. */
            if(lifecycle_ && lifecycle_->OutputPaused())
            {
                continue;
            }
            uint64_t& done = custom_mode_done_[key];
            if(done != snap_it->topology_generation)
            {
                /* E-12b: remember the hardware mode we are
                   about to replace (first observation only) —
                   the exit/idle "restore" policy returns to it. */
                RecordPriorMode(key);
                if(sdk_->SetCustomMode(key))
                {
                    done = snap_it->topology_generation;
                }
            }
        }
    }

    /* Audit E-12b: idle entry/exit (on_idle). */
    PollIdle();
}

/*---------------------------------------------------------*\
|| Audit E-12b: exit/idle output policy                      ||
\*---------------------------------------------------------*/
void EngineHost::RecordPriorMode(const DeviceKey& key)
{
    {
        std::lock_guard<std::mutex> g(output_mutex_);
        if(prior_modes_.count(key) != 0)
        {
            return;                  /* first observation wins */
        }
    }
    int32_t      index = -1;
    sdk::SdkMode mode;
    if(!sdk_ || !sdk_->ActiveMode(key, index, &mode) ||
       mode.name.empty())
    {
        return;                      /* try again next upkeep  */
    }
    PriorMode pm;
    pm.name      = mode.name;
    pm.speed     = mode.speed;
    pm.direction = mode.direction;
    pm.colors    = mode.colors;
    {
        std::lock_guard<std::mutex> g(output_mutex_);
        prior_modes_.emplace(key, std::move(pm));
    }
    Log("output policy: " + key.device + " was in mode '" +
        mode.name + "' before Direct");
}

std::map<DeviceKey, PriorMode> EngineHost::PriorModes() const
{
    std::lock_guard<std::mutex> g(output_mutex_);
    return prior_modes_;
}

OutputPolicy EngineHost::GetOutputPolicy()
{
    std::lock_guard<std::mutex> g(state_mutex_);
    return engine_state_.output_policy;
}

bool EngineHost::SetOutputPolicy(const OutputPolicy& policy,
                                 std::string& err)
{
    std::lock_guard<std::mutex> g(state_mutex_);
    if(engine_state_.output_policy == policy)
    {
        return true;
    }
    EngineState next   = engine_state_;
    next.output_policy = policy;
    if(profile_ && !profile_->Save(next, err))
    {
        return false;                  /* state stays as it was */
    }
    engine_state_ = std::move(next);
    Log(std::string("output policy set: on_exit=") +
        OutputActionName(policy.on_exit) + " on_idle=" +
        OutputActionName(policy.on_idle) + " overrides=" +
        std::to_string(policy.per_controller.size()));
    return true;
}

std::vector<OutputPolicyOutcome> EngineHost::ExitPolicyReport() const
{
    std::lock_guard<std::mutex> g(output_mutex_);
    return exit_outcomes_;
}

std::vector<DeviceSnapshot> EngineHost::OutputPolicyTargets()
{
    std::vector<DeviceSnapshot> out;
    if(!sdk_)
    {
        return out;
    }
    const std::vector<DeviceKey> bound = mapper_.BoundControllers();
    const std::map<DeviceKey, PriorMode> touched = PriorModes();
    for(DeviceSnapshot& s : sdk_->Snapshots())
    {
        if(s.connection != ConnectionState::Connected)
        {
            continue;
        }
        if(std::find(bound.begin(), bound.end(), s.key) != bound.end() ||
           touched.count(s.key) != 0)
        {
            out.push_back(std::move(s));
        }
    }
    return out;
}

OutputPolicyDeps EngineHost::BuildOutputPolicyDeps()
{
    OutputPolicyDeps d;
    d.output_paused = [this]
    { return lifecycle_ && lifecycle_->OutputPaused(); };
    d.calibration_owns = [this](const DeviceKey& k)
    { return calibration_ && calibration_->OwnsGroup(k); };
    d.playback_owns = [this](const DeviceKey& k)
    { return playback_ && playback_->OwnsGroup(k); };
    /* The existing owner write: playback's stop-black. The
       power-control entry point is used so the per-scene
       persisted selection is NOT rewritten to "stopped" — the
       next start replays the user's choice. */
    d.playback_stop_black = [this](std::string& err)
    {
        if(!playback_)
        {
            err = "no playback controller";
            return false;
        }
        const PlaybackController::Result r =
            playback_->RequestPowerControl(PlaybackControl::Stopped);
        if(!r.accepted)
        {
            err = r.reason;
        }
        return r.accepted;
    };
    d.submit_black = [this](const DeviceSnapshot& s, std::string& err)
    {
        RgbFrame f;
        f.controller          = s.key;
        f.topology_generation = s.topology_generation;
        f.sample_time         = Clock::now();
        f.synthetic           = true;
        f.colors.assign(s.LedCount(), RgbColor{ 0, 0, 0 });
        const SubmitResult r = pipeline_->SubmitReliable(f);
        if(r.status == SubmitStatus::Rejected)
        {
            err = r.reason;
            return false;
        }
        return true;
    };
    d.wait_idle = [this](const DeviceKey& k, Clock::duration t)
    { return pipeline_->WaitIdle(k, t); };
    d.select_mode = [this](const DeviceKey& k, const PriorMode& m,
                           std::string& err)
    {
        sdk::ModePatch patch;
        patch.has_speed     = true;
        patch.speed         = m.speed;
        patch.has_direction = true;
        patch.direction     = m.direction;
        patch.has_colors    = !m.colors.empty();
        patch.colors        = m.colors;
        if(!sdk_->SelectMode(k, m.name, patch))
        {
            err = sdk_->LastError();
            return false;
        }
        return true;
    };
    d.drop_pending = [this](const DeviceKey& k)
    { pipeline_->DropPending(k); };
    return d;
}

void EngineHost::LogOutcomes(const char* phase,
                             const std::vector<OutputPolicyOutcome>& out)
{
    for(const OutputPolicyOutcome& o : out)
    {
        Log(std::string("output policy (") + phase + "): " +
            o.key.device + " " + OutputActionName(o.policy) + " -> " +
            o.outcome + (o.detail.empty() ? "" : " (" + o.detail + ")"));
    }
}

/* The "output-policy" shutdown step: frame production is
   stopped (scene worker + reactive inputs), the lanes and the
   transport are still up. */
void EngineHost::RunExitPolicy()
{
    if(!sdk_ || !pipeline_)
    {
        return;
    }
    const OutputPolicy policy = GetOutputPolicy();
    const std::vector<DeviceSnapshot> targets = OutputPolicyTargets();
    std::vector<OutputPolicyOutcome> out;
    {
        std::map<DeviceKey, PriorMode> priors = PriorModes();
        out = RunOutputPolicy(targets, OutputPhase::Exit, policy,
                              priors, BuildOutputPolicyDeps());
    }
    LogOutcomes("exit", out);
    std::lock_guard<std::mutex> g(output_mutex_);
    exit_outcomes_ = std::move(out);
}

nlohmann::json EngineHost::OutputPolicyJson()
{
    nlohmann::json j = OutputPolicyToJson(GetOutputPolicy());
    j["idle"] = OutputIdle();
    return j;
}

std::vector<OutputPolicyOutcome> EngineHost::IdleReport() const
{
    std::lock_guard<std::mutex> g(output_mutex_);
    return idle_outcomes_;
}

/* Idle = no scene, or one with no objects, applied for
   output_idle_after (boot with nothing to adopt, or a scene
   replaced by an empty one — there is no workspace close
   path). on_idle runs once per idle entry; the next applied
   scene ends it. Display-off / suspend is not idle: under the
   power gate the entry waits (PowerLifecycle owns that). */
void EngineHost::PollIdle()
{
    if(!service_)
    {
        return;
    }
    const TimePoint now = opts_.output_clock ? opts_.output_clock()
                                             : Clock::now();
    const std::shared_ptr<const studio::SceneDocument> doc =
        service_->Scene();
    if(doc && !doc->objects.empty())
    {
        idle_tracking_ = false;
        if(output_idle_.exchange(false))
        {
            Log("output policy: idle ended (scene applied)");
        }
        return;
    }
    if(!idle_tracking_)
    {
        idle_tracking_ = true;
        idle_since_    = now;
        return;
    }
    if(output_idle_.load() || now - idle_since_ < opts_.output_idle_after)
    {
        return;
    }
    if(lifecycle_ && lifecycle_->OutputPaused())
    {
        return;                        /* deferred, not consumed */
    }
    output_idle_.store(true);
    std::vector<OutputPolicyOutcome> out;
    if(sdk_ && pipeline_)
    {
        const OutputPolicy policy = GetOutputPolicy();
        out = RunOutputPolicy(OutputPolicyTargets(), OutputPhase::Idle,
                              policy, PriorModes(),
                              BuildOutputPolicyDeps());
    }
    for(const OutputPolicyOutcome& o : out)
    {
        /* A restored device left Direct: the upkeep must
           re-assert it when the next scene binds it. */
        if(o.outcome == "restored")
        {
            custom_mode_done_.erase(o.key);
        }
    }
    Log("output policy: idle entered (" + std::to_string(out.size()) +
        " controller(s))");
    LogOutcomes("idle", out);
    std::lock_guard<std::mutex> g(output_mutex_);
    idle_outcomes_ = std::move(out);
}

/* Zone-size reconciliation. The scene document's declared
   zone_leds is the INTENT — backends that lose persisted
   zone sizes (the managed headless --localconfig server
   never writes its controllers section) enumerate resizable
   zones at 0 and the binding silently writes nothing.
   RESIZEZONE is bounded: at most kZoneResizeMaxTries per
   observed count per binding, so a backend that clamps to a
   different size is not poked forever; a count change
   (replug, another owner) re-arms the allowance. */
void EngineHost::ReconcileZoneSizes(
    std::vector<DeviceSnapshot>& snaps)
{
    static constexpr uint32_t kZoneResizeMaxTries = 3;
    const std::shared_ptr<const studio::SceneDocument> doc =
        mapper_.Scene();
    if(!doc)
    {
        return;
    }
    bool resized = false;
    for(const studio::DeviceBinding& b : doc->bindings)
    {
        /* zone_leds 0 = unchecked — the binding doesn't pin a
           size. Zoneless bindings carry no target either. */
        if(b.zone_name.empty() || b.zone_leds == 0)
        {
            continue;
        }
        ZonePlacement zp;
        if(!mapper_.BoundZone(b.id, zp) ||
           zp.led_count == b.zone_leds)
        {
            continue;
        }
        const auto snap_it =
            std::find_if(snaps.begin(), snaps.end(),
                [&zp](const DeviceSnapshot& s)
                { return s.key == zp.controller; });
        if(snap_it == snaps.end() ||
           snap_it->connection != ConnectionState::Connected ||
           zp.zone_index >= snap_it->zones.size())
        {
            continue;
        }
        const ZoneSnapshot& zs = snap_it->zones[zp.zone_index];
        /* 0/0 bounds = unknown -> not resizable; fixed-size
           zones (min == max) can never reach the declared
           count anyway. */
        if(zs.leds_max == 0 ||
           b.zone_leds < zs.leds_min || b.zone_leds > zs.leds_max)
        {
            continue;
        }
        /* Same gate as the custom-mode upkeep below: RESIZEZONE
           is a hardware write and must not cross a suspend /
           display-off boundary. */
        if(lifecycle_ && lifecycle_->OutputPaused())
        {
            continue;
        }
        ZoneResizeAttempts& att = zone_resize_tries_[b.id];
        if(att.observed != zp.led_count)
        {
            att.observed = zp.led_count;
            att.tries    = 0;
        }
        if(att.tries >= kZoneResizeMaxTries)
        {
            continue;
        }
        ++att.tries;
        if(sdk_->ResizeZone(zp.controller, b.zone_name,
                            (int32_t)b.zone_leds))
        {
            resized = true;
            Log("zone '" + b.zone_name + "' on " +
                snap_it->name + " was " +
                std::to_string(zp.led_count) + " LEDs — resized to " +
                std::to_string(b.zone_leds) +
                " (scene binding " + b.id + ")");
        }
    }
    if(resized)
    {
        /* RESIZEZONE changed the topology and bumped the
           generation; Enumerate refreshes zones + colors now
           so the mapper, lanes, and the upkeep below all see
           the new sizes this same poll. */
        sdk_->Enumerate();
        snaps = sdk_->Snapshots();
        mapper_.SyncDevices(snaps);
    }
}

/* Task 5.3: the bounded backoff is deliberate, but latching
   Disconnected FOREVER after max_attempts is not — a backend
   outage longer than the backoff window (a machine boot,
   a slow manual restart) left the engine permanently
   detached: nothing called ReconnectNow() after Start().
   Poll-side re-arm at a bounded rate recovers honestly. */
void EngineHost::ReprobeSdk(TimePoint now)
{
    if(sdk_->State() != ConnectionState::Disconnected)
    {
        return;
    }
    if(now - last_reprobe_ < opts_.sdk_reprobe)
    {
        return;
    }
    last_reprobe_ = now;
    Log("backend absent — re-arming the bounded SDK reconnect "
        "(client latched Disconnected after exhausting "
        "backoff)");
    sdk_->ReconnectNow();
}

/* The single cheap "is anyone subscribed to `kind`" check every
   host-side event producer (frame_status here; preview bodies are
   built inside EngineService) goes through, so a body nobody
   subscribed to is never built. */
bool EngineHost::EventInterest(const char* kind) const
{
    return ipc_server_ && ipc_server_->HasSubscriber(kind);
}

void EngineHost::Run()
{
    /* frame_status rides a fast tick beside the ~500 ms
       maintenance loop — the UI preview repaints on that
       feed, so it cannot wait for PollOnce's cadence. The
       push rate stays bounded by frame_status_min_interval_ms;
       the tick only decides how soon a due push is noticed. */
    const TimePoint anchor = Clock::now();
    TimePoint next_maint = anchor;
    while(!stop_.load())
    {
        const TimePoint now = Clock::now();
        if(now >= next_maint)
        {
            PollOnce();
            next_maint = now + std::chrono::milliseconds(500);
        }
        if(service_)
        {
            const TimePoint tick = Clock::now();
            /* frame_status body only when someone listens;
               draft previews always ride the fast tick — a
               client subscribed to "preview" alone must not
               fall back to the maintenance cadence. */
            if(EventInterest("frame_status"))
            {
                service_->PollFrameStatus(tick);
            }
            service_->PollPreviews(tick);
        }
        /* Sleep to the next due time, not a fixed relative
           interval (audit E-2): a slow iteration no longer
           pushes every later tick back. */
        std::this_thread::sleep_until(
            NextOwnerWake(anchor, Clock::now(), next_maint));
    }
}

TimePoint EngineHost::NextOwnerWake(TimePoint anchor, TimePoint now,
                                    TimePoint next_maint)
{
    TimePoint slot = anchor + kOwnerTick;
    if(now >= anchor)
    {
        /* First slot strictly after now; integer slot index, so
           the grid never drifts. */
        slot = anchor + ((now - anchor) / kOwnerTick + 1) * kOwnerTick;
    }
    return next_maint < slot ? next_maint : slot;
}

/*---------------------------------------------------------*\
|| Ordered, bounded shutdown                                ||
\*---------------------------------------------------------*/
const OrderlyShutdown::Result& EngineHost::Stop()
{
    if(stopped_)
    {
        return shutdown_result_;
    }
    stopped_ = true;
    /* A partial Start() (it returned false after spawning some
       threads) still runs the full sequence — every step is a
       no-op for a piece that never came up. */
    if(!start_attempted_)
    {
        return shutdown_result_;
    }

    /* Event intake and command intake stop first, then frame
       production, then the delivery lanes (each join bounded
       by the transport's send timeout), then the transport
       itself. Steps run in registration order and each is
       measured against its declared bound — an overrun is
       reported honestly, never silently. */
    OrderlyShutdown shutdown;
    shutdown.Add("power-monitor",
                 std::chrono::milliseconds(2000),
                 [this] { power_monitor_.Stop(); });
    shutdown.Add("ipc-listeners",
                 std::chrono::milliseconds(2000),
                 [this]
                 {
                     if(cmd_listener_)  cmd_listener_->Stop();
                     if(diag_listener_) diag_listener_->Stop();
                 });
    shutdown.Add("ipc-server",
                 std::chrono::milliseconds(2000),
                 [this]
                 { if(ipc_server_) ipc_server_->Stop(); });
    /* Calibration worker joins once no IPC verb can reach it
       and BEFORE frame production / the delivery lanes / the
       transport stop: its Stop() releases a live session's lane
       suppression and playback hold through pipeline_ and
       playback_, and its worker's change hook calls into
       service_ — which destructs before calibration_ (member
       order), so the worker must be gone by then. */
    shutdown.Add("calibration",
                 std::chrono::milliseconds(2000),
                 [this]
                 { if(calibration_) calibration_->Stop(); });
    /* Workspace job worker joins BEFORE frame production and
       the IPC service stop — an in-flight commit's apply_scene
       seam reaches service_ + engine_, so no workspace job may
       still be running when those destruct. */
    shutdown.Add("workspace-jobs",
                 std::chrono::milliseconds(2000),
                 [this]
                 { if(workspaces_) workspaces_->Shutdown(); });
    /* No apply can arrive any more (IPC and workspace jobs are
       down): land the last queued scene snapshot now, inside
       a measured step, rather than implicitly in the service
       destructor (audit I-6 moved the write off the request
       thread). */
    shutdown.Add("scene-snapshot",
                 std::chrono::milliseconds(2000),
                 [this]
                 { if(service_) service_->FlushPersistedScene(); });
    shutdown.Add("scene-worker",
                 std::chrono::milliseconds(2000),
                 [this] { engine_.Stop(); });
    /* Reactive capture sources (Task 4.6): the worker stopped
       first, so sources only feed a dead bus — each source's
       join is bounded by its own thread's wake latency
       (audio ~5 ms chunks, screen ~40-50 ms, key hook's
       WM_QUIT wait bounded by its publish loop). */
    shutdown.Add("reactive-inputs",
                 std::chrono::milliseconds(3000),
                 [this] { if(reactive_) reactive_->Shutdown(); });
    /* Audit E-12b: the exit output policy (hold / black /
       restore per controller). Frame production is stopped;
       the lanes and the transport are still up, so a black
       frame rides the reliable lane slot and a restore goes
       to the transport once its lane is idle. Skipped under
       the power gate. 500 ms per lane wait, lanes draining in
       parallel. */
    shutdown.Add("output-policy",
                 std::chrono::milliseconds(2000),
                 [this] { RunExitPolicy(); });
    /* One in-flight transport send per lane plus join time —
       bounded by the transport's own send timeout (4 s
       default); lanes join sequentially. */
    shutdown.Add("delivery-pipeline",
                 std::chrono::milliseconds(12000),
                 [this] { if(pipeline_) pipeline_->Stop(); });
    shutdown.Add("sdk-transport",
                 std::chrono::milliseconds(1000),
                 [this] { if(sdk_) sdk_->Disconnect(); });
    /* Quit-all backend handoff — last, AFTER the transport
       drains (the same ordering stop-openrgb2.ps1 enforces:
       engine exits before the backend closes). Verified-own
       only and graceful: the backend's shutdown event (or
       WM_CLOSE for a windowed build) lets it persist its own
       config; a missed bound is reported, never escalated to
       a kill. */
    if(pending_backend_pid_.load() != 0)
    {
        shutdown.Add("backend-handoff",
                     std::chrono::milliseconds(12000),
                     [this] { SignalPendingBackend(); });
    }

    /* E-10: the bounds above are enforced, not only measured.
       A step that throws is caught inside Run() (later owners
       still stop); a step that never returns trips the
       watchdog once the whole sequence outlives its total
       bound. */
    if(opts_.shutdown_watchdog)
    {
        shutdown.SetWatchdog(opts_.shutdown_watchdog_grace,
            [this](const std::string& step)
            {
                Log("shutdown watchdog: step '" + step +
                    "' still running past the total bound");
                opts_.shutdown_watchdog(step);
            });
    }
    shutdown_result_ = shutdown.Run();
    for(const OrderlyShutdown::StepResult& s : shutdown_result_.steps)
    {
        if(s.failed)
        {
            Log("shutdown step '" + s.name + "' threw: " + s.error +
                " (later steps still ran)");
        }
    }
    /* Every worker is joined: give the timer resolution back
       (E-2). */
    timer_resolution_.Release();
    return shutdown_result_;
}

/*---------------------------------------------------------*\
|| R-9 backend supervisor                                    ||
\*---------------------------------------------------------*/
TimePoint EngineHost::SupervisorNow() const
{
    return opts_.supervisor_clock ? opts_.supervisor_clock()
                                  : Clock::now();
}

BackendProbe EngineHost::ProbeBackend() const
{
    if(opts_.backend_probe)
    {
        return opts_.backend_probe();
    }
#ifdef _WIN32
    return LiveBackendProbe(opts_.backend_exe, opts_.sdk_port);
#else
    /* Nothing can be verified: report a foreign holder so the
       policy never spawns. */
    BackendProbe p;
    p.port = PortHolder::Unverifiable;
    return p;
#endif
}

/* stop-openrgb2.ps1 writes state\openrgb2.stopping before it
   touches anything and removes it in `finally`. A marker
   older than 10 minutes is a stop that died mid-way — stale,
   ignored, so a crash afterwards is still recovered. */
bool EngineHost::StopMarkerFresh() const
{
    std::filesystem::path p;
    if(!opts_.stop_marker_path.empty())
    {
        p = std::filesystem::u8path(opts_.stop_marker_path);
    }
    else if(!opts_.backend_config_dir.empty())
    {
        p = std::filesystem::u8path(opts_.backend_config_dir)
                .parent_path() / "openrgb2.stopping";
    }
    else
    {
        return false;
    }
    std::error_code ec;
    const auto when = std::filesystem::last_write_time(p, ec);
    if(ec)
    {
        return false;
    }
    const auto age = std::filesystem::file_time_type::clock::now() - when;
    return age < std::chrono::minutes(10);
}

void EngineHost::PollSupervisor()
{
    if(!supervisor_ || !sdk_)
    {
        return;
    }
    const TimePoint now = SupervisorNow();
    SupervisorObservation obs;
    obs.now            = now;
    obs.link_connected = sdk_->State() == ConnectionState::Connected;
    obs.stopping       = stop_.load() || StopMarkerFresh();
    {
        /* The wedge rule only trusts a live status: a reply
           older than 30 s (wall clock of the SDK client) is
           no evidence the group is STILL wedged. */
        sdk::WirelessStatusSnapshot ws;
        sdk_->WirelessStatus(ws);
        obs.wireless_valid = ws.have_data && ws.supported &&
            Clock::now() - ws.fetched_at < std::chrono::seconds(30);
        if(obs.wireless_valid)
        {
            for(const sdk::SdkWirelessGroupStatus& g : ws.status.groups)
            {
                obs.groups.push_back({ g.mac, g.state, g.wedged });
            }
        }
    }
    SupervisorDecision d;
    {
        std::lock_guard<std::mutex> g(supervisor_mutex_);
        d = supervisor_->Decide(obs, [this] { return ProbeBackend(); });
    }
    switch(d.action)
    {
    case SupervisorAction::None:
        if(d.notice)
        {
            RecordSupervisorAction("notice", d, 0);
        }
        break;
    case SupervisorAction::Spawn:
    {
        BackendLaunch l;
        l.exe  = opts_.backend_exe;
        l.args = BackendServerArgs(opts_.sdk_port);
        l.cwd  = opts_.backend_config_dir;
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::u8path(l.cwd), ec);
        uint32_t    pid = 0;
        std::string err;
        bool ok = false;
        if(opts_.backend_launcher)
        {
            ok = opts_.backend_launcher(l, pid, err);
        }
        else
        {
#ifdef _WIN32
            ok = LaunchBackendProcess(l, pid, err);
#else
            err = "backend spawn is Windows-only";
#endif
        }
        {
            std::lock_guard<std::mutex> g(supervisor_mutex_);
            supervisor_->RecordSpawn(now, ok);
        }
        if(ok)
        {
            RecordSupervisorAction("backend_spawned", d, pid);
            /* Re-arm the SDK reconnect now instead of waiting out
               the latched backoff / the 30 s re-probe. */
            sdk_->ReconnectNow();
        }
        else
        {
            d.detail += " -- launch failed: " + err;
            RecordSupervisorAction("backend_spawn_failed", d, 0);
        }
        break;
    }
    case SupervisorAction::CloseWedged:
    {
        std::string err;
        bool ok = false;
        if(opts_.backend_closer)
        {
            ok = opts_.backend_closer(d.pid, err);
        }
        else
        {
#ifdef _WIN32
            ok = CloseOwnBackendNoWait(d.pid, opts_.backend_exe, err);
#else
            err = "backend close is Windows-only";
#endif
        }
        /* Recorded either way: a close that keeps failing
           spends the wedge budget instead of retrying every
           tick. */
        {
            std::lock_guard<std::mutex> g(supervisor_mutex_);
            supervisor_->RecordWedgeClose(now);
        }
        if(ok)
        {
            RecordSupervisorAction("backend_closed_for_wedge", d, d.pid);
        }
        else
        {
            d.detail += " -- close failed: " + err;
            RecordSupervisorAction("backend_close_failed", d, d.pid);
        }
        break;
    }
    }
}

void EngineHost::RecordSupervisorAction(const std::string& action,
                                        const SupervisorDecision& d,
                                        uint32_t pid)
{
    const std::string at = UtcNowIso();
    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> g(supervisor_mutex_);
        seq = ++supervisor_seq_;
        supervisor_history_.push_back({ at, action, d.reason, d.detail,
                                        pid });
        while(supervisor_history_.size() > 16)
        {
            supervisor_history_.pop_front();
        }
        if(action != "notice")
        {
            supervisor_last_at_ = at;
        }
    }
    Log("supervisor: " + action + " (" + d.reason + ")" +
        (pid != 0 ? " pid " + std::to_string(pid) : std::string()) +
        " -- " + d.detail);
    if(service_)
    {
        service_->EmitSupervisor(nlohmann::json::object({
            { "action", action   },
            { "reason", d.reason },
            { "detail", d.detail },
            { "at",     at       },
            { "seq",    seq      },
            { "status", SupervisorJson() } }));
    }
}

nlohmann::json EngineHost::SupervisorJson() const
{
    nlohmann::json j = nlohmann::json::object();
    j["enabled"] = supervisor_ != nullptr;
    if(!supervisor_)
    {
        j["backend_restarts"] = 0;
        j["wedge_restarts"]   = 0;
        j["last_reason"]      = nullptr;
        j["last_at"]          = nullptr;
        j["budget_exhausted"] = false;
        j["seq"]              = 0;
        j["history"]          = nlohmann::json::array();
        return j;
    }
    const TimePoint now = SupervisorNow();
    std::lock_guard<std::mutex> g(supervisor_mutex_);
    const std::string reason = supervisor_->LastReason();
    j["backend_restarts"] = supervisor_->BackendRestarts();
    j["wedge_restarts"]   = supervisor_->WedgeRestarts();
    j["last_reason"]      = reason.empty() ? nlohmann::json(nullptr)
                                           : nlohmann::json(reason);
    j["last_at"]          = supervisor_last_at_.empty()
                                ? nlohmann::json(nullptr)
                                : nlohmann::json(supervisor_last_at_);
    j["budget_exhausted"] = supervisor_->SpawnBudgetExhausted(now) ||
                            supervisor_->WedgeBudgetExhausted(now);
    j["seq"]              = supervisor_seq_;
    nlohmann::json hist = nlohmann::json::array();
    for(const SupervisorEntry& e : supervisor_history_)
    {
        hist.push_back(nlohmann::json::object({
            { "at",     e.at     },
            { "action", e.action },
            { "reason", e.reason },
            { "detail", e.detail },
            { "pid",    e.pid    } }));
    }
    j["history"] = std::move(hist);
    return j;
}

nlohmann::json EngineHost::ArmBackendHandoff(bool stop_backend)
{
    if(!stop_backend)
    {
        return nlohmann::json::object();
    }
    if(opts_.backend_exe.empty())
    {
        Log("quit-all: backend teardown requested but "
            "no --backend-exe was declared — nothing "
            "can be verified as ours; left running");
        return nlohmann::json::object({
            { "backend", "not_configured" } });
    }
#ifdef _WIN32
    const BackendResolution r =
        ResolveManagedBackend(opts_.backend_exe, opts_.sdk_port);
    if(r.pid == 0)
    {
        Log(std::string("quit-all: no verifiably-"
            "managed backend to close (") +
            r.decision + ")");
        return nlohmann::json::object({
            { "backend", r.decision } });
    }
    pending_backend_pid_.store(r.pid);
    return nlohmann::json::object({
        { "backend",     r.decision },
        { "backend_pid", r.pid      } });
#else
    return nlohmann::json::object({
        { "backend", "unverifiable" } });
#endif
}

void EngineHost::SignalPendingBackend()
{
#ifdef _WIN32
    const uint32_t pid = pending_backend_pid_.load();
    if(pid == 0 || opts_.backend_exe.empty())
    {
        return;
    }
    /* Re-verify at signal time: the pid was resolved when the
       request landed — a recycle in between must never inherit
       the close. */
    if(!IsManagedBackendImage(pid,
           NormalizeImagePath(opts_.backend_exe)))
    {
        Log("backend-handoff: pid " + std::to_string(pid) +
            " no longer resolves to " + opts_.backend_exe +
            " — left alone");
        return;
    }
    /* Preferred channel: the vendored backend's PID-scoped
       session event (main_Windows.cpp WaitWhileServerOnline).
       The headless --server process owns no windows, so
       WM_CLOSE cannot reach it; the event takes the same
       graceful StopServer() path as an SCM stop. CreateEvent
       (not OpenEvent) latches the signal for a backend that is
       still starting and has not created its handle yet. */
    const std::wstring ev_name =
        L"Local\\OpenRGB2-Backend-Shutdown-" + std::to_wstring(pid);
    /* Held open until after the wait: if the backend is still
       starting and has not reached its own CreateEvent yet, our
       outstanding handle keeps the named object alive — its first
       poll then observes the already-signaled state. */
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, ev_name.c_str());
    const bool ev_signaled = (ev != nullptr);
    if(ev_signaled)
    {
        SetEvent(ev);
        Log("backend-handoff: signaled shutdown event for backend "
            "(pid " + std::to_string(pid) + ")");
    }
    const size_t posted = PostCloseToProcessWindows(pid);
    if(posted > 0)
    {
        Log("backend-handoff: posted WM_CLOSE to " +
            std::to_string(posted) + " window(s) of backend (pid " +
            std::to_string(pid) + ")");
    }
    if(!ev_signaled && posted == 0)
    {
        Log("backend-handoff: backend (pid " +
            std::to_string(pid) + ") exposes no shutdown event and "
            "owns no top-level windows — unreachable; finish with "
            "scripts\\stop-openrgb2.ps1 -Force");
        return;
    }
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if(h == nullptr)
    {
        Log("backend-handoff: backend (pid " +
            std::to_string(pid) + ") already exited");
        if(ev) { CloseHandle(ev); }
        return;
    }
    const bool exited =
        WaitForSingleObject(h, 8000) == WAIT_OBJECT_0;
    CloseHandle(h);
    if(ev) { CloseHandle(ev); }
    if(exited)
    {
        Log("backend-handoff: backend (pid " +
            std::to_string(pid) + ") exited on shutdown request");
    }
    else
    {
        Log("backend-handoff: backend (pid " +
            std::to_string(pid) + ") still running after the bound "
            "— finish with scripts\\stop-openrgb2.ps1 -Force if needed");
    }
#endif
}

} /* namespace openrgb2 */
