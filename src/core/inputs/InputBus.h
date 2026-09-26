/*---------------------------------------------------------*\
||| InputBus.h                                                |
|||                                                           |
|||   Thread-safe hub between input providers (audio, key,  |
|||   screen — running on their own threads) and the        |
|||   effect engine. Providers push bounded events and      |
|||   continuous signals; the bridge snapshots an           |
|||   InputState for Evaluate() each tick. Qt-free.         |
|||                                                           |
|||   Event times are stamped through a caller-supplied     |
|||   now() so event ages share the playback clock's base.  |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include "../effects/EffectTypes.h"

#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace studio
{

class InputBus
{
public:
    InputBus() = default;

    /* Clock used to stamp pushed events. Set once by the bridge. */
    void SetNow(std::function<double()> fn);

    /* Providers: stamp + enqueue an event. `code` carries the raw
       source code (e.g. a VK) for the bridge to resolve into pos.
       Bounded: the queue never exceeds the cap and stale events are
       pruned on snapshot — bursts cannot grow memory. */
    void PushEvent(const std::string& source, float strength, int code = 0);

    void SetScreenGrid(int cols, int rows, const std::vector<ColorF>& cells);
    void ClearScreenGrid();
    void SetAudioLevel(float level);

    /* Copy current state for the engine; events older than max_age
       (seconds) are dropped. */
    InputState Snapshot(double max_age) const;

    void ClearEvents();
    void ClearAll();

private:
    double Now() const;

    mutable std::mutex        mutex;
    std::function<double()>   now_fn;
    std::deque<InputEvent>    events;
    std::vector<ColorF>       cells;
    int                       cols     = 0;
    int                       rows     = 0;
    float                     level    = 0.0f;
    size_t                    cap      = 96;
};

} /* namespace studio */
