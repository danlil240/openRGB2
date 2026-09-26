/*---------------------------------------------------------*\
| LianLiWirelessRuntime.h                                   |
|                                                           |
|   Transport-independent wireless runtime core. Owns the   |
|   desired/observed/confirmed state model, discovery       |
|   merging, send-until-ack convergence, keep-alive         |
|   scheduling, channel/slot refresh and foreign-master     |
|   debounce — all driven by injected clock + link so the   |
|   logic is testable without USB hardware.                 |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include "LianLiWirelessProtocol.h"
#include <memory>
#include <string>

namespace LianLiWireless
{
    struct IWirelessClock
    {
        virtual uint64_t NowMs() = 0;

        /*---------------------------------------------------------*\
        | Optional local wall-clock time carried by keep-alive      |
        | packets. Default false -> runtime sends a fixed           |
        | timestamp (used by tests).                                |
        \*---------------------------------------------------------*/
        virtual bool WallClock(uint16_t& /*year*/, uint8_t& /*month*/,
                               uint8_t& /*day*/, uint8_t& /*hour*/,
                               uint8_t& /*minute*/, uint8_t& /*second*/)
        {
            return false;
        }

        virtual ~IWirelessClock() = default;
    };

    /*---------------------------------------------------------*\
    | One transport operation slice. Tests fake this; the real  |
    | implementation wraps libusb on the TX/RX pair.            |
    | Read* return false on transport failure — distinct from   |
    | "response arrived but target absent", which is success    |
    | with an empty/unmatching record list.                     |
    \*---------------------------------------------------------*/
    struct IWirelessLink
    {
        virtual bool ReadMasterMac(Mac& out)                                   = 0;
        virtual bool PollDiscovery(std::vector<uint8_t>& out)                  = 0;
        virtual bool SendChunks(const std::array<UsbChunk, USB_CHUNKS_PER_PACKET>& chunks) = 0;
        /* Changes whenever a shared link drops/reopens its USB handles.
           Access is serialized by the service worker with all link I/O. */
        virtual uint64_t ConnectionGeneration() const { return 0; }
        virtual ~IWirelessLink() = default;
    };

    enum class WirelessState
    {
        Boot,           /* reading local master MAC            */
        Searching,      /* waiting for target sighting         */
        Uploading,      /* sending/resending desired upload    */
        Holding,        /* confirmed; watching for drift       */
        Lost,           /* sustained absence past deadline     */
        ForeignMaster,  /* target re-bound to another master   */
        Failed,         /* transport backoff or terminal upload failure */
        Cancelled,
    };

    struct WirelessRuntimeConfig
    {
        uint64_t poll_ms          = 300;
        uint64_t stream_ms        = 25;     /* minimum gap between RGB uploads */
        uint64_t resend_ms        = 1500;
        uint64_t keepalive_ms     = 750;
        uint64_t lost_ms          = 8000;
        uint64_t hold_drift_ms    = 400;    /* poll cadence while holding   */
        uint32_t master_debounce  = 3;      /* consecutive foreign sightings */
        uint32_t drift_debounce   = 3;      /* consecutive mismatches -> resend */
        uint32_t max_send_failures = 5;     /* transport failures -> Failed  */
        uint32_t max_resends      = 8;      /* per-upload resend budget      */
        uint64_t reconnect_ms     = 3000;   /* delay between transport retry batches */
    };

    class WirelessRuntime
    {
    public:
        WirelessRuntime(IWirelessLink& link, IWirelessClock& clock,
                        WirelessRuntimeConfig cfg = {});

        void SetTarget(const Mac& mac, uint8_t expected_fans);
        /* newest-frame-wins: replaces any pending desired upload */
        void SetDesired(std::shared_ptr<const RgbUpload> upload);
        void ClearDesired();
        void Cancel();

        /*---------------------------------------------------------*\
        | Drive one iteration: keep-alive if due, one discovery     |
        | poll+merge, then send/resend scheduling. Cheap to call    |
        | in a loop with a real clock; deterministic in tests.      |
        \*---------------------------------------------------------*/
        void Tick();

        WirelessState   State()        const { return state; }
        const Sighting* Latest()       const { return have_sighting ? &latest : nullptr; }
        bool            Confirmed()    const;
        size_t          ResendCount()  const { return resends; }
        size_t          SendFailures() const { return send_failures + keepalive_failures; }
        const char*     LastError()    const { return last_error.c_str(); }

    private:
        bool PollAndMerge(uint64_t now);
        bool SendDesired(uint64_t now);
        bool SendKeepAlive(uint64_t now, bool initial);
        void Enter(WirelessState s, const char* err = "");
        void TransportFailed(const char* err);
        void RevalidateTransport(const char* err);
        void RetryTransport();

        IWirelessLink&       link;
        IWirelessClock&      clock;
        WirelessRuntimeConfig cfg;

        WirelessState        state = WirelessState::Boot;
        Mac                  target_mac{};
        uint8_t              expected_fans = 0;
        Mac                  master{};
        bool                 master_known = false;
        uint32_t             master_read_tries = 0;

        Sighting             latest{};
        bool                 have_sighting = false;
        uint64_t             last_seen_ms = 0;

        std::shared_ptr<const RgbUpload> desired;
        uint64_t             next_stream_ms = 0;
        uint64_t             next_resend_ms = 0;
        uint32_t             resends = 0;
        uint32_t             send_failures = 0;
        uint32_t             keepalive_failures = 0;
        uint32_t             poll_failures = 0;
        uint32_t             master_mismatch = 0;
        uint32_t             drift_mismatch = 0;

        uint64_t             next_keepalive_ms = 0;
        bool                 keepalive_initial = true;
        bool                 keepalive_ok = true;
        uint64_t             next_poll_ms = 0;

        std::string          last_error;
        bool                 cancelled = false;
        bool                 retry_transport = false;
        uint64_t             next_reconnect_ms = 0;
        uint64_t             connection_generation = 0;
    };
}
