/*---------------------------------------------------------*\
|| lianli_diagnostics_test.cpp                               ||
||                                                           ||
||   Offline tests for the bounded Lian Li wireless         ||
||   performance diagnostics: counter plumbing across       ||
||   service submissions, runtime admission, discovery,     ||
||   heartbeat, compression (via the controller), and      ||
||   the capped CSV trace. No hardware, no Qt.               ||
\*---------------------------------------------------------*/

#include "LianLiWirelessProtocol.h"
#include "LianLiWirelessCodec.h"
#include "LianLiWirelessRuntime.h"
#include "LianLiWirelessService.h"
#include "LianLiWirelessDiagnostics.h"
#include "LianLiWirelessController.h"
#include "LianLiWirelessFanType.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

using namespace LianLiWireless;

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, msg) do { checks++; if(!(cond)) { failures++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); } } while(0)

static Diagnostics& DIAG = Diagnostics::Get();

static Mac MAC_TARGET = { 0x9b, 0x10, 0x76, 0xe5, 0x66, 0xe1 };
static Mac MAC_MASTER = { 0xfa, 0xc0, 0x87, 0xe5, 0x66, 0xe4 };
static Mac MAC_FOREIGN = { 0xde, 0xad, 0xbe, 0xef, 0x00, 0x01 };

static void MakeRecord(uint8_t* r, const Mac& mac, const Mac& master,
                       uint8_t channel = 8, uint8_t receiver = 1,
                       const uint8_t* effect = nullptr)
{
    memset(r, 0, RECORD_SIZE);
    memcpy(r + 0, mac.data(), 6);
    memcpy(r + 6, master.data(), 6);
    r[12] = channel; r[13] = receiver;
    r[18] = 0x00;
    r[19] = 3;
    static const uint8_t def_effect[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
    memcpy(r + 20, effect ? effect : def_effect, 4);
    r[24] = 21; r[25] = 21; r[26] = 21;
    r[41] = RECORD_MARKER;
}

static std::vector<uint8_t> MakeDiscovery(const Mac& mac = MAC_TARGET,
                                          const Mac& master = MAC_MASTER,
                                          const uint8_t* effect = nullptr)
{
    std::vector<uint8_t> resp(4 + RECORD_SIZE, 0);
    resp[0] = USB_CMD_SEND_RF;
    resp[1] = 1;
    MakeRecord(resp.data() + 4, mac, master, 8, 1, effect);
    return resp;
}

static RgbTiming Timing96() { RgbTiming t; t.interval_ticks = 96; return t; }

static RgbUpload MakeUpload(uint8_t fill = 0x30)
{
    std::vector<uint8_t> rgb(360, fill);
    return RgbUpload(LianLiWirelessCodec::Compress(rgb.data(), rgb.size()),
                     120, 1, Timing96());
}

struct FakeClock : IWirelessClock
{
    uint64_t now = 0;
    uint64_t NowMs() override { return now; }
    void Advance(uint64_t ms) { now += ms; }
};

struct FakeLink : IWirelessLink
{
    std::function<std::vector<uint8_t>()> poll_fn;
    bool fail_sends  = false;
    Mac master_mac  = MAC_MASTER;
    int  rgb_sends  = 0;
    uint64_t ConnectionGeneration() const override { return 1; }

    bool ReadMasterMac(Mac& out) override
    {
        out = master_mac;
        return true;
    }
    bool PollDiscovery(std::vector<uint8_t>& out) override
    {
        out = poll_fn ? poll_fn() : std::vector<uint8_t>{};
        return true;
    }
    bool SendChunks(const std::array<UsbChunk, USB_CHUNKS_PER_PACKET>& c) override
    {
        if(fail_sends) return false;
        if(c[0][3] != BROADCAST_RECEIVER) rgb_sends++;
        return true;
    }
};

static void RunFor(WirelessRuntime& rt, FakeClock& clk, uint64_t ms,
                   uint64_t step = 100)
{
    for(uint64_t t = 0; t < ms; t += step) { clk.Advance(step); rt.Tick(); }
}

/*-------------------------------------------------------------*\
|| 1. Counter basics: reset + direct record calls              ||
\*-------------------------------------------------------------*/
static void TestCounterBasics()
{
    DIAG.Reset();
    DiagCounters c = DIAG.Snapshot();
    CHECK(c.set_desired == 0 && c.uploads_admitted == 0 && c.keepalives == 0,
          "reset zeroes counters");

    DIAG.RecordCompress(100, 360, 200);
    DIAG.RecordUsbSend(4, 6500);
    DIAG.RecordUsbRead(1200);
    DIAG.RecordTransportRevalidation();
    DIAG.RecordForeignMaster();
    DIAG.RecordUploadFailure();
    DIAG.RecordKeepAliveFailure();

    c = DIAG.Snapshot();
    CHECK(c.compress_calls == 1 && c.compress_us == 100
          && c.compress_bytes_in == 360 && c.compress_bytes_out == 200,
          "compression counters record time and bytes");
    CHECK(c.usb_chunk_batches == 1 && c.usb_chunks == 4
          && c.usb_send_us == 6500 && c.usb_send_us_max == 6500,
          "usb send counters record batches, chunks, duration");
    CHECK(c.usb_reads == 1 && c.usb_read_us == 1200,
          "usb read counters record duration");
    CHECK(c.transport_revalidations == 1 && c.foreign_master_events == 1,
          "reset/ownership counters record");
    CHECK(c.upload_failures == 1 && c.keepalive_failures == 1,
          "failure counters record");

    DIAG.Reset();
    c = DIAG.Snapshot();
    CHECK(c.compress_calls == 0 && c.usb_reads == 0,
          "second reset zeroes counters");
}

/*-------------------------------------------------------------*\
|| 2. Service submissions: accepted, rejected, step            ||
\*-------------------------------------------------------------*/
static void TestServiceSubmissionCounters()
{
    DIAG.Reset();
    FakeClock clk;
    FakeLink  link;
    LianLiWirelessService svc(link, clk);

    CHECK(svc.AddGroup(MAC_TARGET, 3), "service tracks group");

    auto up  = std::make_shared<RgbUpload>(MakeUpload(0x30));
    auto up2 = std::make_shared<RgbUpload>(MakeUpload(0x70));
    svc.SetDesired(MAC_TARGET, up);
    svc.SetDesired(MAC_TARGET, up2);          /* replaces unacked pending */
    svc.SetDesired(MAC_FOREIGN, up);          /* unknown group */

    DiagCounters c = DIAG.Snapshot();
    CHECK(c.set_desired == 2, "accepted submissions counted");
    CHECK(c.set_desired_rejected == 1, "unknown-group submission rejected");
    CHECK(c.replaced_pending == 1, "second frame replaced unacked pending");

    link.poll_fn = [] { return MakeDiscovery(); };
    for(int i = 0; i < 5; i++) { clk.Advance(100); svc.StepAll(); }

    c = DIAG.Snapshot();
    CHECK(c.step_all == 5, "worker steps counted");
    CHECK(c.step_all_us >= c.step_all_us_max, "step durations bounded by total");
    CHECK(c.lock_wait_us_max >= 0, "lock wait recorded");
}

/*-------------------------------------------------------------*\
|| 3. Replaced-pending: only unconfirmed frames count          ||
\*-------------------------------------------------------------*/
static void TestReplacedPendingOnlyWhenUnconfirmed()
{
    DIAG.Reset();
    FakeClock clk;
    FakeLink  link;
    LianLiWirelessService svc(link, clk);
    CHECK(svc.AddGroup(MAC_TARGET, 3), "service tracks group");

    auto up = std::make_shared<RgbUpload>(MakeUpload(0x30));
    link.poll_fn = [&] { return MakeDiscovery(MAC_TARGET, MAC_MASTER,
        link.rgb_sends > 0 ? up->EffectId().data() : nullptr); };

    svc.SetDesired(MAC_TARGET, up);
    for(int i = 0; i < 30; i++) { clk.Advance(100); svc.StepAll(); }

    LianLiWirelessService::GroupStatus st;
    CHECK(svc.GetStatus(MAC_TARGET, st) && st.confirmed,
          "fixture converged before replacement check");
    DiagCounters c = DIAG.Snapshot();
    CHECK(c.replaced_pending == 0, "first submission is not a replacement");

    /* replace while confirmed: a normal update, not backpressure */
    auto up2 = std::make_shared<RgbUpload>(MakeUpload(0x70));
    svc.SetDesired(MAC_TARGET, up2);
    c = DIAG.Snapshot();
    CHECK(c.replaced_pending == 0,
          "replacing a confirmed frame is not counted as pending replacement");

    /* replace while the new frame is still unacked: pending replacement */
    svc.SetDesired(MAC_TARGET, std::make_shared<RgbUpload>(MakeUpload(0x90)));
    c = DIAG.Snapshot();
    CHECK(c.replaced_pending == 1,
          "replacing an unconfirmed frame counts as pending replacement");
}

/*-------------------------------------------------------------*\
|| 4. Runtime admission: stream budget defers, then admits    ||
\*-------------------------------------------------------------*/
static void TestAdmissionDeferral()
{
    DIAG.Reset();
    FakeClock clk;
    FakeLink  link;
    WirelessRuntime rt(link, clk);
    rt.SetTarget(MAC_TARGET, 3);
    link.poll_fn = [] { return MakeDiscovery(); };
    rt.Tick();                                /* boot + sighting + keep-alive */

    DIAG.Reset();
    rt.SetDesired(std::make_shared<RgbUpload>(MakeUpload(0x30)));
    clk.Advance(25); rt.Tick();               /* budget clear: admitted */
    DiagCounters c = DIAG.Snapshot();
    CHECK(c.uploads_admitted == 1 && c.rf_packets >= 1,
          "first frame admitted with rf packets recorded");
    CHECK(c.header_repeats == 1, "header repeat counted once per upload");

    rt.SetDesired(std::make_shared<RgbUpload>(MakeUpload(0x70)));
    clk.Advance(24); rt.Tick();               /* 1 ms short of the budget */
    c = DIAG.Snapshot();
    CHECK(c.uploads_admitted == 1 && c.uploads_deferred >= 1,
          "frame inside the RF stream budget is deferred, not sent");

    clk.Advance(1); rt.Tick();                /* budget elapsed: admitted */
    c = DIAG.Snapshot();
    CHECK(c.uploads_admitted == 2 && c.uploads_deferred >= 1,
          "deferred frame is admitted once the budget elapses");
}

/*-------------------------------------------------------------*\
|| 5. Discovery counters: polls, records, age, misses         ||
\*-------------------------------------------------------------*/
static void TestDiscoveryCounters()
{
    DIAG.Reset();
    FakeClock clk;
    FakeLink  link;
    WirelessRuntime rt(link, clk);
    rt.SetTarget(MAC_TARGET, 3);
    link.poll_fn = [] { return MakeDiscovery(); };

    RunFor(rt, clk, 2000);
    DiagCounters c = DIAG.Snapshot();
    CHECK(c.discovery_polls >= 3, "polls counted");
    CHECK(c.discovery_records >= 3, "sighting records counted");
    CHECK(c.discovery_misses == 0, "present target is not a miss");
    CHECK(c.discovery_age_ms >= 0 && c.discovery_age_ms_max <= 500,
          "sighting age recorded and bounded by poll cadence");

    DIAG.Reset();
    link.poll_fn = [] { return std::vector<uint8_t>{}; };
    RunFor(rt, clk, 1000);
    c = DIAG.Snapshot();
    CHECK(c.discovery_polls >= 1 && c.discovery_misses == c.discovery_polls,
          "absent target counts every poll as a miss");
}

/*-------------------------------------------------------------*\
|| 6. Discovery deferral: fresh frames postpone the poll      ||
\*-------------------------------------------------------------*/
static void TestDiscoveryDeferral()
{
    DIAG.Reset();
    FakeClock clk;
    FakeLink  link;
    WirelessRuntime rt(link, clk);
    rt.SetTarget(MAC_TARGET, 3);
    link.poll_fn = [] { return MakeDiscovery(); };
    rt.Tick();
    CHECK(rt.State() == WirelessState::Holding, "target validated");

    DIAG.Reset();
    clk.Advance(400);                 /* normal holding poll is due */
    rt.SetDesired(std::make_shared<RgbUpload>(MakeUpload(0x30)));
    rt.Tick();

    DiagCounters c = DIAG.Snapshot();
    CHECK(c.discovery_deferred == 1,
          "fresh frame with a due poll records a discovery deferral");
}

/*-------------------------------------------------------------*\
|| 7. Heartbeat: keep-alive count and gap                      ||
\*-------------------------------------------------------------*/
static void TestKeepaliveGap()
{
    DIAG.Reset();
    FakeClock clk;
    FakeLink  link;
    WirelessRuntime rt(link, clk);
    rt.SetTarget(MAC_TARGET, 3);
    link.poll_fn = [] { return MakeDiscovery(); };

    RunFor(rt, clk, 2000);
    DiagCounters c = DIAG.Snapshot();
    /* keep-alive every 750 ms on a 100 ms tick: t=100, 900, 1700 */
    CHECK(c.keepalives == 3, "keep-alives counted");
    CHECK(c.keepalive_gap_ms_max >= 700 && c.keepalive_gap_ms_max <= 850,
          "keep-alive gap recorded near the 750 ms schedule");
}

/*-------------------------------------------------------------*\
|| 8. Upload retries and failure budget                        ||
\*-------------------------------------------------------------*/
static void TestUploadRetryAndFailure()
{
    DIAG.Reset();
    FakeClock clk;
    FakeLink  link;
    WirelessRuntime rt(link, clk);
    rt.SetTarget(MAC_TARGET, 3);
    rt.SetDesired(std::make_shared<RgbUpload>(MakeUpload()));
    link.poll_fn = [] { return MakeDiscovery(); };   /* never acks */

    RunFor(rt, clk, 30000);
    CHECK(rt.State() == WirelessState::Failed, "retry budget exhausted");
    DiagCounters c = DIAG.Snapshot();
    CHECK(c.uploads_admitted >= 8, "resend budget worth of uploads admitted");
    CHECK(c.upload_retries >= 7, "repeat sends of the same upload counted");
    CHECK(c.upload_failures >= 1, "budget exhaustion recorded as failure");
}

/*-------------------------------------------------------------*\
|| 9. Compression via the device-side controller               ||
\*-------------------------------------------------------------*/
static void TestCompressionViaController()
{
    DIAG.Reset();
    FakeClock clk;
    FakeLink  link;
    LianLiWirelessService svc(link, clk);

    auto owner = std::make_shared<int>(1);
    CHECK(svc.AddGroup(MAC_TARGET, 3), "service tracks group for controller");
    WirelessFanInfo info = ClassifyFanType(21);        /* SL V3, 40 LEDs */
    LianLiWirelessController ctl(owner, svc, MAC_TARGET, 3, info);

    std::vector<RGBColor> colors(120, ToRGBColor(0xFF, 0x00, 0x00));
    ctl.SetLEDs(colors.data(), colors.size());

    DiagCounters c = DIAG.Snapshot();
    CHECK(c.compress_calls == 1 && c.compress_bytes_in == 360,
          "SetLEDs compression recorded with input size");
    CHECK(c.compress_bytes_out > 0, "compressed size recorded");
    CHECK(c.set_desired == 1 && c.set_desired_rejected == 0,
          "controller submission counted");

    std::vector<uint8_t> frames(120 * 3 * 2, 0x40);
    ctl.SetFrames(frames, 2, 50);
    c = DIAG.Snapshot();
    CHECK(c.compress_calls == 2 && c.compress_bytes_in == 360 + 720,
          "SetFrames compression recorded separately");
}

/*-------------------------------------------------------------*\
|| 10. Trace CSV: bounded rows, cap marker, summary file       ||
\*-------------------------------------------------------------*/
static void TestTraceCapAndSummary()
{
    DIAG.Reset();
    DIAG.CloseTrace();

    const std::string path  = "lianli_diagnostics_test_trace.csv";
    const std::string sum   = path + ".summary";

    DIAG.OpenTrace(path, 5);
    CHECK(DIAG.TraceOpen(), "trace opens on demand");

    FakeClock clk;
    FakeLink  link;
    WirelessRuntime rt(link, clk);
    rt.SetTarget(MAC_TARGET, 3);
    link.poll_fn = [] { return MakeDiscovery(); };
    for(int i = 0; i < 24; i++) { clk.Advance(100); rt.Tick(); }

    CHECK(DIAG.TraceRows() == 5, "trace rows stop at the cap");
    DIAG.CloseTrace();
    CHECK(!DIAG.TraceOpen(), "trace closes on demand");

    {
        std::ifstream trace(path);
        CHECK(trace.is_open(), "trace file exists");
        unsigned rows = 0;
        bool saw_header  = false;
        bool saw_capped  = false;
        std::string line;
        while(std::getline(trace, line))
        {
            if(line.rfind("time_us,event,", 0) == 0)      saw_header = true;
            else if(line.find(",trace_capped,") != std::string::npos) saw_capped = true;
            else if(!line.empty())                        rows++;
        }
        CHECK(saw_header, "trace file has the CSV header");
        CHECK(saw_capped, "trace file records the row cap");
        CHECK(rows == 5, "five data rows before the cap marker");
    }
    {
        std::ifstream summary(sum);
        CHECK(summary.is_open(), "summary file exists beside the trace");
        std::string line;
        bool saw_counters = false;
        bool saw_rows     = false;
        bool saw_capped   = false;
        while(std::getline(summary, line))
        {
            if(line.rfind("counter,value", 0) == 0)            saw_counters = true;
            else if(line.rfind("trace_rows,", 0) == 0)         saw_rows = true;
            else if(line.rfind("trace_capped,1", 0) == 0)      saw_capped = true;
        }
        CHECK(saw_counters, "summary file has the counter header");
        CHECK(saw_rows && saw_capped, "summary reports capped row count");
    }

    std::remove(path.c_str());
    std::remove(sum.c_str());

    /* counters keep accumulating with no trace attached */
    DIAG.Reset();
    for(int i = 0; i < 8; i++) { clk.Advance(100); rt.Tick(); }
    CHECK(DIAG.Snapshot().discovery_polls >= 1,
          "counters accumulate without a trace file");
}

int main()
{
    TestCounterBasics();
    TestServiceSubmissionCounters();
    TestReplacedPendingOnlyWhenUnconfirmed();
    TestAdmissionDeferral();
    TestDiscoveryCounters();
    TestDiscoveryDeferral();
    TestKeepaliveGap();
    TestUploadRetryAndFailure();
    TestCompressionViaController();
    TestTraceCapAndSummary();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
