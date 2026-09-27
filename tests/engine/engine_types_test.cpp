// SPDX-License-Identifier: GPL-2.0-or-later
//
// Contract tests for the plan-section-4 engine boundary types
// (src/engine/EngineTypes.h). Qt-free, hardware-independent.

#include "EngineTypes.h"

#include <cmath>
#include <cstdio>
#include <map>

using namespace openrgb2;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

static bool Near(double a, double b, double eps = 1e-6)
{
    return std::fabs(a - b) < eps;
}

static void TestRgbColor()
{
    /* Canonical RGB converts from packed 0x00BBGGRR scene colors. */
    const RgbColor c = FromSceneColor(studio::MakeSceneColor(10, 20, 30));
    CHECK(c.r == 10 && c.g == 20 && c.b == 30);
    CHECK((c == RgbColor{ 10, 20, 30 }));
    CHECK((c != RgbColor{ 10, 20, 31 }));
}

static void TestDeviceKey()
{
    /* Stable identity plus serial/location disambiguation and zone
       identity — never an enumeration index. */
    DeviceKey a; a.device = "lianli-slw-group"; a.serial = "s1";
    DeviceKey b = a;
    CHECK(a == b);
    CHECK(!(a != b));

    /* Identical device names disambiguate by serial/location. */
    b.serial = "s2";
    CHECK(a != b);
    b = a; b.location = "usb:1-4";
    CHECK(a != b);

    /* Zone identity: same controller, different zone -> different
       key; empty zone = controller-level key. */
    b = a; b.zone = "fan-2";
    CHECK(a != b);
    CHECK(a.zone.empty());

    /* Keys order deterministically for map/set use. */
    std::map<DeviceKey, int> m;
    DeviceKey c = a; c.device = "corsair-vengeance"; c.serial.clear();
    m[a] = 1; m[b] = 2; m[c] = 3;
    CHECK(m.size() == 3);
    CHECK(m.at(b) == 2);
}

static void TestDeviceSnapshot()
{
    /* The real wireless group: one controller, three 40-LED zones. */
    DeviceSnapshot s;
    s.key.device = "lianli-slw-group";
    s.key.serial = "s1";
    s.topology_generation = 7;
    s.zones = { { "fan-1", 40 }, { "fan-2", 40 }, { "fan-3", 40 } };
    s.modes = { "Static", "Direct", "Spectrum Cycle", "Rainbow Wave",
                "Breathing" };
    s.active_mode = 1;
    s.connection = ConnectionState::Connected;

    CHECK(s.LedCount() == 120);
    CHECK(s.topology_generation == 7);
    CHECK(s.modes.size() == 5);
    CHECK(s.connection == ConnectionState::Connected);

    DeviceSnapshot empty;
    CHECK(empty.LedCount() == 0);
    CHECK(empty.connection == ConnectionState::Disconnected);
}

static void TestRgbFrame()
{
    RgbFrame f;
    f.controller.device = "lianli-slw-group";
    f.topology_generation = 7;
    f.sequence = 42;
    f.sample_time = Clock::now();
    f.colors.assign(120, RgbColor{ 1, 2, 3 });

    CHECK(f.controller.zone.empty());      /* controller-level key */
    CHECK(f.topology_generation == 7);
    CHECK(f.sequence == 42);
    CHECK(f.colors.size() == 120);
    CHECK(f.sample_time <= Clock::now());
}

static void TestDeliveryStats()
{
    DeliveryStats d;
    CHECK(d.submitted == 0 && d.replaced_pending == 0);
    CHECK(d.transport_completed == 0);
    CHECK(d.provenance == MeasurementProvenance::Unknown);
    CHECK(d.IntervalCount() == 0 && Near(d.MeanIntervalS(), 0.0));

    const TimePoint t0 = Clock::now();
    d.RecordTransportCompletion(t0);   /* first completion: no interval */
    CHECK(d.transport_completed == 1);
    CHECK(d.IntervalCount() == 0);

    d.RecordTransportCompletion(t0 + std::chrono::milliseconds(20));
    d.RecordTransportCompletion(t0 + std::chrono::milliseconds(70));
    CHECK(d.transport_completed == 3);
    CHECK(d.IntervalCount() == 2);
    CHECK(Near(d.last_interval_s, 0.050, 1e-3));
    CHECK(Near(d.min_interval_s, 0.020, 1e-3));
    CHECK(Near(d.max_interval_s, 0.050, 1e-3));
    CHECK(Near(d.MeanIntervalS(), 0.035, 1e-3));

    d.provenance = MeasurementProvenance::TransportMeasured;
    CHECK(d.provenance == MeasurementProvenance::TransportMeasured);
}

static void TestPlaybackMode()
{
    /* Persisted per scene as stable string tokens. */
    PlaybackMode m;
    CHECK(std::string(PlaybackModeName(PlaybackMode::LiveEffects)) ==
          "live_effects");
    CHECK(std::string(PlaybackModeName(PlaybackMode::AnimationPlayback)) ==
          "animation_playback");
    CHECK(PlaybackModeFromName("live_effects", m) &&
          m == PlaybackMode::LiveEffects);
    CHECK(PlaybackModeFromName("animation_playback", m) &&
          m == PlaybackMode::AnimationPlayback);
    CHECK(!PlaybackModeFromName("palette_swap", m));
    CHECK(!PlaybackModeFromName("", m));

    /* Validated against preset capabilities. */
    PlaybackCapabilities caps;                    /* live only  */
    CHECK(PlaybackModeSupported(PlaybackMode::LiveEffects, caps));
    CHECK(!PlaybackModeSupported(PlaybackMode::AnimationPlayback, caps));
    caps.animation_playback = true;
    CHECK(PlaybackModeSupported(PlaybackMode::AnimationPlayback, caps));
    caps.live_effects = false;
    CHECK(!PlaybackModeSupported(PlaybackMode::LiveEffects, caps));
}

static void TestPlaybackStatus()
{
    PlaybackStatus s;
    CHECK(s.mode == PlaybackMode::LiveEffects);
    CHECK(s.preset_revision == 0 && s.parameter_revision == 0);
    CHECK(s.upload == UploadState::None);
    CHECK(s.effective_frame_count == 0);
    CHECK(Near(s.effective_frame_interval_s, 0.0));
    CHECK(Near(s.clock_sample_s, 0.0) && Near(s.clock_uncertainty_s, 0.0));
    CHECK(s.sync == SyncHealth::Unknown);

    /* Physical playback is recorded separately and defaults to
       unknown — never fused with configured/effective values. */
    CHECK(s.physical.provenance == MeasurementProvenance::Unknown);
    CHECK(Near(s.physical.frame_interval_s, 0.0));

    s.mode = PlaybackMode::AnimationPlayback;
    s.upload = UploadState::Active;
    s.effective_frame_count = 1440;
    s.effective_frame_interval_s = 1.0 / 60.0;
    s.physical.frame_interval_s = 1.0 / 58.0;
    s.physical.provenance = MeasurementProvenance::PhysicallyMeasured;
    CHECK(s.physical.provenance == MeasurementProvenance::PhysicallyMeasured);
    CHECK(!Near(s.physical.frame_interval_s,
                s.effective_frame_interval_s));
}

static void TestSubmitResult()
{
    /* accepted / replaced-pending / rejected-with-reason — there is
       deliberately no "displayed" result. */
    SubmitResult a = SubmitResult::Accept();
    CHECK(a.status == SubmitStatus::Accepted && a.reason.empty());

    SubmitResult r = SubmitResult::Replace();
    CHECK(r.status == SubmitStatus::ReplacedPending && r.reason.empty());

    SubmitResult j = SubmitResult::Reject("stale topology generation");
    CHECK(j.status == SubmitStatus::Rejected);
    CHECK(j.reason == "stale topology generation");
}

int main()
{
    TestRgbColor();
    TestDeviceKey();
    TestDeviceSnapshot();
    TestRgbFrame();
    TestDeliveryStats();
    TestPlaybackMode();
    TestPlaybackStatus();
    TestSubmitResult();
    std::printf("engine types: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
