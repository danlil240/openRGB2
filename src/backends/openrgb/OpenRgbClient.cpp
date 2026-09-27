/*---------------------------------------------------------*\
||| OpenRgbClient.cpp                                         |
|||                                                           |
|||   TCP SDK client for the vendored OpenRGB backend.       ||
|||   See OpenRgbClient.h.                                   ||
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "OpenRgbClient.h"

#include <cstring>
#include <stdexcept>
#include <utility>

namespace openrgb2
{
namespace sdk
{

struct OpenRgbClient::TransportError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

namespace
{
    /* DeviceKey identity (plan section 4): stable vendor:name
       device identity plus serial/location disambiguation.
       Enumeration indices are NEVER part of the key — they are
       re-resolved on every connect. */
    DeviceKey MakeKey(const SdkController& c)
    {
        DeviceKey key;
        key.device   = c.vendor.empty() ? c.name : c.vendor + ":" + c.name;
        key.serial   = c.serial;
        key.location = c.location;
        key.zone.clear();                /* controller-level key */
        return key;
    }

    uint32_t PackColor(const RgbColor& c)
    {
        return uint32_t(c.r) | (uint32_t(c.g) << 8) | (uint32_t(c.b) << 16);
    }
}

OpenRgbClient::OpenRgbClient(std::unique_ptr<IStreamTransport> transport)
    : stream_(std::move(transport))
{
    stats_.provenance = MeasurementProvenance::TransportMeasured;
}

OpenRgbClient::OpenRgbClient(OpenRgbClientOptions options,
                             std::unique_ptr<IStreamTransport> transport)
    : options_(std::move(options)), stream_(std::move(transport))
{
    stats_.provenance = MeasurementProvenance::TransportMeasured;
}

OpenRgbClient::~OpenRgbClient()
{
    Disconnect();
}

void OpenRgbClient::SetOptions(OpenRgbClientOptions options)
{
    std::lock_guard<std::mutex> lock(mutex_);
    options_ = std::move(options);
}

ConnectionState OpenRgbClient::State() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

uint32_t OpenRgbClient::ProtocolVersion() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return protocol_version_;
}

std::string OpenRgbClient::LastError() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

uint32_t OpenRgbClient::ReconnectAttempt() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return reconnect_attempt_;
}

TimePoint OpenRgbClient::NextAttemptAt() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return next_attempt_;
}

/*---------------------------------------------------------*\
||| Transport                                                 |
\*---------------------------------------------------------*/

void OpenRgbClient::SendPacket(uint32_t dev_id, uint32_t pkt_id,
                               const std::vector<uint8_t>& body)
{
    const std::vector<uint8_t> bytes = EncodePacket(dev_id, pkt_id, body);
    std::string err;
    if(!stream_->SendAll(bytes.data(), bytes.size(), err))
    {
        throw TransportError(err);
    }
}

bool OpenRgbClient::RecvPacket(Packet& pkt, std::string& err)
{
    std::string io_err;
    const RecvResult hr =
        stream_->RecvExact(&pkt.header, sizeof(pkt.header), io_err);
    if(hr != RecvResult::Ok)
    {
        if(hr == RecvResult::Error)
        {
            throw TransportError(io_err);
        }
        err = (hr == RecvResult::Closed) ? "peer closed"
                                        : "recv timed out";
        return false;
    }
    if(memcmp(pkt.header.magic, "ORGB", 4) != 0)
    {
        throw TransportError("lost packet synchronization (bad magic)");
    }
    if(pkt.header.size > kMaxPacketSize)
    {
        throw TransportError("oversized packet from server");
    }
    pkt.body.resize(pkt.header.size);
    if(pkt.header.size > 0)
    {
        const RecvResult br =
            stream_->RecvExact(pkt.body.data(), pkt.body.size(), io_err);
        if(br != RecvResult::Ok)
        {
            if(br == RecvResult::Error)
            {
                throw TransportError(io_err);
            }
            err = (br == RecvResult::Closed) ? "peer closed"
                                            : "recv timed out";
            return false;
        }
    }
    else
    {
        pkt.body.clear();
    }
    return true;
}

std::vector<uint8_t> OpenRgbClient::Request(uint32_t dev_id, uint32_t pkt_id,
                                            const std::vector<uint8_t>& body,
                                            uint32_t expected_reply_id)
{
    SendPacket(dev_id, pkt_id, body);

    /* Reply matching: the expected reply has the same pkt_id and
       echoes the requested dev_id. Unexpected async packets are
       dropped (bounded — a flooding peer cannot starve us). */
    Packet pkt;
    for(unsigned i = 0; i < 64; ++i)
    {
        std::string err;
        if(!RecvPacket(pkt, err))
        {
            throw TransportError("waiting for reply to packet " +
                                 std::to_string(pkt_id) + ": " + err);
        }
        if(pkt.header.pkt_id == expected_reply_id &&
           pkt.header.dev_id == dev_id)
        {
            return pkt.body;
        }
    }
    throw TransportError("too many unrelated packets waiting for reply " +
                         std::to_string(pkt_id));
}

void OpenRgbClient::DrainPending()
{
    /* Consume unsolicited packets so the socket never backs up.
       Below protocol 6 the server sends none; this is defensive. */
    Packet pkt;
    for(;;)
    {
        bool        readable = false;
        std::string err;
        if(!stream_->WaitReadable(0, readable, err) || !readable)
        {
            return;
        }
        if(!RecvPacket(pkt, err))
        {
            return;
        }
    }
}

void OpenRgbClient::Handshake()
{
    /* Version negotiation: offer our cap; the backend replies with
       ITS version (6 on the vendored fork). Negotiated =
       min(reply, kProtocolCap) = 4. */
    const std::vector<uint8_t> reply =
        Request(0, kPktProtocolVersion,
                BuildProtocolVersionRequest(kProtocolCap),
                kPktProtocolVersion);
    if(reply.size() != 4)
    {
        throw TransportError("malformed protocol version reply");
    }
    uint32_t server_version = 0;
    memcpy(&server_version, reply.data(), 4);
    protocol_version_ = (server_version > kProtocolCap) ? kProtocolCap
                                                        : server_version;
    if(protocol_version_ == 0)
    {
        throw TransportError("backend does not support a usable "
                             "protocol version");
    }

    SendPacket(0, kPktSetClientName,
               BuildClientNameRequest(options_.client_name));
    DrainPending();
}

/*---------------------------------------------------------*\
||| Lifecycle                                                 |
\*---------------------------------------------------------*/

bool OpenRgbClient::EnumerateLocked(std::string& err)
{
    const std::vector<uint8_t> count_reply =
        Request(0, kPktControllerCount, {}, kPktControllerCount);
    if(count_reply.size() != 4)
    {
        err = "malformed controller count reply";
        return false;
    }
    uint32_t count = 0;
    memcpy(&count, count_reply.data(), 4);

    std::map<DeviceKey, DeviceRecord> devices;
    for(uint32_t i = 0; i < count; ++i)
    {
        const std::vector<uint8_t> data =
            Request(i, kPktControllerData,
                    BuildControllerDataRequest(protocol_version_),
                    kPktControllerData);
        SdkController c;
        if(!ParseControllerData(data.data(), data.size(),
                                protocol_version_, c))
        {
            err = "malformed controller data for device " +
                  std::to_string(i);
            return false;
        }

        DeviceRecord rec;
        rec.index               = i;
        rec.snap.key            = MakeKey(c);
        rec.snap.topology_generation = 0;      /* stamped below */
        rec.snap.name           = c.name;
        rec.snap.vendor         = c.vendor;
        rec.snap.device_type    = (int32_t)c.type;
        rec.snap.active_mode    = (int32_t)c.active_mode;
        rec.snap.connection     = ConnectionState::Connected;
        rec.modes               = c.modes;
        for(const SdkMode& m : c.modes)
        {
            rec.snap.modes.push_back(m.name);
        }
        for(const SdkZone& z : c.zones)
        {
            ZoneSnapshot zs;
            zs.name      = z.name;
            zs.led_count = z.leds_count;
            rec.snap.zones.push_back(std::move(zs));
        }
        /* Last-enumerated device colors — the device mapper seeds
           complete-controller frames from them so unmapped LEDs
           keep their state (plan section 4). */
        rec.snap.colors.reserve(c.colors.size());
        for(const uint32_t packed : c.colors)
        {
            rec.snap.colors.push_back(FromSceneColor(packed));
        }

        /* Identical name+serial+location is genuinely ambiguous
           hardware — the first enumeration wins deterministically. */
        devices.emplace(rec.snap.key, std::move(rec));
    }

    /* Every enumeration is a topology generation: frames stamped
       with an older generation are obsolete. */
    ++topology_generation_;
    for(auto& entry : devices)
    {
        entry.second.snap.topology_generation = topology_generation_;
    }
    devices_ = std::move(devices);
    return true;
}

bool OpenRgbClient::ConnectLocked(std::string& err)
{
    stream_->Close();
    protocol_version_ = 0;

    try
    {
        if(!stream_->Connect(options_.host, options_.port,
                            options_.connect_timeout_ms,
                            options_.io_timeout_ms, err))
        {
            return false;
        }
        Handshake();
        if(!EnumerateLocked(err))
        {
            stream_->Close();
            return false;
        }
    }
    catch(const TransportError& e)
    {
        stream_->Close();
        err = e.what();
        return false;
    }

    state_             = ConnectionState::Connected;
    reconnect_attempt_ = 0;
    last_error_.clear();
    return true;
}

bool OpenRgbClient::Connect()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::string err;
    if(!ConnectLocked(err))
    {
        state_      = ConnectionState::Disconnected;
        last_error_ = err;
        return false;
    }
    return true;
}

void OpenRgbClient::Disconnect()
{
    std::lock_guard<std::mutex> lock(mutex_);
    stream_->Close();
    protocol_version_   = 0;
    state_              = ConnectionState::Disconnected;
    reconnect_attempt_  = 0;
}

bool OpenRgbClient::Enumerate()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if(state_ != ConnectionState::Connected)
    {
        last_error_ = "not connected";
        return false;
    }
    std::string err;
    try
    {
        if(!EnumerateLocked(err))
        {
            last_error_ = err;
            return false;
        }
    }
    catch(const TransportError& e)
    {
        MarkBrokenLocked(e.what(), Clock::now());
        return false;
    }
    return true;
}

/*---------------------------------------------------------*\
||| Reconnect schedule                                        |
\*---------------------------------------------------------*/

bool OpenRgbClient::RefreshStatus()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if(state_ != ConnectionState::Connected)
    {
        last_error_ = "not connected";
        return false;
    }
    try
    {
        bool structural = false;
        std::vector<std::pair<DeviceKey, SdkController>> fresh;
        fresh.reserve(devices_.size());
        for(const auto& entry : devices_)
        {
            const std::vector<uint8_t> data =
                Request(entry.second.index, kPktControllerData,
                        BuildControllerDataRequest(protocol_version_),
                        kPktControllerData);
            SdkController c;
            if(!ParseControllerData(data.data(), data.size(),
                                    protocol_version_, c))
            {
                last_error_ = "malformed controller data on refresh";
                return false;
            }
            /* A different identity or a changed zone/LED layout
               at an expected index means the topology moved —
               take the full re-enumeration path so the
               generation bump invalidates stale frames. */
            if(MakeKey(c) != entry.first ||
               (int32_t)c.type != entry.second.snap.device_type ||
               c.zones.size() != entry.second.snap.zones.size())
            {
                structural = true;
            }
            else
            {
                for(size_t z = 0; z < c.zones.size(); ++z)
                {
                    if(c.zones[z].leds_count !=
                       entry.second.snap.zones[z].led_count)
                    {
                        structural = true;
                        break;
                    }
                }
            }
            fresh.emplace_back(entry.first, std::move(c));
        }
        if(structural)
        {
            /* Same path as Enumerate() — a genuine topology
               change must obsolete in-flight work. */
            std::string err;
            if(!EnumerateLocked(err))
            {
                last_error_ = err;
                return false;
            }
            return true;
        }
        /* In-place refresh: mode/parameter state and the
           last-enumerated colors only — the generation (and
           therefore every pending frame) stays valid. */
        for(auto& pair : fresh)
        {
            DeviceRecord& rec    = devices_.at(pair.first);
            SdkController& c     = pair.second;
            rec.snap.active_mode = (int32_t)c.active_mode;
            rec.modes            = c.modes;
            rec.snap.modes.clear();
            for(const SdkMode& m : c.modes)
            {
                rec.snap.modes.push_back(m.name);
            }
            rec.snap.colors.clear();
            rec.snap.colors.reserve(c.colors.size());
            for(const uint32_t packed : c.colors)
            {
                rec.snap.colors.push_back(FromSceneColor(packed));
            }
        }
        DrainPending();
    }
    catch(const TransportError& e)
    {
        MarkBrokenLocked(e.what(), Clock::now());
        return false;
    }
    return true;
}

bool OpenRgbClient::ActiveMode(const DeviceKey& controller,
                               int32_t& mode_index,
                               SdkMode* mode) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = devices_.find(controller);
    if(it == devices_.end())
    {
        return false;
    }
    mode_index = it->second.snap.active_mode;
    if(mode && mode_index >= 0 &&
       (size_t)mode_index < it->second.modes.size())
    {
        SdkMode m = it->second.modes[(size_t)mode_index];
        m.raw.clear();       /* callers get parsed fields only */
        *mode = std::move(m);
    }
    return true;
}

void OpenRgbClient::MarkBrokenLocked(const std::string& reason,
                                     TimePoint now)
{
    stream_->Close();
    protocol_version_  = 0;
    last_error_        = reason;
    state_             = ConnectionState::Reconnecting;
    reconnect_attempt_ = 0;
    next_attempt_      = now + options_.backoff.DelayFor(0);
}

void OpenRgbClient::PollLocked(TimePoint now)
{
    if(state_ == ConnectionState::Connected)
    {
        if(stream_->IsOpen() && stream_->PeerClosed())
        {
            MarkBrokenLocked("peer closed connection", now);
        }
        else
        {
            return;
        }
    }

    if(state_ != ConnectionState::Reconnecting)
    {
        return;
    }
    if(reconnect_attempt_ >= options_.backoff.max_attempts)
    {
        /* Bounded: give up; snapshots keep reporting Disconnected
           until an explicit Connect()/ReconnectNow(). */
        state_ = ConnectionState::Disconnected;
        return;
    }
    if(now < next_attempt_)
    {
        return;
    }

    std::string err;
    if(!ConnectLocked(err))
    {
        ++reconnect_attempt_;
        next_attempt_ = now + options_.backoff.DelayFor(reconnect_attempt_);
        last_error_   = err;
    }
}

bool OpenRgbClient::Poll(TimePoint now)
{
    std::lock_guard<std::mutex> lock(mutex_);
    PollLocked(now);
    return state_ == ConnectionState::Connected;
}

bool OpenRgbClient::ReconnectNow()
{
    std::lock_guard<std::mutex> lock(mutex_);
    const TimePoint now = Clock::now();
    stream_->Close();
    state_             = ConnectionState::Reconnecting;
    reconnect_attempt_ = 0;
    next_attempt_      = now;
    PollLocked(now);
    return state_ == ConnectionState::Connected;
}

/*---------------------------------------------------------*\
||| Topology                                                  |
\*---------------------------------------------------------*/

std::vector<DeviceSnapshot> OpenRgbClient::Snapshots() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DeviceSnapshot> out;
    out.reserve(devices_.size());
    for(const auto& entry : devices_)
    {
        DeviceSnapshot snap  = entry.second.snap;
        snap.connection      = state_;
        out.push_back(std::move(snap));
    }
    return out;
}

bool OpenRgbClient::Snapshot(const DeviceKey& controller,
                             DeviceSnapshot& out) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = devices_.find(controller);
    if(it == devices_.end())
    {
        return false;
    }
    out            = it->second.snap;
    out.connection = state_;
    return true;
}

/*---------------------------------------------------------*\
||| IFrameSink — the direct-mode send path                    |
\*---------------------------------------------------------*/

SubmitResult OpenRgbClient::SubmitLatest(const RgbFrame& frame)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const TimePoint now = Clock::now();
    PollLocked(now);                      /* drive scheduled retries */

    const auto dev = devices_.find(frame.controller);
    if(dev == devices_.end())
    {
        ++stats_.rejected;
        return SubmitResult::Reject("unknown controller");
    }
    if(state_ != ConnectionState::Connected)
    {
        ++stats_.rejected;
        return SubmitResult::Reject("backend not connected");
    }
    if(frame.topology_generation != topology_generation_)
    {
        ++stats_.rejected;
        return SubmitResult::Reject("stale topology generation");
    }
    const uint32_t led_count = dev->second.snap.LedCount();
    if(frame.colors.size() != led_count || frame.colors.empty())
    {
        ++stats_.rejected;
        return SubmitResult::Reject("LED count mismatch");
    }

    /* Complete-controller write: one UPDATELEDS packet covering
       every LED — three zone edits must never become three
       uploads (plan section 4). */
    std::vector<uint32_t> colors;
    colors.reserve(frame.colors.size());
    for(const RgbColor& c : frame.colors)
    {
        colors.push_back(PackColor(c));
    }

    try
    {
        SendPacket(dev->second.index, kPktUpdateLEDs,
                   BuildUpdateLEDsBody(colors));
    }
    catch(const TransportError& e)
    {
        ++stats_.rejected;
        MarkBrokenLocked(e.what(), now);
        return SubmitResult::Reject(std::string("transport error: ") +
                                    e.what());
    }

    ++stats_.submitted;
    /* Transport-completed = handed to the SDK socket, never
       hardware confirmation. */
    stats_.RecordTransportCompletion(now);
    return SubmitResult::Accept();
}

DeliveryStats OpenRgbClient::Stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

/*---------------------------------------------------------*\
||| Mode / resize commands                                    |
\*---------------------------------------------------------*/

bool OpenRgbClient::SetCustomMode(const DeviceKey& controller)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto dev = devices_.find(controller);
    if(dev == devices_.end() || state_ != ConnectionState::Connected)
    {
        last_error_ = "controller unavailable";
        return false;
    }
    try
    {
        SendPacket(dev->second.index, kPktSetCustomMode, {});
        DrainPending();
    }
    catch(const TransportError& e)
    {
        MarkBrokenLocked(e.what(), Clock::now());
        return false;
    }
    return true;
}

bool OpenRgbClient::SelectMode(const DeviceKey& controller,
                               const std::string& name, int32_t speed)
{
    ModePatch patch;
    if(speed >= 0)
    {
        patch.has_speed = true;
        patch.speed     = (uint32_t)speed;
    }
    return SelectMode(controller, name, patch);
}

bool OpenRgbClient::SelectMode(const DeviceKey& controller,
                               const std::string& name,
                               const ModePatch& patch)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto dev = devices_.find(controller);
    if(dev == devices_.end() || state_ != ConnectionState::Connected)
    {
        last_error_ = "controller unavailable";
        return false;
    }

    /* Resolve the mode by NAME from enumeration — never by
       hard-coded array index (plan section 4). */
    int32_t mode_index = -1;
    for(size_t i = 0; i < dev->second.modes.size(); ++i)
    {
        if(dev->second.modes[i].name == name)
        {
            mode_index = (int32_t)i;
            break;
        }
    }
    if(mode_index < 0)
    {
        last_error_ = "unsupported mode '" + name + "'";
        return false;
    }

    std::vector<uint8_t> raw = dev->second.modes[mode_index].raw;
    if(!PatchModeParams(raw, protocol_version_, patch))
    {
        last_error_ = "cannot patch mode parameters";
        return false;
    }

    try
    {
        SendPacket(dev->second.index, kPktUpdateMode,
                   BuildUpdateModeBody(mode_index, raw));
        DrainPending();
    }
    catch(const TransportError& e)
    {
        MarkBrokenLocked(e.what(), Clock::now());
        return false;
    }
    /* snap.active_mode is deliberately NOT patched here: it is
       the last server-reported value (Enumerate/RefreshStatus),
       which is the playback task's honest applied-mode
       acknowledgement — an optimistic write would fabricate a
       confirmation the backend never sent. */
    return true;
}

bool OpenRgbClient::ResizeZone(const DeviceKey& controller,
                               const std::string& zone_name,
                               int32_t new_size)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto dev = devices_.find(controller);
    if(dev == devices_.end() || state_ != ConnectionState::Connected)
    {
        last_error_ = "controller unavailable";
        return false;
    }

    int32_t zone_index = -1;
    for(size_t i = 0; i < dev->second.snap.zones.size(); ++i)
    {
        if(dev->second.snap.zones[i].name == zone_name)
        {
            zone_index = (int32_t)i;
            break;
        }
    }
    if(zone_index < 0)
    {
        last_error_ = "unknown zone '" + zone_name + "'";
        return false;
    }
    if(new_size < 0)
    {
        last_error_ = "invalid zone size";
        return false;
    }

    try
    {
        SendPacket(dev->second.index, kPktResizeZone,
                   BuildResizeZoneBody(zone_index, new_size));
        DrainPending();
    }
    catch(const TransportError& e)
    {
        MarkBrokenLocked(e.what(), Clock::now());
        return false;
    }

    /* The zone's LED count changed on the device: every frame
       assembled against the old topology is obsolete (plan
       section 6 — stop obsolete frames on topology change).
       Bump the generation WITHOUT re-stamping the snapshots:
       snapshots keep reporting their pre-change generation, so
       every subsequent SubmitLatest rejects as stale until the
       caller's Enumerate() refreshes zones and colors. */
    ++topology_generation_;
    return true;
}

} /* namespace sdk */
} /* namespace openrgb2 */
