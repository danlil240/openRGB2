/*---------------------------------------------------------*\
|| DeliveryPipeline.cpp                                      |
||                                                           |
||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "DeliveryPipeline.h"

#include <utility>

namespace openrgb2
{

DeliveryPipeline::DeliveryPipeline()
{
    aggregate_.provenance = MeasurementProvenance::TransportMeasured;
}

DeliveryPipeline::DeliveryPipeline(std::shared_ptr<IFrameSink> transport)
    : transport_(std::move(transport))
{
    aggregate_.provenance = MeasurementProvenance::TransportMeasured;
}

DeliveryPipeline::~DeliveryPipeline()
{
    Stop();
}

void DeliveryPipeline::SetTransport(std::shared_ptr<IFrameSink> t)
{
    std::lock_guard<std::mutex> lock(mutex_);
    transport_ = std::move(t);
}

std::shared_ptr<IFrameSink> DeliveryPipeline::Transport() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return transport_;
}

/*---------------------------------------------------------*\
|| Lanes                                                    |
\*---------------------------------------------------------*/

void DeliveryPipeline::RegisterDevice(const DeviceSnapshot& snapshot)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = lanes_.find(snapshot.key);
    if(it == lanes_.end())
    {
        auto lane            = std::make_unique<Lane>();
        lane->snap           = snapshot;
        lane->min_interval   = default_budget_;
        lane->stats.provenance =
            MeasurementProvenance::TransportMeasured;
        Lane* raw = lane.get();
        lanes_.emplace(snapshot.key, std::move(lane));
        if(running_)
        {
            StartLane(*raw);
        }
        return;
    }

    Lane& lane = *it->second;
    if(lane.snap.topology_generation != snapshot.topology_generation)
    {
        /* Frames are obsolete across a topology change (plan
           section 6): drop pending work and reset the
           sequence guard + delivered-state memory — the new
           generation may carry a new sequence space and the
           device's current colors are unknown again. */
        lane.has_live      = false;
        lane.has_reliable  = false;
        lane.has_sent      = false;
        lane.last_colors.clear();
        lane.max_seq       = 0;
        lane.last_sent_seq = 0;
    }
    lane.snap = snapshot;
    lane.cv.notify_all();   /* re-evaluate against the new basis */
}

void DeliveryPipeline::SyncDevices(
    const std::vector<DeviceSnapshot>& snapshots)
{
    for(const DeviceSnapshot& snap : snapshots)
    {
        RegisterDevice(snap);
    }
    std::vector<DeviceKey> stale;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for(const auto& entry : lanes_)
        {
            bool present = false;
            for(const DeviceSnapshot& snap : snapshots)
            {
                if(snap.key == entry.first)
                {
                    present = true;
                    break;
                }
            }
            if(!present)
            {
                stale.push_back(entry.first);
            }
        }
    }
    for(const DeviceKey& key : stale)
    {
        RemoveDevice(key);
    }
}

void DeliveryPipeline::RemoveDevice(const DeviceKey& controller)
{
    std::unique_ptr<Lane> lane;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = lanes_.find(controller);
        if(it == lanes_.end())
        {
            return;
        }
        lane = std::move(it->second);
        lanes_.erase(it);
        lane->stop = true;
    }
    lane->cv.notify_all();
    StopLane(*lane);    /* joins the lane thread before destruction */
}

bool DeliveryPipeline::HasDevice(const DeviceKey& controller) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lanes_.find(controller) != lanes_.end();
}

void DeliveryPipeline::SetBudget(const DeviceKey& controller,
                                 Clock::duration min_interval)
{
    Lane* lane = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = lanes_.find(controller);
        if(it == lanes_.end())
        {
            return;
        }
        it->second->min_interval = min_interval;
        lane = it->second.get();
    }
    lane->cv.notify_all();  /* a shorter budget may be due already */
}

void DeliveryPipeline::SetDefaultBudget(Clock::duration min_interval)
{
    std::lock_guard<std::mutex> lock(mutex_);
    default_budget_ = min_interval;
}

bool DeliveryPipeline::Budget(const DeviceKey& controller,
                              Clock::duration& out) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = lanes_.find(controller);
    if(it == lanes_.end())
    {
        return false;
    }
    out = it->second->min_interval;
    return true;
}

/*---------------------------------------------------------*\
|| Submissions                                              |
\*---------------------------------------------------------*/

SubmitResult DeliveryPipeline::Validate(const Lane& lane,
                                        const RgbFrame& frame)
{
    if(lane.snap.connection == ConnectionState::Disconnected)
    {
        return SubmitResult::Reject("controller disconnected");
    }
    if(frame.topology_generation != lane.snap.topology_generation)
    {
        return SubmitResult::Reject("stale topology generation");
    }
    if(frame.colors.empty())
    {
        return SubmitResult::Reject("empty frame");
    }
    if(frame.colors.size() != lane.snap.LedCount())
    {
        return SubmitResult::Reject("LED count mismatch");
    }
    return SubmitResult::Accept();
}

SubmitResult DeliveryPipeline::SubmitLatest(const RgbFrame& frame)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = lanes_.find(frame.controller);
    if(it == lanes_.end())
    {
        ++aggregate_.rejected;
        return SubmitResult::Reject("unknown controller");
    }
    Lane& lane = *it->second;

    const SubmitResult valid = Validate(lane, frame);
    if(valid.status == SubmitStatus::Rejected)
    {
        ++lane.stats.rejected;
        ++aggregate_.rejected;
        return valid;
    }
    if(frame.sequence < lane.max_seq)
    {
        /* Obsolete: the producer's sequence is monotonic per
           controller+generation, so an older sequence can never
           displace — or follow — a newer admitted frame. */
        ++lane.stats.rejected;
        ++aggregate_.rejected;
        return SubmitResult::Reject("out-of-order sequence");
    }
    lane.max_seq = frame.sequence;

    ++lane.stats.submitted;
    ++aggregate_.submitted;
    if(lane.has_live)
    {
        /* Latest-wins backpressure: the pending live frame is
           obsolete — replace it, never queue behind it. */
        lane.live = frame;
        ++lane.stats.replaced_pending;
        ++aggregate_.replaced_pending;
        lane.cv.notify_all();
        return SubmitResult::Replace();
    }
    lane.live     = frame;
    lane.has_live = true;
    lane.cv.notify_all();
    return SubmitResult::Accept();
}

SubmitResult DeliveryPipeline::SubmitReliable(const RgbFrame& frame)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = lanes_.find(frame.controller);
    if(it == lanes_.end())
    {
        ++aggregate_.rejected;
        return SubmitResult::Reject("unknown controller");
    }
    Lane& lane = *it->second;

    const SubmitResult valid = Validate(lane, frame);
    if(valid.status == SubmitStatus::Rejected)
    {
        ++lane.stats.rejected;
        ++aggregate_.rejected;
        return valid;
    }
    /* No sequence guard: the convergence path exists precisely
       to re-assert state — including deliberately resent
       frames — and is never suppressed downstream. */

    ++lane.stats.submitted;
    ++aggregate_.submitted;
    if(lane.has_reliable)
    {
        lane.reliable = frame;
        ++lane.stats.replaced_pending;
        ++aggregate_.replaced_pending;
        lane.cv.notify_all();
        return SubmitResult::Replace();
    }
    lane.reliable     = frame;
    lane.has_reliable = true;
    lane.cv.notify_all();
    return SubmitResult::Accept();
}

/*---------------------------------------------------------*\
|| Delivery                                                 |
\*---------------------------------------------------------*/

bool DeliveryPipeline::DeliverOne(Lane& lane, RgbFrame frame,
                                  bool reliable, TimePoint now)
{
    /* Unchanged-frame suppression + stale-live drop (live path
       only — reliable convergence is always transmitted). */
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!reliable && lane.has_sent &&
           (frame.sequence < lane.last_sent_seq ||
            frame.colors == lane.last_colors))
        {
            /* Identical to the delivered state, or older than the
               state already delivered (e.g. a reliable send
               overtook it): retransmission is suppressed. */
            ++lane.stats.suppressed;
            ++aggregate_.suppressed;
            return false;
        }
    }

    std::shared_ptr<IFrameSink> transport;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        transport = transport_;
    }
    const SubmitResult r =
        transport ? transport->SubmitLatest(frame)
                  : SubmitResult::Reject("no transport configured");

    std::lock_guard<std::mutex> lock(mutex_);
    if(r.status == SubmitStatus::Rejected)
    {
        /* The transport refused — the frame is dropped (the
           next live submission supersedes it anyway; bounded,
           no unbounded retry). */
        ++lane.stats.transport_rejected;
        ++aggregate_.transport_rejected;
        return true;
    }
    lane.stats.RecordTransportCompletion(now);
    aggregate_.RecordTransportCompletion(now);
    lane.has_sent      = true;
    lane.last_send     = now;
    lane.last_sent_seq = frame.sequence;
    lane.last_colors   = frame.colors;
    return true;
}

size_t DeliveryPipeline::DrainDue(const DeviceKey& controller,
                                  TimePoint now)
{
    Lane*    lane     = nullptr;
    RgbFrame frame;
    bool     reliable = false;
    bool     have     = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = lanes_.find(controller);
        if(it == lanes_.end())
        {
            return 0;
        }
        lane = it->second.get();
        if(lane->has_reliable)
        {
            frame             = std::move(lane->reliable);
            lane->has_reliable = false;
            reliable          = true;
            have              = true;
        }
        else if(lane->has_live &&
                (!lane->has_sent ||
                 now - lane->last_send >= lane->min_interval))
        {
            frame          = std::move(lane->live);
            lane->has_live = false;
            have           = true;
        }
    }
    if(!have)
    {
        return 0;
    }
    return DeliverOne(*lane, std::move(frame), reliable, now) ? 1 : 0;
}

size_t DeliveryPipeline::DrainAllDue(TimePoint now)
{
    std::vector<DeviceKey> keys;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for(const auto& entry : lanes_)
        {
            keys.push_back(entry.first);
        }
    }
    size_t n = 0;
    for(const DeviceKey& key : keys)
    {
        n += DrainDue(key, now);
    }
    return n;
}

/*---------------------------------------------------------*\
|| Pending inspection / stats                               |
\*---------------------------------------------------------*/

size_t DeliveryPipeline::PendingCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    size_t n = 0;
    for(const auto& entry : lanes_)
    {
        n += size_t(entry.second->has_live) +
             size_t(entry.second->has_reliable);
    }
    return n;
}

size_t DeliveryPipeline::PendingCount(const DeviceKey& controller) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = lanes_.find(controller);
    if(it == lanes_.end())
    {
        return 0;
    }
    return size_t(it->second->has_live) +
           size_t(it->second->has_reliable);
}

bool DeliveryPipeline::Pending(const DeviceKey& controller,
                               RgbFrame* out) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = lanes_.find(controller);
    if(it == lanes_.end() ||
       (!it->second->has_reliable && !it->second->has_live))
    {
        return false;
    }
    if(out)
    {
        /* The next frame the lane would send: reliable first. */
        *out = it->second->has_reliable ? it->second->reliable
                                        : it->second->live;
    }
    return true;
}

bool DeliveryPipeline::Stats(const DeviceKey& controller,
                             DeliveryStats& out) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = lanes_.find(controller);
    if(it == lanes_.end())
    {
        return false;
    }
    out = it->second->stats;
    return true;
}

DeliveryStats DeliveryPipeline::Stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return aggregate_;
}

/*---------------------------------------------------------*\
|| Lane worker threads                                      |
\*---------------------------------------------------------*/

void DeliveryPipeline::LaneRun(Lane* lane)
{
    for(;;)
    {
        RgbFrame frame;
        bool     reliable = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            for(;;)
            {
                if(lane->stop)
                {
                    return;
                }
                if(lane->has_reliable)
                {
                    frame              = std::move(lane->reliable);
                    lane->has_reliable = false;
                    reliable           = true;
                    break;
                }
                if(lane->has_live)
                {
                    const TimePoint now = Clock::now();
                    if(!lane->has_sent ||
                       now - lane->last_send >= lane->min_interval)
                    {
                        frame          = std::move(lane->live);
                        lane->has_live = false;
                        break;
                    }
                    /* Pending but not yet due — wake exactly at the
                       budget deadline (monotonic; a late wake still
                       delivers the newest pending frame). */
                    lane->cv.wait_until(
                        lock, lane->last_send + lane->min_interval);
                    continue;
                }
                lane->cv.wait(lock);
            }
        }
        DeliverOne(*lane, std::move(frame), reliable, Clock::now());
    }
}

void DeliveryPipeline::StartLane(Lane& lane)   /* under mutex_ */
{
    if(lane.thread.joinable())
    {
        return;              /* already running */
    }
    lane.stop    = false;
    lane.thread  = std::thread(&DeliveryPipeline::LaneRun, this, &lane);
}

void DeliveryPipeline::StopLane(Lane& lane)    /* no lock held */
{
    if(lane.thread.joinable())
    {
        lane.thread.join();
    }
}

bool DeliveryPipeline::Start()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if(running_)
    {
        return false;
    }
    running_ = true;
    for(auto& entry : lanes_)
    {
        StartLane(*entry.second);
    }
    return true;
}

void DeliveryPipeline::Stop()
{
    std::vector<Lane*> lanes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!running_)
        {
            return;
        }
        running_ = false;
        for(auto& entry : lanes_)
        {
            entry.second->stop = true;
            entry.second->cv.notify_all();
            lanes.push_back(entry.second.get());
        }
    }
    for(Lane* lane : lanes)
    {
        StopLane(*lane);
    }
}

bool DeliveryPipeline::Running() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

} /* namespace openrgb2 */
