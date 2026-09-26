/*---------------------------------------------------------*\
|| EngineTypes.h                                             |
||                                                           |
||   Engine/SDK boundary types from plan section 4:         |
||   DeviceKey, DeviceSnapshot, RgbFrame, DeliveryStats,    |
||   PlaybackMode, PlaybackStatus, and the                  |
||   submitLatest(RgbFrame) -> SubmitResult contract.       |
||                                                           |
||   Qt-free, hardware-independent value types. The SDK     |
||   adapter and device mapping land in later M3 tasks;     |
||   these types are their shared vocabulary.               |
||                                                           |
||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include "scene/SceneTypes.h" /* studio::SceneColor */

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace openrgb2
{

using Clock     = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

/*---------------------------------------------------------*\
|| RgbColor — canonical 8-bit-per-channel RGB at the        |
|| engine boundary. Scene colors convert at the edge; the   |
|| SDK adapter converts to its wire format.                 |
\*---------------------------------------------------------*/
struct RgbColor
{
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

bool     operator==(const RgbColor& a, const RgbColor& b);
bool     operator!=(const RgbColor& a, const RgbColor& b);
RgbColor FromSceneColor(studio::SceneColor c); /* 0x00BBGGRR unpack */

/*---------------------------------------------------------*\
|| DeviceKey — stable identity plus serial/location         |
|| disambiguation and zone identity (plan section 4).       |
|| NEVER a controller-list index: identities re-resolve     |
|| after reconnect, so reordered controllers cannot         |
|| misroute colors.                                         |
||                                                          |
||   device   — stable controller identity (vendor/model/   |
||              name derived by the SDK adapter)            |
||   serial   — serial-number disambiguation between        |
||              identical units; "" = not reported          |
||   location — physical location (USB path/port);          |
||              "" = not reported                           |
||   zone     — zone identity within the controller;        |
||              "" = the whole controller (controller key)  |
\*---------------------------------------------------------*/
struct DeviceKey
{
    std::string device;
    std::string serial;
    std::string location;
    std::string zone;
};

bool operator==(const DeviceKey& a, const DeviceKey& b);
bool operator!=(const DeviceKey& a, const DeviceKey& b);
bool operator< (const DeviceKey& a, const DeviceKey& b); /* map ordering */

/*---------------------------------------------------------*\
|| DeviceSnapshot — one enumerated controller: topology     |
|| generation, LED counts, modes, and connection state.     |
||                                                          |
||   topology_generation — bumped on every enumeration/     |
||     topology change; frames stamped with a stale         |
||     generation are obsolete and must be rejected         |
||   zones    — zone identities + LED counts (SDK order);   |
||     a zone's offset in the complete-controller buffer    |
||     is the sum of the preceding zones' counts            |
||   modes    — supported mode names from enumeration,      |
||     never hard-coded array indices                       |
||   name/vendor/device_type — raw enumeration fields       |
||     (key.device is the "vendor:name" composite); they    |
||     feed DeviceBinding resolution, which matches on      |
||     name/vendor/serial/location, never on indices        |
||   colors   — the controller's last-ENUMERATED per-LED    |
||     colors (RGB order, LedCount() entries). The device   |
||     mapper seeds complete-controller frames from them    |
||     so unmapped LEDs keep their current colors (plan     |
||     section 4). Empty when the backend reports none.     |
\*---------------------------------------------------------*/
enum class ConnectionState
{
    Disconnected,
    Reconnecting,
    Connected,
};

struct ZoneSnapshot
{
    std::string name;
    uint32_t    led_count = 0;
};

struct DeviceSnapshot
{
    DeviceKey                 key;
    uint64_t                  topology_generation = 0;
    std::string               name;
    std::string               vendor;
    int32_t                   device_type = -1;
    std::vector<ZoneSnapshot> zones;
    std::vector<std::string>  modes;
    int32_t                   active_mode = -1;
    ConnectionState           connection  = ConnectionState::Disconnected;
    std::vector<RgbColor>     colors;

    uint32_t LedCount() const; /* sum of zone LED counts */
};

/*---------------------------------------------------------*\
|| RgbFrame — one complete-controller color update.         |
||                                                          |
||   controller — controller-level DeviceKey (zone == "")   |
||   topology_generation — must match the current           |
||     DeviceSnapshot generation or the frame is obsolete   |
||   sequence — producer-assigned, monotonic per controller |
||   sample_time — monotonic clock sample of the scene      |
||     time the colors were evaluated at                    |
||   colors — canonical RGB for every controller LED in     |
||     SDK zone order (the wireless group's three zone      |
||     edits assemble into ONE complete-controller frame)   |
\*---------------------------------------------------------*/
struct RgbFrame
{
    DeviceKey            controller;
    uint64_t             topology_generation = 0;
    uint64_t             sequence            = 0;
    TimePoint            sample_time;
    std::vector<RgbColor> colors;
};

/*---------------------------------------------------------*\
|| SubmitResult — submitLatest(RgbFrame) outcome.           |
||                                                          |
||   Accepted        — stored as the pending frame          |
||   ReplacedPending — replaced a not-yet-delivered pending |
||                     frame (latest-wins backpressure)     |
||   Rejected        — refused; reason says why             |
||                                                          |
|| There is deliberately no "displayed" result: SDK         |
|| transmission success is not hardware confirmation        |
|| (plan section 4).                                        |
\*---------------------------------------------------------*/
enum class SubmitStatus
{
    Accepted,
    ReplacedPending,
    Rejected,
};

struct SubmitResult
{
    SubmitStatus status = SubmitStatus::Rejected;
    std::string  reason;               /* set for Rejected */

    static SubmitResult Accept();
    static SubmitResult Replace();
    static SubmitResult Reject(std::string why);
};

/*---------------------------------------------------------*\
|| MeasurementProvenance — where a reported number came     |
|| from. Unknown measurements are reported as unknown; a    |
|| configured rate is never passed off as a measured rate   |
|| (plan sections 5 and 6).                                 |
\*---------------------------------------------------------*/
enum class MeasurementProvenance
{
    Unknown,
    Configured,         /* requested value, not measured                */
    TransportMeasured,  /* measured at the SDK/transport boundary       */
    PhysicallyMeasured, /* measured at the device (counters/benchmarks) */
};

/*---------------------------------------------------------*\
|| DeliveryStats — submitLatest/delivery accounting for     |
|| one controller: submitted/replaced/transport-completed   |
|| counts, completion intervals, and measurement            |
|| provenance.                                              |
||                                                          |
||   submitted           — non-rejected SubmitLatest calls  |
||   replaced_pending    — submissions that replaced a      |
||                         pending frame                    |
||   transport_completed — frames accepted by the           |
||                         transport drain (never           |
||                         "displayed")                     |
||   suppressed          — drained frames deliberately not  |
||                         retransmitted (identical to the  |
||                         last delivered frame, or stale   |
||                         versus already-delivered state)  |
||   transport_rejected  — drained frames the downstream    |
||                         sink refused (not connected,     |
||                         send failure, validation)        |
||   intervals           — between consecutive transport    |
||                         completions (seconds)            |
\*---------------------------------------------------------*/
struct DeliveryStats
{
    uint64_t submitted           = 0;
    uint64_t replaced_pending    = 0;
    uint64_t rejected            = 0;
    uint64_t transport_completed = 0;
    uint64_t suppressed          = 0;
    uint64_t transport_rejected  = 0;

    double   last_interval_s = 0.0;
    double   min_interval_s  = 0.0;
    double   max_interval_s  = 0.0;

    MeasurementProvenance provenance = MeasurementProvenance::Unknown;

    /* Records a completed handoff at `at`; intervals derive from
       consecutive completions. */
    void   RecordTransportCompletion(TimePoint at);
    double MeanIntervalS() const; /* 0 when fewer than two completions */
    uint64_t IntervalCount() const;

private:
    TimePoint last_completion_{};
    uint64_t  intervals_    = 0;
    double    interval_sum_ = 0.0;
};

/*---------------------------------------------------------*\
|| PlaybackMode — LiveEffects or AnimationPlayback.         |
|| Persisted per scene (string form below) and validated    |
|| against preset capabilities before selection.            |
\*---------------------------------------------------------*/
enum class PlaybackMode
{
    LiveEffects,
    AnimationPlayback,
};

const char*   PlaybackModeName(PlaybackMode mode);   /* stable tokens  */
bool          PlaybackModeFromName(const std::string& name,
                                   PlaybackMode& out); /* false = bad  */

struct PlaybackCapabilities
{
    bool live_effects        = true;
    bool animation_playback  = false;
};

/* True when the preset/device capabilities support `mode`. */
bool PlaybackModeSupported(PlaybackMode mode,
                           const PlaybackCapabilities& caps);

/*---------------------------------------------------------*\
|| PlaybackStatus — reported playback state. SDK            |
|| transmission success is NOT hardware confirmation: the   |
|| status stays pending until the backend reports the       |
|| applied mode/parameter revision. Physical playback       |
|| measurements are recorded SEPARATELY from live delivery  |
|| stats — never substitute one for the other.              |
\*---------------------------------------------------------*/
enum class UploadState
{
    None,
    Uploading,
    Active,
    Failed,
};

enum class SyncHealth
{
    Unknown,
    Healthy,
    Degraded,
    Stale,
};

/* Separately recorded physical playback measurement. */
struct PlaybackMeasurement
{
    double                frame_interval_s = 0.0;
    double                cycle_s          = 0.0; /* full loop duration */
    MeasurementProvenance provenance       = MeasurementProvenance::Unknown;
};

struct PlaybackStatus
{
    PlaybackMode  mode               = PlaybackMode::LiveEffects;
    uint64_t      preset_revision    = 0; /* selected preset identity   */
    uint64_t      parameter_revision = 0; /* applied parameter revision */
    UploadState   upload             = UploadState::None;

    /* Effective on-device playback geometry after qualification. */
    uint32_t      effective_frame_count      = 0;
    double        effective_frame_interval_s = 0.0;

    /* Validated playback clock sample and its uncertainty — drives
       the matching wired-effect timeline (plan section 4). */
    double        clock_sample_s      = 0.0;
    double        clock_uncertainty_s = 0.0;
    SyncHealth    sync                = SyncHealth::Unknown;

    /* Measured at the device — recorded separately, never fused
       into the configured/effective fields above. */
    PlaybackMeasurement physical;
};

} /* namespace openrgb2 */
