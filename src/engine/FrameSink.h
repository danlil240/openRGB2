/*---------------------------------------------------------*\
|| FrameSink.h                                               |
||                                                           |
||   The submitLatest(RgbFrame) boundary (plan section 4).  |
||   Producers hand complete-controller frames to a sink;   |
||   the sink owns the bounded pending state — never an     |
||   unbounded FIFO.                                        |
||                                                           |
||   Sinks shipped here:                                    |
||     NullFrameSink    — accepts everything (bring-up,     |
||                        tests, "no output configured")    |
||     LatestFrameSink  — depth-1 pending slot per          |
||                        controller with latest-wins       |
||                        replacement and snapshot          |
||                        validation.                       |
||   The scheduled multi-controller delivery pipeline       |
||   (budgets, suppression, reliable convergence path,      |
||   per-lane worker threads) lives in DeliveryPipeline.h.  |
||                                                           |
||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include "EngineTypes.h"

#include <functional>
#include <map>
#include <mutex>

namespace openrgb2
{

class IFrameSink
{
public:
    virtual ~IFrameSink() = default;

    /* Plan section 4 contract: accepted, replaced-pending, or
       rejected with reason — never "displayed." Implementations
       must be callable from any thread. */
    virtual SubmitResult  SubmitLatest(const RgbFrame& frame) = 0;
    virtual DeliveryStats Stats() const = 0;
};

/*---------------------------------------------------------*\
|| NullFrameSink — accepts every frame and counts it as     |
|| submitted. Nothing is transported, so nothing is ever    |
|| recorded as transport-completed (provenance stays        |
|| Unknown — a null sink must not fabricate delivery        |
|| measurements). The skeleton engine's default target.     |
\*---------------------------------------------------------*/
class NullFrameSink final : public IFrameSink
{
public:
    SubmitResult  SubmitLatest(const RgbFrame& frame) override;
    DeliveryStats Stats() const override;

private:
    mutable std::mutex mutex_;
    DeliveryStats      stats_;
};

/*---------------------------------------------------------*\
|| LatestFrameSink — "one pending live frame per controller |
|| plus in-flight work" (plan section 6 backpressure).      |
||                                                          |
|| SubmitLatest validates the frame against the registered  |
|| DeviceSnapshot (unknown controller, disconnected, stale  |
|| topology generation, LED-count mismatch, empty frame ->  |
|| Rejected), then stores it in the controller's depth-1    |
|| pending slot — replacing any frame still waiting ->      |
|| ReplacedPending. A frame whose sequence is older than    |
|| the highest sequence already admitted for the controller |
|| is obsolete -> Rejected("out-of-order sequence") — a     |
|| stale frame can never displace a newer one. Sequence     |
|| tracking resets on topology-generation change.           |
||                                                          |
|| DrainPending hands every pending frame to the delivery   |
|| callback in DeviceKey order and records per-controller   |
|| transport completions (each stamped at its own handoff   |
|| time unless the caller injects a timestamp). If the      |
|| callback throws, undelivered frames are restored to      |
|| their pending slots — latest-wins: a newer submission    |
|| made during the drain is never clobbered. Stats are      |
|| kept per controller AND aggregate.                       |
\*---------------------------------------------------------*/
class LatestFrameSink final : public IFrameSink
{
public:
    using DeliverFn = std::function<void(const RgbFrame&)>;

    /* Registers/replaces the topology basis for validation.
       Drops any pending frame for the device and resets its
       sequence guard — frames are obsolete across a topology
       change. Same-generation re-registration (e.g. a
       connection-state refresh) preserves both. */
    void          RegisterDevice(const DeviceSnapshot& snapshot);
    void          RemoveDevice(const DeviceKey& controller);

    SubmitResult  SubmitLatest(const RgbFrame& frame) override;

    /* Drains every pending frame, invoking `deliver` per frame
       and recording transport completions. When `at` is the
       default (TimePoint{}) each frame is stamped at its own
       handoff time; an explicit `at` stamps every frame in the
       batch identically (deterministic tests). Frames drained
       without a deliver callback are dropped and counted as
       suppressed, never as transport completions. If `deliver`
       throws, all not-yet-delivered frames are restored to
       their pending slots before the exception propagates.
       Returns the number of frames drained. */
    size_t        DrainPending(const DeliverFn& deliver,
                               TimePoint at = TimePoint{});

    bool          Pending(const DeviceKey& controller,
                          RgbFrame* out = nullptr) const;
    size_t        PendingCount() const;

    /* Per-controller stats; false when the controller is not
       registered. Stats() is the aggregate across all
       controllers (intervals span every controller's
       completions). */
    bool          Stats(const DeviceKey& controller,
                        DeliveryStats& out) const;
    DeliveryStats Stats() const override;

private:
    struct Lane
    {
        DeviceSnapshot snap;
        RgbFrame       pending;
        bool           has_pending = false;
        uint64_t       max_seq     = 0; /* admitted high-water   */
        DeliveryStats  stats;
    };

    mutable std::mutex          mutex_;
    std::map<DeviceKey, Lane>   lanes_;
    DeliveryStats               aggregate_;
};

} /* namespace openrgb2 */
