/*---------------------------------------------------------*\
| LogitechPerKey8080Plan.h                                  |
|                                                           |
|   Send planner for HID++ 2.0 feature 0x8080 (Per Key      |
|   Lighting: G512, G410, G810 family). Decides which keys  |
|   go on the wire and how they are grouped and chunked     |
|   into fn3 SetKeyColors frames. Qt-free and hidapi-free   |
|   so it builds into an offline test                       |
|   (tests/backend/logitech_8080_plan_test.cpp).            |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "RGBControllerInterface.h"

/*---------------------------------------------------------*\
| 0x8080 carries up to 14 (keyId,R,G,B) tuples in one       |
| very-long (report 0x12) frame, and GetKeyColors           |
| paginates its readback by the same count. The payload     |
| after the 4-byte HID++ header is keyType u16 BE, count    |
| u16 BE, then the tuples: 4 + 14 * 4 = 60 bytes, the       |
| whole very-long payload.                                  |
\*---------------------------------------------------------*/
#define HIDPP20_8080_KEYS_PER_FRAME         14
#define LOGITECH_8080_FRAME_PAYLOAD_LEN     (4 + HIDPP20_8080_KEYS_PER_FRAME * 4)

/*---------------------------------------------------------*\
| Delta sends repaint only the keys whose wire color        |
| changed. A full repaint is still forced this often, the   |
| safety net against a competing writer (G Hub, Dynamic     |
| Lighting) and against a frame the keyboard dropped        |
| silently. 2 s is a placeholder (plan P-7: the period      |
| trades bus load against how fast a stale key recovers);   |
| 0 disables the timed refresh.                             |
\*---------------------------------------------------------*/
#define HIDPP20_8080_FULL_REFRESH_MS        2000

/*---------------------------------------------------------*\
| One key in a planned frame. led is the OpenRGB LED index  |
| the key was taken from, or -1 when the key did not come   |
| from an LED (the Off path paints every enumerated key).   |
\*---------------------------------------------------------*/
struct LogitechPerKey8080Key
{
    uint8_t     key_id;
    RGBColor    color;
    int         led;
};

/*---------------------------------------------------------*\
| One fn3 SetKeyColors frame: a single keyType and at most  |
| HIDPP20_8080_KEYS_PER_FRAME keys.                         |
\*---------------------------------------------------------*/
struct LogitechPerKey8080Frame
{
    uint16_t                            key_type;
    std::vector<LogitechPerKey8080Key>  keys;
};

typedef std::vector<std::pair<uint16_t, std::vector<std::pair<uint8_t, RGBColor>>>> LogitechPerKey8080Groups;

/*---------------------------------------------------------*\
| Link summary for the 0x8080 send path, logged once per    |
| HIDPP20_8080_SUMMARY_S by the controller: what one        |
| hardware run needs to read the achieved update rate.      |
| fn3 ACKs are not read yet (plan P-7 step 4), so there     |
| is no matched/lost ACK count.                             |
\*---------------------------------------------------------*/
#define HIDPP20_8080_SUMMARY_S              10

struct LogitechPerKey8080Stats
{
    size_t      updates         = 0;    /* DeviceUpdateLEDs calls on the 0x8080 path    */
    size_t      full            = 0;    /* of which full repaints                       */
    size_t      idle            = 0;    /* of which sent nothing (no key changed)       */
    size_t      frames          = 0;    /* fn3 SetKeyColors frames planned              */
    size_t      keys            = 0;    /* keys carried by those frames                 */
    size_t      write_failures  = 0;    /* frames whose hid_write failed                */
    size_t      commit_failures = 0;    /* FlushLEDS sent but not acked                 */
    uint64_t    send_us_total   = 0;    /* wall time of the updates that sent           */
    uint64_t    send_us_max     = 0;

    /*-----------------------------------------------------*\
    | Account one update. sent: frames and FlushLEDS went   |
    | out (false for an idle update); written and           |
    | committed as returned by SetPerKey8080 and            |
    | PerKeyCommit8080; send_us: wall time of the update.   |
    \*-----------------------------------------------------*/
    void        Add(bool is_full, const std::vector<LogitechPerKey8080Frame>& planned,
                    const std::vector<bool>& written, bool sent, bool committed, uint64_t send_us);
    void        Reset();

    /*-----------------------------------------------------*\
    | One-line body, e.g. "600 updates (60.0/s), 20 full,   |
    | 4 idle, 1500 frames (6.1 keys/frame), 0 write failed, |
    | 0 FlushLEDS unacked, send 9.8 ms mean, 21.0 ms max".  |
    | window_s is the time the counters cover.              |
    \*-----------------------------------------------------*/
    std::string Summary(double window_s) const;
};

namespace LogitechPerKey8080Plan
{
    /*-----------------------------------------------------*\
    | Dim scaling applied to every key: each channel        |
    | times brightness / 100, truncated, when brightness    |
    | is below 100; untouched otherwise.                    |
    \*-----------------------------------------------------*/
    RGBColor ScaleBrightness(RGBColor color, int brightness);

    /*-----------------------------------------------------*\
    | Groups -> frames. Applies the G410 quirk              |
    | (kt40_id12_only: keyType 0x40 accepts only keyIds 1   |
    | and 2) and splits each group into frames of up to     |
    | HIDPP20_8080_KEYS_PER_FRAME keys. Keys get led = -1.  |
    \*-----------------------------------------------------*/
    std::vector<LogitechPerKey8080Frame> ChunkGroups
        (
        const LogitechPerKey8080Groups&                     by_type,
        bool                                                kt40_id12_only
        );

    /*-----------------------------------------------------*\
    | LED snapshot -> frames, a full repaint. led_map is    |
    | the LED index -> (keyType, keyId) map; (0, 0) marks   |
    | an LED the device did not enumerate and is skipped.   |
    | Keys are grouped by keyType in order of first         |
    | appearance, in LED order within a keyType.            |
    \*-----------------------------------------------------*/
    std::vector<LogitechPerKey8080Frame> PlanLeds
        (
        const std::vector<RGBColor>&                        snapshot,
        const std::vector<std::pair<uint16_t, uint8_t>>&    led_map,
        int                                                 brightness,
        bool                                                kt40_id12_only
        );

    /*-----------------------------------------------------*\
    | Delta form of PlanLeds. sent holds, per LED, the      |
    | wire color (after dim scaling) the device last        |
    | committed, or a sentinel that matches no real color   |
    | (HIDPP20_UNCOMMITTED, high byte 0xFF) for an LED      |
    | that must be re-sent. Only LEDs whose wire color      |
    | differs from sent are planned. The result is a full   |
    | repaint, identical to the four-argument PlanLeds,     |
    | when IsFullRepaint() holds.                           |
    \*-----------------------------------------------------*/
    std::vector<LogitechPerKey8080Frame> PlanLeds
        (
        const std::vector<RGBColor>&                        snapshot,
        const std::vector<std::pair<uint16_t, uint8_t>>&    led_map,
        int                                                 brightness,
        bool                                                kt40_id12_only,
        const std::vector<RGBColor>&                        sent,
        bool                                                force_full
        );

    /*-----------------------------------------------------*\
    | True when the delta form plans a full repaint:        |
    | force_full (refresh timer), or sent does not cover    |
    | the snapshot (first frame; after sent_colors.clear()  |
    | in SetupZones, Direct, ReapplyActiveMode or a         |
    | re-init; zones resized).                              |
    \*-----------------------------------------------------*/
    bool IsFullRepaint(const std::vector<RGBColor>& sent, size_t n_leds, bool force_full);

    /*-----------------------------------------------------*\
    | Timed full refresh: due once elapsed_ms reaches       |
    | period_ms; period_ms 0 never fires.                   |
    \*-----------------------------------------------------*/
    bool RefreshDue(uint64_t elapsed_ms, uint32_t period_ms);

    /*-----------------------------------------------------*\
    | Record the outcome of one update in sent. sent is     |
    | first sized to n_leds (new entries = unsent). Each    |
    | frame's LED keys then advance to their wire color     |
    | when written[f] and committed (FlushLEDS acked), and  |
    | go to unsent otherwise, so the next update re-sends   |
    | them. A missing written[] entry counts as a failed    |
    | write; keys with led -1 are ignored.                  |
    \*-----------------------------------------------------*/
    void Settle
        (
        const std::vector<LogitechPerKey8080Frame>&         frames,
        const std::vector<bool>&                            written,
        bool                                                committed,
        std::vector<RGBColor>&                              sent,
        size_t                                              n_leds,
        RGBColor                                            unsent
        );

    /*-----------------------------------------------------*\
    | Encode one frame's fn3 payload (without the 4-byte    |
    | HID++ header) into payload, which must hold           |
    | LOGITECH_8080_FRAME_PAYLOAD_LEN bytes. Returns the    |
    | number of bytes to send: 4 + 4 * keys.                |
    \*-----------------------------------------------------*/
    size_t EncodeSetKeyColors(const LogitechPerKey8080Frame& frame, uint8_t* payload);
}
