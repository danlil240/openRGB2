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
    /* Nothing is ever pending: an accepted frame counts as
       completed transport immediately. */
    stats_.RecordTransportCompletion(Clock::now());
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
    const DeviceKey key = snapshot.key;
    const auto      it  = devices_.find(key);
    if(it == devices_.end() ||
       it->second.topology_generation != snapshot.topology_generation)
    {
        /* Frames are obsolete across a topology change (plan
           section 6: stop obsolete frames on topology change). */
        pending_.erase(key);
    }
    devices_[key] = snapshot;
}

void LatestFrameSink::RemoveDevice(const DeviceKey& controller)
{
    std::lock_guard<std::mutex> lock(mutex_);
    devices_.erase(controller);
    pending_.erase(controller);
}

SubmitResult LatestFrameSink::SubmitLatest(const RgbFrame& frame)
{
    std::lock_guard<std::mutex> lock(mutex_);

    const auto dev = devices_.find(frame.controller);
    if(dev == devices_.end())
    {
        ++stats_.rejected;
        return SubmitResult::Reject("unknown controller");
    }
    if(dev->second.connection == ConnectionState::Disconnected)
    {
        ++stats_.rejected;
        return SubmitResult::Reject("controller disconnected");
    }
    if(frame.topology_generation != dev->second.topology_generation)
    {
        ++stats_.rejected;
        return SubmitResult::Reject("stale topology generation");
    }
    if(frame.colors.empty())
    {
        ++stats_.rejected;
        return SubmitResult::Reject("empty frame");
    }
    if(frame.colors.size() != dev->second.LedCount())
    {
        ++stats_.rejected;
        return SubmitResult::Reject("LED count mismatch");
    }

    ++stats_.submitted;
    const auto slot = pending_.find(frame.controller);
    if(slot != pending_.end())
    {
        /* Latest-wins: the pending frame is obsolete — replace it
           rather than queueing behind it. */
        slot->second = frame;
        ++stats_.replaced_pending;
        return SubmitResult::Replace();
    }
    pending_.emplace(frame.controller, frame);
    return SubmitResult::Accept();
}

size_t LatestFrameSink::DrainPending(const DeliverFn& deliver,
                                     TimePoint completed_at)
{
    std::vector<RgbFrame> drained;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        drained.reserve(pending_.size());
        for(auto& entry : pending_)
        {
            drained.push_back(std::move(entry.second));
        }
        pending_.clear();
    }

    for(const RgbFrame& frame : drained)
    {
        if(deliver)
        {
            deliver(frame);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.RecordTransportCompletion(completed_at);
    }
    return drained.size();
}

bool LatestFrameSink::Pending(const DeviceKey& controller, RgbFrame* out) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = pending_.find(controller);
    if(it == pending_.end())
    {
        return false;
    }
    if(out)
    {
        *out = it->second;
    }
    return true;
}

size_t LatestFrameSink::PendingCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}

DeliveryStats LatestFrameSink::Stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

} /* namespace openrgb2 */
