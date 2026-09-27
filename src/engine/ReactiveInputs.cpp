/*---------------------------------------------------------*\
||| ReactiveInputs.cpp                                        ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "ReactiveInputs.h"

#include <algorithm>
#include <chrono>

namespace openrgb2
{

namespace
{

/* Events older than this never reach scene evaluation — the
   engine-side bound on how long a captured event remains
   live (the bus additionally caps the queue at 96). */
constexpr float kEventMaxAgeS = 5.0f;

} /* namespace */

const std::vector<std::string>& ReactiveInputs::InputIds()
{
    static const std::vector<std::string> ids =
        { "audio", "screen", "key" };
    return ids;
}

ReactiveInputs::ReactiveInputs(Deps deps)
    : deps_(std::move(deps))
{
    if(deps_.now)
    {
        /* Input events must be stamped on the SCENE clock —
           EvalRipple ages them as `in.t - e.t` against the
           worker's epoch, so the bus shares the provider. */
        std::function<double()> now = deps_.now;
        bus_.SetNow(
            [now]()
            {
                return static_cast<float>(now());
            });
    }

    if(deps_.source_factory)
    {
        for(const std::string& id : InputIds())
        {
            Slot* s = Find(id);
            if(s != nullptr)
                s->source = deps_.source_factory(id);
        }
    }
}

ReactiveInputs::~ReactiveInputs()
{
    Shutdown();
}

ReactiveInputs::Slot* ReactiveInputs::Find(const std::string& id)
{
    if(id == "audio")  return &audio_;
    if(id == "screen") return &screen_;
    if(id == "key")    return &key_;
    return nullptr;
}

void ReactiveInputs::Changed()
{
    if(suppress_changed_)
        return;
    if(deps_.changed)
        deps_.changed();
}

bool ReactiveInputs::SetEnabled(const std::string& id,
                                bool enabled, std::string& err)
{
    std::unique_lock<std::mutex> lock(mutex_);
    Slot* s = Find(id);
    if(s == nullptr)
    {
        err = "unknown reactive input: " + id;
        return false;
    }
    if(!s->source)
    {
        err = id + " input is not available on this system";
        return false;
    }
    if(s->enabled == enabled && (!enabled || s->source->Running()))
        return true; /* idempotent */

    if(enabled)
    {
        /* Start OUTSIDE the manager mutex — source start may
           block on device init (WASAPI, hook install). The
           slot is marked enabling so a concurrent Snapshot
           still reports correctly. */
        lock.unlock();
        const bool ok = s->source->Start(&bus_);
        lock.lock();
        if(!ok)
        {
            s->enabled = false;
            err        = s->source->Status();
            if(err.empty())
                err = id + " input failed to start";
            lock.unlock();
            Changed();
            return false;
        }
        s->enabled = true;
        s->source->SetPaused(paused_);
        /* Recorded params land on the STARTING source — the
           restore path sets them before enable, so the source
           must not come up on defaults. */
        s->source->SetSensitivity(s->sensitivity);
        s->source->SetScreenIndex(s->screen_index);
    }
    else
    {
        s->enabled = false;
        /* Stop OUTSIDE the mutex — Stop() joins the capture
           thread. After it returns nothing captures for a
           disabled input. */
        lock.unlock();
        s->source->Stop();
        lock.lock();
    }

    lock.unlock();
    Changed();
    return true;
}

bool ReactiveInputs::SetSensitivity(const std::string& id,
                                    float s, std::string& err)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Slot* slot = Find(id);
        if(slot == nullptr)
        {
            err = "unknown reactive input: " + id;
            return false;
        }
        if(id != "audio")
        {
            err = id + " input has no sensitivity parameter";
            return false;
        }
        slot->sensitivity =
            std::clamp(s, kSensitivityMin, kSensitivityMax);
        if(slot->source && slot->source->Running())
            slot->source->SetSensitivity(slot->sensitivity);
    }
    /* Persist + notify like every other externally visible
       change — the slider debounce lands here. */
    Changed();
    return true;
}

bool ReactiveInputs::SetScreenIndex(int index, std::string& err)
{
    if(index < 0)
    {
        err = "screen_index must be >= 0";
        return false;
    }
    if(deps_.enumerate_displays)
    {
        const std::vector<ReactiveDisplay> displays =
            deps_.enumerate_displays();
        if(!displays.empty() &&
           index >= static_cast<int>(displays.size()))
        {
            err = "display index out of range";
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        screen_.screen_index = index;
        if(screen_.source && screen_.source->Running())
            screen_.source->SetScreenIndex(index);
    }
    Changed();
    return true;
}

void ReactiveInputs::SetPaused(bool paused)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(paused_ == paused)
            return;
        paused_ = paused;
        for(Slot* s : { &audio_, &screen_, &key_ })
        {
            if(s->source && s->source->Running())
                s->source->SetPaused(paused);
        }
    }
    Changed();
}

bool ReactiveInputs::Paused() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return paused_;
}

std::shared_ptr<const studio::InputState>
ReactiveInputs::Snapshot()
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto state = std::make_shared<studio::InputState>();
    if(paused_)
        return state; /* display-off/suspend: NOTHING feeds
                         the scene — the gate the Task 3.7
                         atomic was waiting for */

    *state = bus_.Snapshot(kEventMaxAgeS);
    return state;
}

std::vector<ReactiveInputStatus> ReactiveInputs::Status() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ReactiveInputStatus> out;
    out.reserve(3);
    for(const std::string& id : InputIds())
    {
        const Slot* s = nullptr;
        if(id == "audio")       s = &audio_;
        else if(id == "screen") s = &screen_;
        else                    s = &key_;

        ReactiveInputStatus st;
        st.id           = id;
        st.available    = s->source != nullptr;
        st.enabled      = s->enabled;
        st.paused       = paused_;
        st.sensitivity  = s->sensitivity;
        st.screen_index = s->screen_index;
        if(s->source)
        {
            st.running = s->source->Running();
            st.status  = s->source->Status();
        }
        else
        {
            st.status = id + " input is not available on this system";
        }
        out.push_back(st);
    }
    return out;
}

std::vector<ReactiveDisplay> ReactiveInputs::Displays() const
{
    if(deps_.enumerate_displays)
        return deps_.enumerate_displays();
    return {};
}

std::vector<ReactiveInputs::PersistedInput>
ReactiveInputs::PersistedState() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PersistedInput> out;
    for(const std::string& id : InputIds())
    {
        const Slot* s = nullptr;
        if(id == "audio")       s = &audio_;
        else if(id == "screen") s = &screen_;
        else                    s = &key_;

        PersistedInput p;
        p.id           = id;
        /* Record only slots that are actually running —
           enabled-but-dead persists as disabled so a restart
           retries a once-failing source honestly instead of
           silently pinning it on. */
        p.enabled      = s->enabled && s->source &&
                         s->source->Running();
        p.sensitivity  = s->sensitivity;
        p.screen_index = s->screen_index;
        out.push_back(p);
    }
    return out;
}

void ReactiveInputs::Restore(
    const std::vector<PersistedInput>& saved)
{
    suppress_changed_ = true;
    for(const PersistedInput& p : saved)
    {
        std::string err;
        /* params before enable so a starting source gets the
           recorded values, not the defaults */
        SetSensitivity(p.id, p.sensitivity, err);
        if(p.id == "screen")
            SetScreenIndex(p.screen_index, err);
        if(p.enabled)
            SetEnabled(p.id, true, err); /* a source that
                fails simply stays disabled — Status() is
                the honest record */
    }
    suppress_changed_ = false;
}

void ReactiveInputs::Shutdown()
{
    for(Slot* s : { &audio_, &screen_, &key_ })
    {
        if(s->source)
            s->source->Stop();
    }
}

} /* namespace openrgb2 */
