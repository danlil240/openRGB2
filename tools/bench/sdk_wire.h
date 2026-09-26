/*---------------------------------------------------------*\
|| sdk_wire.h                                                ||
||                                                           ||
||   Minimal OpenRGB SDK wire-format helpers for the       ||
||   Milestone 1 benchmark producer. Pure parsing and       ||
||   packet building — no sockets, no Qt, offline-testable. ||
||   The negotiated protocol version is capped at 4.       ||
||                                                           ||
||   Wire layout mirrors RGBController::GetDeviceDescription ||
||   in the vendored backend (protocol <= 4 fields only).    ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace openrgb_bench
{
    constexpr uint32_t SDK_PROTOCOL_CAP = 4;

    /* packet ids used by the bench producer (NetworkProtocol.h) */
    constexpr uint32_t PKT_REQUEST_PROTOCOL_VERSION = 40;
    constexpr uint32_t PKT_SET_CLIENT_NAME          = 50;
    constexpr uint32_t PKT_REQUEST_CONTROLLER_COUNT = 0;
    constexpr uint32_t PKT_REQUEST_CONTROLLER_DATA  = 1;
    constexpr uint32_t PKT_UPDATELEDS               = 1050;
    constexpr uint32_t PKT_SETCUSTOMMODE             = 1100;
    constexpr uint32_t PKT_UPDATEMODE                = 1101;

    struct PacketHeader
    {
        char        magic[4];    /* "ORGB" */
        uint32_t    dev_id;
        uint32_t    pkt_id;
        uint32_t    size;
    };
    static_assert(sizeof(PacketHeader) == 16, "SDK packet header is 16 bytes");

    struct SdkMode
    {
        std::string              name;
        uint32_t                 value       = 0;
        uint32_t                 flags       = 0;
        uint32_t                 speed       = 0;
        uint32_t                 color_mode  = 0;
        /* raw serialized description at the negotiated protocol version —
           replayed verbatim by UpdateMode for state restore */
        std::vector<uint8_t>     raw;
    };

    struct SdkZone
    {
        std::string  name;
        uint32_t     type       = 0;
        uint32_t     leds_min   = 0;
        uint32_t     leds_max   = 0;
        uint32_t     leds_count = 0;
    };

    struct SdkController
    {
        uint32_t                 type = 0;
        std::string              name;
        std::string              vendor;
        std::string              description;
        std::string              version;
        std::string              serial;
        std::string              location;
        uint32_t                 active_mode = 0;
        std::vector<SdkMode>     modes;
        std::vector<SdkZone>     zones;
        uint32_t                 num_leds    = 0;
        std::vector<uint32_t>    colors;      /* 0x00BBGGRR              */
    };

    /*---------------------------------------------------------*\
    | Parse a REQUEST_CONTROLLER_DATA reply body (the bytes    |
    | after the 16-byte packet header). version is the         |
    | negotiated protocol version. Returns false on           |
    | malformed/truncated input.                              |
    \*---------------------------------------------------------*/
    bool ParseControllerData(const uint8_t* data, size_t len,
                             uint32_t version, SdkController& out);

    /*---------------------------------------------------------*\
    | Build request bodies (bytes after the packet header).    |
    \*---------------------------------------------------------*/
    std::vector<uint8_t> BuildProtocolVersionRequest(uint32_t our_version);
    std::vector<uint8_t> BuildClientNameRequest(const std::string& name);
    std::vector<uint8_t> BuildControllerDataRequest(uint32_t protocol_version);

    /* colors are 0x00BBGGRR, matching RGBColor packing */
    std::vector<uint8_t> BuildUpdateLEDsBody(const std::vector<uint32_t>& colors);

    /* replays a raw mode description captured by ParseControllerData */
    std::vector<uint8_t> BuildUpdateModeBody(int32_t mode_index,
                                             const std::vector<uint8_t>& raw_mode);

    /*---------------------------------------------------------*\
    | Overwrites the speed field inside a raw mode             |
    | description (as captured in SdkMode::raw). Returns       |
    | false on malformed input.                                |
    \*---------------------------------------------------------*/
    bool PatchModeSpeed(std::vector<uint8_t>& raw, uint32_t version,
                        uint32_t speed);
}
