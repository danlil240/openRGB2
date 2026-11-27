/*---------------------------------------------------------*\
|||| OpenRgbClient.cpp                                         |
||||                                                           |
||||   TCP SDK client for the vendored OpenRGB backend.       ||
||||   See OpenRgbClient.h for the two-mutex locking          ||
||||   contract (io_mutex_ serializes the wire; mutex_ only  ||
||||   ever guards short state reads/commits — lock order     ||
||||   io_mutex_ -> mutex_, never the reverse).               ||
||||                                                           |
||||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#include "OpenRgbClient.h"

#include <chrono>
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

ConnectionSnapshotInfo OpenRgbClient::ConnectionSnapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    ConnectionSnapshotInfo out;
    out.state             = state_;
    out.protocol_version  = protocol_version_;
    out.reconnect_attempt = reconnect_attempt_;
    out.last_error        = last_error_;
    return out;
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
|||| Transport — every helper below runs ONLY under          ||
|||| io_mutex_.                                              ||
|\*---------------------------------------------------------*/

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

RecvResult OpenRgbClient::RecvPacket(Packet& pkt)
{
    /* Typed outcome: Ok, or a CLEAN Closed/Timeout — clean means
       no byte of this packet was consumed (the transport's
       RecvExact contract: a timeout after a partial read is an
       Error). A timeout after the header arrived would leave
       the rest of the body in the stream and desynchronize
       every later reply, so it is a transport error here. */
    std::string io_err;
    const RecvResult hr =
        stream_->RecvExact(&pkt.header, sizeof(pkt.header), io_err);
    if(hr != RecvResult::Ok)
    {
        if(hr == RecvResult::Error)
        {
            throw TransportError(io_err);
        }
        return hr;
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
        if(br == RecvResult::Error)
        {
            throw TransportError(io_err);
        }
        if(br == RecvResult::Closed)
        {
            return br;      /* the caller tears the wire down */
        }
        if(br == RecvResult::Timeout)
        {
            throw TransportError("recv timed out mid-packet");
        }
    }
    else
    {
        pkt.body.clear();
    }
    if(pkt.header.pkt_id == kPktLianLiWirelessStatus)
    {
        NoteWirelessReplyIo();
    }
    return RecvResult::Ok;
}

namespace
{
    const char* RecvResultText(RecvResult r)
    {
        return (r == RecvResult::Closed) ? "peer closed"
                                         : "recv timed out";
    }
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
        const RecvResult r = RecvPacket(pkt);
        if(r != RecvResult::Ok)
        {
            throw TransportError("waiting for reply to packet " +
                                 std::to_string(pkt_id) + ": " +
                                 RecvResultText(r));
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
        const RecvResult r = RecvPacket(pkt);
        if(r == RecvResult::Timeout)
        {
            /* Readable, yet no complete header within the io
               timeout: a torn header is stuck in the stream. */
            throw TransportError("recv timed out mid-packet");
        }
        if(r != RecvResult::Ok)
        {
            return;
        }
    }
}

void OpenRgbClient::Handshake(const OpenRgbClientOptions& opts)
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
    const uint32_t negotiated =
        (server_version > kProtocolCap) ? kProtocolCap : server_version;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        protocol_version_ = negotiated;
    }
    if(negotiated == 0)
    {
        throw TransportError("backend does not support a usable "
                             "protocol version");
    }

    SendPacket(0, kPktSetClientName,
               BuildClientNameRequest(opts.client_name));
    DrainPending();
}

/*---------------------------------------------------------*\
|||| Lifecycle                                               ||
|\*---------------------------------------------------------*/

bool OpenRgbClient::EnumerateIo(std::string& err)
{
    uint32_t pv = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pv = protocol_version_;
    }

    const std::vector<uint8_t> count_reply =
        Request(0, kPktControllerCount, {}, kPktControllerCount);
    if(count_reply.size() != 4)
    {
        err = "malformed controller count reply";
        return false;
    }
    uint32_t count = 0;
    memcpy(&count, count_reply.data(), 4);

    /* Built entirely on the side; the committed map swaps in
       atomically under mutex_ — a state reader never observes
       a half-enumerated device list. */
    std::map<DeviceKey, DeviceRecord> devices;
    for(uint32_t i = 0; i < count; ++i)
    {
        const std::vector<uint8_t> data =
            Request(i, kPktControllerData,
                    BuildControllerDataRequest(pv),
                    kPktControllerData);
        SdkController c;
        if(!ParseControllerData(data.data(), data.size(),
                                pv, c))
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
            zs.name        = z.name;
            zs.led_count   = z.leds_count;
            zs.leds_min    = z.leds_min;
            zs.leds_max    = z.leds_max;
            zs.matrix_rows = z.matrix_rows;
            zs.matrix_cols = z.matrix_cols;
            zs.matrix_map  = z.matrix_map;
            rec.snap.zones.push_back(std::move(zs));
        }
        rec.snap.led_names = c.led_names;
        /* Last-enumerated device colors — the device mapper seeds
           complete-controller frames from them so unmapped LEDs
           keep their state (plan section 4). */
        rec.snap.colors.reserve(c.colors.size());
        for(const uint32_t packed : c.colors)
        {
            rec.snap.colors.push_back(FromSceneColor(packed));
        }

        /* Identical name+serial+location is genuinely ambiguous
           hardware — the first enumeration wins deterministically.
           The dropped duplicate still counts toward the server's
           controller count (server_controller_count_ below), so
           RefreshStatus compares like with like. */
        devices.emplace(rec.snap.key, std::move(rec));
    }

    /* Every enumeration is a topology generation: frames stamped
       with an older generation are obsolete. The generation bump
       and the map swap commit in the SAME state-lock hold so a
       reader can never pair the new generation with the old map
       (or the reverse). */
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++topology_generation_;
        for(auto& entry : devices)
        {
            entry.second.snap.topology_generation = topology_generation_;
        }
        devices_ = std::move(devices);
        server_controller_count_ = count;
    }
    /* Probe the wireless-status extension on every enumeration:
       covers Connect(), Enumerate(), and the structural-
       refresh path. An enumeration always re-probes, even a
       latched-unsupported backend (a fork that was merely slow
       on the first probe recovers here); the probe has its own
       short deadline. false means the wire broke mid-probe
       (already marked). */
    if(!FetchWirelessStatusIo(/*force=*/true))
    {
        err = "wireless status probe broke the connection";
        return false;
    }
    return true;
}

bool OpenRgbClient::ConnectIo(std::string& err)
{
    /* One options snapshot per wire operation — SetOptions()
       can never flip timeouts mid-sequence (the old lock gave
       the same guarantee by serializing the whole call). */
    OpenRgbClientOptions opts;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        opts = options_;
        protocol_version_  = 0;
        /* A (re)connect may reach a different backend — the
           extension probe starts over. */
        wireless_status_   = WirelessStatusSnapshot{};
        wireless_next_probe_ = TimePoint{};
    }
    /* A fresh stream carries no unanswered probe replies. */
    wireless_pending_ = 0;
    stream_->Close();

    try
    {
        if(!stream_->Connect(opts.host, opts.port,
                            opts.connect_timeout_ms,
                            opts.io_timeout_ms, err))
        {
            return false;
        }
        Handshake(opts);
        if(!EnumerateIo(err))
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

    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_             = ConnectionState::Connected;
        reconnect_attempt_ = 0;
        last_error_.clear();
    }
    return true;
}

bool OpenRgbClient::Connect()
{
    std::lock_guard<std::mutex> io(io_mutex_);
    std::string err;
    if(!ConnectIo(err))
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_      = ConnectionState::Disconnected;
        last_error_ = err;
        return false;
    }
    return true;
}

void OpenRgbClient::Disconnect()
{
    /* Both locks, io order: the close is atomic against any
       in-flight wire operation (a reconnect that already
       started finishes before the socket is pulled). */
    std::lock_guard<std::mutex> io(io_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    stream_->Close();
    protocol_version_  = 0;
    state_             = ConnectionState::Disconnected;
    reconnect_attempt_ = 0;
}

bool OpenRgbClient::Enumerate()
{
    std::lock_guard<std::mutex> io(io_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(state_ != ConnectionState::Connected)
        {
            last_error_ = "not connected";
            return false;
        }
    }
    std::string err;
    try
    {
        if(!EnumerateIo(err))
        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_error_ = err;
            return false;
        }
    }
    catch(const TransportError& e)
    {
        MarkBrokenIo(e.what(), Clock::now());
        return false;
    }
    return true;
}

/*---------------------------------------------------------*\
|||| Reconnect schedule                                      ||
|\*---------------------------------------------------------*/

bool OpenRgbClient::RefreshStatus()
{
    std::lock_guard<std::mutex> io(io_mutex_);

    /* Snapshot everything the wire phase needs — the io_mutex_
       hold means no other thread can commit a topology change
       mid-refresh, so this view stays consistent for the whole
       call. */
    struct KnownDevice
    {
        DeviceKey                 key;
        uint32_t                  index;
        int32_t                   device_type;
        std::vector<ZoneSnapshot> zones;
    };
    uint32_t                pv = 0;
    uint32_t                server_count = 0;
    std::vector<KnownDevice> known;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(state_ != ConnectionState::Connected)
        {
            last_error_ = "not connected";
            return false;
        }
        pv = protocol_version_;
        server_count = server_controller_count_;
        known.reserve(devices_.size());
        for(const auto& entry : devices_)
        {
            known.push_back(KnownDevice{ entry.first,
                                         entry.second.index,
                                         entry.second.snap.device_type,
                                         entry.second.snap.zones });
        }
    }

    try
    {
        /* A device added or removed changes the server-side
           count — the per-device refresh below iterates only
           the KNOWN map, so it can never see an addition and
           sees a removal only indirectly through a shifted
           index. Re-fetch the count first: any difference
           takes the full re-enumeration path (generation bump
           obsoletes stale frames + remaps positional ids).
           Compared against the SERVER count recorded at
           enumeration, not known.size(): duplicate-key
           controllers collapse in the map, and comparing with
           the map size would re-enumerate on every refresh. */
        const std::vector<uint8_t> count_reply =
            Request(0, kPktControllerCount, {},
                    kPktControllerCount);
        if(count_reply.size() != 4)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_error_ = "malformed controller count reply";
            return false;
        }
        uint32_t count = 0;
        memcpy(&count, count_reply.data(), 4);
        if(count != server_count)
        {
            std::string err;
            if(!EnumerateIo(err))
            {
                std::lock_guard<std::mutex> lock(mutex_);
                last_error_ = err;
                return false;
            }
            return true;
        }

        bool structural = false;
        std::vector<std::pair<DeviceKey, SdkController>> fresh;
        fresh.reserve(known.size());
        for(const KnownDevice& kd : known)
        {
            const std::vector<uint8_t> data =
                Request(kd.index, kPktControllerData,
                        BuildControllerDataRequest(pv),
                        kPktControllerData);
            SdkController c;
            if(!ParseControllerData(data.data(), data.size(),
                                    pv, c))
            {
                std::lock_guard<std::mutex> lock(mutex_);
                last_error_ = "malformed controller data on refresh";
                return false;
            }
            /* A different identity or a changed zone/LED layout
               at an expected index means the topology moved —
               take the full re-enumeration path so the
               generation bump invalidates stale frames. */
            if(MakeKey(c) != kd.key ||
               (int32_t)c.type != kd.device_type ||
               c.zones.size() != kd.zones.size())
            {
                structural = true;
            }
            else
            {
                for(size_t z = 0; z < c.zones.size(); ++z)
                {
                    if(c.zones[z].leds_count !=
                       kd.zones[z].led_count ||
                       c.zones[z].matrix_rows !=
                       kd.zones[z].matrix_rows ||
                       c.zones[z].matrix_cols !=
                       kd.zones[z].matrix_cols ||
                       c.zones[z].matrix_map !=
                       kd.zones[z].matrix_map)
                    {
                        structural = true;
                        break;
                    }
                }
            }
            fresh.emplace_back(kd.key, std::move(c));
        }
        if(structural)
        {
            /* Same path as Enumerate() — a genuine topology
               change must obsolete in-flight work. */
            std::string err;
            if(!EnumerateIo(err))
            {
                std::lock_guard<std::mutex> lock(mutex_);
                last_error_ = err;
                return false;
            }
            return true;
        }
        /* In-place refresh: mode/parameter state and the
           last-enumerated colors only — the generation (and
           therefore every pending frame) stays valid. Committed
           in one state-lock hold. */
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for(auto& pair : fresh)
            {
                const auto it = devices_.find(pair.first);
                if(it == devices_.end())
                {
                    continue;    /* defensive — devices_ cannot
                                    change under the io hold  */
                }
                DeviceRecord& rec    = it->second;
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
        }
        DrainPending();
        /* Keep the wireless-status cache live on the refresh
           cadence — the N01 wedge surface ages in seconds. */
        if(!FetchWirelessStatusIo(/*force=*/false))
        {
            return false;
        }
    }
    catch(const TransportError& e)
    {
        MarkBrokenIo(e.what(), Clock::now());
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

/*---------------------------------------------------------*\
|| Wireless status — the lianli-wireless fork extension     ||
|| (N01 observability).                                     ||
\*---------------------------------------------------------*/

void OpenRgbClient::NoteWirelessReplyIo()
{
    /* Every packet-4000 reply passes through RecvPacket, in
       order. Replies to probes that timed out arrive late and
       are counted off here — wherever they land (inside a
       later probe, a Request() reply wait, DrainPending) — so
       a probe never mistakes an older probe's reply for its
       own (the stream stays synchronized). */
    if(wireless_pending_ > 0)
    {
        --wireless_pending_;
    }
    /* Any reply proves the backend speaks the extension: a
       fork that was merely slow on the first probe must not
       stay latched unsupported for the whole connection. The
       next refresh re-probes it. */
    std::lock_guard<std::mutex> lock(mutex_);
    if(wireless_status_.probed && !wireless_status_.supported)
    {
        wireless_status_.supported = true;
        wireless_next_probe_       = TimePoint{};
    }
}

bool OpenRgbClient::FetchWirelessStatusIo(bool force, bool* fresh)
{
    /* Runs under io_mutex_ (EnumerateIo / RefreshStatus / the
       public wrappers). Gated on the stream, not state_:
       ConnectIo reaches this through EnumerateIo BEFORE the
       Connected commit. `fresh` (optional) reports whether
       THIS call received and parsed a reply — a cached
       snapshot must never be passed off as a new sample. */
    if(fresh)
    {
        *fresh = false;
    }
    if(!stream_->IsOpen())
    {
        return true;
    }
    uint32_t probe_ms   = 0;
    uint32_t reprobe_ms = 0;
    const TimePoint start = Clock::now();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        probe_ms   = options_.wireless_probe_timeout_ms;
        reprobe_ms = options_.wireless_reprobe_ms;
        if(!force && wireless_status_.probed &&
           !wireless_status_.supported &&
           start < wireless_next_probe_)
        {
            /* Latched unsupported: a stock backend drops the
               packet silently — re-probing on every refresh
               would stall the wire (io_mutex_, so every
               SubmitLatest) by the probe deadline for nothing.
               Re-probed on the slow wireless_reprobe_ms
               schedule, on every enumeration, and as soon as a
               late reply shows the backend does speak the
               extension. */
            return true;
        }
    }

    SendPacket(0, kPktLianLiWirelessStatus, {});
    ++wireless_pending_;

    /* Replies arrive in request order: this probe's reply is
       the one that brings wireless_pending_ back to zero. One
       OVERALL deadline (wireless_probe_timeout_ms, not the 4 s
       io timeout per packet). Waiting is select()-based, so a
       timeout consumes no bytes and the socket stays usable;
       once bytes are readable a whole packet is read, and a
       stall mid-packet is a transport error. A timeout is
       INFORMATION (the packet is unsupported or slow), never a
       connection failure. */
    const TimePoint deadline =
        start + std::chrono::milliseconds(probe_ms);
    Packet pkt;
    for(unsigned i = 0; i < 64; ++i)
    {
        const TimePoint now = Clock::now();
        if(now >= deadline)
        {
            break;
        }
        const uint32_t left_ms = (uint32_t)
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now).count();
        bool        readable = false;
        std::string err;
        if(!stream_->WaitReadable(left_ms, readable, err))
        {
            throw TransportError(err);
        }
        if(!readable)
        {
            break;                              /* deadline hit */
        }
        const RecvResult r = RecvPacket(pkt);
        if(r == RecvResult::Closed)
        {
            /* Closed on the wire is a real fault. */
            MarkBrokenIo("peer closed", Clock::now());
            return false;
        }
        if(r == RecvResult::Timeout)
        {
            /* Readable, yet no whole header within the io
               timeout — a torn header is stuck in the stream. */
            throw TransportError("recv timed out mid-packet");
        }
        if(pkt.header.pkt_id == kPktLianLiWirelessStatus &&
           pkt.header.dev_id == 0)
        {
            if(wireless_pending_ > 0)
            {
                continue;     /* an older probe's late reply */
            }
            SdkWirelessStatus parsed;
            bool ok = ParseWirelessStatus(
                pkt.body.data(), pkt.body.size(), parsed);
            /* {"detecting":true,...}: the backend answers promptly
               while its first detection is still running —
               supported, but no data yet. Not a sample (never
               `fresh`, never overwrites the cache); the normal
               refresh cadence asks again. */
            if(ok && parsed.raw)
            {
                const auto dit = parsed.raw->find("detecting");
                if(dit != parsed.raw->end() && dit->is_boolean() &&
                   dit->get<bool>())
                {
                    ok = false;
                }
            }
            std::lock_guard<std::mutex> lock(mutex_);
            wireless_status_.probed    = true;
            wireless_status_.supported = true;
            if(ok)
            {
                wireless_status_.status     = std::move(parsed);
                wireless_status_.fetched_at = Clock::now();
                wireless_status_.have_data  = true;
                if(fresh)
                {
                    *fresh = true;
                }
            }
            return true;
        }
        /* Unrelated/async traffic — drop and keep waiting. */
    }
    /* Deadline or 64 unrelated packets: no reply to THIS probe.
       Its reply may still arrive — wireless_pending_ keeps it
       from being read as a later probe's answer. */
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wireless_status_.probed = true;
        /* Only a never-answered probe latches unsupported; a
           timeout AFTER a successful fetch leaves the last data
           in place — it ages out via fetched_at instead of
           vanishing (and `fresh` stays false). */
        if(!wireless_status_.have_data)
        {
            wireless_status_.supported = false;
            wireless_next_probe_ =
                Clock::now() + std::chrono::milliseconds(reprobe_ms);
        }
    }
    return true;
}

bool OpenRgbClient::FetchWirelessStatus()
{
    std::lock_guard<std::mutex> io(io_mutex_);
    try
    {
        return FetchWirelessStatusIo(/*force=*/false);
    }
    catch(const TransportError& e)
    {
        MarkBrokenIo(e.what(), Clock::now());
        return false;
    }
}

void OpenRgbClient::WirelessStatus(WirelessStatusSnapshot& out) const
{
    /* Cheap under the state lock: the raw backend document is
       shared (shared_ptr<const json>), never deep-copied. */
    std::lock_guard<std::mutex> lock(mutex_);
    out = wireless_status_;
}

bool OpenRgbClient::WirelessClockSample(const DeviceKey& controller,
                                        PlaybackClockSnapshot& out)
{
    out = {};
    WirelessStatusSnapshot snap;
    {
        std::lock_guard<std::mutex> io(io_mutex_);
        bool fresh = false;
        try
        {
            if(!FetchWirelessStatusIo(/*force=*/false, &fresh))
            {
                return false;
            }
        }
        catch(const TransportError& e)
        {
            MarkBrokenIo(e.what(), Clock::now());
            return false;
        }
        /* Only a reply received by THIS call is a sample: a
           stalled backend (probe timed out, malformed reply)
           leaves the cache in place, and re-stamping that as
           current would feed old ticks to the playback clock
           as if they were new. */
        if(!fresh)
        {
            return false;
        }
        WirelessStatus(snap);
    }
    if(!snap.have_data)
    {
        return false;
    }
    for(const SdkWirelessGroupStatus& g : snap.status.groups)
    {
        /* group mac == controller serial — the join the
           backend's RGBController exposes. */
        if(g.mac != controller.serial || !g.clock.valid)
        {
            continue;
        }
        const auto age = std::chrono::milliseconds(
            (std::chrono::milliseconds::rep)g.clock.age_ms);
        out.valid      = true;
        out.ticks      = g.clock.ticks;
        /* Anchored at the reply's arrival, not at "now": the
           backend's age is relative to when it serialized. */
        out.sampled_at = snap.fetched_at - age;
        /* Backend-side age at serialization + a conservative
           transport margin — the reading is only ever older
           than the backend reported. */
        out.uncertainty_s = (double)g.clock.age_ms / 1000.0 + 0.05;
        return true;
    }
    return false;
}

void OpenRgbClient::MarkBrokenIo(const std::string& reason,
                                 TimePoint now)
{
    /* Caller holds io_mutex_ — the close cannot race another
       wire operation. The state transition commits under
       mutex_ afterwards (io -> state order). */
    stream_->Close();
    std::lock_guard<std::mutex> lock(mutex_);
    protocol_version_  = 0;
    last_error_        = reason;
    state_             = ConnectionState::Reconnecting;
    reconnect_attempt_ = 0;
    next_attempt_      = now + options_.backoff.DelayFor(0);
}

void OpenRgbClient::PollIo(TimePoint now)
{
    /* stream_->IsOpen()/PeerClosed() are wire-side probes —
       they run under io_mutex_ (held by the caller), never
       inside the mutex_ state sections. */
    bool check_schedule = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(state_ != ConnectionState::Connected)
        {
            check_schedule = true;   /* Reconnecting/Disconnected */
        }
    }
    if(!check_schedule)
    {
        if(stream_->IsOpen() && stream_->PeerClosed())
        {
            MarkBrokenIo("peer closed connection", now);
            check_schedule = true;
        }
        else
        {
            return;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(state_ != ConnectionState::Reconnecting)
        {
            return;
        }
        if(reconnect_attempt_ >= options_.backoff.max_attempts)
        {
            /* Bounded: give up; snapshots keep reporting
               Disconnected until an explicit
               Connect()/ReconnectNow(). */
            state_ = ConnectionState::Disconnected;
            return;
        }
        if(now < next_attempt_)
        {
            return;
        }
    }

    std::string err;
    if(!ConnectIo(err))
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++reconnect_attempt_;
        next_attempt_ = now + options_.backoff.DelayFor(reconnect_attempt_);
        last_error_   = err;
    }
}

bool OpenRgbClient::Poll(TimePoint now)
{
    std::lock_guard<std::mutex> io(io_mutex_);
    PollIo(now);
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == ConnectionState::Connected;
}

bool OpenRgbClient::ReconnectNow()
{
    std::lock_guard<std::mutex> io(io_mutex_);
    const TimePoint now = Clock::now();
    stream_->Close();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_             = ConnectionState::Reconnecting;
        reconnect_attempt_ = 0;
        next_attempt_      = now;
    }
    PollIo(now);
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == ConnectionState::Connected;
}

/*---------------------------------------------------------*\
|||| Topology — pure state reads (mutex_ only, never the    ||
|||| wire): the diagnostics surface stays responsive while  ||
|||| a wire operation is in flight.                         ||
|\*---------------------------------------------------------*/

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
|||| IFrameSink — the direct-mode send path                  ||
|\*---------------------------------------------------------*/

SubmitResult OpenRgbClient::SubmitLatest(const RgbFrame& frame)
{
    const TimePoint now = Clock::now();
    std::lock_guard<std::mutex> io(io_mutex_);
    PollIo(now);                       /* drive scheduled retries */

    /* Validate against committed state in one state-lock hold.
       Holding io_mutex_ means nothing can re-enumerate or
       transition the connection between this read and the
       send below — the resolved index stays valid. */
    uint32_t dev_index = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
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
        dev_index = dev->second.index;
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
        SendPacket(dev_index, kPktUpdateLEDs,
                   BuildUpdateLEDsBody(colors));
    }
    catch(const TransportError& e)
    {
        MarkBrokenIo(e.what(), now);
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.rejected;
        return SubmitResult::Reject(std::string("transport error: ") +
                                    e.what());
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.submitted;
        /* Transport-completed = handed to the SDK socket, never
           hardware confirmation. */
        stats_.RecordTransportCompletion(now);
    }
    return SubmitResult::Accept();
}

DeliveryStats OpenRgbClient::Stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

/*---------------------------------------------------------*\
|||| Mode / resize commands                                  ||
|\*---------------------------------------------------------*/

bool OpenRgbClient::SetCustomMode(const DeviceKey& controller)
{
    std::lock_guard<std::mutex> io(io_mutex_);
    uint32_t dev_index = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto dev = devices_.find(controller);
        if(dev == devices_.end() || state_ != ConnectionState::Connected)
        {
            last_error_ = "controller unavailable";
            return false;
        }
        dev_index = dev->second.index;
    }
    try
    {
        SendPacket(dev_index, kPktSetCustomMode, {});
        DrainPending();
    }
    catch(const TransportError& e)
    {
        MarkBrokenIo(e.what(), Clock::now());
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
    std::lock_guard<std::mutex> io(io_mutex_);
    uint32_t             dev_index  = 0;
    int32_t              mode_index = -1;
    std::vector<uint8_t> raw;
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

        raw = dev->second.modes[(size_t)mode_index].raw;
        if(!PatchModeParams(raw, protocol_version_, patch))
        {
            last_error_ = "cannot patch mode parameters";
            return false;
        }
        dev_index = dev->second.index;
    }

    try
    {
        SendPacket(dev_index, kPktUpdateMode,
                   BuildUpdateModeBody(mode_index, raw));
        DrainPending();
    }
    catch(const TransportError& e)
    {
        MarkBrokenIo(e.what(), Clock::now());
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
    std::lock_guard<std::mutex> io(io_mutex_);
    uint32_t dev_index  = 0;
    int32_t  zone_index = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto dev = devices_.find(controller);
        if(dev == devices_.end() || state_ != ConnectionState::Connected)
        {
            last_error_ = "controller unavailable";
            return false;
        }
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
        dev_index = dev->second.index;
    }

    try
    {
        SendPacket(dev_index, kPktResizeZone,
                   BuildResizeZoneBody(zone_index, new_size));
        DrainPending();
    }
    catch(const TransportError& e)
    {
        MarkBrokenIo(e.what(), Clock::now());
        return false;
    }

    /* The zone's LED count changed on the device: every frame
       assembled against the old topology is obsolete (plan
       section 6 — stop obsolete frames on topology change).
       Bump the generation WITHOUT re-stamping the snapshots:
       snapshots keep reporting their pre-change generation, so
       every subsequent SubmitLatest rejects as stale until the
       caller's Enumerate() refreshes zones and colors. */
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++topology_generation_;
    }
    return true;
}

} /* namespace sdk */
} /* namespace openrgb2 */
