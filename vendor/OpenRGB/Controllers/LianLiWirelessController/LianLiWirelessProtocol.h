/*---------------------------------------------------------*\
| LianLiWirelessProtocol.h                                  |
|                                                           |
|   Pure protocol layer for Lian Li SL Wireless TX/RX       |
|   dongles (VID 0x0416, PID 0x8040/0x8041).                |
|   Packet construction, parsing, and validation only —     |
|   no I/O, no Qt, no OS dependencies.                      |
|                                                           |
|   Ported from the lian-li-smoke harness (PowerShell/C#)   |
|   and sgtaziz/lian-li-linux (MIT). See THIRD-PARTY notes. |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace LianLiWireless
{
    /*---------------------------------------------------------*\
    | Wire constants                                            |
    \*---------------------------------------------------------*/
    constexpr uint16_t USB_VID                    = 0x0416;
    constexpr uint16_t TX_PID                     = 0x8040;
    constexpr uint16_t RX_PID                     = 0x8041;

    constexpr uint8_t  USB_CMD_SEND_RF            = 0x10;
    constexpr uint8_t  USB_CMD_GET_MAC            = 0x11;

    constexpr uint8_t  RF_SELECT                  = 0x12;
    constexpr uint8_t  RF_CLOCK_SYNC              = 0x14;
    constexpr uint8_t  RF_SET_RGB                 = 0x20;

    constexpr size_t   RF_DATA_SIZE               = 240;   /* one RF packet   */
    constexpr size_t   USB_CHUNK_SIZE             = 64;    /* USB envelope    */
    constexpr size_t   USB_CHUNK_PAYLOAD          = 60;
    constexpr size_t   USB_CHUNKS_PER_PACKET      = RF_DATA_SIZE / USB_CHUNK_PAYLOAD;
    constexpr size_t   RGB_CHUNK_BYTES            = 220;   /* payload / chunk */
    constexpr size_t   MAX_COMPRESSED_BYTES       = 12288;
    constexpr size_t   MAX_LED_COUNT              = 255;
    constexpr size_t   MAX_FRAME_COUNT            = 4096;

    constexpr uint8_t  BROADCAST_RECEIVER         = 0xFF;
    constexpr uint8_t  DEVICE_TYPE_MASTER         = 0xFF;
    constexpr uint8_t  RECORD_MARKER              = 0x1C;
    constexpr size_t   RECORD_SIZE                = 42;
    constexpr size_t   MAX_DISCOVERY_RECORDS      = 10;

    using Mac = std::array<uint8_t, 6>;
    using RfPacket = std::array<uint8_t, RF_DATA_SIZE>;
    using UsbChunk = std::array<uint8_t, USB_CHUNK_SIZE>;

    /*---------------------------------------------------------*\
    | One timestamped RF sighting from the RX GetDev table.     |
    | Records are transient — a missing record is a miss, not   |
    | an error; callers poll and merge.                         |
    \*---------------------------------------------------------*/
    struct Sighting
    {
        Mac      mac;
        Mac      master;
        uint8_t  channel;
        uint8_t  receiver;
        uint32_t device_ticks;        /* ms * 0.625 uptime counter   */
        uint8_t  device_type;         /* 0=fan group, 0xFF=master    */
        uint8_t  fan_count;
        bool     right_attach;        /* fan_num >= 10 encoding      */
        std::array<uint8_t, 4>  effect_id;
        std::array<uint8_t, 4>  fan_types;
        std::array<uint16_t, 4> rpm;
        std::array<uint8_t, 4>  pwm;
        uint8_t  cmd_seq;
        bool     mb_rgb_sync;
        bool     pwm_line;
    };

    /*---------------------------------------------------------*\
    | Parse one 42-byte record. Returns false for records that  |
    | cannot name a fan group (bad marker, master record,       |
    | zero MAC). Masters are returned via ParseMasterRecord.    |
    \*---------------------------------------------------------*/
    bool ParseDeviceRecord(const uint8_t* data, size_t len, Sighting& out);
    bool ParseMasterRecord(const uint8_t* data, size_t len, Mac& mac, uint8_t& channel);

    /*---------------------------------------------------------*\
    | Parse a full GetDev response. Throws std::invalid_argument|
    | on wrong opcode or truncation.                            |
    \*---------------------------------------------------------*/
    std::vector<Sighting> ParseDiscoveryResponse(const uint8_t* data, size_t len);

    /*---------------------------------------------------------*\
    | RGB upload timing fields                                  |
    \*---------------------------------------------------------*/
    struct RgbTiming
    {
        uint16_t interval_ticks          = 0;   /* hundredths of a 0.625ms tick */
        uint8_t  interval_fraction       = 0;
        uint16_t secondary_interval_ticks = 0;
        uint16_t secondary_frame_count    = 0;
        bool     outer_longest            = false;
    };

    /*---------------------------------------------------------*\
    | A prepared RGB animation upload. Effect ID is FNV-1a over |
    | compressed data + all timing/layout fields (deterministic,|
    | never zero), matching the vendor uploader.                |
    \*---------------------------------------------------------*/
    class RgbUpload
    {
    public:
        /*---------------------------------------------------------*\
        | compressed must already be tinyuz-encoded. variant salts  |
        | the effect ID: the firmware ignores reuse of an ID it has |
        | already seen, so re-uploading identical content requires  |
        | a different variant.                                      |
        \*---------------------------------------------------------*/
        RgbUpload(std::vector<uint8_t> compressed, uint8_t led_count,
                  uint16_t frame_count, RgbTiming timing, uint8_t variant = 0);

        const std::array<uint8_t, 4>& EffectId() const { return effect_id; }
        uint8_t LedCount()    const { return led_count; }
        uint16_t FrameCount() const { return frame_count; }
        size_t  PacketCount() const;

        /* index 0 = header packet; 1..n = payload chunks        */
        RfPacket Packet(size_t index, const Mac& target, const Mac& master) const;

    private:
        std::vector<uint8_t>   compressed;
        uint8_t                led_count;
        uint16_t               frame_count;
        RgbTiming              timing;
        std::array<uint8_t, 4> effect_id;
    };

    /*---------------------------------------------------------*\
    | Split a 240-byte RF packet into four 64-byte USB chunks   |
    | addressed to (channel, receiver). Rejects non-RGB packets.|
    \*---------------------------------------------------------*/
    std::array<UsbChunk, USB_CHUNKS_PER_PACKET>
        UsbChunks(const RfPacket& rf, uint8_t channel, uint8_t receiver);

    /*---------------------------------------------------------*\
    | Master-clock keep-alive packet (broadcast receiver).      |
    | initial=true fills the sensor region with 0x14 padding;   |
    | otherwise year/month/day/hour/min/sec are encoded.        |
    \*---------------------------------------------------------*/
    std::array<UsbChunk, USB_CHUNKS_PER_PACKET>
        ClockChunks(const Mac& master, uint8_t channel,
                    uint16_t year, uint8_t month, uint8_t day,
                    uint8_t hour, uint8_t minute, uint8_t second,
                    bool initial);

    /*---------------------------------------------------------*\
    | Validation helpers                                        |
    \*---------------------------------------------------------*/
    bool IsValidUnicastMac(const Mac& mac);
}
