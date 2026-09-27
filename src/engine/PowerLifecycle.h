/*---------------------------------------------------------*\
||| PowerLifecycle.h                                          ||
|||                                                           ||
|||   The engine's power/lifecycle state machine (plan     ||
|||   section 3.4 lifecycle + section 6 constraints —     ||
|||   Task 3.7). Platform-independent: power events arrive ||
|||   through PostEvent() from a small injectable source — ||
|||   production wires the Win32 monitor in               ||
|||   src/platform/windows/WinPowerMonitor, tests inject    ||
|||   events directly. All hardware-facing operations are   ||
|||   injected deps (delivery pipeline + playback           ||
|||   controller + snapshot provider), so the whole         ||
|||   machine runs offline and deterministic.               ||
|||                                                           ||
|||   Contract:                                            ||
|||   - DisplayOff: gate ALL RGB frame output and reactive  ||
|||     capture (pipeline rejects with reason, pending      ||
|||     frames are discarded), then freeze an active        ||
|||     uploaded loop ONCE via an explicit sampled-hold     ||
|||     write. No periodic writes exist that could wake a   ||
|||     sleeping display.                                   ||
|||   - Suspend: discard pending frames; the controller is  ||
|||     parked (power gate) so nothing writes across the    ||
|||     sleep boundary. We never promise a loop stops —     ||
|||     if power/communication disappears before a          ||
|||     replacement reaches the device, the loop keeps      ||
|||     whatever it has.                                    ||
|||   - Resume / reconnect: revalidate identity +           ||
|||     backend-reported state BEFORE any restore write     ||
|||     (PlaybackController::RevalidateAndRestore); a       ||
|||     still-consistent owned group gets no write; live    ||
|||     lanes drop stale pending, reset delivered state,    ||
|||     and re-assert the last delivered frame through the  ||
|||     reliable convergence slot (a same-generation        ||
|||     reconnect's delivered memory can otherwise eat the  ||
|||     first post-reconnect frame — deferred 3.3 note).    ||
|||   - "Stop maintenance traffic" is NEVER an animation-   ||
|||     stop mechanism: the engine carries no device        ||
|||     maintenance channel at all, and the API refuses     ||
|||     the request explicitly (test item, plan 3.4).       ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#pragma once

#include "EngineTypes.h"
#include "PlaybackController.h"

#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace openrgb2
{

class PowerLifecycle
{
public:
    /* The small injectable event surface — the Win32 monitor
       (src/platform/windows/WinPowerMonitor) is the production
       source; tests post events directly. */
    enum class Event
    {
        DisplayOff,
        DisplayOn,
        Suspend,
        Resume,
    };

    struct Deps
    {
        /* All backend/pipeline/scene operations are injected.
           An empty dep means "not available" — the machine
           degrades honestly (e.g. no snapshots -> no reconnect
           diffing) rather than guessing. */

        /* The delivery pipeline's global output gate + lane
           primitives. */
        std::function<void(bool, const std::string&)>
                                                set_output_paused;
        std::function<std::vector<DeviceKey>()> lane_keys;
        std::function<size_t(const DeviceKey&)> drop_pending;
        std::function<void(const DeviceKey&)>   reset_delivered;
        std::function<bool(const DeviceKey&, RgbFrame&)>
                                                last_delivered;
        std::function<SubmitResult(const RgbFrame&)>
                                                reliable_submit;

        /* Reactive-capture pause hook (screen/audio/key
           samplers read it — platform inputs are a later
           milestone's producers). */
        std::function<void(bool)>               set_capture_paused;

        /* Current controller snapshots — reconnect diffing +
           restore gating. */
        std::function<std::vector<DeviceSnapshot>()>
                                                snapshots;

        /* The playback state machine (nullable — a bare engine
           still gets gate/discard behavior). */
        PlaybackController*                     playback = nullptr;

        /* Diagnostic note sink (stderr in production). */
        std::function<void(const std::string&)> log;
        std::function<TimePoint()>              now;
    };

    struct Config
    {
        /* Bounded pending-event queue — power events are rare;
           a storm drops the OLDEST entries because handlers are
           state-based and converge on the newest state. */
        size_t max_pending_events = 16;
    };

    explicit PowerLifecycle(Deps deps, Config cfg = Config{});

    PowerLifecycle(const PowerLifecycle&)            = delete;
    PowerLifecycle& operator=(const PowerLifecycle&) = delete;

    /* Event intake — thread-safe (the monitor's window thread
       calls this). Events are queued and applied by Poll() on
       the engine's owner loop: freeze/restore transitions do
       blocking transport I/O and must never run inside a
       window proc. */
    void PostEvent(Event e);

    /* Owner-loop hook (~500 ms): applies queued events in order,
       diffs snapshot connection state for reconnect recovery,
       and attempts any deferred wake restore. */
    void Poll(TimePoint now);

    bool        DisplayOn()      const;
    bool        Suspended()      const;   /* suspend seen, not yet resumed */
    bool        OutputPaused()   const;   /* the RGB gate is asserted      */
    bool        PowerHold()      const;   /* we froze the loop for power   */
    bool        RestorePending() const;   /* wake restore outstanding      */
    std::string Detail()         const;   /* last failure/defer note       */

    /* Hard rule (plan section 3.4 — test item): stopping the
       device's required maintenance traffic is NEVER a valid
       way to stop an animation. The engine owns no maintenance
       channel (the backend driver holds heartbeats — cooling-
       adjacent housekeeping must never be a casualty), so this
       is the only place such a request could be expressed, and
       it is refused unconditionally. */
    static bool StopAnimationByMaintenanceStop() { return false; }

private:
    void ApplyEvent(Event e);
    bool WantGate() const;
    void ApplyGateAndCapture();
    void DropAllLanes();
    void DiffConnections();
    void RecoverLiveLane(const DeviceSnapshot& snap);
    void AttemptRestore();

    Deps   deps_;
    Config cfg_;

    mutable std::mutex      mutex_;
    std::deque<Event>       pending_;

    bool        display_on_       = true;
    bool        suspended_        = false;
    bool        gated_            = false;
    bool        capture_paused_   = false;
    bool        power_hold_       = false;  /* we froze the loop  */
    bool        restore_pending_  = false;
    int         restore_attempts_ = 0;
    std::string detail_;

    /* Reconnect diffing: last-seen connection per key. The
       first Poll seeds the baseline silently — existing
       connections are not "reconnects". */
    bool                            conn_seeded_ = false;
    std::map<DeviceKey, ConnectionState> conn_;
};

} /* namespace openrgb2 */
