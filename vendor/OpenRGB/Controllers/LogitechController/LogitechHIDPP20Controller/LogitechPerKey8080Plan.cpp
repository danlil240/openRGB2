/*---------------------------------------------------------*\
| LogitechPerKey8080Plan.cpp                                |
|                                                           |
|   Send planner for HID++ 2.0 feature 0x8080 per-key       |
|   lighting. See LogitechPerKey8080Plan.h.                 |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include <cstring>
#include <map>
#include "LogitechPerKey8080Plan.h"

typedef std::vector<std::pair<uint16_t, std::vector<LogitechPerKey8080Key>>> KeyGroups;

RGBColor LogitechPerKey8080Plan::ScaleBrightness(RGBColor color, int brightness)
{
    if(brightness < 100)
    {
        uint8_t r = RGBGetRValue(color) * brightness / 100;
        uint8_t g = RGBGetGValue(color) * brightness / 100;
        uint8_t b = RGBGetBValue(color) * brightness / 100;
        color = ToRGBColor(r, g, b);
    }

    return color;
}

/*---------------------------------------------------------*\
| Shared tail of both entry points: per group, the G410     |
| keyType 0x40 filter, then frames of up to 14 keys.        |
\*---------------------------------------------------------*/
static std::vector<LogitechPerKey8080Frame> ChunkKeyGroups(const KeyGroups& by_type, bool kt40_id12_only)
{
    std::vector<LogitechPerKey8080Frame> frames;

    for(size_t t = 0; t < by_type.size(); t++)
    {
        uint16_t                                    key_type = by_type[t].first;
        const std::vector<LogitechPerKey8080Key>&   src_keys = by_type[t].second;

        /*-------------------------------------------------*\
        | G410 keyType 0x40 accepts only keyId 1            |
        | and 2; drop any others before encoding.           |
        \*-------------------------------------------------*/
        std::vector<LogitechPerKey8080Key>          filtered;
        const std::vector<LogitechPerKey8080Key>*   keys = &src_keys;

        if(kt40_id12_only && key_type == 0x40)
        {
            for(size_t k = 0; k < src_keys.size(); k++)
            {
                if(src_keys[k].key_id == 1 || src_keys[k].key_id == 2)
                {
                    filtered.push_back(src_keys[k]);
                }
            }
            keys = &filtered;
        }

        for(size_t off = 0; off < keys->size(); off += HIDPP20_8080_KEYS_PER_FRAME)
        {
            size_t count = keys->size() - off;
            if(count > HIDPP20_8080_KEYS_PER_FRAME)
            {
                count = HIDPP20_8080_KEYS_PER_FRAME;
            }

            LogitechPerKey8080Frame frame;
            frame.key_type = key_type;
            frame.keys.assign(keys->begin() + off, keys->begin() + off + count);
            frames.push_back(frame);
        }
    }

    return frames;
}

std::vector<LogitechPerKey8080Frame> LogitechPerKey8080Plan::ChunkGroups
    (
    const LogitechPerKey8080Groups&     by_type,
    bool                                kt40_id12_only
    )
{
    KeyGroups groups;

    for(size_t t = 0; t < by_type.size(); t++)
    {
        std::vector<LogitechPerKey8080Key> keys;

        for(size_t k = 0; k < by_type[t].second.size(); k++)
        {
            LogitechPerKey8080Key key;
            key.key_id = by_type[t].second[k].first;
            key.color  = by_type[t].second[k].second;
            key.led    = -1;
            keys.push_back(key);
        }

        groups.emplace_back(by_type[t].first, keys);
    }

    return ChunkKeyGroups(groups, kt40_id12_only);
}

std::vector<LogitechPerKey8080Frame> LogitechPerKey8080Plan::PlanLeds
    (
    const std::vector<RGBColor>&                        snapshot,
    const std::vector<std::pair<uint16_t, uint8_t>>&    led_map,
    int                                                 brightness,
    bool                                                kt40_id12_only
    )
{
    /*-----------------------------------------------------*\
    | Group painted LEDs by keyType, preserving keyType     |
    | order of first appearance. Skip unaddressable LEDs    |
    | (keyType 0 + keyId 0, present in the matrix but not   |
    | enumerated on the device).                            |
    \*-----------------------------------------------------*/
    KeyGroups                   by_type;
    std::map<uint16_t, size_t>  type_slot;

    for(size_t i = 0; i < snapshot.size() && i < led_map.size(); i++)
    {
        uint16_t key_type = led_map[i].first;
        uint8_t  key_id   = led_map[i].second;

        if(key_type == 0 && key_id == 0)
        {
            continue;
        }

        std::map<uint16_t, size_t>::iterator slot = type_slot.find(key_type);

        if(slot == type_slot.end())
        {
            type_slot[key_type] = by_type.size();
            by_type.emplace_back(key_type, std::vector<LogitechPerKey8080Key>());
            slot = type_slot.find(key_type);
        }

        LogitechPerKey8080Key key;
        key.key_id = key_id;
        key.color  = ScaleBrightness(snapshot[i], brightness);
        key.led    = (int)i;

        by_type[slot->second].second.push_back(key);
    }

    return ChunkKeyGroups(by_type, kt40_id12_only);
}

size_t LogitechPerKey8080Plan::EncodeSetKeyColors(const LogitechPerKey8080Frame& frame, uint8_t* payload)
{
    size_t count = frame.keys.size();
    if(count > HIDPP20_8080_KEYS_PER_FRAME)
    {
        count = HIDPP20_8080_KEYS_PER_FRAME;
    }

    /*-----------------------------------------------------*\
    | Payload: keyType(u16 BE) + count(u16 BE) + count x    |
    | (keyId, R, G, B). The sender prepends the 4-byte      |
    | HID++ header.                                         |
    \*-----------------------------------------------------*/
    memset(payload, 0, LOGITECH_8080_FRAME_PAYLOAD_LEN);

    payload[0] = (uint8_t)(frame.key_type >> 8);
    payload[1] = (uint8_t)(frame.key_type & 0xFF);
    payload[2] = (uint8_t)(count          >> 8);
    payload[3] = (uint8_t)(count          & 0xFF);

    size_t pos = 4;
    for(size_t k = 0; k < count; k++)
    {
        RGBColor color = frame.keys[k].color;
        payload[pos++] = frame.keys[k].key_id;      /* keyId (USB HID usage) */
        payload[pos++] = RGBGetRValue(color);
        payload[pos++] = RGBGetGValue(color);
        payload[pos++] = RGBGetBValue(color);
    }

    return pos;
}
