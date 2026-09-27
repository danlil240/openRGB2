/*---------------------------------------------------------*\
||| PlaybackPresets.cpp                                       |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
|\*---------------------------------------------------------*/

#include "PlaybackPresets.h"

#include "LianLiWirelessEffects.h"
#include "effects/Presets.h"    /* studio::FindPreset — reactive needs */

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <tuple>

namespace openrgb2
{

namespace
{

constexpr int32_t kSpeedMin = 10;    /* LLW mode speed domain  */
constexpr int32_t kSpeedMax = 200;

struct PresetRow
{
    LianLiWireless::PlaybackPreset preset;
    const char*                    sdk_name;
    const char*                    id;
    PlaybackPresetSupport          controls;
};

const PresetRow kPresets[] =
{
    { LianLiWireless::PlaybackPreset::SpectrumCycle,
      "Spectrum Cycle", "spectrum_cycle",
      PlaybackPresetSupport{ true, false, false } },
    { LianLiWireless::PlaybackPreset::RainbowWave,
      "Rainbow Wave", "rainbow_wave",
      PlaybackPresetSupport{ true, true, false } },
    { LianLiWireless::PlaybackPreset::Breathing,
      "Breathing", "breathing",
      PlaybackPresetSupport{ true, false, true } },
};

/* Case-insensitive name match that ignores spaces,
   underscores, and dashes — "Rainbow Wave", "rainbow_wave",
   and "rainbow-wave" are the same selection. */
std::string Normalize(const std::string& name)
{
    std::string out;
    out.reserve(name.size());
    for(const unsigned char ch : name)
    {
        if(ch == ' ' || ch == '_' || ch == '-')
        {
            continue;
        }
        out.push_back((char)std::tolower(ch));
    }
    return out;
}

} /* anonymous namespace */

int32_t ClampPlaybackSpeed(int32_t speed)
{
    return std::max(kSpeedMin, std::min(kSpeedMax, speed));
}

const char* PlaybackPresetSdkName(LianLiWireless::PlaybackPreset preset)
{
    for(const PresetRow& row : kPresets)
    {
        if(row.preset == preset)
        {
            return row.sdk_name;
        }
    }
    return "";
}

PlaybackPresetSupport PlaybackPresetControls(
    LianLiWireless::PlaybackPreset preset)
{
    for(const PresetRow& row : kPresets)
    {
        if(row.preset == preset)
        {
            return row.controls;
        }
    }
    return {};
}

const std::vector<LianLiWireless::PlaybackPreset>& PlaybackPresetList()
{
    static const std::vector<LianLiWireless::PlaybackPreset> list = {
        LianLiWireless::PlaybackPreset::SpectrumCycle,
        LianLiWireless::PlaybackPreset::RainbowWave,
        LianLiWireless::PlaybackPreset::Breathing,
    };
    return list;
}

const char* PlaybackPresetId(LianLiWireless::PlaybackPreset preset)
{
    for(const PresetRow& row : kPresets)
    {
        if(row.preset == preset)
        {
            return row.id;
        }
    }
    return "";
}

bool PlaybackPresetFromName(const std::string& name,
                            LianLiWireless::PlaybackPreset& out)
{
    const std::string want = Normalize(name);
    for(const PresetRow& row : kPresets)
    {
        if(want == Normalize(row.sdk_name) || want == Normalize(row.id))
        {
            out = row.preset;
            return true;
        }
    }
    return false;
}

bool IsPlaybackPresetName(const std::string& name)
{
    LianLiWireless::PlaybackPreset p;
    return PlaybackPresetFromName(name, p);
}

/*---------------------------------------------------------*\
||| Scene capability classification                         |
|\*---------------------------------------------------------*/

ScenePlaybackCaps ClassifyScenePlayback(const studio::SceneDocument* doc)
{
    ScenePlaybackCaps out;
    out.caps.live_effects       = true;
    out.caps.animation_playback = false;

    if(!doc)
    {
        out.reason = "no scene is loaded";
        return out;
    }
    const std::string& preset = doc->effect.preset;
    if(preset.empty())
    {
        /* Arbitrary inline-layer scenes (and empty scenes) are
           live-only — they cannot be an uploaded loop. */
        out.reason = doc->effect.layers.empty()
            ? "scene has no playback-capable preset"
            : "arbitrary layered scenes stay live effects";
        return out;
    }
    LianLiWireless::PlaybackPreset p;
    if(PlaybackPresetFromName(preset, p))
    {
        out.caps.animation_playback = true;
        out.preset     = p;
        out.has_preset = true;
        return out;
    }
    /* A named preset that is not one of the three uploaded
       presets: reactive looks get the honest reason first. */
    const studio::PresetInfo* info = studio::FindPreset(preset);
    if(info && !info->needs.empty())
    {
        out.reason = "'" + preset + "' reacts to " + info->needs +
                     " input — reactive scenes stay live effects";
    }
    else
    {
        out.reason = "preset '" + preset +
                     "' is not one of the three uploaded playback "
                     "presets (Spectrum Cycle, Rainbow Wave, "
                     "Breathing)";
    }
    return out;
}

/*---------------------------------------------------------*\
||| Effective loop geometry                                 |
|\*---------------------------------------------------------*/

PlaybackLoopGeometry EffectiveLoopGeometry(
    LianLiWireless::PlaybackPreset preset,
    const PlaybackParams& params,
    uint32_t led_count)
{
    /* The shared deterministic builder: identical inputs give
       the same frame count, quantization, and duration the
       backend uploads — including the duration-preserving
       frame reduction when a loop exceeds the compressed
       upload limit. */
    LianLiWireless::PlaybackRequest req;
    req.preset       = preset;
    req.led_count    = led_count;
    req.speed        = (unsigned)ClampPlaybackSpeed(params.speed);
    req.reverse      = params.reverse;
    req.leds_per_fan = 0;          /* wired canonical order     */
    req.red          = params.color.r;
    req.green        = params.color.g;
    req.blue         = params.color.b;
    const LianLiWireless::PlaybackLoop loop =
        LianLiWireless::BuildPlaybackLoop(req);

    PlaybackLoopGeometry g;
    g.frame_count      = loop.frame_count;
    g.cycle_s          = loop.effective_cycle_ms / 1000.0;
    g.frame_interval_s = g.cycle_s / loop.frame_count;
    return g;
}

/*---------------------------------------------------------*\
||| PlaybackFrameSampler                                    |
|\*---------------------------------------------------------*/

bool PlaybackFrameSampler::Key::operator<(const Key& o) const
{
    return std::tie(preset, reverse, led_count, frame_count, r, g, b) <
           std::tie(o.preset, o.reverse, o.led_count, o.frame_count,
                    o.r, o.g, o.b);
}

size_t PlaybackFrameSampler::CachedLoops() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return loops_.size();
}

std::vector<RgbColor> PlaybackFrameSampler::Sample(
    LianLiWireless::PlaybackPreset preset,
    const PlaybackParams& params,
    uint32_t led_count,
    uint32_t frame_count,
    double phase)
{
    std::vector<RgbColor> empty;
    if(led_count == 0 || frame_count == 0)
    {
        return empty;
    }

    const Key key{ (int)preset, 0, params.reverse, led_count,
                   frame_count, params.color.r, params.color.g,
                   params.color.b };
    std::shared_ptr<const std::vector<uint8_t>> loop;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = loops_.find(key);
        if(it == loops_.end())
        {
            std::vector<uint8_t> rgb;
            switch(preset)
            {
            case LianLiWireless::PlaybackPreset::SpectrumCycle:
                rgb = LianLiWireless::RenderSpectrumCycle(
                    led_count, frame_count);
                break;
            case LianLiWireless::PlaybackPreset::RainbowWave:
                /* Canonical linear layout for wired endpoints —
                   the per-fan order only applies to the wireless
                   group's own upload. */
                rgb = LianLiWireless::RenderRainbowWave(
                    led_count, frame_count, params.reverse);
                break;
            case LianLiWireless::PlaybackPreset::Breathing:
                rgb = LianLiWireless::RenderBreathing(
                    led_count, frame_count, params.color.r,
                    params.color.g, params.color.b);
                break;
            }
            it = loops_.emplace(key,
                std::make_shared<const std::vector<uint8_t>>(
                    std::move(rgb))).first;
            lru_.push_back(key);
            /* Bounded LRU: arbitrary playback colors must not
               grow engine memory without bound. */
            while(lru_.size() > kMaxLoops)
            {
                loops_.erase(lru_.front());
                lru_.pop_front();
            }
        }
        else
        {
            /* Most-recent touch. */
            for(auto li = lru_.begin(); li != lru_.end(); ++li)
            {
                if(!(*li < key) && !(key < *li))
                {
                    lru_.erase(li);
                    break;
                }
            }
            lru_.push_back(key);
        }
        loop = it->second;
    }

    const std::vector<uint8_t>& rgb = *loop;
    double p = phase - std::floor(phase);
    if(p < 0.0)
    {
        p += 1.0;
    }
    const size_t frame =
        std::min<size_t>((size_t)(p * (double)frame_count),
                         (size_t)frame_count - 1);
    const size_t base = frame * (size_t)led_count * 3;

    std::vector<RgbColor> colors(led_count);
    for(uint32_t i = 0; i < led_count; ++i)
    {
        colors[i] = RgbColor{ rgb[base + i * 3 + 0],
                              rgb[base + i * 3 + 1],
                              rgb[base + i * 3 + 2] };
    }
    return colors;
}

} /* namespace openrgb2 */
