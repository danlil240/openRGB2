// SPDX-License-Identifier: GPL-2.0-or-later
//
// CalibrationSession tests (src/engine/CalibrationSession.h) —
// studio-import Task 11. Deterministic: a fake topology, a fake
// transport sink, a synthetic clock, a REAL DeliveryPipeline and a
// REAL PlaybackController — no hardware, no sockets.
//
// Covers the Task 11 acceptance list:
//   - begin validation: unknown/disconnected controller, bad/empty
//     zone, output-paused, unwired deps, second session while busy
//   - ownership: lane suppressed + output hold asserted at Begin;
//     whole wireless group stays dark; one producer (the session
//     worker) writes the zone
//   - show: latest-only coalescing, -1 dark frame, bounds/wrong-
//     session/wrong-token refusals
//   - end: claims released, restore through RevalidateAndRestore,
//     idempotent, EndForSession/EndInternal/timeout/topology paths
//   - cancellation: End during Starting never lights a LED
//   - wired controllers run the same session without a group hold

#include "CalibrationSession.h"
#include "DeliveryPipeline.h"
#include "DeviceMapper.h"       /* IsWirelessGroupController */
#include "PlaybackController.h"

#include <cstdio>
#include <functional>
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
|| Fakes + harness                                          ||
\*---------------------------------------------------------*/

struct FakeTransport final : public IFrameSink
{
    SubmitResult SubmitLatest(const RgbFrame&) override
    {
        return SubmitResult::Accept();
    }
    DeliveryStats Stats() const override { return DeliveryStats{}; }
};

struct FakeBackend
{
    std::vector<DeviceSnapshot> snaps;
    BackendModeReport           report;      /* server-reported */
    int                         refresh_calls = 0;
    int                         custom_calls  = 0;
    bool                        custom_ok     = true;
    bool                        select_ok     = true;
    std::vector<std::string>    selects;
    std::vector<PlaybackModePatch> patches;
};

static std::shared_ptr<studio::SceneDocument>
MakeScene(const std::string& name, const std::string& preset)
{
    auto d = std::make_shared<studio::SceneDocument>();
    d->name          = name;
    d->effect.preset = preset;
    return d;
}

static DeviceSnapshot WirelessSnap(uint64_t gen = 7)
{
    DeviceSnapshot s;
    s.key = DeviceKey{ "Lian Li: SL Wireless", "AABBCCDDEEFF",
                       "Wireless: AA:BB:CC:DD:EE:FF", "" };
    s.name  = "SL Wireless Hub";
    s.topology_generation = gen;
    s.zones = { { "Fan 1", 4 }, { "Fan 2", 4 }, { "Fan 3", 4 } };
    /* The enumerated modes the upload path needs — "Direct"
       for live + set_custom_mode, the preset names for
       AnimationPlayback selection. */
    s.modes = { "Direct", "Static", "Spectrum Cycle",
                "Rainbow Wave", "Breathing" };
    s.connection = ConnectionState::Connected;
    /* Enumerated colors seed the session's base frame — a
       recognizable gradient so the zone-span math is visible. */
    s.colors.assign(s.LedCount(), RgbColor{});
    for(size_t i = 0; i < s.colors.size(); ++i)
    {
        s.colors[i] = RgbColor{ (uint8_t)i, (uint8_t)i, (uint8_t)i };
    }
    return s;
}

static DeviceSnapshot WiredSnap(uint64_t gen = 7)
{
    DeviceSnapshot s;
    s.key = DeviceKey{ "ASUSTek: Aura MB", "", "usb:1-4", "" };
    s.name  = "Aura MB";
    s.topology_generation = gen;
    s.zones = { { "Header", 6 } };
    s.modes = { "Direct", "Static" };
    s.connection = ConnectionState::Connected;
    return s;
}

struct Harness
{
    DeliveryPipeline  pipeline;
    FakeTransport     transport;
    FakeBackend       backend;
    TimePoint         now{};

    std::unique_ptr<PlaybackController>  playback;
    std::unique_ptr<CalibrationSession>  cal;

    std::shared_ptr<const studio::SceneDocument> scene;
    std::vector<RgbFrame> submits;
    int                   changes       = 0;
    bool                  output_paused = false;
    bool                  submit_ok     = true;
    std::string           last_log;

    Harness()
    {
        pipeline.SetTransport(
            std::shared_ptr<IFrameSink>(&transport,
                                        [](IFrameSink*){}));

        /* The real playback controller — the session's hold/
           revalidate calls must land on the same object the
           group's upload path uses. */
        PlaybackController::Deps pd;
        pd.scene        = [this] { return scene; };
        pd.snapshots    = [this] { return backend.snaps; };
        pd.wired_targets = [this]
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
        pd.select_mode =
            [this](const DeviceKey&, const std::string& name,
                   const PlaybackModePatch& p, std::string&)
            {
                if(!backend.select_ok)
                {
                    return false;
                }
                backend.selects.push_back(name);
                backend.patches.push_back(p);
                return true;
            };
        pd.active_mode =
            [this](const DeviceKey&, BackendModeReport& rep)
            {
                rep = backend.report;
                return true;
            };
        pd.refresh_status =
            [this](std::string&)
            {
                ++backend.refresh_calls;
                return true;
            };
        pd.set_custom_mode =
            [this](const DeviceKey&, std::string& err)
            {
                if(!backend.custom_ok)
                {
                    err = "custom mode failed";
                    return false;
                }
                ++backend.custom_calls;
                return true;
            };
        pd.direct_submit =
            [this](const RgbFrame& f)
            {
                submits.push_back(f);
                return SubmitResult::Accept();
            };
        pd.wired_submit =
            [this](const RgbFrame& f)
            { return pipeline.SubmitLatest(f); };
        pd.set_suppressed =
            [this](const DeviceKey& k, bool s, const std::string& r)
            { pipeline.SetSuppressed(k, s, r); };
        pd.drop_pending =
            [this](const DeviceKey& k)
            { return pipeline.DropPending(k); };
        pd.reset_delivered =
            [this](const DeviceKey& k)
            { pipeline.ResetDeliveredState(k); };
        pd.wait_idle =
            [this](const DeviceKey& k, Clock::duration t)
            { return pipeline.WaitIdle(k, t); };
        pd.clock_sample =
            [](PlaybackClockSnapshot&) { return false; };
        pd.now = [this] { return now; };
        playback = std::make_unique<PlaybackController>(pd);

        CalibrationSession::Deps cd;
        cd.snapshots  = [this] { return backend.snaps; };
        cd.playback   = playback.get();
        cd.set_suppressed =
            [this](const DeviceKey& k, bool s, const std::string& r)
            { pipeline.SetSuppressed(k, s, r); };
        cd.is_suppressed =
            [this](const DeviceKey& k)
            { return pipeline.IsSuppressed(k); };
        cd.drop_pending =
            [this](const DeviceKey& k)
            { return pipeline.DropPending(k); };
        cd.reset_delivered =
            [this](const DeviceKey& k)
            { pipeline.ResetDeliveredState(k); };
        cd.wait_idle =
            [this](const DeviceKey& k, Clock::duration t)
            { return pipeline.WaitIdle(k, t); };
        cd.direct_submit =
            [this](const RgbFrame& f)
            {
                submits.push_back(f);
                return submit_ok
                    ? SubmitResult::Accept()
                    : SubmitResult::Reject("injected rejection");
            };
        cd.set_custom_mode =
            [this](const DeviceKey&, std::string& err)
            {
                if(!backend.custom_ok)
                {
                    err = "custom mode failed";
                    return false;
                }
                ++backend.custom_calls;
                return true;
            };
        cd.output_paused = [this] { return output_paused; };
        cd.log =
            [this](const std::string& s) { last_log = s; };
        cd.changed = [this] { ++changes; };
        cd.now     = [this] { return now; };
        CalibrationSession::Options opts;
        opts.session_timeout = std::chrono::minutes(10);
        cal = std::make_unique<CalibrationSession>(cd, opts);
    }

    /* The session's worker is a real thread — wait on the
       state, never on sleep guesses. */
    bool WaitFor(const std::function<bool()>& pred,
                 int budget_ms = 4000)
    {
        while(budget_ms > 0)
        {
            if(pred())
            {
                return true;
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(2));
            budget_ms -= 2;
        }
        return pred();
    }
    bool WaitActive()
    {
        return WaitFor([this] { return cal->GetStatus().active; });
    }
    bool WaitIdle()
    {
        return WaitFor([this] { return !cal->Busy(); });
    }

    /* Playback ack plumbing — mirror playback_mode_test: the
       backend reports whatever the controller last selected
       (the applied values, not the enumerated ones). */
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
            if(p.has_direction) backend.report.direction =
                                    p.direction;
            if(p.has_colors)    backend.report.colors =
                                    p.colors;
        }
    }
    void PlaybackPoll()
    {
        now += std::chrono::milliseconds(600);
        playback->Poll(now);
    }

    /* Count frames with exactly one lit LED inside `span`. */
    static bool OneLit(const RgbFrame& f, uint32_t start,
                       uint32_t count, int which)
    {
        for(uint32_t i = start;
            i < start + count && i < f.colors.size(); ++i)
        {
            const RgbColor& c = f.colors[i];
            const bool lit  = (c.r || c.g || c.b);
            if(lit != ((int)(i - start) == which))
            {
                return false;
            }
        }
        return true;
    }
    static bool ZoneDark(const RgbFrame& f, uint32_t start,
                         uint32_t count)
    {
        for(uint32_t i = start;
            i < start + count && i < f.colors.size(); ++i)
        {
            const RgbColor& c = f.colors[i];
            if(c.r || c.g || c.b)
            {
                return false;
            }
        }
        return true;
    }
};

/*---------------------------------------------------------*\
|| Begin validation                                         ||
\*---------------------------------------------------------*/

static void TestBeginValidation()
{
    /* Unknown controller. */
    {
        Harness h;
        h.backend.snaps = { WirelessSnap() };
        const DeviceKey missing{ "nope", "", "loc", "" };
        auto r = h.cal->Begin(1, missing, "Fan 1");
        CHECK(!r.accepted);
        CHECK(r.error.find("topology") != std::string::npos);
        CHECK(!h.cal->Busy());
    }
    /* Disconnected controller. */
    {
        Harness h;
        DeviceSnapshot s = WirelessSnap();
        s.connection = ConnectionState::Disconnected;
        h.backend.snaps = { s };
        auto r = h.cal->Begin(1, s.key, "Fan 1");
        CHECK(!r.accepted);
        CHECK(r.error.find("connected") != std::string::npos);
    }
    /* Empty + missing zone names. */
    {
        Harness h;
        h.backend.snaps = { WirelessSnap() };
        auto r1 = h.cal->Begin(1, h.backend.snaps[0].key, "");
        CHECK(!r1.accepted);
        auto r2 = h.cal->Begin(1, h.backend.snaps[0].key,
                               "NoSuchZone");
        CHECK(!r2.accepted);
        CHECK(r2.error.find("no zone") != std::string::npos ||
              r2.error.find("zone") != std::string::npos);
        /* Empty zone — zero LEDs. */
        DeviceSnapshot s = WirelessSnap();
        s.zones[2].led_count = 0;
        h.backend.snaps = { s };
        auto r3 = h.cal->Begin(1, s.key, "Fan 3");
        CHECK(!r3.accepted);
    }
    /* Output paused refuses honestly. */
    {
        Harness h;
        h.backend.snaps = { WirelessSnap() };
        h.output_paused = true;
        auto r = h.cal->Begin(1, h.backend.snaps[0].key, "Fan 1");
        CHECK(!r.accepted);
        CHECK(r.error.find("paused") != std::string::npos);
    }
    /* Unwired deps refuse (no snapshots/direct_submit/
       set_suppressed). */
    {
        CalibrationSession bare(
            CalibrationSession::Deps{});
        Harness h;      /* for the snap builder only */
        DeviceSnapshot s = WirelessSnap();
        auto r = bare.Begin(1, s.key, "Fan 1");
        CHECK(!r.accepted);
        CHECK(r.error.find("not wired") != std::string::npos);
    }
}

/*---------------------------------------------------------*\
|| Happy path: wireless group lease + show + end            ||
\*---------------------------------------------------------*/

static void TestWirelessSession()
{
    Harness h;
    const DeviceSnapshot ws = WirelessSnap();
    h.backend.snaps = { ws };
    h.pipeline.RegisterDevice(ws);

    auto r = h.cal->Begin(11, ws.key, "Fan 1");
    CHECK(r.accepted);
    CHECK(r.pending);
    CHECK(!r.token.empty());
    /* Claims are taken synchronously at Begin — no live frame
       can slip between the reply and the worker. */
    CHECK(h.pipeline.IsSuppressed(ws.key));
    CHECK(h.playback->OutputHold());
    CHECK(h.cal->OwnsGroup(ws.key));
    CHECK(h.cal->Busy());

    CHECK(h.WaitActive());
    const auto st = h.cal->GetStatus();
    CHECK(st.active);
    CHECK(st.client_session == 11);
    CHECK(st.token == r.token);
    CHECK(st.led_count == 4);
    CHECK(st.zone == "Fan 1");
    CHECK(st.owns_group);
    /* The worker's first write blacks ONLY the bound zone;
       other zones keep their enumerated colors. */
    CHECK(h.submits.size() >= 1);
    CHECK(Harness::ZoneDark(h.submits.front(), 0, 4));
    CHECK(h.submits.front().colors.size() == ws.LedCount());
    CHECK(h.submits.front().colors[4].r == 4);   /* zone 2 keeps its color */

    /* Show: exactly one lit LED inside the zone span. */
    std::string err;
    CHECK(h.cal->Show(11, r.token, 2, err));
    CHECK(h.WaitFor([&h] {
        return !h.submits.empty() &&
               Harness::OneLit(h.submits.back(), 0, 4, 2);
    }));
    /* -1 = zone dark. */
    CHECK(h.cal->Show(11, r.token, -1, err));
    CHECK(h.WaitFor([&h] {
        return Harness::ZoneDark(h.submits.back(), 0, 4) &&
               h.cal->GetStatus().last_shown == -1;
    }));

    /* Wrong session/wrong token/out-of-range are refused —
       and write NOTHING. */
    const size_t writes = h.submits.size();
    CHECK(!h.cal->Show(22, r.token, 0, err));
    CHECK(!h.cal->Show(11, "bogus", 0, err));
    CHECK(!h.cal->Show(11, r.token, 4, err));
    CHECK(!h.cal->Show(11, r.token, -2, err));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(h.submits.size() == writes);

    /* Second owner refused while busy. */
    auto r2 = h.cal->Begin(22, ws.key, "Fan 2");
    CHECK(!r2.accepted);
    CHECK(r2.error.find("already") != std::string::npos);

    /* End: claims released, honest idle status. Restore
       through RevalidateAndRestore is a no-op here — the
       playback controller never owned the group (NotOwned);
       the owned-upload restore path is covered by
       TestRestoreThroughPlayback. */
    CHECK(h.cal->End(11, r.token, err));
    CHECK(h.WaitIdle());
    CHECK(!h.pipeline.IsSuppressed(ws.key));
    CHECK(!h.playback->OutputHold());
    CHECK(!h.cal->OwnsGroup(ws.key));
    const auto end_st = h.cal->GetStatus();
    CHECK(!end_st.active);
    CHECK(end_st.client_session == 0);
    CHECK(end_st.token.empty());
    CHECK(end_st.frames_written >= 2);
    CHECK(h.changes >= 3);       /* starting + active + ended */

    /* End is idempotent — ending an idle session is true. */
    CHECK(h.cal->End(11, r.token, err));
}

/*---------------------------------------------------------*\
|| Latest-only coalescing                                   ||
\*---------------------------------------------------------*/

static void TestShowCoalescing()
{
    Harness h;
    const DeviceSnapshot ws = WirelessSnap();
    h.backend.snaps = { ws };
    h.pipeline.RegisterDevice(ws);
    auto r = h.cal->Begin(1, ws.key, "Fan 2");
    CHECK(r.accepted);
    CHECK(h.WaitActive());
    /* Zone 2 spans leds 4..7. Rapid steps collapse — the
       hardware never replays intermediate positions. */
    std::string err;
    CHECK(h.cal->Show(1, r.token, 0, err));
    CHECK(h.cal->Show(1, r.token, 1, err));
    CHECK(h.cal->Show(1, r.token, 3, err));
    CHECK(h.WaitFor([&h] {
        return h.cal->GetStatus().last_shown == 3 &&
               !h.submits.empty() &&
               Harness::OneLit(h.submits.back(), 4, 4, 3);
    }));
    CHECK(h.cal->End(1, r.token, err));
    CHECK(h.WaitIdle());
}

/*---------------------------------------------------------*\
|| Cancellation during Starting                             ||
\*---------------------------------------------------------*/

static void TestCancelDuringStart()
{
    Harness h;
    const DeviceSnapshot ws = WirelessSnap();
    h.backend.snaps = { ws };
    h.pipeline.RegisterDevice(ws);
    auto r = h.cal->Begin(1, ws.key, "Fan 1");
    CHECK(r.accepted);
    /* End before the worker finishes — cancel propagates to
       the begin worker's checkpoints; no LED is ever lit
       (at most the dark frame may have written). */
    std::string err;
    CHECK(h.cal->End(1, r.token, err));
    CHECK(h.WaitIdle());
    for(const RgbFrame& f : h.submits)
    {
        CHECK(Harness::ZoneDark(f, 0, 4));
    }
    CHECK(!h.pipeline.IsSuppressed(ws.key));
    CHECK(!h.playback->OutputHold());
}

/*---------------------------------------------------------*\
|| Poll endings: topology, disconnect, pause, timeout       ||
\*---------------------------------------------------------*/

static void TestPollEndings()
{
    /* Generation change ends the session. */
    {
        Harness h;
        h.backend.snaps = { WirelessSnap(7) };
        auto r = h.cal->Begin(1, h.backend.snaps[0].key, "Fan 1");
        CHECK(r.accepted);
        CHECK(h.WaitActive());
        h.backend.snaps = { WirelessSnap(8) };   /* reorder/re-enumerate */
        h.now += std::chrono::milliseconds(100);
        h.cal->Poll(h.now);
        CHECK(h.WaitIdle());
        CHECK(!h.pipeline.IsSuppressed(h.backend.snaps[0].key));
    }
    /* Disconnect ends it. */
    {
        Harness h;
        h.backend.snaps = { WirelessSnap() };
        auto r = h.cal->Begin(1, h.backend.snaps[0].key, "Fan 1");
        CHECK(r.accepted);
        CHECK(h.WaitActive());
        h.backend.snaps[0].connection =
            ConnectionState::Disconnected;
        h.cal->Poll(h.now);
        CHECK(h.WaitIdle());
    }
    /* Zone shape change ends it. */
    {
        Harness h;
        h.backend.snaps = { WirelessSnap() };
        auto r = h.cal->Begin(1, h.backend.snaps[0].key, "Fan 1");
        CHECK(r.accepted);
        CHECK(h.WaitActive());
        h.backend.snaps[0].zones[0].led_count = 2;
        h.cal->Poll(h.now);
        CHECK(h.WaitIdle());
    }
    /* Output pause ends it — the lifecycle owns the restore. */
    {
        Harness h;
        h.backend.snaps = { WirelessSnap() };
        auto r = h.cal->Begin(1, h.backend.snaps[0].key, "Fan 1");
        CHECK(r.accepted);
        CHECK(h.WaitActive());
        h.output_paused = true;
        h.cal->Poll(h.now);
        CHECK(h.WaitIdle());
    }
    /* Inactivity timeout via the injected clock. */
    {
        Harness h;
        h.backend.snaps = { WirelessSnap() };
        auto r = h.cal->Begin(1, h.backend.snaps[0].key, "Fan 1");
        CHECK(r.accepted);
        CHECK(h.WaitActive());
        h.now += std::chrono::minutes(11);
        h.cal->Poll(h.now);
        CHECK(h.WaitIdle());
    }
    /* EndForSession: the dead client's session ends without a
       token; a DIFFERENT session's teardown leaves it alone. */
    {
        Harness h;
        h.backend.snaps = { WirelessSnap() };
        auto r = h.cal->Begin(1, h.backend.snaps[0].key, "Fan 1");
        CHECK(r.accepted);
        CHECK(h.WaitActive());
        h.cal->EndForSession(99);              /* not the owner */
        CHECK(h.cal->Busy());
        h.cal->EndForSession(1);               /* owner        */
        CHECK(h.WaitIdle());
    }
}

/*---------------------------------------------------------*\
|| Transport + mode-switch failures are honest              ||
\*---------------------------------------------------------*/

static void TestFailures()
{
    /* set_custom_mode refused -> session never activates,
       claims release, message is honest. */
    {
        Harness h;
        const DeviceSnapshot ws = WirelessSnap();
        h.backend.snaps = { ws };
        h.pipeline.RegisterDevice(ws);
        h.backend.custom_ok = false;
        auto r = h.cal->Begin(1, ws.key, "Fan 1");
        CHECK(r.accepted);
        CHECK(h.WaitIdle());
        CHECK(!h.pipeline.IsSuppressed(ws.key));
        CHECK(!h.playback->OutputHold());
        CHECK(h.cal->GetStatus().message.find("refused") !=
              std::string::npos ||
              h.cal->GetStatus().message.find("direct color") !=
              std::string::npos);
    }
    /* Transport rejects the calibration frame. */
    {
        Harness h;
        h.backend.snaps = { WirelessSnap() };
        h.submit_ok = false;
        auto r = h.cal->Begin(1, h.backend.snaps[0].key, "Fan 1");
        CHECK(r.accepted);
        CHECK(h.WaitIdle());
        CHECK(!h.cal->Busy());
    }
}

/*---------------------------------------------------------*\
|| Wired controller: session works, no group hold           ||
\*---------------------------------------------------------*/

static void TestWiredSession()
{
    Harness h;
    const DeviceSnapshot wd = WiredSnap();
    h.backend.snaps = { wd };
    h.pipeline.RegisterDevice(wd);
    auto r = h.cal->Begin(5, wd.key, "Header");
    CHECK(r.accepted);
    CHECK(h.pipeline.IsSuppressed(wd.key));
    CHECK(!h.playback->OutputHold());     /* no group to hold */
    CHECK(h.cal->OwnsGroup(wd.key));      /* still exclusive  */
    CHECK(h.WaitActive());
    const auto st = h.cal->GetStatus();
    CHECK(st.owns_group == false);
    std::string err;
    CHECK(h.cal->Show(5, r.token, 0, err));
    CHECK(h.WaitFor([&h] {
        return Harness::OneLit(h.submits.back(), 0, 6, 0);
    }));
    CHECK(h.cal->End(5, r.token, err));
    CHECK(h.WaitIdle());
    CHECK(!h.pipeline.IsSuppressed(wd.key));
}

/*---------------------------------------------------------*\
|| Restore through the CURRENT validated playback state      ||
\*---------------------------------------------------------*/

static void TestRestoreThroughPlayback()
{
    Harness h;
    const DeviceSnapshot ws = WirelessSnap();
    h.backend.snaps = { ws };
    h.pipeline.RegisterDevice(ws);
    h.scene = MakeScene("rainbow", "rainbow_wave");
    h.backend.report.valid = true;
    h.backend.report.name  = "Direct";

    /* Own the wireless group first — an acknowledged upload;
       calibration borrows the hardware and must hand it back
       through the CURRENT validated state, never a saved
       scene/frame. */
    PlaybackParams params;
    params.speed = 100;
    auto pr0 = h.playback->Request(PlaybackMode::AnimationPlayback,
                                   PlaybackControl::Playing,
                                   params);
    CHECK(pr0.accepted);
    if(!pr0.accepted)
    {
        std::printf("restore: request refused — %s\n",
                    pr0.reason.c_str());
    }
    CHECK(pr0.pending);
    CHECK(h.backend.selects.size() == 1);
    if(h.backend.selects.size() == 1)
    {
        CHECK(h.backend.selects[0] == "Rainbow Wave");
    }
    h.ReportPending();
    h.PlaybackPoll();
    CHECK(!h.playback->AckPending());
    CHECK(h.playback->Status().upload == UploadState::Active);
    CHECK(h.pipeline.IsSuppressed(ws.key));   /* playback owns */

    /* Calibration claims the already-suppressed lane — it
       does NOT release the playback suppression on end; the
       hold is what gates the group's own producers. */
    const int refresh0 = h.backend.refresh_calls;
    auto b = h.cal->Begin(7, ws.key, "Fan 2");
    CHECK(b.accepted);
    CHECK(h.playback->OutputHold());
    CHECK(h.WaitActive());
    CHECK(!h.submits.empty());

    /* A playback request issued during the hold validates +
       records + parks — it must NOT write now. (Distinct
       params so the request is a real change, not the
       idempotent no-op path.) */
    h.scene = MakeScene("breath", "breathing");
    const size_t selects0 = h.backend.selects.size();
    PlaybackParams params2 = params;
    params2.speed = 200;
    auto pr1 = h.playback->Request(PlaybackMode::AnimationPlayback,
                                   PlaybackControl::Playing,
                                   params2);
    CHECK(pr1.accepted);
    CHECK(pr1.pending);
    CHECK(h.backend.selects.size() == selects0);
    /* Poll stays dark under the hold: no mode refresh, no
       transport control, no replay. */
    const int refresh1 = h.backend.refresh_calls;
    h.PlaybackPoll();
    CHECK(h.backend.selects.size() == selects0);
    CHECK(h.backend.refresh_calls == refresh1);

    /* End: the hold releases BEFORE RevalidateAndRestore —
       refresh_status is the restore's first observable
       step. The parked request replays on the next ungated
       poll (the deferred-replay path, never a saved frame). */
    std::string err;
    CHECK(h.cal->End(7, b.token, err));
    CHECK(h.WaitIdle());
    CHECK(!h.playback->OutputHold());
    CHECK(h.backend.refresh_calls > refresh1);
    h.PlaybackPoll();
    CHECK(h.backend.selects.size() > selects0);
    if(h.backend.selects.size() > selects0)
    {
        CHECK(h.backend.selects.back() == "Breathing");
    }

    /* The session is honestly done; the lane belongs to
       playback again (it kept its own suppression through
       the whole session). */
    CHECK(!h.cal->Busy());
    CHECK(h.cal->GetStatus().frames_written >= 1);
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestBeginValidation();
    TestWirelessSession();
    TestShowCoalescing();
    TestCancelDuringStart();
    TestPollEndings();
    TestFailures();
    TestWiredSession();
    TestRestoreThroughPlayback();

    std::printf("calibration_session_test: %d checks, %d failures\n",
                checks, failures);
    return failures ? 1 : 0;
}
