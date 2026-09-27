/*---------------------------------------------------------*\
||| PlaybackPresets.h                                         |
|||                                                           |
|||   The first-release Animation-playback preset table     |
|||   (plan section 3.4): exactly the three uploaded modes  |
|||   the vendored backend can render device-side —         |
|||   Spectrum Cycle, Rainbow Wave, Breathing — mapped to   |
|||   their SDK mode names and to the scene-preset names a  |
|||   scene document may carry.                             |
|||                                                           |
|||   Also the capability classification contract: only a   |
|||   scene whose effect names one of the three playback    |
|||   presets may select AnimationPlayback. Arbitrary       |
|||   layered scenes, audio reactions, screen mirroring,    |
|||   and key reactions stay Live-only — the classification |
|||   returns the short reason the UI shows.                |
|||                                                           |
|||   The shared deterministic implementation requirement   |
|||   is honored by reusing src/core/playback/PlaybackLoop   |
|||   for effective loop geometry and the pure              |
|||   LianLiWireless renderers for the matching wired       |
|||   output — the same code the backend uploads from.      |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
|\*---------------------------------------------------------*/

#pragma once

#include "EngineTypes.h"

#include "playback/PlaybackLoop.h"
#include "scene/SceneTypes.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace openrgb2
{

/*---------------------------------------------------------*\
||| PlaybackParams — the supported controls of the three    |
||| uploaded presets (plan section 5: expose only the      |
||| controls the selected preset supports).                |
|||                                                           |
|||   speed     — OpenRGB speed domain 10..200 (clamped);   |
|||               preserves the 240000/speed ms full-cycle  |
|||               duration                                  |
|||   reverse   — Rainbow Wave direction (MODE_DIRECTION_   |
|||               LEFT/RIGHT on the wire)                   |
|||   color     — Breathing mode color                      |
|\*---------------------------------------------------------*/
struct PlaybackParams
{
    int32_t  speed   = 100;
    bool     reverse = false;
    RgbColor color{ 0xFF, 0x00, 0x00 };
};

/* Clamp a requested speed into the preset domain. */
int32_t ClampPlaybackSpeed(int32_t speed);

/* Which controls a preset supports (UI/diagnostics). */
struct PlaybackPresetSupport
{
    bool speed     = true;
    bool direction = false;
    bool color     = false;
};

/*---------------------------------------------------------*\
||| Preset table                                            |
|||                                                           |
||| PlaybackPresetSdkName — the SDK UPDATEMODE name on the  |
||| Lian Li wireless RGBController (resolved by name from   |
||| enumeration, never by index).                           |
|||                                                           |
||| PlaybackPresetFromName — accepts the SDK mode name and  |
||| the snake_case scene-preset id (case/space/underscore   |
||| insensitive); false for any other name.                 |
|\*---------------------------------------------------------*/
const char*                     PlaybackPresetSdkName(
    LianLiWireless::PlaybackPreset preset);
PlaybackPresetSupport           PlaybackPresetControls(
    LianLiWireless::PlaybackPreset preset);
const std::vector<LianLiWireless::PlaybackPreset>&
                                PlaybackPresetList();
bool                            PlaybackPresetFromName(
    const std::string& name,
    LianLiWireless::PlaybackPreset& out);
const char*                     PlaybackPresetId(
    LianLiWireless::PlaybackPreset preset); /* snake_case id  */

/* True when `name` is a playback preset — EngineService uses
   this so a scene may name a playback preset that is not an
   effect-look registry entry. */
bool                            IsPlaybackPresetName(
    const std::string& name);

/*---------------------------------------------------------*\
||| ScenePlaybackCaps — per-scene capability classification.|
|||                                                           |
|||   animation_playback is true ONLY when the scene's      |
|||   effect names one of the three playback presets. Any   |
|||   other scene — arbitrary inline layers, reactive       |
|||   presets (audio/key/screen inputs), plain presets —    |
|||   is Live-only and carries a short human reason         |
|||   (first-release scoping, plan section 3.4).            |
|\*---------------------------------------------------------*/
struct ScenePlaybackCaps
{
    PlaybackCapabilities             caps;
    LianLiWireless::PlaybackPreset   preset =
        LianLiWireless::PlaybackPreset::SpectrumCycle;
    bool    has_preset = false;   /* scene names a playback preset */
    std::string reason;           /* why animation is unavailable  */
};

ScenePlaybackCaps ClassifyScenePlayback(
    const studio::SceneDocument* doc);   /* nullptr -> no scene */

/*---------------------------------------------------------*\
||| Effective loop geometry + wired sampling.               |
|||                                                           |
||| EffectiveLoopGeometry builds the loop the backend would |
||| upload for the wireless group through the shared        |
||| PlaybackLoop builder — the engine therefore knows the   |
||| effective frame count, effective frame interval, and    |
||| effective cycle duration WITHOUT a diagnostics channel  |
||| (same deterministic inputs -> same result; the builder  |
||| throws std::invalid_argument/std::runtime_error on      |
||| unsupported inputs, reported as the request reason).    |
|||                                                           |
||| PlaybackFrameSampler renders each preset's canonical    |
||| frames once per (preset, params, led_count) and serves  |
||| a complete-controller color buffer for a loop position  |
||| (phase 0..1). It feeds the matching-effect output on    |
||| wired devices and the pause "sampled hold" frame.       |
|\*---------------------------------------------------------*/
struct PlaybackLoopGeometry
{
    uint32_t frame_count        = 0;
    double   frame_interval_s   = 0.0;
    double   cycle_s            = 0.0;
};

/* Compute the effective geometry for `led_count` LEDs.
   Throws on unsupported input (same contract as the shared
   builder — the caller turns the message into the request
   reason). */
PlaybackLoopGeometry EffectiveLoopGeometry(
    LianLiWireless::PlaybackPreset preset,
    const PlaybackParams& params,
    uint32_t led_count);

class PlaybackFrameSampler
{
public:
    /* Colors for `led_count` LEDs at loop position `phase`
       (wrapped to [0,1)). Deterministic; identical inputs
       reproduce the uploaded frame content. */
    std::vector<RgbColor> Sample(
        LianLiWireless::PlaybackPreset preset,
        const PlaybackParams& params,
        uint32_t led_count,
        uint32_t frame_count,
        double phase);

    /* Cache statistics for diagnostics/tests. */
    size_t CachedLoops() const;

private:
    struct Key
    {
        int      preset;
        int32_t  speed_unused = 0;  /* speed affects timing only  */
        bool     reverse;
        uint32_t led_count;
        uint32_t frame_count;
        uint8_t  r, g, b;           /* breathing color            */
        bool operator<(const Key& o) const;
    };
    /* One rendered loop: frame_count * led_count * 3 bytes. */
    std::map<Key, std::shared_ptr<const std::vector<uint8_t>>> loops_;
};

} /* namespace openrgb2 */
