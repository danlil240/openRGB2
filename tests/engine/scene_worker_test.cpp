// SPDX-License-Identifier: GPL-2.0-or-later
//
// Scene-evaluation worker tests (src/engine/SceneWorker.h):
// deterministic stepping, absolute-time sampling, latest-wins state
// slots, and the dedicated 60 Hz thread. Qt-free; drives
// studio::EffectEngine::Evaluate exactly as the core suites do.

#include "Engine.h"
#include "SceneWorker.h"

#include "effects/EffectEngine.h"
#include "scene/SceneTypes.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

using namespace openrgb2;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

static bool Near(double a, double b, double eps = 1e-9)
{
    return std::fabs(a - b) < eps;
}

static studio::SceneObject OneLedDevice(const std::string& id, float x)
{
    studio::SceneObject o;
    o.id       = id;
    o.kind     = studio::ObjectKind::Device;
    o.geometry = "test_body";
    o.transform.position = { x, 0.0f, 0.0f };
    o.verified = true;
    studio::Emitter e;
    e.local_pos = { 0, 0, 0 };
    e.group     = id;
    e.address   = 0;
    o.emitters.push_back(e);
    return o;
}

/* A scene whose single LED color depends on t: a wave moving at
   0.1 m/s over a device at x = 0.25 — Evaluate(t) and Evaluate(t')
   differ whenever t != t'. */
static std::shared_ptr<studio::SceneDocument> WaveDoc()
{
    auto doc = std::make_shared<studio::SceneDocument>();
    doc->objects.push_back(OneLedDevice("mid", 0.25f));
    return doc;
}

static std::vector<studio::EffectLayer> WaveLayers()
{
    studio::EffectLayer wave;
    wave.primitive = "wave";
    wave.direction = { 1.0f, 0.0f, 0.0f };
    wave.scale     = 1.0f;
    wave.speed     = 0.1f;
    wave.density   = 0.0f;
    wave.palette   = studio::MakePalette({
        { 0.0f, studio::ToColorF(studio::MakeSceneColor(0, 0, 0))     },
        { 0.5f, studio::ToColorF(studio::MakeSceneColor(255, 255, 255)) },
        { 1.0f, studio::ToColorF(studio::MakeSceneColor(0, 0, 0))     },
    });
    return { wave };
}

static std::vector<studio::EffectLayer> SolidLayers(unsigned r,
                                                  unsigned g,
                                                  unsigned b)
{
    studio::EffectLayer solid;
    solid.primitive = "static";
    solid.density   = 0.0f;
    solid.palette   = studio::MakePalette({ studio::MakeSceneColor(r, g, b) });
    return { solid };
}

/* Reference evaluation: what the core engine produces for the same
   inputs, computed without the worker. */
static studio::FrameColors Reference(const studio::SceneDocument& doc,
                                     const std::vector<studio::EffectLayer>& layers,
                                     double t)
{
    studio::EffectEngine engine;
    engine.SetLayers(layers);
    studio::FrameColors frame;
    engine.Evaluate(doc, t, frame);
    return frame;
}

static void TestDeterministicStep()
{
    SceneWorker worker;
    auto doc = WaveDoc();
    worker.SubmitScene(doc, WaveLayers(), /*revision*/ 1);

    std::vector<EvaluatedFrame> frames;
    worker.SetFrameCallback([&frames](const EvaluatedFrame& f)
    {
        frames.push_back(f);
    });

    const TimePoint epoch = Clock::now();
    worker.SetEpoch(epoch);

    /* Step at an explicit absolute time: t = now - epoch. */
    CHECK(worker.Step(epoch + std::chrono::milliseconds(250)));
    CHECK(frames.size() == 1);
    CHECK(frames[0].sequence == 1);
    CHECK(Near(frames[0].scene_time_s, 0.250));
    CHECK(frames[0].sample_time == epoch + std::chrono::milliseconds(250));
    CHECK(frames[0].colors == Reference(*doc, WaveLayers(), 0.250));
    CHECK(worker.AppliedRevision() == 1);
    CHECK(worker.HasScene());
}

static void TestAbsoluteTimeSampling()
{
    /* Late work samples CURRENT scene time: a Step at t = 0.90 s
       must evaluate 0.90 exactly — it does not replay the scheduled
       ticks between the previous step and now. */
    SceneWorker worker;
    auto doc = WaveDoc();
    worker.SubmitScene(doc, WaveLayers());

    std::vector<EvaluatedFrame> frames;
    worker.SetFrameCallback([&frames](const EvaluatedFrame& f)
    {
        frames.push_back(f);
    });

    const TimePoint epoch = Clock::now();
    worker.SetEpoch(epoch);

    CHECK(worker.Step(epoch));                                  /* t=0 */
    CHECK(worker.Step(epoch + std::chrono::milliseconds(900))); /* t=0.9 */
    CHECK(frames.size() == 2);
    CHECK(Near(frames[1].scene_time_s, 0.900));
    CHECK(frames[1].colors == Reference(*doc, WaveLayers(), 0.900));
    CHECK(frames[1].colors != frames[0].colors); /* time advanced */
    CHECK(frames[1].sequence == 2);
}

static void TestLatestWinsScene()
{
    SceneWorker worker;

    std::vector<EvaluatedFrame> frames;
    worker.SetFrameCallback([&frames](const EvaluatedFrame& f)
    {
        frames.push_back(f);
    });

    const TimePoint epoch = Clock::now();
    worker.SetEpoch(epoch);

    /* Two scenes submitted before any step: only the newest is ever
       applied — the superseded one is dropped, not queued. */
    worker.SubmitScene(WaveDoc(), SolidLayers(200, 0, 0), /*rev*/ 1);
    worker.SubmitScene(WaveDoc(), SolidLayers(0, 0, 200), /*rev*/ 2);
    CHECK(worker.Step(epoch));
    CHECK(worker.AppliedRevision() == 2);
    CHECK(frames.size() == 1);
    CHECK(frames[0].colors ==
          Reference(*WaveDoc(), SolidLayers(0, 0, 200), 0.0));
    CHECK(frames[0].colors !=
          Reference(*WaveDoc(), SolidLayers(200, 0, 0), 0.0));
}

static void TestNoScene()
{
    SceneWorker worker;
    CHECK(!worker.HasScene());
    CHECK(!worker.Step(Clock::now()));   /* nothing to evaluate */
    CHECK(worker.Sequence() == 0);
}

static void TestInputSlot()
{
    SceneWorker worker;
    auto doc = WaveDoc();
    worker.SubmitScene(doc, WaveLayers());

    std::vector<EvaluatedFrame> frames;
    worker.SetFrameCallback([&frames](const EvaluatedFrame& f)
    {
        frames.push_back(f);
    });

    const TimePoint epoch = Clock::now();
    worker.SetEpoch(epoch);

    /* Latest-wins applies to reactive input too. */
    auto input = std::make_shared<studio::InputState>();
    input->audio_level = 0.75f;
    worker.SubmitInput(input);
    CHECK(worker.Step(epoch));
    CHECK(frames.size() == 1);

    /* nullptr clears. */
    worker.SubmitInput(nullptr);
    CHECK(worker.Step(epoch));
    CHECK(frames.size() == 2);
}

static void TestReentrantInputProvider()
{
    /* Task 4.6 regression: the pull provider —
       ReactiveInputs::Snapshot() — stamps input-event ages
       on the scene clock, and EngineHost wires that as
       Worker().Running()/Epoch(). If Step ever invoked the
       provider while holding mutex_, those accessors re-lock
       a non-recursive std::mutex and the worker self-
       deadlocks on its first tick (pre-fix this test hung;
       the hang itself is the failure signature). */
    SceneWorker worker;
    auto doc = WaveDoc();
    worker.SubmitScene(doc, WaveLayers());

    std::vector<EvaluatedFrame> frames;
    worker.SetFrameCallback([&frames](const EvaluatedFrame& f)
    {
        frames.push_back(f);
    });

    const TimePoint epoch = Clock::now();
    worker.SetEpoch(epoch);

    std::atomic<int> epoch_reads{ 0 };
    std::atomic<int> running_reads{ 0 };
    auto input = std::make_shared<studio::InputState>();
    input->audio_level = 0.5f;
    worker.SetInputProvider(
        [&]() -> std::shared_ptr<const studio::InputState>
        {
            (void)worker.Epoch();    /* re-entrant worker reads */
            (void)worker.Running();
            ++epoch_reads;
            ++running_reads;
            return input;
        });

    CHECK(worker.Step(epoch + std::chrono::milliseconds(50)));
    CHECK(epoch_reads.load() == 1);
    CHECK(running_reads.load() == 1);
    CHECK(frames.size() == 1);
    /* The provider result is the tick's input — the pushed
       slot would be overridden the same way. */
}

static void TestWorkerThread()
{
    SceneWorker worker;
    worker.SubmitScene(WaveDoc(), SolidLayers(0, 200, 0));

    std::atomic<uint64_t> frames{ 0 };
    std::atomic<double>   last_t{ 0.0 };
    std::thread::id       cb_thread;
    worker.SetFrameCallback([&](const EvaluatedFrame& f)
    {
        ++frames;
        last_t.store(f.scene_time_s);
        cb_thread = std::this_thread::get_id();
    });

    CHECK(worker.Start());
    CHECK(worker.Running());
    CHECK(!worker.Start());              /* already running */

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    worker.Stop();
    CHECK(!worker.Running());

    /* ~9 ticks at 60 Hz in 150 ms; keep the bound loose — this is a
       liveness check, not a scheduler benchmark. */
    CHECK(frames.load() >= 2);
    CHECK(last_t.load() > 0.0);
    CHECK(cb_thread != std::this_thread::get_id());

    /* Stop is idempotent and the destructor is safe after it. */
    worker.Stop();
}

static void TestNextDeadline()
{
    const TimePoint epoch = Clock::now();

    /* First tick strictly after now on a past epoch: 25 ms in is
       tick 2 of the 60 Hz grid (epoch + 2/60 s). */
    TimePoint d = SceneWorker::NextDeadline(
        epoch, epoch + std::chrono::milliseconds(25));
    const TimePoint tick2 = epoch +
        std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(2.0 / 60.0));
    CHECK(d > epoch + std::chrono::milliseconds(25));
    CHECK(d <= epoch + std::chrono::milliseconds(25) +
                   std::chrono::milliseconds(17));
    CHECK(std::chrono::abs(d - tick2) < std::chrono::milliseconds(1));

    /* Missed ticks are dropped — a far-past epoch still yields the
       next grid point just ahead of now, not a catch-up backlog. */
    d = SceneWorker::NextDeadline(epoch, epoch + std::chrono::seconds(1));
    const TimePoint tick61 = epoch +
        std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(61.0 / 60.0));
    CHECK(std::chrono::abs(d - tick61) < std::chrono::milliseconds(1));

    /* Regression (task 3.1 review): an epoch at or after now is a
       future "scene starts at T" reference — the deadline is the
       epoch itself. Pre-fix, a negative elapsed was cast to
       uint64_t (UB, ~2^64 garbage), producing a deadline hundreds
       of millions of years out and silently deading the worker. */
    CHECK(SceneWorker::NextDeadline(epoch, epoch) == epoch);
    const TimePoint future = epoch + std::chrono::seconds(5);
    CHECK(SceneWorker::NextDeadline(future, epoch) == future);
    CHECK(SceneWorker::NextDeadline(future,
                                    future - std::chrono::milliseconds(1)) ==
          future);
    /* Past the future epoch, the grid resumes normally. */
    d = SceneWorker::NextDeadline(future,
                                  future + std::chrono::milliseconds(20));
    CHECK(d > future + std::chrono::milliseconds(20));
}

static void TestFutureEpochWorker()
{
    /* Live regression for the same bug: with a future epoch, the
       worker must still wake at the epoch and produce frames
       (t >= 0), not sleep forever on a garbage deadline. */
    SceneWorker worker;
    worker.SubmitScene(WaveDoc(), SolidLayers(0, 200, 0));

    std::atomic<uint64_t> frames{ 0 };
    std::atomic<int>      nonnegative_t{ 0 };
    worker.SetFrameCallback([&](const EvaluatedFrame& f)
    {
        ++frames;
        if(f.scene_time_s >= 0.0)
        {
            ++nonnegative_t;
        }
    });

    CHECK(worker.Start());
    worker.SetEpoch(Clock::now() + std::chrono::milliseconds(150));
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    worker.Stop();

    /* ~12 post-epoch ticks in the last 200 ms at 60 Hz; a stalled
       worker produces none with t >= 0. */
    CHECK(nonnegative_t.load() >= 5);
    CHECK(frames.load() >= 5);
}

static void TestEngineComposition()
{
    Engine engine;
    CHECK(!engine.Running());
    engine.Worker().SubmitScene(WaveDoc(), SolidLayers(1, 2, 3));

    std::atomic<uint64_t> frames{ 0 };
    engine.Worker().SetFrameCallback(
        [&frames](const EvaluatedFrame&) { ++frames; });

    CHECK(engine.Start());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    engine.Stop();
    CHECK(frames.load() >= 2);

    /* Reported playback status defaults per plan section 4. */
    CHECK(engine.Status().mode == PlaybackMode::LiveEffects);
    CHECK(engine.Status().upload == UploadState::None);

    PlaybackStatus s;
    s.mode = PlaybackMode::AnimationPlayback;
    s.upload = UploadState::Active;
    engine.SetPlaybackStatus(s);
    CHECK(engine.Status().mode == PlaybackMode::AnimationPlayback);
    CHECK(engine.Status().upload == UploadState::Active);
}

int main()
{
    TestDeterministicStep();
    TestAbsoluteTimeSampling();
    TestLatestWinsScene();
    TestNoScene();
    TestInputSlot();
    TestReentrantInputProvider();
    TestNextDeadline();
    TestFutureEpochWorker();
    TestWorkerThread();
    TestEngineComposition();
    std::printf("scene worker: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
