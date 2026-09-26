// SPDX-License-Identifier: GPL-2.0-or-later
#include "LianLiWirelessService.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <mutex>
#include <thread>

using namespace LianLiWireless;

struct TestClock : IWirelessClock
{
    uint64_t now = 0;
    uint64_t NowMs() override { return now; }
};

struct BlockingLink : IWirelessLink
{
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    bool ReadMasterMac(Mac& out) override
    {
        out = {7, 8, 9, 10, 11, 12};
        return true;
    }
    bool PollDiscovery(std::vector<uint8_t>& out) override
    {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        cv.notify_all();
        cv.wait(lock, [this] { return release; });
        out = {USB_CMD_SEND_RF, 0, 0, 0};
        return true;
    }
    bool SendChunks(const std::array<UsbChunk, USB_CHUNKS_PER_PACKET>&) override { return true; }
};

struct FreshnessLink : IWirelessLink
{
    int polls = 0;
    int clock_reads = 0;
    bool clock_ok = true;
    uint32_t clock_ticks = 0xFFFFFFFEu;
    bool ReadMasterMac(Mac& out) override
    {
        out = {7, 8, 9, 10, 11, 12};
        return true;
    }
    bool PollDiscovery(std::vector<uint8_t>& out) override
    {
        ++polls;
        out.assign(4 + RECORD_SIZE, 0);
        out[0] = USB_CMD_SEND_RF;
        out[1] = 1;
        uint8_t* record = out.data() + 4;
        const Mac target = {1, 2, 3, 4, 5, 6};
        const Mac master = {7, 8, 9, 10, 11, 12};
        for(size_t i = 0; i < 6; ++i)
        {
            record[i] = target[i];
            record[6 + i] = master[i];
        }
        record[12] = 8;
        record[13] = 1;
        record[19] = 3;
        record[24] = record[25] = record[26] = 21;
        record[41] = RECORD_MARKER;
        return true;
    }
    bool SendChunks(const std::array<UsbChunk, USB_CHUNKS_PER_PACKET>&) override { return true; }
    bool ReadRgbClock(const Mac&, uint8_t, uint32_t& ticks) override
    {
        ++clock_reads;
        ticks = clock_ticks;
        return clock_ok;
    }
};

int main()
{
    TestClock clock;
    BlockingLink link;
    LianLiWirelessService service(link, clock);
    const Mac target = {1, 2, 3, 4, 5, 6};
    service.AddGroup(target, 3);
    const Mac second_target = {2, 2, 3, 4, 5, 6};
    service.AddGroup(second_target, 3);
    RgbTiming queued_timing;
    queued_timing.interval_ticks = 96;
    service.SetDesired(second_target, std::make_shared<RgbUpload>(
        std::vector<uint8_t>(12, 1), 120, 1, queued_timing));
    std::thread worker([&] { service.StepAll(); });
    {
        std::unique_lock<std::mutex> lock(link.mutex);
        link.cv.wait(lock, [&] { return link.entered; });
    }
    auto upload = std::shared_ptr<const RgbUpload>();
    auto submit = std::async(std::launch::async, [&] { service.SetDesired(target, upload); });
    service.BeginMode(second_target, true);
    const bool prompt = submit.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready;
    {
        std::lock_guard<std::mutex> lock(link.mutex);
        link.release = true;
    }
    link.cv.notify_all();
    worker.join();
    submit.wait();
    LianLiWirelessService::GroupStatus second_status;
    service.GetStatus(second_target, second_status);
    if(second_status.frame_count != 0)
    {
        std::puts("FAIL: drained old frame survived a concurrent mode transition");
        return 1;
    }
    if(!prompt)
    {
        std::puts("FAIL: submission waited on blocking transport I/O");
        return 1;
    }
    FreshnessLink fresh_link;
    TestClock fresh_clock;
    WirelessRuntime runtime(fresh_link, fresh_clock);
    runtime.SetTarget(target, 3);
    RgbTiming timing;
    timing.interval_ticks = 96;
    auto frame = std::make_shared<RgbUpload>(std::vector<uint8_t>(12, 1),
        120, 1, timing);
    runtime.Tick();
    for(int i = 0; i < 20; ++i)
    {
        fresh_clock.now += 100;
        runtime.SetDesired(frame);
        runtime.Tick();
    }
    if(fresh_link.polls < 2)
    {
        std::puts("FAIL: continuous frames postponed every discovery poll");
        return 1;
    }
    FreshnessLink clock_link;
    TestClock rgb_clock;
    WirelessRuntime playback(clock_link, rgb_clock);
    playback.SetTarget(target, 3);
    auto animation = std::make_shared<RgbUpload>(std::vector<uint8_t>(12, 1),
        120, 2, timing);
    playback.SetDesired(animation);
    playback.Tick();
    playback.Tick(); // the initial discovery precedes the first keep-alive
    auto snapshot = playback.ClockSnapshot();
    if(!snapshot.valid || snapshot.ticks != 0xFFFFFFFEu || clock_link.clock_reads != 1)
    {
        std::puts("FAIL: playback clock was not sampled through worker");
        return 1;
    }
    rgb_clock.now = 1;
    playback.SetDesired(animation);
    if(playback.NextDeadlineMs() != 25)
    {
        std::puts("FAIL: next stream deadline ignores the 25 ms admission budget");
        return 1;
    }
    rgb_clock.now = 3000;
    clock_link.clock_ticks = 2;
    playback.Tick();
    if(!playback.ClockSnapshot().valid || playback.ClockSnapshot().ticks != 2)
    {
        std::puts("FAIL: wrapping clock was rejected");
        return 1;
    }
    rgb_clock.now = 6000;
    clock_link.clock_ticks = 1;
    playback.Tick();
    if(playback.ClockSnapshot().valid)
    {
        std::puts("FAIL: clock reboot was reported as continuous");
        return 1;
    }
    FreshnessLink status_link;
    TestClock status_clock;
    LianLiWirelessService status_service(status_link, status_clock);
    status_service.AddGroup(target, 3);
    status_service.SetDesired(target, animation);
    status_service.StepAll();
    status_service.StepAll();
    LianLiWirelessService::GroupStatus status;
    if(!status_service.GetStatus(target, status)
       || status.frame_count != 2
       || status.interval_hundredths != 9600
       || status.effective_cycle_ms != 120.0
       || status.nominal_fps < 16.6
       || !status.rgb_clock.valid)
    {
        std::puts("FAIL: read-only playback metadata is incomplete");
        return 1;
    }
    status_service.BeginMode(target, true);
    status_service.StepAll();
    status_service.GetStatus(target, status);
    if(status.frame_count != 0 || status.rgb_clock.valid)
    {
        std::puts("FAIL: old loop metadata survived mode invalidation");
        return 1;
    }
    // A live frame queued before or during a playback transition must not
    // replace the new loop. An older compile of the same preset must not win.
    auto live = std::make_shared<RgbUpload>(std::vector<uint8_t>(12, 2),
        120, 1, timing);
    status_service.SetDesired(target, live);
    const auto old_revision = status_service.BeginMode(target, true);
    status_service.SetDesired(target, live, old_revision);
    const auto new_revision = status_service.BeginMode(target, true);
    status_service.SetDesired(target, animation, old_revision);
    status_service.SetDesired(target, animation, new_revision);
    status_service.StepAll();
    if(!status_service.GetStatus(target, status) || status.frame_count != 2)
    {
        std::puts("FAIL: stale live frame or compile replaced playback");
        return 1;
    }
    const auto live_revision = status_service.BeginMode(target, false);
    status_service.SetDesired(target, animation, new_revision);
    status_service.SetDesired(target, live, live_revision);
    status_service.StepAll();
    if(!status_service.GetStatus(target, status) || status.frame_count != 1)
    {
        std::puts("FAIL: stale playback replaced live mode");
        return 1;
    }
    rgb_clock.now = 8000;
    if(playback.ClockSnapshot().valid)
    {
        std::puts("FAIL: old clock sample was reported as current");
        return 1;
    }
    rgb_clock.now = 9000;
    clock_link.clock_ok = false;
    playback.Tick();
    if(playback.ClockSnapshot().valid)
    {
        std::puts("FAIL: timed out clock was reported as current");
        return 1;
    }
    std::puts("service pending: submission stayed responsive");
    return 0;
}
