// SPDX-License-Identifier: GPL-2.0-or-later
//
// Power/lifecycle tests (Task 3.7 — src/engine/PowerLifecycle.h +
// the DeliveryPipeline output gate + the PlaybackController power
// interface). Deterministic: injected power events, a fake backend,
// a fake transport sink, a synthetic clock — no Win32, no hardware.
//
// Covers the Task 3.7 acceptance list:
//   - display-off discards pending frames and gates ALL RGB output
//     (honest rejections, nothing reaches the transport while the
//     display sleeps)
//   - an active uploaded loop freezes exactly once — one mode write
//     + one sampled-hold frame, never a stream and never a
//     maintenance stop
//   - suspend discards pending work; resume revalidates identity
//     (refresh) BEFORE any restore write
//   - reconnect recovery drops stale pending, resets delivered
//     state, and re-asserts the last delivered frame through the
//     reliable lane — without restarting hardware the engine does
//     not own
//   - "stop maintenance traffic" is never an animation-stop
//     mechanism

#include "PowerLifecycle.h"
#include "DeliveryPipeline.h"
#include "DeviceMapper.h"      /* IsWirelessGroupController */
#include "PlaybackController.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace openrgb2;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

/*---------------------------------------------------------*\
|| Fakes                                                    ||
\*---------------------------------------------------------*/

struct FakeTransport final : public IFrameSink
{
    std::vector<RgbFrame> frames;

    SubmitResult SubmitLatest(const RgbFrame& frame) override
    {
        frames.push_back(frame);
        return SubmitResult::Accept();
    }
    DeliveryStats Stats() const override { return DeliveryStats{}; }
};

struct FakeBackend
{
    std::vector<DeviceSnapshot> snaps;
    BackendModeReport           report;
    int                         refresh_calls = 0;
    int                         custom_calls  = 0;
    std::vector<std::string>    selects;
    std::vector<PlaybackModePatch> patches;
    std::vector<RgbFrame>       direct;      /* transport-bypass */
    std::vector<std::string>    ops;         /* ordering log      */
};

static DeviceSnapshot WirelessSnap(uint64_t gen = 7)
{
    DeviceSnapshot s;
    s.key = DeviceKey{ "Lian Li: SL Wireless", "AABBCCDDEEFF",
                       "Wireless: AA:BB:CC:DD:EE:FF", "" };
    s.topology_generation = gen;
    s.zones = { { "Fan 1", 40 }, { "Fan 2", 40 }, { "Fan 3", 40 } };
    s.modes = { "Direct", "Static", "Spectrum Cycle",
                "Rainbow Wave", "Breathing" };
    s.connection = ConnectionState::Connected;
    return s;
}

static DeviceSnapshot WiredSnap(uint64_t gen = 7)
{
    DeviceSnapshot s;
    s.key = DeviceKey{ "ASUSTek: Aura MB", "", "usb:1-4", "" };
    s.zones = { { "Header", 16 } };
    s.modes = { "Direct", "Static" };
    s.topology_generation = gen;
    s.connection = ConnectionState::Connected;
    return s;
}

static RgbFrame FrameFor(const DeviceSnapshot& s, uint64_t seq,
                         RgbColor c)
{
    RgbFrame f;
    f.controller          = s.key;
    f.topology_generation = s.topology_generation;
    f.sequence            = seq;
    f.colors.assign(s.LedCount(), c);
    return f;
}

static std::shared_ptr<studio::SceneDocument>
MakeScene(const std::string& name, const std::string& preset)
{
    auto d = std::make_shared<studio::SceneDocument>();
    d->name = name;
    d->effect.preset = preset;
    return d;
}

struct Harness
{
    DeliveryPipeline  pipeline;
    FakeTransport     transport;
    FakeBackend       backend;
    TimePoint         now{};
    bool              capture_paused = false;

    std::shared_ptr<const studio::SceneDocument> scene;

    std::unique_ptr<PlaybackController> ctl;
    std::unique_ptr<PowerLifecycle>     life;

    Harness()
    {
        pipeline.SetTransport(
            std::shared_ptr<IFrameSink>(&transport,
                                        [](IFrameSink*){}));

        PlaybackController::Deps d;
        d.scene     = [this] { return scene; };
        d.snapshots = [this] { return backend.snaps; };
        d.wired_targets = [this]
        {
            std::vector<DeviceSnapshot> out;
            for(const DeviceSnapshot& s : backend.snaps)
            {
                if(!IsWirelessGroupController(s))
                {
                    out.push_back(s);
                }
            }
            return out;
        };
        d.select_mode =
            [this](const DeviceKey&, const std::string& name,
                   const PlaybackModePatch& patch, std::string& err)
            {
                (void)err;
                backend.selects.push_back(name);
                backend.patches.push_back(patch);
                backend.ops.push_back("select:" + name);
                return true;
            };
        d.active_mode =
            [this](const DeviceKey&, BackendModeReport& rep)
            { rep = backend.report; return true; };
        d.refresh_status =
            [this](std::string&)
            {
                ++backend.refresh_calls;
                backend.ops.push_back("refresh");
                return true;
            };
        d.set_custom_mode =
            [this](const DeviceKey&, std::string&)
            {
                ++backend.custom_calls;
                backend.ops.push_back("custom");
                backend.report.valid  = true;
                backend.report.name   = "Direct";
                backend.report.speed  = 0;
                backend.report.direction = 0;
                backend.report.colors.clear();
                return true;
            };
        d.direct_submit =
            [this](const RgbFrame& f)
            {
                backend.direct.push_back(f);
                backend.ops.push_back("direct");
                return SubmitResult::Accept();
            };
        d.wired_submit =
            [this](const RgbFrame& f)
            { return pipeline.SubmitLatest(f); };
        d.set_suppressed =
            [this](const DeviceKey& k, bool s, const std::string& r)
            { pipeline.SetSuppressed(k, s, r); };
        d.drop_pending =
            [this](const DeviceKey& k)
            { return pipeline.DropPending(k); };
        d.reset_delivered =
            [this](const DeviceKey& k)
            { pipeline.ResetDeliveredState(k); };
        d.wait_idle =
            [this](const DeviceKey& k, Clock::duration t)
            { return pipeline.WaitIdle(k, t); };
        d.now = [this] { return now; };
        ctl = std::make_unique<PlaybackController>(d);

        PowerLifecycle::Deps l;
        l.set_output_paused =
            [this](bool p, const std::string& r)
            { pipeline.SetOutputPaused(p, r); };
        l.lane_keys =
            [this] { return pipeline.LaneKeys(); };
        l.drop_pending =
            [this](const DeviceKey& k)
            { return pipeline.DropPending(k); };
        l.reset_delivered =
            [this](const DeviceKey& k)
            { pipeline.ResetDeliveredState(k); };
        l.last_delivered =
            [this](const DeviceKey& k, RgbFrame& out)
            { return pipeline.LastDelivered(k, out); };
        l.reliable_submit =
            [this](const RgbFrame& f)
            { return pipeline.SubmitReliable(f); };
        l.set_capture_paused =
            [this](bool p) { capture_paused = p; };
        l.snapshots = [this] { return backend.snaps; };
        l.playback  = ctl.get();
        life = std::make_unique<PowerLifecycle>(l);
    }

    /* One owner-loop step: the lifecycle sees the events +
       snapshots, then the controller runs its maintenance —
       the same order as main.cpp. */
    void Step()
    {
        now += std::chrono::milliseconds(600);
        life->Poll(now);
        ctl->Poll(now);
    }

    void ReportPending()
    {
        if(backend.selects.empty())
        {
            return;
        }
        backend.report.valid     = true;
        backend.report.name      = backend.selects.back();
        backend.report.speed     = 0;
        backend.report.direction = 0;
        backend.report.colors.clear();
        if(!backend.patches.empty())
        {
            const PlaybackModePatch& p = backend.patches.back();
            if(p.has_speed)     backend.report.speed = p.speed;
            if(p.has_direction) backend.report.direction = p.direction;
            if(p.has_colors)    backend.report.colors = p.colors;
        }
    }

    /* Drive the controller into an acknowledged Active
       animation (rainbow_wave uploaded). */
    void UploadActive()
    {
        scene = MakeScene("rainbow", "rainbow_wave");
        backend.report.valid = true;
        backend.report.name  = "Direct";
        PlaybackParams params;
        params.speed = 100;
        PlaybackController::Result r =
            ctl->Request(PlaybackMode::AnimationPlayback,
                         PlaybackControl::Playing, params);
        CHECK(r.accepted);
        ReportPending();
        Step();
        CHECK(ctl->Status().mode == PlaybackMode::AnimationPlayback);
        CHECK(ctl->Status().upload == UploadState::Active);
        CHECK(ctl->Status().control == PlaybackControl::Playing);
    }

    RgbColor Red() const { return RgbColor{ 0xFF, 0x00, 0x00 }; }
};

static size_t FramesFor(const FakeTransport& t, const DeviceKey& k)
{
    size_t n = 0;
    for(const RgbFrame& f : t.frames)
    {
        if(f.controller == k)
        {
            ++n;
        }
    }
    return n;
}

/*---------------------------------------------------------*\
|| T1: display-off discards pending + gates all output      ||
\*---------------------------------------------------------*/

static void TestDisplayOffGate()
{
    Harness h;
    const DeviceSnapshot ws = WirelessSnap();
    const DeviceSnapshot wd = WiredSnap();
    h.backend.snaps = { ws, wd };
    h.pipeline.RegisterDevice(ws);
    h.pipeline.RegisterDevice(wd);

    h.pipeline.SubmitLatest(FrameFor(ws, 1, h.Red()));
    h.pipeline.SubmitLatest(FrameFor(wd, 1, h.Red()));
    CHECK(h.pipeline.PendingCount() == 2);

    /* Display off: pending discarded, output gated, capture
       paused — all in the same owner-loop step. */
    h.life->PostEvent(PowerLifecycle::Event::DisplayOff);
    h.Step();
    CHECK(h.life->OutputPaused());
    CHECK(h.pipeline.OutputPaused());
    CHECK(h.capture_paused);
    CHECK(h.pipeline.PendingCount() == 0);
    CHECK(!h.life->Suspended());

    /* Nothing reaches the transport while gated — not the
       pre-gate leftovers (already dropped), not new work
       (honest rejection with reason). */
    CHECK(h.pipeline.DrainDue(ws.key, h.now) == 0);
    const SubmitResult r =
        h.pipeline.SubmitLatest(FrameFor(ws, 2, h.Red()));
    CHECK(r.status == SubmitStatus::Rejected);
    CHECK(!r.reason.empty());
    CHECK(FramesFor(h.transport, ws.key) == 0);
    CHECK(FramesFor(h.transport, wd.key) == 0);

    /* A SECOND DisplayOff is a no-op (state-based, idempotent). */
    h.life->PostEvent(PowerLifecycle::Event::DisplayOff);
    h.Step();
    CHECK(h.life->OutputPaused());

    /* Display on: gate releases, fresh frames flow. */
    h.life->PostEvent(PowerLifecycle::Event::DisplayOn);
    h.Step();
    CHECK(!h.life->OutputPaused());
    CHECK(!h.pipeline.OutputPaused());
    CHECK(!h.capture_paused);
    CHECK(h.pipeline.SubmitLatest(FrameFor(ws, 3, h.Red())).status
          == SubmitStatus::Accepted);
    CHECK(h.pipeline.DrainDue(ws.key, h.now) == 1);
    CHECK(FramesFor(h.transport, ws.key) == 1);
}

/*---------------------------------------------------------*\
|| T2: the uploaded loop freezes exactly once               ||
\*---------------------------------------------------------*/

static void TestFreezeOnce()
{
    Harness h;
    const DeviceSnapshot ws = WirelessSnap();
    const DeviceSnapshot wd = WiredSnap();
    h.backend.snaps = { ws, wd };
    h.pipeline.RegisterDevice(ws);
    h.pipeline.RegisterDevice(wd);
    h.UploadActive();
    const size_t selects_at_upload = h.backend.selects.size();
    const int    custom_at_upload  = h.backend.custom_calls;

    /* Display off: the freeze is ONE mode write (Direct) + ONE
       sampled-hold frame straight to the transport — the loop
       is paused by an explicit transition, never by starving
       traffic and never by a write stream. */
    h.life->PostEvent(PowerLifecycle::Event::DisplayOff);
    h.Step();
    CHECK(h.life->PowerHold());
    CHECK(h.ctl->Status().control == PlaybackControl::Paused);
    CHECK(h.backend.custom_calls == custom_at_upload + 1);
    CHECK(h.backend.direct.size() == 1);
    CHECK(h.backend.direct[0].controller == ws.key);
    CHECK(h.backend.selects.size() == selects_at_upload);
    /* The lane stays owned — the freeze never releases the
       group to live producers. */
    CHECK(h.pipeline.IsSuppressed(ws.key));

    /* Exactly once: a repeated DisplayOff freezes nothing
       again. */
    h.life->PostEvent(PowerLifecycle::Event::DisplayOff);
    h.Step();
    CHECK(h.backend.custom_calls == custom_at_upload + 1);
    CHECK(h.backend.direct.size() == 1);

    /* No frames streamed to the sleeping display: the lane is
       suppressed + output gated; the only wireless write was
       the single hold frame via the owner bypass. */
    for(int i = 0; i < 8; ++i)
    {
        h.Step();
    }
    CHECK(FramesFor(h.transport, ws.key) == 0);

    /* Display on: revalidation runs (refresh BEFORE any
       restore write), the backend honestly reports the freeze
       took it to Direct, so the loop is re-uploaded — once. */
    h.life->PostEvent(PowerLifecycle::Event::DisplayOn);
    const int refresh_before = h.backend.refresh_calls;
    h.Step();
    CHECK(h.backend.refresh_calls > refresh_before);
    CHECK(!h.life->RestorePending());
    CHECK(!h.life->PowerHold());
    CHECK(h.backend.selects.size() == selects_at_upload + 1);
    CHECK(h.backend.selects.back() == "Rainbow Wave");

    /* Ordering proof: after the freeze ("direct"), the
       revalidation refresh precedes the restore select. */
    size_t di = h.backend.ops.size(), ri = h.backend.ops.size(),
           si = h.backend.ops.size();
    for(size_t i = 0; i < h.backend.ops.size(); ++i)
    {
        if(h.backend.ops[i] == "direct" &&
           di == h.backend.ops.size())
        {
            di = i;
        }
        else if(h.backend.ops[i] == "refresh" &&
                ri == h.backend.ops.size() &&
                di != h.backend.ops.size() && i > di)
        {
            ri = i;
        }
        else if(h.backend.ops[i] == "select:Rainbow Wave" &&
                ri != h.backend.ops.size() && i > ri)
        {
            si = i;
        }
    }
    CHECK(di < ri && ri < si);

    /* The restored upload converges to Active/Playing. */
    h.ReportPending();
    h.Step();
    CHECK(h.ctl->Status().control == PlaybackControl::Playing);
    CHECK(h.ctl->Status().upload == UploadState::Active);
    CHECK(!h.life->OutputPaused());
}

/*---------------------------------------------------------*\
|| T3: suspend discards; resume revalidates BEFORE restore  ||
\*---------------------------------------------------------*/

static void TestSuspendResume()
{
    Harness h;
    const DeviceSnapshot ws = WirelessSnap();
    const DeviceSnapshot wd = WiredSnap();
    h.backend.snaps = { ws, wd };
    h.pipeline.RegisterDevice(ws);
    h.pipeline.RegisterDevice(wd);
    h.UploadActive();
    const size_t ops_at_upload = h.backend.ops.size();

    /* Live work pending on the wired lane at suspend time. */
    h.pipeline.SubmitLatest(FrameFor(wd, 1, h.Red()));
    CHECK(h.pipeline.PendingCount(wd.key) == 1);

    /* Suspend: pending frames discarded, output parked, the
       controller's maintenance gated — and NO stop/freeze
       write attempted across the sleep boundary (we never
       promise the loop stops). */
    h.life->PostEvent(PowerLifecycle::Event::Suspend);
    h.Step();
    CHECK(h.life->Suspended());
    CHECK(h.life->OutputPaused());
    CHECK(h.pipeline.PendingCount() == 0);
    CHECK(h.ctl->PowerGate());
    CHECK(h.backend.ops.size() == ops_at_upload); /* no writes */
    CHECK(!h.life->PowerHold());   /* suspend is not a freeze */

    /* The device drops off during the sleep window — the
       backend loses the uploaded loop (reports Direct). */
    h.backend.snaps = { wd };
    h.backend.report.name = "Direct";
    h.Step();                                   /* suspended */
    CHECK(h.life->Suspended());

    /* Wake with the group back but its loop gone: the resume
       path revalidates (refresh) BEFORE the restore write —
       and restores because the reported state diverged. */
    h.backend.snaps = { ws, wd };
    h.life->PostEvent(PowerLifecycle::Event::Resume);
    h.Step();
    CHECK(!h.life->Suspended());
    CHECK(!h.life->RestorePending());
    CHECK(!h.ctl->PowerGate());

    size_t ri = h.backend.ops.size(), si = h.backend.ops.size();
    for(size_t i = ops_at_upload; i < h.backend.ops.size(); ++i)
    {
        if(h.backend.ops[i] == "refresh" &&
           ri == h.backend.ops.size())
        {
            ri = i;
        }
        if(h.backend.ops[i] == "select:Rainbow Wave")
        {
            si = i;
        }
    }
    CHECK(ri < si);   /* revalidation strictly before restore */

    h.ReportPending();
    h.Step();
    CHECK(h.ctl->Status().upload == UploadState::Active);
    CHECK(!h.life->OutputPaused());
}

/*---------------------------------------------------------*\
|| T3b: resume with a still-consistent device writes nothing||
\*---------------------------------------------------------*/

static void TestResumeConsistent()
{
    Harness h;
    const DeviceSnapshot ws = WirelessSnap();
    const DeviceSnapshot wd = WiredSnap();
    h.backend.snaps = { ws, wd };
    h.pipeline.RegisterDevice(ws);
    h.pipeline.RegisterDevice(wd);
    h.UploadActive();
    const size_t ops_at_upload = h.backend.ops.size();

    /* Suspend + resume with the backend STILL reporting the
       uploaded loop: revalidation proves consistency and no
       restore write is issued — a loop the engine never lost
       is not restarted. */
    h.life->PostEvent(PowerLifecycle::Event::Suspend);
    h.Step();
    h.backend.report.name = "Rainbow Wave"; /* loop survived  */
    h.backend.report.speed = 100;
    h.life->PostEvent(PowerLifecycle::Event::Resume);
    h.Step();
    CHECK(!h.life->RestorePending());
    /* Post-suspend ops: the revalidation refresh only — no
       select/custom/direct restore writes. */
    bool refresh_seen = false;
    for(size_t i = ops_at_upload; i < h.backend.ops.size(); ++i)
    {
        CHECK(h.backend.ops[i] == "refresh");
        refresh_seen = true;
    }
    CHECK(refresh_seen);
    CHECK(!h.life->OutputPaused());
}

/*---------------------------------------------------------*\
|| T4: reconnect recovery — live lanes, no hardware takeover||
\*---------------------------------------------------------*/

static void TestReconnectResend()
{
    Harness h;
    const DeviceSnapshot ws = WirelessSnap();
    const DeviceSnapshot wd = WiredSnap();
    h.backend.snaps = { ws, wd };
    h.pipeline.RegisterDevice(ws);
    h.pipeline.RegisterDevice(wd);

    /* Live mode — the engine owns NO uploaded hardware state.
       Deliver a complete-group frame so delivered-state memory
       exists. */
    CHECK(h.pipeline.SubmitLatest(
              FrameFor(ws, 1, h.Red())).status
          == SubmitStatus::Accepted);
    CHECK(h.pipeline.DrainDue(ws.key, h.now) == 1);
    CHECK(FramesFor(h.transport, ws.key) == 1);

    h.Step();      /* baseline the connection diff */

    /* The group drops (device sleep, dongle reset) and returns
       at the SAME topology generation — the deferred 3.3 case
       where surviving delivered memory could eat the first
       post-reconnect frame. */
    h.backend.snaps = { wd };
    h.Step();
    h.backend.snaps = { ws, wd };

    /* Stale live work queued before the recovery point must be
       discarded, not delivered post-reconnect. */
    h.pipeline.SubmitLatest(FrameFor(wd, 50, h.Red()));
    h.Step();

    /* Recovery: last delivered state re-asserted through the
       reliable lane — it transmits even though it is identical
       to the lane's (reset) delivered memory. */
    CHECK(h.pipeline.DrainDue(ws.key, h.now) == 1);
    CHECK(FramesFor(h.transport, ws.key) == 2);
    CHECK(h.transport.frames.back().colors ==
          std::vector<RgbColor>(ws.LedCount(), h.Red()));

    /* ...and the queued wired frame was dropped by recovery,
       not delivered stale. */
    CHECK(FramesFor(h.transport, wd.key) == 0);

    /* The recovery touched no hardware state the engine does
       not own: no mode selects, no custom-mode asserts, no
       owner-bypass writes. */
    CHECK(h.backend.selects.empty());
    CHECK(h.backend.custom_calls == 0);
    CHECK(h.backend.direct.empty());
}

/*---------------------------------------------------------*\
|| T7: the power gate parks user requests — no write        ||
||     crosses the suspend boundary; the parked intent      ||
||     replays through the validated path post-wake (5.4)   ||
\*---------------------------------------------------------*/

static void TestSuspendParksRequest()
{
    Harness h;
    const DeviceSnapshot ws = WirelessSnap();
    const DeviceSnapshot wd = WiredSnap();
    h.backend.snaps = { ws, wd };
    h.pipeline.RegisterDevice(ws);
    h.pipeline.RegisterDevice(wd);
    h.UploadActive();
    const size_t ops_at_upload = h.backend.ops.size();

    /* Suspend: gate asserts. An IPC-side playback Request
       inside the suspend window must NOT write — it is
       validated, recorded, and parked for replay. */
    h.life->PostEvent(PowerLifecycle::Event::Suspend);
    h.Step();
    CHECK(h.ctl->PowerGate());

    PlaybackParams slower;
    slower.speed = 40;
    PlaybackController::Result r = h.ctl->Request(
        PlaybackMode::AnimationPlayback,
        PlaybackControl::Playing, slower);
    CHECK(r.accepted);                 /* parked, not refused  */
    CHECK(r.pending);
    CHECK(h.ctl->AckPending());        /* honest pending       */
    CHECK(h.ctl->RequestedMode() ==
          PlaybackMode::AnimationPlayback);
    /* Zero hardware writes across the boundary: no selects,
       no custom-mode asserts, no direct frames. */
    CHECK(h.backend.ops.size() == ops_at_upload);
    h.Step();
    CHECK(h.backend.ops.size() == ops_at_upload);

    /* A second parked request overwrites the first — latest
       intent wins, still zero writes. */
    PlaybackController::Result r2 = h.ctl->Request(
        PlaybackMode::AnimationPlayback,
        PlaybackControl::Stopped, slower);
    CHECK(r2.accepted && r2.pending);
    CHECK(h.ctl->IntendedControl() == PlaybackControl::Stopped);
    CHECK(h.backend.ops.size() == ops_at_upload);

    /* Resume: the wake path revalidates (refresh) BEFORE any
       write, then the parked intent lands — Stopped here, so
       the restore leaves the device in Direct, never a
       Playing loop the user cancelled mid-sleep. */
    h.life->PostEvent(PowerLifecycle::Event::Resume);
    h.Step();
    CHECK(!h.ctl->PowerGate());
    size_t ri = h.backend.ops.size(), wi = h.backend.ops.size();
    for(size_t i = ops_at_upload; i < h.backend.ops.size(); ++i)
    {
        if(h.backend.ops[i] == "refresh" &&
           ri == h.backend.ops.size())
        {
            ri = i;
        }
        if((h.backend.ops[i] == "custom" ||
            h.backend.ops[i].rfind("select:", 0) == 0 ||
            h.backend.ops[i] == "direct") &&
           wi == h.backend.ops.size())
        {
            wi = i;
        }
    }
    CHECK(ri < wi);   /* revalidation strictly before writes   */
    CHECK(h.ctl->Status().control == PlaybackControl::Stopped);

    /* Nothing else pending: the deferred slot is consumed. */
    for(int i = 0; i < 4; ++i)
    {
        h.Step();
    }
    CHECK(!h.ctl->AckPending());
}

/*---------------------------------------------------------*\
|| T8: parked request while NOT owning the group — a new    ||
||     animation selection made during suspend applies      ||
||     post-wake through the normal transition              ||
\*---------------------------------------------------------*/

static void TestSuspendParkedNewUpload()
{
    Harness h;
    const DeviceSnapshot ws = WirelessSnap();
    h.backend.snaps = { ws };
    h.pipeline.RegisterDevice(ws);
    h.scene = MakeScene("rainbow", "rainbow_wave");
    h.backend.report.valid = true;
    h.backend.report.name  = "Direct";

    h.life->PostEvent(PowerLifecycle::Event::Suspend);
    h.Step();
    CHECK(h.ctl->PowerGate());

    PlaybackParams params;
    params.speed = 60;
    PlaybackController::Result r = h.ctl->Request(
        PlaybackMode::AnimationPlayback,
        PlaybackControl::Playing, params);
    CHECK(r.accepted && r.pending);
    CHECK(h.backend.selects.empty());   /* nothing written   */

    /* Resume: no owned group to revalidate (NotOwned), so the
       parked request itself replays — the upload lands
       post-wake, never mid-suspend. */
    h.life->PostEvent(PowerLifecycle::Event::Resume);
    h.Step();
    CHECK(!h.backend.selects.empty());
    CHECK(h.backend.selects.back() == "Rainbow Wave");
    h.ReportPending();
    h.Step();
    CHECK(h.ctl->Status().upload == UploadState::Active);
    CHECK(h.ctl->Status().control == PlaybackControl::Playing);
}

/*---------------------------------------------------------*\
||| T9: a parked PAUSED request keeps the power hold — the   ||
|||     wake must NOT un-hold to Playing. This is the only   ||
|||     test where !HoldOverridden() is the deciding clause  ||
|||     in AttemptRestore: the parked intent is Paused, so   ||
|||     IntendedControl()==Paused already holds and only the ||
|||     flag stands between the user intent and the un-hold. ||
|||     (T7 parks Stopped — its first clause fails before    ||
|||     the flag is even consulted.)                         ||
|\*---------------------------------------------------------*/

static void TestParkedPauseKeepsHold()
{
    Harness h;
    const DeviceSnapshot ws = WirelessSnap();
    const DeviceSnapshot wd = WiredSnap();
    h.backend.snaps = { ws, wd };
    h.pipeline.RegisterDevice(ws);
    h.pipeline.RegisterDevice(wd);
    h.UploadActive();
    const size_t selects_at_upload = h.backend.selects.size();

    /* Display off: the power freeze parks the loop at Paused
       (control_ AND intended both Paused, power_hold_ set) —
       a lifecycle-imposed hold, NOT user intent. */
    h.life->PostEvent(PowerLifecycle::Event::DisplayOff);
    h.Step();
    CHECK(h.life->PowerHold());
    CHECK(h.ctl->Status().control == PlaybackControl::Paused);
    CHECK(h.ctl->IntendedControl() == PlaybackControl::Paused);
    CHECK(!h.ctl->HoldOverridden());     /* power-imposed hold */
    const size_t ops_after_freeze = h.backend.ops.size();

    /* Inside the off window the user re-asserts Paused with a
       param change (a real recorded request, not an idempotent
       no-op): validated + parked, zero writes — and it marks
       the hold overridden by USER intent. */
    PlaybackParams held;
    held.speed = 40;                     /* upload used 100    */
    PlaybackController::Result r = h.ctl->Request(
        PlaybackMode::AnimationPlayback,
        PlaybackControl::Paused, held);
    CHECK(r.accepted && r.pending);
    CHECK(h.ctl->IntendedControl() == PlaybackControl::Paused);
    CHECK(h.ctl->HoldOverridden());      /* user overrode hold */
    CHECK(h.backend.ops.size() == ops_after_freeze);  /* parked */

    /* Wake: AttemptRestore must NOT un-hold to Playing — the
       flag is the sole deciding predicate here (intended is
       already Paused). Check intended after the LIFECYCLE poll
       alone, before the parked-replay tick can re-record it. */
    h.life->PostEvent(PowerLifecycle::Event::DisplayOn);
    h.now += std::chrono::milliseconds(600);
    h.life->Poll(h.now);
    CHECK(h.ctl->IntendedControl() == PlaybackControl::Paused);
    /* And no Playing-restore write ran: the revalidation saw
       the device already held (Direct == intended Paused), so
       the only post-freeze op is the revalidation refresh. */
    bool refresh_seen = false;
    for(size_t i = ops_after_freeze; i < h.backend.ops.size(); ++i)
    {
        CHECK(h.backend.ops[i] == "refresh");
        refresh_seen = true;
    }
    CHECK(refresh_seen);
    CHECK(h.backend.selects.size() == selects_at_upload);
    CHECK(!h.life->RestorePending());
    CHECK(!h.life->PowerHold());
    CHECK(!h.ctl->PowerGate());
    CHECK(!h.life->OutputPaused());

    /* The parked request replays on the first ungated tick —
       a no-op now (intended == applied hold), so still no
       upload restore write and the device stays paused. */
    h.Step();
    CHECK(!h.ctl->AckPending());
    CHECK(h.ctl->Status().control == PlaybackControl::Paused);
    CHECK(h.ctl->Status().upload == UploadState::Active);
    CHECK(h.backend.selects.size() == selects_at_upload);
    /* Nothing ever reached the display: the only wireless
       write in the whole scenario is the freeze's one
       sampled-hold frame via the owner bypass. */
    CHECK(FramesFor(h.transport, ws.key) == 0);
}

/*---------------------------------------------------------*\
|| T5: maintenance traffic is never the stop mechanism      ||
\*---------------------------------------------------------*/

static void TestMaintenanceNeverStopped()
{
    Harness h;
    const DeviceSnapshot ws = WirelessSnap();
    const DeviceSnapshot wd = WiredSnap();
    h.backend.snaps = { ws, wd };
    h.pipeline.RegisterDevice(ws);
    h.pipeline.RegisterDevice(wd);
    h.UploadActive();

    /* The API contract: the engine exposes no way to stop an
       animation by cutting device maintenance traffic. */
    CHECK(!PowerLifecycle::StopAnimationByMaintenanceStop());

    /* Under the display-off freeze the delivery lanes and the
       backend connection stay fully alive — nothing is torn
       down to "stop" the animation: */
    h.life->PostEvent(PowerLifecycle::Event::DisplayOff);
    h.Step();
    bool lane_alive = false;
    for(const DeviceKey& k : h.pipeline.LaneKeys())
    {
        if(k == ws.key)
        {
            lane_alive = true;
        }
    }
    CHECK(lane_alive);                        /* lane kept     */
    CHECK(h.pipeline.IsSuppressed(ws.key));   /* ownership held*/
    CHECK(h.backend.snaps[0].connection ==
          ConnectionState::Connected);        /* SDK untouched */
    /* The freeze itself was the one explicit write
       (custom+direct), proven in T2 — not starvation. */
    CHECK(h.backend.custom_calls == 1);
    CHECK(h.backend.direct.size() == 1);

    /* And controller maintenance stays parked-but-alive: the
       suspend gate stops Poll writes while the object and its
       state remain intact for the restore path. */
    h.life->PostEvent(PowerLifecycle::Event::Suspend);
    h.Step();
    CHECK(h.ctl->PowerGate());
    CHECK(h.ctl->Status().upload != UploadState::None);
}

/*---------------------------------------------------------*\
|| T6: gate ordering — discard is ordered before output runs||
\*---------------------------------------------------------*/

static void TestGateOrdering()
{
    Harness h;
    const DeviceSnapshot ws = WirelessSnap();
    h.backend.snaps = { ws };
    h.pipeline.RegisterDevice(ws);

    /* A frame pending at event time is discarded inside the
       same Poll that asserts the gate — the drain can never
       see it. */
    h.pipeline.SubmitLatest(FrameFor(ws, 1, h.Red()));
    h.life->PostEvent(PowerLifecycle::Event::DisplayOff);
    h.Step();
    CHECK(h.pipeline.PendingCount(ws.key) == 0);
    CHECK(h.pipeline.DrainDue(ws.key, h.now) == 0);
    CHECK(FramesFor(h.transport, ws.key) == 0);

    /* Suspend stacks on top: still gated, and a submission
       under the gate is an honest rejection — never queued. */
    h.life->PostEvent(PowerLifecycle::Event::Suspend);
    const SubmitResult r =
        h.pipeline.SubmitLatest(FrameFor(ws, 2, h.Red()));
    CHECK(r.status == SubmitStatus::Rejected);
    CHECK(h.pipeline.PendingCount(ws.key) == 0);
    h.Step();
    CHECK(h.pipeline.PendingCount(ws.key) == 0);
    CHECK(h.life->Suspended());
    CHECK(h.life->OutputPaused());
}

int main()
{
    TestDisplayOffGate();
    TestFreezeOnce();
    TestSuspendResume();
    TestResumeConsistent();
    TestReconnectResend();
    TestSuspendParksRequest();
    TestSuspendParkedNewUpload();
    TestParkedPauseKeepsHold();
    TestMaintenanceNeverStopped();
    TestGateOrdering();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
