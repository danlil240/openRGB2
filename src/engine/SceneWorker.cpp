/*---------------------------------------------------------*\
|| SceneWorker.cpp                                           |
||                                                           |
||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "SceneWorker.h"

#include <cmath>
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

void SceneWorker::SubmitScene(
    std::shared_ptr<const studio::SceneDocument> doc,
    std::vector<studio::EffectLayer> layers,
    uint64_t revision)
{
    std::lock_guard<std::mutex> lock(mutex_);
    pending_scene_.doc      = std::move(doc);
    pending_scene_.layers   = std::move(layers);
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
    return active_scene_.doc != nullptr;
}

void SceneWorker::SetEpoch(TimePoint epoch)
{
    std::lock_guard<std::mutex> lock(mutex_);
    epoch_ = epoch;
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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        provider = input_provider_;
    }
    std::shared_ptr<const studio::InputState> pulled_input;
    if(provider)
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
            engine_.SetLayers(active_scene_.layers);
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
        if(provider)
        {
            active_input_ = std::move(pulled_input);
        }

        evaluate = active_scene_.doc != nullptr;
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
        frame.scene_time_s =
            std::chrono::duration<double>(now - epoch_).count();
        frame.sample_time  = now;
        cb = callback_;
    }

    /* Evaluate outside the lock — the core engine is pure; the
       callback may re-enter the worker (SubmitScene/SubmitInput)
       without deadlocking. Idle-hook frames skip evaluation: there
       is no scene, so colors stay empty and the callback's overlay
       pass (identify) is what produces output. */
    if(evaluate)
    {
        engine_.Evaluate(*active_scene_.doc, frame.scene_time_s,
                         frame.colors, active_input_.get());
    }
    if(cb)
    {
        cb(frame);
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
