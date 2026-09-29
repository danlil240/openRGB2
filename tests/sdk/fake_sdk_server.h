// SPDX-License-Identifier: GPL-2.0-or-later
//
// In-process fake OpenRGB SDK backend for the tests/sdk
// suite. No sockets at all: FakeStream is the client's
// injected IStreamTransport and the scripted server answers
// synchronously inside SendAll — the request/reply flow is
// fully deterministic and offline (no real backend, no
// hardware, no threads, no sleeps).
//
// The fake speaks the protocol-4 wire shape verified against
// vendor/OpenRGB/NetworkServer.cpp: 16-byte ORGB headers,
// u32 dev_id = controller INDEX, a protocol-version reply
// carrying the server's own version, and no ACKs/async
// traffic below v6. Client requests are decoded through the
// production PacketParser — the same bounded parse the wire
// tests exercise byte-wise.

#pragma once

#include "SdkWire.h"
#include "Transport.h"

#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace sdk_test
{

using openrgb2::sdk::Packet;
using openrgb2::sdk::PacketParser;
using openrgb2::sdk::IStreamTransport;
using openrgb2::sdk::RecvResult;

/*---------------------------------------------------------*\
||| Scripted controller description (protocol-4 fields).   ||
\*---------------------------------------------------------*/
struct FakeMode
{
    std::string          name;
    uint32_t             value          = 0;
    uint32_t             flags          = 0;
    uint32_t             speed_min      = 0;
    uint32_t             speed_max      = 255;
    uint32_t             brightness_min = 0;
    uint32_t             brightness_max = 255;
    uint32_t             colors_min     = 0;
    uint32_t             colors_max     = 0;
    uint32_t             speed          = 50;
    uint32_t             brightness     = 255;
    uint32_t             direction      = 0;
    uint32_t             color_mode     = 0;
    std::vector<uint32_t> colors;
};

struct FakeZone
{
    std::string name;
    uint32_t    type       = 0;
    uint32_t    leds_min   = 0;
    uint32_t    leds_max   = 0;
    uint32_t    leds_count = 0;
};

struct FakeController
{
    uint32_t             type = 2;
    std::string          name;
    std::string          vendor      = "Fake";
    std::string          description = "fake controller";
    std::string          version     = "1.0";
    std::string          serial;
    std::string          location;
    uint32_t             active_mode = 0;
    std::vector<FakeMode> modes;
    std::vector<FakeZone> zones;
    std::vector<std::string> led_names;
    std::vector<uint32_t>  colors;
};

struct ServerEvent
{
    uint32_t             dev_id = 0;
    uint32_t             pkt_id = 0;
    std::vector<uint8_t> body;
};

class FakeStream;

/*---------------------------------------------------------*\
||| FakeSdkServer — the scripted peer. HandleSend parses    ||
||| client packets and enqueues reply bytes onto the        ||
||| stream; every other packet is recorded as an event      ||
||| (no replies below v6, matching the real backend).       ||
\*---------------------------------------------------------*/
class FakeSdkServer
{
public:
    /* What the server reports as ITS protocol version (the
       vendored fork reports 6). */
    uint32_t reported_version = 6;

    /* false models a downed backend — Connect fails. */
    bool     listening = true;

    /* true corrupts every reply's magic — models a desynced
       peer the client must reject. */
    bool     corrupt_reply_magic = false;

    std::vector<FakeController> controllers;
    std::vector<ServerEvent>    events;

    /* Thread-safe views. HandleSend may run on delivery-lane
       threads (the engine E2E test drives the REAL pipeline),
       so concurrent readers must go through these accessors
       rather than iterating `events` directly — the public
       member stays for the single-threaded tests/sdk suites. */
    std::vector<ServerEvent> SnapshotEvents() const
    {
        std::lock_guard<std::mutex> g(mutex_);
        return events;
    }
    size_t CountEvents(uint32_t pkt_id) const
    {
        std::lock_guard<std::mutex> g(mutex_);
        size_t n = 0;
        for(const ServerEvent& e : events)
        {
            if(e.pkt_id == pkt_id) ++n;
        }
        return n;
    }
    size_t CountEvents(uint32_t pkt_id, uint32_t dev_id) const
    {
        std::lock_guard<std::mutex> g(mutex_);
        size_t n = 0;
        for(const ServerEvent& e : events)
        {
            if(e.pkt_id == pkt_id && e.dev_id == dev_id) ++n;
        }
        return n;
    }
    void ClearEvents()
    {
        std::lock_guard<std::mutex> g(mutex_);
        events.clear();
    }

    /* Device disconnect/reconnect simulation (task 5.2): the
       backend's enumeration is positional — erasing a
       controller shifts the indices of every later one,
       exactly like the real server serving a shorter
       controller list. HandleSend reads `controllers` under
       the same mutex, so a removal mid-request is safe. */
    void RemoveController(size_t index)
    {
        std::lock_guard<std::mutex> g(mutex_);
        if(index < controllers.size())
        {
            controllers.erase(controllers.begin() +
                              (ptrdiff_t)index);
        }
    }
    void AddController(const FakeController& c)
    {
        std::lock_guard<std::mutex> g(mutex_);
        controllers.push_back(c);
    }

    void HandleSend(FakeStream& stream, const uint8_t* data,
                    size_t len);

    /* Serialize one controller description at protocol <= 5
       (v4 layout). Mirrors
       RGBController::GetDeviceDescriptionData. */
    static std::vector<uint8_t> BuildControllerBody(
        const FakeController& c);

private:
    PacketParser        parser_;
    mutable std::mutex  mutex_;
};

/*---------------------------------------------------------*\
||| FakeStream — IStreamTransport bound to a FakeSdkServer. ||
||| Bytes the client sends are handed to the server's       ||
||| script synchronously; reply bytes queue for RecvExact.  ||
\*---------------------------------------------------------*/
class FakeStream final : public IStreamTransport
{
public:
    explicit FakeStream(FakeSdkServer& server) : server_(&server) {}

    /* Test hooks: inject arbitrary server->client bytes
       (malformed-packet tests) or sever the peer.
       SetPeerClosed models a graceful close (PeerClosed()
       reports it; next send fails). SetSendFails models an
       undetected reset — the next send fails before any
       liveness peek sees it. */
    void InjectServerBytes(std::vector<uint8_t> bytes)
    {
        recv_queue_.insert(recv_queue_.end(), bytes.begin(),
                           bytes.end());
    }
    void SetPeerClosed(bool closed) { peer_closed_ = closed; }
    void SetSendFails(bool fails)   { send_fails_  = fails; }

    bool Connect(const std::string& host, uint16_t port,
                 uint32_t connect_timeout_ms,
                 uint32_t recv_timeout_ms,
                 std::string& err) override
    {
        (void)host; (void)port; (void)connect_timeout_ms;
        (void)recv_timeout_ms;
        if(connected_)
        {
            err = "socket already connected";
            return false;
        }
        if(!server_->listening)
        {
            err = "cannot connect to " + host + ":" +
                  std::to_string(port);
            return false;
        }
        connected_   = true;
        peer_closed_ = false;
        send_fails_  = false;
        return true;
    }

    void Close() override
    {
        connected_ = false;
        recv_queue_.clear();
    }

    bool IsOpen() const override { return connected_; }

    bool SendAll(const void* data, size_t len,
                 std::string& err) override
    {
        if(!connected_ || peer_closed_ || send_fails_)
        {
            err = "send failed (connection reset)";
            return false;
        }
        server_->HandleSend(*this, (const uint8_t*)data, len);
        return true;
    }

    RecvResult RecvExact(void* buf, size_t len,
                         std::string& err) override
    {
        (void)err;
        if(peer_closed_)
        {
            return RecvResult::Closed;
        }
        if(recv_queue_.size() < len)
        {
            return RecvResult::Timeout;
        }
        uint8_t* out = (uint8_t*)buf;
        for(size_t i = 0; i < len; ++i)
        {
            out[i] = recv_queue_.front();
            recv_queue_.pop_front();
        }
        return RecvResult::Ok;
    }

    bool WaitReadable(uint32_t timeout_ms, bool& readable,
                      std::string& err) override
    {
        (void)timeout_ms; (void)err;
        readable = peer_closed_ || !recv_queue_.empty();
        return true;
    }

    bool PeerClosed() override { return peer_closed_; }

private:
    FakeSdkServer*      server_;
    bool                connected_   = false;
    bool                peer_closed_ = false;
    bool                send_fails_  = false;
    std::deque<uint8_t> recv_queue_;
};

/*---------------------------------------------------------*\
||| FakeSdkServer implementation                            ||
\*---------------------------------------------------------*/

namespace detail
{
    inline void PutU16(std::vector<uint8_t>& b, uint16_t v)
    {
        const size_t s = b.size();
        b.resize(s + 2);
        memcpy(b.data() + s, &v, 2);
    }
    inline void PutU32(std::vector<uint8_t>& b, uint32_t v)
    {
        const size_t s = b.size();
        b.resize(s + 4);
        memcpy(b.data() + s, &v, 4);
    }
    inline void PutStr(std::vector<uint8_t>& b, const std::string& s)
    {
        PutU16(b, (uint16_t)(s.size() + 1));
        b.insert(b.end(), s.begin(), s.end());
        b.push_back(0);
    }

    /* Apply the applied fields of a replayed mode description
       (the raw blob inside an UPDATEMODE body — the same
       v4-shaped serialization BuildControllerBody emits for
       one mode). Bounded: false on malformed input, never
       reads past `len`. Only the fields the client can patch
       (speed/direction/colors + name) are written back. */
    inline bool ApplyModeBlob(const uint8_t* p, size_t len,
                              FakeMode& m)
    {
        if(len < 2)
        {
            return false;
        }
        uint16_t name_len = 0;
        memcpy(&name_len, p, 2);
        if(name_len == 0 || len < 2u + name_len)
        {
            return false;
        }
        m.name.assign(reinterpret_cast<const char*>(p + 2),
                      name_len - 1);
        const uint8_t* q = p + 2 + name_len;
        size_t         left = len - 2 - name_len;
        const auto u32 = [&q, &left](uint32_t& v) -> bool
        {
            if(left < 4)
            {
                return false;
            }
            memcpy(&v, q, 4);
            q += 4;
            left -= 4;
            return true;
        };
        /* value, flags, speed_min, speed_max, brightness_min,
           brightness_max, colors_min, colors_max */
        uint32_t v = 0;
        for(int i = 0; i < 8; ++i)
        {
            if(!u32(v)) { return false; }
        }
        if(!u32(m.speed) || !u32(v) /* brightness */ ||
           !u32(m.direction) || !u32(m.color_mode))
        {
            return false;
        }
        if(left < 2)
        {
            return false;
        }
        uint16_t ncolors = 0;
        memcpy(&ncolors, q, 2);
        q += 2;
        left -= 2;
        if((size_t)ncolors * 4 > left)
        {
            return false;
        }
        m.colors.assign(ncolors, 0);
        for(uint16_t i = 0; i < ncolors; ++i)
        {
            memcpy(&m.colors[i], q + i * 4, 4);
        }
        return true;
    }
}

inline std::vector<uint8_t> FakeSdkServer::BuildControllerBody(
    const FakeController& c)
{
    using detail::PutU16;
    using detail::PutU32;
    using detail::PutStr;
    std::vector<uint8_t> b;
    PutU32(b, 0);                                  /* size placeholder */
    PutU32(b, c.type);
    PutStr(b, c.name);
    PutStr(b, c.vendor);
    PutStr(b, c.description);
    PutStr(b, c.version);
    PutStr(b, c.serial);
    PutStr(b, c.location);
    PutU16(b, (uint16_t)c.modes.size());
    PutU32(b, c.active_mode);
    for(const FakeMode& m : c.modes)
    {
        PutStr(b, m.name);
        PutU32(b, m.value);
        PutU32(b, m.flags);
        PutU32(b, m.speed_min);
        PutU32(b, m.speed_max);
        PutU32(b, m.brightness_min);               /* v3+ fields */
        PutU32(b, m.brightness_max);
        PutU32(b, m.colors_min);
        PutU32(b, m.colors_max);
        PutU32(b, m.speed);
        PutU32(b, m.brightness);
        PutU32(b, m.direction);
        PutU32(b, m.color_mode);
        PutU16(b, (uint16_t)m.colors.size());
        for(uint32_t col : m.colors)
        {
            PutU32(b, col);
        }
    }
    PutU16(b, (uint16_t)c.zones.size());
    for(const FakeZone& z : c.zones)
    {
        PutStr(b, z.name);
        PutU32(b, z.type);
        PutU32(b, z.leds_min);
        PutU32(b, z.leds_max);
        PutU32(b, z.leds_count);
        PutU16(b, 0);                              /* matrix map size */
        PutU16(b, 0);                              /* num segments    */
    }
    PutU16(b, (uint16_t)c.led_names.size());
    for(const std::string& n : c.led_names)
    {
        PutStr(b, n);
        PutU32(b, 0);                              /* led value */
    }
    PutU16(b, (uint16_t)c.colors.size());
    for(uint32_t col : c.colors)
    {
        PutU32(b, col);
    }
    const uint32_t size = (uint32_t)b.size();
    memcpy(b.data(), &size, 4);
    return b;
}

inline void FakeSdkServer::HandleSend(FakeStream& stream,
                                      const uint8_t* data, size_t len)
{
    /* One writer at a time: parser_ and events are shared
       state. Lane threads serialize through the client's
       send mutex today, but locking here keeps the fake
       correct even if a caller reaches it without that
       ordering (and pairs with the snapshot accessors). */
    std::lock_guard<std::mutex> g(mutex_);
    std::vector<Packet> requests;
    if(!parser_.Feed(data, len, requests))
    {
        /* Malformed client stream: the real backend would drop
           the connection. */
        stream.SetPeerClosed(true);
        parser_.Reset();
        return;
    }

    const auto reply = [this](FakeStream& stream, uint32_t dev_id,
                              uint32_t pkt_id,
                              std::vector<uint8_t> body)
    {
        std::vector<uint8_t> pkt =
            openrgb2::sdk::EncodePacket(dev_id, pkt_id, body);
        if(corrupt_reply_magic)
        {
            pkt[0] = 'X';
        }
        stream.InjectServerBytes(std::move(pkt));
    };

    for(const Packet& req : requests)
    {
        switch(req.header.pkt_id)
        {
        case openrgb2::sdk::kPktProtocolVersion:
        {
            std::vector<uint8_t> ver(4);
            memcpy(ver.data(), &reported_version, 4);
            reply(stream, 0, openrgb2::sdk::kPktProtocolVersion,
                  std::move(ver));
            break;
        }
        case openrgb2::sdk::kPktControllerCount:
        {
            std::vector<uint8_t> cnt(4);
            const uint32_t count = (uint32_t)controllers.size();
            memcpy(cnt.data(), &count, 4);
            reply(stream, 0, openrgb2::sdk::kPktControllerCount,
                  std::move(cnt));
            break;
        }
        case openrgb2::sdk::kPktControllerData:
        {
            if(req.header.dev_id < controllers.size())
            {
                reply(stream, req.header.dev_id,
                      openrgb2::sdk::kPktControllerData,
                      BuildControllerBody(
                          controllers[req.header.dev_id]));
            }
            break;
        }
        case openrgb2::sdk::kPktUpdateMode:
        {
            /* The real backend applies the replayed mode
               description: active_mode moves and the patched
               fields (speed/direction/colors) stick — the
               playback task's RefreshStatus acknowledgement
               reads exactly these back. */
            events.push_back(ServerEvent{ req.header.dev_id,
                                          req.header.pkt_id,
                                          req.body });
            if(req.header.dev_id < controllers.size() &&
               req.body.size() >= 8)
            {
                int32_t mode_index = 0;
                memcpy(&mode_index, req.body.data() + 4, 4);
                FakeController& c = controllers[req.header.dev_id];
                if(mode_index >= 0 &&
                   (size_t)mode_index < c.modes.size())
                {
                    c.active_mode = (uint32_t)mode_index;
                    detail::ApplyModeBlob(req.body.data() + 8,
                                          req.body.size() - 8,
                                          c.modes[(size_t)mode_index]);
                }
            }
            break;
        }
        case openrgb2::sdk::kPktSetCustomMode:
        {
            /* SETCUSTOMMODE switches the controller to its
               direct mode — the mode named "Direct" here
               (matches the real wireless controller's naming;
               index 0 is the custom mode on OpenRGB devices). */
            events.push_back(ServerEvent{ req.header.dev_id,
                                          req.header.pkt_id,
                                          req.body });
            if(req.header.dev_id < controllers.size())
            {
                FakeController& c = controllers[req.header.dev_id];
                uint32_t direct = 0;
                for(size_t i = 0; i < c.modes.size(); ++i)
                {
                    if(c.modes[i].name == "Direct")
                    {
                        direct = (uint32_t)i;
                        break;
                    }
                }
                c.active_mode = direct;
            }
            break;
        }
        case openrgb2::sdk::kPktResizeZone:
        {
            /* RESIZEZONE body = i32 zone_index + i32 new_size.
               Apply like the real backend: clamp into the
               zone's [leds_min, leds_max] window, and keep the
               colors vector sized to the new total so the next
               enumeration is self-consistent. */
            events.push_back(ServerEvent{ req.header.dev_id,
                                          req.header.pkt_id,
                                          req.body });
            if(req.header.dev_id < controllers.size() &&
               req.body.size() >= 8)
            {
                int32_t zone_index = 0, new_size = 0;
                memcpy(&zone_index, req.body.data(), 4);
                memcpy(&new_size,   req.body.data() + 4, 4);
                FakeController& c = controllers[req.header.dev_id];
                if(zone_index >= 0 &&
                   (size_t)zone_index < c.zones.size() &&
                   new_size >= 0)
                {
                    FakeZone& z = c.zones[(size_t)zone_index];
                    uint32_t sz = (uint32_t)new_size;
                    if(sz < z.leds_min) { sz = z.leds_min; }
                    if(sz > z.leds_max) { sz = z.leds_max; }
                    z.leds_count = sz;
                    uint32_t total = 0;
                    for(const FakeZone& zz : c.zones)
                    {
                        total += zz.leds_count;
                    }
                    c.colors.resize(total, 0x00010101u);
                    c.led_names.resize(total);
                }
            }
            break;
        }
        default:
            events.push_back(ServerEvent{ req.header.dev_id,
                                          req.header.pkt_id,
                                          req.body });
            break;
        }
    }
}

} /* namespace sdk_test */
