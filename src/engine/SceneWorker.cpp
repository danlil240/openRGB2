/*---------------------------------------------------------*\
|| SceneWorker.cpp                                           |
||                                                           |
||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "SceneWorker.h"

#include <cmath>
#include <exception>
#include <utility>

namespace openrgb2
{

SceneWorker::~SceneWorker()
{
    Stop();
}

void SceneWorker::SetFrameCallback(FrameCallback cb)
{
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(cb);
}

void SceneWorker::SetIdleFrameHook(IdleFrameHook hook)
{
    std::lock_guard<std::mutex> lock(mutex_);
    idle_frame_hook_ = std::move(hook);
}

void SceneWorker::SetOutputGate(OutputGate gate)
{
    std::lock_guard<std::mutex> lock(mutex_);
    output_gate_ = std::move(gate);
}

void SceneWorker::SubmitScene(
    std::shared_ptr<const studio::SceneDocument> doc,
    std::vector<studio::EffectLayer> layers,
    uint64_t revision)
{
    PreparedStudioScene prepared;
    prepared.doc    = std::move(doc);
    prepared.layers = std::move(layers);
    SubmitPreparedScene(prepared, revision);
}

void SceneWorker::SubmitPreparedScene(const PreparedStudioScene& scene,
                                      uint64_t revision)
{
    std::lock_guard<std::mutex> lock(mutex_);
    pending_scene_.scene    = scene;      /* doc pointer shared —
                                           never a doc copy */
    pending_scene_.revision = revision;
    has_pending_scene_      = true;
}

void SceneWorker::SubmitInput(std::shared_ptr<const studio::InputState> input)
{
    std::lock_guard<std::mutex> lock(mutex_);
    pending_input_      = std::move(input);
    has_pending_input_  = true;
}

void SceneWorker::SetInputProvider(InputProvider provider)
{
    std::lock_guard<std::mutex> lock(mutex_);
    input_provider_ = std::move(provider);
}

void SceneWorker::SetLogger(EngineLogFn log)
{
    std::lock_guard<std::mutex> lock(mutex_);
    log_ = std::move(log);
}

void SceneWorker::Log(const std::string& line)
{
    EngineLogFn sink;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sink = log_;
    }
    EmitEngineLog(sink, line);
}

void SceneWorker::NoteStepFailure(const std::string& msg)
{
    /* Step is single-caller (the worker thread owns it), so the
       counter needs no lock. Throttle: a persistent fault would
       otherwise print 60 lines/second — first 3 immediately,
       then one summary line every ~10 s of ticks. */
    const uint64_t n = ++step_failures_;
    if(n <= 3 || n % 600 == 0)
    {
        Log(msg + " (step failure #" + std::to_string(n) + ")");
    }
}

void SceneWorker::SetEvaluateGate(EvaluateGate gate)
{
    std::lock_guard<std::mutex> lock(mutex_);
    evaluate_gate_ = std::move(gate);
}

uint64_t SceneWorker::Evaluations() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return evaluations_;
}

TimePoint SceneWorker::Epoch() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return epoch_;
}

uint64_t SceneWorker::AppliedRevision() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return active_scene_.revision;
}

uint64_t SceneWorker::Sequence() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return sequence_;
}

bool SceneWorker::HasScene() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return active_scene_.scene.doc != nullptr;
}

void SceneWorker::SetEpoch(TimePoint epoch)
{
    std::lock_guard<std::mutex> lock(mutex_);
    epoch_ = epoch;
}

double SceneWorker::SceneTimeLocked(TimePoint now) const
{
    if(held_)
    {
        return held_time_s_;
    }
    return std::chrono::duration<double>(now - epoch_).count();
}

bool SceneWorker::SetHold(bool on, TimePoint now)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if(on == held_)
    {
        return false;
    }
    if(on)
    {
        held_        = true;
        hold_start_  = now;
        /* A future epoch ("scene starts at T") freezes at t = 0
           — the scene clock never runs backwards. */
        held_time_s_ = now > epoch_
            ? std::chrono::duration<double>(now - epoch_).count()
            : 0.0;
        return true;
    }
    held_ = false;
    /* Resume from the paused frame: t = now - epoch must equal
       the held time again — shift by the (integer) time spent
       held; a hold that froze a future epoch at t = 0 resumes
       at t = 0. */
    if(hold_start_ > epoch_)
    {
        epoch_ += now - hold_start_;
    }
    else
    {
        epoch_ = now;
    }
    /* Input stamped on the old epoch would evaluate with wrong
       ages — drop it; the provider's next pull (or the next
       push) supplies fresh state. */
    pending_input_.reset();
    has_pending_input_ = false;
    active_input_.reset();
    return true;
}

bool SceneWorker::Held() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return held_;
}

std::shared_ptr<const studio::InputState>
SceneWorker::PendingInputForTesting() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_input_;
}

std::shared_ptr<const studio::InputState>
SceneWorker::ActiveInputForTesting() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return active_input_;
}

bool SceneWorker::Step(TimePoint now)
{
    FrameCallback cb;
    EvaluatedFrame frame;
    bool         evaluate;

    /* Pull input OUTSIDE the worker lock: the provider —
       ReactiveInputs::Snapshot() — stamps event ages through
       the scene clock, and EngineHost wires that as
       Worker().Running()/Epoch(), both of which take mutex_.
       Invoking the provider while holding mutex_ self-
       deadlocks on a non-recursive std::mutex. The provider
       runs only on the worker thread (Step is single-caller),
       so capturing it under the lock and calling it here
       keeps latest-wins semantics identical. */
    InputProvider provider;
    OutputGate    gate;
    EvaluateGate  eval_gate;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        /* R-1: a held clock pulls no input (frozen frame). */
        provider  = held_ ? InputProvider() : input_provider_;
        gate      = output_gate_;
        eval_gate = evaluate_gate_;
    }
    /* The gates are read outside the lock for the same reason as
       the provider (they take the pipeline's / playback's own
       mutexes). */
    const bool gated = gate && gate();
    /* E-6: output owned by another producer — tick the clock and
       run the callback, but evaluate nothing. */
    const bool skip_eval = !gated && eval_gate && eval_gate();
    std::shared_ptr<const studio::InputState> pulled_input;
    if(provider && !gated && !skip_eval)
    {
        pulled_input = provider();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);

        /* Latest-wins: apply the newest submitted state; anything it
           replaced is dropped, never replayed. */
        if(has_pending_scene_)
        {
            active_scene_      = std::move(pending_scene_);
            pending_scene_     = SceneState{};
            has_pending_scene_ = false;
            /* Audit C-2: replace at this tick's scene time so a
               layer whose speed/wavelength changed keeps its
               pattern where it is (t below is the same value). */
            engine_.ReplaceLayers(
                active_scene_.scene.layers,
                SceneTimeLocked(now));
            world_dirty_ = true;
        }
        if(has_pending_input_)
        {
            active_input_      = std::move(pending_input_);
            pending_input_.reset();
            has_pending_input_ = false;
        }
        /* Pull input: the provider (ReactiveInputs) owns the
           per-tick state when wired — pause/disable takes
           effect on the very next tick. */
        if(provider && !gated && !skip_eval)
        {
            active_input_ = std::move(pulled_input);
        }
        if(gated)
        {
            return false;   /* output gated: no evaluate/map */
        }

        evaluate = active_scene_.scene.doc != nullptr;
        if(!evaluate &&
           (!idle_frame_hook_ || !idle_frame_hook_()))
        {
            /* No scene and nothing downstream needs a frame —
               the clock idles (Task 4.4 review: an identify
               overlay wires the hook, so the blink reaches
               hardware with no scene loaded). */
            return false;
        }

        /* Absolute-time sampling: t derives from the time this Step
           actually runs at — a late tick evaluates the current scene
           time, never the scheduled tick it missed. */
        frame.sequence     = ++sequence_;
        frame.scene_time_s = SceneTimeLocked(now);   /* frozen
                                                        while held */
        frame.sample_time  = now;
        frame.scene_revision = active_scene_.revision;
        if(evaluate && skip_eval)
        {
            evaluate             = false;
            frame.evaluate_gated = true;
        }
        if(evaluate)
        {
            ++evaluations_;
        }
        cb = callback_;
    }

    /* Evaluate outside the lock — the core engine is pure; the
       callback may re-enter the worker (SubmitScene/SubmitInput)
       without deadlocking. Idle-hook frames skip evaluation: there
       is no scene, so colors stay empty and the callback's overlay
       pass (identify) is what produces output.

       Fault containment (QA hardening): an exception escaping
       Evaluate or the frame callback would propagate out of Run()
       and std::terminate the process. Catch, log (throttled), and
       continue — the tick counts as produced so the sequence stays
       monotonic; a bad callback can never kill the worker. */
    try
    {
        if(evaluate)
        {
            /* Task 6: transient key events resolve to emitter world
               positions through the prepared scene's lookup — the
               copy stays local to this tick; typed-text history is
               never retained on the worker. */
            const studio::InputState* input = active_input_.get();
            std::shared_ptr<const studio::InputState> positioned;
            if(input != nullptr && !input->events.empty() &&
               !active_scene_.scene.key_positions.empty())
            {
                positioned = std::make_shared<studio::InputState>(
                    PositionStudioInput(*input, active_scene_.scene));
                input = positioned.get();
            }
            if(world_dirty_)
            {
                /* Once per applied document, inside the fault
                   containment below. */
                active_world_ = studio::ResolveWorldMatrices(
                    *active_scene_.scene.doc);
                world_dirty_  = false;
            }
            /* Audit C-10: evaluate into last tick's buffers. The
               callback only reads the frame (synchronously), so
               the colors come back to the scratch afterwards. */
            frame.colors.swap(frame_scratch_);
            engine_.Evaluate(*active_scene_.scene.doc, active_world_,
                             frame.scene_time_s,
                             frame.colors, input);
        }
        if(cb)
        {
            cb(frame);
        }
        if(evaluate)
        {
            frame_scratch_.swap(frame.colors);
        }
    }
    catch(const std::exception& e)
    {
        NoteStepFailure(std::string("scene worker step threw: ") +
                        e.what());
    }
    catch(...)
    {
        NoteStepFailure("scene worker step threw an unknown "
                        "exception");
    }
    return true;
}

bool SceneWorker::Start()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if(run_)
    {
        return false;
    }
    epoch_ = Clock::now();
    run_   = true;
    thread_ = std::thread(&SceneWorker::Run, this);
    return true;
}

void SceneWorker::Stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!run_)
        {
            return;
        }
        run_ = false;
    }
    cv_.notify_all();
    if(thread_.joinable())
    {
        thread_.join();
    }
}

bool SceneWorker::Running() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return run_;
}

TimePoint SceneWorker::NextDeadline(TimePoint epoch, TimePoint now)
{
    const std::chrono::duration<double> period(1.0 / kTickHz);
    const double elapsed =
        std::chrono::duration<double>(now - epoch).count();
    if(elapsed <= 0.0)
    {
        /* Epoch is now or in the future: the first frame is due at
           the epoch itself (t = 0). Returning `epoch` here also
           avoids casting a negative tick index to uint64_t, which
           would be UB and could yield a deadline ~2^64 ns out —
           silently stalling the worker while it reports Running(). */
        return epoch;
    }
    const uint64_t next_tick =
        static_cast<uint64_t>(std::floor(elapsed * kTickHz)) + 1;
    return epoch + std::chrono::duration_cast<Clock::duration>(
                       period * static_cast<double>(next_tick));
}

void SceneWorker::Run()
{
    for(;;)
    {
        /* Deadline = the first tick strictly after now. Ticks already
           missed are dropped — the next evaluation samples the scene
           at the time it runs, not the time it was due. */
        TimePoint deadline;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(!run_)
            {
                return;
            }
            deadline = NextDeadline(epoch_, Clock::now());
        }

        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_until(lock, deadline, [this] { return !run_; });
            if(!run_)
            {
                return;
            }
        }

        Step(Clock::now());
    }
}

} /* namespace openrgb2 */
