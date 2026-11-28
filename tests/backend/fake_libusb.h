// SPDX-License-Identifier: GPL-2.0-or-later
//
// fake_libusb -- a test-only, link-time stand-in for the handful of
// libusb-1.0 entry points LianLiWirelessTransport.cpp calls (audit
// P-13 step 3). A test target compiles the REAL transport against
// fake_libusb.cpp instead of linking libusb: no production change,
// no real USB, fully deterministic.
//
// The stub defines, with the exact prototypes of the vendored
// libusb 1.0.27 header (a drifted signature fails the compile):
//   libusb_init, libusb_get_device_list, libusb_free_device_list,
//   libusb_get_device_descriptor, libusb_open, libusb_close,
//   libusb_claim_interface, libusb_release_interface,
//   libusb_set_auto_detach_kernel_driver, libusb_bulk_transfer.
// Anything else the transport starts calling is a link error -- add
// it here on purpose.
//
// ------------------------------------------------------------------
// MODEL
// ------------------------------------------------------------------
// The "bus" is a list of fake devices (vid/pid), added with
// AddDevice(); the index it returns names the device in every other
// call. libusb_open() hands out a handle bound to that device; every
// bulk transfer on the handle is scripted and logged on the DEVICE.
//
// IN transfers (endpoint & 0x80) consume the device's read queue,
// one ReadChunk per libusb_bulk_transfer call:
//   * empty queue, or the front chunk is not available yet
//     -> LIBUSB_ERROR_TIMEOUT with 0 bytes ("silence");
//   * chunk.rc == LIBUSB_SUCCESS -> its bytes, rc 0;
//   * chunk.rc == LIBUSB_ERROR_TIMEOUT -> its bytes (possibly 0)
//     WITH the timeout rc: the "bytes that arrived before the
//     timeout" case libusb reports through `transferred`;
//   * any other rc -> that error, 0 bytes (e.g. NO_DEVICE, IO, PIPE);
//   * a chunk longer than the requested `length` -> the first
//     `length` bytes and LIBUSB_ERROR_OVERFLOW, like a real bulk IN
//     whose packet does not fit the buffer (the rest is dropped).
// A chunk is "available" once the device has seen at least
// `after_writes` OUT transfers (successful or not). QueueRead()
// makes a chunk available at once -- stale input that a flush must
// eat; QueueReply() makes it available only after the NEXT write --
// the answer to a query the transport is about to send, which the
// pre-query flush must NOT eat.
//
// OUT transfers (endpoint & 0x80 == 0) take the next WriteResult
// from the device's write queue; an empty queue means full-length
// success. WriteResult{rc, transferred}: transferred < 0 means
// "all of it" (a short write is transferred >= 0 with rc 0).
//
// Unplug(i) makes every later transfer on the device's handles fail
// with LIBUSB_ERROR_NO_DEVICE, and drops it from device lists (and
// libusb_open of a stale libusb_device* fails NO_DEVICE). Replug(i)
// reverses that; queued chunks are kept.
//
// ------------------------------------------------------------------
// OBSERVING
// ------------------------------------------------------------------
// Device::log records EVERY bulk transfer in call order (direction,
// endpoint, requested length, timeout, rc, bytes moved), so a test
// can assert read counts, per-read capacity (length-bounded reads)
// and the timeout budget (sum of timeout_ms over silent reads).
// Counters per device: opens, closes, claims, releases,
// auto_detach_calls, reads, writes, live_handles (opens - closes).
// Globals: init_calls, list_calls, lists_live (lists not yet
// freed -- a leak check), and on_transfer (optional callback run
// after each transfer is logged, e.g. to advance a fake clock).
//
// Failure injection beyond transfers: Globals::init_rc,
// Globals::list_rc (a negative value fails libusb_get_device_list),
// Device::descriptor_rc, Device::open_rc, Device::claim_rc.
//
// Reset() forgets every device and zeroes the globals; call it at
// the top of each test case. Handles still open from an earlier
// case are detached (their transfers fail NO_DEVICE) and leaked
// on purpose -- close links before Reset().
//
// Not thread-safe: the transport is driven from one thread here.
//
// Example (a discovery poll answering three 64-byte chunks):
//   fake_libusb::Reset();
//   const int tx = fake_libusb::AddDevice(0x0416, 0x8040);
//   const int rx = fake_libusb::AddDevice(0x0416, 0x8041);
//   for(int i = 0; i < 3; ++i)
//       fake_libusb::QueueReply(rx, std::vector<uint8_t>(64, (uint8_t)i));
//   LianLiWirelessUsbLink link;
//   std::vector<uint8_t> out;
//   link.PollDiscovery(out);               // out.size() == 192
//   fake_libusb::Dev(rx).reads;            // flush + 3 data + 1 silent
//   fake_libusb::Dev(rx).log[...].length;  // capacity per read

#pragma once

#include <libusb.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

namespace fake_libusb
{

struct ReadChunk
{
    std::vector<uint8_t> data;
    int                  rc           = LIBUSB_SUCCESS;
    int                  after_writes = 0;   /* see MODEL above */
};

struct WriteResult
{
    int rc          = LIBUSB_SUCCESS;
    int transferred = -1;                     /* < 0: full length */
};

struct Transfer
{
    bool                 in         = false;
    unsigned char        endpoint   = 0;
    int                  length     = 0;      /* requested bytes  */
    unsigned             timeout_ms = 0;
    int                  rc         = 0;
    std::vector<uint8_t> data;                /* bytes moved      */
};

struct Device
{
    uint16_t vid = 0;
    uint16_t pid = 0;
    bool     present = true;

    /* Failure injection (0 = success). */
    int descriptor_rc = 0;
    int open_rc       = 0;
    int claim_rc      = 0;

    std::deque<ReadChunk>   reads_queue;
    std::deque<WriteResult> writes_queue;
    std::vector<Transfer>   log;

    int opens             = 0;
    int closes            = 0;
    int claims            = 0;
    int releases          = 0;
    int auto_detach_calls = 0;
    int reads             = 0;
    int writes            = 0;
    int live_handles      = 0;

    /* Convenience views over `log`. */
    std::vector<Transfer> Reads() const;
    std::vector<Transfer> Writes() const;
};

struct Globals
{
    int init_rc    = LIBUSB_SUCCESS;
    int list_rc    = 0;         /* < 0: libusb_get_device_list fails */
    int init_calls = 0;
    int list_calls = 0;
    int lists_live = 0;
    std::function<void(int device, const Transfer&)> on_transfer;
};

void     Reset();
int      AddDevice(uint16_t vid, uint16_t pid);
Device&  Dev(int index);
Globals& G();

/* Available immediately (stale input a flush should consume). */
void QueueRead(int dev, std::vector<uint8_t> data,
               int rc = LIBUSB_SUCCESS);
/* Available after the device's NEXT OUT transfer. */
void QueueReply(int dev, std::vector<uint8_t> data,
                int rc = LIBUSB_SUCCESS);
/* An IN transfer that fails with rc and moves nothing. */
void QueueReadError(int dev, int rc, bool after_next_write = false);
/* The next OUT transfer's result. */
void QueueWrite(int dev, int rc, int transferred = -1);

void Unplug(int dev);
void Replug(int dev);

} /* namespace fake_libusb */
