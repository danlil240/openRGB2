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

void InputBus::PushEvent(const std::string& source, float strength, int code)
{
    std::lock_guard<std::mutex> lock(mutex);
    InputEvent e;
    e.t        = Now();
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

InputState InputBus::Snapshot(double max_age) const
{
    std::lock_guard<std::mutex> lock(mutex);
    InputState out;
    out.audio_level = level;
    out.screen_cols = cols;
    out.screen_rows = rows;
    out.screen_cells = cells;

    const double now = Now();
    for(const InputEvent& e : events)
    {
        const double age = now - e.t;
        if(age >= 0.0 && age <= max_age)
        {
            out.events.push_back(e);
        }
    }
    return out;
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
