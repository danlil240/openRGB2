/*---------------------------------------------------------*\
||| InputBus.cpp                                              |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "InputBus.h"

namespace studio
{

void InputBus::SetNow(std::function<double()> fn)
{
    std::lock_guard<std::mutex> lock(mutex);
    now_fn = std::move(fn);
}

double InputBus::Now() const
{
    return now_fn ? now_fn() : 0.0;
}

/* Audit C-8: Snapshot used to filter expired events out of its
   copy but leave them queued, and nothing else ever removed them
   (ClearEvents has no runtime caller), so after the first key
   press or onset Idle() stayed false for the life of the process
   and ReactiveInputs copied a full InputState 60 times a second.
   Expired events now leave the queue. An event stamped AFTER now
   predates a clock reset (worker restart / SetEpoch): it is
   dropped too, instead of resurfacing as a fresh event once the
   new clock catches up with its old stamp. */
void InputBus::PruneLocked(double now)
{
    for(auto it = events.begin(); it != events.end();)
    {
        const double age = now - it->t;
        if(age < 0.0 || (window >= 0.0 && age > window))
        {
            it = events.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void InputBus::PushEvent(const std::string& source, float strength, int code)
{
    std::lock_guard<std::mutex> lock(mutex);
    const double now = Now();
    PruneLocked(now);
    InputEvent e;
    e.t        = now;
    e.strength = strength;
    e.source   = source;
    e.code     = code;
    events.push_back(e);
    while(events.size() > cap)
    {
        events.pop_front();
    }
}

void InputBus::SetScreenGrid(int c, int r, const std::vector<ColorF>& new_cells)
{
    std::lock_guard<std::mutex> lock(mutex);
    cols  = c;
    rows  = r;
    cells = new_cells;
}

void InputBus::ClearScreenGrid()
{
    std::lock_guard<std::mutex> lock(mutex);
    cols = 0;
    rows = 0;
    cells.clear();
}

void InputBus::SetAudioLevel(float l)
{
    std::lock_guard<std::mutex> lock(mutex);
    level = l;
}

InputState InputBus::Snapshot(double max_age)
{
    std::lock_guard<std::mutex> lock(mutex);
    InputState out;
    out.audio_level = level;
    out.screen_cols = cols;
    out.screen_rows = rows;
    out.screen_cells = cells;

    window = max_age;
    PruneLocked(Now());
    out.events.assign(events.begin(), events.end());
    return out;
}

bool InputBus::Idle()
{
    std::lock_guard<std::mutex> lock(mutex);
    if(!events.empty())
    {
        PruneLocked(Now());
    }
    return events.empty() && cells.empty() && cols == 0 && rows == 0 &&
           level == 0.0f;
}

void InputBus::ClearEvents()
{
    std::lock_guard<std::mutex> lock(mutex);
    events.clear();
}

void InputBus::ClearAll()
{
    ClearEvents();
    ClearScreenGrid();
    SetAudioLevel(0.0f);
}

} /* namespace studio */
