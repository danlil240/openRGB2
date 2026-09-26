/*---------------------------------------------------------*\
|| FrameSink.h                                               |
||                                                           |
||   The submitLatest(RgbFrame) boundary (plan section 4).  |
||   Producers hand complete-controller frames to a sink;   |
||   the sink owns the bounded pending state — never an     |
||   unbounded FIFO.                                        |
||                                                           |
||   This task ships the interface plus two sinks:          |
||     NullFrameSink    — accepts everything (bring-up,     |
||                        tests, "no output configured")    |
||     LatestFrameSink  — depth-1 pending slot per          |
||                        controller with latest-wins       |
||                        replacement and snapshot          |
||                        validation; the SDK delivery      |
||                        worker drains it (later M3 task)  |
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
|| transport-completed (nothing is ever pending). The       |
|| skeleton engine's default target until the SDK adapter   |
|| and device mapping land.                                 |
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
|| DeviceSnapshot (unknown controller, stale topology       |
|| generation, LED-count mismatch, empty frame ->           |
|| Rejected), then stores it in the controller's depth-1    |
|| pending slot — replacing any frame still waiting ->      |
|| ReplacedPending.                                         |
||                                                          |
|| DrainPending hands every pending frame to the delivery   |
|| callback in DeviceKey order and records transport        |
|| completions + intervals. The SDK writer task will call   |
|| it on its own budget; tests drive it directly.           |
\*---------------------------------------------------------*/
class LatestFrameSink final : public IFrameSink
{
public:
    using DeliverFn = std::function<void(const RgbFrame&)>;

    /* Registers/replaces the topology basis for validation.
       Drops any pending frame for the device — frames are
       obsolete across a topology change. */
    void          RegisterDevice(const DeviceSnapshot& snapshot);
    void          RemoveDevice(const DeviceKey& controller);

    SubmitResult  SubmitLatest(const RgbFrame& frame) override;

    /* Drains every pending frame, invoking `deliver` (may be
       empty) per frame and recording transport completions at
       `completed_at`. Returns the number of frames drained. */
    size_t        DrainPending(const DeliverFn& deliver,
                               TimePoint completed_at = Clock::now());

    bool          Pending(const DeviceKey& controller,
                          RgbFrame* out = nullptr) const;
    size_t        PendingCount() const;

    DeliveryStats Stats() const override;

private:
    mutable std::mutex              mutex_;
    std::map<DeviceKey, DeviceSnapshot> devices_;
    std::map<DeviceKey, RgbFrame>       pending_;
    DeliveryStats                     stats_;
};

} /* namespace openrgb2 */
