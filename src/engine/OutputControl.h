/*---------------------------------------------------------*\
|| OutputControl.h                                           ||
||                                                           ||
||   R-1: runtime output overrides that live OUTSIDE any    ||
||   document — the tray's master brightness, "Off"         ||
||   (blackout) and pause (hold). A tray click must change  ||
||   the LEDs within one tick and must never dirty the open ||
||   workspace draft, so none of this is a scene field.     ||
||                                                           ||
||   Arbitration is decided once, here:                     ||
||     effective = blackout ? 0                             ||
||                          : master * min(contributors)    ||
||   Contributors are named runtime slots ("schedule",      ||
||   "lock", "idle" — R-4); none is set by R-1 itself.      ||
||   `hold` never scales: it freezes the scene clock        ||
||   (SceneWorker::SetHold) or pauses an uploaded loop      ||
||   (PlaybackController::SetUserOverride).                 ||
||                                                           ||
||   Persistence: master + blackout survive an engine       ||
||   restart (EngineState.output); hold and contributors    ||
||   do not — a restart resumes.                            ||
||                                                           ||
||   Threading: every setter is safe from any thread. The   ||
||   effective scale is ALSO published through one          ||
||   std::atomic<float> the device mapper reads lock-free   ||
||   on the worker thread. The changed callback runs on the ||
||   writing thread WITHOUT the lock held, after the state  ||
||   moved — it may read the control back.                  ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace openrgb2
{

/* What EngineState.output records. */
struct PersistedOutput
{
    float master   = 1.0f;
    bool  blackout = false;

    bool operator==(const PersistedOutput& o) const
    {
        return master == o.master && blackout == o.blackout;
    }
    bool operator!=(const PersistedOutput& o) const
    {
        return !(*this == o);
    }
};

/* One consistent read of the control. */
struct OutputState
{
    float                        master    = 1.0f;
    bool                         blackout  = false;
    bool                         hold      = false;
    float                        effective = 1.0f;
    std::map<std::string, float> contributors;
};

class OutputControl
{
public:
    using ChangedFn = std::function<void(const OutputState&)>;

    OutputControl() = default;

    OutputControl(const OutputControl&)            = delete;
    OutputControl& operator=(const OutputControl&) = delete;

    /* Invoked after every change (never for a no-op write). */
    void SetChanged(ChangedFn fn);

    /* Each setter returns true when the state moved. Values
       are clamped into [0, 1]; a non-finite value is refused
       (false, nothing changes). */
    bool SetMaster(float value);
    bool SetBlackout(bool on);
    bool SetHold(bool on);
    /* Named contributor slot (R-4). An empty name is refused. */
    bool SetContributor(const std::string& name, float scale);
    bool ClearContributor(const std::string& name);

    /* Restart path: master + blackout come back, hold stays
       off. Fires the changed callback when the state moved. */
    void Restore(const PersistedOutput& p);

    OutputState     State() const;
    PersistedOutput Persisted() const;

    /* Lock-free read of the arbitrated scale. */
    float EffectiveScale() const { return effective_.load(); }
    /* The same atomic, for consumers that hold a pointer to it
       (DeviceMapper::SetOutputScaleSource). Lives as long as
       this object. */
    const std::atomic<float>& EffectiveScaleSource() const
    {
        return effective_;
    }

    /* The arbitration rule on its own. */
    static float Arbitrate(float master, bool blackout,
                           const std::map<std::string, float>& contributors);

private:
    /* Under mutex_: recompute + publish effective_, snapshot. */
    OutputState CommitLocked();
    void        Notify(const OutputState& s);

    mutable std::mutex           mutex_;
    float                        master_   = 1.0f;
    bool                         blackout_ = false;
    bool                         hold_     = false;
    std::map<std::string, float> contributors_;
    std::atomic<float>           effective_{ 1.0f };
    /* Guarded by its own mutex so a callback swap never races a
       notify (copied out, then invoked unlocked). */
    mutable std::mutex           cb_mutex_;
    ChangedFn                    changed_;
};

} /* namespace openrgb2 */
