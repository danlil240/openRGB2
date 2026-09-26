/*---------------------------------------------------------*\
|| sdk_wire.cpp                                              ||
||                                                           ||
||   OpenRGB SDK wire-format helpers for the benchmark     ||
||   producer. See sdk_wire.h.                              ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "sdk_wire.h"

#include <cstring>

namespace openrgb_bench
{
namespace
{
    /*---------------------------------------------------------*\
    | Bounds-checked cursor over the reply body. Everything    |
    | is little-endian on both ends (x86/x64 Windows).         |
    \*---------------------------------------------------------*/
    struct Cursor
    {
        const uint8_t* p;
        size_t        left;

        bool U16(uint16_t& out){ if(left < 2) return false; memcpy(&out, p, 2); p += 2; left -= 2; return true; }
        bool U32(uint32_t& out){ if(left < 4) return false; memcpy(&out, p, 4); p += 4; left -= 4; return true; }
        bool Skip(size_t n)     { if(left < n) return false; p += n; left -= n; return true; }

        bool String(std::string& out)
        {
            uint16_t len = 0;
            if(!U16(len) || len == 0 || left < len)
            {
                return false;
            }
            out.assign(reinterpret_cast<const char*>(p), len - 1);  /* strip NUL */
            return Skip(len);
        }

        const uint8_t* Mark() const { return p; }
    };

    /*---------------------------------------------------------*\
    | Mode description order (GetModeDescriptionData,          |
    | protocol <= 4): name, value, flags, speed_min, speed_max, |
    | [v3+] brightness_min, brightness_max, colors_min,        |
    | colors_max, speed, [v3+] brightness, direction,           |
    | color_mode, num_colors, colors.                           |
    \*---------------------------------------------------------*/
    bool ParseMode(Cursor& c, uint32_t version, SdkMode& out)
    {
        const uint8_t* begin = c.Mark();
        uint32_t speed_min = 0, speed_max = 0, speed = 0;
        uint32_t colors_min = 0, colors_max = 0;
        uint32_t brightness_min = 0, brightness_max = 0, brightness = 0;
        uint32_t direction = 0;

        if(!c.String(out.name) || !c.U32(out.value) || !c.U32(out.flags)
           || !c.U32(speed_min) || !c.U32(speed_max))
        {
            return false;
        }
        if(version >= 3)
        {
            if(!c.U32(brightness_min) || !c.U32(brightness_max))
            {
                return false;
            }
        }
        if(!c.U32(colors_min) || !c.U32(colors_max) || !c.U32(speed))
        {
            return false;
        }
        if(version >= 3)
        {
            if(!c.U32(brightness))
            {
                return false;
            }
        }
        if(!c.U32(direction) || !c.U32(out.color_mode))
        {
            return false;
        }

        uint16_t num_colors = 0;
        if(!c.U16(num_colors))
        {
            return false;
        }
        for(uint16_t i = 0; i < num_colors; i++)
        {
            uint32_t color = 0;
            if(!c.U32(color))
            {
                return false;
            }
        }

        const uint8_t* end = c.Mark();
        out.raw.assign(begin, end);
        return true;
    }

    bool ParseZone(Cursor& c, uint32_t version, SdkZone& out)
    {
        if(!c.String(out.name) || !c.U32(out.type)
           || !c.U32(out.leds_min) || !c.U32(out.leds_max)
           || !c.U32(out.leds_count))
        {
            return false;
        }

        uint16_t matrix_map_size = 0;
        if(!c.U16(matrix_map_size) || !c.Skip(matrix_map_size))
        {
            return false;
        }

        if(version >= 4)
        {
            uint16_t num_segments = 0;
            if(!c.U16(num_segments))
            {
                return false;
            }
            /* segment: name, type, start_idx, leds_count (v < 6) */
            for(uint16_t s = 0; s < num_segments; s++)
            {
                std::string seg_name;
                uint32_t    seg_type = 0, seg_start = 0, seg_count = 0;
                if(!c.String(seg_name) || !c.U32(seg_type)
                   || !c.U32(seg_start) || !c.U32(seg_count))
                {
                    return false;
                }
            }
        }
        return true;
    }
}

bool ParseControllerData(const uint8_t* data, size_t len, uint32_t version,
                         SdkController& out)
{
    out = SdkController();
    Cursor c{ data, len };

    uint32_t data_size = 0;
    if(!c.U32(data_size) || data_size != len - 4)
    {
        return false;
    }

    uint16_t num_modes = 0, num_zones = 0, num_leds = 0, num_colors = 0;

    if(!c.U32(out.type) || !c.String(out.name) || !c.String(out.vendor)
       || !c.String(out.description) || !c.String(out.version)
       || !c.String(out.serial) || !c.String(out.location)
       || !c.U16(num_modes) || !c.U32(out.active_mode))
    {
        return false;
    }

    out.modes.reserve(num_modes);
    for(uint16_t i = 0; i < num_modes; i++)
    {
        SdkMode mode;
        if(!ParseMode(c, version, mode))
        {
            return false;
        }
        out.modes.push_back(std::move(mode));
    }

    if(!c.U16(num_zones))
    {
        return false;
    }
    out.zones.reserve(num_zones);
    for(uint16_t i = 0; i < num_zones; i++)
    {
        SdkZone zone;
        if(!ParseZone(c, version, zone))
        {
            return false;
        }
        out.zones.push_back(std::move(zone));
    }

    if(!c.U16(num_leds))
    {
        return false;
    }
    for(uint16_t i = 0; i < num_leds; i++)
    {
        std::string led_name;
        uint32_t    led_value = 0;
        if(!c.String(led_name) || !c.U32(led_value))
        {
            return false;
        }
    }
    out.num_leds = num_leds;

    if(!c.U16(num_colors))
    {
        return false;
    }
    out.colors.reserve(num_colors);
    for(uint16_t i = 0; i < num_colors; i++)
    {
        uint32_t color = 0;
        if(!c.U32(color))
        {
            return false;
        }
        out.colors.push_back(color);
    }

    return c.left == 0;
}

std::vector<uint8_t> BuildProtocolVersionRequest(uint32_t our_version)
{
    std::vector<uint8_t> body(4);
    memcpy(body.data(), &our_version, 4);
    return body;
}

std::vector<uint8_t> BuildClientNameRequest(const std::string& name)
{
    /* the server reads a NUL-terminated string */
    std::vector<uint8_t> body(name.begin(), name.end());
    body.push_back('\0');
    return body;
}

std::vector<uint8_t> BuildControllerDataRequest(uint32_t protocol_version)
{
    std::vector<uint8_t> body(4);
    memcpy(body.data(), &protocol_version, 4);
    return body;
}

std::vector<uint8_t> BuildUpdateLEDsBody(const std::vector<uint32_t>& colors)
{
    const uint16_t count = (uint16_t)colors.size();
    std::vector<uint8_t> body(4 + 2 + colors.size() * 4);
    const uint32_t size = (uint32_t)body.size();
    memcpy(body.data(), &size, 4);
    memcpy(body.data() + 4, &count, 2);
    if(!colors.empty())
    {
        memcpy(body.data() + 6, colors.data(), colors.size() * 4);
    }
    return body;
}

std::vector<uint8_t> BuildUpdateModeBody(int32_t mode_index,
                                         const std::vector<uint8_t>& raw_mode)
{
    std::vector<uint8_t> body(4 + 4 + raw_mode.size());
    const uint32_t size = (uint32_t)body.size();
    memcpy(body.data(), &size, 4);
    memcpy(body.data() + 4, &mode_index, 4);
    if(!raw_mode.empty())
    {
        memcpy(body.data() + 8, raw_mode.data(), raw_mode.size());
    }
    return body;
}
}
