/*---------------------------------------------------------*\
||| SdkWire.h                                                 |
|||                                                           |
|||   OpenRGB SDK wire format for the engine-side client.   ||
|||   Pure parsing and packet building — no sockets, no Qt, ||
|||   offline-testable. The negotiated protocol version is   ||
|||   capped at 4 (plan section 4): the vendored backend     ||
|||   advertises 6, but every command below is exercised on  ||
|||   the protocol-4 wire shape only (verified against       ||
|||   vendor/OpenRGB/NetworkServer.cpp and RGBController::   ||
|||   GetDeviceDescriptionData/Set*Description).             ||
|||                                                           |
|||   Protocol-4 facts this file relies on:                  ||
|||     - NetPacketHeader is 16 bytes: magic "ORGB", u32     ||
|||       dev_id (controller INDEX, not stable id), u32      ||
|||       pkt_id, u32 body size                              ||
|||     - REQUEST_PROTOCOL_VERSION replies with the server's ||
|||       own version (6); the negotiated version is         ||
|||       min(reply, kProtocolCap)                           ||
|||     - REQUEST_CONTROLLER_DATA carries the requested      ||
|||       protocol version in its 4-byte body                ||
|||     - UPDATELEDS/UPDATEMODE bodies start with a u32      ||
|||       data_size equal to the packet body size            ||
|||     - RESIZEZONE is just { i32 zone_idx, i32 new_size }  ||
|||     - No ACKs and no unsolicited traffic below v6        ||
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace openrgb2
{
namespace sdk
{

constexpr uint32_t kProtocolCap   = 4;
constexpr size_t   kMaxPacketSize = 8 * 1024 * 1024; /* matches backend */
constexpr size_t   kHeaderSize    = 16;

/* Packet ids used by this client (vendor/OpenRGB/NetworkProtocol.h).
   All are valid at protocol <= 4; no v5+/v6-only command is sent. */
constexpr uint32_t kPktControllerCount   = 0;    /* REQUEST_CONTROLLER_COUNT  */
constexpr uint32_t kPktControllerData    = 1;    /* REQUEST_CONTROLLER_DATA   */
constexpr uint32_t kPktDeviceListUpdated = 100;  /* async notification        */
constexpr uint32_t kPktProtocolVersion   = 40;   /* REQUEST_PROTOCOL_VERSION  */
constexpr uint32_t kPktSetClientName     = 50;   /* SET_CLIENT_NAME           */
constexpr uint32_t kPktResizeZone        = 1000; /* RGBCONTROLLER_RESIZEZONE  */
constexpr uint32_t kPktUpdateLEDs        = 1050; /* RGBCONTROLLER_UPDATELEDS  */
constexpr uint32_t kPktSetCustomMode     = 1100; /* SETCUSTOMMODE             */
constexpr uint32_t kPktUpdateMode        = 1101; /* RGBCONTROLLER_UPDATEMODE  */
constexpr uint32_t kPktSaveMode          = 1102; /* RGBCONTROLLER_SAVEMODE    */
/* lianli-wireless fork extension (openrgb2): read-only
   wireless group status + diagnostics counters. A stock
   backend silently drops unknown packets under protocol
   <= 4 — the client must treat a reply timeout as
   "unsupported", never as a broken connection. */
constexpr uint32_t kPktLianLiWirelessStatus = 4000;

struct PacketHeader
{
    char        magic[4];    /* "ORGB" */
    uint32_t    dev_id;
    uint32_t    pkt_id;
    uint32_t    size;        /* body bytes following the header */
};
static_assert(sizeof(PacketHeader) == kHeaderSize,
              "SDK packet header is 16 bytes");

struct Packet
{
    PacketHeader         header;
    std::vector<uint8_t> body;
};

/*---------------------------------------------------------*\
||| PacketParser — bounded incremental TCP packet parser.  ||
|||                                                         ||
||| Feed() appends received bytes and emits every complete  ||
||| packet. Splitting a packet byte-wise or coalescing      ||
||| several into one read is transparent. A declared body   ||
||| size above kMaxPacketSize or a bad magic poisons the    ||
||| parser (Ok() == false): the connection is desynced and  ||
||| must be dropped. The parser never reads beyond the      ||
||| buffered bytes — truncated input simply waits for more. ||
\*---------------------------------------------------------*/
class PacketParser
{
public:
    bool Feed(const uint8_t* data, size_t len, std::vector<Packet>& out);
    bool               Ok() const { return ok_; }
    const std::string& Error() const { return error_; }
    void               Reset();
    size_t             Buffered() const { return buffer_.size(); }

private:
    std::vector<uint8_t> buffer_;
    bool                 ok_ = true;
    std::string          error_;
};

/* Serialize one packet (header + body). */
std::vector<uint8_t> EncodePacket(uint32_t dev_id, uint32_t pkt_id,
                                  const std::vector<uint8_t>& body);

/*---------------------------------------------------------*\
||| Parsed controller description (protocol <= 4 fields     ||
||| only — the wire layout mirrors                           ||
||| RGBController::GetDeviceDescriptionData).                ||
\*---------------------------------------------------------*/
struct SdkMode
{
    std::string          name;
    uint32_t             value      = 0;
    uint32_t             flags      = 0;
    uint32_t             speed_min  = 0;
    uint32_t             speed_max  = 0;
    uint32_t             speed      = 0;
    uint32_t             direction  = 0;
    uint32_t             color_mode = 0;
    std::vector<uint32_t> colors;     /* mode-specific colors   */
    /* Raw serialized description at the negotiated protocol
       version — replayed verbatim by UPDATEMODE. */
    std::vector<uint8_t> raw;
};

struct SdkZone
{
    std::string  name;
    uint32_t     type       = 0;
    uint32_t     leds_min   = 0;
    uint32_t     leds_max   = 0;
    uint32_t     leds_count = 0;
    /* Zone matrix map (keyboards): row-major cells hold the
       LED index for that grid position or 0xFFFFFFFF for an
       empty cell — the encoding RGBController::
       GetMatrixMapDescriptionData serializes. Empty when the
       zone reports no matrix. Bounded by the u16 on-wire
       blob size (<= ~16k cells). */
    uint32_t              matrix_rows = 0;   /* wire "height" */
    uint32_t              matrix_cols = 0;   /* wire "width"  */
    std::vector<uint32_t> matrix_map;
};

struct SdkController
{
    uint32_t              type = 0;
    std::string           name;
    std::string           vendor;
    std::string           description;
    std::string           version;
    std::string           serial;
    std::string           location;
    uint32_t              active_mode = 0;
    std::vector<SdkMode>  modes;
    std::vector<SdkZone>  zones;
    uint32_t              num_leds = 0;
    /* Per-LED names in SDK zone order ("Key: Escape", ...) —
       what scene preparation resolves key emitters against. */
    std::vector<std::string> led_names;
    std::vector<uint32_t> colors;    /* 0x00BBGGRR */
};

/*---------------------------------------------------------*\
||| Parse a REQUEST_CONTROLLER_DATA reply body (bytes after ||
||| the 16-byte packet header). `version` is the negotiated ||
||| protocol version. Bounded: returns false on malformed/  ||
||| truncated input, never reads past `len`, and rejects    ||
||| bodies with trailing garbage.                           ||
\*---------------------------------------------------------*/
bool ParseControllerData(const uint8_t* data, size_t len,
                         uint32_t version, SdkController& out);

/*---------------------------------------------------------*\
|| Parsed LIANLI_WIRELESS_STATUS reply — the vendor        ||
|| extension's null-terminated JSON body (SETTINGS reply   ||
|| convention). `raw` retains the whole document verbatim  ||
|| so diagnostics can pass backend-side fields through     ||
|| without a client rev; `groups` carries the typed view   ||
|| the clock-sample and wedge-detection paths consume.    ||
\*---------------------------------------------------------*/
struct SdkWirelessClock
{
    bool     valid          = false;
    uint32_t ticks          = 0;
    uint64_t sampled_at_ms  = 0;   /* backend monotonic ms  */
    uint64_t age_ms         = 0;   /* backend-side age      */
};

struct SdkWirelessGroupStatus
{
    std::string       mac;            /* == controller serial */
    std::string       state;          /* WirelessState lowercased */
    bool              wedged        = false;
    bool              confirmed     = false;
    uint64_t          resends       = 0;
    uint64_t          send_failures = 0;
    std::string       last_error;
    SdkWirelessClock  clock;
};

struct SdkWirelessStatus
{
    nlohmann::json                        raw;
    std::vector<SdkWirelessGroupStatus>   groups;
};

/* Bounded: false on non-JSON, non-object, or a malformed
   "groups" member. Tolerates (and requires neither) the
   trailing NUL the server appends. */
bool ParseWirelessStatus(const uint8_t* data, size_t len,
                         SdkWirelessStatus& out);

/*---------------------------------------------------------*\
||| Request bodies (bytes after the packet header).         ||
\*---------------------------------------------------------*/
std::vector<uint8_t> BuildProtocolVersionRequest(uint32_t our_version);
std::vector<uint8_t> BuildClientNameRequest(const std::string& name);
std::vector<uint8_t> BuildControllerDataRequest(uint32_t protocol_version);

/* UPDATELEDS body: u32 data_size (== body size) + u16 count +
   colors (0x00BBGGRR). Covers the WHOLE controller — the
   complete-controller write required by plan section 4. */
std::vector<uint8_t> BuildUpdateLEDsBody(const std::vector<uint32_t>& colors);

/* UPDATEMODE body: u32 data_size + i32 mode_index + raw mode
   description captured by ParseControllerData. */
std::vector<uint8_t> BuildUpdateModeBody(int32_t mode_index,
                                         const std::vector<uint8_t>& raw_mode);

/* RESIZEZONE body: i32 zone_idx + i32 new_size (no leading
   data_size field — verified in ProcessRequest_RGBController_
   ResizeZone). */
std::vector<uint8_t> BuildResizeZoneBody(int32_t zone_index,
                                         int32_t new_size);

/*---------------------------------------------------------*\
||| Overwrite the speed field inside a raw mode description ||
||| (SdkMode::raw). Returns false on malformed input.       ||
\*---------------------------------------------------------*/
bool PatchModeSpeed(std::vector<uint8_t>& raw, uint32_t version,
                    uint32_t speed);

/*---------------------------------------------------------*\
||| ModePatch — the playback-mode parameter patch: speed,  ||
||| direction, and mode-specific colors inside a raw mode  ||
||| description (UPDATEMODE replays the whole blob, so the  ||
||| applied parameters ride the same packet). Unset fields ||
||| keep the enumerated value. Colors replace the whole    ||
||| tail — the mode description's variable-length colors   ||
||| list is always last (verified layout in SdkWire.cpp).  ||
\*---------------------------------------------------------*/
struct ModePatch
{
    bool                  has_speed     = false;   uint32_t speed     = 0;
    bool                  has_direction = false;   uint32_t direction = 0;
    bool                  has_colors    = false;
    std::vector<uint32_t> colors;
};

/* Returns false on malformed input (bad name field or a
   blob shorter than the protocol-version layout requires). */
bool PatchModeParams(std::vector<uint8_t>& raw, uint32_t version,
                     const ModePatch& patch);

} /* namespace sdk */
} /* namespace openrgb2 */
