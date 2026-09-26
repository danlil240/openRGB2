/*---------------------------------------------------------*\
|| SceneWorker.h                                             |
||                                                           |
||   Scene-evaluation worker for the independent engine.    |
||   The 60 Hz scene clock (plan section 4) and the         |
||   evaluation loop run on a dedicated worker thread that  |
||   calls studio::EffectEngine::Evaluate(doc, t, frame,    |
||   input) — the core stays deterministic and              |
||   hardware-independent; this class only owns the clock   |
||   and the latest-wins state slots.                       |
||                                                           |
||   Plan section 4: late work samples current scene time   |
||   rather than replaying an obsolete FIFO. Both scene and |
||   input submissions are depth-1 slots — a new submission |
||   replaces any not-yet-applied state — and every tick    |
||   evaluates at the wall/monotonic time it actually runs, |
||   never at the time the tick was scheduled.              |
||                                                           |
||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include "EngineTypes.h"

#include "effects/EffectEngine.h"
#include "scene/SceneTypes.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace openrgb2
{

/* One evaluated scene tick — the engine-side analogue of an
   RgbFrame before device mapping (a later M3 task) splits it
   into per-controller frames. */
struct EvaluatedFrame
{
    uint64_t            sequence = 0;
    double              scene_time_s = 0.0; /* t handed to Evaluate */
    TimePoint           sample_time;        /* monotonic sample     */
    studio::FrameColors colors;             /* object id -> LEDs    */
};

class SceneWorker
{
public:
    /* Plan section 4: 60 Hz scene clock. */
    static constexpr double kTickHz = 60.0;

    using FrameCallback = std::function<void(const EvaluatedFrame&)>;

    SceneWorker() = default;
    ~SceneWorker();                          /* Stop() */

    SceneWorker(const SceneWorker&)            = delete;
    SceneWorker& operator=(const SceneWorker&) = delete;

    /* Invoked on the worker thread once per produced frame. Must be
       cheap and non-blocking — a slow callback delays the next tick,
       which then samples the CURRENT scene time (frames are dropped,
       never queued). */
    void     SetFrameCallback(FrameCallback cb);

    /* Latest-wins scene submission: `doc` is immutable once shared;
       `layers` is the caller-resolved effect stack (preset/inline
       resolution stays with the scene provider). `revision` is
       caller-assigned bookkeeping echoed by AppliedRevision().
       Replaces any not-yet-applied scene. */
    void     SubmitScene(std::shared_ptr<const studio::SceneDocument> doc,
                         std::vector<studio::EffectLayer> layers,
                         uint64_t revision = 0);

    /* Latest-wins reactive-input submission; nullptr clears. */
    void     SubmitInput(std::shared_ptr<const studio::InputState> input);

    /* Scene revision most recently applied by a Step. */
    uint64_t AppliedRevision() const;
    uint64_t Sequence() const;               /* frames produced      */
    bool     HasScene() const;

    /* Sets the t = 0 reference for the scene clock. Start() does
       this implicitly with Clock::now(). */
    void     SetEpoch(TimePoint epoch);

    /* One deterministic evaluation step at absolute time `now`:
       applies pending state, samples t = now - epoch, evaluates,
       and fires the frame callback. Returns false when no scene is
       set. The threaded loop calls this with Clock::now() after
       waking; tests call it directly with synthetic times.
       Single-caller: do not invoke Step concurrently (the worker
       thread owns it while Running()). */
    bool     Step(TimePoint now);

    /* Starts/stops the dedicated 60 Hz worker thread. Start()
       returns false when already running. Stop() is synchronous. */
    bool     Start();
    void     Stop();
    bool     Running() const;

private:
    struct SceneState
    {
        uint64_t                                     revision = 0;
        std::shared_ptr<const studio::SceneDocument> doc;
        std::vector<studio::EffectLayer>             layers;
    };

    void Run();

    mutable std::mutex      mutex_;
    std::condition_variable cv_;
    std::thread             thread_;
    bool                    run_ = false;

    TimePoint               epoch_{};
    uint64_t                sequence_ = 0;

    /* Depth-1 pending slots + applied state (mutex_). */
    SceneState                                    pending_scene_;
    SceneState                                    active_scene_;
    std::shared_ptr<const studio::InputState>     pending_input_;
    std::shared_ptr<const studio::InputState>     active_input_;
    bool                                          has_pending_scene_ = false;
    bool                                          has_pending_input_ = false;

    studio::EffectEngine  engine_;
    FrameCallback         callback_;
};

} /* namespace openrgb2 */
