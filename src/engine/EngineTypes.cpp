/*---------------------------------------------------------*\
|| EngineTypes.cpp                                           |
||                                                           |
||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "EngineTypes.h"

#include <tuple>
#include <utility>

namespace openrgb2
{

bool operator==(const RgbColor& a, const RgbColor& b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

bool operator!=(const RgbColor& a, const RgbColor& b)
{
    return !(a == b);
}

RgbColor FromSceneColor(studio::SceneColor c)
{
    /* Scene colors are packed 0x00BBGGRR (SceneTypes.h). */
    RgbColor out;
    out.r = uint8_t(c & 0xFF);
    out.g = uint8_t((c >> 8) & 0xFF);
    out.b = uint8_t((c >> 16) & 0xFF);
    return out;
}

bool operator==(const DeviceKey& a, const DeviceKey& b)
{
    return a.device == b.device && a.serial == b.serial &&
           a.location == b.location && a.zone == b.zone;
}

bool operator!=(const DeviceKey& a, const DeviceKey& b)
{
    return !(a == b);
}

bool operator<(const DeviceKey& a, const DeviceKey& b)
{
    return std::tie(a.device, a.serial, a.location, a.zone) <
           std::tie(b.device, b.serial, b.location, b.zone);
}

uint32_t DeviceSnapshot::LedCount() const
{
    uint32_t total = 0;
    for(const ZoneSnapshot& z : zones)
    {
        total += z.led_count;
    }
    return total;
}

SubmitResult SubmitResult::Accept()
{
    return SubmitResult{ SubmitStatus::Accepted, {} };
}

SubmitResult SubmitResult::Replace()
{
    return SubmitResult{ SubmitStatus::ReplacedPending, {} };
}

SubmitResult SubmitResult::Reject(std::string why)
{
    return SubmitResult{ SubmitStatus::Rejected, std::move(why) };
}

void DeliveryStats::RecordTransportCompletion(TimePoint at)
{
    ++transport_completed;
    if(transport_completed > 1)
    {
        const double interval =
            std::chrono::duration<double>(at - last_completion_).count();
        last_interval_s = interval;
        if(intervals_ == 0 || interval < min_interval_s)
        {
            min_interval_s = interval;
        }
        if(interval > max_interval_s)
        {
            max_interval_s = interval;
        }
        interval_sum_ += interval;
        ++intervals_;
    }
    last_completion_ = at;
}

double DeliveryStats::MeanIntervalS() const
{
    return intervals_ ? interval_sum_ / double(intervals_) : 0.0;
}

uint64_t DeliveryStats::IntervalCount() const
{
    return intervals_;
}

const char* PlaybackModeName(PlaybackMode mode)
{
    switch(mode)
    {
    case PlaybackMode::LiveEffects:       return "live_effects";
    case PlaybackMode::AnimationPlayback: return "animation_playback";
    }
    return "live_effects";
}

bool PlaybackModeFromName(const std::string& name, PlaybackMode& out)
{
    if(name == "live_effects")
    {
        out = PlaybackMode::LiveEffects;
        return true;
    }
    if(name == "animation_playback")
    {
        out = PlaybackMode::AnimationPlayback;
        return true;
    }
    return false;
}

bool PlaybackModeSupported(PlaybackMode mode, const PlaybackCapabilities& caps)
{
    switch(mode)
    {
    case PlaybackMode::LiveEffects:       return caps.live_effects;
    case PlaybackMode::AnimationPlayback: return caps.animation_playback;
    }
    return false;
}

} /* namespace openrgb2 */
