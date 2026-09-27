/*---------------------------------------------------------*\
|| DeliveryPipeline.h                                        |
||                                                           |
||   The frame delivery pipeline (plan sections 3.2/3.3/4   |
||   and 6): sits between the scene worker's evaluated      |
||   output and the transport sink (OpenRgbClient). It is   |
||   itself an IFrameSink — producers SubmitLatest() into   |
||   it — and it drains each controller's bounded pending   |
||   state into the downstream transport.                   |
||                                                           |
||   Per controller ("lane"):                               |
||   - exactly ONE pending live frame plus in-flight work   |
||     ("Backpressure" acceptance row); a new live frame    |
||     replaces an unsent older one (latest-wins)           |
||   - a depth-1 RELIABLE slot for static-state             |
||     convergence (plan 3.3): never suppressed, never      |
||     displaced by live traffic, sent before live work     |
||   - an independently qualified budget: the minimum       |
||     interval between sends for THAT controller — a slow  |
||     wireless group does not pace fast wired devices      |
||   - unchanged-frame suppression: a live frame identical  |
||     to the last delivered one is not retransmitted       |
||   - per-controller DeliveryStats (aggregate too), with   |
||     TransportMeasured provenance — completions mean      |
||     "accepted by the transport", never "displayed"       |
||                                                           |
||   Absolute-time sampling: only the newest pending frame  |
||   survives, so a worker that wakes late sends the        |
||   current-state frame — never a replayed FIFO backlog.   |
||                                                           |
||   Delivery runs either synchronously (DrainDue — tests   |
||   and external schedulers) or on one worker thread per   |
||   lane (Start), so a lane blocked inside a slow send     |
||   cannot stall the others.                               |
||                                                           |
||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include "EngineTypes.h"
#include "FrameSink.h"

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace openrgb2
{

class DeliveryPipeline final : public IFrameSink
{
public:
    DeliveryPipeline();
    explicit DeliveryPipeline(std::shared_ptr<IFrameSink> transport);
    ~DeliveryPipeline();                             /* Stop() */

    DeliveryPipeline(const DeliveryPipeline&)            = delete;
    DeliveryPipeline& operator=(const DeliveryPipeline&) = delete;

    /* The downstream transport sink (OpenRgbClient in
       production, a fake in tests). May be swapped live;
       nullptr drains count as transport rejections. */
    void                        SetTransport(std::shared_ptr<IFrameSink> t);
    std::shared_ptr<IFrameSink> Transport() const;

    /*---- Lanes ---------------------------------------------*\
    | RegisterDevice creates a lane keyed by the snapshot's   |
    | controller DeviceKey (new lanes inherit the default     |
    | budget). A topology-generation change drops pending     |
    | frames and resets the lane's sequence guard + delivered |
    | state — frames are obsolete across a topology change.   |
    | Same-generation re-registration (connection refresh)    |
    | preserves pending state.                                |
    | SyncDevices registers every listed snapshot and removes |
    | lanes for controllers absent from the list (pending     |
    | frames for vanished controllers are dropped).           |
    \*---------------------------------------------------------*/
    void RegisterDevice(const DeviceSnapshot& snapshot);
    void SyncDevices(const std::vector<DeviceSnapshot>& snapshots);
    void RemoveDevice(const DeviceKey& controller);
    bool HasDevice(const DeviceKey& controller) const;

    /* Per-controller send budget: the minimum interval between
       transport handoffs for that controller. Zero = unpaced
       (producer rate governs). Budgets are qualified per
       controller — never a global rate. */
    void SetBudget(const DeviceKey& controller,
                   Clock::duration min_interval);
    void SetDefaultBudget(Clock::duration min_interval);
    bool Budget(const DeviceKey& controller, Clock::duration& out) const;

    /*---- Submissions ---------------------------------------*\

    | SubmitLatest — the IFrameSink live-frame path: validates |
    | against the lane's snapshot (unknown controller /        |
    | disconnected / stale topology generation / LED-count     |
    | mismatch / empty -> Rejected with reason), rejects an    |
    | out-of-order sequence older than the highest admitted,   |
    | then stores the frame in the lane's depth-1 live slot —  |
    | replacing any unsent frame -> ReplacedPending.           |
    \*---------------------------------------------------------*/
    SubmitResult SubmitLatest(const RgbFrame& frame) override;

    /* The reliable static-state convergence path (plan 3.3 —
       kept distinct from replaceable live frames): same
       validation, but the frame is NEVER suppressed by
       unchanged-frame detection and NEVER displaced by live
       submissions. It occupies the lane's separate depth-1
       reliable slot; a second reliable submission replaces an
       unsent one (ReplacedPending). Reliable sends are not
       paced by the budget, though they do restart it — the
       next live frame waits one budget interval. */
    SubmitResult SubmitReliable(const RgbFrame& frame);

    /*---- Delivery ------------------------------------------*\
    | DrainDue delivers at most one frame for the controller   |
    | — a pending reliable frame first, else the pending live |
    | frame if its budget is due at `now`. Returns 1 when a   |
    | frame was handed to the transport, 0 otherwise          |
    | (nothing pending, not yet due, or suppressed).          |
    | DrainAllDue applies DrainDue to every lane. The lane    |
    | worker threads call the same path with Clock::now().    |
    \*---------------------------------------------------------*/
    size_t DrainDue(const DeviceKey& controller,
                    TimePoint now = Clock::now());
    size_t DrainAllDue(TimePoint now = Clock::now());

    /* Pending inspection (live + reliable slots). Pending(key)
       reports the next frame the lane would send — reliable
       first, then live. */
    size_t PendingCount() const;
    size_t PendingCount(const DeviceKey& controller) const;
    bool   Pending(const DeviceKey& controller,
                   RgbFrame* out = nullptr) const;

    /* Per-controller stats (false when unregistered) and the
       aggregate across all lanes. Provenance is
       TransportMeasured: transport_completed counts frames
       accepted by the downstream sink — admitted/submitted,
       never displayed. */
    bool          Stats(const DeviceKey& controller,
                        DeliveryStats& out) const;
    DeliveryStats Stats() const override;

    /* The last frame handed to the transport for `controller`
       — the delivered-state preview (plan section 5: preview
       and output use the same sampled colors). False when the
       controller is unregistered or nothing was delivered yet.
       Also exposes the lane's send budget via `budget` when
       non-null. */
    bool          LastDelivered(const DeviceKey& controller,
                                RgbFrame& out,
                                Clock::duration* budget = nullptr) const;

    /* One delivery worker thread per lane. A lane blocked
       inside a slow transport send cannot stall the others —
       its submissions still collapse into the depth-1 slots.

       Stop() is synchronous (joins every lane thread). Its
       bound is honest, not instant: one in-flight transport
       send per lane plus join time — a lane inside
       transport->SubmitLatest cannot be interrupted, so the
       bound is only as strong as the transport's own send
       timeout. The production transport applies SO_SNDTIMEO
       (OpenRgbClientOptions::io_timeout_ms, default 4 s), so
       Stop() completes within ~(lanes x io_timeout) in the
       worst case; lanes join sequentially but each blocked
       send hits its timeout independently. The same bound
       applies to RemoveDevice() and the destructor. */
    bool Start();
    void Stop();
    bool Running() const;

private:
    struct Lane
    {
        DeviceSnapshot        snap;
        Clock::duration       min_interval{};

        RgbFrame              live;         bool has_live     = false;
        RgbFrame              reliable;     bool has_reliable = false;

        /* Delivered-state memory — suppression and staleness. */
        bool                  has_sent      = false;
        uint64_t              last_sent_seq = 0;
        std::vector<RgbColor> last_colors;
        TimePoint             last_send{};

        uint64_t              max_seq = 0;  /* admitted high-water */
        DeliveryStats         stats;

        std::condition_variable cv;
        std::thread             thread;
        bool                    stop = false;
    };

    /* Validates a frame against a lane's snapshot; on success the
       result is Accept() (a sentinel — callers return their own
       status). All under mutex_. */
    static SubmitResult Validate(const Lane& lane, const RgbFrame& frame);

    /* Suppression check + transport call + stats. Delivers the
       frame popped by the caller; runs the transport send WITHOUT
       holding mutex_. `now` stamps the send: the lane threads pass
       Clock::now(), DrainDue passes the caller's time so budgets
       and intervals are fully deterministic under a synthetic
       clock. Returns true when the frame was handed to the
       transport (even if the transport rejected it). */
    bool   DeliverOne(Lane& lane, RgbFrame frame, bool reliable,
                      TimePoint now);

    void   LaneRun(Lane* lane);
    void   StartLane(Lane& lane);      /* under mutex_ */
    void   StopLane(Lane& lane);       /* join — no lock held */

    /* Lanes are shared_ptr: synchronous drains (DrainDue /
       DrainAllDue) and Stop() hold a reference across the
       unlocked transport call, so a concurrent RemoveDevice /
       SyncDevices can erase the lane from the map — and even
       destroy it — while a drain still uses it safely. The
       lane object outlives its map entry until the last
       in-flight user releases it. */
    mutable std::mutex                 mutex_;
    std::shared_ptr<IFrameSink>        transport_;
    std::map<DeviceKey, std::shared_ptr<Lane>> lanes_;
    Clock::duration                    default_budget_{};
    bool                               running_ = false;
    DeliveryStats                      aggregate_;
};

} /* namespace openrgb2 */
