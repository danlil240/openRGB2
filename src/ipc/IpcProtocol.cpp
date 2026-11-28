/*---------------------------------------------------------*\
|| IpcProtocol.cpp                                           ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "IpcProtocol.h"
#include "config/JsonBounded.h"

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

/* Channel outcome -> reader outcome. Always leaves `detail`
   populated — callers surface it verbatim ("read failed:
   timeout" vs "peer closed"; qa F12 showed an empty detail
   on the diagnostics CLI when the channel reported a clean
   Timeout/Closed without text). */
ReadResult MapIo(IpcIoResult r, std::string& detail)
{
    if(detail.empty())
    {
        detail = r == IpcIoResult::Closed  ? "peer closed"
               : r == IpcIoResult::Timeout ? "timeout"
                                           : "transport error";
    }
    return r == IpcIoResult::Closed   ? ReadResult::Closed
         : r == IpcIoResult::Timeout  ? ReadResult::Timeout
                                     : ReadResult::TransportError;
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
        event_kind::kReactive,
        event_kind::kWorkspace,
        event_kind::kPreview,
        event_kind::kCalibration,
        event_kind::kOwnership,
        event_kind::kOutput,
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
    case MsgType::Quit:            return "QUIT";
    case MsgType::ReactiveCommand: return "REACTIVE_COMMAND";
    case MsgType::WorkspaceCommand:return "WORKSPACE_COMMAND";
    case MsgType::OutputCommand:   return "OUTPUT_COMMAND";
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
    case MsgType::Output:          return "OUTPUT";
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

nlohmann::json ParseJsonBounded(const std::string& text, int max_depth)
{
    /* One implementation for the wire codec and the engine's
       file/staged/bundle parses (audit I-2) — see
       src/core/config/JsonBounded.h. */
    return studio::ParseJsonBounded<nlohmann::json>(text, max_depth);
}

const nlohmann::json& FieldOrNull(const nlohmann::json& obj,
                                  const char* key)
{
    static const nlohmann::json kNull;
    if(!obj.is_object())
    {
        return kNull;
    }
    const auto it = obj.find(key);
    return it == obj.end() ? kNull : *it;
}

nlohmann::json Message::Body() const
{
    if(body_raw.empty())
    {
        return nlohmann::json::object();
    }
    return ParseJsonBounded(body_raw);
}

std::vector<uint8_t> EncodeMessage(MsgType type, uint32_t request_id,
                                   const nlohmann::json& body,
                                   uint16_t version)
{
    const std::string raw = body.is_discarded() ? std::string()
                                              : body.dump();
    return EncodeRawMessage(type, request_id, raw, version);
}

std::vector<uint8_t> EncodeRawMessage(MsgType type, uint32_t request_id,
                                      const std::string& raw,
                                      uint16_t version)
{
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

IpcIoResult MessageReader::Fill(size_t want, uint64_t deadline_abs_ms,
                                std::string& err)
{
    /* Compact first: consumed bytes never accumulate. */
    if(pos_ > 0)
    {
        buf_.erase(buf_.begin(), buf_.begin() + (ptrdiff_t)pos_);
        pos_ = 0;
    }
    const uint32_t left = RemainingMs(deadline_abs_ms);
    if(left == 0)
    {
        return IpcIoResult::Timeout;
    }
    /* At least a small chunk (fewer syscalls for small
       messages), at most 64 KiB per call — bounds the
       over-read past the current message. */
    size_t chunk = want < 4096 ? 4096 : want;
    if(chunk > 64 * 1024)
    {
        chunk = 64 * 1024;
    }
    const size_t old = buf_.size();
    buf_.resize(old + chunk);
    size_t got = 0;
    const IpcIoResult r =
        ch_->RecvSome(buf_.data() + old, chunk, got, left, err);
    buf_.resize(old + (r == IpcIoResult::Ok ? got : 0));
    return r;
}

ReadResult MessageReader::Read(Message& msg, uint32_t deadline_ms,
                               std::string& detail, uint32_t idle_ms)
{
    if(poisoned_)
    {
        detail = "stream abandoned after an unskippable body";
        return ReadResult::ResyncExceeded;
    }
    /* The per-message clock starts at the first byte when an
       idle bound is given and nothing of the next message is
       buffered yet; otherwise it starts now. */
    bool     started      = idle_ms == 0 || Avail() > 0 ||
                            skip_left_ > 0;
    uint64_t deadline_abs = NowMs() + (started ? deadline_ms
                                               : idle_ms);

    const auto fill = [&](size_t want) -> IpcIoResult
    {
        const IpcIoResult r = Fill(want, deadline_abs, detail);
        if(r == IpcIoResult::Ok && !started)
        {
            started      = true;
            deadline_abs = NowMs() + deadline_ms;
        }
        return r;
    };

    for(;;)
    {
        /*---- 0. Discard a TooLarge body by its length -------*/
        if(skip_left_ > 0)
        {
            const size_t n = (size_t)std::min<uint64_t>(skip_left_,
                                                        Avail());
            pos_       += n;
            skip_left_ -= n;
            if(skip_left_ > 0)
            {
                const IpcIoResult r = fill((size_t)
                    std::min<uint64_t>(skip_left_, 64 * 1024));
                if(r != IpcIoResult::Ok)
                {
                    return MapIo(r, detail);
                }
                continue;
            }
        }

        /*---- 1. Magic hunt (resync) -------------------------*\
        | A synced stream hits magic on the first word at zero  |
        | cost. Out of sync, the buffer is scanned forward a    |
        | byte at a time — the next real message is recovered,  |
        | garbage is skipped. Bounded by kMaxResyncBytes per    |
        | message (the count survives a Timeout).               |
        \*------------------------------------------------------*/
        while(Avail() >= 4 && GetU32(buf_.data() + pos_) != kMagic)
        {
            if(scanned_ >= kMaxResyncBytes)
            {
                detail = "no magic within resync bound";
                return ReadResult::ResyncExceeded;
            }
            ++pos_;
            ++scanned_;
            ++resync_bytes_;
        }
        if(scanned_ >= kMaxResyncBytes)
        {
            detail = "no magic within resync bound";
            return ReadResult::ResyncExceeded;
        }
        if(Avail() < 4)
        {
            const IpcIoResult r = fill(kHeaderBytes - Avail());
            if(r != IpcIoResult::Ok)
            {
                if(r == IpcIoResult::Timeout && scanned_ > 0 &&
                   detail.empty())
                {
                    detail = "deadline during resync";
                }
                return MapIo(r, detail);
            }
            continue;
        }

        /*---- 2. Header --------------------------------------*/
        if(Avail() < kHeaderBytes)
        {
            const IpcIoResult r = fill(kHeaderBytes - Avail());
            if(r != IpcIoResult::Ok)
            {
                return MapIo(r, detail);
            }
            continue;
        }
        const uint8_t* hdr = buf_.data() + pos_ + 4;
        msg.version    = GetU16(hdr + 0);
        msg.type       = (MsgType)GetU16(hdr + 2);
        msg.request_id = GetU32(hdr + 4);
        const uint32_t body_len = GetU32(hdr + 8);

        if(body_len > kMaxBodyBytes)
        {
            pos_     += kHeaderBytes;
            scanned_  = 0;
            msg.body_raw.clear();
            detail = "body_len " + std::to_string(body_len) +
                     " exceeds cap " + std::to_string(kMaxBodyBytes);
            if(body_len <= kMaxSkipBytes)
            {
                skip_left_ = body_len;   /* dropped by length */
            }
            else
            {
                poisoned_ = true;
                detail += "; beyond the skip bound";
            }
            return ReadResult::TooLarge;
        }

        /*---- 3. Body ----------------------------------------*/
        if(Avail() < kHeaderBytes + (size_t)body_len)
        {
            const IpcIoResult r =
                fill(kHeaderBytes + (size_t)body_len - Avail());
            if(r != IpcIoResult::Ok)
            {
                return MapIo(r, detail);
            }
            continue;
        }
        const char* body =
            (const char*)buf_.data() + pos_ + kHeaderBytes;
        msg.body_raw.assign(body, body + body_len);
        pos_     += kHeaderBytes + body_len;
        scanned_  = 0;
        if(pos_ == buf_.size())
        {
            buf_.clear();
            pos_ = 0;
        }
        return ReadResult::Ok;
    }
}

} /* namespace ipc */
} /* namespace openrgb2 */
