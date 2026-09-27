/*---------------------------------------------------------*\
|| Engine.h                                                  |
||                                                           |
||   The independent lighting engine composition. Owns the  |
||   scene-evaluation worker and the submitLatest frame     |
||   sink; holds the reported PlaybackStatus.               |
||                                                           |
||   Deliberately small: SDK client, device mapping, IPC,   |
||   power handling, and profiles are later Milestone-3     |
||   tasks that plug into these seams.                      |
||                                                           |
||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include "EngineTypes.h"
#include "FrameSink.h"
#include "SceneWorker.h"

#include <memory>
#include <mutex>

namespace openrgb2
{

class Engine
{
public:
    Engine();                            /* sink = NullFrameSink */
    ~Engine();                           /* Stop()               */

    Engine(const Engine&)            = delete;
    Engine& operator=(const Engine&) = delete;

    SceneWorker& Worker();

    /* Frame sink plumbing. SetFrameSink(nullptr) restores the
       NullFrameSink. SubmitLatest forwards to the current sink —
       the device-mapping task produces the frames. */
    void                        SetFrameSink(std::shared_ptr<IFrameSink> sink);
    std::shared_ptr<IFrameSink> Sink() const;
    SubmitResult                SubmitLatest(const RgbFrame& frame);

    /* Reported playback state (plan section 4). Defaults to
       LiveEffects/none; later tasks update it as uploads and
       mode acknowledgements land. */
    PlaybackStatus Status() const;
    void           SetPlaybackStatus(const PlaybackStatus& status);

    /* Starts/stops the scene-evaluation worker. The engine keeps
       running with no UI attached — the UI is a separate process
       (plan section 4, process ownership). */
    bool Start();
    void Stop();
    bool Running() const;

private:
    SceneWorker                 worker_;
    mutable std::mutex          mutex_;
    std::shared_ptr<IFrameSink> sink_;
    PlaybackStatus              status_;
};

} /* namespace openrgb2 */
