/*---------------------------------------------------------*\
| LianLiWirelessRuntime.cpp                                 |
|                                                           |
|   Transport-independent wireless runtime core.            |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "LianLiWirelessRuntime.h"
#include "LianLiWirelessDiagnostics.h"
#include <chrono>
#include <stdexcept>

using namespace LianLiWireless;

WirelessRuntime::WirelessRuntime(IWirelessLink& l, IWirelessClock& c, WirelessRuntimeConfig cfg_in)
    : link(l), clock(c), cfg(cfg_in)
{
}

void WirelessRuntime::SetTarget(const Mac& mac, uint8_t fans)
{
    target_mac    = mac;
    expected_fans = fans;
}

void WirelessRuntime::SetDesired(std::shared_ptr<const RgbUpload> upload)
{
    desired        = std::move(upload);
    /* Newest frame wins, but never bypass the RF upload budget. */
    const uint64_t now = clock.NowMs();
    next_resend_ms = now > next_stream_ms ? now : next_stream_ms;
    /* A live stream changes effect_id faster than discovery can confirm it.
       Once identity/channel are known, postpone the blocking RX poll from
       the newest frame so continuous animation keeps the TX path clear.
       Discovery resumes one normal poll interval after the stream stops. */
    if(have_sighting)
    {
        const uint64_t postponed = now + cfg.poll_ms;
        if(postponed > next_poll_ms)
        {
            Diagnostics::Get().RecordDiscoveryDeferred();
        }
        next_poll_ms = postponed;
    }
    resends        = 0;
    drift_mismatch = 0;
    if(state == WirelessState::Holding)
    {
        Enter(WirelessState::Uploading);
    }
}

void WirelessRuntime::ClearDesired()
{
    desired.reset();
}

void WirelessRuntime::Cancel()
{
    cancelled = true;
    Enter(WirelessState::Cancelled);
}

bool WirelessRuntime::Confirmed() const
{
    return state == WirelessState::Holding
        && have_sighting
        && desired
        && latest.effect_id == desired->EffectId();
}

void WirelessRuntime::Enter(WirelessState s, const char* err)
{
    state = s;
    if(s == WirelessState::ForeignMaster)
    {
        Diagnostics::Get().RecordForeignMaster();
    }
    if(err && *err)
    {
        last_error = err;
    }
    else if(s == WirelessState::Holding)
    {
        last_error.clear();
    }
}

void WirelessRuntime::TransportFailed(const char* err)
{
    have_sighting = false;
    Enter(WirelessState::Failed, err);
    retry_transport = true;
    next_reconnect_ms = clock.NowMs() + cfg.reconnect_ms;
}

void WirelessRuntime::RetryTransport()
{
    /* Preserve the desired profile and original master identity, but
       discard all observations/counters from before the outage. */
    Diagnostics::Get().RecordTransportRevalidation();
    master_known = false;
    have_sighting = false;
    latest = {};
    last_seen_ms = 0;
    master_read_tries = 0;
    master_mismatch = 0;
    poll_failures = 0;
    send_failures = 0;
    keepalive_failures = 0;
    resends = 0;
    drift_mismatch = 0;
    next_poll_ms = 0;
    next_stream_ms = 0;
    next_resend_ms = 0;
    next_keepalive_ms = 0;
    keepalive_initial = true;
    retry_transport = false;
    Enter(WirelessState::Boot);
}

void WirelessRuntime::RevalidateTransport(const char* err)
{
    /* Even one failed operation can invalidate and reopen the USB pair.
       Re-read its identity before any further discovery-driven writes. */
    Diagnostics::Get().RecordTransportRevalidation();
    master_known = false;
    have_sighting = false;
    latest = {};
    master_read_tries = 0;
    master_mismatch = 0;
    keepalive_initial = true;
    next_keepalive_ms = 0;
    next_resend_ms = 0;
    next_poll_ms = clock.NowMs() + cfg.poll_ms;
    Enter(WirelessState::Boot, err);
}

void WirelessRuntime::Tick()
{
    if(cancelled || state == WirelessState::ForeignMaster)
    {
        return;
    }

    uint64_t now = clock.NowMs();
    if(state == WirelessState::Failed)
    {
        if(!retry_transport || now < next_reconnect_ms)
        {
            return;
        }
        RetryTransport();
    }
    if(master_known && connection_generation != link.ConnectionGeneration())
    {
        RevalidateTransport("shared USB connection changed");
        return;
    }

    /*---------------------------------------------------------*\
    | Keep-alive takes priority over animation traffic and is   |
    | required before the device will reliably accept uploads.  |
    \*---------------------------------------------------------*/
    if(master_known && have_sighting && now >= next_keepalive_ms)
    {
        if(!SendKeepAlive(now, keepalive_initial))
        {
            if(++keepalive_failures >= cfg.max_send_failures)
            {
                TransportFailed("keep-alive transport failure");
            }
            else
            {
                RevalidateTransport("keep-alive transport failure");
            }
            return;
        }
        else
        {
            keepalive_initial  = false;
            keepalive_failures = 0;
            next_keepalive_ms  = now + cfg.keepalive_ms;
        }
    }

    if(now >= next_poll_ms)
    {
        next_poll_ms = now + (state == WirelessState::Holding ? cfg.hold_drift_ms
                                                             : cfg.poll_ms);

        if(!PollAndMerge(now))
        {
            return;             /* transport failure or Lost handled inside */
        }

        switch(state)
        {
            case WirelessState::Searching:
                if(have_sighting)
                {
                    Enter(desired ? WirelessState::Uploading : WirelessState::Holding);
                }
                break;

            case WirelessState::Uploading:
            case WirelessState::Holding:
            case WirelessState::Lost:
                if(desired && have_sighting)
                {
                    if(latest.effect_id == desired->EffectId())
                    {
                        drift_mismatch = 0;
                        resends        = 0;
                        if(state != WirelessState::Holding)
                        {
                            Enter(WirelessState::Holding);
                        }
                    }
                    else if(state == WirelessState::Holding)
                    {
                        /* Confirmed effect drifted: debounce before resend. */
                        if(++drift_mismatch >= cfg.drift_debounce)
                        {
                            drift_mismatch = 0;
                            Enter(WirelessState::Uploading);
                            next_resend_ms = now;
                        }
                    }
                    else if(state == WirelessState::Lost)
                    {
                        /* Returning target gets a fresh resend budget. */
                        resends        = 0;
                        drift_mismatch = 0;
                        Enter(WirelessState::Uploading);
                        next_resend_ms = now;
                    }
                }
                else if(!desired && have_sighting && state == WirelessState::Lost)
                {
                    Enter(WirelessState::Holding);
                }
                break;

            default:
                break;
        }
    }

    /* Discovery confirms identity and channel, but it must not gate
       every animation frame. Keep-alive still has first priority. */
    if(state == WirelessState::Uploading && desired && have_sighting
       && !keepalive_initial && now >= next_resend_ms
       && now >= next_stream_ms)
    {
        SendDesired(now);
    }
    else if(state == WirelessState::Uploading && desired && have_sighting
            && !keepalive_initial)
    {
        /* desired frame pending, but the RF stream budget, the resend
           schedule, or an outstanding initial keep-alive gated it */
        Diagnostics::Get().RecordUploadDeferred();
    }
}

bool WirelessRuntime::PollAndMerge(uint64_t now)
{
    if(state == WirelessState::Boot)
    {
        Mac current_master{};
        if(!link.ReadMasterMac(current_master))
        {
            if(++master_read_tries >= 3)
            {
                TransportFailed("cannot read master MAC");
                return false;
            }
            return false;                   /* retry next poll */
        }
        if(master != Mac{} && current_master != master)
        {
            Enter(WirelessState::ForeignMaster, "transmitter identity changed");
            return false;
        }
        master = current_master;
        connection_generation = link.ConnectionGeneration();
        master_read_tries = 0;
        master_known = true;
        Enter(WirelessState::Searching);
    }

    std::vector<uint8_t> response;
    const auto poll_begin = std::chrono::steady_clock::now();
    bool poll_ok = link.PollDiscovery(response);
    const uint64_t poll_duration_us =
        (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - poll_begin).count();
    if(!poll_ok)
    {
        have_sighting = false;
        /*---------------------------------------------------------*\
        | USB-level failure is not a miss — it is a transport       |
        | problem and counts toward failure. Kept separate from     |
        | send failures: TX can work while RX is broken.            |
        \*---------------------------------------------------------*/
        if(++poll_failures >= cfg.max_send_failures)
        {
            TransportFailed("discovery transport failure");
            return false;
        }
        RevalidateTransport("discovery transport failure");
        return false;           /* failed RX is not a fresh observation */
    }
    poll_failures = 0;

    std::vector<Sighting> sightings;
    try
    {
        sightings = ParseDiscoveryResponse(response.data(), response.size());
    }
    catch(const std::invalid_argument&)
    {
        /* malformed reply = miss, not error */
        sightings.clear();
    }

    const Sighting* found = nullptr;
    for(const Sighting& s : sightings)
    {
        if(s.mac == target_mac)
        {
            found = &s;
        }
    }

    Diagnostics::Get().RecordDiscovery(poll_duration_us, found != nullptr,
                                       sightings.size(),
                                       have_sighting ? (int64_t)(now - last_seen_ms) : -1);

    if(found == nullptr)
    {
        /*---------------------------------------------------------*\
        | A stale sighting is not an observation — clear it so the  |
        | Tick state machine stops resending to a vanished device.  |
        \*---------------------------------------------------------*/
        if(have_sighting && now - last_seen_ms > cfg.lost_ms)
        {
            have_sighting = false;
            Enter(WirelessState::Lost, "target absent past deadline");
        }
        else if(!have_sighting && now > cfg.lost_ms
                && state != WirelessState::Lost)
        {
            Enter(WirelessState::Lost, "target never sighted");
        }
        return true;
    }

    /*---------------------------------------------------------*\
    | Foreign-master debounce: only N consecutive sightings     |
    | reporting a different master count as a re-bind.          |
    \*---------------------------------------------------------*/
    if(master_known && found->master != master)
    {
        have_sighting = false;
        if(++master_mismatch >= cfg.master_debounce)
        {
            Enter(WirelessState::ForeignMaster, "wireless master changed");
            return false;
        }
        return false;           /* debounce ownership without writing to it */
    }
    else
    {
        master_mismatch = 0;
    }

    if(found->fan_count != expected_fans || found->mb_rgb_sync)
    {
        have_sighting = false;
        Enter(WirelessState::Failed, "fan layout or motherboard RGB ownership changed");
        return false;
    }

    latest        = *found;
    have_sighting = true;
    last_seen_ms  = now;
    return true;
}

bool WirelessRuntime::SendDesired(uint64_t now)
{
    if(resends >= cfg.max_resends)
    {
        Diagnostics::Get().RecordUploadFailure();
        Enter(WirelessState::Failed, "upload retry budget exhausted");
        return false;
    }

    /*---------------------------------------------------------*\
    | Address with the freshest sighting's channel/receiver so  |
    | a channel move self-corrects.                             |
    \*---------------------------------------------------------*/
    const auto send_begin = std::chrono::steady_clock::now();
    bool ok = true;
    for(size_t i = 0; i < desired->PacketCount(); i++)
    {
        RfPacket rf = desired->Packet(i, target_mac, master);
        auto chunks = UsbChunks(rf, latest.channel, latest.receiver);

        /* first (header) packet is sent twice, matching vendor behavior */
        uint8_t repeats = (i == 0) ? 2 : 1;
        for(uint8_t r = 0; r < repeats && ok; r++)
        {
            ok = link.SendChunks(chunks);
        }
    }
    const uint64_t send_duration_us =
        (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - send_begin).count();

    if(!ok)
    {
        Diagnostics::Get().RecordUploadFailure();
        if(++send_failures >= cfg.max_send_failures)
        {
            TransportFailed("upload transport failure");
        }
        else
        {
            RevalidateTransport("upload transport failure");
        }
        return false;
    }

    Diagnostics::Get().RecordUploadAdmitted(desired->PacketCount(),
                                             1, send_duration_us);
    if(resends >= 1)
    {
        Diagnostics::Get().RecordUploadRetry();
    }

    send_failures  = 0;
    resends++;
    next_stream_ms = now + cfg.stream_ms;
    next_resend_ms = now + cfg.resend_ms;
    return true;
}

bool WirelessRuntime::SendKeepAlive(uint64_t now, bool /*initial*/)
{
    uint16_t year   = 2000;
    uint8_t  month  = 1, day = 1, hour = 0, minute = 0, second = 0;
    clock.WallClock(year, month, day, hour, minute, second);
    auto chunks = ClockChunks(master, have_sighting ? latest.channel : 0,
                              year, month, day, hour, minute, second,
                              keepalive_initial);

    const auto begin = std::chrono::steady_clock::now();
    const bool ok   = link.SendChunks(chunks);
    const uint64_t duration_us =
        (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - begin).count();

    if(ok)
    {
        const int64_t gap_ms = keepalive_completed
                             ? (int64_t)(now - last_keepalive_ms) : -1;
        Diagnostics::Get().RecordKeepAlive(gap_ms, duration_us);
        last_keepalive_ms    = now;
        keepalive_completed  = true;
    }
    else
    {
        Diagnostics::Get().RecordKeepAliveFailure();
    }
    return ok;
}
