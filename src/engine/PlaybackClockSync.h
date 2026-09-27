/*---------------------------------------------------------*\
||| PlaybackClockSync.h                                       |
|||                                                           |
|||   Validated wireless playback-clock -> wired-timeline   |
|||   synchronization (plan sections 3.4 and 4).            |
|||                                                           |
|||   The M2 backend samples the V1 master RGB clock        |
|||   (~1600 ticks/s, 0.625 ms/tick) through its serialized |
|||   worker and exposes timestamped snapshots via          |
|||   GroupStatus. Whatever diagnostics channel carries     |
|||   them to the engine, this component is the consumer:   |
|||   it validates each snapshot, anchors an extrapolated   |
|||   device-clock estimate, applies bounded drift          |
|||   correction, and answers the loop phase used to drive  |
|||   the matching wired effect.                            |
|||                                                           |
|||   Honest lock semantics (plan section 3.4):             |
|||   - no sample ever            -> Unknown                |
|||   - sample older than the                                   |
|||     staleness bound           -> Stale                  |
|||   - a device-clock reset (a   -> Degraded until enough  |
|||     backward jump beyond a        consecutive samples    |
|||     normal 32-bit wrap)           re-establish the lock |
|||   - fresh but too few         -> Degraded               |
|||     consistent samples                                  |
|||   - fresh + consistent        -> Healthy                |
|||                                                           |
|||   Phase() reports a loop position ONLY while sync is   |
|||   Healthy — Degraded (fresh but not yet re-locked      |
|||   after a wrap/reset), Stale, and Unknown all produce  |
|||   no phase rather than a false lock; the wired         |
|||   timeline falls back to the local clock and status    |
|||   reports the honest health.                           |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
|\*---------------------------------------------------------*/

#pragma once

#include "EngineTypes.h"

#include <cstdint>
#include <optional>

namespace openrgb2
{

/*---------------------------------------------------------*\
||| PlaybackClockSnapshot — one validated clock reading as  |
||| delivered by the backend diagnostics path.             |
|||                                                           |
|||   ticks         — device tick counter (~1600 ticks/s;  |
|||                   0.625 ms/tick — the M2 V1 clock       |
|||                   query, docs/benchmarks/               |
|||                   playback-result.md)                   |
|||   sampled_at    — LOCAL monotonic time the sample was  |
|||                   taken                                 |
|||   uncertainty_s — round-trip/query bound on the sample  |
|||   valid         — false when the backend could not     |
|||                   read the clock (treated as missing    |
|||                   data — staleness, not a reset)        |
|\*---------------------------------------------------------*/
struct PlaybackClockSnapshot
{
    bool      valid         = false;
    uint32_t  ticks         = 0;
    TimePoint sampled_at;
    double    uncertainty_s = 0.0;
};

class PlaybackClockSync
{
public:
    struct Config
    {
        double   nominal_tick_hz      = 1600.0; /* measured M2 rate */
        double   tick_s               = 0.000625; /* 0.625 ms/tick   */
        double   stale_after_s        = 9.0;    /* > 3 s query       */
                                                 /*   cadence        */
        double   max_rate_deviation   = 0.10;   /* drift bound ±10%  */
        double   rate_ema_alpha       = 0.3;
        uint32_t lock_samples         = 2;      /* consistent        */
                                                /* samples to lock   */
        /* Measured-latency compensation advances the wired
           timeline by this many seconds to hide delivery
           latency. Zero until measured — latency compensation
           is added only after measurement (plan section 4). */
        double   latency_compensation_s = 0.0;
    };

    explicit PlaybackClockSync(Config cfg = Config{});

    void SetConfig(const Config& cfg);
    const Config& GetConfig() const;

    /* Feed one snapshot. Invalid snapshots are recorded as a
       miss (the clock stayed unreadable — the anchor ages and
       goes stale on its own). A valid snapshot whose tick
       delta would mean a backward jump larger than a 32-bit
       wrap is a device RESET: the estimate re-anchors on the
       new sample, the rate estimate is discarded, and the
       lock needs config.lock_samples consecutive consistent
       samples to re-establish — no false lock on reboot. */
    void Update(const PlaybackClockSnapshot& snap);

    /* Drop all state (mode transition / source lost). */
    void Reset();

    SyncHealth Health(TimePoint now) const;

    /* Loop position in [0,1) for a loop of `cycle_s` seconds
       at local time `now`, including latency compensation.
       nullopt unless sync is Healthy — degraded, stale, or
       missing data never produces a phase. */
    std::optional<double> Phase(TimePoint now, double cycle_s) const;

    /* Diagnostics. */
    bool     Anchored() const;
    double   EstimatedTickHz() const;     /* drift-corrected     */
    double   ClockSeconds(TimePoint now) const; /* extrapolated     */
    double   UncertaintyS(TimePoint now) const;
    uint64_t Resets() const;              /* resets observed     */
    uint64_t Samples() const;             /* valid samples seen  */
    uint64_t Misses() const;              /* invalid samples     */
    TimePoint LastSampleAt() const;

private:
    Config    cfg_;

    bool      have_anchor_     = false;
    uint32_t  anchor_ticks_    = 0;
    TimePoint anchor_at_{};
    double    anchor_uncertainty_s_ = 0.0;

    uint32_t  consistent_      = 0;   /* consecutive consistent */
    double    est_tick_hz_     = 0.0; /* init: nominal          */
    uint64_t  resets_          = 0;
    uint64_t  samples_         = 0;
    uint64_t  misses_          = 0;
};

} /* namespace openrgb2 */
