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
        auto lane            = std::make_shared<Lane>();
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
    std::shared_ptr<Lane> lane;
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
    std::shared_ptr<Lane> lane;   /* keep alive across the notify */
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = lanes_.find(controller);
        if(it == lanes_.end())
        {
            return;
        }
        it->second->min_interval = min_interval;
        lane = it->second;
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

    if(lane.suppressed)
    {
        /* An uploaded playback loop owns this group — live
           frames are rejected with the reason, never silently
           dropped (plan section 3.4: one RGB producer per
           wireless group). */
        ++lane.stats.rejected;
        ++aggregate_.rejected;
        return SubmitResult::Reject(
            lane.suppress_reason.empty()
                ? "controller owned by uploaded playback"
                : lane.suppress_reason);
    }

    if(output_paused_)
    {
        /* Display-off / suspend (Task 3.7): all RGB frame output
           is gated. The refusal is honest — the producer sees a
           Rejected with reason, never a silent drop. */
        ++lane.stats.rejected;
        ++aggregate_.rejected;
        return SubmitResult::Reject(
            pause_reason_.empty()
                ? "RGB output paused (display off or suspend)"
                : pause_reason_);
    }

    const SubmitResult valid = Validate(lane, frame);
    if(valid.status == SubmitStatus::Rejected)
    {
        ++lane.stats.rejected;
        ++aggregate_.rejected;
        return valid;
    }
    if(!frame.synthetic)
    {
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
    }
    /* Synthetic frames (playback-baked, stop-black) bypass the
       sequence namespace: they are authoritative state from the
       owning producer and never raise the live high-water — a
       later live frame cannot be starved by them. */

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

    if(lane.suppressed)
    {
        /* Reliable convergence is still an RGB producer — the
           single-producer rule applies to it too. */
        ++lane.stats.rejected;
        ++aggregate_.rejected;
        return SubmitResult::Reject(
            lane.suppress_reason.empty()
                ? "controller owned by uploaded playback"
                : lane.suppress_reason);
    }

    if(output_paused_)
    {
        ++lane.stats.rejected;
        ++aggregate_.rejected;
        return SubmitResult::Reject(
            pause_reason_.empty()
                ? "RGB output paused (display off or suspend)"
                : pause_reason_);
    }

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

void DeliveryPipeline::SetSuppressed(const DeviceKey& controller,
                                     bool suppressed,
                                     std::string reason)
{
    std::shared_ptr<Lane> lane;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = lanes_.find(controller);
        if(it == lanes_.end())
        {
            return;
        }
        it->second->suppressed      = suppressed;
        it->second->suppress_reason = std::move(reason);
        lane = it->second;
    }
    lane->cv.notify_all();   /* let WaitIdle/waits re-evaluate */
}

bool DeliveryPipeline::IsSuppressed(const DeviceKey& controller) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = lanes_.find(controller);
    return it != lanes_.end() && it->second->suppressed;
}

size_t DeliveryPipeline::DropPending(const DeviceKey& controller)
{
    std::shared_ptr<Lane> lane;
    size_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = lanes_.find(controller);
        if(it == lanes_.end())
        {
            return 0;
        }
        lane = it->second;
        dropped = size_t(lane->has_live) + size_t(lane->has_reliable);
        lane->has_live     = false;
        lane->has_reliable = false;
    }
    lane->cv.notify_all();
    return dropped;
}

void DeliveryPipeline::ResetDeliveredState(const DeviceKey& controller)
{
    std::shared_ptr<Lane> lane;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = lanes_.find(controller);
        if(it == lanes_.end())
        {
            return;
        }
        lane = it->second;
        /* Delivered-state memory is stale across a mode
           transition: the uploaded loop changed the device's
           colors without touching this lane. Clear it so the
           first frame back on live is never suppressed — the
           sequence guard (max_seq) intentionally survives: it
           is per generation, not per delivered frame. */
        lane->has_sent      = false;
        lane->last_sent_seq = 0;
        lane->last_colors.clear();
    }
    lane->cv.notify_all();
}

size_t DeliveryPipeline::InFlight(const DeviceKey& controller) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = lanes_.find(controller);
    return it == lanes_.end() ? 0 : it->second->in_flight;
}

bool DeliveryPipeline::WaitIdle(const DeviceKey& controller,
                                Clock::duration timeout)
{
    /* Bounded wait for "no pending and nothing inside a
       transport send" — mode changes serialize behind the
       last live send so a queued or in-flight frame cannot
       overwrite a newly uploaded loop. */
    const TimePoint deadline = Clock::now() + timeout;
    std::shared_ptr<Lane> lane;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = lanes_.find(controller);
        if(it == lanes_.end())
        {
            return true;      /* no lane == nothing pending  */
        }
        lane = it->second;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    return lane->cv.wait_until(lock, deadline,
        [&lane]
        {
            return !lane->has_live && !lane->has_reliable &&
                   lane->in_flight == 0;
        });
}

size_t DeliveryPipeline::DropAllPending()
{
    std::vector<std::shared_ptr<Lane>> lanes;
    size_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for(auto& entry : lanes_)
        {
            dropped += size_t(entry.second->has_live) +
                       size_t(entry.second->has_reliable);
            entry.second->has_live     = false;
            entry.second->has_reliable = false;
            lanes.push_back(entry.second);
        }
    }
    for(const std::shared_ptr<Lane>& lane : lanes)
    {
        lane->cv.notify_all();
    }
    return dropped;
}

std::vector<DeviceKey> DeliveryPipeline::LaneKeys() const
{
    std::vector<DeviceKey> out;
    std::lock_guard<std::mutex> lock(mutex_);
    out.reserve(lanes_.size());
    for(const auto& entry : lanes_)
    {
        out.push_back(entry.first);
    }
    return out;
}

void DeliveryPipeline::SetOutputPaused(bool paused,
                                       std::string reason)
{
    std::vector<std::shared_ptr<Lane>> lanes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        output_paused_ = paused;
        pause_reason_  = std::move(reason);
        for(auto& entry : lanes_)
        {
            lanes.push_back(entry.second);
        }
    }
    /* Wake every lane so a paused worker sleeps and a resumed
       worker re-evaluates its pending frame immediately. */
    for(const std::shared_ptr<Lane>& lane : lanes)
    {
        lane->cv.notify_all();
    }
}

bool DeliveryPipeline::OutputPaused() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return output_paused_;
}

bool DeliveryPipeline::DeliverOne(Lane& lane, RgbFrame frame,
                                  bool reliable, TimePoint now)
{
    /* The caller popped this frame and charged in_flight —
       every exit path clears it (and wakes WaitIdle). */
    struct FlightGuard
    {
        Lane& lane; std::mutex& m;
        ~FlightGuard()
        {
            {
                std::lock_guard<std::mutex> lock(m);
                if(lane.in_flight > 0)
                {
                    --lane.in_flight;
                }
            }
            lane.cv.notify_all();   /* WaitIdle observes 0    */
        }
    } guard{ lane, mutex_ };

    /* Unchanged-frame suppression + stale-live drop (live path
       only — reliable convergence is always transmitted). */
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!reliable && lane.has_sent &&
           ((!frame.synthetic && frame.sequence < lane.last_sent_seq) ||
            frame.colors == lane.last_colors))
        {
            /* Identical to the delivered state, or older than the
               state already delivered (e.g. a reliable send
               overtook it): retransmission is suppressed. A
               synthetic frame only dedupes on identical colors —
               its sequence lives outside the live namespace. */
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
    /* A synthetic sequence is outside the live namespace —
       recording it would let it stale-suppress every future
       live frame. Live high-water tracking stays live-only. */
    if(!frame.synthetic)
    {
        lane.last_sent_seq = frame.sequence;
    }
    lane.last_colors   = frame.colors;
    return true;
}

size_t DeliveryPipeline::DrainDue(const DeviceKey& controller,
                                  TimePoint now)
{
    /* Hold a shared_ptr across the unlocked transport call —
       a concurrent RemoveDevice/SyncDevices may erase and
       destroy the lane's map entry mid-drain. */
    std::shared_ptr<Lane> lane;
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
        lane = it->second;
        if(lane->suppressed)
        {
            /* An uploaded loop owns the group: nothing in this
               lane may reach the transport until the producer
               resumes (plan section 3.4 — one producer). */
            return 0;
        }
        if(output_paused_)
        {
            /* Display-off/suspend: a pending frame stored just
               before the gate must not reach the transport. */
            return 0;
        }
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
        if(have)
        {
            ++lane->in_flight;   /* cleared inside DeliverOne */
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

bool DeliveryPipeline::LastDelivered(const DeviceKey& controller,
                                     RgbFrame& out,
                                     Clock::duration* budget) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = lanes_.find(controller);
    if(it == lanes_.end() || !it->second->has_sent)
    {
        return false;
    }
    const Lane& lane = *it->second;
    out.controller          = controller;
    out.topology_generation = lane.snap.topology_generation;
    out.sequence            = lane.last_sent_seq;
    out.sample_time         = lane.last_send;
    out.colors              = lane.last_colors;
    if(budget)
    {
        *budget = lane.min_interval;
    }
    return true;
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
                if(lane->suppressed)
                {
                    /* Uploaded playback owns the group — pending
                       work must not deliver until resume (the
                       controller drops it on transition anyway). */
                    lane->cv.wait(lock);
                    continue;
                }
                if(output_paused_)
                {
                    /* Display-off/suspend: RGB output is gated.
                       The lane sleeps until the lifecycle resumes
                       output — no periodic writes can leave while
                       the display sleeps (plan section 6). */
                    lane->cv.wait(lock);
                    continue;
                }
                if(lane->has_reliable)
                {
                    frame              = std::move(lane->reliable);
                    lane->has_reliable = false;
                    reliable           = true;
                    ++lane->in_flight;  /* cleared in DeliverOne */
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
                        ++lane->in_flight;
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
    /* shared_ptr refs keep every lane alive across the joins —
       a concurrent RemoveDevice cannot destroy a lane whose
       thread is still being joined here. Bound: the sum of
       in-flight transport sends (bounded by the transport's
       own send timeout) plus join time — see the Stop() doc. */
    std::vector<std::shared_ptr<Lane>> lanes;
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
            lanes.push_back(entry.second);
        }
    }
    for(const std::shared_ptr<Lane>& lane : lanes)
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
