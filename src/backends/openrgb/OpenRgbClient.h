/*---------------------------------------------------------*\
||| OpenRgbClient.h                                           |
|||                                                           |
|||   TCP SDK client for the vendored OpenRGB backend        ||
|||   (plan section 4, "Engine and SDK"). Replaces the old   ||
|||   plugin's in-process controller access: the backend is  ||
|||   launched externally with --server on 127.0.0.1:6744    ||
|||   and remains the SOLE hardware owner — this client      ||
|||   never touches USB/HID/SMBus.                           ||
|||                                                           ||
|||   - Negotiates the protocol down to the cap (4). The     ||
|||     fork advertises 6; every command here uses the       ||
|||     protocol-4 wire shape (SdkWire.h documents the       ||
|||     verified behavior).                                  ||
|||   - Enumeration resolves DeviceKeys (vendor:name +       ||
|||     serial + location) to controller indices; indices    ||
|||     are re-resolved on EVERY connect so reordered        ||
|||     controllers can never receive a stale-index write.   ||
|||   - Implements IFrameSink: SubmitLatest(RgbFrame) sends  ||
|||     ONE complete-controller UPDATELEDS packet — never    ||
|||     three zone edits causing three uploads (plan         ||
|||     section 4). Results report into DeliveryStats at     ||
|||     the transport boundary — never "displayed".          ||
|||   - Reconnect uses bounded exponential backoff; the      ||
|||     connection state is surfaced through                 ||
|||     DeviceSnapshot.connection.                           ||
|||                                                           |
|||   Threading: all public methods are mutex-protected and  ||
|||   callable from any thread. Sends are serialized under   ||
|||   the mutex — the per-controller delivery worker of      ||
|||   Task 3.3 builds on this send path.                     ||
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "EngineTypes.h"
#include "FrameSink.h"
#include "ReconnectBackoff.h"
#include "SdkWire.h"
#include "Transport.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace openrgb2
{
namespace sdk
{

struct OpenRgbClientOptions
{
    std::string      host               = "127.0.0.1";
    uint16_t         port               = 6744;
    std::string      client_name        = "openrgb2-engine";
    uint32_t         connect_timeout_ms = 3000;
    uint32_t         io_timeout_ms      = 4000;
    ReconnectBackoff backoff;
};

class OpenRgbClient final : public IFrameSink
{
public:
    /* The byte-stream transport is injected: production passes a
       TcpSocket (loopback TCP); tests inject an in-process fake
       so the offline suite never opens a socket. The client
       reuses the same transport across reconnects (Close +
       Connect). */
    explicit OpenRgbClient(std::unique_ptr<IStreamTransport> transport);
    OpenRgbClient(OpenRgbClientOptions options,
                  std::unique_ptr<IStreamTransport> transport);
    ~OpenRgbClient();                              /* Disconnect() */

    OpenRgbClient(const OpenRgbClient&)            = delete;
    OpenRgbClient& operator=(const OpenRgbClient&) = delete;

    void SetOptions(OpenRgbClientOptions options);

    /*---- Connection lifecycle -------------------------------*\
    | Connect() performs the socket connect, protocol-version   |
    | handshake, client-name registration, and a full           |
    | enumeration. Returns false + LastError() on failure; the  |
    | state is then Disconnected (manual attempts do not enter  |
    | the reconnect schedule — Poll() does).                    |
    \*---------------------------------------------------------*/
    bool            Connect();
    void            Disconnect();
    ConnectionState State() const;
    uint32_t        ProtocolVersion() const;   /* negotiated, <= 4 */
    std::string     LastError() const;

    /* Re-fetches controller count + data and rebuilds the
       DeviceKey map; bumps the topology generation so frames
       stamped with older generations are obsolete. Requires
       Connected. */
    bool Enumerate();

    /* Reconnect schedule state. Poll(now) checks socket liveness
       while Connected (a peer close starts the schedule), and
       performs the next due attempt while Reconnecting. After
       backoff.max_attempts failures the state latches to
       Disconnected until Connect()/ReconnectNow(). */
    bool            Poll(TimePoint now = Clock::now());
    bool            ReconnectNow();
    uint32_t        ReconnectAttempt() const;
    TimePoint       NextAttemptAt() const;

    /*---- Topology --------------------------------------------*\
    | One DeviceSnapshot per enumerated controller. The        |
    | snapshot's connection field mirrors the client's current  |
    | state; mode/zone names come from enumeration, never       |
    | hard-coded array indices.                                 |
    \*---------------------------------------------------------*/
    std::vector<DeviceSnapshot> Snapshots() const;
    bool Snapshot(const DeviceKey& controller, DeviceSnapshot& out) const;

    /*---- IFrameSink: the direct-mode send path ----------------*\
    | Validates the frame against the current snapshot          |
    | (unknown controller, not connected, stale topology        |
    | generation, or LED-count mismatch -> Rejected with        |
    | reason), re-resolves the DeviceKey to the current         |
    | controller index, and sends ONE UPDATELEDS packet for     |
    | the whole controller. A transport failure breaks the      |
    | connection (Reconnecting) and returns Rejected —          |
    | SubmitResult is never "displayed".                        |
    \*---------------------------------------------------------*/
    SubmitResult  SubmitLatest(const RgbFrame& frame) override;
    DeliveryStats Stats() const override;

    /*---- Mode selection / parameters / resize ----------------*\
    | All take the controller-level DeviceKey (zone == "").    |
    | Mode names resolve against the enumerated mode list —    |
    | unsupported names fail cleanly without sending.           |
    \*---------------------------------------------------------*/

    /* SETCUSTOMMODE: switches to the controller's direct
       (custom) mode — required before live UpdateLEDs writes. */
    bool SetCustomMode(const DeviceKey& controller);

    /* UPDATEMODE: selects mode `name` (resolved by name from
       enumeration). speed >= 0 patches the speed field of the
       replayed raw mode description (clamped by the caller to
       the enumerated speed_min/max range). */
    bool SelectMode(const DeviceKey& controller, const std::string& name,
                    int32_t speed = -1);

    /* RESIZEZONE: resizes the zone named `zone_name` (resolved
       by name). Call Enumerate() afterwards — topology changed. */
    bool ResizeZone(const DeviceKey& controller, const std::string& zone_name,
                    int32_t new_size);

private:
    struct DeviceRecord
    {
        uint32_t              index = 0;   /* dev_id for this connect */
        DeviceSnapshot        snap;        /* connection always Connected */
        std::vector<SdkMode>  modes;       /* raw blobs for UPDATEMODE  */
    };

    /* Transport (all may throw TransportError). */
    struct TransportError;
    void SendPacket(uint32_t dev_id, uint32_t pkt_id,
                    const std::vector<uint8_t>& body);
    bool RecvPacket(Packet& pkt, std::string& err);
    std::vector<uint8_t> Request(uint32_t dev_id, uint32_t pkt_id,
                                 const std::vector<uint8_t>& body,
                                 uint32_t expected_reply_id);
    void DrainPending();
    void Handshake();          /* version negotiation + client name */

    bool EnumerateLocked(std::string& err);
    bool ConnectLocked(std::string& err);
    void MarkBrokenLocked(const std::string& reason, TimePoint now);
    void PollLocked(TimePoint now);

    mutable std::mutex   mutex_;
    OpenRgbClientOptions options_;
    std::unique_ptr<IStreamTransport> stream_;

    uint32_t             protocol_version_    = 0;
    ConnectionState      state_               = ConnectionState::Disconnected;
    std::string          last_error_;
    uint64_t             topology_generation_ = 0;
    std::map<DeviceKey, DeviceRecord> devices_;

    /* Reconnect schedule (valid while state_ == Reconnecting). */
    uint32_t             reconnect_attempt_ = 0;
    TimePoint            next_attempt_{};

    DeliveryStats        stats_;
};

} /* namespace sdk */
} /* namespace openrgb2 */
