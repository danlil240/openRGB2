/*---------------------------------------------------------*\
|| IpcProtocol.cpp                                           ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "IpcProtocol.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace openrgb2
{
namespace ipc
{

namespace
{

uint64_t NowMs()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint32_t RemainingMs(uint64_t deadline_abs_ms)
{
    const uint64_t now = NowMs();
    if(now >= deadline_abs_ms)
    {
        return 0;
    }
    return (uint32_t)(deadline_abs_ms - now);
}

void PutU16(std::vector<uint8_t>& b, uint16_t v)
{
    b.push_back((uint8_t)(v & 0xFF));
    b.push_back((uint8_t)((v >> 8) & 0xFF));
}

void PutU32(std::vector<uint8_t>& b, uint32_t v)
{
    b.push_back((uint8_t)(v & 0xFF));
    b.push_back((uint8_t)((v >> 8) & 0xFF));
    b.push_back((uint8_t)((v >> 16) & 0xFF));
    b.push_back((uint8_t)((v >> 24) & 0xFF));
}

uint16_t GetU16(const uint8_t* p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

uint32_t GetU32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

} /* anonymous namespace */

const std::vector<std::string>& EventKinds()
{
    static const std::vector<std::string> kinds = {
        event_kind::kTopology,
        event_kind::kScene,
        event_kind::kPlayback,
        event_kind::kConnection,
        event_kind::kFrameStatus,
    };
    return kinds;
}

const char* MsgTypeName(MsgType t)
{
    switch(t)
    {
    case MsgType::Hello:           return "HELLO";
    case MsgType::GetSnapshots:    return "GET_SNAPSHOTS";
    case MsgType::GetScene:        return "GET_SCENE";
    case MsgType::SetScene:        return "SET_SCENE";
    case MsgType::GetPlayback:     return "GET_PLAYBACK";
    case MsgType::SetPlaybackMode: return "SET_PLAYBACK_MODE";
    case MsgType::GetDeliveryStats:return "GET_DELIVERY_STATS";
    case MsgType::GetState:        return "GET_STATE";
    case MsgType::LayoutCommand:   return "LAYOUT_COMMAND";
    case MsgType::Subscribe:       return "SUBSCRIBE";
    case MsgType::Unsubscribe:     return "UNSUBSCRIBE";
    case MsgType::Ping:            return "PING";
    case MsgType::GetPresets:      return "GET_PRESETS";
    case MsgType::DiagGet:         return "DIAG_GET";
    case MsgType::Welcome:         return "WELCOME";
    case MsgType::Ok:              return "OK";
    case MsgType::Error:           return "ERROR";
    case MsgType::Snapshots:       return "SNAPSHOTS";
    case MsgType::Scene:           return "SCENE";
    case MsgType::Playback:        return "PLAYBACK";
    case MsgType::DeliveryStats:   return "DELIVERY_STATS";
    case MsgType::State:           return "STATE";
    case MsgType::Pong:            return "PONG";
    case MsgType::Presets:         return "PRESETS";
    case MsgType::Event:           return "EVENT";
    case MsgType::Diagnostics:     return "DIAGNOSTICS";
    }
    return "?";
}

const char* ErrCodeName(ErrCode c)
{
    switch(c)
    {
    case ErrCode::Ok:              return "ok";
    case ErrCode::Malformed:       return "malformed";
    case ErrCode::TooLarge:        return "too_large";
    case ErrCode::VersionMismatch: return "version_mismatch";
    case ErrCode::ExpectedHello:   return "expected_hello";
    case ErrCode::UnknownCommand:  return "unknown_command";
    case ErrCode::BadRequest:      return "bad_request";
    case ErrCode::Unavailable:     return "unavailable";
    case ErrCode::ReadOnly:        return "read_only";
    case ErrCode::RateLimited:     return "rate_limited";
    case ErrCode::Busy:            return "busy";
    case ErrCode::Internal:        return "internal";
    }
    return "?";
}

nlohmann::json Message::Body() const
{
    if(body_raw.empty())
    {
        return nlohmann::json::object();
    }
    return nlohmann::json::parse(body_raw, nullptr, false);
}

std::vector<uint8_t> EncodeMessage(MsgType type, uint32_t request_id,
                                   const nlohmann::json& body,
                                   uint16_t version)
{
    const std::string raw = body.is_discarded() ? std::string()
                                              : body.dump();
    if(raw.size() > kMaxBodyBytes)
    {
        return {};
    }
    std::vector<uint8_t> out;
    out.reserve(kHeaderBytes + raw.size());
    PutU32(out, kMagic);
    PutU16(out, version);
    PutU16(out, (uint16_t)type);
    PutU32(out, request_id);
    PutU32(out, (uint32_t)raw.size());
    out.insert(out.end(), raw.begin(), raw.end());
    return out;
}

std::vector<uint8_t> EncodeError(uint32_t request_id, ErrCode code,
                                 const std::string& message)
{
    return EncodeMessage(MsgType::Error, request_id,
        nlohmann::json::object({
            { "code",    (uint32_t)code           },
            { "error",   ErrCodeName(code)        },
            { "message", message                  },
        }));
}

/*---------------------------------------------------------*\
|| MessageReader                                            ||
\*---------------------------------------------------------*/

MessageReader::MessageReader(IIpcChannel& channel) : ch_(&channel) {}

uint64_t MessageReader::ResyncBytes() const
{
    return resync_bytes_;
}

IpcIoResult MessageReader::ReadExact(void* buf, size_t len,
                                     uint64_t deadline_abs_ms,
                                     std::string& err)
{
    uint8_t* out = (uint8_t*)buf;
    size_t   got = 0;

    /* Drain carry_ first — resync leftovers. */
    const size_t avail = carry_.size() - carry_pos_;
    const size_t take  = avail < len ? avail : len;
    if(take)
    {
        memcpy(out, carry_.data() + carry_pos_, take);
        carry_pos_ += take;
        got        += take;
        if(carry_pos_ == carry_.size())
        {
            carry_.clear();
            carry_pos_ = 0;
        }
    }

    while(got < len)
    {
        const uint32_t left = RemainingMs(deadline_abs_ms);
        if(left == 0)
        {
            return IpcIoResult::Timeout;
        }
        size_t n = 0;
        const IpcIoResult r =
            ch_->RecvSome(out + got, len - got, n, left, err);
        if(r != IpcIoResult::Ok)
        {
            return r;
        }
        got += n;
    }
    return IpcIoResult::Ok;
}

ReadResult MessageReader::Read(Message& msg, uint32_t deadline_ms,
                               std::string& detail)
{
    const uint64_t deadline_abs = NowMs() + deadline_ms;

    /*---- 1. Magic hunt (resync) ---------------------------*\
    | A synced stream hits magic on the first word at zero   |
    | cost. Out of sync, the stream is scanned forward a     |
    | sliding little-endian window — the next real message   |
    | is recovered, garbage is skipped. Bounded by           |
    | kMaxResyncBytes per Read call.                         |
    \*---------------------------------------------------------*/
    uint32_t window = 0;
    {
        uint8_t first4[4];
        const IpcIoResult r =
            ReadExact(first4, 4, deadline_abs, detail);
        if(r != IpcIoResult::Ok)
        {
            return r == IpcIoResult::Closed   ? ReadResult::Closed
                 : r == IpcIoResult::Timeout  ? ReadResult::Timeout
                                             : ReadResult::TransportError;
        }
        window = GetU32(first4);
    }

    uint64_t scanned = 0;
    while(window != kMagic)
    {
        if(scanned >= kMaxResyncBytes)
        {
            detail = "no magic within resync bound";
            return ReadResult::ResyncExceeded;
        }
        /* Carry first — leftovers staged by a previous resync
           are stream bytes and must stay in order. They are
           consumed one byte at a time so that when magic is
           found, the unread tail of carry_ is exactly the
           bytes that follow it in the stream. */
        if(carry_pos_ < carry_.size())
        {
            window = (window >> 8)
                   | ((uint32_t)carry_[carry_pos_++] << 24);
            ++scanned;
            ++resync_bytes_;
            if(carry_pos_ == carry_.size())
            {
                carry_.clear();
                carry_pos_ = 0;
            }
            continue;
        }
        carry_.clear();
        carry_pos_ = 0;

        uint8_t chunk[256];
        const size_t want =
            (size_t)std::min<uint64_t>(sizeof(chunk),
                                       kMaxResyncBytes - scanned);
        size_t got = 0;
        const uint32_t left = RemainingMs(deadline_abs);
        if(left == 0)
        {
            detail = "deadline during resync";
            return ReadResult::Timeout;
        }
        const IpcIoResult r =
            ch_->RecvSome(chunk, want, got, left, detail);
        if(r != IpcIoResult::Ok)
        {
            return r == IpcIoResult::Closed   ? ReadResult::Closed
                 : r == IpcIoResult::Timeout  ? ReadResult::Timeout
                                            : ReadResult::TransportError;
        }
        for(size_t i = 0; i < got; ++i)
        {
            window = (window >> 8) | ((uint32_t)chunk[i] << 24);
            ++scanned;
            ++resync_bytes_;
            if(window == kMagic)
            {
                /* Bytes past the magic belong to the stream —
                   stage them so ReadExact sees them first.
                   carry_ is empty here (drained above). */
                if(i + 1 < got)
                {
                    carry_.insert(carry_.end(), chunk + i + 1,
                                  chunk + got);
                }
                goto header;
            }
            if(scanned >= kMaxResyncBytes)
            {
                detail = "no magic within resync bound";
                return ReadResult::ResyncExceeded;
            }
        }
    }
header:

    /*---- 2. Header ----------------------------------------*/
    uint8_t hdr[12];
    {
        const IpcIoResult r = ReadExact(hdr, sizeof(hdr), deadline_abs,
                                        detail);
        if(r != IpcIoResult::Ok)
        {
            return r == IpcIoResult::Closed   ? ReadResult::Closed
                 : r == IpcIoResult::Timeout  ? ReadResult::Timeout
                                            : ReadResult::TransportError;
        }
    }
    msg.version    = GetU16(hdr + 0);
    msg.type       = (MsgType)GetU16(hdr + 2);
    msg.request_id = GetU32(hdr + 4);
    const uint32_t body_len = GetU32(hdr + 8);

    if(body_len > kMaxBodyBytes)
    {
        detail = "body_len " + std::to_string(body_len) +
                 " exceeds cap " + std::to_string(kMaxBodyBytes);
        return ReadResult::TooLarge;   /* body left for resync */
    }

    /*---- 3. Body ------------------------------------------*/
    if(body_len)
    {
        msg.body_raw.resize(body_len);
        const IpcIoResult r =
            ReadExact(msg.body_raw.data(), body_len, deadline_abs,
                      detail);
        if(r != IpcIoResult::Ok)
        {
            return r == IpcIoResult::Closed   ? ReadResult::Closed
                 : r == IpcIoResult::Timeout  ? ReadResult::Timeout
                                            : ReadResult::TransportError;
        }
    }
    else
    {
        msg.body_raw.clear();
    }
    return ReadResult::Ok;
}

} /* namespace ipc */
} /* namespace openrgb2 */
