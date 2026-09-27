/*---------------------------------------------------------*\
||| PlaybackClockSync.cpp                                     |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
|\*---------------------------------------------------------*/

#include "PlaybackClockSync.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace openrgb2
{

PlaybackClockSync::PlaybackClockSync(Config cfg)
    : cfg_(cfg), est_tick_hz_(cfg.nominal_tick_hz)
{
}

void PlaybackClockSync::SetConfig(const Config& cfg)
{
    cfg_ = cfg;
}

const PlaybackClockSync::Config& PlaybackClockSync::GetConfig() const
{
    return cfg_;
}

void PlaybackClockSync::Update(const PlaybackClockSnapshot& snap)
{
    if(!snap.valid)
    {
        /* A missed clock query is missing data, not a reset —
           the anchor ages and goes stale on the staleness
           bound. */
        ++misses_;
        return;
    }
    ++samples_;

    if(have_anchor_)
    {
        /* Forward delta on the 32-bit counter — unsigned
           subtraction handles a normal wraparound for free. */
        const uint32_t delta_ticks = snap.ticks - anchor_ticks_;
        if(delta_ticks > 0x80000000u)
        {
            /* A backward jump beyond the wrap interpretation is
               a device reset/reboot (same check the backend
               runtime uses). Re-anchor on the fresh sample,
               discard the rate estimate, and require the lock
               samples again — never report a false lock. */
            ++resets_;
            est_tick_hz_ = cfg_.nominal_tick_hz;
            consistent_  = 0;
        }
        else
        {
            const double dt = std::chrono::duration<double>(
                snap.sampled_at - anchor_at_).count();
            if(dt > 0.0)
            {
                /* Drift correction, bounded: the measured tick
                   rate is clamped to the configured deviation
                   band around nominal so a single bad pair
                   cannot steer the estimate arbitrarily. */
                const double measured = (double)delta_ticks / dt;
                const double lo = cfg_.nominal_tick_hz *
                                  (1.0 - cfg_.max_rate_deviation);
                const double hi = cfg_.nominal_tick_hz *
                                  (1.0 + cfg_.max_rate_deviation);
                const double clamped =
                    std::max(lo, std::min(hi, measured));
                est_tick_hz_ += cfg_.rate_ema_alpha *
                                (clamped - est_tick_hz_);
            }
        }
    }

    anchor_ticks_          = snap.ticks;
    anchor_at_             = snap.sampled_at;
    anchor_uncertainty_s_  = snap.uncertainty_s;
    have_anchor_           = true;
    ++consistent_;
}

void PlaybackClockSync::Reset()
{
    have_anchor_  = false;
    consistent_   = 0;
    est_tick_hz_  = cfg_.nominal_tick_hz;
    anchor_ticks_ = 0;
    anchor_uncertainty_s_ = 0.0;
}

SyncHealth PlaybackClockSync::Health(TimePoint now) const
{
    if(!have_anchor_)
    {
        return SyncHealth::Unknown;
    }
    const double age =
        std::chrono::duration<double>(now - anchor_at_).count();
    if(age < 0.0 || age > cfg_.stale_after_s)
    {
        return SyncHealth::Stale;
    }
    if(consistent_ < cfg_.lock_samples)
    {
        return SyncHealth::Degraded;
    }
    return SyncHealth::Healthy;
}

std::optional<double> PlaybackClockSync::Phase(TimePoint now,
                                               double cycle_s) const
{
    const SyncHealth h = Health(now);
    if(h != SyncHealth::Healthy || cycle_s <= 0.0)
    {
        /* Degraded (not yet re-locked after a wrap/reset),
           Stale, and Unknown all withhold the phase — never
           a false lock. */
        return std::nullopt;
    }
    const double device_s = ClockSeconds(now);
    double phase = device_s / cycle_s +
                   cfg_.latency_compensation_s / cycle_s;
    phase -= std::floor(phase);
    if(phase < 0.0)
    {
        phase += 1.0;
    }
    return phase;
}

bool PlaybackClockSync::Anchored() const
{
    return have_anchor_;
}

double PlaybackClockSync::EstimatedTickHz() const
{
    return est_tick_hz_;
}

double PlaybackClockSync::ClockSeconds(TimePoint now) const
{
    if(!have_anchor_)
    {
        return 0.0;
    }
    const double age =
        std::chrono::duration<double>(now - anchor_at_).count();
    const double ticks = (double)anchor_ticks_ +
                         (age > 0.0 ? age : 0.0) * est_tick_hz_;
    return ticks * cfg_.tick_s;
}

double PlaybackClockSync::UncertaintyS(TimePoint now) const
{
    if(!have_anchor_)
    {
        return 0.0;
    }
    /* Base query uncertainty plus the worst-case drift the
       bounded correction may still be off by over the sample
       age. */
    const double age = std::max(0.0,
        std::chrono::duration<double>(now - anchor_at_).count());
    return anchor_uncertainty_s_ +
           age * cfg_.max_rate_deviation * cfg_.nominal_tick_hz *
                 cfg_.tick_s;
}

uint64_t PlaybackClockSync::Resets() const
{
    return resets_;
}

uint64_t PlaybackClockSync::Samples() const
{
    return samples_;
}

uint64_t PlaybackClockSync::Misses() const
{
    return misses_;
}

TimePoint PlaybackClockSync::LastSampleAt() const
{
    return anchor_at_;
}

} /* namespace openrgb2 */
