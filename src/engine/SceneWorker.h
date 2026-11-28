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

#include "EngineLog.h"
#include "EngineTypes.h"
#include "StudioScenePreparation.h"

#include "effects/EffectEngine.h"
#include "scene/SceneTypes.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
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
    uint64_t            scene_revision = 0; /* applied revision the
                                               colors belong to
                                               (0 = no scene)       */
    studio::FrameColors colors;             /* object id -> LEDs    */
    /* True when the evaluate gate skipped evaluation (E-6):
       `colors` is empty on purpose and the frame must not be
       mapped to devices — only the clock-driven consumers
       (uploaded-playback Tick) use it. */
    bool                evaluate_gated = false;
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

    /* No-scene ticks normally produce nothing. A host-wired idle
       hook can request the frame path anyway — the one consumer is
       the device mapper's identify overlay (Task 4.4): the blink
       must reach hardware BEFORE any scene exists to bind it.
       While the hook reports true, Step produces an EvaluatedFrame
       with an EMPTY color map — no scene evaluation — and runs the
       normal frame callback, so overlay output still flows to the
       transports. Queried once per no-scene tick; must be cheap.
       The hook bounds the ticking itself: it reports false once
       its work ends (an expired-but-unpruned overlay still counts
       as work — the prune/restore frame needs its tick). */
    using IdleFrameHook = std::function<bool()>;
    void     SetIdleFrameHook(IdleFrameHook hook);

    /* Latest-wins scene submission: `doc` is immutable once shared;
       `layers` is the caller-resolved effect stack (preset/inline
       resolution stays with the scene provider). `revision` is
       caller-assigned bookkeeping echoed by AppliedRevision().
       Replaces any not-yet-applied scene. */
    void     SubmitScene(std::shared_ptr<const studio::SceneDocument> doc,
                         std::vector<studio::EffectLayer> layers,
                         uint64_t revision = 0);

    /* Task 6 shared prepared scene (StudioScenePreparation): the
       workspace-apply path submits the expanded document, the
       runtime layer stack (global params + ripple decay applied to
       the COPY), and the keyboard-position lookup as one atomic
       state — the same state the Task 7 preview evaluates. The
       worker enriches transient key events through
       PositionStudioInput so ripples spawn at real key positions. */
    void     SubmitPreparedScene(const PreparedStudioScene& scene,
                                 uint64_t revision = 0);

    /* Latest-wins reactive-input submission; nullptr clears. */
    void     SubmitInput(std::shared_ptr<const studio::InputState> input);

    /* Pull-style reactive input (Task 4.6): when set, the
       provider is queried once per tick and its result becomes
       the tick's input — the ReactiveInputs manager is the
       producer, so a paused/disabled source stops feeding the
       scene on the very next tick rather than waiting for a
       push. Takes precedence over the SubmitInput depth-1
       slot. Must be cheap and non-blocking. It is invoked on
       the worker thread WITHOUT mutex_ held — it may read
       worker state (Epoch(), Running()) but must not call
       Step(). */
    using InputProvider =
        std::function<std::shared_ptr<const studio::InputState>()>;
    void     SetInputProvider(InputProvider provider);

    /* Output gate (display-off / suspend): while it reports
       true, Step still applies submitted scene/input state
       (AppliedRevision stays truthful) but skips the input pull,
       the scene evaluation and the frame callback — every
       frame would only be rejected at the gated pipeline. The
       clock keeps ticking, so the first ungated tick evaluates
       the current scene time and the identify overlay's
       expiry/restore runs then (its restore would be refused
       while gated anyway). Queried once per tick WITHOUT
       mutex_ held; must be cheap. */
    using OutputGate = std::function<bool()>;
    void     SetOutputGate(OutputGate gate);

    /* Evaluate gate (E-6): while it reports true, Step applies
       submitted state and still runs the frame callback on the
       scene clock, but skips the input pull and the scene
       evaluation — the frame carries evaluate_gated = true and
       an empty color map. For output owned by another producer
       (uploaded playback) that still needs the clock tick.
       Queried once per tick WITHOUT mutex_ held; must be cheap.
       The output gate takes precedence (no callback at all). */
    using EvaluateGate = std::function<bool()>;
    void     SetEvaluateGate(EvaluateGate gate);

    /* Ticks that actually ran the scene evaluation (gated and
       idle-hook frames excluded) — diagnostics/tests. */
    uint64_t Evaluations() const;

    /* The scene clock's t = 0 reference. Reactive-input
       providers stamp event times on this base so event ages
       evaluate correctly against the tick's scene_time_s. */
    TimePoint Epoch() const;

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
       set AND no idle hook requested a frame (an idle-hook frame
       carries an empty color map — evaluation is skipped). The
       threaded loop calls this with Clock::now() after waking;
       tests call it directly with synthetic times.
       Single-caller: do not invoke Step concurrently (the worker
       thread owns it while Running()).

       Fault containment: an exception escaping Evaluate or the
       frame callback would terminate the worker thread
       (std::terminate on the Run loop). Step catches, logs via
       the installed sink (throttled — first failures + a
       periodic count), and the tick still counts as produced. */
    bool     Step(TimePoint now);

    /* Optional log sink (EngineLogFn) for worker-internal
       diagnostics — evaluation/callback faults above. Defaults
       to stderr with the "openrgb2-engine:" prefix. */
    void     SetLogger(EngineLogFn log);

    /* Starts/stops the dedicated 60 Hz worker thread. Start()
       returns false when already running. Stop() is synchronous. */
    bool     Start();
    void     Stop();
    bool     Running() const;

    /* Next tick deadline strictly after `now` for the tick grid
       anchored at `epoch`. Missed ticks are dropped — the result is
       the first grid point after `now`, never a backlog. When `epoch`
       is at or after `now` (a future "scene starts at T" epoch), the
       deadline is the epoch itself (t = 0 first frame). Exposed for
       deterministic deadline tests; the worker loop uses it too. */
    static TimePoint NextDeadline(TimePoint epoch, TimePoint now);

private:
    struct SceneState
    {
        uint64_t            revision = 0;
        /* doc/layers/key_positions move as ONE prepared unit —
           a stale document can never pair with a newer key
           lookup. */
        PreparedStudioScene scene;
    };

    void Run();
    void NoteStepFailure(const std::string& msg);
    void Log(const std::string& line);

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
    /* World matrices of active_scene_'s document — resolved once
       when the scene is applied (the document is immutable once
       shared), not every 60 Hz tick. Worker-thread state. */
    std::map<std::string, studio::Mat4> active_world_;
    bool                                world_dirty_ = true;
    /* The previous tick's frame colors, handed back to Evaluate so
       it reuses their map nodes and vectors (audit C-10): swapped
       into the frame for evaluation + callback, then swapped back.
       Worker-thread state. */
    studio::FrameColors                 frame_scratch_;
    FrameCallback         callback_;
    IdleFrameHook         idle_frame_hook_;
    InputProvider         input_provider_;
    OutputGate            output_gate_;
    EvaluateGate          evaluate_gate_;
    uint64_t              evaluations_ = 0;     /* mutex_ */
    EngineLogFn           log_;
    /* Worker-thread-only counter (Step is single-caller) —
       throttles the per-fault log line. */
    uint64_t              step_failures_ = 0;
};

} /* namespace openrgb2 */
