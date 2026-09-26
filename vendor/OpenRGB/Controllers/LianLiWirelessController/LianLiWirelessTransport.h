/*---------------------------------------------------------*\
|| LianLiWirelessTransport.h                                 ||
||                                                           ||
||   libusb-backed transport for the Lian Li wireless TX/RX  ||
||   dongle pair (0416:8040 TX, 0416:8041 RX) plus the       ||
||   system clock used by the runtime keep-alive.            ||
||                                                           ||
||   Transfer semantics are ported from the lian-li-smoke    ||
||   harness (LianLiSmoke.cs): bulk EP 0x01 out / 0x81 in,   ||
||   read-until-silence discovery replies, ~1 ms inter-chunk ||
||   pacing. Reconnect policy follows sgtaziz/lian-li-linux: ||
||   disconnect-class errors drop the handles and the next   ||
||   operation reopens once after a short backoff.           ||
||                                                           ||
||   This file is part of the OpenRGB project                ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "LianLiWirelessRuntime.h"
#include <libusb.h>
#include <string>

namespace LianLiWireless
{
    /*---------------------------------------------------------*\
    | Monotonic milliseconds + local wall clock for keep-alive  |
    \*---------------------------------------------------------*/
    class SystemWirelessClock : public IWirelessClock
    {
    public:
        uint64_t NowMs() override;
        bool WallClock(uint16_t& year, uint8_t& month, uint8_t& day,
                       uint8_t& hour, uint8_t& minute, uint8_t& second) override;
    };

    /*---------------------------------------------------------*\
    | Count attached TX (0x8040) and RX (0x8041) dongles        |
    | without opening them. Returns false on enumeration        |
    | failure.                                                  |
    \*---------------------------------------------------------*/
    bool CountWirelessDongles(unsigned& tx_count, unsigned& rx_count);

    /*---------------------------------------------------------*\
    | One TX/RX dongle pair. Conservative: requires exactly     |
    | one transmitter and one receiver on the bus — anything    |
    | else is reported rather than guessed at.                  |
    |                                                           |
    | All methods return false on transport failure; a missed   |
    | discovery response is success with an empty buffer.       |
    \*---------------------------------------------------------*/
    class LianLiWirelessUsbLink : public IWirelessLink
    {
    public:
        LianLiWirelessUsbLink();
        ~LianLiWirelessUsbLink() override;

        LianLiWirelessUsbLink(const LianLiWirelessUsbLink&) = delete;
        LianLiWirelessUsbLink& operator=(const LianLiWirelessUsbLink&) = delete;

        bool ReadMasterMac(Mac& out) override;
        bool PollDiscovery(std::vector<uint8_t>& out) override;
        bool SendChunks(const std::array<UsbChunk, USB_CHUNKS_PER_PACKET>& chunks) override;
        uint64_t ConnectionGeneration() const override { return connection_generation; }

        const char* LastError() const { return last_error.c_str(); }
        bool        IsOpen()    const { return tx != nullptr && rx != nullptr; }

    private:
        bool EnsureOpen();
        void Close();
        bool WriteAll(libusb_device_handle* handle, const uint8_t* data, int len,
                      unsigned timeout_ms);
        /* returns bytes read, 0 on timeout/silence, -1 on hard failure */
        int  ReadSome(libusb_device_handle* handle, uint8_t* buf, int capacity,
                      unsigned timeout_ms);
        bool FlushInput(libusb_device_handle* handle);
        bool Fail(const char* what, int rc);

        libusb_device_handle* tx = nullptr;
        libusb_device_handle* rx = nullptr;
        uint64_t              reopen_not_before_ms = 0;
        uint64_t              connection_generation = 0;
        std::string           last_error;
    };
}
