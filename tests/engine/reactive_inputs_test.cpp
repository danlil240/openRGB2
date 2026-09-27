// SPDX-License-Identifier: GPL-2.0-or-later
//
// Reactive-input tests (Task 4.6 — src/engine/ReactiveInputs.h):
//   - the InputBus queue stays BOUNDED under flood and drops the
//     oldest events — bursts can't grow memory
//   - enable/disable actually starts and stops the injected
//     capture sources; a disabled input captures nothing
//   - a source that fails to start reports its own reason — the
//     slot stays disabled, never a fabricated toggle
//   - capture_paused (the Task 3.7 gate) feeds NOTHING to scene
//     evaluation — verified end to end at the LED level through
//     the SceneWorker input provider
//   - params: audio sensitivity domain, display-index bounds,
//     wrong-input rejection
//   - the persisted selection round-trips and re-arms
//   - a self-terminated source reaps its zombie thread on
//     re-enable instead of terminating (round-2 review)
//   - a throwing Start/Stop releases the transition slot —
//     no wedged busy flag, no hung Shutdown (round-2 review)
//   - pause/params changed mid-start are replayed onto the
//     source once it is running (round-2 review)
//   - inline reactive layers report the honest Live-only
//     playback reason
// No real desktop capture anywhere: every source is a fake.

#include "ReactiveInputs.h"
#include "SceneWorker.h"
#include "EngineTypes.h"
#include "PlaybackPresets.h"

#include "effects/EffectTypes.h"
#include "scene/SceneTypes.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace openrgb2;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

/*---------------------------------------------------------*\
|| Injected capture source — records every manager call;    ||
|| tests drive bus writes by hand.                          ||
\*---------------------------------------------------------*/
class FakeSource final : public IReactiveSource
{
public:
    bool Start(studio::InputBus* b) override
    {
        ++starts;
        bus = b;
        if(throw_start)
            throw std::runtime_error(name + ": start threw");
        if(fail_start)
        {
            status = name + ": start failed";
            return false;
        }
        running = true;
        status  = name + ": listening";
        if(push_on_start && bus != nullptr)
        {
            bus->SetAudioLevel(0.8f);
            bus->PushEvent("audio", 1.0f, 65);
        }
        return true;
    }
    void Stop() override
    {
        ++stops;
        running = false;
        if(throw_stop)
            throw std::runtime_error(name + ": stop threw");
        status  = name + ": off";
    }
    bool        Running() const override     { return running; }
    std::string Status() const override      { return status; }
    void        SetPaused(bool p) override   { paused = p; }
    void        SetSensitivity(float s) override { sens = s; }
    void        SetScreenIndex(int i) override   { screen = i; }

    std::string        name   = "fake";
    std::string        status = "fake: off";
    studio::InputBus*  bus    = nullptr;
    bool               running = false;
    bool               fail_start    = false;
    bool               throw_start   = false;
    bool               throw_stop    = false;
    bool               push_on_start = false;
    bool               paused        = false;
    float              sens   = 1.0f;
    int                screen = 0;
    int                starts = 0;
    int                stops  = 0;
};

/*---------------------------------------------------------*\
||| A fake whose Start really spawns a capture thread — the ||
||| regression hook for the enable/disable TOCTOU: racing   ||
||| commands for one input must yield exactly ONE live      ||
||| capture thread, and disable must join it. block_start   ||
||| holds the transition open so a racer reliably lands in  ||
||| the serialization window.                               ||
\*---------------------------------------------------------*/
class ThreadedSource final : public IReactiveSource
{
public:
    ~ThreadedSource() override { Stop(); }

    bool Start(studio::InputBus* b) override
    {
        std::lock_guard<std::mutex> lock(mu_);
        if(running_.load())
            return false;
        ++start_calls;
        bus_ = b;
        while(block_start.load())
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1));
        running_ = true;
        thread_ = std::thread([this]
        {
            int live = ++live_threads;
            int prev = max_live.load();
            while(live > prev &&
                  !max_live.compare_exchange_weak(prev, live))
            {
            }
            while(running_.load())
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(1));
            --live_threads;
        });
        return true;
    }
    void Stop() override
    {
        std::lock_guard<std::mutex> lock(mu_);
        running_ = false;
        if(thread_.joinable())
            thread_.join();
        bus_ = nullptr;
    }
    bool        Running() const override { return running_.load(); }
    std::string Status() const override  { return "fake"; }
    /* Recording hooks — prove the manager replays paused/
       params onto the source after Start settles (round-2
       review: mid-start changes used to be dropped). */
    void SetPaused(bool p) override
    {
        ++paused_calls;
        last_paused = p;
    }
    void SetSensitivity(float s) override
    {
        ++sens_calls;
        last_sens = s;
    }
    void SetScreenIndex(int i) override
    {
        ++screen_calls;
        last_screen = i;
    }

    std::mutex         mu_;
    std::thread        thread_;
    studio::InputBus*  bus_ = nullptr;
    std::atomic<bool>  running_     { false };
    std::atomic<bool>  block_start  { false };
    std::atomic<int>   start_calls  { 0 };
    std::atomic<int>   live_threads { 0 };
    std::atomic<int>   max_live     { 0 };
    std::atomic<int>   paused_calls { 0 };
    std::atomic<int>   sens_calls   { 0 };
    std::atomic<int>   screen_calls { 0 };
    std::atomic<bool>  last_paused  { false };
    std::atomic<float> last_sens    { 1.0f };
    std::atomic<int>   last_screen  { 0 };
};

/*---------------------------------------------------------*\
||| A fake whose capture thread can SELF-TERMINATE: it     ||
||| flips running_=false and returns while thread_ is      ||
||| still joinable — the same zombie shape the real        ||
||| sources leave after an in-capture failure (hook        ||
||| install failure, unsupported mix format). Start()      ||
||| reaps the zombie before respawning, mirroring the      ||
||| fixed production contract; zombie_seen records that    ||
||| the re-enable really did pass through a joinable       ||
||| thread_ (without the reap the move-assign would be     ||
||| std::terminate — the round-2 Finding-A crash).         ||
\*---------------------------------------------------------*/
class ZombieSource final : public IReactiveSource
{
public:
    ~ZombieSource() override { Stop(); }

    bool Start(studio::InputBus* b) override
    {
        std::lock_guard<std::mutex> lock(mu_);
        if(running_.load())
            return false;
        if(thread_.joinable())
        {
            zombie_seen = true;
            thread_.join();
        }
        ++starts;
        bus_     = b;
        running_ = true;
        thread_  = std::thread([this]
        {
            if(die_fast.load())
            {
                /* die like WinKeyHook's failed install:
                   running_ flips false, thread_ stays
                   joinable for whoever comes next */
                running_ = false;
                return;
            }
            while(running_.load())
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(1));
        });
        return true;
    }
    void Stop() override
    {
        std::lock_guard<std::mutex> lock(mu_);
        running_ = false;
        if(thread_.joinable())
            thread_.join();
        bus_ = nullptr;
    }
    bool        Running() const override { return running_.load(); }
    std::string Status() const override  { return "zombie"; }
    bool        Joinable() const         { return thread_.joinable(); }

    std::mutex         mu_;
    std::thread        thread_;
    studio::InputBus*  bus_ = nullptr;
    std::atomic<bool>  running_    { false };
    std::atomic<bool>  die_fast    { true };
    std::atomic<bool>  zombie_seen { false };
    std::atomic<int>   starts      { 0 };
};

/* A manager with per-id fakes; factory order matches
   InputIds(): [audio, screen, key]. */
struct Fixture
{
    ReactiveInputs::Deps deps;
    std::vector<FakeSource*> fakes;      /* construction order */
    int changes = 0;

    explicit Fixture(bool displays = true)
    {
        deps.now = [] { return 0.0; };
        deps.source_factory =
            [this](const std::string& id)
                -> std::unique_ptr<IReactiveSource>
            {
                auto s = std::make_unique<FakeSource>();
                s->name = id;
                fakes.push_back(s.get());
                return s;
            };
        if(displays)
        {
            deps.enumerate_displays = []
            {
                return std::vector<ReactiveDisplay>{
                    { 0, "DISPLAY1", 1920, 1080 },
                    { 1, "DISPLAY2", 2560, 1440 } };
            };
        }
        deps.changed = [this] { ++changes; };
    }
};

static void TestBoundedQueue()
{
    studio::InputBus bus;
    double now = 0.0;
    bus.SetNow([&now] { return now; });

    /* Flood: 500 pushes never grow the queue past its cap,
       and the cap drops the OLDEST events (newest wins). */
    for(int i = 0; i < 500; ++i)
        bus.PushEvent("key", 1.0f, i);
    studio::InputState snap = bus.Snapshot(100.0);
    CHECK(snap.events.size() <= 96);
    CHECK(snap.events.size() == 96);
    if(!snap.events.empty())
    {
        CHECK(snap.events.front().code == 404);
        CHECK(snap.events.back().code  == 499);
    }

    /* Stale events are pruned at snapshot time. */
    now = 1000.0;
    snap = bus.Snapshot(5.0);
    CHECK(snap.events.empty());

    /* Continuous signals bounded by construction: level and a
       fixed-size grid, not queues. */
    bus.SetAudioLevel(0.5f);
    bus.SetScreenGrid(2, 1, { studio::ColorF{}, studio::ColorF{} });
    snap = bus.Snapshot(5.0);
    CHECK(snap.audio_level == 0.5f);
    CHECK(snap.screen_cols == 2);
    bus.ClearAll();
    snap = bus.Snapshot(5.0);
    CHECK(snap.audio_level == 0.0f);
    CHECK(snap.screen_cols == 0);
}

static void TestEnableDisableLifecycle()
{
    Fixture f;
    ReactiveInputs m(std::move(f.deps));

    /* The factory ran per id; nothing is capturing yet — a
       manager alone is cheap. */
    CHECK(f.fakes.size() == 3);
    for(const ReactiveInputStatus& s : m.Status())
    {
        CHECK(s.available);
        CHECK(!s.enabled);
        CHECK(!s.running);
    }
    CHECK(m.Snapshot()->events.empty());

    std::string err;
    CHECK(m.SetEnabled("audio", true, err));
    CHECK(f.fakes[0]->starts == 1);
    CHECK(f.fakes[0]->running);
    /* Idempotent — no second Start, no double capture. */
    CHECK(m.SetEnabled("audio", true, err));
    CHECK(f.fakes[0]->starts == 1);
    {
        const std::vector<ReactiveInputStatus> st = m.Status();
        CHECK(st.front().enabled && st.front().running);
        CHECK(st.front().status == "audio: listening");
    }

    /* Disabled = the capture source is STOPPED — no hidden
       CPU/device cost behind an off toggle. */
    CHECK(m.SetEnabled("audio", false, err));
    CHECK(f.fakes[0]->stops == 1);
    CHECK(!f.fakes[0]->running);
    CHECK(!m.Status().front().enabled);
    CHECK(!m.Status().front().running);
    CHECK(f.changes >= 2);
}

static void TestUnavailableHonest()
{
    /* No source for a slot -> available:false + the honest
       reason; enabling is Unavailable, never a fake toggle. */
    ReactiveInputs::Deps deps;
    deps.source_factory =
        [](const std::string&)
            -> std::unique_ptr<IReactiveSource>
        { return nullptr; };
    ReactiveInputs m(std::move(deps));

    std::string err;
    CHECK(!m.SetEnabled("audio", true, err));
    CHECK(err.find("not available") != std::string::npos);
    for(const ReactiveInputStatus& s : m.Status())
    {
        CHECK(!s.available);
        CHECK(!s.enabled);
        CHECK(!s.running);
        CHECK(s.status.find("not available") !=
              std::string::npos);
    }
    CHECK(!m.SetEnabled("nonsense", true, err));  /* unknown id */
    CHECK(!m.SetSensitivity("nonsense", 1.0f, err));

    /* A source that fails to start: the slot reports the
       source's own reason and stays disabled. */
    ReactiveInputs::Deps deps2;
    deps2.source_factory =
        [](const std::string& id)
            -> std::unique_ptr<IReactiveSource>
        {
            auto s = std::make_unique<FakeSource>();
            s->name       = id;
            s->fail_start = true;
            return s;
        };
    ReactiveInputs m2(std::move(deps2));
    err.clear();
    CHECK(!m2.SetEnabled("audio", true, err));
    CHECK(err == "audio: start failed");
    CHECK(!m2.Status().front().enabled);
    CHECK(!m2.Status().front().running);
    CHECK(m2.Status().front().status == "audio: start failed");
}

static void TestParams()
{
    Fixture f;
    ReactiveInputs m(std::move(f.deps));
    std::string   err;
    FakeSource*   audio  = f.fakes[0];
    FakeSource*   screen = f.fakes[1];
    (void)screen;

    /* sensitivity is audio-only; the domain clamps to
       0.25..2.0 and is forwarded while running. */
    CHECK(m.SetSensitivity("audio", 10.0f, err));
    CHECK(!m.SetSensitivity("screen", 1.0f, err));
    CHECK(!m.SetSensitivity("key", 1.0f, err));
    CHECK(!m.SetSensitivity("bogus", 1.0f, err));
    CHECK(m.SetEnabled("audio", true, err));
    /* The stored (clamped) value was applied at Start. */
    CHECK(audio->sens == 2.0f);
    CHECK(m.SetSensitivity("audio", 1.5f, err));
    CHECK(audio->sens == 1.5f);
    CHECK(m.SetSensitivity("audio", 0.01f, err));
    CHECK(audio->sens == 0.25f);

    /* screen_index bounds-checks against the enumerated
       displays when the dep is wired. */
    CHECK(!m.SetScreenIndex(-1, err));
    CHECK(!m.SetScreenIndex(2, err));
    CHECK(m.SetScreenIndex(1, err));
    /* Forwarded once the screen source runs. */
    CHECK(m.SetEnabled("screen", true, err));
    CHECK(f.fakes[1]->screen == 1);
    CHECK(m.SetScreenIndex(0, err));
    CHECK(f.fakes[1]->screen == 0);
}

static void TestPauseGate()
{
    Fixture f;
    ReactiveInputs m(std::move(f.deps));
    std::string err;
    CHECK(m.SetEnabled("audio", true, err));
    CHECK(m.SetEnabled("key", true, err));

    m.Bus()->SetAudioLevel(0.7f);
    m.Bus()->PushEvent("key", 1.0f, 32);
    CHECK(!m.Snapshot()->events.empty());
    CHECK(m.Snapshot()->audio_level == 0.7f);

    /* capture_paused: sources are told AND the scene-side
       snapshot returns an empty state — nothing reaches
       evaluation while the gate holds. */
    m.SetPaused(true);
    CHECK(f.fakes[0]->paused);
    CHECK(f.fakes[2]->paused);
    CHECK(m.Paused());
    {
        const auto s = m.Snapshot();
        CHECK(s->events.empty());
        CHECK(s->audio_level == 0.0f);
        CHECK(s->screen_cols == 0);
    }
    for(const ReactiveInputStatus& st : m.Status())
        CHECK(st.paused);

    m.SetPaused(false);
    CHECK(!f.fakes[0]->paused);
    /* Resume restores what capture still holds. */
    CHECK(m.Snapshot()->audio_level == 0.7f);
}

/* The Task 3.7 gate end to end: pause stops captured values
   reaching SCENE evaluation — verified through the real
   SceneWorker input provider at the LED level. */
static void TestPauseReachesScene()
{
    Fixture f(false /* no display list needed */);
    ReactiveInputs m(std::move(f.deps));

    SceneWorker worker;
    auto doc = std::make_shared<studio::SceneDocument>();
    {
        studio::SceneObject o;
        o.id       = "dev";
        o.kind     = studio::ObjectKind::Device;
        o.geometry = "test_body";
        o.verified = true;
        studio::Emitter e;
        e.group   = "dev";
        e.address = 0;
        o.emitters.push_back(e);
        doc->objects.push_back(o);
    }
    /* A "level" layer: alpha tracks the audio envelope —
       deterministic (no event timing involved). */
    studio::EffectLayer lvl;
    lvl.primitive = "level";
    lvl.opacity   = 1.0f;
    lvl.palette   = studio::MakePalette(
        { studio::MakeSceneColor(255, 255, 255) });
    worker.SubmitScene(doc, { lvl }, /*revision*/ 1);

    const TimePoint epoch = Clock::now();
    worker.SetEpoch(epoch);
    worker.SetInputProvider([&m] { return m.Snapshot(); });

    std::vector<EvaluatedFrame> frames;
    worker.SetFrameCallback(
        [&frames](const EvaluatedFrame& fr)
        { frames.push_back(fr); });

    std::string err;
    CHECK(m.SetEnabled("audio", true, err));
    m.Bus()->SetAudioLevel(0.8f);

    CHECK(worker.Step(epoch + std::chrono::milliseconds(100)));
    CHECK(frames.size() == 1);
    CHECK(frames[0].colors["dev"].size() == 1);
    /* level 0.8 over a black base -> ~204 (0xCC) channels. */
    CHECK((frames[0].colors["dev"][0] & 0xFF) > 180);

    m.SetPaused(true);
    CHECK(worker.Step(epoch + std::chrono::milliseconds(200)));
    CHECK(frames.size() == 2);
    /* paused -> empty input -> the layer contributes nothing */
    CHECK((frames[1].colors["dev"][0] & 0xFF) == 0);

    m.SetPaused(false);
    CHECK(worker.Step(epoch + std::chrono::milliseconds(300)));
    CHECK(frames.size() == 3);
    CHECK((frames[2].colors["dev"][0] & 0xFF) > 180);
}

static void TestPersistRestore()
{
    /* Arm inputs + params on one manager, persist, restore
       into a second — the selection re-arms. */
    Fixture f1;
    std::string err;
    std::vector<ReactiveInputs::PersistedInput> saved;
    {
        ReactiveInputs m(std::move(f1.deps));
        CHECK(m.SetSensitivity("audio", 1.5f, err));
        CHECK(m.SetEnabled("audio", true, err));
        CHECK(m.SetScreenIndex(1, err));
        /* screen stays disabled — only its index persists */
        saved = m.PersistedState();
    }
    CHECK(saved.size() == 3);
    CHECK(saved[0].id == "audio" && saved[0].enabled);
    CHECK(saved[0].sensitivity == 1.5f);
    CHECK(saved[1].id == "screen" && !saved[1].enabled);
    CHECK(saved[1].screen_index == 1);

    Fixture f2;
    ReactiveInputs m2(std::move(f2.deps));
    m2.Restore(saved);
    CHECK(f2.fakes[0]->starts == 1);   /* audio re-armed */
    CHECK(f2.fakes[0]->running);
    CHECK(f2.fakes[0]->sens == 1.5f);  /* param applied at start */
    CHECK(f2.fakes[1]->starts == 0);   /* screen stayed off */
    CHECK(m2.Status()[1].screen_index == 1);
}

/* The Task-4.6-review TOCTOU: two sessions racing
   set_enabled for the same input while Start() is in flight.
   Commands serialize per slot — exactly one Start runs, one
   capture thread lives, and a disable issued mid-enable
   still wins after the start settles (it joins the thread). */
static void TestConcurrentEnableSerialized()
{
    ThreadedSource* src = nullptr;
    ReactiveInputs::Deps deps;
    deps.now = [] { return 0.0; };
    deps.source_factory =
        [&src](const std::string& id)
            -> std::unique_ptr<IReactiveSource>
        {
            if(id != "audio")
                return std::make_unique<FakeSource>();
            auto s = std::make_unique<ThreadedSource>();
            src = s.get();
            return s;
        };
    ReactiveInputs m(std::move(deps));
    CHECK(src != nullptr);

    /*--- concurrent enable + enable --------------------------*/
    src->block_start = true;      /* hold Start() open */
    bool r1 = false, r2 = false;
    std::string e1, e2;
    std::thread t1([&] { r1 = m.SetEnabled("audio", true, e1); });
    while(src->start_calls.load() == 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    /* t1 is inside Start() with the manager mutex dropped and
       the slot marked busy — t2 must serialize behind it,
       never enter the source a second time. */
    std::thread t2([&] { r2 = m.SetEnabled("audio", true, e2); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    src->block_start = false;
    t1.join();
    t2.join();
    CHECK(r1);
    CHECK(r2);                     /* second is idempotent */
    CHECK(src->start_calls.load() == 1);
    /* The spawned capture thread may not have run its first
       instruction yet — poll briefly for liveness. */
    for(int i = 0; i < 200 && src->live_threads.load() != 1; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(src->live_threads.load() == 1);
    CHECK(src->max_live.load() == 1);
    {
        const ReactiveInputStatus st = m.Status().front();
        CHECK(st.enabled && st.running);
    }

    /*--- disable issued while an enable is in flight ---------*/
    /* Re-disable first, then re-enable with Start held open
       and race a disable — the disable applies right after
       the start settles (last command wins). */
    std::string err;
    CHECK(m.SetEnabled("audio", false, err));
    CHECK(src->live_threads.load() == 0);

    src->block_start = true;
    bool r3 = false, r4 = false;
    std::string e3, e4;
    std::thread t3([&] { r3 = m.SetEnabled("audio", true, e3); });
    while(src->start_calls.load() == 1)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::thread t4([&] { r4 = m.SetEnabled("audio", false, e4); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    src->block_start = false;
    t3.join();
    t4.join();
    CHECK(r3);
    CHECK(r4);
    /* Deterministic end state: the disable won — nothing
       captures for a disabled input, no orphan thread. */
    CHECK(src->start_calls.load() == 2);
    CHECK(src->live_threads.load() == 0);
    {
        const ReactiveInputStatus st = m.Status().front();
        CHECK(!st.enabled && !st.running);
    }

    /*--- and a clean enable still works afterwards -----------*/
    CHECK(m.SetEnabled("audio", true, err));
    CHECK(src->start_calls.load() == 3);
    for(int i = 0; i < 200 && src->live_threads.load() != 1; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(src->live_threads.load() == 1);
    CHECK(m.SetEnabled("audio", false, err));
    CHECK(src->live_threads.load() == 0);
}

/* Round-2 Finding A: a source that self-terminated (running_
   false, thread_ still joinable) keeps the slot enabled — a
   re-enable then fails the idempotent check and re-enters
   Start(), where the zombie must be REAPED before respawn
   (the round-1 fix covered Stop() but missed this path:
   move-assign onto a joinable std::thread is std::terminate,
   reachable via a legal IPC command). */
static void TestZombieRespawn()
{
    ZombieSource* src = nullptr;
    ReactiveInputs::Deps deps;
    deps.now = [] { return 0.0; };
    deps.source_factory =
        [&src](const std::string& id)
            -> std::unique_ptr<IReactiveSource>
        {
            if(id != "audio")
                return std::make_unique<FakeSource>();
            auto s = std::make_unique<ZombieSource>();
            src = s.get();
            return s;
        };
    ReactiveInputs m(std::move(deps));
    CHECK(src != nullptr);

    std::string err;
    CHECK(m.SetEnabled("audio", true, err));
    CHECK(src->starts.load() == 1);

    /* The capture thread dies on its own: running_ goes
       false while thread_ is still joinable — the same
       zombie the real sources leave after an in-capture
       failure. */
    for(int i = 0;
        i < 200 && (src->Running() || !src->Joinable()); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(!src->Running());
    CHECK(src->Joinable());              /* the zombie exists */
    {
        /* enabled stays the user's selection; running is the
           honest liveness report (the contract's
           enabled:true, running:false state). */
        const ReactiveInputStatus st = m.Status().front();
        CHECK(st.enabled && !st.running);
    }

    /* Re-enable: enabled==requested but !Running() so the
       idempotent check fails and Start() re-runs. With the
       reap this respawns cleanly; without it this call was
       std::terminate. */
    src->die_fast = false;   /* the retry stays alive */
    CHECK(m.SetEnabled("audio", true, err));
    CHECK(src->starts.load() == 2);
    CHECK(src->zombie_seen.load());      /* proves Start()
            observed + reaped the joinable zombie */
    for(int i = 0; i < 200 && !src->Running(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(src->Running());
    {
        const ReactiveInputStatus st = m.Status().front();
        CHECK(st.enabled && st.running);
    }

    /* And the normal lifecycle still works afterwards. */
    CHECK(m.SetEnabled("audio", false, err));
    CHECK(!src->Running());
    CHECK(!src->Joinable());
}

/* Round-2 Finding B: a Start()/Stop() that THROWS must not
   wedge the slot — `busy` left set would block every later
   set_enabled on transition_cv_ forever and hang Shutdown().
   The exception still propagates (IPC boundary answers
   Internal); the slot must stay honest and usable. */
static void TestThrowingStartStopUnwedges()
{
    Fixture f;
    ReactiveInputs m(std::move(f.deps));
    std::string err;

    /*--- Start() throws ------------------------------------*/
    f.fakes[0]->throw_start = true;
    bool threw = false;
    try
    {
        m.SetEnabled("audio", true, err);
    }
    catch(const std::exception&)
    {
        threw = true;
    }
    CHECK(threw);
    /* Honest state: not enabled, not running. */
    {
        const ReactiveInputStatus st = m.Status().front();
        CHECK(!st.enabled && !st.running);
    }
    /* busy was released — this call completing IS the check
       (a wedged slot would hang here on transition_cv_). */
    f.fakes[0]->throw_start = false;
    CHECK(m.SetEnabled("audio", true, err));
    CHECK(m.Status().front().running);

    /*--- Stop() throws -------------------------------------*/
    f.fakes[0]->throw_stop = true;
    threw = false;
    try
    {
        m.SetEnabled("audio", false, err);
    }
    catch(const std::exception&)
    {
        threw = true;
    }
    CHECK(threw);
    CHECK(!m.Status().front().enabled);  /* honestly disabled */
    f.fakes[0]->throw_stop = false;
    /* Still not wedged: a fresh enable/disable cycle works. */
    CHECK(m.SetEnabled("audio", true, err));
    CHECK(m.SetEnabled("audio", false, err));
    /* Shutdown() at scope exit must not hang either —
       reaching the next test proves it. */
}

/* Round-2 Finding C: SetPaused/SetSensitivity/SetScreenIndex
   only push to a source that is already Running() — a change
   issued while a latch holds Start() open updates manager
   state but skips the source. The post-start replay must
   re-apply the persisted values so nothing is lost (a missed
   unpause = dead-but-enabled input; a missed pause = screen
   grabs while displays sleep). */
static void TestMidStartChangesReapplied()
{
    ThreadedSource* audio  = nullptr;
    ThreadedSource* screen = nullptr;
    ReactiveInputs::Deps deps;
    deps.now = [] { return 0.0; };
    deps.enumerate_displays = []
    {
        return std::vector<ReactiveDisplay>{
            { 0, "DISPLAY1", 1920, 1080 },
            { 1, "DISPLAY2", 2560, 1440 } };
    };
    deps.source_factory =
        [&audio, &screen](const std::string& id)
            -> std::unique_ptr<IReactiveSource>
        {
            if(id == "key")
                return std::make_unique<FakeSource>();
            auto s = std::make_unique<ThreadedSource>();
            if(id == "audio") audio  = s.get();
            else              screen = s.get();
            return s;
        };
    ReactiveInputs m(std::move(deps));
    CHECK(audio != nullptr);
    CHECK(screen != nullptr);

    /*--- pause + sensitivity land inside the start window ---*/
    audio->block_start = true;
    bool r = false;
    std::string e;
    std::thread t1([&] { r = m.SetEnabled("audio", true, e); });
    while(audio->start_calls.load() == 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    /* t1 is inside Start() with the mutex dropped: these hit
       manager state only — the not-yet-running source is
       skipped. */
    m.SetPaused(true);
    std::string err;
    CHECK(m.SetSensitivity("audio", 1.5f, err));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    audio->block_start = false;
    t1.join();
    CHECK(r);
    /* The post-start replay pushed the persisted values:
       last write is the mid-start one, and the source saw
       both the pre-Start application and the replay. */
    CHECK(audio->last_paused.load());
    CHECK(audio->last_sens.load() == 1.5f);
    CHECK(audio->paused_calls.load() >= 2);
    CHECK(audio->sens_calls.load()  >= 2);

    /*--- screen_index lands inside the start window --------*/
    screen->block_start = true;
    r = false;
    e.clear();
    std::thread t2([&] { r = m.SetEnabled("screen", true, e); });
    while(screen->start_calls.load() == 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(m.SetScreenIndex(1, err));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    screen->block_start = false;
    t2.join();
    CHECK(r);
    CHECK(screen->last_screen.load() == 1);
    CHECK(screen->screen_calls.load() >= 2);
    /* The pause gate replayed too — a paused start comes up
       gated (plan section 6). */
    CHECK(screen->last_paused.load());

    m.SetPaused(false);
}

static void TestReactiveSceneReason()
{
    /* Inline reactive layers -> Live-only playback with the
       honest reason (Task 4.6: never silently uploadable). */
    auto with_layers = [](const std::string& primitive)
    {
        studio::SceneDocument doc;
        studio::EffectLayer l;
        l.primitive = primitive;
        doc.effect.layers = { l };
        return doc;
    };
    {
        studio::SceneDocument doc = with_layers("level");
        const ScenePlaybackCaps caps =
            ClassifyScenePlayback(&doc);
        CHECK(!caps.caps.animation_playback);
        CHECK(caps.reason.find("audio") != std::string::npos);
        CHECK(caps.reason.find("live") != std::string::npos);
    }
    {
        studio::SceneDocument doc = with_layers("ripple");
        const ScenePlaybackCaps caps =
            ClassifyScenePlayback(&doc);
        CHECK(!caps.caps.animation_playback);
        CHECK(caps.reason.find("input") != std::string::npos);
    }
    {
        studio::SceneDocument doc = with_layers("screenfield");
        const ScenePlaybackCaps caps =
            ClassifyScenePlayback(&doc);
        CHECK(!caps.caps.animation_playback);
        CHECK(caps.reason.find("screen") != std::string::npos);
    }
    /* A non-reactive inline layer keeps the generic reason. */
    {
        studio::SceneDocument doc = with_layers("wave");
        const ScenePlaybackCaps caps =
            ClassifyScenePlayback(&doc);
        CHECK(!caps.caps.animation_playback);
        CHECK(caps.reason.find("screen") == std::string::npos);
    }
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    TestBoundedQueue();
    TestEnableDisableLifecycle();
    TestUnavailableHonest();
    TestParams();
    TestPauseGate();
    TestPauseReachesScene();
    TestPersistRestore();
    TestConcurrentEnableSerialized();
    TestZombieRespawn();
    TestThrowingStartStopUnwedges();
    TestMidStartChangesReapplied();
    TestReactiveSceneReason();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
