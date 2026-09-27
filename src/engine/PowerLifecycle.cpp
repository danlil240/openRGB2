/*---------------------------------------------------------*\
||| PowerLifecycle.cpp                                        ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#include "PowerLifecycle.h"

#include <utility>

namespace openrgb2
{

namespace
{
/* Bounded wake-restore attempts — a restore that keeps failing
   (e.g. the backend is gone) must not retry forever (plan
   section 6: bound retries). */
constexpr int kMaxRestoreAttempts = 4;
}

PowerLifecycle::PowerLifecycle(Deps deps, Config cfg)
    : deps_(std::move(deps)), cfg_(cfg)
{
    if(!deps_.now)
    {
        deps_.now = [] { return Clock::now(); };
    }
}

void PowerLifecycle::PostEvent(Event e)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if(pending_.size() >= cfg_.max_pending_events)
    {
        /* Coalesce a storm: handlers are state-based and converge
           on the newest state, so the oldest entry is the safe
           drop (bounded queue — plan section 6). */
        pending_.pop_front();
    }
    pending_.push_back(e);
}

bool PowerLifecycle::DisplayOn() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return display_on_;
}

bool PowerLifecycle::Suspended() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return suspended_;
}

bool PowerLifecycle::OutputPaused() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return gated_;
}

bool PowerLifecycle::PowerHold() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return power_hold_;
}

bool PowerLifecycle::RestorePending() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return restore_pending_;
}

std::string PowerLifecycle::Detail() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return detail_;
}

/*---------------------------------------------------------*\
||| Gate / helpers                                           ||
\*---------------------------------------------------------*/

bool PowerLifecycle::WantGate() const
{
    /* The RGB output gate covers: display off, the suspend
       window, and the post-wake restore — so nothing flows to
       the transport while the recovery revalidation is still
       ordering itself. Single-caller state (Poll thread); the
       flags are mutated under mutex_ by ApplyEvent. */
    return !display_on_ || suspended_ || restore_pending_;
}

void PowerLifecycle::ApplyGateAndCapture()
{
    bool gate;
    bool capture_changed = false;
    std::string reason;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        gate   = WantGate();
        gated_ = gate;
        if(capture_paused_ != gate)
        {
            capture_paused_  = gate;
            capture_changed  = true;
        }
        reason = suspended_       ? "system suspend"
               : restore_pending_ ? "post-wake restore"
                                  : "display off";
    }
    if(deps_.set_output_paused)
    {
        /* Always applied — SetOutputPaused refreshes the reason
           even when the flag is unchanged (idempotent store). */
        deps_.set_output_paused(gate, gate ? reason : "");
    }
    if(capture_changed && deps_.set_capture_paused)
    {
        deps_.set_capture_paused(gate);
    }
}

void PowerLifecycle::DropAllLanes()
{
    if(!deps_.lane_keys || !deps_.drop_pending)
    {
        return;
    }
    for(const DeviceKey& key : deps_.lane_keys())
    {
        deps_.drop_pending(key);
    }
}

/*---------------------------------------------------------*\
||| Events                                                   ||
\*---------------------------------------------------------*/

void PowerLifecycle::ApplyEvent(Event e)
{
    switch(e)
    {
    case Event::DisplayOff:
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(!display_on_)
            {
                return;    /* already off — freeze-once holds   */
            }
            display_on_ = false;
        }
        ApplyGateAndCapture();
        DropAllLanes();
        if(deps_.log)
        {
            deps_.log("power: display off — RGB output paused, "
                      "pending frames discarded");
        }
        if(!deps_.playback)
        {
            return;
        }
        /* Freeze the uploaded loop ONCE: an explicit sampled-
           hold transition, then silence — never a stream of
           writes to a sleeping display, and never stopping
           maintenance traffic to kill the animation (plan
           3.4). A loop the user already held is left alone. */
        const PlaybackStatus st = deps_.playback->Status();
        if(st.mode    == PlaybackMode::AnimationPlayback &&
           st.upload  == UploadState::Active &&
           st.control == PlaybackControl::Playing)
        {
            bool held;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                held = suspended_;
            }
            if(held)
            {
                /* Suspending already: park the intent, no write
                   may be attempted across the sleep boundary. */
                deps_.playback->SetIntendedControl(
                    PlaybackControl::Paused);
                std::lock_guard<std::mutex> lock(mutex_);
                power_hold_ = true;
            }
            else
            {
                const PlaybackController::Result r =
                    deps_.playback->RequestPowerControl(
                        PlaybackControl::Paused);
                std::lock_guard<std::mutex> lock(mutex_);
                power_hold_ = r.accepted;
            }
        }
        else if(st.mode   == PlaybackMode::AnimationPlayback &&
                st.upload == UploadState::Uploading &&
                deps_.playback->IntendedControl() ==
                    PlaybackControl::Playing)
        {
            /* In-flight upload on a dark display: park the
               intent so the post-ack convergence lands held. */
            deps_.playback->SetIntendedControl(
                PlaybackControl::Paused);
            std::lock_guard<std::mutex> lock(mutex_);
            power_hold_ = true;
        }
        return;
    }

    case Event::DisplayOn:
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(display_on_)
            {
                return;
            }
            display_on_ = true;
            if(power_hold_)
            {
                /* The freeze was power-imposed — the wake path
                   revalidates then restores (never blindly
                   resumes). */
                restore_pending_ = true;
            }
        }
        ApplyGateAndCapture();
        if(deps_.log)
        {
            deps_.log("power: display on");
        }
        return;
    }

    case Event::Suspend:
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(suspended_)
            {
                return;
            }
            suspended_ = true;
        }
        ApplyGateAndCapture();
        DropAllLanes();
        if(deps_.playback)
        {
            /* Park the controller across the sleep boundary —
               no ack refresh, clock sampling, or transport
               control may write while the system suspends. We
               never promise the loop stops: whatever state the
               device has, it keeps until wake. */
            deps_.playback->SetPowerGate(true);
        }
        if(deps_.log)
        {
            deps_.log("power: system suspend — pending frames "
                      "discarded, output parked");
        }
        return;
    }

    case Event::Resume:
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            suspended_       = false;
            restore_pending_ = true;
        }
        ApplyGateAndCapture();   /* restore_pending_ holds the gate */
        if(deps_.log)
        {
            deps_.log("power: system resumed — revalidating "
                      "identity before restore");
        }
        return;
    }
    }
}

/*---------------------------------------------------------*\
||| Reconnect diffing + live-lane recovery                   ||
\*---------------------------------------------------------*/

void PowerLifecycle::DiffConnections()
{
    std::vector<DeviceSnapshot> snaps;
    if(deps_.snapshots)
    {
        snaps = deps_.snapshots();
    }
    std::map<DeviceKey, ConnectionState> next;
    for(const DeviceSnapshot& s : snaps)
    {
        next[s.key] = s.connection;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!conn_seeded_)
        {
            conn_        = std::move(next);
            conn_seeded_ = true;
            return;    /* baseline — nothing "reconnected" yet */
        }
    }
    for(const DeviceSnapshot& s : snaps)
    {
        ConnectionState prev = ConnectionState::Disconnected;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = conn_.find(s.key);
            if(it != conn_.end())
            {
                prev = it->second;
            }
        }
        if(prev != ConnectionState::Connected &&
           s.connection == ConnectionState::Connected)
        {
            /* Disconnect/suspend recovery revalidates identity
               before restoring playback (plan 3.4). While a
               wake-restore is outstanding (or the system is
               still suspended) AttemptRestore owns the
               revalidation — the diff must not fire a second,
               duplicate restore behind it. */
            bool wake_restore_owns = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                wake_restore_owns =
                    restore_pending_ || suspended_;
            }
            if(deps_.playback &&
               deps_.playback->OwnsGroup(s.key))
            {
                if(!wake_restore_owns)
                {
                    const auto rv =
                        deps_.playback->RevalidateAndRestore();
                    if(deps_.log)
                    {
                        deps_.log(
                            std::string("power: owned group reconnected "
                                        "— revalidation result ") +
                            std::to_string((int)rv));
                    }
                }
            }
            else
            {
                RecoverLiveLane(s);
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        conn_ = std::move(next);
    }
}

void PowerLifecycle::RecoverLiveLane(const DeviceSnapshot& snap)
{
    if(snap.connection != ConnectionState::Connected)
    {
        return;
    }
    /* Same-generation reconnect: the lane's delivered-state
       memory survives and could eat the first post-reconnect
       frame (deferred 3.3 note). Drop obsolete pending work,
       reset delivered memory, then re-assert the last
       delivered state through the reliable convergence slot —
       it always transmits and lands before live work. */
    if(deps_.drop_pending)
    {
        deps_.drop_pending(snap.key);
    }
    RgbFrame resend;
    const bool have = deps_.last_delivered &&
                      deps_.last_delivered(snap.key, resend);
    if(deps_.reset_delivered)
    {
        deps_.reset_delivered(snap.key);
    }
    if(have && deps_.reliable_submit && !OutputPaused())
    {
        resend.controller          = snap.key;
        resend.topology_generation = snap.topology_generation;
        resend.sample_time         = deps_.now();
        resend.synthetic           = true;
        const SubmitResult r = deps_.reliable_submit(resend);
        if(r.status == SubmitStatus::Rejected)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            detail_ = "post-reconnect resend rejected: " +
                      r.reason;
        }
    }
}

/*---------------------------------------------------------*\
||| Wake restore                                             ||
\*---------------------------------------------------------*/

void PowerLifecycle::AttemptRestore()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!restore_pending_ || suspended_ || !display_on_)
        {
            return;    /* deferred — gate stays asserted */
        }
    }

    /* The freeze was power-imposed: the intended state is
       Playing again — set it BEFORE revalidation so the
       compare-and-restore sees "Playing", not the frozen hold.
       On a deferred restore the intent reverts to Paused so a
       poll-side convergence can never write ahead of the
       revalidation ordering. */
    bool flipped = false;
    if(deps_.playback)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        flipped = power_hold_;
    }
    if(flipped)
    {
        deps_.playback->SetIntendedControl(PlaybackControl::Playing);
    }

    /*---- Phase 1: owned-group revalidation + restore --------*\
    || RevalidateAndRestore performs RefreshStatus + identity   ||
    || re-resolution BEFORE any restore write — a still-        ||
    || consistent group gets none.                              ||
    \*----------------------------------------------------------*/
    if(deps_.playback)
    {
        const auto rv = deps_.playback->RevalidateAndRestore();
        if(rv == PlaybackController::RevalidateResult::Busy ||
           rv == PlaybackController::RevalidateResult::TransportFailed)
        {
            bool gave_up = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++restore_attempts_;
                if(restore_attempts_ >= kMaxRestoreAttempts)
                {
                    detail_ = "wake restore gave up after " +
                              std::to_string(restore_attempts_) +
                              " revalidation attempts";
                    restore_pending_  = false;
                    restore_attempts_ = 0;
                    power_hold_       = false;
                    gave_up           = true;
                }
                else
                {
                    detail_ = "wake restore deferred — "
                              "revalidation could not complete";
                }
            }
            if(flipped)
            {
                /* Deferral or give-up: the intended state stays
                   the hold — a poll-side convergence may never
                   write ahead of the revalidation ordering, and
                   on give-up the device keeps whatever state it
                   actually has. */
                deps_.playback->SetIntendedControl(
                    PlaybackControl::Paused);
            }
            if(gave_up)
            {
                /* The suspend-time park must release even when
                   the restore could not land — live output
                   resumes under the normal rules. */
                deps_.playback->SetPowerGate(false);
            }
            ApplyGateAndCapture();
            return;
        }
        if(deps_.log &&
           rv != PlaybackController::RevalidateResult::NotOwned)
        {
            static const char* names[] = {
                "not-owned", "busy", "transport-failed",
                "identity-lost", "consistent", "restored" };
            deps_.log(std::string("power: wake revalidation -> ") +
                      names[(int)rv]);
        }
    }

    /*---- Phase 2: ungate, then live-lane convergence --------*\
    || The gate lifts BEFORE the resends — they are ordinary    ||
    || pipeline submissions (reliable slot) and must not be     ||
    || gated by the pause they are part of finishing.           ||
    \*----------------------------------------------------------*/
    {
        std::lock_guard<std::mutex> lock(mutex_);
        restore_pending_  = false;
        restore_attempts_ = 0;
        power_hold_       = false;
        detail_.clear();
    }
    ApplyGateAndCapture();
    if(deps_.playback)
    {
        deps_.playback->SetPowerGate(false);
    }

    std::vector<DeviceSnapshot> snaps;
    if(deps_.snapshots)
    {
        snaps = deps_.snapshots();
    }
    for(const DeviceSnapshot& s : snaps)
    {
        if(s.connection != ConnectionState::Connected)
        {
            continue;
        }
        if(deps_.playback && deps_.playback->OwnsGroup(s.key))
        {
            continue;    /* the owned group was restored above */
        }
        RecoverLiveLane(s);
    }
}

/*---------------------------------------------------------*\
||| Owner-loop hook                                          ||
\*---------------------------------------------------------*/

void PowerLifecycle::Poll(TimePoint now)
{
    (void)now;
    std::deque<Event> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        events.swap(pending_);
    }
    for(const Event e : events)
    {
        ApplyEvent(e);
    }
    DiffConnections();
    AttemptRestore();
}

} /* namespace openrgb2 */
