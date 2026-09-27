/*---------------------------------------------------------*\
||| SdkWire.cpp                                               |
|||                                                           |
|||   OpenRGB SDK wire format. See SdkWire.h.               ||
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "SdkWire.h"

#include <cstring>

namespace openrgb2
{
namespace sdk
{

void PacketParser::Reset()
{
    buffer_.clear();
    ok_ = true;
    error_.clear();
}

bool PacketParser::Feed(const uint8_t* data, size_t len,
                        std::vector<Packet>& out)
{
    if(!ok_)
    {
        return false;
    }
    if(len > 0)
    {
        buffer_.insert(buffer_.end(), data, data + len);
    }

    for(;;)
    {
        if(buffer_.size() < kHeaderSize)
        {
            return true;                 /* truncated header: wait */
        }

        PacketHeader hdr;
        memcpy(&hdr, buffer_.data(), sizeof(hdr));

        if(memcmp(hdr.magic, "ORGB", 4) != 0)
        {
            ok_    = false;
            error_ = "bad packet magic (stream desynchronized)";
            buffer_.clear();
            return false;
        }
        if(hdr.size > kMaxPacketSize)
        {
            ok_    = false;
            error_ = "packet body exceeds the 8 MiB bound";
            buffer_.clear();
            return false;
        }

        const size_t total = kHeaderSize + hdr.size;
        if(buffer_.size() < total)
        {
            return true;                 /* truncated body: wait */
        }

        Packet pkt;
        pkt.header = hdr;
        pkt.body.assign(buffer_.begin() + kHeaderSize,
                        buffer_.begin() + total);
        out.push_back(std::move(pkt));
        buffer_.erase(buffer_.begin(), buffer_.begin() + total);
    }
}

std::vector<uint8_t> EncodePacket(uint32_t dev_id, uint32_t pkt_id,
                                  const std::vector<uint8_t>& body)
{
    PacketHeader hdr;
    memcpy(hdr.magic, "ORGB", 4);
    hdr.dev_id = dev_id;
    hdr.pkt_id = pkt_id;
    hdr.size   = (uint32_t)body.size();

    std::vector<uint8_t> bytes(kHeaderSize + body.size());
    memcpy(bytes.data(), &hdr, kHeaderSize);
    if(!body.empty())
    {
        memcpy(bytes.data() + kHeaderSize, body.data(), body.size());
    }
    return bytes;
}

namespace
{
    /*---------------------------------------------------------*\
    | Bounds-checked little-endian cursor over a reply body.   |
    \*---------------------------------------------------------*/
    struct Cursor
    {
        const uint8_t* p;
        size_t         left;

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
            out.assign(reinterpret_cast<const char*>(p), len - 1); /* strip NUL */
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
        uint32_t colors_min = 0, colors_max = 0;
        uint32_t brightness = 0, direction = 0;

        if(!c.String(out.name) || !c.U32(out.value) || !c.U32(out.flags)
           || !c.U32(out.speed_min) || !c.U32(out.speed_max))
        {
            return false;
        }
        if(version >= 3)
        {
            uint32_t brightness_min = 0, brightness_max = 0;
            if(!c.U32(brightness_min) || !c.U32(brightness_max))
            {
                return false;
            }
        }
        if(!c.U32(colors_min) || !c.U32(colors_max) || !c.U32(out.speed))
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
        out.direction = direction;

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
            out.colors.push_back(color);
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
} /* anonymous namespace */

bool ParseControllerData(const uint8_t* data, size_t len, uint32_t version,
                         SdkController& out)
{
    out = SdkController();
    Cursor c{ data, len };

    uint32_t data_size = 0;
    /* The server's size field counts the whole body, including itself. */
    if(!c.U32(data_size) || data_size != len || len < 4)
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
    /* The server reads a NUL-terminated string. */
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

std::vector<uint8_t> BuildResizeZoneBody(int32_t zone_index, int32_t new_size)
{
    std::vector<uint8_t> body(8);
    memcpy(body.data(), &zone_index, 4);
    memcpy(body.data() + 4, &new_size, 4);
    return body;
}

bool PatchModeSpeed(std::vector<uint8_t>& raw, uint32_t version,
                    uint32_t speed)
{
    /* name_len + name, then value/flags/speed_min/speed_max, then at v3+
       brightness_min/brightness_max, then colors_min/colors_max —
       the speed field follows. */
    if(raw.size() < 2)
    {
        return false;
    }
    uint16_t name_len = 0;
    memcpy(&name_len, raw.data(), 2);

    size_t off = 2 + name_len + 16;   /* name, value, flags, speed_min/max */
    if(version >= 3)
    {
        off += 8;                     /* brightness_min/max                */
    }
    off += 8;                         /* colors_min, colors_max            */
    if(off + 4 > raw.size())
    {
        return false;
    }
    memcpy(raw.data() + off, &speed, 4);
    return true;
}

bool PatchModeParams(std::vector<uint8_t>& raw, uint32_t version,
                     const ModePatch& patch)
{
    /* Layout mirrors ParseMode / GetModeDescriptionData:
       name(u16+bytes), value, flags, speed_min, speed_max,
       [v3+] brightness_min/max, colors_min/max, speed,
       [v3+] brightness, direction, color_mode,
       num_colors(u16), colors(u32 each) — always last. */
    if(raw.size() < 2)
    {
        return false;
    }
    uint16_t name_len = 0;
    memcpy(&name_len, raw.data(), 2);

    size_t off = 2 + name_len + 16;   /* name, value, flags, speed_min/max */
    if(version >= 3)
    {
        off += 8;                     /* brightness_min/max                */
    }
    off += 8;                         /* colors_min, colors_max            */
    if(off + 4 > raw.size())
    {
        return false;
    }
    if(patch.has_speed)
    {
        memcpy(raw.data() + off, &patch.speed, 4);
    }
    off += 4;                         /* speed                             */
    if(version >= 3)
    {
        off += 4;                     /* brightness                        */
    }
    if(off + 4 > raw.size())
    {
        return false;
    }
    if(patch.has_direction)
    {
        memcpy(raw.data() + off, &patch.direction, 4);
    }
    off += 4;                         /* direction                         */
    off += 4;                         /* color_mode                        */
    if(off + 2 > raw.size())
    {
        return false;
    }
    if(patch.has_colors)
    {
        /* The colors list is the last field — replace the tail
           wholesale so the count always matches the data. */
        const uint16_t n = (uint16_t)patch.colors.size();
        raw.resize(off);
        const size_t at = raw.size();
        raw.resize(at + 2 + n * 4);
        memcpy(raw.data() + at, &n, 2);
        for(uint16_t i = 0; i < n; ++i)
        {
            memcpy(raw.data() + at + 2 + i * 4, &patch.colors[i], 4);
        }
    }
    return true;
}

} /* namespace sdk */
} /* namespace openrgb2 */
