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
    static void Compress(PlaybackLoop& loop)
    {
        const auto compression_start = std::chrono::steady_clock::now();
        loop.compressed = LianLiWirelessCodec::Compress(loop.rgb.data(), loop.rgb.size());
        loop.compression_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - compression_start).count());
    }

    static void Render(PlaybackLoop& loop, size_t led_count, size_t count,
                       const LoopRenderer& render)
    {
        loop.rgb = render(count);
        if(loop.rgb.size() != count * led_count * 3)
        {
            throw std::invalid_argument("rendered loop does not match its frame count and layout");
        }
    }

    PlaybackLoop BuildLoopFromRenderer(double cycle_ms, size_t led_count,
                                       const LoopRenderer& render)
    {
        if(led_count == 0 || led_count > MAX_LED_COUNT || !render
           || !std::isfinite(cycle_ms) || cycle_ms < 0.0)
        {
            throw std::invalid_argument("unsupported playback layout or cycle");
        }

        if(cycle_ms == 0.0)
        {
            // A still look: one frame the firmware holds. The interval
            // is nominal (1 s = 160000 units); one frame never advances.
            const uint64_t units = 160000;
            PlaybackLoop loop;
            loop.frame_count = 1;
            loop.timing.interval_ticks = static_cast<uint16_t>(units / 100);
            loop.timing.interval_fraction = static_cast<uint8_t>(units % 100);
            loop.effective_cycle_ms = units / 160.0;
            loop.nominal_fps = 160000.0 / units;
            Render(loop, led_count, 1, render);
            Compress(loop);
            if(loop.compressed.size() > MAX_COMPRESSED_BYTES)
            {
                throw std::runtime_error("playback loop exceeds compressed upload limit");
            }
            return loop;
        }

        size_t count = std::min<size_t>(MAX_FRAME_COUNT,
            std::max<size_t>(2, static_cast<size_t>(std::lround(cycle_ms * 0.06))));

        while(count >= 2)
        {
            // One protocol unit is 0.00625 ms (1/100 of a 0.625 ms tick).
            const uint64_t units = static_cast<uint64_t>(
                std::llround(cycle_ms * 160.0 / count));
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

            Render(loop, led_count, count, render);
            Compress(loop);
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

    static LoopRenderer PresetRenderer(const PlaybackRequest& request)
    {
        // Captures the request by value: the renderer outlives no call.
        switch(request.preset)
        {
            case PlaybackPreset::SpectrumCycle:
                return [request](size_t count) {
                    return RenderSpectrumCycle(request.led_count, count);
                };
            case PlaybackPreset::RainbowWave:
                return [request](size_t count) {
                    return RenderRainbowWave(request.led_count, count,
                        request.reverse, request.leds_per_fan, request.led_order);
                };
            case PlaybackPreset::Breathing:
                return [request](size_t count) {
                    return RenderBreathing(request.led_count, count,
                        request.red, request.green, request.blue);
                };
            case PlaybackPreset::PerimeterWave:
                return [request](size_t count) {
                    return RenderPerimeterWave(request.led_count, count,
                        request.reverse, request.leds_per_fan,
                        request.stack_pos);
                };
            case PlaybackPreset::PerimeterComet:
                return [request](size_t count) {
                    return RenderPerimeterComet(request.led_count, count,
                        request.reverse, request.leds_per_fan,
                        request.stack_pos, request.red, request.green,
                        request.blue);
                };
            case PlaybackPreset::PerimeterChase:
                return [request](size_t count) {
                    return RenderPerimeterChase(request.led_count, count,
                        request.reverse, request.leds_per_fan,
                        request.stack_pos);
                };
            case PlaybackPreset::PerimeterPingPong:
                return [request](size_t count) {
                    return RenderPerimeterPingPong(request.led_count, count,
                        request.leds_per_fan, request.stack_pos,
                        request.red, request.green, request.blue);
                };
            case PlaybackPreset::StackWave:
                return [request](size_t count) {
                    return RenderStackWave(request.led_count, count,
                        request.leds_per_fan, request.led_height);
                };
            case PlaybackPreset::StackRainbow:
                return [request](size_t count) {
                    return RenderStackRainbow(request.led_count, count,
                        request.reverse, request.leds_per_fan,
                        request.led_height);
                };
            default:
                throw std::invalid_argument("unsupported playback preset");
        }
    }

    PlaybackLoop BuildPlaybackLoop(const PlaybackRequest& request)
    {
        if(request.led_count == 0 || request.led_count > MAX_LED_COUNT
           || request.speed < 10 || request.speed > 200)
        {
            throw std::invalid_argument("unsupported playback layout or speed");
        }
        return BuildLoopFromRenderer(240000.0 / request.speed, request.led_count,
                                     PresetRenderer(request));
    }
}
