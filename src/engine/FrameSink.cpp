/*---------------------------------------------------------*\
|| FrameSink.cpp                                             |
||                                                           |
||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "FrameSink.h"

#include <utility>
#include <vector>

namespace openrgb2
{

SubmitResult NullFrameSink::SubmitLatest(const RgbFrame& frame)
{
    (void)frame;
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.submitted;
    /* Nothing is transported: no fabricated completions —
       transport_completed stays 0 and provenance stays Unknown. */
    return SubmitResult::Accept();
}

DeliveryStats NullFrameSink::Stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void LatestFrameSink::RegisterDevice(const DeviceSnapshot& snapshot)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Lane& lane = lanes_[snapshot.key];
    if(lane.snap.topology_generation != snapshot.topology_generation)
    {
        /* Frames are obsolete across a topology change (plan
           section 6: stop obsolete frames on topology change);
           the admitted-sequence guard resets with them — a new
           generation may carry a new producer sequence space. */
        lane.has_pending = false;
        lane.pending     = RgbFrame{};
        lane.max_seq     = 0;
    }
    lane.snap = snapshot;
}

void LatestFrameSink::RemoveDevice(const DeviceKey& controller)
{
    std::lock_guard<std::mutex> lock(mutex_);
    lanes_.erase(controller);
}

SubmitResult LatestFrameSink::SubmitLatest(const RgbFrame& frame)
{
    std::lock_guard<std::mutex> lock(mutex_);

    const auto dev = lanes_.find(frame.controller);
    if(dev == lanes_.end())
    {
        ++aggregate_.rejected;
        return SubmitResult::Reject("unknown controller");
    }
    Lane& lane = dev->second;
    if(lane.snap.connection == ConnectionState::Disconnected)
    {
        ++lane.stats.rejected;
        ++aggregate_.rejected;
        return SubmitResult::Reject("controller disconnected");
    }
    if(frame.topology_generation != lane.snap.topology_generation)
    {
        ++lane.stats.rejected;
        ++aggregate_.rejected;
        return SubmitResult::Reject("stale topology generation");
    }
    if(frame.colors.empty())
    {
        ++lane.stats.rejected;
        ++aggregate_.rejected;
        return SubmitResult::Reject("empty frame");
    }
    if(frame.colors.size() != lane.snap.LedCount())
    {
        ++lane.stats.rejected;
        ++aggregate_.rejected;
        return SubmitResult::Reject("LED count mismatch");
    }
    if(frame.sequence < lane.max_seq)
    {
        /* Obsolete: an older sequence can never displace (or
           follow) a newer admitted frame — the producer's
           sequence is monotonic per controller+generation. */
        ++lane.stats.rejected;
        ++aggregate_.rejected;
        return SubmitResult::Reject("out-of-order sequence");
    }
    lane.max_seq = frame.sequence;

    ++lane.stats.submitted;
    ++aggregate_.submitted;
    if(lane.has_pending)
    {
        /* Latest-wins: the pending frame is obsolete — replace it
           rather than queueing behind it. */
        lane.pending = frame;
        ++lane.stats.replaced_pending;
        ++aggregate_.replaced_pending;
        return SubmitResult::Replace();
    }
    lane.pending     = frame;
    lane.has_pending = true;
    return SubmitResult::Accept();
}

size_t LatestFrameSink::DrainPending(const DeliverFn& deliver,
                                     TimePoint at)
{
    std::vector<RgbFrame> drained;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for(auto& entry : lanes_)
        {
            if(entry.second.has_pending)
            {
                drained.push_back(std::move(entry.second.pending));
                entry.second.has_pending = false;
            }
        }
    }

    size_t completed = 0;
    for(size_t i = 0; i < drained.size(); ++i)
    {
        if(deliver)
        {
            try
            {
                deliver(drained[i]);
            }
            catch(...)
            {
                /* The throw must not silently lose frames: restore
                   the failed frame and every undelivered one to
                   their pending slots — latest-wins, so a newer
                   submission made during the drain is kept. */
                std::lock_guard<std::mutex> lock(mutex_);
                for(size_t j = i; j < drained.size(); ++j)
                {
                    RgbFrame& f  = drained[j];
                    auto      it = lanes_.find(f.controller);
                    if(it == lanes_.end())
                    {
                        continue; /* device removed mid-drain — obsolete */
                    }
                    Lane& l = it->second;
                    if(!l.has_pending || f.sequence >= l.pending.sequence)
                    {
                        l.pending     = std::move(f);
                        l.has_pending = true;
                    }
                }
                throw;
            }
        }

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = lanes_.find(drained[i].controller);
        if(it == lanes_.end())
        {
            ++completed; /* device removed mid-drain — already handed off */
            continue;
        }
        Lane& lane = it->second;
        if(deliver)
        {
            /* Each frame stamps its own handoff time (callers may
               inject `at` for deterministic tests — the stamp is
               then shared across the batch by design). */
            const TimePoint stamp =
                (at == TimePoint{}) ? Clock::now() : at;
            lane.stats.RecordTransportCompletion(stamp);
            aggregate_.RecordTransportCompletion(stamp);
        }
        else
        {
            /* Drained without a handoff: deliberately dropped, so
               it counts as suppressed — never as a transport
               completion. */
            ++lane.stats.suppressed;
            ++aggregate_.suppressed;
        }
        ++completed;
    }
    return completed;
}

bool LatestFrameSink::Pending(const DeviceKey& controller, RgbFrame* out) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = lanes_.find(controller);
    if(it == lanes_.end() || !it->second.has_pending)
    {
        return false;
    }
    if(out)
    {
        *out = it->second.pending;
    }
    return true;
}

size_t LatestFrameSink::PendingCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    size_t n = 0;
    for(const auto& entry : lanes_)
    {
        if(entry.second.has_pending)
        {
            ++n;
        }
    }
    return n;
}

bool LatestFrameSink::Stats(const DeviceKey& controller,
                            DeliveryStats& out) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = lanes_.find(controller);
    if(it == lanes_.end())
    {
        return false;
    }
    out = it->second.stats;
    return true;
}

DeliveryStats LatestFrameSink::Stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return aggregate_;
}

} /* namespace openrgb2 */
