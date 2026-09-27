/*---------------------------------------------------------*\
|| IpcProtocol.h                                             ||
||                                                           ||
||   The versioned wire contract for engine IPC (plan      ||
||   section 4 — "versioned local named pipes"; docs/      ||
||   ipc-protocol.md is the human-readable specification   ||
||   Milestone 4 builds against).                            ||
||                                                           ||
||   Framing — every message on the byte stream:           ||
||                                                           ||
||     u32 magic       = kMagic ("O2IP" on the wire)         ||
||     u16 version     = wire version of THIS message        ||
||     u16 type        = MsgType code                        ||
||     u32 request_id  = correlator (0 for pushes)           ||
||     u32 body_len    = JSON body bytes that follow         ||
||     u8  body[body_len] = UTF-8 JSON ({} when body_len=0)  ||
||                                                           ||
||   All integers little-endian. Bounds (plan section 6):  ||
||   bodies are capped at kMaxBodyBytes; a lost-framing    ||
||   stream resynchronizes on the next magic within        ||
||   kMaxResyncBytes scanned bytes, then the session is    ||
||   dropped. Every message read is deadline-bounded at    ||
||   the channel layer.                                    ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "IpcChannel.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace openrgb2
{
namespace ipc
{

/*---------------------------------------------------------*\
|| Constants                                                ||
\*---------------------------------------------------------*/

constexpr uint32_t kMagic           = 0x5049324F; /* LE wire bytes "O2IP" */
constexpr uint16_t kProtocolVersion = 1;
constexpr uint32_t kHeaderBytes     = 16;
constexpr uint32_t kMaxBodyBytes    = 1u << 20;   /* 1 MiB body cap       */
constexpr uint32_t kMaxResyncBytes  = 4u * kMaxBodyBytes;

/* Stable event-kind tokens for SUBSCRIBE / EVENT bodies. */
namespace event_kind
{
constexpr const char* kTopology    = "topology";      /* snapshot list changed  */
constexpr const char* kScene       = "scene";         /* applied scene revision */
constexpr const char* kPlayback    = "playback";      /* playback state changed */
constexpr const char* kConnection  = "connection";    /* SDK link state         */
constexpr const char* kFrameStatus = "frame_status";  /* delivery stats/colors  */
}

/* The set SnapshotEvent/SUBSCRIBE validation knows. */
const std::vector<std::string>& EventKinds();

/*---------------------------------------------------------*\
|| Message types — request codes grow up, replies at 0x8xxx.||
|| The diagnostics channel accepts ONLY DiagGet after the   ||
|| handshake (read-only by construction).                   ||
\*---------------------------------------------------------*/
enum class MsgType : uint16_t
{
    /*---- lifecycle ----------------------------------------*/
    Hello            = 0x0001,   /* first message; body {client_name,
                                  min_version, max_version}           */

    /*---- command channel requests -------------------------*/
    GetSnapshots     = 0x0100,   /* -> Snapshots                        */
    GetScene         = 0x0101,   /* -> Scene                            */
    SetScene         = 0x0102,   /* {document} -> Ok                    */
    GetPlayback      = 0x0103,   /* -> Playback                         */
    SetPlaybackMode  = 0x0104,   /* {mode} -> Ok                        */
    GetDeliveryStats = 0x0105,   /* {controller?} -> DeliveryStats      */
    GetState         = 0x0106,   /* -> State                            */
    LayoutCommand    = 0x0107,   /* {verb,args} -> Ok                   */
    Subscribe        = 0x0108,   /* {events:[kind]|"all"} -> Ok + EVENTs*/
    Unsubscribe      = 0x0109,   /* -> Ok                               */
    Ping             = 0x010a,   /* -> Pong                             */
    GetPresets       = 0x010b,   /* -> Presets (preset cards)           */
    Quit             = 0x010c,   /* -> Ok; requests the engine's ordered
                                       shutdown (the "Quit All" hook —
                                       plan section 4)                  */

    /*---- diagnostics channel (the only post-HELLO verb) ---*/
    DiagGet          = 0x0300,   /* -> Diagnostics                      */

    /*---- server -> client ---------------------------------*/
    Welcome          = 0x8001,   /* handshake accept                    */
    Ok               = 0x8002,   /* generic success reply               */
    Error            = 0x8003,   /* {code,error,message}                */
    Snapshots        = 0x8100,
    Scene            = 0x8101,
    Playback         = 0x8102,
    DeliveryStats    = 0x8103,
    State            = 0x8104,
    Pong             = 0x8105,
    Presets          = 0x8106,
    Event            = 0x8200,   /* push: {kind,data}; request_id = 0   */
    Diagnostics      = 0x8300,
};

const char* MsgTypeName(MsgType t);

/*---------------------------------------------------------*\
|| Error codes — stable, wire-visible. Carried in the ERROR ||
|| body as {code, error:"name", message:"human detail"}.    ||
\*---------------------------------------------------------*/
enum class ErrCode : uint32_t
{
    Ok              = 0,
    Malformed       = 1,   /* bad JSON / missing fields / bad framing   */
    TooLarge        = 2,   /* body_len over kMaxBodyBytes               */
    VersionMismatch = 3,   /* no common version, or message version !=
                              the negotiated one                        */
    ExpectedHello   = 4,   /* first message was not HELLO               */
    UnknownCommand  = 5,   /* type code not in the catalogue            */
    BadRequest      = 6,   /* well-formed request, invalid arguments    */
    Unavailable     = 7,   /* honest absence (no SDK, no scene, not yet
                              implemented — never a silent lie)         */
    ReadOnly        = 8,   /* any command verb on the diagnostics pipe  */
    RateLimited     = 9,   /* flood bound tripped; connection closes    */
    Busy            = 10,  /* connection cap reached; refused           */
    Internal        = 11,
};

const char* ErrCodeName(ErrCode c);

/*---------------------------------------------------------*\
|| Wire message + codec                                     ||
\*---------------------------------------------------------*/
struct Message
{
    uint16_t    version    = kProtocolVersion;
    MsgType     type       = MsgType::Ping;
    uint32_t    request_id = 0;
    std::string body_raw;              /* raw UTF-8 JSON; "" = {} */

    /* Parsed body accessor — is_discarded() when body_raw is not
       valid JSON. Callers treat a discarded parse as Malformed. */
    nlohmann::json Body() const;
};

/* Encodes one framed message. Empty vector when the serialized
   body exceeds kMaxBodyBytes (encode must be bounded too). */
std::vector<uint8_t> EncodeMessage(MsgType type, uint32_t request_id,
                                   const nlohmann::json& body,
                                   uint16_t version = kProtocolVersion);

std::vector<uint8_t> EncodeError(uint32_t request_id, ErrCode code,
                                 const std::string& message);

/*---------------------------------------------------------*\
|| MessageReader — resynchronizing, deadline-bounded stream ||
|| reader over IIpcChannel.                                 ||
||                                                          ||
|| A framing violation never kills the server: the reader   ||
|| scans forward for the next magic (resync) — a malformed  ||
|| or oversized message loses only itself; the NEXT valid   ||
|| message is still processed. The scan is bounded by       ||
|| kMaxResyncBytes per Read() call: a peer emitting         ||
|| unbounded garbage is closed once the bound is spent.     ||
\*---------------------------------------------------------*/
enum class ReadResult
{
    Ok,              /* msg filled                                */
    Closed,          /* orderly peer close                        */
    Timeout,         /* deadline elapsed mid-message (truncated)  */
    TooLarge,        /* header parsed, body_len > cap; fields in
                        msg carry the offending header; the body
                        is left for the next Read() to resync     */
    ResyncExceeded,  /* no magic within kMaxResyncBytes — close   */
    TransportError,  /* channel error; detail set                 */
};

class MessageReader
{
public:
    explicit MessageReader(IIpcChannel& channel);

    /* deadline_ms bounds the ENTIRE message (magic hunt +
       header + body) — a truncated message cannot stall the
       session longer than the deadline. */
    ReadResult Read(Message& msg, uint32_t deadline_ms,
                    std::string& detail);

    /* Byte count consumed by resync scans (diagnostics). */
    uint64_t ResyncBytes() const;

private:
    IIpcChannel* ch_;

    /* Buffered reads: resync scans chunk-wise and push the
       bytes past a found magic into carry_ so the next Read
       starts exactly at the header. */
    std::vector<uint8_t> carry_;
    size_t             carry_pos_ = 0;

    IpcIoResult ReadExact(void* buf, size_t len, uint64_t deadline_abs_ms,
                        std::string& err);
    uint64_t    resync_bytes_ = 0;
};

} /* namespace ipc */
} /* namespace openrgb2 */
