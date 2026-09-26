/*---------------------------------------------------------*\
|| LianLiWirelessTransport.cpp                               ||
||                                                           ||
||   libusb-backed transport for the TX/RX dongle pair.      ||
||                                                           ||
||   This file is part of the OpenRGB project                ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "LianLiWirelessTransport.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>

using namespace LianLiWireless;
using namespace std::chrono_literals;

namespace
{
    constexpr uint8_t  EP_OUT               = 0x01;
    constexpr uint8_t  EP_IN                = 0x81;
    constexpr unsigned WRITE_TIMEOUT_MS     = 1000;
    constexpr unsigned QUERY_TIMEOUT_MS     = 1000;
    constexpr unsigned REPLY_FIRST_MS       = 100;
    constexpr unsigned REPLY_NEXT_MS        = 20;
    constexpr unsigned FLUSH_TIMEOUT_MS     = 10;
    constexpr int      FLUSH_ATTEMPTS       = 8;
    constexpr int      DISCOVERY_CAPACITY   = 512;
    constexpr uint64_t REOPEN_BACKOFF_MS    = 1000;

    uint64_t SteadyNowMs()
    {
        return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    /*-----------------------------------------------------------------*\
    | Errors that invalidate the USB handles: device gone or the pipe   |
    | broke. Timeouts and protocol-level problems do not reopen.        |
    \*-----------------------------------------------------------------*/
    bool NeedsReopen(int rc)
    {
        return rc == LIBUSB_ERROR_NO_DEVICE
            || rc == LIBUSB_ERROR_IO
            || rc == LIBUSB_ERROR_PIPE;
    }
}

uint64_t SystemWirelessClock::NowMs()
{
    return SteadyNowMs();
}

bool SystemWirelessClock::WallClock(uint16_t& year, uint8_t& month, uint8_t& day,
                                    uint8_t& hour, uint8_t& minute, uint8_t& second)
{
    std::time_t t = std::time(nullptr);
    std::tm     tm_buf;
    std::tm*    tm = nullptr;

#ifdef _WIN32
    if(localtime_s(&tm_buf, &t) == 0)
    {
        tm = &tm_buf;
    }
#else
    tm = localtime_r(&t, &tm_buf);
#endif
    if(tm == nullptr)
    {
        return false;
    }

    year   = (uint16_t)(tm->tm_year + 1900);
    month  = (uint8_t)(tm->tm_mon + 1);
    day    = (uint8_t)tm->tm_mday;
    hour   = (uint8_t)tm->tm_hour;
    minute = (uint8_t)tm->tm_min;
    second = (uint8_t)tm->tm_sec;
    return true;
}

bool LianLiWireless::CountWirelessDongles(unsigned& tx_count, unsigned& rx_count)
{
    tx_count = 0;
    rx_count = 0;

    if(libusb_init(nullptr) < 0)
    {
        return false;
    }

    libusb_device** list = nullptr;
    ssize_t         num  = libusb_get_device_list(nullptr, &list);
    if(num < 0)
    {
        return false;
    }

    for(ssize_t i = 0; i < num; i++)
    {
        libusb_device_descriptor desc;
        if(libusb_get_device_descriptor(list[i], &desc) < 0)
        {
            continue;
        }
        if(desc.idVendor != USB_VID)
        {
            continue;
        }
        if(desc.idProduct == TX_PID)
        {
            tx_count++;
        }
        else if(desc.idProduct == RX_PID)
        {
            rx_count++;
        }
    }

    libusb_free_device_list(list, 1);
    return true;
}

LianLiWirelessUsbLink::LianLiWirelessUsbLink()
{
    libusb_init(nullptr);
}

LianLiWirelessUsbLink::~LianLiWirelessUsbLink()
{
    Close();
}

void LianLiWirelessUsbLink::Close()
{
    if(tx != nullptr || rx != nullptr)
    {
        ++connection_generation;
    }
    if(rx != nullptr)
    {
        libusb_release_interface(rx, 0);
        libusb_close(rx);
        rx = nullptr;
    }
    if(tx != nullptr)
    {
        libusb_release_interface(tx, 0);
        libusb_close(tx);
        tx = nullptr;
    }
}

bool LianLiWirelessUsbLink::Fail(const char* what, int rc)
{
    char buf[160];
    snprintf(buf, sizeof(buf), "%s: libusb error %d", what, rc);
    last_error = buf;
    if(NeedsReopen(rc))
    {
        Close();
    }
    return false;
}

bool LianLiWirelessUsbLink::EnsureOpen()
{
    if(IsOpen())
    {
        return true;
    }

    uint64_t now = SteadyNowMs();
    if(now < reopen_not_before_ms)
    {
        last_error = "reconnect backoff";
        return false;
    }
    reopen_not_before_ms = now + REOPEN_BACKOFF_MS;

    /*---------------------------------------------------------*\
    | Conservative detection: exactly one TX and one RX. A      |
    | second pair would make cross-talk ambiguous.              |
    \*---------------------------------------------------------*/
    libusb_device** list = nullptr;
    ssize_t         num  = libusb_get_device_list(nullptr, &list);
    if(num < 0)
    {
        last_error = "USB enumeration failed";
        return false;
    }

    libusb_device* tx_dev = nullptr;
    libusb_device* rx_dev = nullptr;
    unsigned       tx_num = 0, rx_num = 0;

    for(ssize_t i = 0; i < num; i++)
    {
        libusb_device_descriptor desc;
        if(libusb_get_device_descriptor(list[i], &desc) < 0
        || desc.idVendor != USB_VID)
        {
            continue;
        }
        if(desc.idProduct == TX_PID)
        {
            tx_num++;
            tx_dev = list[i];
        }
        else if(desc.idProduct == RX_PID)
        {
            rx_num++;
            rx_dev = list[i];
        }
    }

    libusb_device_handle* new_tx = nullptr;
    libusb_device_handle* new_rx = nullptr;
    int                   rc     = 0;
    const char*           what   = nullptr;

    if(tx_num != 1 || rx_num != 1)
    {
        char buf[96];
        snprintf(buf, sizeof(buf), "expected one TX and one RX dongle, found %u/%u",
                 tx_num, rx_num);
        last_error = buf;
    }
    else if((rc = libusb_open(tx_dev, &new_tx)) < 0)
    {
        what = "open transmitter";
    }
    else
    {
        /* auto-detach is a no-op where unsupported (Windows/WinUSB) */
        libusb_set_auto_detach_kernel_driver(new_tx, 1);
        if((rc = libusb_claim_interface(new_tx, 0)) < 0)
        {
            what = "claim transmitter";
        }
        else if((rc = libusb_open(rx_dev, &new_rx)) < 0)
        {
            what = "open receiver";
        }
        else
        {
            libusb_set_auto_detach_kernel_driver(new_rx, 1);
            if((rc = libusb_claim_interface(new_rx, 0)) < 0)
            {
                what = "claim receiver";
            }
        }
    }

    libusb_free_device_list(list, 1);

    if(what != nullptr)
    {
        if(new_rx != nullptr)
        {
            libusb_close(new_rx);
        }
        if(new_tx != nullptr)
        {
            libusb_close(new_tx);
        }
        return Fail(what, rc);
    }
    if(new_tx == nullptr || new_rx == nullptr)
    {
        return false;   /* dongle-count message already recorded */
    }

    tx = new_tx;
    rx = new_rx;
    ++connection_generation;
    return true;
}

int LianLiWirelessUsbLink::ReadSome(libusb_device_handle* handle, uint8_t* buf,
                                    int capacity, unsigned timeout_ms)
{
    int transferred = 0;
    int rc = libusb_bulk_transfer(handle, EP_IN, buf, capacity,
                                  &transferred, timeout_ms);
    if(rc == LIBUSB_ERROR_TIMEOUT)
    {
        return transferred;     /* keep bytes that arrived before timeout */
    }
    if(rc < 0)
    {
        Fail("USB read", rc);
        return -1;
    }
    return transferred;
}

bool LianLiWirelessUsbLink::WriteAll(libusb_device_handle* handle,
                                     const uint8_t* data, int len,
                                     unsigned timeout_ms)
{
    int transferred = 0;
    int rc = libusb_bulk_transfer(handle, EP_OUT, const_cast<uint8_t*>(data),
                                  len, &transferred, timeout_ms);
    if(rc < 0)
    {
        return Fail("USB write", rc);
    }
    if(transferred != len)
    {
        last_error = "short USB write";
        return false;
    }
    return true;
}

bool LianLiWirelessUsbLink::FlushInput(libusb_device_handle* handle)
{
    uint8_t buf[DISCOVERY_CAPACITY];
    for(int i = 0; i < FLUSH_ATTEMPTS; i++)
    {
        int n = ReadSome(handle, buf, sizeof(buf), FLUSH_TIMEOUT_MS);
        if(n < 0)
        {
            return false;
        }
        if(n == 0)
        {
            return true;
        }
    }
    last_error = "USB input did not become idle";
    return false;
}

bool LianLiWirelessUsbLink::ReadMasterMac(Mac& out)
{
    if(!EnsureOpen() || !FlushInput(tx))
    {
        return false;
    }

    uint8_t query[USB_CHUNK_SIZE]{};
    query[0] = USB_CMD_GET_MAC;
    if(!WriteAll(tx, query, sizeof(query), WRITE_TIMEOUT_MS))
    {
        return false;
    }

    uint8_t reply[USB_CHUNK_SIZE];
    int n = ReadSome(tx, reply, sizeof(reply), QUERY_TIMEOUT_MS);
    if(n < 0)
    {
        return false;
    }
    if(n < 7 || reply[0] != USB_CMD_GET_MAC)
    {
        last_error = "invalid master query response";
        return false;
    }

    std::memcpy(out.data(), reply + 1, 6);
    return true;
}

bool LianLiWirelessUsbLink::PollDiscovery(std::vector<uint8_t>& out)
{
    out.clear();
    if(!EnsureOpen() || !FlushInput(rx))
    {
        return false;
    }

    uint8_t query[USB_CHUNK_SIZE]{};
    query[0] = USB_CMD_SEND_RF;
    query[1] = 1;
    if(!WriteAll(rx, query, sizeof(query), WRITE_TIMEOUT_MS))
    {
        return false;
    }

    /*---------------------------------------------------------*\
    | The reply is a burst of 64-byte chunks; silence ends it.  |
    | An empty burst is a valid "table empty" answer, not an    |
    | error.                                                    |
    \*---------------------------------------------------------*/
    uint8_t buf[DISCOVERY_CAPACITY];
    out.reserve(DISCOVERY_CAPACITY);
    unsigned timeout = REPLY_FIRST_MS;
    while(out.size() < DISCOVERY_CAPACITY)
    {
        int n = ReadSome(rx, buf, (int)sizeof(buf) - (int)out.size(), timeout);
        if(n < 0)
        {
            out.clear();
            return false;
        }
        if(n == 0)
        {
            break;
        }
        out.insert(out.end(), buf, buf + n);
        timeout = REPLY_NEXT_MS;
    }
    return true;
}

bool LianLiWirelessUsbLink::SendChunks(
    const std::array<UsbChunk, USB_CHUNKS_PER_PACKET>& chunks)
{
    if(!EnsureOpen())
    {
        return false;
    }
    for(const UsbChunk& chunk : chunks)
    {
        if(!WriteAll(tx, chunk.data(), (int)chunk.size(), WRITE_TIMEOUT_MS))
        {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    std::this_thread::sleep_for(2ms);
    return true;
}
