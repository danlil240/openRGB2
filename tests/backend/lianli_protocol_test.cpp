/*---------------------------------------------------------*\
| lianli_protocol_test.cpp                                  |
|                                                           |
|   Deterministic offline tests for the Lian Li wireless    |
|   protocol, codec, and runtime core. No hardware, no Qt,  |
|   no vendor DLLs required (yuz.dll cross-check is         |
|   optional and skips cleanly when absent).                |
\*---------------------------------------------------------*/

#include "LianLiWirelessProtocol.h"
#include "LianLiWirelessCodec.h"
#include "LianLiWirelessRuntime.h"
#include "LianLiWirelessService.h"
#include "LianLiWirelessEffects.h"
#include "LianLiWirelessFanType.h"

#include <cstdio>
#include <cstring>
#include <functional>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace LianLiWireless;

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, msg) do { checks++; if(!(cond)) { failures++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); } } while(0)

static Mac MAC_TARGET = { 0x9b, 0x10, 0x76, 0xe5, 0x66, 0xe1 };
static Mac MAC_MASTER = { 0xfa, 0xc0, 0x87, 0xe5, 0x66, 0xe4 };
static Mac MAC_FOREIGN = { 0xde, 0xad, 0xbe, 0xef, 0x00, 0x01 };

/*-------------------------------------------------------------*\
| Helpers: build a valid 42-byte discovery record + response    |
\*-------------------------------------------------------------*/
static void MakeRecord(uint8_t* r, const Mac& mac, const Mac& master,
                       uint8_t channel = 8, uint8_t receiver = 1,
                       const uint8_t* effect = nullptr)
{
    memset(r, 0, RECORD_SIZE);
    memcpy(r + 0, mac.data(), 6);
    memcpy(r + 6, master.data(), 6);
    r[12] = channel; r[13] = receiver;
    r[14] = 0x00; r[15] = 0x00; r[16] = 0x10; r[17] = 0x20;  /* ticks */
    r[18] = 0x00;                                          /* type: fan */
    r[19] = 3;                                             /* 3 fans    */
    static const uint8_t def_effect[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
    memcpy(r + 20, effect ? effect : def_effect, 4);
    r[24] = 21; r[25] = 21; r[26] = 21;                    /* SLV3 LED  */
    r[28] = 0x03; r[29] = 0xD0;                            /* RPM 976   */
    r[36] = 6; r[37] = 6; r[38] = 6; r[39] = 6;            /* PWM       */
    r[40] = 0x2A;                                          /* cmd_seq   */
    r[41] = RECORD_MARKER;
}

static std::vector<uint8_t> MakeDiscovery(const Mac& mac = MAC_TARGET,
                                          const Mac& master = MAC_MASTER,
                                          const uint8_t* effect = nullptr,
                                          uint8_t channel = 8, uint8_t receiver = 1)
{
    std::vector<uint8_t> resp(4 + RECORD_SIZE, 0);
    resp[0] = USB_CMD_SEND_RF;
    resp[1] = 1;
    MakeRecord(resp.data() + 4, mac, master, channel, receiver, effect);
    return resp;
}

static RgbTiming Timing96() { RgbTiming t; t.interval_ticks = 96; return t; }

static RgbUpload MakeUpload(uint8_t fill = 0x30)
{
    std::vector<uint8_t> rgb(360, fill);
    return RgbUpload(LianLiWirelessCodec::Compress(rgb.data(), rgb.size()),
                     120, 1, Timing96());
}

/*-------------------------------------------------------------*\
| Fake clock + link for runtime tests                           |
\*-------------------------------------------------------------*/
struct FakeClock : IWirelessClock
{
    uint64_t now = 0;
    uint64_t NowMs() override { return now; }
    void Advance(uint64_t ms) { now += ms; }
};

struct FakeLink : IWirelessLink
{
    /* scripted discovery responses; empty vector = absent target */
    std::function<std::vector<uint8_t>()> poll_fn;
    bool fail_polls   = false;
    bool fail_sends   = false;
    bool fail_master  = false;
    bool fail_rgb     = false;
    Mac master_mac   = MAC_MASTER;
    int master_reads = 0;
    int poll_calls   = 0;
    uint64_t generation = 1;
    uint64_t ConnectionGeneration() const override { return generation; }
    int  send_calls   = 0;
    int  clock_sends  = 0;      /* receiver==0xFF chunks          */
    int  rgb_sends    = 0;      /* receiver!=0xFF chunks          */
    std::vector<std::array<UsbChunk, USB_CHUNKS_PER_PACKET>> sent;

    bool ReadMasterMac(Mac& out) override
    {
        master_reads++;
        out = master_mac;
        return !fail_master;
    }
    bool PollDiscovery(std::vector<uint8_t>& out) override
    {
        poll_calls++;
        if(fail_polls) return false;
        out = poll_fn ? poll_fn() : std::vector<uint8_t>{};
        return true;
    }
    bool SendChunks(const std::array<UsbChunk, USB_CHUNKS_PER_PACKET>& c) override
    {
        send_calls++;
        if(fail_sends) return false;
        if(fail_rgb && c[0][3] != BROADCAST_RECEIVER) return false;
        sent.push_back(c);
        if(c[0][3] == BROADCAST_RECEIVER) clock_sends++; else rgb_sends++;
        return true;
    }
};

static void RunFor(WirelessRuntime& rt, FakeClock& clk, uint64_t ms, uint64_t step = 100)
{
    for(uint64_t t = 0; t < ms; t += step) { clk.Advance(step); rt.Tick(); }
}

/*-------------------------------------------------------------*\
| 1. Protocol: RGB packet layout, chunk boundaries, addressing  |
\*-------------------------------------------------------------*/
static void TestProtocol()
{
    /* header + payload spanning */
    std::vector<uint8_t> data(251);
    for(size_t i = 0; i < data.size(); i++) data[i] = (uint8_t)i;
    RgbTiming t96 = Timing96();
    RgbUpload up(data, 120, 1, t96);
    CHECK(up.PacketCount() == 3, "251 bytes -> header + 2 chunks");

    RfPacket p0 = up.Packet(0, MAC_TARGET, MAC_MASTER);
    CHECK(p0[0] == RF_SELECT && p0[1] == RF_SET_RGB, "RGB opcodes");
    CHECK(memcmp(&p0[2], MAC_TARGET.data(), 6) == 0, "target MAC in header");
    CHECK(memcmp(&p0[8], MAC_MASTER.data(), 6) == 0, "master MAC in header");
    CHECK(p0[20] == 0 && p0[21] == 0 && p0[22] == 0 && p0[23] == 251, "payload length BE32");
    CHECK(p0[25] == 0 && p0[26] == 1 && p0[27] == 120, "frames + LED count");
    CHECK(p0[32] == 0 && p0[33] == 96, "interval ticks BE16");

    /* payload reassembly across chunk boundary */
    RfPacket p1 = up.Packet(1, MAC_TARGET, MAC_MASTER);
    RfPacket p2 = up.Packet(2, MAC_TARGET, MAC_MASTER);
    std::vector<uint8_t> reassembled(p1.begin() + 20, p1.end());
    reassembled.insert(reassembled.end(), p2.begin() + 20, p2.begin() + 20 + 31);
    CHECK(reassembled == data, "chunk reassembly preserves payload");
    CHECK(p1[18] == 1 && p2[18] == 2 && p1[19] == 3, "chunk index/count fields");

    /* USB envelope */
    auto chunks = UsbChunks(p0, 8, 1);
    CHECK(chunks[3][0] == 0x10 && chunks[3][1] == 3 && chunks[3][2] == 8 && chunks[3][3] == 1,
          "USB envelope addresses channel+receiver");
    std::vector<uint8_t> rf_rt;
    for(auto& c : chunks) rf_rt.insert(rf_rt.end(), c.begin() + 4, c.end());
    CHECK(std::equal(rf_rt.begin(), rf_rt.end(), p0.begin()), "USB chunk round trip");

    /* deterministic effect ID: same content -> same id, never zero */
    RgbUpload same(data, 120, 1, t96);
    CHECK(up.EffectId() == same.EffectId(), "effect ID deterministic (FNV)");
    RgbTiming t95 = Timing96(); t95.interval_ticks = 95;
    RgbUpload diff(data, 120, 1, t95);
    CHECK(up.EffectId() != diff.EffectId(), "timing change changes effect ID");

    /* validation rejections */
    Mac zero_mac{};
    bool threw = false;
    try { up.Packet(0, zero_mac, MAC_MASTER); } catch(...) { threw = true; }
    CHECK(threw, "zero MAC rejected");
    threw = false;
    try { RgbUpload bad(data, 0, 1, t96); } catch(...) { threw = true; }
    CHECK(threw, "zero LED count rejected");
    threw = false;
    RfPacket not_rgb{}; not_rgb[0] = 0x12; not_rgb[1] = 0x14;
    try { UsbChunks(not_rgb, 8, 1); } catch(...) { threw = true; }
    CHECK(threw, "non-RGB packet rejected by lighting transport");

    /* clock keep-alive */
    auto clk = ClockChunks(MAC_MASTER, 8, 2026, 9, 14, 9, 0, 1, false);
    CHECK(clk[0][0] == 0x10 && clk[0][1] == 0 && clk[0][2] == 8 && clk[0][3] == 0xFF,
          "clock broadcast envelope");
    CHECK(clk[0][4] == RF_SELECT && clk[0][5] == RF_CLOCK_SYNC, "clock opcode");
    CHECK(memcmp(&clk[0][12], MAC_MASTER.data(), 6) == 0, "clock carries master MAC");
    /* rf[46..52] lands in chunk 0 payload at offset 46-4=42 */
    CHECK(clk[0][4 + 46] == 0x07 && clk[0][4 + 47] == 0xEA, "clock year BE16");
    CHECK(clk[0][4 + 50] == 9 && clk[0][4 + 52] == 1, "clock hour+second fields");
    auto clk0 = ClockChunks(MAC_MASTER, 8, 0,0,0,0,0,0, true);
    CHECK(clk0[0][4 + 14] == 0x14 && clk0[1][0] == 0x10, "initial clock padding");
}

/*-------------------------------------------------------------*\
| 2. Discovery parsing: complete/empty/truncated/malformed      |
\*-------------------------------------------------------------*/
static void TestDiscovery()
{
    auto resp = MakeDiscovery();
    auto list = ParseDiscoveryResponse(resp.data(), resp.size());
    CHECK(list.size() == 1, "one record parses");
    auto& d = list[0];
    CHECK(d.mac == MAC_TARGET && d.master == MAC_MASTER, "MACs decode");
    CHECK(d.channel == 8 && d.receiver == 1, "channel+receiver decode");
    CHECK(d.device_ticks == 0x1020, "device ticks decode");
    CHECK(d.fan_count == 3 && !d.right_attach, "plain fan count");
    CHECK(d.effect_id[0] == 0xAA && d.effect_id[3] == 0xDD, "effect ID decodes");
    CHECK(d.rpm[0] == 976, "RPM nibble decode");
    CHECK(d.fan_types[0] == 21, "fan type byte");
    CHECK(d.cmd_seq == 0x2A, "cmd_seq decode");
    CHECK(!d.mb_rgb_sync && !d.pwm_line, "status bits clear");

    /* right-attach encoding */
    auto resp2 = MakeDiscovery(); resp2[4 + 19] = 13;
    d = ParseDiscoveryResponse(resp2.data(), resp2.size())[0];
    CHECK(d.fan_count == 3 && d.right_attach, "fan_num>=10 -> right attach, 3 fans");

    /* master record skipped for devices but parses for masters */
    auto resp3 = MakeDiscovery(); resp3[4 + 18] = 0xFF;
    CHECK(ParseDiscoveryResponse(resp3.data(), resp3.size()).empty(),
          "master record excluded from device list");
    Mac mm; uint8_t mch;
    CHECK(ParseMasterRecord(resp3.data() + 4, RECORD_SIZE, mm, mch) && mm == MAC_TARGET,
          "master record parses separately");

    /* zero-MAC skip */
    auto resp4 = MakeDiscovery();
    memset(resp4.data() + 4, 0, 6);
    CHECK(ParseDiscoveryResponse(resp4.data(), resp4.size()).empty(),
          "zero-MAC record skipped");

    /* bad marker skip */
    auto resp5 = MakeDiscovery(); resp5[4 + 41] = 0;
    CHECK(ParseDiscoveryResponse(resp5.data(), resp5.size()).empty(),
          "bad record marker skipped");

    /* empty response = zero records, not an error */
    uint8_t empty[4] = { USB_CMD_SEND_RF, 0, 0, 0 };
    CHECK(ParseDiscoveryResponse(empty, 4).empty(), "empty response parses empty");

    /* truncated + wrong opcode rejected */
    threw:;
    bool threw = false;
    try { uint8_t t[4] = { 0x10, 1, 0, 0 }; ParseDiscoveryResponse(t, 4); }
    catch(...) { threw = true; }
    CHECK(threw, "truncated discovery rejected");
    threw = false;
    try { uint8_t t[4] = { 0x11, 0, 0, 0 }; ParseDiscoveryResponse(t, 4); }
    catch(...) { threw = true; }
    CHECK(threw, "wrong opcode rejected");
}

/*-------------------------------------------------------------*\
| 3. Codec: independent decode + optional vendor cross-check    |
\*-------------------------------------------------------------*/
static void TestCodec()
{
    /* round-trip three payload shapes through the tinyuz decoder */
    for(int shape = 0; shape < 3; shape++)
    {
        std::vector<uint8_t> rgb(360);
        for(size_t i = 0; i < rgb.size(); i++)
        {
            rgb[i] = shape == 0 ? 0x30
                   : shape == 1 ? (uint8_t)(i & 0xFF)
                                : (uint8_t)((i * 31 + 7) & 0xFF);
        }
        auto packed = LianLiWirelessCodec::Compress(rgb.data(), rgb.size());
        std::vector<uint8_t> out(360);
        CHECK(LianLiWirelessCodec::Decompress(packed.data(), packed.size(),
                                            out.data(), out.size()),
              "tinyuz decode succeeds");
        CHECK(out == rgb, "tinyuz decode matches original payload");
    }

    /* malformed input must fail, not crash */
    uint8_t garbage[16] = { 0xDE, 0xAD };
    uint8_t out[360];
    CHECK(!LianLiWirelessCodec::Decompress(garbage, sizeof(garbage), out, sizeof(out)),
          "malformed compressed input rejected");

#if defined(_WIN32) && defined(LLW_YUZ_CROSSCHECK)
    /*---------------------------------------------------------*\
    | Independent decode via the vendor yuz.dll when present —  |
    | validates wire compatibility, not just self-consistency.  |
    | Off by default: the dynamic-load pattern gets the test    |
    | binary flagged by Smart App Control. Build with           |
    | /DLLW_YUZ_CROSSCHECK to enable.                           |
    \*---------------------------------------------------------*/
    HMODULE yuz = LoadLibraryW(L"C:\\Program Files\\Lian-Li\\L-Connect 3\\yuz.dll");
    if(yuz)
    {
        using DecFn = void(__cdecl*)(const uint8_t*, int, int, uint8_t*);
        DecFn TuzDec = (DecFn)GetProcAddress(yuz, "TuzDec");
        if(TuzDec)
        {
            std::vector<uint8_t> rgb(360);
            for(size_t i = 0; i < rgb.size(); i++) rgb[i] = (uint8_t)(i % 7);
            auto packed = LianLiWirelessCodec::Compress(rgb.data(), rgb.size());
            std::vector<uint8_t> out(360);
            TuzDec(packed.data(), (int)packed.size(), 360, out.data());
            CHECK(out == rgb, "vendor yuz.dll decodes our tinyuz output");
        }
        FreeLibrary(yuz);
    }
#endif
}

/*-------------------------------------------------------------*\
| 4. Runtime: happy path — boot, search, upload, ack, hold      |
\*-------------------------------------------------------------*/
static void TestRuntimeHappyPath()
{
    FakeClock clk;
    FakeLink link;
    WirelessRuntime rt(link, clk);

    rt.SetTarget(MAC_TARGET, 3);
    auto up = std::make_shared<RgbUpload>(MakeUpload());
    rt.SetDesired(up);

    /* boot: reads master, starts keep-alive, searches */
    rt.Tick();
    CHECK(rt.State() == WirelessState::Searching, "boot -> searching");

    /* device reports its old effect until an upload lands, then acks */
    link.poll_fn = [&] {
        return MakeDiscovery(MAC_TARGET, MAC_MASTER,
                             link.rgb_sends > 0 ? up->EffectId().data() : nullptr);
    };
    RunFor(rt, clk, 2000);
    CHECK(rt.State() == WirelessState::Holding, "acked effect -> holding");
    CHECK(rt.Confirmed(), "confirmed state requires matching effect id");
    CHECK(rt.SendFailures() == 0, "no transport failures");
    CHECK(link.clock_sends > 0, "keep-alives sent");
    CHECK(link.rgb_sends > 0, "rgb upload chunks sent");
}

/*-------------------------------------------------------------*\
| 5. Temporary absence vs USB failure                           |
\*-------------------------------------------------------------*/
static void TestRuntimeAbsenceVsFailure()
{
    /* transient absence (< lost_ms) is not failure */
    {
        FakeClock clk; FakeLink link;
        WirelessRuntime rt(link, clk);
        rt.SetTarget(MAC_TARGET, 3);
        link.poll_fn = [] { return std::vector<uint8_t>{}; };   /* absent */
        RunFor(rt, clk, 5000);
        CHECK(rt.State() == WirelessState::Searching,
              "5s absence stays searching (not failed)");
        RunFor(rt, clk, 4000);
        CHECK(rt.State() == WirelessState::Lost, "sustained absence -> Lost");
    }

    /* USB-level failure is distinct and counted */
    {
        FakeClock clk; FakeLink link;
        WirelessRuntime rt(link, clk);
        rt.SetTarget(MAC_TARGET, 3);
        rt.Tick();                       /* boot */
        link.fail_polls = true;
        RunFor(rt, clk, 3000);
        CHECK(rt.State() == WirelessState::Failed,
              "transport failures reach Failed, not Lost");
    }
}

/*-------------------------------------------------------------*\
| 6. Foreign-master debounce                                    |
\*-------------------------------------------------------------*/
static void TestRuntimeForeignMaster()
{
    FakeClock clk; FakeLink link;
    WirelessRuntime rt(link, clk);
    rt.SetTarget(MAC_TARGET, 3);
    rt.Tick();
    link.poll_fn = [] { return MakeDiscovery(); };
    clk.Advance(400); rt.Tick();
    CHECK(rt.State() == WirelessState::Holding, "sighted -> holding (no desired)");

    /* 1-2 foreign sightings: transient flap, not a re-bind */
    link.poll_fn = [] { return MakeDiscovery(MAC_TARGET, MAC_FOREIGN); };
    clk.Advance(400); rt.Tick();
    clk.Advance(400); rt.Tick();
    CHECK(rt.State() == WirelessState::Holding, "2 foreign sightings debounced");

    /* 3rd consecutive -> ForeignMaster */
    clk.Advance(400); rt.Tick();
    CHECK(rt.State() == WirelessState::ForeignMaster,
          "sustained foreign master reported");
}

/*-------------------------------------------------------------*\
| 7. Retry exhaustion + cancellation                            |
\*-------------------------------------------------------------*/
static void TestRuntimeRetryAndCancel()
{
    /* upload never acked -> resend budget exhausts -> Failed */
    {
        FakeClock clk; FakeLink link;
        WirelessRuntime rt(link, clk);
        rt.SetTarget(MAC_TARGET, 3);
        auto up = std::make_shared<RgbUpload>(MakeUpload());
        rt.SetDesired(up);
        /* device always reports a different effect */
        link.poll_fn = [] { return MakeDiscovery(); };
        RunFor(rt, clk, 30000);
        CHECK(rt.State() == WirelessState::Failed,
              "unacknowledged upload exhausts retries");
        CHECK(rt.ResendCount() >= 8, "resend budget reached");
    }

    /* Cancel mid-upload stops everything */
    {
        FakeClock clk; FakeLink link;
        WirelessRuntime rt(link, clk);
        rt.SetTarget(MAC_TARGET, 3);
        rt.SetDesired(std::make_shared<RgbUpload>(MakeUpload()));
        link.poll_fn = [] { return MakeDiscovery(); };
        rt.Tick(); rt.Tick();
        int sends_before = link.send_calls;
        rt.Cancel();
        RunFor(rt, clk, 5000);
        CHECK(rt.State() == WirelessState::Cancelled, "cancel -> Cancelled");
        CHECK(link.send_calls == sends_before, "no sends after cancel");
    }
}

/*-------------------------------------------------------------*\
| 8. Ack followed by effect loss (drift resend)                 |
\*-------------------------------------------------------------*/
static void TestRuntimeDriftResend()
{
    FakeClock clk; FakeLink link;
    WirelessRuntime rt(link, clk);
    rt.SetTarget(MAC_TARGET, 3);
    auto up = std::make_shared<RgbUpload>(MakeUpload());
    rt.SetDesired(up);
    link.poll_fn = [&] { return MakeDiscovery(MAC_TARGET, MAC_MASTER, up->EffectId().data()); };
    RunFor(rt, clk, 2000);
    CHECK(rt.State() == WirelessState::Holding, "reached holding");

    /* firmware drifts to a different effect — 3 consecutive sightings */
    link.poll_fn = [] { return MakeDiscovery(); };   /* default effect */
    int sends_before = link.send_calls;
    RunFor(rt, clk, 5000);
    CHECK(link.send_calls > sends_before,
          "drift triggers resend after debounce");
}

/*-------------------------------------------------------------*\
| 9. Restoration failure remains a failure                      |
\*-------------------------------------------------------------*/
static void TestRuntimeRestoreFailure()
{
    FakeClock clk; FakeLink link;
    WirelessRuntime rt(link, clk);
    rt.SetTarget(MAC_TARGET, 3);
    auto up = std::make_shared<RgbUpload>(MakeUpload());
    rt.SetDesired(up);
    link.poll_fn = [] { return MakeDiscovery(); };   /* never acks */

    RunFor(rt, clk, 30000);
    CHECK(rt.State() == WirelessState::Failed, "failed restore is Failed");
    WirelessState s1 = rt.State();
    RunFor(rt, clk, 5000);
    CHECK(rt.State() == s1, "Failed state is terminal, not silently retried");
}

/*-------------------------------------------------------------*\
| 10. Newest-frame-wins queue                                   |
\*-------------------------------------------------------------*/
static void TestRuntimeNewestWins()
{
    FakeClock clk; FakeLink link;
    WirelessRuntime rt(link, clk);
    rt.SetTarget(MAC_TARGET, 3);
    rt.Tick();

    auto up1 = std::make_shared<RgbUpload>(MakeUpload(0x30));
    auto up2 = std::make_shared<RgbUpload>(MakeUpload(0x70));
    link.poll_fn = [] { return MakeDiscovery(); };
    rt.SetDesired(up1);
    rt.SetDesired(up2);            /* supersedes before first send */

    /* find the effect bytes actually transmitted (chunk 0, rf byte 14..17
       = usb chunk 0 offset 4+14=18..21) */
    RunFor(rt, clk, 1500);
    CHECK(!link.sent.empty(), "upload was sent");
    bool saw_up2 = false, saw_up1 = false;
    for(auto& c : link.sent)
    {
        if(c[0][3] == BROADCAST_RECEIVER) continue;
        std::array<uint8_t,4> id = { c[0][18], c[0][19], c[0][20], c[0][21] };
        if(id == up2->EffectId()) saw_up2 = true;
        if(id == up1->EffectId()) saw_up1 = true;
    }
    CHECK(saw_up2, "newest desired upload sent");
    CHECK(!saw_up1, "superseded upload never sent");
}

/* A live effect replaces the desired frame much faster than the RX
   discovery interval. Once a target has been validated, a new frame
   must reach TX without waiting for another discovery response. */
static void TestRuntimeStreamsBetweenPolls()
{
    FakeClock clk; FakeLink link;
    WirelessRuntime rt(link, clk);
    rt.SetTarget(MAC_TARGET, 3);
    link.poll_fn = [] { return MakeDiscovery(); };
    rt.Tick();                       /* validated sighting at t=0 */
    CHECK(rt.State() == WirelessState::Holding, "stream: target validated");
    const int polls = link.poll_calls;

    auto first = std::make_shared<RgbUpload>(MakeUpload(0x30));
    rt.SetDesired(first);
    clk.Advance(50); rt.Tick();      /* keep-alive, but no RX poll due */
    CHECK(link.poll_calls == polls, "stream: discovery cadence unchanged");
    CHECK(link.rgb_sends > 0, "stream: first frame sent between polls");

    auto second = std::make_shared<RgbUpload>(MakeUpload(0x70));
    rt.SetDesired(second);
    const int sends_before = link.rgb_sends;
    clk.Advance(24); rt.Tick();
    CHECK(link.rgb_sends == sends_before, "stream: RF upload budget respected");
    clk.Advance(1); rt.Tick();
    CHECK(link.poll_calls == polls, "stream: second frame needs no RX poll");
    CHECK(link.rgb_sends > sends_before, "stream: next frame sent between polls");

    bool sent_second = false;
    for(const auto& c : link.sent)
    {
        if(c[0][3] == BROADCAST_RECEIVER) continue;
        std::array<uint8_t, 4> id = { c[0][18], c[0][19], c[0][20], c[0][21] };
        if(id == second->EffectId()) sent_second = true;
    }
    CHECK(sent_second, "stream: transmitted latest frame, not only first frame");
}

/* A continuously changing Studio frame makes the last discovery result
   stale by definition. Replacing the pending frame must defer the blocking
   discovery read until the stream has gone quiet for one poll interval. */
static void TestRuntimeContinuousStreamDefersDiscovery()
{
    FakeClock clk; FakeLink link;
    WirelessRuntime rt(link, clk);
    rt.SetTarget(MAC_TARGET, 3);
    link.poll_fn = [] { return MakeDiscovery(); };
    rt.Tick();
    CHECK(rt.State() == WirelessState::Holding,
          "continuous stream: target validated");
    const int initial_polls = link.poll_calls;

    clk.Advance(400);                 /* normal holding poll is due */
    rt.SetDesired(std::make_shared<RgbUpload>(MakeUpload(0x30)));
    rt.Tick();
    CHECK(link.poll_calls == initial_polls,
          "continuous stream: fresh frame defers blocking discovery");
    CHECK(link.rgb_sends > 0,
          "continuous stream: fresh frame still reaches transmitter");

    clk.Advance(299);
    rt.Tick();
    CHECK(link.poll_calls == initial_polls,
          "continuous stream: discovery remains deferred during quiet window");
    clk.Advance(1);
    rt.Tick();
    CHECK(link.poll_calls == initial_polls + 1,
          "continuous stream: discovery resumes after stream goes quiet");
}

/*-------------------------------------------------------------*\
| 11. Channel/slot refresh from latest sighting                 |
\*-------------------------------------------------------------*/
static void TestRuntimeChannelRefresh()
{
    FakeClock clk; FakeLink link;
    WirelessRuntime rt(link, clk);
    rt.SetTarget(MAC_TARGET, 3);
    rt.Tick();

    /* sighting reports channel 8 / receiver 1, then moves to 9/2 */
    uint8_t ch = 8, rx = 1;
    link.poll_fn = [&] { return MakeDiscovery(MAC_TARGET, MAC_MASTER, nullptr, ch, rx); };
    clk.Advance(400); rt.Tick();
    CHECK(rt.State() == WirelessState::Holding, "sighted -> holding");

    auto up = std::make_shared<RgbUpload>(MakeUpload());
    rt.SetDesired(up);
    RunFor(rt, clk, 500);
    CHECK(link.rgb_sends > 0, "upload sent");
    /* find last rgb send (non-broadcast) */
    bool found_first = false;
    for(auto it = link.sent.rbegin(); it != link.sent.rend(); ++it)
    {
        if((*it)[0][3] != BROADCAST_RECEIVER)
        {
            CHECK((*it)[0][2] == 8 && (*it)[0][3] == 1,
                  "addressed to sighted channel+receiver");
            found_first = true;
            break;
        }
    }
    CHECK(found_first, "rgb packet found in send log");

    ch = 9; rx = 2;
    RunFor(rt, clk, 2000);
    /* find last rgb send (non-broadcast) */
    for(auto it = link.sent.rbegin(); it != link.sent.rend(); ++it)
    {
        if((*it)[0][3] != BROADCAST_RECEIVER)
        {
            CHECK((*it)[0][2] == 9 && (*it)[0][3] == 2,
                  "addressing refreshed to new channel+receiver");
            break;
        }
    }
}

/*-------------------------------------------------------------*\
|| 12. Lost -> re-sighting resumes pending upload                ||
\*-------------------------------------------------------------*/
static void TestRuntimeLostRecovery()
{
    FakeClock clk;
    FakeLink  link;
    WirelessRuntime rt(link, clk);
    rt.SetTarget(MAC_TARGET, 3);
    auto up = std::make_shared<RgbUpload>(MakeUpload());
    rt.SetDesired(up);
    link.poll_fn = [] { return MakeDiscovery(); };   /* wrong effect */
    RunFor(rt, clk, 3000);
    CHECK(rt.State() == WirelessState::Uploading, "uploading while sighted");

    /* device vanishes past the lost deadline */
    link.poll_fn = [] { return std::vector<uint8_t>{}; };
    RunFor(rt, clk, 10000);
    CHECK(rt.State() == WirelessState::Lost, "sustained absence -> Lost");

    /* returns still reporting the old effect -> resume uploading */
    int sends_before = link.send_calls;
    link.poll_fn = [] { return MakeDiscovery(); };
    RunFor(rt, clk, 2000);
    CHECK(rt.State() == WirelessState::Uploading,
          "re-sighting with stale effect resumes uploading");
    CHECK(link.send_calls > sends_before, "upload resent after Lost");
}

/*-------------------------------------------------------------*\
|| 13. Service: group routing, status, convergence               ||
\*-------------------------------------------------------------*/
static void TestService()
{
    FakeClock clk;
    FakeLink  link;
    LianLiWirelessService svc(link, clk);

    CHECK(svc.AddGroup(MAC_TARGET, 3), "service tracks group");
    CHECK(!svc.AddGroup(MAC_TARGET, 3), "duplicate group rejected");
    CHECK(svc.Groups().size() == 1, "one group listed");

    auto up = std::make_shared<RgbUpload>(MakeUpload());
    svc.SetDesired(MAC_TARGET, up);
    svc.SetDesired(MAC_FOREIGN, up);          /* unknown MAC ignored */

    link.poll_fn = [&] {
        return MakeDiscovery(MAC_TARGET, MAC_MASTER,
                             link.rgb_sends > 0 ? up->EffectId().data() : nullptr);
    };
    for(int i = 0; i < 30; i++) { clk.Advance(100); svc.StepAll(); }

    LianLiWirelessService::GroupStatus st;
    CHECK(svc.GetStatus(MAC_TARGET, st), "status available");
    CHECK(st.state == WirelessState::Holding && st.confirmed,
          "service converged to confirmed");
    CHECK(st.have_sighting && st.latest.mac == MAC_TARGET,
          "status carries latest sighting");
    CHECK(!svc.GetStatus(MAC_FOREIGN, st), "unknown group has no status");

    svc.RemoveGroup(MAC_TARGET);
    CHECK(!svc.GetStatus(MAC_TARGET, st), "removed group drops status");
    CHECK(svc.Groups().empty(), "group list empty after removal");
}

/* SetDesired is the service's producer/consumer boundary. A new frame must
   wake a sleeping worker instead of inheriting the periodic maintenance
   delay; otherwise every hardware frame gains up to one full service tick. */
static void TestServiceDesiredFrameWakesWorker()
{
    FakeClock clk;
    FakeLink  link;
    LianLiWirelessService svc(link, clk);
    link.poll_fn = [] { return MakeDiscovery(); };
    CHECK(svc.AddGroup(MAC_TARGET, 3), "wake: service tracks group");
    svc.Start(200);

    LianLiWirelessService::GroupStatus st;
    const auto ready_deadline = std::chrono::steady_clock::now()
                              + std::chrono::milliseconds(100);
    do
    {
        if(svc.GetStatus(MAC_TARGET, st)
           && st.state == WirelessState::Holding)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    while(std::chrono::steady_clock::now() < ready_deadline);
    CHECK(st.state == WirelessState::Holding, "wake: worker reached holding");

    svc.SetDesired(MAC_TARGET,
                   std::make_shared<RgbUpload>(MakeUpload(0x70)));
    bool sent = false;
    const auto send_deadline = std::chrono::steady_clock::now()
                             + std::chrono::milliseconds(75);
    do
    {
        if(svc.GetStatus(MAC_TARGET, st) && st.resends > 0)
        {
            sent = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    while(std::chrono::steady_clock::now() < send_deadline);

    CHECK(sent, "wake: desired frame bypasses maintenance sleep");
    svc.Stop();
}

/* Transport outages must recover through the same runtime used by OpenRGB. */
static void TestTransportReconnect()
{
    FakeClock clk; FakeLink link;
    WirelessRuntime rt(link, clk);
    rt.SetTarget(MAC_TARGET, 3);
    auto first = std::make_shared<RgbUpload>(MakeUpload(0x30));
    auto newest = std::make_shared<RgbUpload>(MakeUpload(0x70));
    rt.SetDesired(first);
    link.poll_fn = [&] { return MakeDiscovery(MAC_TARGET, MAC_MASTER,
        link.rgb_sends > 0 ? first->EffectId().data() : nullptr); };
    RunFor(rt, clk, 2000);
    CHECK(rt.Confirmed(), "reconnect fixture initially holds desired lighting");

    link.fail_polls = true;         /* receiver drops out during sleep */
    RunFor(rt, clk, 2000);
    CHECK(rt.State() == WirelessState::Failed, "transport outage is reported during backoff");
    CHECK(!rt.Confirmed() && rt.Latest() == nullptr, "outage invalidates cached device confirmation");
    int reads_before = link.master_reads;
    int polls_before = link.poll_calls;
    int sends_before = link.send_calls;
    int rgb_before = link.rgb_sends;
    size_t packets_before = link.sent.size();
    rt.SetDesired(newest);          /* profile changed while disconnected */
    link.fail_polls = false;
    link.poll_fn = [&] { return MakeDiscovery(MAC_TARGET, MAC_MASTER,
        link.rgb_sends > rgb_before ? newest->EffectId().data() : nullptr, 9, 2); };
    RunFor(rt, clk, 1000);
    CHECK(link.master_reads == reads_before && link.poll_calls == polls_before
          && link.send_calls == sends_before, "reconnect backoff performs no USB operations");
    RunFor(rt, clk, 6000);
    CHECK(link.master_reads > reads_before, "reconnect re-reads transmitter identity");
    CHECK(rt.Confirmed(), "same runtime restores newest desired effect after reconnect");
    CHECK(std::strlen(rt.LastError()) == 0, "confirmed recovery clears stale failure message");
    CHECK(rt.Latest() && rt.Latest()->channel == 9 && rt.Latest()->receiver == 2,
          "reconnect uses new discovery addressing");
    for(size_t i = packets_before; i < link.sent.size(); i++)
    {
        const auto& c = link.sent[i];
        if(c[0][3] == BROADCAST_RECEIVER || c[0][2] != 9) continue;
        std::array<uint8_t, 4> id = { c[0][18], c[0][19], c[0][20], c[0][21] };
        CHECK(id == newest->EffectId(), "no superseded profile replayed after reconnect");
    }
    link.fail_polls = true;
    RunFor(rt, clk, 2000);
    rt.Cancel();
    reads_before = link.master_reads; polls_before = link.poll_calls; sends_before = link.send_calls;
    RunFor(rt, clk, 20000);
    CHECK(rt.State() == WirelessState::Cancelled && link.master_reads == reads_before
          && link.poll_calls == polls_before && link.send_calls == sends_before,
          "cancel during backoff prevents later recovery attempts");
}

static void TestMasterReadRecovery()
{
    FakeClock clk; FakeLink link;
    WirelessRuntime rt(link, clk);
    rt.SetTarget(MAC_TARGET, 3);
    auto up = std::make_shared<RgbUpload>(MakeUpload());
    rt.SetDesired(up);
    link.fail_master = true;
    RunFor(rt, clk, 10000);
    CHECK(link.master_reads > 3 && link.master_reads <= 12,
          "long outage retries master reads with bounded attempts and delay");
    CHECK(link.rgb_sends == 0, "no RGB while transmitter identity is unavailable");
    link.fail_master = false;
    link.poll_fn = [&] { return MakeDiscovery(MAC_TARGET, MAC_MASTER,
        link.rgb_sends > 0 ? up->EffectId().data() : nullptr); };
    RunFor(rt, clk, 6000);
    CHECK(rt.Confirmed(), "runtime recovers when transmitter becomes readable");
}

static void TestTransmitRecovery()
{
    FakeClock clk; FakeLink link;
    WirelessRuntime rt(link, clk);
    rt.SetTarget(MAC_TARGET, 3);
    auto up = std::make_shared<RgbUpload>(MakeUpload());
    rt.SetDesired(up);
    link.poll_fn = [&] { return MakeDiscovery(MAC_TARGET, MAC_MASTER,
        link.rgb_sends > 0 ? up->EffectId().data() : nullptr); };
    RunFor(rt, clk, 2000);
    link.fail_sends = true;
    RunFor(rt, clk, 3000);
    CHECK(rt.State() == WirelessState::Failed && !rt.Confirmed(),
          "keep-alive transport failure invalidates confirmation");
    int rgb_before = link.rgb_sends;
    link.fail_sends = false;
    link.poll_fn = [] { return std::vector<uint8_t>{}; };
    RunFor(rt, clk, 4000);
    CHECK(link.rgb_sends == rgb_before, "reopened USB without fresh target cannot send RGB");
    link.poll_fn = [&] { return MakeDiscovery(MAC_TARGET, MAC_MASTER,
        link.rgb_sends > rgb_before ? up->EffectId().data() : nullptr); };
    size_t packets_before = link.sent.size();
    RunFor(rt, clk, 6000);
    CHECK(rt.Confirmed(), "keep-alive failure can recover and restore lighting");
    CHECK(link.sent.size() > packets_before && link.sent[packets_before][0][3] == BROADCAST_RECEIVER,
          "returning fan receives initial keep-alive before any RGB upload");
}

static void TestBriefOutageRevalidatesIdentity()
{
    for(bool tx_failure : { false, true })
    {
        FakeClock clk; FakeLink link;
        WirelessRuntime rt(link, clk);
        rt.SetTarget(MAC_TARGET, 3);
        rt.SetDesired(std::make_shared<RgbUpload>(MakeUpload()));
        link.poll_fn = [] { return MakeDiscovery(); };
        RunFor(rt, clk, 2000);
        link.fail_polls = !tx_failure;
        link.fail_sends = tx_failure;
        clk.Advance(1000); rt.Tick(); /* exactly one failed operation */
        int rgb_before = link.rgb_sends;
        int reads_before = link.master_reads;
        link.fail_polls = link.fail_sends = false;
        link.master_mac = MAC_FOREIGN;
        /* Cached RF table still reports the old binding after USB reopens. */
        RunFor(rt, clk, 5000);
        CHECK(link.master_reads > reads_before, "one transport failure forces identity revalidation");
        CHECK(rt.State() == WirelessState::ForeignMaster && link.rgb_sends == rgb_before,
              "brief outage cannot resume writes through a replacement transmitter");
    }
}

static void TestRgbFailureBudget()
{
    FakeClock clk; FakeLink link;
    WirelessRuntime rt(link, clk);
    rt.SetTarget(MAC_TARGET, 3);
    auto up = std::make_shared<RgbUpload>(MakeUpload());
    rt.SetDesired(up);
    link.poll_fn = [&] { return MakeDiscovery(MAC_TARGET, MAC_MASTER,
        link.rgb_sends > 0 ? up->EffectId().data() : nullptr); };
    link.fail_rgb = true;
    RunFor(rt, clk, 4000);
    CHECK(rt.State() == WirelessState::Failed,
          "successful keep-alives cannot erase RGB transport failure budget");
    link.fail_rgb = false;
    RunFor(rt, clk, 6000);
    CHECK(rt.Confirmed(), "RGB-only transport failures recover after backoff");
}

static void TestLayoutMismatchDebounce()
{
    FakeClock clk; FakeLink link;
    WirelessRuntime rt(link, clk);
    rt.SetTarget(MAC_TARGET, 3);
    auto up = std::make_shared<RgbUpload>(MakeUpload());
    rt.SetDesired(up);
    int bad_polls = 0;
    link.poll_fn = [&]
    {
        auto response = MakeDiscovery(MAC_TARGET, MAC_MASTER,
            link.rgb_sends > 0 ? up->EffectId().data() : nullptr);
        if(bad_polls++ == 0)
            response[4 + 19] = 2;
        return response;
    };
    RunFor(rt, clk, 300);
    CHECK(link.rgb_sends == 0 && rt.State() != WirelessState::Failed,
          "one inconsistent fan count stops writes without terminal failure");
    RunFor(rt, clk, 2000);
    CHECK(rt.Confirmed(), "valid layout after one mismatch restores playback");

    bad_polls = 0;
    for(int i = 0; i < 10 && bad_polls == 0; ++i)
    {
        clk.Advance(100);
        rt.Tick();
    }
    CHECK(bad_polls > 0 && rt.Latest() == nullptr
          && rt.State() != WirelessState::Failed,
          "one layout mismatch during active playback invalidates the route");
    RunFor(rt, clk, 2000);
    CHECK(rt.Confirmed(), "active playback resumes after valid layout returns");

    link.poll_fn = [&]
    {
        auto response = MakeDiscovery(MAC_TARGET, MAC_MASTER);
        response[4 + 28] |= 0x40;
        return response;
    };
    RunFor(rt, clk, 2000);
    CHECK(rt.State() == WirelessState::Failed && !rt.Confirmed(),
          "persistent motherboard RGB ownership mismatch remains terminal");
}

static void TestIncompleteDiscoveryRecord()
{
    /* Seen on hardware: the RX table briefly reports the target MAC
       with fan_count=0 at stream start — a half-written row, not a
       bound group with zero fans. That is an incomplete observation
       (a miss), not an incompatible layout: streaming keeps using the
       still-fresh sighting, sustained incomplete rows degrade to Lost
       instead of Failed, and valid rows recover on their own. */
    FakeClock clk; FakeLink link;
    WirelessRuntime rt(link, clk);
    rt.SetTarget(MAC_TARGET, 3);
    auto up = std::make_shared<RgbUpload>(MakeUpload());
    rt.SetDesired(up);
    bool incomplete = false;
    link.poll_fn = [&]
    {
        auto response = MakeDiscovery(MAC_TARGET, MAC_MASTER,
            link.rgb_sends > 0 ? up->EffectId().data() : nullptr);
        if(incomplete)
            response[4 + 19] = 0;
        return response;
    };
    RunFor(rt, clk, 2000);
    CHECK(rt.Confirmed(), "upload confirmed before incomplete rows");

    incomplete = true;
    rt.SetDesired(std::make_shared<RgbUpload>(MakeUpload()));
    int rgb_before = link.rgb_sends;
    RunFor(rt, clk, 3000);
    CHECK(link.rgb_sends > rgb_before,
          "incomplete rows do not interrupt streaming on a fresh sighting");
    CHECK(rt.State() != WirelessState::Failed,
          "incomplete rows never vote toward terminal failure");

    RunFor(rt, clk, 7000);
    CHECK(rt.State() == WirelessState::Lost && !rt.Confirmed(),
          "sustained incomplete rows degrade to Lost, not Failed");
    incomplete = false;
    RunFor(rt, clk, 4000);
    CHECK(rt.Confirmed(), "valid rows recover after an incomplete window");
}

static void TestChangedTransmitterOnReconnect()
{
    FakeClock clk; FakeLink link;
    WirelessRuntime rt(link, clk);
    rt.SetTarget(MAC_TARGET, 3);
    rt.SetDesired(std::make_shared<RgbUpload>(MakeUpload()));
    link.poll_fn = [] { return MakeDiscovery(); };
    RunFor(rt, clk, 1000);
    link.fail_polls = true;
    RunFor(rt, clk, 2000);
    int rgb_before = link.rgb_sends;
    link.fail_polls = false;
    link.master_mac = MAC_FOREIGN;
    link.poll_fn = [] { return MakeDiscovery(MAC_TARGET, MAC_FOREIGN); };
    RunFor(rt, clk, 10000);
    CHECK(rt.State() == WirelessState::ForeignMaster, "replacement transmitter requires explicit rediscovery");
    CHECK(link.rgb_sends == rgb_before, "recovery does not transfer ownership to another transmitter");
}

static void TestSharedLinkReconnect()
{
    FakeClock clk; FakeLink link;
    WirelessRuntime a(link, clk), b(link, clk);
    a.SetTarget(MAC_TARGET, 3);
    b.SetTarget(MAC_FOREIGN, 3);      /* second group on the same transmitter */
    auto up = std::make_shared<RgbUpload>(MakeUpload());
    a.SetDesired(up); b.SetDesired(up);
    link.poll_fn = [&] {
        auto response = MakeDiscovery(MAC_TARGET, MAC_MASTER, up->EffectId().data());
        auto second = MakeDiscovery(MAC_FOREIGN, MAC_MASTER, up->EffectId().data());
        response[1] = 2;
        response.insert(response.end(), second.begin() + 4, second.end());
        return response;
    };
    for(int i = 0; i < 20; i++) { clk.Advance(100); a.Tick(); b.Tick(); }
    CHECK(a.Confirmed() && b.Confirmed(), "both groups hold lighting on shared USB pair");
    link.fail_polls = true;
    clk.Advance(1000); a.Tick();
    ++link.generation;             /* real link closes both handles on hard failure */
    link.fail_polls = false;
    link.master_mac = MAC_FOREIGN;
    int sends_before = link.send_calls;
    b.Tick();
    CHECK(link.send_calls == sends_before && !b.Confirmed(),
          "shared-link invalidation stops peer keep-alive/RGB before any reopen");
    RunFor(b, clk, 2000);
    CHECK(b.State() == WirelessState::ForeignMaster && link.send_calls == sends_before,
          "peer rechecks identity instead of silently using reopened pair");
}

/*-------------------------------------------------------------*\
|| 14. Effect loops: render layout + multi-frame upload header   ||
\*-------------------------------------------------------------*/
static void TestEffectRendering()
{
    const size_t leds   = 120;
    const size_t frames = EFFECT_LOOP_FRAMES;

    /* spectrum: uniform color within each frame, hue turns full circle */
    auto spec = RenderSpectrumCycle(leds, frames);
    CHECK(spec.size() == leds * frames * 3, "spectrum payload covers every frame");
    CHECK(spec[0] == 255 && spec[1] == 0 && spec[2] == 0,
          "spectrum frame 0 is red");
    bool uniform = true;
    for(size_t i = 1; i < leds; i++)
    {
        if(memcmp(&spec[i * 3], &spec[0], 3) != 0) uniform = false;
    }
    CHECK(uniform, "spectrum frame is uniform across LEDs");
    const size_t half = frames / 2;
    CHECK(spec[half * leds * 3] == 0 && spec[half * leds * 3 + 1] == 255
          && spec[half * leds * 3 + 2] == 255,
          "spectrum mid-loop is cyan (hue 180)");

    /* rainbow wave: neighboring LEDs differ, phase moves, reverse mirrors */
    auto wave   = RenderRainbowWave(leds, frames, false);
    auto wave_r = RenderRainbowWave(leds, frames, true);
    CHECK(wave.size() == leds * frames * 3, "rainbow payload covers every frame");
    CHECK(memcmp(&wave[0], &wave[3], 3) != 0,
          "rainbow wave offsets adjacent LEDs");
    CHECK(memcmp(&wave[0], &wave[leds * 3], 3) != 0,
          "rainbow wave advances phase per frame");
    CHECK(wave != wave_r, "direction flag mirrors the wave");

    /* continuous wheel: one hue circle spans the whole group so
       the wave continues from each fan into the next — fan 1
       starts where fan 0's third of the wheel ends */
    auto wave_p = RenderRainbowWave(leds, frames, false, 40, nullptr);
    CHECK(wave_p[0] == 255 && wave_p[1] == 0 && wave_p[2] == 0,
          "chain slot 0 starts red");
    CHECK(wave_p[40 * 3] == 0 && wave_p[40 * 3 + 1] == 255
          && wave_p[40 * 3 + 2] == 0,
          "fan 1 slot 0 continues at a third of the wheel");
    CHECK(wave_p[80 * 3] == 0 && wave_p[80 * 3 + 1] == 0
          && wave_p[80 * 3 + 2] == 255,
          "fan 2 slot 0 continues at two thirds");
    /* the hue gradient is smooth across the fan boundary */
    CHECK(memcmp(&wave_p[39 * 3], &wave_p[40 * 3], 3) != 0,
          "hue keeps walking across the fan 0 -> fan 1 seam");

    /* perimeter order: virtual slot lands on physical LED order[slot]
       — e.g. slot 12 (start of the reversed inner-right strip)
       -> LED 31 */
    auto wave_o = RenderRainbowWave(leds, frames, false, 40, SLV3_LED_ORDER);
    CHECK(memcmp(&wave_o[31 * 3], &wave_p[12 * 3], 3) == 0,
          "rainbow order maps slot 12 to physical LED 31");
    const size_t f2 = frames / 3;
    /* fan 1 slot 19 -> physical LED 40 + order[19] = 64 */
    CHECK(memcmp(&wave_o[(f2 * leds + 64) * 3], &wave_p[(f2 * leds + 59) * 3], 3)
          == 0, "order remap holds on later frames and later fans");

    /* stack effects: the perimeter path walks up the left edge
       through fans 0..2 then down the right edge 2..0 — 24 slots
       per fan, 72 for the group */
    auto pwave = RenderPerimeterWave(leds, frames, false, 40,
                                     SLV3_STACK_POS);
    CHECK(pwave.size() == leds * frames * 3,
          "perimeter wave covers every frame");
    /* inner and outer strips share a position at the same height:
       fan 0 left-edge top — inner LED 11 and outer LED 19 both sit
       at stack slot 11, so they carry the same color */
    CHECK(memcmp(&pwave[11 * 3], &pwave[19 * 3], 3) == 0,
          "inner/outer strips share the perimeter position");
    /* the path is continuous across fans: fan 0 slot 11 (LED 11)
       neighbors fan 1 slot 12 (LED 51 = 40 + inner-left top) */
    CHECK(memcmp(&pwave[11 * 3], &pwave[51 * 3], 3) != 0,
          "perimeter wave walks across the fan 0 -> fan 1 seam");
    CHECK(memcmp(&pwave[0], &pwave[leds * 3], 3) != 0,
          "perimeter wave advances phase per frame");

    auto comet = RenderPerimeterComet(leds, frames, false, 40,
                                      SLV3_STACK_POS, 255, 255, 255);
    bool any_lit = false, any_dark = false;
    for(size_t b = 0; b < leds * 3; b++)
    {
        if(comet[b] > 200) any_lit = true;
        if(comet[b] == 0)  any_dark = true;
    }
    CHECK(any_lit && any_dark,
          "perimeter comet leaves a lit head and dark gap");

    auto pong = RenderPerimeterPingPong(leds, frames, 40,
                                        SLV3_STACK_POS, 0, 255, 0);
    /* the head travels: slot-0 LEDs are lit at frame 0 and dark at
       mid-frame when the head has reached the far end */
    const uint8_t* pong0 = &pong[0];
    const uint8_t* pong_mid = &pong[(frames / 2) * leds * 3];
    CHECK(pong0[1] > 200 && pong_mid[1] == 0,
          "ping pong head leaves the start position");
    /* ...and reaches the path end: LED 32 (fan 0 outer-right bottom)
       is the last slot on the loop */
    CHECK(pong_mid[32 * 3 + 1] > 200,
          "ping pong head reaches the far end mid-cycle");

    auto srain = RenderStackRainbow(leds, frames, false, 40,
                                    SLV3_LED_HEIGHT);
    /* same height on different fans matches: bottom of fan 0
       (LED 0) vs bottom of fan 1 (LED 40) differ — the stack
       coordinate lifts each fan by a third */
    CHECK(memcmp(&srain[0], &srain[40 * 3], 3) != 0,
          "stack rainbow separates fans by height");
    /* left/right strips at the same height share a color:
       inner-left 0 and inner-right 20 are both fan-bottom */
    CHECK(memcmp(&srain[0], &srain[20 * 3], 3) == 0,
          "same height lights both sides equally");

    /* breathing: dark at the loop seam, full brightness mid-cycle */
    auto br = RenderBreathing(leds, frames, 200, 40, 10);
    CHECK(br.size() == leds * frames * 3, "breathing payload covers every frame");
    CHECK(br[0] == 0 && br[1] == 0 && br[2] == 0, "breathing starts dark");
    CHECK(br[half * leds * 3] == 200 && br[half * leds * 3 + 1] == 40
          && br[half * leds * 3 + 2] == 10, "breathing peaks at mid-loop");
    const uint8_t last_r = br[(frames - 1) * leds * 3];
    CHECK(last_r > 0 && last_r < 64, "breathing returns to dark before seam");

    /* rendered loop feeds a multi-frame upload: header carries the
       frame count + interval the firmware plays autonomously */
    RgbTiming t;
    t.interval_ticks = 80;          /* 50 ms per frame */
    RgbUpload anim(LianLiWirelessCodec::Compress(spec.data(), spec.size()),
                   (uint8_t)leds, (uint16_t)frames, t);
    RfPacket hdr = anim.Packet(0, MAC_TARGET, MAC_MASTER);
    CHECK(hdr[25] == 0 && hdr[26] == (uint8_t)frames,
          "animation header carries frame count");
    CHECK(hdr[27] == (uint8_t)leds, "animation header carries LED count");
    CHECK(hdr[32] == 0 && hdr[33] == 80, "animation header carries interval");
    CHECK(anim.PacketCount() > 1, "animation upload spans payload chunks");
}

int main()
{
    TestProtocol();
    TestDiscovery();
    TestCodec();
    TestRuntimeHappyPath();
    TestRuntimeAbsenceVsFailure();
    TestRuntimeForeignMaster();
    TestRuntimeRetryAndCancel();
    TestRuntimeDriftResend();
    TestRuntimeRestoreFailure();
    TestRuntimeNewestWins();
    TestRuntimeStreamsBetweenPolls();
    TestRuntimeContinuousStreamDefersDiscovery();
    TestRuntimeChannelRefresh();
    TestRuntimeLostRecovery();
    TestService();
    TestServiceDesiredFrameWakesWorker();
    TestTransportReconnect();
    TestMasterReadRecovery();
    TestTransmitRecovery();
    TestChangedTransmitterOnReconnect();
    TestBriefOutageRevalidatesIdentity();
    TestRgbFailureBudget();
    TestLayoutMismatchDebounce();
    TestIncompleteDiscoveryRecord();
    TestSharedLinkReconnect();
    TestEffectRendering();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
