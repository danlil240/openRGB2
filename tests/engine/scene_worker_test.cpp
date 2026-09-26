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
    TestWorkerThread();
    TestEngineComposition();
    std::printf("scene worker: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
