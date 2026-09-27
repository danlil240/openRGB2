// SPDX-License-Identifier: GPL-2.0-or-later
//
// Playback-mode integration tests (src/engine/PlaybackController.h +
// PlaybackClockSync + the DeliveryPipeline suppression contract).
// Deterministic: a fake backend (select/active-mode reports), a fake
// transport sink, a synthetic clock, and a real DeliveryPipeline —
// no hardware, no sockets.
//
// Covers the Task 3.6 acceptance list:
//   - live -> playback -> live never restores obsolete frames;
//     queued live frames during a transition are rejected
//   - unsupported preset / reactive / layered scenes rejected
//     with a short reason; selection persists per scene
//   - mode/parameter revisions stay pending until the backend
//     reports the applied revision
//   - live suppression applies to the owned wireless group only
//   - clock wrap/reset/stale -> sync unavailable; bounded drift
//   - pause = sampled hold, stop = black

#include "PlaybackController.h"
#include "DeliveryPipeline.h"
#include "DeviceMapper.h"      /* IsWirelessGroupController */
#include "PlaybackClockSync.h"
#include "PlaybackPresets.h"

#include <cstdio>
#include <memory>
#include <string>
#include <thread>
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
    BackendModeReport           report;     /* server-reported   */
    int                         refresh_calls = 0;
    int                         custom_calls  = 0;
    std::vector<std::string>    selects;
    std::vector<PlaybackModePatch> patches;
    std::vector<RgbFrame>       direct;
    bool                        select_ok = true;
    bool                        custom_ok = true;
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
    s.topology_generation = gen;
    s.zones = { { "Header", 16 } };
    s.modes = { "Direct", "Static" };
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
MakeScene(const std::string& name, const std::string& preset,
          bool inline_layer = false)
{
    auto d = std::make_shared<studio::SceneDocument>();
    d->name = name;
    d->effect.preset = preset;
    if(inline_layer)
    {
        d->effect.layers.push_back(studio::EffectLayer{});
    }
    return d;
}

struct Harness
{
    DeliveryPipeline  pipeline;
    FakeTransport     transport;
    FakeBackend       backend;
    TimePoint         now{};

    std::shared_ptr<const studio::SceneDocument> scene;
    bool                clock_have = false;
    PlaybackClockSnapshot clock_snap;

    std::unique_ptr<PlaybackController> ctl;

    Harness()
    {
        pipeline.SetTransport(
            std::shared_ptr<IFrameSink>(&transport,
                                        [](IFrameSink*){}));
        PlaybackController::Deps d;
        d.scene        = [this] { return scene; };
        d.snapshots    = [this] { return backend.snaps; };
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
                if(!backend.select_ok)
                {
                    err = "select failed";
                    return false;
                }
                backend.selects.push_back(name);
                backend.patches.push_back(patch);
                return true;
            };
        d.active_mode =
            [this](const DeviceKey&, BackendModeReport& rep)
            {
                rep = backend.report;
                return true;
            };
        d.refresh_status =
            [this](std::string&)
            {
                ++backend.refresh_calls;
                return true;
            };
        d.set_custom_mode =
            [this](const DeviceKey&, std::string& err)
            {
                if(!backend.custom_ok)
                {
                    err = "custom mode failed";
                    return false;
                }
                ++backend.custom_calls;
                backend.report.valid     = true;
                backend.report.name      = "Direct";
                backend.report.speed     = 0;
                backend.report.direction = 0;
                backend.report.colors.clear();
                return true;
            };
        d.direct_submit =
            [this](const RgbFrame& f)
            {
                backend.direct.push_back(f);
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
        d.clock_sample =
            [this](PlaybackClockSnapshot& s)
            {
                if(!clock_have)
                {
                    return false;
                }
                s = clock_snap;
                return true;
            };
        d.now = [this] { return now; };
        ctl = std::make_unique<PlaybackController>(d);
    }

    void ReportPreset(const char* name, uint32_t speed = 100,
                      uint32_t direction = 0,
                      uint32_t color = 0)
    {
        backend.report.valid     = true;
        backend.report.name      = name;
        backend.report.speed     = speed;
        backend.report.direction = direction;
        backend.report.colors    = { color };
    }

    /* Acknowledge whatever the controller last selected —
       echoing the last mode-parameter patch (the backend
       reports the applied values, not the enumerated ones). */
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

    RgbColor Red()   const { return RgbColor{ 0xFF, 0x00, 0x00 }; }

    /* One maintenance cycle: advance past the 500 ms
       ack-poll interval so Poll actually attempts the
       refresh, then run it. */
    void Poll()
    {
        now += std::chrono::milliseconds(600);
        ctl->Poll(now);
    }
};

static bool AllBlack(const std::vector<RgbColor>& colors)
{
    for(const RgbColor& c : colors)
    {
        if(c.r || c.g || c.b)
        {
            return false;
        }
    }
    return true;
}

/*---------------------------------------------------------*\
|| T1: live -> playback -> live lifecycle                   ||
\*---------------------------------------------------------*/

static void TestLifecycle()
{
    Harness h;
    const DeviceSnapshot ws = WirelessSnap();
    const DeviceSnapshot wd = WiredSnap();
    h.backend.snaps = { ws, wd };
    h.pipeline.RegisterDevice(ws);
    h.pipeline.RegisterDevice(wd);
    h.scene = MakeScene("rainbow", "rainbow_wave");
    h.backend.report.valid = true;
    h.backend.report.name  = "Direct";

    /* A live frame queued just before the request must be
       dropped, never delivered after the upload. */
    h.pipeline.SubmitLatest(FrameFor(ws, 1, h.Red()));
    CHECK(h.pipeline.PendingCount(ws.key) == 1);

    PlaybackParams params;
    params.speed = 100;
    PlaybackController::Result r =
        h.ctl->Request(PlaybackMode::AnimationPlayback,
                       PlaybackControl::Playing, params);
    CHECK(r.accepted);
    CHECK(r.pending);
    CHECK(r.revision == 1);
    CHECK(h.backend.selects.size() == 1);
    CHECK(h.backend.selects[0] == "Rainbow Wave");

    /* The queued pre-transition frame was invalidated. */
    CHECK(h.pipeline.PendingCount(ws.key) == 0);

    /* Suppressed lane: live submissions reject with reason —
       never silently dropped. */
    CHECK(h.pipeline.IsSuppressed(ws.key));
    const SubmitResult rej =
        h.pipeline.SubmitLatest(FrameFor(ws, 2, h.Red()));
    CHECK(rej.status == SubmitStatus::Rejected);
    CHECK(!rej.reason.empty());

    /* Suppression is wireless-group only — the wired lane
       still accepts. Seq 100 mirrors a lane that already has
       live history (production max_seq is arbitrarily large —
       baked frames must not collide with it). */
    const SubmitResult wr =
        h.pipeline.SubmitLatest(FrameFor(wd, 100, h.Red()));
    CHECK(wr.status == SubmitStatus::Accepted);
    CHECK(h.pipeline.DrainDue(wd.key, h.now) == 1);

    /* Pending until the backend reports the applied mode. */
    CHECK(h.ctl->AckPending());
    CHECK(h.ctl->Status().mode == PlaybackMode::LiveEffects);
    h.Poll();
    CHECK(h.ctl->AckPending());          /* report is "Direct" */
    CHECK(h.ctl->Status().mode == PlaybackMode::LiveEffects);

    /* Backend reports the preset applied -> Active. */
    h.ReportPending();
    h.Poll();
    CHECK(!h.ctl->AckPending());
    const PlaybackStatus st1 = h.ctl->Status();
    CHECK(st1.mode == PlaybackMode::AnimationPlayback);
    CHECK(st1.upload == UploadState::Active);
    CHECK(st1.preset_revision == 1);
    CHECK(st1.parameter_revision == 1);
    CHECK(st1.effective_frame_count > 0);
    CHECK(st1.effective_frame_interval_s > 0.0);
    CHECK(st1.control == PlaybackControl::Playing);
    CHECK(h.pipeline.IsSuppressed(ws.key));  /* still owned */
    CHECK(h.ctl->PlaybackOwnsWired());

    /* Nothing reached the transport for the wireless group:
       the stale queued frame and the rejected submissions
       never overwrote the uploaded loop. */
    for(const RgbFrame& f : h.transport.frames)
    {
        CHECK(!(f.controller == ws.key));
    }

    /* Baked wired production: Tick feeds the wired lane.
       The lane's live high-water is 100 — baked frames are
       synthetic, so they deliver regardless of sequence. */
    const size_t before = h.transport.frames.size();
    h.ctl->Tick(h.now);
    CHECK(h.pipeline.DrainDue(wd.key, h.now) == 1);
    CHECK(h.transport.frames.size() > before);
    const RgbFrame& baked = h.transport.frames.back();
    CHECK(baked.controller == wd.key);
    CHECK(baked.colors.size() == wd.LedCount());

    /* The guard still does its real job: a stale live seq is
       rejected, a fresh one is admitted — synthetic frames
       never poisoned the live high-water. */
    CHECK(h.pipeline.SubmitLatest(FrameFor(wd, 50, h.Red())).status
          == SubmitStatus::Rejected);
    CHECK(h.pipeline.SubmitLatest(FrameFor(wd, 101, h.Red())).status
          == SubmitStatus::Accepted);
    CHECK(h.pipeline.DrainDue(wd.key, h.now) == 1);

    /* Back to live: pending until "Direct" is reported. */
    r = h.ctl->Request(PlaybackMode::LiveEffects,
                     PlaybackControl::Playing, params);
    CHECK(r.accepted);
    CHECK(r.pending);
    CHECK(h.backend.custom_calls == 1);
    CHECK(h.pipeline.IsSuppressed(ws.key));   /* held for ack */
    h.Poll();          /* report already "Direct" */
    CHECK(!h.ctl->AckPending());
    const PlaybackStatus st2 = h.ctl->Status();
    CHECK(st2.mode == PlaybackMode::LiveEffects);
    CHECK(st2.upload == UploadState::None);
    CHECK(!h.pipeline.IsSuppressed(ws.key));
    CHECK(!h.ctl->PlaybackOwnsWired());

    /* Fresh complete-group frame delivers — delivered-state
       memory was reset so it cannot be suppressed by the
       pre-upload delivered state. */
    const SubmitResult fresh =
        h.pipeline.SubmitLatest(FrameFor(ws, 3, h.Red()));
    CHECK(fresh.status == SubmitStatus::Accepted);
    CHECK(h.pipeline.DrainDue(ws.key, h.now) == 1);
    CHECK(h.transport.frames.back().controller == ws.key);
}

/*---------------------------------------------------------*\
|| T2: capability rejections carry a short reason           ||
\*---------------------------------------------------------*/

static void TestRejections()
{
    Harness h;
    const DeviceSnapshot ws = WirelessSnap();
    h.backend.snaps = { ws };
    h.pipeline.RegisterDevice(ws);
    PlaybackParams params;

    /* Non-playback preset scene. */
    h.scene = MakeScene("other", "not_a_real_preset");
    PlaybackController::Result r =
        h.ctl->Request(PlaybackMode::AnimationPlayback,
                       PlaybackControl::Playing, params);
    CHECK(!r.accepted);
    CHECK(!r.reason.empty());
    CHECK(r.reason.find("playback") != std::string::npos ||
          r.reason.find("preset") != std::string::npos);

    /* Arbitrary layered scene: live-only. */
    h.scene = MakeScene("layers", "", /*inline_layer*/ true);
    r = h.ctl->Request(PlaybackMode::AnimationPlayback,
                       PlaybackControl::Playing, params);
    CHECK(!r.accepted);
    CHECK(r.reason.find("live") != std::string::npos);

    /* Reactive preset (key input): rejected with the
       reactive reason — animation stays live-only. */
    h.scene = MakeScene("keys", "keyripple");
    r = h.ctl->Request(PlaybackMode::AnimationPlayback,
                       PlaybackControl::Playing, params);
    CHECK(!r.accepted);
    CHECK(!r.reason.empty());

    /* No scene at all. */
    h.scene.reset();
    r = h.ctl->Request(PlaybackMode::AnimationPlayback,
                       PlaybackControl::Playing, params);
    CHECK(!r.accepted);
    CHECK(!r.reason.empty());

    /* No wireless group connected. */
    h.scene = MakeScene("rainbow", "rainbow_wave");
    h.backend.snaps = { WiredSnap() };
    r = h.ctl->Request(PlaybackMode::AnimationPlayback,
                       PlaybackControl::Playing, params);
    CHECK(!r.accepted);
    CHECK(r.reason.find("wireless") != std::string::npos);

    /* Backend not offering the mode. */
    h.backend.snaps = { ws };
    DeviceSnapshot cut = ws;
    cut.modes = { "Direct" };
    h.backend.snaps = { cut };
    r = h.ctl->Request(PlaybackMode::AnimationPlayback,
                       PlaybackControl::Playing, params);
    CHECK(!r.accepted);
    CHECK(!r.reason.empty());
    CHECK(!h.pipeline.IsSuppressed(ws.key));
}

/*---------------------------------------------------------*\
|| T3: per-scene persistence                                ||
\*---------------------------------------------------------*/

static void TestPersistence()
{
    Harness h;
    const DeviceSnapshot ws = WirelessSnap();
    const DeviceSnapshot wd = WiredSnap();
    h.backend.snaps = { ws, wd };
    h.pipeline.RegisterDevice(ws);
    h.pipeline.RegisterDevice(wd);
    h.backend.report.name = "Direct";
    h.backend.report.valid = true;

    /* Scene A chooses animation with custom params. */
    h.scene = MakeScene("sceneA", "rainbow_wave");
    h.ctl->OnSceneApplied(h.scene);
    PlaybackParams params;
    params.speed = 150;
    params.reverse = true;
    h.ctl->Request(PlaybackMode::AnimationPlayback,
                   PlaybackControl::Playing, params);
    CHECK(h.backend.selects.size() == 1);
    CHECK(h.backend.patches[0].has_speed);
    CHECK(h.backend.patches[0].speed == 150);
    CHECK(h.backend.patches[0].has_direction);
    CHECK(h.backend.patches[0].direction == 1);
    h.ReportPending();
    h.Poll();
    CHECK(h.ctl->Status().mode == PlaybackMode::AnimationPlayback);

    /* Scene B never chose playback -> live default: the
       apply path restores live (Direct reported). */
    h.scene = MakeScene("sceneB", "aurora");
    h.ctl->OnSceneApplied(h.scene);
    h.Poll();
    CHECK(h.ctl->Status().mode == PlaybackMode::LiveEffects);

    /* Re-applying scene A restores its persisted animation
       selection — with the persisted parameters. */
    h.scene = MakeScene("sceneA", "rainbow_wave");
    h.ctl->OnSceneApplied(h.scene);
    CHECK(h.backend.selects.size() == 2);
    CHECK(h.backend.patches.back().speed == 150);
    CHECK(h.backend.patches.back().direction == 1);
    h.ReportPending();
    h.Poll();
    CHECK(h.ctl->Status().mode == PlaybackMode::AnimationPlayback);

    CHECK(h.ctl->PersistedScenes() >= 2);
}

/*---------------------------------------------------------*\
|| T4: parameter revision acknowledgement                   ||
\*---------------------------------------------------------*/

static void TestRevisions()
{
    Harness h;
    const DeviceSnapshot ws = WirelessSnap();
    h.backend.snaps = { ws };
    h.pipeline.RegisterDevice(ws);
    h.scene = MakeScene("breath", "breathing");
    h.backend.report.name = "Direct";
    h.backend.report.valid = true;

    PlaybackParams params;
    params.speed = 120;
    params.color = RgbColor{ 0x00, 0x80, 0xFF };
    PlaybackController::Result r =
        h.ctl->Request(PlaybackMode::AnimationPlayback,
                       PlaybackControl::Playing, params);
    CHECK(r.accepted && r.pending);
    CHECK(h.backend.selects[0] == "Breathing");
    CHECK(h.backend.patches[0].has_colors);
    /* 0x00BBGGRR packing. */
    CHECK(h.backend.patches[0].colors[0] == 0x00FF8000u);

    /* Stale report keeps it pending. */
    h.Poll();
    CHECK(h.ctl->Status().parameter_revision == 0);
    CHECK(h.ctl->Status().upload == UploadState::Uploading);

    h.ReportPreset("Breathing", 120, 0, 0x00FF8000u);
    h.Poll();
    CHECK(h.ctl->Status().parameter_revision == 1);
    CHECK(h.ctl->Status().upload == UploadState::Active);

    /* Parameter-only change: new revision, new upload. */
    params.speed = 60;
    r = h.ctl->Request(PlaybackMode::AnimationPlayback,
                       PlaybackControl::Playing, params);
    CHECK(r.accepted && r.pending);
    CHECK(r.revision == 2);
    /* Still active on the old upload until acked. */
    CHECK(h.ctl->Status().parameter_revision == 1);
    h.ReportPreset("Breathing", 60, 0, 0x00FF8000u);
    h.Poll();
    CHECK(h.ctl->Status().parameter_revision == 2);
    /* Same preset — the preset revision does not move. */
    CHECK(h.ctl->Status().preset_revision == 1);
}

/*---------------------------------------------------------*\
|| T5: pause = sampled hold, stop = black                   ||
\*---------------------------------------------------------*/

static void TestPauseStop()
{
    Harness h;
    const DeviceSnapshot ws = WirelessSnap();
    const DeviceSnapshot wd = WiredSnap();
    h.backend.snaps = { ws, wd };
    h.pipeline.RegisterDevice(ws);
    h.pipeline.RegisterDevice(wd);
    h.scene = MakeScene("rainbow", "rainbow_wave");
    h.backend.report.name = "Direct";
    h.backend.report.valid = true;

    PlaybackParams params;
    params.speed = 100;
    h.ctl->Request(PlaybackMode::AnimationPlayback,
                   PlaybackControl::Playing, params);
    h.ReportPending();
    h.Poll();      /* Active; active_since_ = now */
    CHECK(h.ctl->Status().upload == UploadState::Active);

    /* Pause: direct-mode select + ONE held frame equal to
       the sampled loop frame at the current phase. */
    PlaybackController::Result r =
        h.ctl->Request(PlaybackMode::AnimationPlayback,
                       PlaybackControl::Paused, params);
    CHECK(r.accepted);
    CHECK(!r.pending);                /* hold is synchronous */
    CHECK(h.backend.custom_calls == 1);
    CHECK(h.backend.direct.size() == 1);
    const RgbFrame& hold = h.backend.direct.back();
    CHECK(hold.controller == ws.key);
    CHECK(hold.colors.size() == ws.LedCount());
    {
        /* Phase 0 (no elapsed time since activation): the
           hold equals the first baked frame. */
        PlaybackFrameSampler sampler;
        const std::vector<RgbColor> want = sampler.Sample(
            LianLiWireless::PlaybackPreset::RainbowWave,
            params, ws.LedCount(),
            h.ctl->Status().effective_frame_count, 0.0);
        CHECK(hold.colors == want);
    }
    CHECK(h.ctl->Status().control == PlaybackControl::Paused);

    /* Paused: no new wired frames (the hold persists). */
    const size_t n_wired = h.transport.frames.size();
    h.ctl->Tick(h.now);
    h.pipeline.DrainDue(wd.key, h.now);
    CHECK(h.transport.frames.size() == n_wired);

    /* Give the wired lane live history: seq 250 delivered.
       The stop-black write is synthetic — it must deliver
       even with a large prior high-water (production lanes
       always have history). */
    h.pipeline.SubmitLatest(FrameFor(wd, 250, h.Red()));
    CHECK(h.pipeline.DrainDue(wd.key, h.now) == 1);

    /* Stop: the group goes black (hold frame replaced). */
    r = h.ctl->Request(PlaybackMode::AnimationPlayback,
                       PlaybackControl::Stopped, params);
    CHECK(r.accepted);
    CHECK(h.backend.direct.size() == 2);
    CHECK(AllBlack(h.backend.direct.back().colors));
    CHECK(h.ctl->Status().control == PlaybackControl::Stopped);
    /* Wired peers also converge on black. */
    h.pipeline.DrainDue(wd.key, h.now);
    CHECK(AllBlack(h.transport.frames.back().colors));

    /* Resume: the preset is re-uploaded and re-acked. */
    r = h.ctl->Request(PlaybackMode::AnimationPlayback,
                       PlaybackControl::Playing, params);
    CHECK(r.accepted && r.pending);
    CHECK(h.backend.selects.size() == 2);
    h.ReportPending();
    h.Poll();
    CHECK(h.ctl->Status().control == PlaybackControl::Playing);
    CHECK(h.ctl->Status().upload == UploadState::Active);
}

/*---------------------------------------------------------*\
|| T6: clock wrap/reset/stale -> unavailable; bounded drift ||
\*---------------------------------------------------------*/

static void TestClock()
{
    PlaybackClockSync::Config cfg;
    cfg.lock_samples = 2;
    PlaybackClockSync clock(cfg);
    const TimePoint t0 = Clock::now();
    const double cycle = 2.4;

    /* No data: sync unavailable. */
    CHECK(clock.Health(t0) == SyncHealth::Unknown);
    CHECK(!clock.Phase(t0, cycle).has_value());

    PlaybackClockSnapshot s;
    s.valid = true; s.ticks = 1600; s.sampled_at = t0;
    clock.Update(s);
    /* First sample: anchored but not locked. */
    CHECK(clock.Health(t0) == SyncHealth::Degraded);
    CHECK(!clock.Phase(t0, cycle).has_value());

    s.ticks = 3200; s.sampled_at = t0 + std::chrono::seconds(1);
    clock.Update(s);
    CHECK(clock.Health(s.sampled_at) == SyncHealth::Healthy);
    CHECK(clock.Phase(s.sampled_at, cycle).has_value());
    CHECK(clock.Samples() == 2);

    /* 32-bit wrap is transparent (forward delta). */
    PlaybackClockSync wrap(cfg);
    PlaybackClockSnapshot w;
    w.valid = true; w.ticks = 0xFFFFFF00; w.sampled_at = t0;
    wrap.Update(w);
    w.ticks = 0x120; w.sampled_at = t0 + std::chrono::seconds(1);
    wrap.Update(w);
    CHECK(wrap.Health(w.sampled_at) == SyncHealth::Healthy);
    CHECK(wrap.Resets() == 0);

    /* Backward jump beyond wrap = device reset: the lock is
       dropped until enough consistent samples re-establish. */
    PlaybackClockSync rst(cfg);
    PlaybackClockSnapshot q;
    q.valid = true; q.ticks = 5000; q.sampled_at = t0;
    rst.Update(q);
    q.ticks = 6600; q.sampled_at = t0 + std::chrono::seconds(1);
    rst.Update(q);
    CHECK(rst.Health(q.sampled_at) == SyncHealth::Healthy);
    q.ticks = 10;  q.sampled_at = t0 + std::chrono::seconds(2);
    rst.Update(q);
    CHECK(rst.Resets() == 1);
    CHECK(rst.Health(q.sampled_at) == SyncHealth::Degraded);
    CHECK(!rst.Phase(q.sampled_at, cycle).has_value());
    q.ticks = 1610; q.sampled_at = t0 + std::chrono::seconds(3);
    rst.Update(q);
    CHECK(rst.Health(q.sampled_at) == SyncHealth::Healthy);
    CHECK(rst.Phase(q.sampled_at, cycle).has_value());

    /* Stale: an old anchor stops producing a phase. */
    const TimePoint stale_now =
        q.sampled_at + std::chrono::seconds(30);
    CHECK(rst.Health(stale_now) == SyncHealth::Stale);
    CHECK(!rst.Phase(stale_now, cycle).has_value());

    /* Invalid samples are misses, not resets. */
    PlaybackClockSnapshot bad;
    bad.valid = false; bad.sampled_at = q.sampled_at;
    rst.Update(bad);
    CHECK(rst.Misses() == 1);
    CHECK(rst.Resets() == 1);

    /* Drift bound: a wildly deviating measured rate is
       clamped into the configured ±10% band. */
    PlaybackClockSync drift(cfg);
    PlaybackClockSnapshot d;
    d.valid = true; d.ticks = 0; d.sampled_at = t0;
    drift.Update(d);
    for(int i = 1; i <= 10; ++i)
    {
        d.ticks      = (uint32_t)(2200 * i);   /* +37.5% rate */
        d.sampled_at = t0 + std::chrono::seconds(i);
        drift.Update(d);
    }
    const double hi = cfg.nominal_tick_hz *
                      (1.0 + cfg.max_rate_deviation);
    CHECK(drift.EstimatedTickHz() <= hi + 1e-6);
    CHECK(drift.EstimatedTickHz() > cfg.nominal_tick_hz);

    /* Latency compensation advances the phase. */
    PlaybackClockSync::Config lc = cfg;
    lc.latency_compensation_s = 0.5;
    PlaybackClockSync lat(lc);
    PlaybackClockSnapshot l;
    l.valid = true; l.ticks = 1600; l.sampled_at = t0;
    lat.Update(l);
    l.ticks = 3200; l.sampled_at = t0 + std::chrono::seconds(1);
    lat.Update(l);
    const auto p1 = lat.Phase(l.sampled_at, cycle);
    CHECK(p1.has_value());
    const double expect =
        (2.0 + 0.5) / cycle - std::floor((2.0 + 0.5) / cycle);
    CHECK(std::abs(*p1 - expect) < 1e-6);
}

/*---------------------------------------------------------*\
|| T7: pipeline suppression mechanics                       ||
\*---------------------------------------------------------*/

static void TestPipelineSuppression()
{
    DeliveryPipeline pipe;
    FakeTransport    transport;
    pipe.SetTransport(std::shared_ptr<IFrameSink>(
        &transport, [](IFrameSink*){}));
    const DeviceSnapshot ws = WirelessSnap();
    const DeviceSnapshot wd = WiredSnap();
    pipe.RegisterDevice(ws);
    pipe.RegisterDevice(wd);
    const TimePoint now = Clock::now();

    /* A frame pending when suppression lands must not
       deliver while suppressed. */
    RgbFrame held = FrameFor(ws, 1, RgbColor{1,2,3});
    pipe.SubmitLatest(held);
    pipe.SetSuppressed(ws.key, true, "uploaded loop");
    CHECK(pipe.DrainDue(ws.key, now) == 0);
    CHECK(transport.frames.empty());

    /* Both submission paths reject with the reason. */
    SubmitResult r1 = pipe.SubmitLatest(FrameFor(ws, 2, RgbColor{}));
    SubmitResult r2 = pipe.SubmitReliable(FrameFor(ws, 3, RgbColor{}));
    CHECK(r1.status == SubmitStatus::Rejected);
    CHECK(r2.status == SubmitStatus::Rejected);
    CHECK(r1.reason == "uploaded loop");

    /* Other lanes are unaffected. */
    CHECK(pipe.SubmitLatest(FrameFor(wd, 1, RgbColor{7,7,7})).status
          == SubmitStatus::Accepted);
    CHECK(pipe.DrainDue(wd.key, now) == 1);

    /* Resume: reset delivered-state, unsuppress, and the
       pre-suppression pending frame is gone after drop. */
    pipe.DropPending(ws.key);
    pipe.ResetDeliveredState(ws.key);
    pipe.SetSuppressed(ws.key, false, "");
    CHECK(pipe.PendingCount(ws.key) == 0);
    RgbFrame fresh = FrameFor(ws, 4, RgbColor{9,9,9});
    CHECK(pipe.SubmitLatest(fresh).status == SubmitStatus::Accepted);
    CHECK(pipe.DrainDue(ws.key, now) == 1);
    CHECK(transport.frames.back().colors == fresh.colors);
    /* Identical re-submit suppresses normally again. */
    CHECK(pipe.DrainDue(ws.key, now) == 0);

    /* WaitIdle: a lane with no pending and no in-flight is
       idle immediately. */
    CHECK(pipe.WaitIdle(ws.key, std::chrono::milliseconds(10)));
}

/*---------------------------------------------------------*\
||| T8: failed upload -> explicit Live restores the lane    ||
\*---------------------------------------------------------*/

static void TestFailedRestore()
{
    Harness h;
    const DeviceSnapshot ws = WirelessSnap();
    h.backend.snaps = { ws };
    h.pipeline.RegisterDevice(ws);
    h.scene = MakeScene("breath", "breathing");
    /* Report stays "Direct": the animation ack never lands. */
    h.backend.report.name  = "Direct";
    h.backend.report.valid = true;

    PlaybackParams params;
    h.ctl->Request(PlaybackMode::AnimationPlayback,
                   PlaybackControl::Playing, params);
    CHECK(h.pipeline.IsSuppressed(ws.key));

    /* Run the ack clock out — the upload is declared Failed
       but the lane stays owned (the device may be looping). */
    h.now += std::chrono::seconds(9);
    h.Poll();
    CHECK(h.ctl->Status().upload == UploadState::Failed);
    CHECK(h.pipeline.IsSuppressed(ws.key));
    CHECK(h.ctl->Status().mode == PlaybackMode::LiveEffects);

    /* An explicit Live request must NOT hit the idempotent
       no-op — it restores the lane via the Direct path. */
    PlaybackController::Result r =
        h.ctl->Request(PlaybackMode::LiveEffects,
                       PlaybackControl::Playing, params);
    CHECK(r.accepted);
    CHECK(r.pending);                       /* Direct ack    */
    CHECK(h.backend.custom_calls == 1);
    h.Poll();
    CHECK(!h.pipeline.IsSuppressed(ws.key));
    CHECK(h.ctl->Status().mode == PlaybackMode::LiveEffects);
    CHECK(h.ctl->Status().upload == UploadState::None);
    CHECK(h.pipeline.SubmitLatest(FrameFor(ws, 5, h.Red())).status
          == SubmitStatus::Accepted);
}

/*---------------------------------------------------------*\
||| T9: bounded sampler cache + concurrent access           ||
\*---------------------------------------------------------*/

static void TestSamplerBound()
{
    PlaybackFrameSampler s;
    /* 20 distinct breathing colors -> cache stays bounded. */
    for(int i = 0; i < 20; ++i)
    {
        PlaybackParams p;
        p.color = RgbColor{ (uint8_t)i, 0, 0 };
        CHECK(!s.Sample(
            LianLiWireless::PlaybackPreset::Breathing,
            p, 16, 8, 0.25).empty());
    }
    CHECK(s.CachedLoops() <= 8);

    /* Concurrent find+emplace across threads must be safe —
       guard is inside the sampler, not the caller's mutex. */
    PlaybackFrameSampler shared;
    auto work = [&shared]
    {
        for(int i = 0; i < 12; ++i)
        {
            PlaybackParams p;
            p.color   = RgbColor{ (uint8_t)i, 1, 2 };
            p.reverse = (i % 2) == 0;
            shared.Sample(
                LianLiWireless::PlaybackPreset::Breathing,
                p, 24, 8, 0.5);
            shared.Sample(
                LianLiWireless::PlaybackPreset::RainbowWave,
                p, 24, 8, 0.5);
        }
    };
    std::thread t1(work), t2(work), t3(work);
    t1.join(); t2.join(); t3.join();
    CHECK(shared.CachedLoops() <= 8);
    CHECK(shared.CachedLoops() > 0);
}

/*---------------------------------------------------------*\
||| T10: persisted-scene store is bounded                   ||
\*---------------------------------------------------------*/

static void TestSceneStoreBound()
{
    Harness h;
    const DeviceSnapshot ws = WirelessSnap();
    h.backend.snaps = { ws };
    h.pipeline.RegisterDevice(ws);
    h.backend.report.name  = "Direct";
    h.backend.report.valid = true;

    /* Apply 40 distinct scenes — the per-scene store stays
       capped at the most-recent 32. */
    for(int i = 0; i < 40; ++i)
    {
        const std::string name = "scene" + std::to_string(i);
        h.scene = MakeScene(name, "layers" + name, true);
        h.ctl->OnSceneApplied(h.scene);
    }
    CHECK(h.ctl->PersistedScenes() <= 32);
}

int main()
{
    TestLifecycle();
    TestRejections();
    TestPersistence();
    TestRevisions();
    TestPauseStop();
    TestClock();
    TestPipelineSuppression();
    TestFailedRestore();
    TestSamplerBound();
    TestSceneStoreBound();

    std::printf("playback_test: %d checks, %d failures\n",
                checks, failures);
    return failures ? 1 : 0;
}
