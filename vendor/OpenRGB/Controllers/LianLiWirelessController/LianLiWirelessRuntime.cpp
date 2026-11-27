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
#include <algorithm>
#include <chrono>
#include <stdexcept>

using namespace LianLiWireless;

WirelessRuntime::WirelessRuntime(IWirelessLink& l, IWirelessClock& c, WirelessRuntimeConfig cfg_in)
    : link(l), clock(c), cfg(cfg_in)
{
    rearm_delay_ms = cfg.rearm_ms;
}

RgbClockSnapshot WirelessRuntime::ClockSnapshot() const
{
    RgbClockSnapshot result = rgb_clock;
    result.age_ms = clock.NowMs() >= result.sampled_at_ms
        ? clock.NowMs() - result.sampled_at_ms : UINT64_MAX;
    result.valid = result.valid && result.age_ms <= 6000;
    return result;
}

uint64_t WirelessRuntime::NextDeadlineMs() const
{
    const uint64_t now = clock.NowMs();
    if(cancelled || state == WirelessState::ForeignMaster)
        return now + 1000;
    if(state == WirelessState::Failed)
        return retry_transport ? next_reconnect_ms : now + 1000;

    uint64_t due = next_poll_ms;
    if(master_known && have_sighting)
        due = std::min(due, next_keepalive_ms);
    if(state == WirelessState::Uploading && desired && have_sighting
       && !keepalive_initial)
        due = std::min(due, std::max(next_stream_ms, next_resend_ms));
    if(master_known && have_sighting && desired && desired->FrameCount() > 1)
        due = std::min(due, next_rgb_clock_ms);
    return due;
}

void WirelessRuntime::SetTarget(const Mac& mac, uint8_t fans)
{
    const bool changed = (mac != target_mac || fans != expected_fans);
    target_mac    = mac;
    expected_fans = fans;
    layout_mismatch = 0;
    /* A corrected fan count (rescan) can only cure a layout wedge; it
       is not an operator decision about ownership or transmitters. */
    if(changed && IsTerminalWedge() && wedge_cause == WedgeCause::Layout)
    {
        RearmTerminal();
    }
}

void WirelessRuntime::SetDesired(std::shared_ptr<const RgbUpload> upload)
{
    desired        = std::move(upload);
    RearmTerminal();
    /* Newest frame wins, but never bypass the RF upload budget. */
    const uint64_t now = clock.NowMs();
    next_resend_ms = now > next_stream_ms ? now : next_stream_ms;
    /* A live stream changes effect_id faster than discovery can confirm it.
       Defer a poll only until the last sighting reaches the freshness bound;
       continuous frames must not hide a lost or re-bound group. */
    if(have_sighting)
    {
        const uint64_t postponed = std::min(now + cfg.poll_ms,
            last_seen_ms + cfg.max_discovery_defer_ms);
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
    rgb_clock.valid = false;
    /* Mode changes route through here (service BeginMode). A wedged
       runtime must re-observe too, so keep-alives resume once the
       group reports our master again. */
    RearmTerminal();
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
    if(s != state || (err && *err))
        Diagnostics::Get().Trace("state", (long long)state, (long long)s,
                                  err && *err ? 1 : 0);
    const bool searching_before = (state == WirelessState::Boot
                                   || state == WirelessState::Searching);
    const bool searching_after  = (s == WirelessState::Boot
                                   || s == WirelessState::Searching);
    if(searching_after && !searching_before)
    {
        /* A new search: the never-sighted grace restarts at the
           first poll of this search. */
        search_armed = false;
    }
    if(s == WirelessState::Holding && state != WirelessState::Holding)
    {
        /* The timed re-arm backoff resets only after Holding has been
           sustained (Tick), never on a brief confirmation. */
        holding_since_ms = clock.NowMs();
    }
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
    layout_mismatch = 0;
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
    rgb_clock.valid = false;
    next_rgb_clock_ms = 0;
    rearm_scheduled = false;
    Enter(WirelessState::Boot);
}

bool WirelessRuntime::IsTerminalWedge() const
{
    return state == WirelessState::ForeignMaster
        || (state == WirelessState::Failed && !retry_transport);
}

void WirelessRuntime::RearmTerminal(bool from_intent)
{
    /*---------------------------------------------------------*\
    | N01 (QA 2026-10-01): a terminal wedge must not wait for  |
    | process restart while new intent keeps arriving. In       |
    | Failed (resend budget / layout debounce) or ForeignMaster |
    | Tick early-returns forever — keep-alives stop, desired    |
    | uploads are stored but never sent, and the fans hold      |
    | their last frame while the SDK still reports Direct +     |
    | black at the socket boundary. A new desired upload (or a  |
    | mode change clearing it) is new intent: re-open the       |
    | boot -> search -> upload cycle through the same reset     |
    | used for transport reconnects. Observation still gates    |
    | every write, so a genuinely foreign or incompatible       |
    | group simply re-fails through the normal debounce path.   |
    | A Failed runtime already inside a reconnect backoff       |
    | keeps its schedule; Cancelled is never re-armed.          |
    |                                                           |
    | Uploaded playback loops send no further intent, so Tick   |
    | also re-arms (from_intent=false) on a slow timer          |
    | (rearm_ms, doubling to rearm_max_ms) — but ONLY the       |
    | observation-only wedges (WedgeCause::GroupMaster /        |
    | Layout). After such a re-arm keep-alives and uploads      |
    | still require a sighting that reports our master and the  |
    | expected layout without motherboard RGB sync, so nothing  |
    | is written until the group is confirmed ours again.       |
    |                                                           |
    | Competing owners are report-only: UploadBudget (another   |
    | owner keeps overriding the effect on our own transmitter) |
    | would fight again on every re-arm, and                    |
    | TransmitterIdentity would hand the fans to whatever       |
    | transmitter is present. Both stay wedged until new intent |
    | (an operator mode/effect request). New intent on a        |
    | TransmitterIdentity wedge adopts the transmitter present  |
    | at the next Boot — that is the operator's decision — and  |
    | TakeAdoptedMaster reports it so detection can follow.     |
    \*---------------------------------------------------------*/
    if(!IsTerminalWedge())
    {
        return;
    }
    if(!from_intent && !TimedRearmAllowed(wedge_cause))
    {
        return;
    }
    if(wedge_cause == WedgeCause::TransmitterIdentity)
    {
        master        = Mac{};
        adopt_on_boot = true;
    }
    wedge_cause = WedgeCause::None;
    RetryTransport();
}

bool WirelessRuntime::TakeAdoptedMaster(Mac& out)
{
    if(!adopted_pending)
    {
        return false;
    }
    adopted_pending = false;
    out = master;
    return true;
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
    layout_mismatch = 0;
    keepalive_initial = true;
    next_keepalive_ms = 0;
    next_resend_ms = 0;
    next_poll_ms = clock.NowMs() + cfg.poll_ms;
    rgb_clock.valid = false;
    next_rgb_clock_ms = 0;
    Enter(WirelessState::Boot, err);
}

void WirelessRuntime::Tick()
{
    if(cancelled)
    {
        return;
    }

    uint64_t now = clock.NowMs();
    if(IsTerminalWedge())
    {
        /*-----------------------------------------------------*\
        | Timed re-arm (see RearmTerminal): observation-only     |
        | causes only. The first Tick in such a wedge schedules  |
        | it; the wedge stays silent until then. Report-only     |
        | causes stay silent until new intent.                   |
        \*-----------------------------------------------------*/
        if(!TimedRearmAllowed(wedge_cause))
        {
            return;
        }
        if(!rearm_scheduled)
        {
            rearm_scheduled = true;
            next_rearm_ms   = now + rearm_delay_ms;
            return;
        }
        if(now < next_rearm_ms)
        {
            return;
        }
        Diagnostics::Get().Trace("timed_rearm", (long long)state,
                                 (long long)rearm_delay_ms, 0);
        rearm_delay_ms = std::min(rearm_delay_ms * 2, cfg.rearm_max_ms);
        RearmTerminal(false);
        return;
    }
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

    /* Confirmed ours for a sustained period: the next wedge starts a
       fresh timed re-arm backoff. A brief Holding does not count. */
    if(state == WirelessState::Holding
       && now - holding_since_ms >= cfg.rearm_stable_ms)
    {
        rearm_delay_ms = cfg.rearm_ms;
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

    // Master-clock reads are maintenance traffic, never part of live frame
    // admission. The worker serializes this bounded query with TX/RX.
    if(master_known && have_sighting && !keepalive_initial && desired
       && desired->FrameCount() > 1 && now >= next_rgb_clock_ms)
    {
        next_rgb_clock_ms = now + 3000;
        uint32_t ticks = 0;
        const bool got = link.ReadRgbClock(master, latest.channel, ticks)
                         && ticks != 0;
        if(got)
        {
            const bool reboot = rgb_clock.valid
                && uint32_t(ticks - rgb_clock.ticks) > 0x80000000u;
            rgb_clock.valid = !reboot;
            rgb_clock.ticks = ticks;
            rgb_clock.sampled_at_ms = clock.NowMs();
        }
        else
        {
            rgb_clock.valid = false;
        }
        Diagnostics::Get().RecordRgbClock(got && rgb_clock.valid, ticks);
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
            wedge_cause = WedgeCause::TransmitterIdentity;
            Enter(WirelessState::ForeignMaster, "transmitter identity changed");
            return false;
        }
        master = current_master;
        if(adopt_on_boot)
        {
            /* new intent re-armed an identity wedge: operator adoption */
            adopt_on_boot   = false;
            adopted_pending = true;
        }
        connection_generation = link.ConnectionGeneration();
        master_read_tries = 0;
        master_known = true;
        Enter(WirelessState::Searching);
    }

    if(!search_armed)
    {
        search_armed      = true;
        search_started_ms = now;
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

    /*---------------------------------------------------------*\
    | A matching record with missing/corrupt fields is an       |
    | incomplete observation — the RX table can briefly expose  |
    | a partially written row under RF load (seen on hardware   |
    | at stream start). A bound group always reports >= 1 fan,  |
    | a unicast master MAC, and a receiver slot >= 1 (UsbChunks |
    | rejects receiver 0). Any field violating that means "row  |
    | not yet reliable": treat it as a miss, keep the last good |
    | sighting until it goes stale, and never let it vote       |
    | toward foreign-master or layout failures.                 |
    \*---------------------------------------------------------*/
    if(found != nullptr && !IsCompleteSighting(*found))
    {
        Diagnostics::Get().Trace("layout_incomplete", expected_fans,
                                 found->fan_count, found->mb_rgb_sync ? 1 : 0);
        found = nullptr;
    }

    Diagnostics::Get().RecordDiscovery(poll_duration_us, found != nullptr,
                                       sightings.size(),
                                       have_sighting ? (int64_t)(now - last_seen_ms) : -1);

    if(found == nullptr)
    {
        /* A miss is not a sighting: it cannot extend a consecutive-
           observation debounce (foreign master, layout) either. */
        layout_mismatch = 0;
        master_mismatch = 0;
        /*---------------------------------------------------------*\
        | A stale sighting is not an observation — clear it so the  |
        | Tick state machine stops resending to a vanished device.  |
        \*---------------------------------------------------------*/
        if(have_sighting && now - last_seen_ms > cfg.lost_ms)
        {
            have_sighting = false;
            Enter(WirelessState::Lost, "target absent past deadline");
        }
        else if(!have_sighting
                && now - std::max(search_started_ms, last_seen_ms) > cfg.lost_ms
                && state != WirelessState::Lost)
        {
            /* NowMs is a steady clock since boot: measure the grace from
               the start of this search (or the last good sighting, if
               later), never from the clock's epoch. */
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
        layout_mismatch = 0;
        have_sighting = false;
        if(++master_mismatch >= cfg.master_debounce)
        {
            wedge_cause = WedgeCause::GroupMaster;
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
        Diagnostics::Get().Trace("layout_mismatch", expected_fans,
                                  found->fan_count, found->mb_rgb_sync ? 1 : 0);
        have_sighting = false;
        /* A single discovery reply can briefly report an incomplete layout.
           Stop writes immediately, then require repeated incompatible replies
           before making the failure terminal. */
        if(++layout_mismatch >= cfg.layout_debounce)
        {
            wedge_cause = WedgeCause::Layout;
            Enter(WirelessState::Failed, "fan layout or motherboard RGB ownership changed");
        }
        return false;
    }

    layout_mismatch = 0;
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
        wedge_cause = WedgeCause::UploadBudget;
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
