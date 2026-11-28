/*---------------------------------------------------------*\
|| LianLiWirelessService.h                                   ||
||                                                           ||
||   Owns one TX/RX dongle pair for the lifetime of the      ||
||   driver. All link access is serialized on a single       ||
||   worker thread that drives one WirelessRuntime per fan   ||
||   group — fan-group adapters share this service and never ||
||   open the dongles independently.                         ||
||                                                           ||
||   This file is part of the OpenRGB project                ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "LianLiWirelessRuntime.h"
#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace LianLiWireless
{
    /*---------------------------------------------------------*\
    | Merges discovery sightings by MAC over time. Only         |
    | complete rows (IsCompleteSighting) count, and a group's   |
    | fan count is the modal count over the rows seen — one     |
    | short or torn row cannot size a group. Thread-safe.       |
    \*---------------------------------------------------------*/
    class SightingAccumulator
    {
    public:
        void Add(const Sighting& s, uint64_t now_ms);
        void AddResponse(const std::vector<uint8_t>& response, uint64_t now_ms);

        /* Groups seen within max_age_ms of now_ms; fan_count is the
           modal count (ties go to the most recently seen count). */
        std::vector<Sighting> Snapshot(uint64_t now_ms, uint64_t max_age_ms) const;

    private:
        struct Entry
        {
            Sighting                    last{};
            uint64_t                    last_seen_ms = 0;
            std::map<uint8_t, uint32_t> fan_votes;
        };
        mutable std::mutex      mutex;
        std::map<Mac, Entry>    entries;
    };

    class LianLiWirelessService
    {
    public:
        struct GroupStatus
        {
            WirelessState state         = WirelessState::Boot;
            bool          confirmed     = false;
            size_t        resends       = 0;
            size_t        send_failures = 0;
            std::string   last_error;
            bool          have_sighting = false;
            Sighting      latest{};
            uint16_t      frame_count = 0;
            uint32_t      interval_hundredths = 0;
            double        effective_cycle_ms = 0;
            double        nominal_fps = 0;
            size_t        compressed_bytes = 0;
            std::array<uint8_t, 4> effect_id{};
            RgbClockSnapshot rgb_clock;
        };

        /*---------------------------------------------------------*\
        | link/clock are owned by the caller and must outlive the   |
        | service — tests inject fakes, production passes the USB   |
        | link and system clock.                                    |
        \*---------------------------------------------------------*/
        LianLiWirelessService(IWirelessLink& link, IWirelessClock& clock,
                              WirelessRuntimeConfig cfg = {});
        ~LianLiWirelessService();

        LianLiWirelessService(const LianLiWirelessService&) = delete;
        LianLiWirelessService& operator=(const LianLiWirelessService&) = delete;

        /* returns false when the group is already tracked */
        bool AddGroup(const Mac& mac, uint8_t fan_count);
        void RemoveGroup(const Mac& mac);
        std::vector<Mac> Groups() const;

        /* Corrects a tracked group's expected fan count (rescan with a
           better observation); re-arms a layout wedge. False if the
           group is unknown. */
        bool UpdateGroupFans(const Mac& mac, uint8_t fan_count);

        /*---------------------------------------------------------*\
        | Discovery rows observed by the worker's own polls (every  |
        | runtime's discovery goes through the service link), so a  |
        | rescan can find groups that appeared later without        |
        | driving the link from the detector thread.                |
        \*---------------------------------------------------------*/
        std::vector<Sighting> RecentSightings(uint64_t max_age_ms) const;

        /*---------------------------------------------------------*\
        | The transmitter most recently adopted by a runtime after  |
        | new intent (an operator mode/effect request) re-armed a   |
        | transmitter-identity wedge. False if none ever was. The   |
        | detector's rescan uses it so groups bound to the adopted  |
        | transmitter are accepted; it is never set automatically.  |
        \*---------------------------------------------------------*/
        bool AdoptedMaster(Mac& out) const;

        /* newest-frame-wins; unknown MAC is ignored */
        uint64_t BeginMode(const Mac& mac, bool playback);

        /*---------------------------------------------------------*\
        | A controller rebuilt by a rescan starts in a mode of the  |
        | given kind without a mode update. If the group's pending  |
        | slot is already in that kind of mode, return its current  |
        | revision (frames are admitted at once, nothing is         |
        | cleared); otherwise BeginMode(playback). 0 for an unknown |
        | MAC or a slot with no mode selected yet (revision 0 is    |
        | admitted then).                                           |
        \*---------------------------------------------------------*/
        uint64_t AdoptMode(const Mac& mac, bool playback);
        void SetDesired(const Mac& mac, std::shared_ptr<const RgbUpload> upload,
                        uint64_t revision = 0);
        void ClearDesired(const Mac& mac);

        /*---------------------------------------------------------*\
        | Served from a per-group snapshot the worker publishes     |
        | after each runtime Tick — never waits on USB I/O.         |
        \*---------------------------------------------------------*/
        bool GetStatus(const Mac& mac, GroupStatus& out) const;

        /*---------------------------------------------------------*\
        | Worker thread: StepAll every tick_ms until Stop. Stop     |
        | cancels every runtime and joins the thread.               |
        \*---------------------------------------------------------*/
        void Start(uint32_t tick_ms = 50);
        void Stop();

        /* one Tick for every group; called by the worker, safe for tests */
        void StepAll();

    private:
        /*---------------------------------------------------------*\
        | Forwards to the real link and records every discovery     |
        | response into the accumulator. Used only on the worker    |
        | (all calls are serialized under the service mutex).       |
        |                                                           |
        | P-12: the discovery query has no group or channel field,  |
        | so one reply answers every group. A poll within           |
        | share_window_ms of the last successful read, on the same  |
        | connection generation, returns that reply without USB     |
        | I/O (and is not re-added to the accumulator). A failed    |
        | read, any other failed link operation, or a generation    |
        | change drops the shared reply.                            |
        \*---------------------------------------------------------*/
        struct ObservingLink : IWirelessLink
        {
            ObservingLink(IWirelessLink& inner_in, IWirelessClock& clock_in,
                          SightingAccumulator& sink_in, uint64_t share_window_ms_in)
                : inner(inner_in), clock(clock_in), sink(sink_in),
                  share_window_ms(share_window_ms_in) {}

            bool ReadMasterMac(Mac& out) override
            {
                return Forwarded(inner.ReadMasterMac(out));
            }
            bool PollDiscovery(std::vector<uint8_t>& out) override;
            bool SendChunks(const std::array<UsbChunk, USB_CHUNKS_PER_PACKET>& chunks) override
            {
                return Forwarded(inner.SendChunks(chunks));
            }
            bool ReadRgbClock(const Mac& m, uint8_t ch, uint32_t& ticks) override
            {
                /* false also means "no answer", which is not a transport
                   fault; dropping the shared reply then only costs one
                   extra read */
                return Forwarded(inner.ReadRgbClock(m, ch, ticks));
            }
            uint64_t ConnectionGeneration() const override { return inner.ConnectionGeneration(); }

            bool Forwarded(bool ok)
            {
                if(!ok)
                {
                    shared_valid = false;
                }
                return ok;
            }

            IWirelessLink&       inner;
            IWirelessClock&      clock;
            SightingAccumulator& sink;
            uint64_t             share_window_ms;

            bool                 shared_valid = false;
            uint64_t             shared_ms = 0;
            uint64_t             shared_generation = 0;
            std::vector<uint8_t> shared_reply;
        };

        static GroupStatus BuildStatus(const WirelessRuntime& rt);
        void PublishStatus(const Mac& mac, const WirelessRuntime& rt);
        void Wake();

        IWirelessLink&        link;
        IWirelessClock&       clock;
        WirelessRuntimeConfig cfg;
        SightingAccumulator   sightings;
        ObservingLink         observed;

        /* guards the runtime map; held across every runtime Tick */
        mutable std::mutex    mutex;
        std::map<Mac, std::unique_ptr<WirelessRuntime>> runtimes;

        /* small lock for the published per-group status snapshot */
        mutable std::mutex    status_mutex;
        std::map<Mac, GroupStatus> status_snapshot;
        Mac                   adopted_master{};         /* status_mutex */
        bool                  has_adopted_master = false; /* status_mutex */
        struct PendingUpload
        {
            bool queued = false;
            std::shared_ptr<const RgbUpload> upload;
            bool mode_selected = false;
            bool playback = false;
            uint64_t revision = 0;
        };
        std::mutex pending_mutex;
        std::map<Mac, PendingUpload> pending;
        std::thread           worker;
        std::atomic<bool>     stop{false};
        std::mutex            wake_mutex;
        std::condition_variable wake_cv;
        std::atomic<bool>     wake_requested{false};
    };
}
