// SPDX-License-Identifier: GPL-2.0-or-later
//
// Contract tests for the frame delivery pipeline
// (src/engine/DeliveryPipeline.h): per-controller depth-1
// pending slots, latest-wins replacement, absolute-time
// sampling (no backlog replay), unchanged-frame suppression
// with the reliable convergence carve-out, independent
// per-controller budgets, and per-controller DeliveryStats.
// Fake transport only — no hardware, no real backend.

#include "DeliveryPipeline.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

using namespace openrgb2;
using namespace std::chrono_literals;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

/*---------------------------------------------------------*\
|| Recording/blocking fake transport                        |
\*---------------------------------------------------------*/
class FakeTransport final : public IFrameSink
{
public:
    SubmitResult SubmitLatest(const RgbFrame& f) override
    {
        std::unique_lock<std::mutex> lock(m_);
        if(blocked_.count(f.controller) && !released_)
        {
            ++blocked_entries_;
            blocked_cv_.wait(lock, [this] { return released_; });
        }
        if(on_send)
        {
            on_send();
        }
        delivered_[f.controller].push_back(f);
        return SubmitResult::Accept();
    }

    /* Optional hook run inside every send — widens the
       interleaving window for concurrency tests. */
    std::function<void()> on_send;

    DeliveryStats Stats() const override { return DeliveryStats{}; }

    /* Parks SubmitLatest calls for `key` inside the transport
       (a lane blocked mid-send) until Release(). */
    void Block(const DeviceKey& key)
    {
        std::lock_guard<std::mutex> lock(m_);
        released_ = false;
        blocked_.insert(key);
    }
    void Release()
    {
        {
            std::lock_guard<std::mutex> lock(m_);
            released_ = true;
        }
        blocked_cv_.notify_all();
    }
    size_t BlockedEntries() const
    {
        std::lock_guard<std::mutex> lock(m_);
        return blocked_entries_;
    }

    std::vector<RgbFrame> Delivered(const DeviceKey& key) const
    {
        std::lock_guard<std::mutex> lock(m_);
        const auto it = delivered_.find(key);
        return it == delivered_.end() ? std::vector<RgbFrame>{}
                                      : it->second;
    }
    size_t DeliveredCount(const DeviceKey& key) const
    {
        return Delivered(key).size();
    }

    /* Poll helper for the threaded tests. */
    bool WaitDelivered(const DeviceKey& key, size_t n,
                       std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while(std::chrono::steady_clock::now() < deadline)
        {
            if(DeliveredCount(key) >= n)
            {
                return true;
            }
            std::this_thread::sleep_for(2ms);
        }
        return DeliveredCount(key) >= n;
    }

private:
    mutable std::mutex          m_;
    std::condition_variable     blocked_cv_;
    bool                        released_ = true;
    std::set<DeviceKey>         blocked_;
    size_t                      blocked_entries_ = 0;
    std::map<DeviceKey, std::vector<RgbFrame>> delivered_;
};

/*---------------------------------------------------------*\
|| Fixtures                                                  |
\*---------------------------------------------------------*/
static DeviceSnapshot Wireless()
{
    /* One controller, 3 x 40-LED zones = 120 LEDs. */
    DeviceSnapshot s;
    s.key.device = "Lian Li:Lian Li SL Wireless";
    s.key.serial = "W1";
    s.key.location = "hid:W1";
    s.topology_generation = 1;
    s.zones = { { "fan-1", 40 }, { "fan-2", 40 }, { "fan-3", 40 } };
    s.connection = ConnectionState::Connected;
    return s;
}

static DeviceSnapshot Wired()
{
    DeviceSnapshot s;
    s.key.device = "ASUS:Motherboard";
    s.key.serial = "M1";
    s.key.location = "smbus:0";
    s.topology_generation = 1;
    s.zones = { { "header-1", 8 }, { "header-2", 16 },
                { "header-3", 8 } };
    s.connection = ConnectionState::Connected;
    return s;
}

static RgbFrame MakeFrame(const DeviceSnapshot& s, uint64_t seq,
                          RgbColor fill, TimePoint at = TimePoint{})
{
    RgbFrame f;
    f.controller          = s.key;
    f.topology_generation = s.topology_generation;
    f.sequence            = seq;
    f.sample_time         = at;
    f.colors.assign(s.LedCount(), fill);
    return f;
}

/*---------------------------------------------------------*\
|| Tests                                                     |
\*---------------------------------------------------------*/

/* Burst input collapses to exactly one pending live frame per
   controller — queue depth never exceeds 1 (plan section 6,
   "Backpressure"). */
static void TestBurstCollapse()
{
    auto transport = std::make_shared<FakeTransport>();
    DeliveryPipeline pipe(transport);
    const DeviceSnapshot w = Wireless();
    pipe.RegisterDevice(w);

    CHECK(pipe.SubmitLatest(MakeFrame(w, 1, { 1, 0, 0 })).status ==
          SubmitStatus::Accepted);
    for(uint64_t seq = 2; seq <= 5; ++seq)
    {
        CHECK(pipe.SubmitLatest(MakeFrame(w, seq, { 2, 0, 0 })).status ==
              SubmitStatus::ReplacedPending);
    }
    CHECK(pipe.PendingCount(w.key) == 1);

    RgbFrame pending;
    CHECK(pipe.Pending(w.key, &pending));
    CHECK(pending.sequence == 5);

    DeliveryStats st;
    CHECK(pipe.Stats(w.key, st));
    CHECK(st.submitted == 5);
    CHECK(st.replaced_pending == 4);
    CHECK(st.rejected == 0);

    /* The drain delivers exactly the newest frame, once. */
    CHECK(pipe.DrainDue(w.key) == 1);
    const auto delivered = transport->Delivered(w.key);
    CHECK(delivered.size() == 1);
    CHECK(delivered[0].sequence == 5);
    CHECK(pipe.PendingCount(w.key) == 0);
    CHECK(pipe.Stats(w.key, st) && st.transport_completed == 1);
}

/* A late-waking worker samples the CURRENT state — the newest
   pending frame with its newest sample time — never replaying
   a backlog of stale frames (plan section 4). */
static void TestLateWakeSamplesLatest()
{
    auto transport = std::make_shared<FakeTransport>();
    DeliveryPipeline pipe(transport);
    const DeviceSnapshot w = Wireless();
    pipe.RegisterDevice(w);

    const TimePoint t1{};
    const TimePoint t2 = t1 + 16ms;
    const TimePoint t3 = t1 + 33ms;
    /* Three scene ticks arrive while the lane is undrained —
       only the freshest survives. */
    pipe.SubmitLatest(MakeFrame(w, 1, { 10, 0, 0 }, t1));
    pipe.SubmitLatest(MakeFrame(w, 2, { 20, 0, 0 }, t2));
    pipe.SubmitLatest(MakeFrame(w, 3, { 30, 0, 0 }, t3));

    CHECK(pipe.DrainDue(w.key) == 1);
    const auto delivered = transport->Delivered(w.key);
    CHECK(delivered.size() == 1);
    CHECK(delivered[0].sequence == 3);
    CHECK(delivered[0].sample_time == t3);

    /* Next drain has nothing — no backlog exists to replay. */
    CHECK(pipe.DrainDue(w.key) == 0);
    CHECK(transport->DeliveredCount(w.key) == 1);
}

/* Identical consecutive frames are not retransmitted; the
   reliable convergence path always is. */
static void TestUnchangedSuppression()
{
    auto transport = std::make_shared<FakeTransport>();
    DeliveryPipeline pipe(transport);
    const DeviceSnapshot w = Wireless();
    pipe.RegisterDevice(w);

    const RgbColor red{ 200, 0, 0 };
    const RgbColor blue{ 0, 0, 200 };

    pipe.SubmitLatest(MakeFrame(w, 1, red));
    CHECK(pipe.DrainDue(w.key) == 1);       /* first send: red   */

    /* Same colors, newer sequence -> suppressed. */
    pipe.SubmitLatest(MakeFrame(w, 2, red));
    CHECK(pipe.DrainDue(w.key) == 0);
    CHECK(transport->DeliveredCount(w.key) == 1);
    DeliveryStats st;
    CHECK(pipe.Stats(w.key, st));
    CHECK(st.suppressed == 1);
    CHECK(st.transport_completed == 1);

    /* Changed colors -> delivered. */
    pipe.SubmitLatest(MakeFrame(w, 3, blue));
    CHECK(pipe.DrainDue(w.key) == 1);
    CHECK(transport->DeliveredCount(w.key) == 2);

    /* Convergence carve-out: a RELIABLE frame re-asserting the
       delivered color is never suppressed — the device must
       converge even when the bytes would repeat. */
    pipe.SubmitReliable(MakeFrame(w, 4, blue));
    CHECK(pipe.DrainDue(w.key) == 1);
    CHECK(transport->DeliveredCount(w.key) == 3);

    /* ...and the reliable send refreshes delivered-state memory,
       so the following identical live frame suppresses again. */
    pipe.SubmitLatest(MakeFrame(w, 5, blue));
    CHECK(pipe.DrainDue(w.key) == 0);
    CHECK(pipe.Stats(w.key, st));
    CHECK(st.suppressed == 2);
    CHECK(st.transport_completed == 3);
}

/* Reliable frames are not paced by the lane budget. */
static void TestReliableBypassesBudget()
{
    auto transport = std::make_shared<FakeTransport>();
    DeliveryPipeline pipe(transport);
    const DeviceSnapshot w = Wireless();
    pipe.RegisterDevice(w);
    pipe.SetBudget(w.key, std::chrono::hours(1));  /* extreme */

    const TimePoint t0{};
    pipe.SubmitLatest(MakeFrame(w, 1, { 1, 1, 1 }));
    CHECK(pipe.DrainDue(w.key, t0) == 1);

    /* A live frame far inside the budget window is held back. */
    pipe.SubmitLatest(MakeFrame(w, 2, { 2, 2, 2 }));
    CHECK(pipe.DrainDue(w.key, t0 + 1ms) == 0);
    CHECK(transport->DeliveredCount(w.key) == 1);

    /* A reliable frame delivers immediately regardless. */
    pipe.SubmitReliable(MakeFrame(w, 3, { 3, 3, 3 }));
    CHECK(pipe.DrainDue(w.key, t0 + 2ms) == 1);
    const auto delivered = transport->Delivered(w.key);
    CHECK(delivered.size() == 2);
    CHECK(delivered[1].sequence == 3);
}

/* Independent per-controller budgets: a 100 ms wireless lane
   and a 10 ms wired lane keep their own rates on a shared
   synthetic clock. */
static void TestIndependentBudgets()
{
    auto transport = std::make_shared<FakeTransport>();
    DeliveryPipeline pipe(transport);
    const DeviceSnapshot w = Wireless();
    const DeviceSnapshot f = Wired();
    pipe.RegisterDevice(w);
    pipe.RegisterDevice(f);
    pipe.SetBudget(w.key, 100ms);
    pipe.SetBudget(f.key, 10ms);

    const TimePoint t0{};
    pipe.SubmitLatest(MakeFrame(w, 1, { 1, 0, 0 }));
    pipe.SubmitLatest(MakeFrame(f, 1, { 1, 0, 0 }));
    CHECK(pipe.DrainAllDue(t0) == 2);       /* first sends unpaced */

    pipe.SubmitLatest(MakeFrame(w, 2, { 2, 0, 0 }));
    pipe.SubmitLatest(MakeFrame(f, 2, { 2, 0, 0 }));
    /* At +50 ms the wired lane is due (10 ms budget); the
       wireless lane is not (100 ms) and keeps its frame pending. */
    CHECK(pipe.DrainAllDue(t0 + 50ms) == 1);
    CHECK(transport->DeliveredCount(f.key) == 2);
    CHECK(transport->DeliveredCount(w.key) == 1);
    CHECK(pipe.PendingCount(w.key) == 1);

    pipe.SubmitLatest(MakeFrame(f, 3, { 3, 0, 0 }));
    /* +55 ms: wired sent at +50, next due at +60 — not yet. */
    CHECK(pipe.DrainAllDue(t0 + 55ms) == 0);
    /* +60 ms: wired due again while wireless still waits. */
    CHECK(pipe.DrainAllDue(t0 + 60ms) == 1);
    CHECK(transport->DeliveredCount(f.key) == 3);

    /* +150 ms: the wireless budget finally elapses — the held
       frame (seq 2) is delivered, not dropped. */
    CHECK(pipe.DrainAllDue(t0 + 150ms) == 1);
    const auto wdel = transport->Delivered(w.key);
    CHECK(wdel.size() == 2 && wdel[1].sequence == 2);

    DeliveryStats ws, fs;
    CHECK(pipe.Stats(w.key, ws) && ws.transport_completed == 2);
    CHECK(pipe.Stats(f.key, fs) && fs.transport_completed == 3);
    /* Honest interval accounting per controller: wired ~10 ms
       gaps, wireless ~150 ms. */
    CHECK(fs.last_interval_s > 0.0 && fs.last_interval_s < 0.05);
    CHECK(ws.last_interval_s > 0.1);
}

/* Validation + out-of-order guard + per-controller isolation. */
static void TestValidationAndStats()
{
    auto transport = std::make_shared<FakeTransport>();
    DeliveryPipeline pipe(transport);
    const DeviceSnapshot w = Wireless();
    const DeviceSnapshot f = Wired();

    /* Unknown controller. */
    CHECK(pipe.SubmitLatest(MakeFrame(w, 1, { 1, 0, 0 })).status ==
          SubmitStatus::Rejected);

    pipe.RegisterDevice(w);
    pipe.RegisterDevice(f);

    /* Stale topology generation. */
    RgbFrame stale = MakeFrame(w, 1, { 1, 0, 0 });
    stale.topology_generation = 0;
    CHECK(pipe.SubmitLatest(stale).status == SubmitStatus::Rejected);

    /* LED-count mismatch and empty frame. */
    RgbFrame shortf = MakeFrame(w, 2, { 1, 0, 0 });
    shortf.colors.resize(40);
    CHECK(pipe.SubmitLatest(shortf).status == SubmitStatus::Rejected);
    RgbFrame empty = MakeFrame(w, 3, { 1, 0, 0 });
    empty.colors.clear();
    CHECK(pipe.SubmitLatest(empty).status == SubmitStatus::Rejected);

    /* Disconnected lane. */
    DeviceSnapshot off = f;
    off.connection = ConnectionState::Disconnected;
    pipe.RegisterDevice(off);
    CHECK(pipe.SubmitLatest(MakeFrame(f, 1, { 1, 0, 0 })).status ==
          SubmitStatus::Rejected);

    /* Out-of-order: admitted seq 5, then seq 4 -> Rejected and
       never displaces the newer pending frame. */
    CHECK(pipe.SubmitLatest(MakeFrame(w, 5, { 5, 0, 0 })).status ==
          SubmitStatus::Accepted);
    CHECK(pipe.SubmitLatest(MakeFrame(w, 4, { 4, 0, 0 })).status ==
          SubmitStatus::Rejected);
    RgbFrame pending;
    CHECK(pipe.Pending(w.key, &pending));
    CHECK(pending.sequence == 5);

    /* Per-controller stats are independent of each other and of
       the aggregate. */
    DeliveryStats ws, fs, agg = pipe.Stats();
    CHECK(pipe.Stats(w.key, ws));
    CHECK(pipe.Stats(f.key, fs));
    CHECK(ws.submitted == 1 && ws.rejected == 4);
    CHECK(fs.submitted == 0 && fs.rejected == 1);
    CHECK(agg.submitted == ws.submitted + fs.submitted);
    /* +1: the unknown-controller rejection has no lane to
       charge — it counts in the aggregate only. */
    CHECK(agg.rejected  == ws.rejected + fs.rejected + 1);
    CHECK(agg.provenance == MeasurementProvenance::TransportMeasured);
}

/* Topology change drops pending frames and resets delivered
   state + the sequence guard. */
static void TestTopologyChange()
{
    auto transport = std::make_shared<FakeTransport>();
    DeliveryPipeline pipe(transport);
    DeviceSnapshot w = Wireless();
    pipe.RegisterDevice(w);

    const RgbColor red{ 100, 0, 0 };
    pipe.SubmitLatest(MakeFrame(w, 1, red));
    pipe.DrainDue(w.key);
    pipe.SubmitLatest(MakeFrame(w, 2, { 2, 0, 0 }));
    CHECK(pipe.PendingCount(w.key) == 1);

    w.topology_generation = 2;
    pipe.RegisterDevice(w);
    CHECK(pipe.PendingCount(w.key) == 0);

    /* Old-generation frames are rejected; the sequence guard
       reset admits a fresh low sequence under the new
       generation; delivered-state memory reset means even a
       previously-sent color transmits again. */
    RgbFrame old = MakeFrame(w, 3, { 3, 0, 0 });
    old.topology_generation = 1;
    CHECK(pipe.SubmitLatest(old).status == SubmitStatus::Rejected);
    CHECK(pipe.SubmitLatest(MakeFrame(w, 1, red)).status ==
          SubmitStatus::Accepted);
    CHECK(pipe.DrainDue(w.key) == 1);
    CHECK(transport->DeliveredCount(w.key) == 2);
}

/* SyncDevices removes lanes whose controllers vanished. */
static void TestSyncDevices()
{
    DeliveryPipeline pipe(std::make_shared<FakeTransport>());
    const DeviceSnapshot w = Wireless();
    const DeviceSnapshot f = Wired();
    pipe.RegisterDevice(w);
    pipe.RegisterDevice(f);
    CHECK(pipe.HasDevice(w.key) && pipe.HasDevice(f.key));

    pipe.SyncDevices({ w });
    CHECK(pipe.HasDevice(w.key));
    CHECK(!pipe.HasDevice(f.key));
    CHECK(pipe.SubmitLatest(MakeFrame(f, 1, { 1, 0, 0 })).status ==
          SubmitStatus::Rejected);
}

/* A reliable frame is delivered ahead of pending live work —
   and the older live frame behind it is then stale and
   suppressed rather than retransmitted over newer state. */
static void TestReliablePriority()
{
    auto transport = std::make_shared<FakeTransport>();
    DeliveryPipeline pipe(transport);
    const DeviceSnapshot w = Wireless();
    pipe.RegisterDevice(w);

    /* Establish delivered state at seq 10. */
    pipe.SubmitLatest(MakeFrame(w, 10, { 1, 0, 0 }));
    pipe.DrainDue(w.key);

    pipe.SubmitLatest(MakeFrame(w, 11, { 2, 0, 0 }));   /* pending */
    pipe.SubmitReliable(MakeFrame(w, 12, { 3, 0, 0 })); /* reliable */

    /* Pending() reports the next-to-send frame: the reliable one. */
    RgbFrame next;
    CHECK(pipe.Pending(w.key, &next));
    CHECK(next.sequence == 12);

    CHECK(pipe.DrainDue(w.key) == 1);                   /* reliable */
    /* The live seq 11 is now older than delivered seq 12 —
       suppressed, not sent over the newer state. */
    CHECK(pipe.DrainDue(w.key) == 0);

    const auto delivered = transport->Delivered(w.key);
    CHECK(delivered.size() == 2);
    CHECK(delivered[1].sequence == 12);
    DeliveryStats st;
    CHECK(pipe.Stats(w.key, st));
    CHECK(st.suppressed == 1);
    CHECK(st.transport_completed == 2);
}

/* With no transport configured, drained frames count as
   transport-rejected — never as completions. */
static void TestNoTransport()
{
    DeliveryPipeline pipe;   /* no transport */
    const DeviceSnapshot w = Wireless();
    pipe.RegisterDevice(w);

    CHECK(pipe.SubmitLatest(MakeFrame(w, 1, { 1, 0, 0 })).status ==
          SubmitStatus::Accepted);
    CHECK(pipe.DrainDue(w.key) == 1);
    DeliveryStats st;
    CHECK(pipe.Stats(w.key, st));
    CHECK(st.transport_completed == 0);
    CHECK(st.transport_rejected == 1);
}

/* Threaded acceptance: a wireless lane blocked inside a slow
   transport send cannot stall the fast wired lane — wired
   keeps its rate while wireless submissions collapse into
   the depth-1 pending slot. */
static void TestBlockedWirelessKeepsWiredRate()
{
    auto transport = std::make_shared<FakeTransport>();
    DeliveryPipeline pipe(transport);
    const DeviceSnapshot w = Wireless();
    const DeviceSnapshot f = Wired();
    pipe.RegisterDevice(w);
    pipe.RegisterDevice(f);
    /* Wireless paced like the measured ~33.5/s ceiling; wired
       unpaced. */
    pipe.SetBudget(w.key, 30ms);
    pipe.SetBudget(f.key, 0ms);
    CHECK(pipe.Start());

    transport->Block(w.key);

    /* The first wireless frame goes in-flight and parks inside
       the transport send. */
    pipe.SubmitLatest(MakeFrame(w, 1, { 1, 0, 0 }));
    const auto wait_deadline = std::chrono::steady_clock::now() + 2s;
    while(transport->BlockedEntries() == 0 &&
          std::chrono::steady_clock::now() < wait_deadline)
    {
        std::this_thread::sleep_for(2ms);
    }
    CHECK(transport->BlockedEntries() == 1);

    /* Subsequent wireless submissions collapse into the slot. */
    pipe.SubmitLatest(MakeFrame(w, 2, { 2, 0, 0 }));
    pipe.SubmitLatest(MakeFrame(w, 3, { 3, 0, 0 }));
    pipe.SubmitLatest(MakeFrame(w, 4, { 4, 0, 0 }));
    RgbFrame pending;
    CHECK(pipe.Pending(w.key, &pending));
    CHECK(pending.sequence == 4);
    CHECK(pipe.PendingCount(w.key) == 1);

    /* Meanwhile the wired lane keeps delivering at its own rate —
       ~150 ms of 5 ms-spaced submissions. */
    for(uint64_t seq = 1; seq <= 20; ++seq)
    {
        pipe.SubmitLatest(MakeFrame(f, seq, { uint8_t(seq), 0, 0 }));
        std::this_thread::sleep_for(5ms);
    }
    const size_t wired_delivered = transport->DeliveredCount(f.key);
    CHECK(wired_delivered >= 10);
    /* Wireless delivered nothing yet — its lane is still parked. */
    CHECK(transport->DeliveredCount(w.key) == 0);

    /* Release the blocked send: the lane completes the in-flight
       frame, then delivers only the LATEST pending (seq 4) —
       seqs 2 and 3 were replaced, never queued. */
    transport->Release();
    CHECK(transport->WaitDelivered(w.key, 2, 2s));
    const auto wdel = transport->Delivered(w.key);
    CHECK(wdel.size() == 2);
    CHECK(wdel[0].sequence == 1);   /* the in-flight send       */
    CHECK(wdel[1].sequence == 4);   /* newest pending — latest  */

    pipe.Stop();
    CHECK(!pipe.Running());
}

/* Regression: DrainDue/DrainAllDue ran on a raw Lane* across
   the unlocked transport call — a concurrent RemoveDevice /
   SyncDevices could free the lane mid-drain. Lanes are now
   shared_ptr so the drain itself extends the lifetime; this
   hammers the interleaving. */
static void TestDrainVsDeviceRemoval()
{
    auto transport = std::make_shared<FakeTransport>();
    transport->on_send = [] { std::this_thread::yield(); };
    DeliveryPipeline pipe(transport);
    const DeviceSnapshot w = Wireless();
    const DeviceSnapshot f = Wired();
    pipe.RegisterDevice(w);
    pipe.RegisterDevice(f);

    std::atomic<bool> run{ true };
    std::thread drainer([&]
    {
        uint64_t seq = 0;
        while(run.load())
        {
            /* Seq keeps climbing so re-registered lanes (guard
               reset) still admit; removed lanes reject. */
            pipe.SubmitLatest(MakeFrame(w, ++seq, { 1, 0, 0 }));
            pipe.DrainDue(w.key);
            pipe.DrainAllDue();
        }
    });
    for(int i = 0; i < 2000; ++i)
    {
        pipe.SyncDevices({ w, f });
        pipe.SyncDevices({ f });
        pipe.SyncDevices({});
    }
    run = false;
    drainer.join();

    /* Reaching here without heap corruption is the test — the
       pre-fix path dereferenced a freed Lane*. End state is
       deterministic: the final SyncDevices emptied the map. */
    CHECK(!pipe.HasDevice(w.key));
    CHECK(!pipe.HasDevice(f.key));
}

/* Regression: Stop()/RemoveDevice join lane threads, and a
   lane inside transport->SubmitLatest cannot be interrupted —
   the honest bound is "in-flight send + join". The production
   transport bounds the send itself via SO_SNDTIMEO
   (io_timeout_ms); here a fake parks the send and Stop() must
   wait for it, then join promptly once released. */
static void TestStopBoundedByInFlightSend()
{
    auto transport = std::make_shared<FakeTransport>();
    DeliveryPipeline pipe(transport);
    const DeviceSnapshot w = Wireless();
    pipe.RegisterDevice(w);
    CHECK(pipe.Start());

    transport->Block(w.key);
    pipe.SubmitLatest(MakeFrame(w, 1, { 1, 0, 0 }));
    const auto entered_deadline =
        std::chrono::steady_clock::now() + 2s;
    while(transport->BlockedEntries() == 0 &&
          std::chrono::steady_clock::now() < entered_deadline)
    {
        std::this_thread::sleep_for(2ms);
    }
    CHECK(transport->BlockedEntries() == 1);

    std::atomic<bool> stopped{ false };
    std::thread         stopper([&]
    {
        pipe.Stop();
        stopped = true;
    });
    /* Stop waits for the in-flight send — it must NOT return
       while the send is still parked. */
    std::this_thread::sleep_for(100ms);
    CHECK(!stopped.load());

    /* Once the send completes (in production: returns or hits
       SO_SNDTIMEO), the join finishes promptly. */
    transport->Release();
    const auto stop_deadline = std::chrono::steady_clock::now() + 5s;
    while(!stopped.load() &&
          std::chrono::steady_clock::now() < stop_deadline)
    {
        std::this_thread::sleep_for(2ms);
    }
    CHECK(stopped.load());
    stopper.join();
    CHECK(!pipe.Running());
}

int main()
{
    TestBurstCollapse();
    TestLateWakeSamplesLatest();
    TestUnchangedSuppression();
    TestReliableBypassesBudget();
    TestIndependentBudgets();
    TestValidationAndStats();
    TestTopologyChange();
    TestSyncDevices();
    TestReliablePriority();
    TestNoTransport();
    TestBlockedWirelessKeepsWiredRate();
    TestDrainVsDeviceRemoval();
    TestStopBoundedByInFlightSend();
    std::printf("delivery pipeline: %d checks, %d failures\n",
                checks, failures);
    return failures ? 1 : 0;
}
