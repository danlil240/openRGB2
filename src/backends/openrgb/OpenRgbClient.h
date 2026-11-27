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
|||   Threading: all public methods are callable from any   ||
|||   thread under a TWO-mutex split (qa F12 — the old        ||
|||   single mutex was held across multi-second wire I/O,     ||
|||   starving every state reader behind reconnect churn):    ||
|||                                                           ||
|||   io_mutex_ — serializes ALL wire operations on           ||
|||     stream_ (connect/handshake/enumerate/request-send     ||
|||     sequences, liveness probes). A thread holding         ||
|||     io_mutex_ owns the wire and is the only thread that   ||
|||     may commit connection-state transitions.              ||
|||   mutex_    — the STATE lock: short reads/commits of      ||
|||     options_, protocol_version_, state_, last_error_,     ||
|||     devices_, topology_generation_, the reconnect         ||
|||     schedule, stats_. NEVER held across wire I/O — that   ||
|||     is what unblocks the diagnostics surface (State/      ||
|||     Snapshots/etc.) while a wire operation is in flight.  ||
|||                                                           ||
|||   LOCK ORDER: io_mutex_ -> mutex_. A thread holding       ||
|||   io_mutex_ may briefly take mutex_ to snapshot or        ||
|||   commit state; NO code may acquire io_mutex_ while       ||
|||   holding mutex_ (getters never touch the wire). State    ||
|||   commits under mutex_ are atomic per operation — a       ||
|||   reader never sees a half-swapped device map.            ||
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "EngineTypes.h"
#include "FrameSink.h"
#include "PlaybackClockSync.h"
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
    /* Packet-4000 (wireless status) probe: one overall reply
       deadline, far below io_timeout_ms — a stock backend never
       answers and the probe holds the wire (io_mutex_) while it
       waits. A backend that missed it is re-probed after
       wireless_reprobe_ms (and on every enumeration). */
    uint32_t         wireless_probe_timeout_ms = 300;
    uint32_t         wireless_reprobe_ms       = 60000;
    ReconnectBackoff backoff;
};

/* The connection state fields in ONE atomic read — used by
   the IPC connection/diagnostics surfaces so a status read
   never turns into four separate state-lock acquisitions
   (let alone queue behind wire I/O). */
struct ConnectionSnapshotInfo
{
    ConnectionState state             = ConnectionState::Disconnected;
    uint32_t        protocol_version  = 0;    /* negotiated, <= 4 */
    uint32_t        reconnect_attempt = 0;
    std::string     last_error;
};

/* Cached result of the LIANLI_WIRELESS_STATUS vendor
   extension (N01 observability — a wedged wireless group
   must surface in diagnostics instead of staying silent).

   probed    — the extension request has been sent at least
               once on this connection.
   supported — the backend answered (the lianli-wireless
               fork). A stock backend drops unknown packets
               silently under protocol <= 4, so the probe
               times out (its own short deadline): supported
               latches false and the connection stays
               healthy. The latch is re-probed only on the
               slow wireless_reprobe_ms schedule and on every
               enumeration, and clears as soon as a late reply
               arrives (a fork that was merely slow).
   have_data — a reply was received AND parsed (a replying
               server is "supported" even when one reply is
               malformed).
   fetched_at/status — the last parsed reply and when it
               arrived; diagnostics computes staleness from
               fetched_at rather than trusting a cached body
               to be current. status.raw is shared
               (shared_ptr<const json>), so copying the
               snapshot never deep-copies the document. */
struct WirelessStatusSnapshot
{
    bool              probed    = false;
    bool              supported = false;
    bool              have_data = false;
    TimePoint         fetched_at{};
    SdkWirelessStatus status;
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

    /* All four connection-state fields in ONE state-lock
       acquisition (state + protocol_version +
       reconnect_attempt + last_error) — the atomic read the
       IPC connection/diagnostics surfaces consume. Never
       touches io_mutex_; returns promptly while a wire
       operation is in flight. */
    ConnectionSnapshotInfo ConnectionSnapshot() const;

    /* Re-fetches controller count + data and rebuilds the
       DeviceKey map; bumps the topology generation so frames
       stamped with older generations are obsolete. Requires
       Connected. */
    bool Enumerate();

    /* Lightweight refresh for mode/state acknowledgement
       (the playback task) and slow periodic topology
       revalidation (the owner loop): re-fetches the
       controller count + every known controller's data block
       WITHOUT bumping the topology generation — pending
       frames stay valid. Only mode/speed/direction/color
       fields and last-enumerated colors are updated in
       place; a structural difference (device identity, zone
       layout, LED counts, device count — including an added
       or removed device, caught by the count) falls through
       to the full EnumerateIo path and bumps the
       generation. False when disconnected or the fetch fails
       (connection marked broken). */
    bool RefreshStatus();

    /* Last server-reported mode state for `controller` —
       updated by Enumerate()/RefreshStatus(). `mode_index`
       is the enumerated active_mode; `mode` (when non-null)
       receives the parsed active-mode description (name,
       speed, direction, colors — no raw blob). This is the
       applied-mode acknowledgement the playback task polls:
       SDK sends are fire-and-forget, so the re-fetched value
       is the only honest "backend applied it" signal. */
    bool ActiveMode(const DeviceKey& controller,
                    int32_t& mode_index,
                    SdkMode* mode = nullptr) const;

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

    /* The current topology generation (state lock only, no
       copy) — lets callers cache snapshot-derived data and
       rebuild only when the topology moved (E-6). Mode/colour
       refreshes never bump it. */
    uint64_t TopologyGeneration() const;

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

    /* UPDATEMODE with the full parameter patch (playback
       presets: speed, direction, mode-specific colors).
       Fields not set in `patch` keep the enumerated values. */
    bool SelectMode(const DeviceKey& controller, const std::string& name,
                    const ModePatch& patch);

    /* RESIZEZONE: resizes the zone named `zone_name` (resolved
       by name). Call Enumerate() afterwards — topology changed. */
    bool ResizeZone(const DeviceKey& controller, const std::string& zone_name,
                    int32_t new_size);

    /*---- Wireless status (lianli-wireless fork extension) ----*\
    | FetchWirelessStatus() is a wire op (io_mutex_): sends    |
    | NET_PACKET_ID_LIANLI_WIRELESS_STATUS and caches the      |
    | reply. It also rides Connect()/Enumerate() once and      |
    | RefreshStatus() on every call so the snapshot stays      |
    | live. Returns false only when the wire op broke the      |
    | connection — a silently-dropped request on a stock       |
    | backend returns true with supported=false latched.       |
    | Replies are matched by counting outstanding requests, so |
    | a late reply to a timed-out probe is never read as the   |
    | next probe's answer.                                     |
    | WirelessStatus() copies the cache under the state lock   |
    | only — never touches the wire.                           |
    \*---------------------------------------------------------*/
    bool FetchWirelessStatus();
    void WirelessStatus(WirelessStatusSnapshot& out) const;

    /* Fresh fetch, then extract the PlaybackClockSnapshot for
       the group whose `mac` equals `controller.serial` — the
       playback-clock-sync dep (PlaybackClockSync.h expects a
       per-call provider). False when the extension is
       unsupported, THIS fetch got no parsed reply (a cached
       status is never re-stamped as a new sample), the group
       is unknown, or its rgb_clock is invalid — the caller
       treats a miss like any absent sample (staleness, not
       breakage). sampled_at = reply arrival - backend age. */
    bool WirelessClockSample(const DeviceKey& controller,
                             PlaybackClockSnapshot& out);

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
    /* Ok, or a clean Closed/Timeout (no byte consumed); a
       stall after part of a packet throws TransportError. */
    RecvResult RecvPacket(Packet& pkt);
    std::vector<uint8_t> Request(uint32_t dev_id, uint32_t pkt_id,
                                 const std::vector<uint8_t>& body,
                                 uint32_t expected_reply_id);
    void DrainPending();
    /* Version negotiation + client name. Takes the wire
       operation's options snapshot — options_ is never read
       lock-free. */
    void Handshake(const OpenRgbClientOptions& opts);

    /*---- Wire operations -----------------------------------*\
    | The *Io helpers run ONLY under io_mutex_ (one wire op  |
    | at a time). They take mutex_ briefly for state reads/  |
    | commits — lock order io_mutex_ -> mutex_, never the    |
    | reverse (see the class-header threading note).         |
    \*--------------------------------------------------------*/
    bool EnumerateIo(std::string& err);
    bool ConnectIo(std::string& err);
    /* force: probe even when latched unsupported. fresh: set
       true only when THIS call parsed a reply. */
    bool FetchWirelessStatusIo(bool force, bool* fresh = nullptr);
    void NoteWirelessReplyIo();
    void MarkBrokenIo(const std::string& reason, TimePoint now);
    void PollIo(TimePoint now);

    mutable std::mutex   io_mutex_;  /* wire serialization      */
    mutable std::mutex   mutex_;     /* state reads/commits     */
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

    /* Last wireless-status fetch — state lock; cleared on
       (re)connect so a swapped backend gets a fresh probe. */
    WirelessStatusSnapshot wireless_status_;
    TimePoint              wireless_next_probe_{};  /* state lock */

    /* Server-reported controller count at the last enumeration
       (state lock) — may exceed devices_.size() when
       duplicate-key controllers collapse. */
    uint32_t               server_controller_count_ = 0;

    /* Packet-4000 requests sent whose replies have not been
       read yet. io_mutex_ only (wire-side bookkeeping). */
    uint32_t               wireless_pending_ = 0;
};

} /* namespace sdk */
} /* namespace openrgb2 */
