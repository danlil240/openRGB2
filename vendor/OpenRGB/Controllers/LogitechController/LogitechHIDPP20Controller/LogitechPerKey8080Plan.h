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
    | Encode one frame's fn3 payload (without the 4-byte    |
    | HID++ header) into payload, which must hold           |
    | LOGITECH_8080_FRAME_PAYLOAD_LEN bytes. Returns the    |
    | number of bytes to send: 4 + 4 * keys.                |
    \*-----------------------------------------------------*/
    size_t EncodeSetKeyColors(const LogitechPerKey8080Frame& frame, uint8_t* payload);
}
