// SPDX-License-Identifier: GPL-2.0-or-later
//
// Audit P-13 step 3: the REAL LianLiWirelessTransport.cpp (vendor
// OpenRGB fork, libusb TX/RX dongle link) compiled against the
// fake_libusb link seam (fake_libusb.h documents the stub). No
// production change, no real USB, no hardware.
//
// Covers: dongle counting; the open path (exactly one TX + one RX,
// claim, generation); ReadMasterMac / PollDiscovery / SendChunks
// happy paths with the wire bytes, endpoints, read capacities and
// timeouts they use; open failures that must not leak handles; and
// the NeedsReopen class (NO_DEVICE / IO / PIPE) closing the link and
// bumping ConnectionGeneration, while timeouts and short writes keep
// it open. P-10 (timeouts, close, reopen) and P-12 (chunked,
// length-bounded discovery reads) extend this suite.

#include "LianLiWirelessTransport.h"
#include "LianLiWirelessProtocol.h"
#include "fake_libusb.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace LianLiWireless;
namespace fu = fake_libusb;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

static constexpr unsigned char kEpOut = 0x01;
static constexpr unsigned char kEpIn  = 0x81;

static bool Contains(const char* s, const char* needle)
{
    return std::string(s).find(needle) != std::string::npos;
}

struct Bus
{
    int tx = -1;
    int rx = -1;
};

/* A fresh bus with one TX and one RX dongle plus an unrelated
   device (it must be ignored). */
static Bus FreshBus()
{
    fu::Reset();
    fu::AddDevice(0x046D, 0xC33C);               /* a keyboard */
    Bus b;
    b.tx = fu::AddDevice(USB_VID, TX_PID);
    b.rx = fu::AddDevice(USB_VID, RX_PID);
    return b;
}

static std::vector<uint8_t> MacReply(const Mac& mac)
{
    std::vector<uint8_t> r(USB_CHUNK_SIZE, 0);
    r[0] = USB_CMD_GET_MAC;
    std::memcpy(r.data() + 1, mac.data(), 6);
    return r;
}

static const Mac kMac = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60 };

/* Opens the link through ReadMasterMac (the first thing the
   controller does). */
static bool OpenViaMac(LianLiWirelessUsbLink& link, const Bus& b)
{
    fu::QueueReply(b.tx, MacReply(kMac));
    Mac got{};
    return link.ReadMasterMac(got) && got == kMac;
}

/*---------------------------------------------------------*\
||| CountWirelessDongles                                    ||
\*---------------------------------------------------------*/
static void TestCountDongles()
{
    FreshBus();
    fu::AddDevice(USB_VID, RX_PID);               /* a second RX */
    unsigned tx = 9, rx = 9;
    CHECK(CountWirelessDongles(tx, rx));
    CHECK(tx == 1 && rx == 2);
    CHECK(fu::G().lists_live == 0);               /* list freed */
    for(int i = 0; i < 4; ++i)
    {
        CHECK(fu::Dev(i).opens == 0);             /* counting never opens */
    }

    fu::G().list_rc = LIBUSB_ERROR_NO_MEM;
    CHECK(!CountWirelessDongles(tx, rx));
    CHECK(tx == 0 && rx == 0);

    fu::Reset();
    fu::G().init_rc = LIBUSB_ERROR_OTHER;
    CHECK(!CountWirelessDongles(tx, rx));
    CHECK(fu::G().list_calls == 0);
}

/*---------------------------------------------------------*\
||| Open + happy paths                                       ||
\*---------------------------------------------------------*/
static void TestReadMasterMacOpensAndQueries()
{
    const Bus b = FreshBus();
    LianLiWirelessUsbLink link;
    CHECK(fu::G().init_calls == 1);
    CHECK(!link.IsOpen());
    CHECK(link.ConnectionGeneration() == 0);

    /* Stale input sitting in the TX pipe: the pre-query flush
       must eat it, not mistake it for the reply. */
    fu::QueueRead(b.tx, std::vector<uint8_t>(USB_CHUNK_SIZE, 0xEE));
    fu::QueueReply(b.tx, MacReply(kMac));
    Mac got{};
    CHECK(link.ReadMasterMac(got));
    CHECK(got == kMac);
    CHECK(link.IsOpen());
    CHECK(link.ConnectionGeneration() == 1);
    CHECK(fu::G().lists_live == 0);

    const fu::Device& tx = fu::Dev(b.tx);
    const fu::Device& rx = fu::Dev(b.rx);
    CHECK(tx.opens == 1 && rx.opens == 1);
    CHECK(tx.claims == 1 && rx.claims == 1);
    CHECK(tx.live_handles == 1 && rx.live_handles == 1);
    CHECK(tx.auto_detach_calls == 1 && rx.auto_detach_calls == 1);

    /* Wire: flush reads (stale chunk, then silence), one 64-byte
       GET_MAC query, then the 1000 ms reply read. */
    const std::vector<fu::Transfer> w = tx.Writes();
    CHECK(w.size() == 1);
    if(w.size() == 1)
    {
        CHECK(w[0].endpoint == kEpOut);
        CHECK(w[0].length == (int)USB_CHUNK_SIZE);
        CHECK(w[0].timeout_ms == 1000);
        CHECK(!w[0].data.empty() && w[0].data[0] == USB_CMD_GET_MAC);
    }
    const std::vector<fu::Transfer> r = tx.Reads();
    CHECK(r.size() == 3);
    if(r.size() == 3)
    {
        CHECK(r[0].endpoint == kEpIn && r[0].timeout_ms == 10);
        CHECK(r[0].data.size() == USB_CHUNK_SIZE);   /* stale eaten */
        CHECK(r[1].rc == LIBUSB_ERROR_TIMEOUT && r[1].data.empty());
        CHECK(r[2].timeout_ms == 1000);
        CHECK(r[2].length == (int)USB_CHUNK_SIZE);
        CHECK(r[2].rc == LIBUSB_SUCCESS);
    }
    CHECK(rx.log.empty());                         /* RX untouched */

    /* Already open: a second query does not reopen. */
    fu::QueueReply(b.tx, MacReply(kMac));
    CHECK(link.ReadMasterMac(got));
    CHECK(fu::Dev(b.tx).opens == 1);
    CHECK(link.ConnectionGeneration() == 1);
}

static void TestPollDiscoveryReadsBurstUntilSilence()
{
    const Bus b = FreshBus();
    LianLiWirelessUsbLink link;
    for(uint8_t i = 0; i < 3; ++i)
    {
        fu::QueueReply(b.rx, std::vector<uint8_t>(USB_CHUNK_SIZE, (uint8_t)(i + 1)));
    }
    std::vector<uint8_t> out;
    CHECK(link.PollDiscovery(out));
    CHECK(out.size() == 3 * USB_CHUNK_SIZE);
    if(out.size() == 3 * USB_CHUNK_SIZE)
    {
        CHECK(out[0] == 1 && out[64] == 2 && out[191] == 3);
    }

    const fu::Device& rx = fu::Dev(b.rx);
    const std::vector<fu::Transfer> w = rx.Writes();
    CHECK(w.size() == 1);
    if(w.size() == 1)
    {
        CHECK(w[0].endpoint == kEpOut);
        CHECK(w[0].data.size() == USB_CHUNK_SIZE);
        CHECK(w[0].data[0] == USB_CMD_SEND_RF && w[0].data[1] == 1);
    }
    /* Reads today: one silent flush read, then three data reads
       and one terminating silent read. Capacity shrinks by what
       has arrived (512, 448, 384, 320); the first wait is 100 ms
       and every later one 20 ms. P-12 revisits this budget. */
    const std::vector<fu::Transfer> r = rx.Reads();
    CHECK(r.size() == 5);
    if(r.size() == 5)
    {
        CHECK(r[0].timeout_ms == 10 && r[0].length == 512);
        CHECK(r[1].timeout_ms == 100 && r[1].length == 512);
        CHECK(r[2].timeout_ms == 20  && r[2].length == 448);
        CHECK(r[3].timeout_ms == 20  && r[3].length == 384);
        CHECK(r[4].timeout_ms == 20  && r[4].length == 320);
        CHECK(r[4].rc == LIBUSB_ERROR_TIMEOUT);
    }
    CHECK(fu::Dev(b.tx).log.empty());              /* TX untouched */

    /* An empty burst is a valid "table empty" answer. */
    CHECK(link.PollDiscovery(out));
    CHECK(out.empty());
    CHECK(link.IsOpen());

    /* Bytes that arrive with a timeout are kept (partial chunk). */
    fu::QueueReply(b.rx, std::vector<uint8_t>(20, 0x5A), LIBUSB_ERROR_TIMEOUT);
    CHECK(link.PollDiscovery(out));
    CHECK(out.size() == 20);
}

static void TestPollDiscoveryStopsAtCapacity()
{
    /* 8 x 64 = 512 bytes fills the discovery buffer: no
       terminating silent read is issued. */
    const Bus b = FreshBus();
    LianLiWirelessUsbLink link;
    for(int i = 0; i < 9; ++i)
    {
        fu::QueueReply(b.rx, std::vector<uint8_t>(USB_CHUNK_SIZE, 0x11));
    }
    std::vector<uint8_t> out;
    CHECK(link.PollDiscovery(out));
    CHECK(out.size() == 512);
    const std::vector<fu::Transfer> r = fu::Dev(b.rx).Reads();
    CHECK(r.size() == 1 + 8);                       /* flush + 8 data */
    CHECK(!r.empty() && r.back().length == 64);
    CHECK(fu::Dev(b.rx).reads_queue.size() == 1);  /* 9th left over */
}

static void TestSendChunksWritesEveryChunk()
{
    const Bus b = FreshBus();
    LianLiWirelessUsbLink link;
    std::array<UsbChunk, USB_CHUNKS_PER_PACKET> chunks{};
    for(size_t i = 0; i < chunks.size(); ++i)
    {
        chunks[i].fill((uint8_t)(0xA0 + i));
    }
    CHECK(link.SendChunks(chunks));
    CHECK(link.IsOpen());
    const std::vector<fu::Transfer> w = fu::Dev(b.tx).Writes();
    CHECK(w.size() == USB_CHUNKS_PER_PACKET);
    for(size_t i = 0; i < w.size() && i < chunks.size(); ++i)
    {
        CHECK(w[i].endpoint == kEpOut);
        CHECK(w[i].data.size() == USB_CHUNK_SIZE);
        CHECK(std::memcmp(w[i].data.data(), chunks[i].data(),
                          USB_CHUNK_SIZE) == 0);
    }
    CHECK(fu::Dev(b.tx).Reads().empty());           /* no flush on send */
    CHECK(fu::Dev(b.rx).log.empty());
}

static void TestDestructorReleasesAndCloses()
{
    const Bus b = FreshBus();
    {
        LianLiWirelessUsbLink link;
        CHECK(OpenViaMac(link, b));
    }
    CHECK(fu::Dev(b.tx).releases == 1 && fu::Dev(b.rx).releases == 1);
    CHECK(fu::Dev(b.tx).closes == 1 && fu::Dev(b.rx).closes == 1);
    CHECK(fu::Dev(b.tx).live_handles == 0 && fu::Dev(b.rx).live_handles == 0);
}

/*---------------------------------------------------------*\
||| Open failures                                            ||
\*---------------------------------------------------------*/
static void TestOpenRequiresExactlyOnePair()
{
    fu::Reset();
    fu::AddDevice(USB_VID, TX_PID);
    LianLiWirelessUsbLink link;
    Mac got{};
    CHECK(!link.ReadMasterMac(got));
    CHECK(Contains(link.LastError(), "expected one TX and one RX dongle, found 1/0"));
    CHECK(!link.IsOpen());
    CHECK(fu::Dev(0).opens == 0);
    CHECK(fu::G().lists_live == 0);
    CHECK(link.ConnectionGeneration() == 0);

    fu::Reset();
    fu::G().list_rc = LIBUSB_ERROR_IO;
    LianLiWirelessUsbLink link2;
    CHECK(!link2.ReadMasterMac(got));
    CHECK(Contains(link2.LastError(), "USB enumeration failed"));
}

static void TestOpenFailureClosesPartialHandles()
{
    /* RX open fails after TX was opened and claimed: the TX
       handle must not leak. */
    {
        const Bus b = FreshBus();
        fu::Dev(b.rx).open_rc = LIBUSB_ERROR_ACCESS;
        LianLiWirelessUsbLink link;
        Mac got{};
        CHECK(!link.ReadMasterMac(got));
        CHECK(Contains(link.LastError(), "open receiver: libusb error -3"));
        CHECK(!link.IsOpen());
        CHECK(fu::Dev(b.tx).opens == 1 && fu::Dev(b.tx).live_handles == 0);
        CHECK(fu::Dev(b.rx).live_handles == 0);
        CHECK(fu::G().lists_live == 0);
        CHECK(link.ConnectionGeneration() == 0);
        CHECK(fu::Dev(b.tx).log.empty());          /* no I/O attempted */
    }
    /* Claim on TX fails (interface busy). */
    {
        const Bus b = FreshBus();
        fu::Dev(b.tx).claim_rc = LIBUSB_ERROR_BUSY;
        LianLiWirelessUsbLink link;
        std::vector<uint8_t> out;
        CHECK(!link.PollDiscovery(out));
        CHECK(Contains(link.LastError(), "claim transmitter: libusb error -6"));
        CHECK(fu::Dev(b.tx).live_handles == 0);
        CHECK(fu::Dev(b.rx).opens == 0);
    }
    /* A failed open starts the reopen backoff: the next call
       does not touch the bus. */
    {
        const Bus b = FreshBus();
        fu::Dev(b.rx).claim_rc = LIBUSB_ERROR_BUSY;
        LianLiWirelessUsbLink link;
        Mac got{};
        CHECK(!link.ReadMasterMac(got));
        const int lists = fu::G().list_calls;
        fu::Dev(b.rx).claim_rc = 0;
        CHECK(!link.ReadMasterMac(got));
        CHECK(Contains(link.LastError(), "reconnect backoff"));
        CHECK(fu::G().list_calls == lists);
    }
}

/*---------------------------------------------------------*\
||| NeedsReopen: NO_DEVICE / IO / PIPE close the link and   ||
||| bump the generation; other errors keep it open.         ||
\*---------------------------------------------------------*/
static void TestReopenClassErrorsClose()
{
    const int reopen_class[] = { LIBUSB_ERROR_NO_DEVICE,
                                 LIBUSB_ERROR_IO,
                                 LIBUSB_ERROR_PIPE };
    for(int rc : reopen_class)
    {
        /* ...on a write (SendChunks). */
        {
            const Bus b = FreshBus();
            LianLiWirelessUsbLink link;
            CHECK(OpenViaMac(link, b));
            CHECK(link.ConnectionGeneration() == 1);
            fu::QueueWrite(b.tx, rc);
            std::array<UsbChunk, USB_CHUNKS_PER_PACKET> chunks{};
            CHECK(!link.SendChunks(chunks));
            CHECK(!link.IsOpen());
            CHECK(link.ConnectionGeneration() == 2);
            char want[64];
            std::snprintf(want, sizeof(want), "USB write: libusb error %d", rc);
            CHECK(Contains(link.LastError(), want));
            CHECK(fu::Dev(b.tx).live_handles == 0 && fu::Dev(b.rx).live_handles == 0);
            CHECK(fu::Dev(b.tx).releases == 1 && fu::Dev(b.rx).releases == 1);
            /* Inside the backoff window nothing reopens. */
            CHECK(!link.SendChunks(chunks));
            CHECK(Contains(link.LastError(), "reconnect backoff"));
            CHECK(fu::Dev(b.tx).opens == 1);
            CHECK(link.ConnectionGeneration() == 2);
        }
        /* ...on a read (discovery reply). */
        {
            const Bus b = FreshBus();
            LianLiWirelessUsbLink link;
            CHECK(OpenViaMac(link, b));
            fu::QueueReadError(b.rx, rc, /*after_next_write=*/true);
            std::vector<uint8_t> out(5, 0);
            CHECK(!link.PollDiscovery(out));
            CHECK(out.empty());
            CHECK(!link.IsOpen());
            CHECK(link.ConnectionGeneration() == 2);
            char want[64];
            std::snprintf(want, sizeof(want), "USB read: libusb error %d", rc);
            CHECK(Contains(link.LastError(), want));
        }
    }
}

static void TestUnplugThenReopenAfterBackoff()
{
    const Bus b = FreshBus();
    LianLiWirelessUsbLink link;
    CHECK(OpenViaMac(link, b));

    /* The TX dongle is pulled: the flush read fails NO_DEVICE. */
    fu::Unplug(b.tx);
    Mac got{};
    CHECK(!link.ReadMasterMac(got));
    CHECK(!link.IsOpen());
    CHECK(link.ConnectionGeneration() == 2);

    /* Back in, and the 1 s reopen backoff has passed: the next
       operation reopens once, on a new generation. */
    fu::Replug(b.tx);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    CHECK(OpenViaMac(link, b));
    CHECK(link.IsOpen());
    CHECK(link.ConnectionGeneration() == 3);
    CHECK(fu::Dev(b.tx).opens == 2 && fu::Dev(b.rx).opens == 2);
    CHECK(fu::Dev(b.tx).live_handles == 1 && fu::Dev(b.rx).live_handles == 1);
}

static void TestNonReopenErrorsKeepLinkOpen()
{
    const Bus b = FreshBus();
    LianLiWirelessUsbLink link;
    CHECK(OpenViaMac(link, b));
    std::array<UsbChunk, USB_CHUNKS_PER_PACKET> chunks{};

    /* Write timeout: failure, but the handles stay. */
    fu::QueueWrite(b.tx, LIBUSB_ERROR_TIMEOUT, 0);
    CHECK(!link.SendChunks(chunks));
    CHECK(Contains(link.LastError(), "USB write: libusb error -7"));
    CHECK(link.IsOpen());
    CHECK(link.ConnectionGeneration() == 1);

    /* Short write. */
    fu::QueueWrite(b.tx, LIBUSB_SUCCESS, 10);
    CHECK(!link.SendChunks(chunks));
    CHECK(Contains(link.LastError(), "short USB write"));
    CHECK(link.IsOpen());

    /* An oversized IN packet (OVERFLOW) on a query reply. */
    std::vector<uint8_t> big = MacReply(kMac);
    big.resize(USB_CHUNK_SIZE + 8, 0);
    fu::QueueReply(b.tx, big);
    Mac got{};
    CHECK(!link.ReadMasterMac(got));
    CHECK(Contains(link.LastError(), "USB read: libusb error -8"));
    CHECK(link.IsOpen());

    /* A malformed reply is a protocol error, not a transport one. */
    std::vector<uint8_t> bad(USB_CHUNK_SIZE, 0);
    bad[0] = 0x42;
    fu::QueueReply(b.tx, bad);
    CHECK(!link.ReadMasterMac(got));
    CHECK(Contains(link.LastError(), "invalid master query response"));
    CHECK(link.IsOpen());

    /* No reply at all (silence) is the same. */
    CHECK(!link.ReadMasterMac(got));
    CHECK(Contains(link.LastError(), "invalid master query response"));
    CHECK(link.IsOpen());
    CHECK(link.ConnectionGeneration() == 1);
    CHECK(fu::Dev(b.tx).opens == 1);
}

static void TestFlushGivesUpOnBusyInput()
{
    /* Input that never goes quiet: 8 flush attempts, then a
       failure before the query is sent. */
    const Bus b = FreshBus();
    LianLiWirelessUsbLink link;
    for(int i = 0; i < 8; ++i)
    {
        fu::QueueRead(b.tx, std::vector<uint8_t>(USB_CHUNK_SIZE, 0x77));
    }
    Mac got{};
    CHECK(!link.ReadMasterMac(got));
    CHECK(Contains(link.LastError(), "USB input did not become idle"));
    CHECK(fu::Dev(b.tx).Writes().empty());
    CHECK(fu::Dev(b.tx).Reads().size() == 8);
    CHECK(link.IsOpen());
}

int main()
{
    TestCountDongles();
    TestReadMasterMacOpensAndQueries();
    TestPollDiscoveryReadsBurstUntilSilence();
    TestPollDiscoveryStopsAtCapacity();
    TestSendChunksWritesEveryChunk();
    TestDestructorReleasesAndCloses();
    TestOpenRequiresExactlyOnePair();
    TestOpenFailureClosesPartialHandles();
    TestReopenClassErrorsClose();
    TestUnplugThenReopenAfterBackoff();
    TestNonReopenErrorsKeepLinkOpen();
    TestFlushGivesUpOnBusyInput();

    std::printf("lianli_transport_test: %d checks, %d failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
