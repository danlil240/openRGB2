/*---------------------------------------------------------*\
|| Engine.cpp                                                |
||                                                           |
||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "Engine.h"

#include <utility>

namespace openrgb2
{

Engine::Engine()
    : sink_(std::make_shared<NullFrameSink>())
{
}

Engine::~Engine()
{
    Stop();
}

SceneWorker& Engine::Worker()
{
    return worker_;
}

void Engine::SetFrameSink(std::shared_ptr<IFrameSink> sink)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sink_ = sink ? std::move(sink) : std::make_shared<NullFrameSink>();
}

std::shared_ptr<IFrameSink> Engine::Sink() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return sink_;
}

SubmitResult Engine::SubmitLatest(const RgbFrame& frame)
{
    std::shared_ptr<IFrameSink> sink;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sink = sink_;
    }
    return sink->SubmitLatest(frame);
}

PlaybackStatus Engine::Status() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void Engine::SetPlaybackStatus(const PlaybackStatus& status)
{
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = status;
}

bool Engine::Start()
{
    return worker_.Start();
}

void Engine::Stop()
{
    worker_.Stop();
}

bool Engine::Running() const
{
    return worker_.Running();
}

} /* namespace openrgb2 */
