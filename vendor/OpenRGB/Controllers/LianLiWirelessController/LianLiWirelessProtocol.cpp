/*---------------------------------------------------------*\
| LianLiWirelessProtocol.cpp                                |
|                                                           |
|   Pure protocol layer for Lian Li SL Wireless dongles.    |
|   Ported from lian-li-smoke and sgtaziz/lian-li-linux.    |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "LianLiWirelessProtocol.h"
#include <cstring>
#include <stdexcept>

using namespace LianLiWireless;

static void BE16(uint8_t* at, uint16_t value)
{
    at[0] = (uint8_t)(value >> 8);
    at[1] = (uint8_t)value;
}

static void BE32(uint8_t* at, uint32_t value)
{
    at[0] = (uint8_t)(value >> 24);
    at[1] = (uint8_t)(value >> 16);
    at[2] = (uint8_t)(value >> 8);
    at[3] = (uint8_t)value;
}

bool LianLiWireless::IsValidUnicastMac(const Mac& mac)
{
    bool zero = true, broadcast = true;
    for(uint8_t b : mac)
    {
        zero      &= (b == 0x00);
        broadcast &= (b == 0xFF);
    }
    return !zero && !broadcast;
}

bool LianLiWireless::ParseDeviceRecord(const uint8_t* data, size_t len, Sighting& out)
{
    if(len < RECORD_SIZE || data[41] != RECORD_MARKER || data[18] == DEVICE_TYPE_MASTER)
    {
        return false;
    }

    Sighting d;
    std::memcpy(d.mac.data(),    data + 0, 6);
    std::memcpy(d.master.data(), data + 6, 6);

    if(!IsValidUnicastMac(d.mac))
    {
        return false;
    }

    d.channel       = data[12];
    d.receiver      = data[13];
    d.device_ticks  = ((uint32_t)data[14] << 24) | ((uint32_t)data[15] << 16)
                    | ((uint32_t)data[16] << 8)  | data[17];
    d.device_type   = data[18];

    /*---------------------------------------------------------*\
    | fan_num >= 10 flags right-attach (SL-INF daisy-chain)     |
    \*---------------------------------------------------------*/
    uint8_t raw_fans = data[19];
    d.right_attach   = raw_fans >= 10;
    d.fan_count      = d.right_attach ? std::min<uint8_t>(4, raw_fans - 10)
                                      : std::min<uint8_t>(4, raw_fans);

    std::memcpy(d.effect_id.data(), data + 20, 4);
    std::memcpy(d.fan_types.data(), data + 24, 3);
    d.fan_types[3] = 0;

    uint8_t status   = data[28];
    d.mb_rgb_sync    = (status & 0x40) != 0;
    d.pwm_line       = (status & 0x20) != 0;

    for(int i = 0; i < 4; i++)
    {
        d.rpm[i] = ((uint16_t)(data[28 + i * 2] & 0x0F) << 8) | data[29 + i * 2];
    }
    std::memcpy(d.pwm.data(), data + 36, 4);
    d.cmd_seq = data[40];

    out = d;
    return true;
}

bool LianLiWireless::ParseMasterRecord(const uint8_t* data, size_t len, Mac& mac, uint8_t& channel)
{
    if(len < RECORD_SIZE || data[41] != RECORD_MARKER || data[18] != DEVICE_TYPE_MASTER)
    {
        return false;
    }
    Mac m;
    std::memcpy(m.data(), data, 6);
    if(m == Mac{})
    {
        return false;
    }
    mac     = m;
    channel = data[12];
    return true;
}

std::vector<Sighting> LianLiWireless::ParseDiscoveryResponse(const uint8_t* data, size_t len)
{
    if(data == nullptr || len < 4 || data[0] != USB_CMD_SEND_RF)
    {
        throw std::invalid_argument("Invalid discovery response header");
    }

    size_t records = std::min<size_t>(data[1], MAX_DISCOVERY_RECORDS);
    if(len < 4 + RECORD_SIZE * records)
    {
        throw std::invalid_argument("Truncated discovery response");
    }

    std::vector<Sighting> result;
    for(size_t i = 0; i < records; i++)
    {
        Sighting s;
        if(ParseDeviceRecord(data + 4 + RECORD_SIZE * i, RECORD_SIZE, s))
        {
            result.push_back(s);
        }
    }
    return result;
}

RgbUpload::RgbUpload(std::vector<uint8_t> compressed_in, uint8_t led_count_in,
                     uint16_t frame_count_in, RgbTiming timing_in, uint8_t variant)
    : compressed(std::move(compressed_in))
    , led_count(led_count_in)
    , frame_count(frame_count_in)
    , timing(timing_in)
{
    if(compressed.empty() || compressed.size() > MAX_COMPRESSED_BYTES)
    {
        throw std::invalid_argument("Unsupported compressed RGB payload size");
    }
    if(led_count < 1 || frame_count < 1 || frame_count > MAX_FRAME_COUNT)
    {
        throw std::invalid_argument("Unsupported RGB upload dimensions");
    }
    if(timing.interval_ticks < 1)
    {
        throw std::invalid_argument("Unsupported RGB interval");
    }
    if(timing.secondary_frame_count == 0
       && (timing.secondary_interval_ticks != 0 || timing.outer_longest))
    {
        throw std::invalid_argument("Invalid secondary RGB region timing");
    }

    /*---------------------------------------------------------*\
    | FNV-1a over compressed payload + all timing/layout fields |
    \*---------------------------------------------------------*/
    uint32_t hash = 0x811C9DC5u;
    auto mix = [&hash](uint8_t b) { hash = (hash ^ b) * 0x01000193u; };

    for(uint8_t b : compressed) mix(b);
    mix((uint8_t)(timing.interval_ticks >> 8));
    mix((uint8_t)timing.interval_ticks);
    mix(timing.interval_fraction);
    mix((uint8_t)(frame_count >> 8));
    mix((uint8_t)frame_count);
    mix(led_count);
    mix((uint8_t)(timing.secondary_interval_ticks >> 8));
    mix((uint8_t)timing.secondary_interval_ticks);
    mix((uint8_t)(timing.secondary_frame_count >> 8));
    mix((uint8_t)timing.secondary_frame_count);
    mix(timing.outer_longest ? 1 : 0);
    mix(variant);

    uint32_t id = std::max<uint32_t>(hash, 1);
    effect_id = { (uint8_t)(id >> 24), (uint8_t)(id >> 16),
                  (uint8_t)(id >> 8), (uint8_t)id };
}

size_t RgbUpload::PacketCount() const
{
    return (compressed.size() + RGB_CHUNK_BYTES - 1) / RGB_CHUNK_BYTES + 1;
}

RfPacket RgbUpload::Packet(size_t index, const Mac& target, const Mac& master) const
{
    if(!IsValidUnicastMac(target) || !IsValidUnicastMac(master))
    {
        throw std::invalid_argument("Invalid unicast target");
    }
    if(index >= PacketCount())
    {
        throw std::invalid_argument("RGB packet index out of range");
    }

    RfPacket p{};
    p[0] = RF_SELECT;
    p[1] = RF_SET_RGB;
    std::memcpy(&p[2],  target.data(), 6);
    std::memcpy(&p[8],  master.data(), 6);
    std::memcpy(&p[14], effect_id.data(), 4);
    p[18] = (uint8_t)index;
    p[19] = (uint8_t)PacketCount();

    if(index == 0)
    {
        BE32(&p[20], (uint32_t)compressed.size());
        BE16(&p[25], frame_count);
        p[27] = led_count;
        BE16(&p[32], timing.interval_ticks);
        p[34] = timing.interval_fraction;
        BE16(&p[35], timing.secondary_interval_ticks);
        p[37] = timing.outer_longest ? 1 : 0;
        BE16(&p[38], timing.secondary_frame_count);
    }
    else
    {
        size_t offset = (index - 1) * RGB_CHUNK_BYTES;
        size_t n      = std::min(RGB_CHUNK_BYTES, compressed.size() - offset);
        std::memcpy(&p[20], compressed.data() + offset, n);
    }
    return p;
}

std::array<UsbChunk, USB_CHUNKS_PER_PACKET>
LianLiWireless::UsbChunks(const RfPacket& rf, uint8_t channel, uint8_t receiver)
{
    if(rf[0] != RF_SELECT || rf[1] != RF_SET_RGB || receiver < 1)
    {
        throw std::invalid_argument("Only addressed RGB uploads are allowed");
    }

    std::array<UsbChunk, USB_CHUNKS_PER_PACKET> chunks;
    for(size_t i = 0; i < USB_CHUNKS_PER_PACKET; i++)
    {
        UsbChunk p{};
        p[0] = USB_CMD_SEND_RF;
        p[1] = (uint8_t)i;
        p[2] = channel;
        p[3] = receiver;
        std::memcpy(&p[4], &rf[i * USB_CHUNK_PAYLOAD], USB_CHUNK_PAYLOAD);
        chunks[i] = p;
    }
    return chunks;
}

std::array<UsbChunk, USB_CHUNKS_PER_PACKET>
LianLiWireless::ClockChunks(const Mac& master, uint8_t channel,
                            uint16_t year, uint8_t month, uint8_t day,
                            uint8_t hour, uint8_t minute, uint8_t second,
                            bool initial)
{
    if(!IsValidUnicastMac(master))
    {
        throw std::invalid_argument("Invalid master MAC");
    }

    RfPacket rf{};
    rf[0] = RF_SELECT;
    rf[1] = RF_CLOCK_SYNC;
    std::memcpy(&rf[8], master.data(), 6);

    if(initial)
    {
        for(size_t i = 14; i < 64; i++)
        {
            rf[i] = RF_CLOCK_SYNC;
        }
    }
    else
    {
        BE16(&rf[46], year);
        rf[48] = month;
        rf[49] = day;
        rf[50] = hour;
        rf[51] = minute;
        rf[52] = second;
    }

    std::array<UsbChunk, USB_CHUNKS_PER_PACKET> chunks;
    for(size_t i = 0; i < USB_CHUNKS_PER_PACKET; i++)
    {
        UsbChunk p{};
        p[0] = USB_CMD_SEND_RF;
        p[1] = (uint8_t)i;
        p[2] = channel;
        p[3] = BROADCAST_RECEIVER;
        std::memcpy(&p[4], &rf[i * USB_CHUNK_PAYLOAD], USB_CHUNK_PAYLOAD);
        chunks[i] = p;
    }
    return chunks;
}
