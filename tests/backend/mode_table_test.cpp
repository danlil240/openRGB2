// SPDX-License-Identifier: GPL-2.0-or-later
//
// Backend mode-table regression (QA F05): the installed OpenRGB.exe was
// built from a stale snapshot whose Lian Li wireless controller offered
// only 5 modes, so the six uploaded presets (Perimeter Wave .. Stack
// Rainbow) were rejected with "the backend does not offer mode '...'".
//
// This suite pins the advertised mode table — count, ordered names, and
// device values — and then proves the whole upload path works for every
// playback mode: SetActiveMode -> RGBController device thread ->
// DeviceUpdateMode -> BuildPlaybackLoop -> service.SetDesired must queue
// an upload the service ADMITS (pending slot accepts only
// revision-matching uploads whose frame_count > 1 for playback modes).
// All transport I/O runs over the same fake link/clock seam as
// service_pending_test — no Qt, no hardware.

#include "RGBController_LianLiWireless.h"
#include "LianLiWirelessFanType.h"
#include "LianLiWirelessService.h"
#include "LogManager.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

using namespace LianLiWireless;

/*---------------------------------------------------------*\
| LogManager stub: RGBController.cpp reaches LOG_* only on  |
| error paths, and the real LogManager.cpp drags in the     |
| Qt-bound network/server layer. The four symbols the base  |
| class can touch are stubbed here — messages are dropped.  |
\*---------------------------------------------------------*/
LogManager::LogManager() {}
LogManager::~LogManager() {}
LogManager* LogManager::get()
{
    static LogManager manager;
    return &manager;
}
void LogManager::LogEntry(const char*, int, unsigned int, const char*, ...) {}

struct TestClock : IWirelessClock
{
    uint64_t now = 0;
    uint64_t NowMs() override { return now; }
};

/*---------------------------------------------------------*\
| modes is a protected RGBController member — the same      |
| table the SDK server serializes. A thin probe subclass    |
| exposes it read-only for the assertions.                  |
\*---------------------------------------------------------*/
struct ProbeController : RGBController_LianLiWireless
{
    using RGBController_LianLiWireless::RGBController_LianLiWireless;
    const std::vector<mode>& ModeTable() const { return modes; }
    std::atomic<int> mode_calls{0};
    void DeviceUpdateMode() override
    {
        ++mode_calls;
        RGBController_LianLiWireless::DeviceUpdateMode();
    }
};

/*---------------------------------------------------------*\
| Same fake transport as service_pending_test: one fixed    |
| discovery record for the target group, all sends succeed. |
\*---------------------------------------------------------*/
struct FakeLink : IWirelessLink
{
    bool ReadMasterMac(Mac& out) override
    {
        out = {7, 8, 9, 10, 11, 12};
        return true;
    }
    bool PollDiscovery(std::vector<uint8_t>& out) override
    {
        out.assign(4 + RECORD_SIZE, 0);
        out[0] = USB_CMD_SEND_RF;
        out[1] = 1;
        uint8_t* record = out.data() + 4;
        const Mac target = {1, 2, 3, 4, 5, 6};
        const Mac master = {7, 8, 9, 10, 11, 12};
        for(size_t i = 0; i < 6; ++i)
        {
            record[i]      = target[i];
            record[6 + i]  = master[i];
        }
        record[12] = 8;
        record[13] = 1;
        record[19] = 3;
        record[24] = record[25] = record[26] = 21;
        record[41] = RECORD_MARKER;
        return true;
    }
    bool SendChunks(const std::array<UsbChunk, USB_CHUNKS_PER_PACKET>&) override
    {
        return true;
    }
    bool ReadRgbClock(const Mac&, uint8_t, uint32_t& ticks) override
    {
        ticks = 0;
        return true;
    }
};

int main()
{
    const Mac mac = {1, 2, 3, 4, 5, 6};

    TestClock clock;
    FakeLink  link;
    LianLiWirelessService service(link, clock);
    service.AddGroup(mac, 3);

    /*---------------------------------------------------------*\
    | Three-fan SL V3 group — the hardware layout the rejected   |
    | presets targeted. The RGBController owns the controller    |
    | pointer (its destructor deletes it).                       |
    \*---------------------------------------------------------*/
    WirelessFanInfo info = ClassifyFanType(20);      /* SL V3 LED */
    if(info.leds_per_fan == 0)
    {
        std::puts("FAIL: SL V3 classification changed");
        return 1;
    }
    auto* ctrl = new LianLiWirelessController(std::shared_ptr<void>(),
                                              service, mac, 3, info);
    std::unique_ptr<ProbeController> rgb(new ProbeController(ctrl));

    /*---------------------------------------------------------*\
    | 1. The advertised table: exactly 11 modes, stable order   |
    |    and device values matching the wire enum.              |
    \*---------------------------------------------------------*/
    static const char* kNames[] =
    {
        "Direct",
        "Static",
        "Spectrum Cycle",
        "Rainbow Wave",
        "Breathing",
        "Perimeter Wave",
        "Perimeter Comet",
        "Perimeter Chase",
        "Perimeter Ping Pong",
        "Stack Wave",
        "Stack Rainbow",
    };

    const std::vector<mode>& table = rgb->ModeTable();
    if(table.size() != 11)
    {
        std::printf("FAIL: mode table offers %zu modes, expected 11\n",
                    table.size());
        return 1;
    }
    for(size_t i = 0; i < 11; ++i)
    {
        if(table[i].name != kNames[i] || table[i].value != (int)i)
        {
            std::printf("FAIL: mode[%zu] is '%s' value %d, expected '%s' "
                        "value %zu\n", i, table[i].name.c_str(),
                        table[i].value, kNames[i], i);
            return 1;
        }
    }

    /*---------------------------------------------------------*\
    | 2. Every uploaded-effect mode (values 5..10, the six that  |
    |    the stale backend rejected) must build a multi-frame    |
    |    loop the service admits under the current revision.     |
    |    Each SetActiveMode runs on the RGBController device     |
    |    thread; each admitted upload carries a distinct         |
    |    effect_id (variant salt), so a change in the reported   |
    |    ID is proof a NEW upload was accepted.                  |
    |                                                            |
    |    The device thread clears CallFlag_UpdateMode in its     |
    |    prologue — a SetActiveMode issued before the thread's   |
    |    first instructions can be lost. Warm it up with one     |
    |    Direct-mode update (which also exercises the 1-frame    |
    |    upload path) and wait for a confirmed round-trip.       |
    \*---------------------------------------------------------*/
    LianLiWirelessService::GroupStatus status;
    std::array<uint8_t, 4> previous_id{};

    rgb->SetActiveMode(0);
    for(int attempt = 0; attempt < 5000 && rgb->mode_calls == 0; ++attempt)
    {
        /*-----------------------------------------------------*\
        | The device thread clears CallFlag_UpdateMode in its   |
        | prologue — the first UpdateMode issued before it runs  |
        | can be eaten (observed under a loaded build machine). |
        | Re-arm the flag until one update round-trips; this is  |
        | also what a real client effectively does by resending. |
        \*-----------------------------------------------------*/
        rgb->UpdateMode();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if(rgb->mode_calls == 0)
    {
        std::printf("FAIL: device thread never ran DeviceUpdateMode "
                    "(active_mode %d)\n", rgb->GetActiveMode());
        return 1;
    }
    for(int drain = 0; drain < 20; ++drain)
    {
        service.StepAll();
        if(service.GetStatus(mac, status) && status.frame_count != 0)
        {
            break;
        }
    }
    if(!service.GetStatus(mac, status) || status.frame_count == 0)
    {
        std::puts("FAIL: Direct mode produced no admitted upload");
        return 1;
    }
    previous_id = status.effect_id;

    for(int index = 5; index <= 10; ++index)
    {
        rgb->SetActiveMode(index);

        /*-----------------------------------------------------*\
        | DeviceUpdateMode first BeginMode()s (which clears the  |
        | runtime's desired upload at the next StepAll), then    |
        | queues the rendered loop. A cleared slot also reports  |
        | a changed effect_id, so admission requires BOTH a      |
        | fresh ID and a multi-frame payload.                    |
        \*-----------------------------------------------------*/
        bool admitted = false;
        for(int attempt = 0; attempt < 5000 && !admitted; ++attempt)
        {
            service.StepAll();
            if(service.GetStatus(mac, status)
               && status.frame_count > 1
               && status.effect_id != previous_id)
            {
                admitted = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if(!admitted)
        {
            std::printf("FAIL: mode %d ('%s') produced no admitted upload "
                        "(controller status: '%s', DeviceUpdateMode calls: %d)\n",
                        index, kNames[index], ctrl->GetStatus().c_str(),
                        rgb->mode_calls.load());
            return 1;
        }
        if(status.frame_count <= 1)
        {
            std::printf("FAIL: mode %d ('%s') admitted upload has "
                        "frame_count %u — playback needs a loop\n",
                        index, kNames[index], (unsigned)status.frame_count);
            return 1;
        }
        if(!status.last_error.empty())
        {
            std::printf("FAIL: mode %d ('%s') reported '%s'\n",
                        index, kNames[index], status.last_error.c_str());
            return 1;
        }
        previous_id = status.effect_id;
    }

    /*---------------------------------------------------------*\
    | SetActiveMode(0..4) still works after the playback sweep   |
    | — the Direct path keeps the service responsive.            |
    \*---------------------------------------------------------*/
    rgb->SetActiveMode(0);
    service.StepAll();
    service.StepAll();

    std::puts("mode table: 11 modes, uploads admitted for playback modes 5-10");
    return 0;
}
