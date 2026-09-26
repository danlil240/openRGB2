// SPDX-License-Identifier: GPL-2.0-or-later
#include "PlaybackLoop.h"
#include "LianLiWirelessCodec.h"
#include "LianLiWirelessEffects.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

namespace LianLiWireless
{
    PlaybackLoop BuildPlaybackLoop(const PlaybackRequest& request)
    {
        if(request.led_count == 0 || request.led_count > MAX_LED_COUNT
           || request.speed < 10 || request.speed > 200)
        {
            throw std::invalid_argument("unsupported playback layout or speed");
        }

        const double requested_cycle_ms = 240000.0 / request.speed;
        size_t count = std::min<size_t>(MAX_FRAME_COUNT,
            std::max<size_t>(2, static_cast<size_t>(std::lround(requested_cycle_ms * 0.06))));

        while(count >= 2)
        {
            // One protocol unit is 0.00625 ms (1/100 of a 0.625 ms tick).
            const uint64_t units = static_cast<uint64_t>(
                std::llround(requested_cycle_ms * 160.0 / count));
            if(units < 100 || units > 6553599)
            {
                throw std::invalid_argument("playback interval outside protocol range");
            }

            PlaybackLoop loop;
            loop.frame_count = static_cast<uint16_t>(count);
            loop.timing.interval_ticks = static_cast<uint16_t>(units / 100);
            loop.timing.interval_fraction = static_cast<uint8_t>(units % 100);
            loop.effective_cycle_ms = count * units / 160.0;
            loop.nominal_fps = 160000.0 / units;

            switch(request.preset)
            {
                case PlaybackPreset::SpectrumCycle:
                    loop.rgb = RenderSpectrumCycle(request.led_count, count);
                    break;
                case PlaybackPreset::RainbowWave:
                    loop.rgb = RenderRainbowWave(request.led_count, count,
                        request.reverse, request.leds_per_fan, request.led_order);
                    break;
                case PlaybackPreset::Breathing:
                    loop.rgb = RenderBreathing(request.led_count, count,
                        request.red, request.green, request.blue);
                    break;
                default:
                    throw std::invalid_argument("unsupported playback preset");
            }
            const auto compression_start = std::chrono::steady_clock::now();
            loop.compressed = LianLiWirelessCodec::Compress(loop.rgb.data(), loop.rgb.size());
            loop.compression_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - compression_start).count());
            if(loop.compressed.size() <= MAX_COMPRESSED_BYTES)
            {
                return loop;
            }
            // Large loops can be far beyond the upload limit. Reduce by
            // the measured ratio with a little margin; each retry uses the
            // real compressed size, and the cycle duration stays fixed.
            const size_t estimate = std::max<size_t>(2, static_cast<size_t>(
                count * (double(MAX_COMPRESSED_BYTES) / loop.compressed.size()) * 0.95));
            count = std::min(count - 1, estimate);
        }
        throw std::runtime_error("playback loop exceeds compressed upload limit");
    }
}
