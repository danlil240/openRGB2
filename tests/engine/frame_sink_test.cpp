// SPDX-License-Identifier: GPL-2.0-or-later
//
// Contract tests for the submitLatest(RgbFrame) sinks
// (src/engine/FrameSink.h): NullFrameSink and the bounded
// LatestFrameSink pending slot. Qt-free, hardware-independent.

#include "Engine.h"
#include "FrameSink.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace openrgb2;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

static DeviceSnapshot WirelessGroup()
{
    /* The real wireless target: one controller, 3 x 40-LED zones. */
    DeviceSnapshot s;
    s.key.device = "lianli-slw-group";
    s.key.serial = "s1";
    s.topology_generation = 1;
    s.zones = { { "fan-1", 40 }, { "fan-2", 40 }, { "fan-3", 40 } };
    s.connection = ConnectionState::Connected;
    return s;
}

static RgbFrame MakeFrame(const DeviceSnapshot& s, uint64_t seq,
                          RgbColor fill)
{
    RgbFrame f;
    f.controller           = s.key;
    f.topology_generation  = s.topology_generation;
    f.sequence             = seq;
    f.sample_time          = Clock::now();
    f.colors.assign(s.LedCount(), fill);
    return f;
}

static void TestNullSink()
{
    NullFrameSink sink;
    RgbFrame f;                       /* deliberately unvalidated */
    f.colors = { RgbColor{ 1, 2, 3 } };

    CHECK(sink.SubmitLatest(f).status == SubmitStatus::Accepted);
    CHECK(sink.SubmitLatest(f).status == SubmitStatus::Accepted);

    const DeliveryStats d = sink.Stats();
    CHECK(d.submitted == 2);
    CHECK(d.replaced_pending == 0);
    CHECK(d.rejected == 0);
    /* A null sink transports nothing: no fabricated completions
       and provenance stays Unknown. */
    CHECK(d.transport_completed == 0);
    CHECK(d.provenance == MeasurementProvenance::Unknown);
}

static void TestLatestSinkValidation()
{
    LatestFrameSink sink;
    const DeviceSnapshot snap = WirelessGroup();
    RgbFrame f = MakeFrame(snap, 1, { 10, 0, 0 });

    /* Unknown controller -> rejected with reason. */
    CHECK(sink.SubmitLatest(f).status == SubmitStatus::Rejected);
    CHECK(sink.Stats().rejected == 1);
    CHECK(sink.PendingCount() == 0);

    sink.RegisterDevice(snap);

    /* Stale topology generation -> rejected, never delivered. */
    RgbFrame stale = f;
    stale.topology_generation = 0;
    SubmitResult r = sink.SubmitLatest(stale);
    CHECK(r.status == SubmitStatus::Rejected && !r.reason.empty());

    /* LED-count mismatch -> rejected (a complete-controller frame
       must cover all 120 LEDs). */
    RgbFrame short_frame = f;
    short_frame.colors.resize(40);
    CHECK(sink.SubmitLatest(short_frame).status == SubmitStatus::Rejected);

    RgbFrame empty = f;
    empty.colors.clear();
    CHECK(sink.SubmitLatest(empty).status == SubmitStatus::Rejected);
    CHECK(sink.Stats().rejected == 4);
}

static void TestLatestSinkReplacement()
{
    LatestFrameSink sink;
    const DeviceSnapshot snap = WirelessGroup();
    sink.RegisterDevice(snap);

    /* First submission fills the depth-1 pending slot. */
    CHECK(sink.SubmitLatest(MakeFrame(snap, 1, { 10, 0, 0 })).status ==
          SubmitStatus::Accepted);
    CHECK(sink.PendingCount() == 1);

    /* A second submission before delivery replaces it — the pending
       frame is obsolete, never queued behind. */
    CHECK(sink.SubmitLatest(MakeFrame(snap, 2, { 20, 0, 0 })).status ==
          SubmitStatus::ReplacedPending);
    CHECK(sink.PendingCount() == 1);

    RgbFrame pending;
    CHECK(sink.Pending(snap.key, &pending));
    CHECK(pending.sequence == 2);
    CHECK((pending.colors[0] == RgbColor{ 20, 0, 0 }));

    const DeliveryStats d = sink.Stats();
    CHECK(d.submitted == 2);
    CHECK(d.replaced_pending == 1);
    CHECK(d.rejected == 0);

    /* Per-controller stats mirror the aggregate for the single
       registered device. */
    DeliveryStats per;
    CHECK(sink.Stats(snap.key, per));
    CHECK(per.submitted == 2 && per.replaced_pending == 1);
    CHECK(!sink.Stats(DeviceKey{ "nope", "", "", "" }, per));
}

static void TestLatestSinkSequenceGuard()
{
    LatestFrameSink sink;
    const DeviceSnapshot snap = WirelessGroup();
    sink.RegisterDevice(snap);

    CHECK(sink.SubmitLatest(MakeFrame(snap, 5, { 1, 0, 0 })).status ==
          SubmitStatus::Accepted);
    /* An older sequence is obsolete — it must not displace the
       newer pending frame. */
    CHECK(sink.SubmitLatest(MakeFrame(snap, 4, { 9, 0, 0 })).status ==
          SubmitStatus::Rejected);
    RgbFrame pending;
    CHECK(sink.Pending(snap.key, &pending));
    CHECK(pending.sequence == 5);
    /* An equal sequence still replaces (same vintage, latest
       arrival wins). */
    CHECK(sink.SubmitLatest(MakeFrame(snap, 5, { 2, 0, 0 })).status ==
          SubmitStatus::ReplacedPending);

    sink.DrainPending(nullptr);

    /* The guard also covers already-drained state: seq 3 < the
       admitted high-water 5 — still obsolete. */
    CHECK(sink.SubmitLatest(MakeFrame(snap, 3, { 7, 0, 0 })).status ==
          SubmitStatus::Rejected);

    /* A topology-generation change resets the guard. */
    DeviceSnapshot next = snap;
    next.topology_generation = 2;
    sink.RegisterDevice(next);
    RgbFrame f = MakeFrame(next, 1, { 1, 0, 0 });
    CHECK(sink.SubmitLatest(f).status == SubmitStatus::Accepted);
}

static void TestLatestSinkDrainRestore()
{
    LatestFrameSink sink;
    const DeviceSnapshot snap = WirelessGroup();
    sink.RegisterDevice(snap);

    sink.SubmitLatest(MakeFrame(snap, 1, { 1, 0, 0 }));
    sink.SubmitLatest(MakeFrame(snap, 2, { 2, 0, 0 }));

    int calls = 0;
    const auto throwing = [&calls](const RgbFrame&)
    {
        ++calls;
        throw std::runtime_error("deliver failed");
    };
    bool threw = false;
    try
    {
        sink.DrainPending(throwing);
    }
    catch(const std::runtime_error&)
    {
        threw = true;
    }
    CHECK(threw && calls == 1);
    /* The throw restored the undelivered frame to its slot. */
    RgbFrame pending;
    CHECK(sink.Pending(snap.key, &pending));
    CHECK(pending.sequence == 2);
    CHECK(sink.Stats().transport_completed == 0);

    /* Draining with no deliver callback drops without fabricating
       completions — the frame counts as suppressed. */
    sink.SubmitLatest(MakeFrame(snap, 3, { 3, 0, 0 }));
    CHECK(sink.DrainPending(nullptr) == 1);
    const DeliveryStats d = sink.Stats();
    CHECK(d.transport_completed == 0);
    CHECK(d.suppressed == 1);
}

static void TestLatestSinkDrain()
{
    LatestFrameSink sink;
    const DeviceSnapshot snap = WirelessGroup();
    sink.RegisterDevice(snap);

    std::vector<RgbFrame> delivered;
    const auto collect = [&delivered](const RgbFrame& f)
    {
        delivered.push_back(f);
    };

    /* Nothing pending -> drains zero. */
    CHECK(sink.DrainPending(collect) == 0);
    CHECK(sink.Stats().transport_completed == 0);

    sink.SubmitLatest(MakeFrame(snap, 1, { 1, 0, 0 }));
    sink.SubmitLatest(MakeFrame(snap, 2, { 2, 0, 0 })); /* replaces */
    CHECK(sink.DrainPending(collect) == 1);
    CHECK(delivered.size() == 1 && delivered[0].sequence == 2);
    CHECK(sink.PendingCount() == 0);

    /* After the drain the slot is empty again: next submit is a
       fresh accept, not another replacement. */
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(sink.SubmitLatest(MakeFrame(snap, 3, { 3, 0, 0 })).status ==
          SubmitStatus::Accepted);
    CHECK(sink.DrainPending(collect) == 1);

    const DeliveryStats d = sink.Stats();
    CHECK(d.submitted == 3);
    CHECK(d.replaced_pending == 1);
    CHECK(d.transport_completed == 2);
    CHECK(d.IntervalCount() == 1);
    CHECK(d.last_interval_s > 0.0);

    /* A disconnected controller rejects new frames. */
    DeviceSnapshot off = snap;
    off.connection = ConnectionState::Disconnected;
    sink.RegisterDevice(off);
    CHECK(sink.SubmitLatest(MakeFrame(snap, 4, { 4, 0, 0 })).status ==
          SubmitStatus::Rejected);
}

static void TestLatestSinkTopologyChange()
{
    LatestFrameSink sink;
    DeviceSnapshot snap = WirelessGroup();
    sink.RegisterDevice(snap);
    sink.SubmitLatest(MakeFrame(snap, 1, { 1, 0, 0 }));
    CHECK(sink.PendingCount() == 1);

    /* Re-registering with a NEW topology generation drops the
       obsolete pending frame (plan section 6: stop obsolete frames
       on topology change). */
    snap.topology_generation = 2;
    sink.RegisterDevice(snap);
    CHECK(sink.PendingCount() == 0);

    /* Old-generation frames are now rejected. */
    CHECK(sink.SubmitLatest(MakeFrame(snap, 2, { 1, 0, 0 })).status ==
          SubmitStatus::Accepted);
    RgbFrame old = MakeFrame(snap, 3, { 1, 0, 0 });
    old.topology_generation = 1;
    CHECK(sink.SubmitLatest(old).status == SubmitStatus::Rejected);
}

static void TestEngineSinkPlumbing()
{
    /* The Engine default is a NullFrameSink; SetFrameSink swaps it. */
    Engine engine;
    RgbFrame f;
    f.colors = { RgbColor{ 9, 9, 9 } };
    CHECK(engine.SubmitLatest(f).status == SubmitStatus::Accepted);

    auto latest = std::make_shared<LatestFrameSink>();
    const DeviceSnapshot snap = WirelessGroup();
    latest->RegisterDevice(snap);
    engine.SetFrameSink(latest);
    CHECK(engine.SubmitLatest(MakeFrame(snap, 1, { 5, 0, 0 })).status ==
          SubmitStatus::Accepted);
    CHECK(latest->Stats().submitted == 1);
}

int main()
{
    TestNullSink();
    TestLatestSinkValidation();
    TestLatestSinkReplacement();
    TestLatestSinkSequenceGuard();
    TestLatestSinkDrainRestore();
    TestLatestSinkDrain();
    TestLatestSinkTopologyChange();
    TestEngineSinkPlumbing();
    std::printf("frame sink: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
