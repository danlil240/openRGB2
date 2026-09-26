/*---------------------------------------------------------*\
||| TcpSocket.h                                               |
|||                                                           |
|||   Blocking TCP IStreamTransport for the OpenRGB SDK      ||
|||   client. Loopback-only usage (127.0.0.1:6744 per plan   ||
|||   section 4); connect/recv carry bounded timeouts.       ||
|||   Qt-free, Windows (Winsock2). The SOCKET handle stays   ||
|||   inside the .cpp so winsock never leaks into headers    ||
|||   or dependents.                                         ||
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "Transport.h"

namespace openrgb2
{
namespace sdk
{

class TcpSocket final : public IStreamTransport
{
public:
    TcpSocket();
    ~TcpSocket() override;                   /* Close() */

    TcpSocket(const TcpSocket&)            = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    bool       Connect(const std::string& host, uint16_t port,
                       uint32_t connect_timeout_ms,
                       uint32_t recv_timeout_ms,
                       std::string& err) override;
    void       Close() override;
    bool       IsOpen() const override;
    bool       SendAll(const void* data, size_t len,
                       std::string& err) override;
    RecvResult RecvExact(void* buf, size_t len,
                         std::string& err) override;
    bool       WaitReadable(uint32_t timeout_ms, bool& readable,
                            std::string& err) override;
    bool       PeerClosed() override;

private:
    struct Impl;
    Impl* impl_;
};

} /* namespace sdk */
} /* namespace openrgb2 */
