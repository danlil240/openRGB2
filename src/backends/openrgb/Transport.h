/*---------------------------------------------------------*\
||| Transport.h                                               |
|||                                                           |
|||   Byte-stream transport abstraction for the OpenRGB SDK  ||
|||   client. The production implementation is TcpSocket     ||
|||   (Winsock2, loopback TCP to 127.0.0.1:6744); tests      ||
|||   inject an in-process implementation so the offline     ||
|||   suite needs no sockets at all — fully deterministic.   ||
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace openrgb2
{
namespace sdk
{

enum class RecvResult
{
    Ok,        /* exactly len bytes arrived                */
    Closed,    /* peer performed an orderly close          */
    Timeout,   /* recv timed out before len bytes arrived  */
    Error,     /* transport error — err is set             */
};

class IStreamTransport
{
public:
    virtual ~IStreamTransport() = default;

    /* Connects to host:port with a bounded connect timeout and
       a bounded per-call I/O timeout applied to BOTH recv and
       send (a wedged peer must not hold a send forever — see
       the shutdown bound in DeliveryPipeline.h). Returns
       false + err on failure. */
    virtual bool       Connect(const std::string& host, uint16_t port,
                               uint32_t connect_timeout_ms,
                               uint32_t io_timeout_ms,
                               std::string& err) = 0;
    virtual void       Close() = 0;
    virtual bool       IsOpen() const = 0;

    /* Sends every byte (loops over partial sends). */
    virtual bool       SendAll(const void* data, size_t len,
                               std::string& err) = 0;

    /* Reads exactly len bytes. Timeout/Closed are clean
       outcomes, not errors. */
    virtual RecvResult RecvExact(void* buf, size_t len,
                                 std::string& err) = 0;

    /* readable = whether bytes (or a close) are pending
       within timeout_ms. */
    virtual bool       WaitReadable(uint32_t timeout_ms, bool& readable,
                                    std::string& err) = 0;

    /* Nonblocking liveness check: true when the peer closed.
       Pending unread data does NOT count as closed. */
    virtual bool       PeerClosed() = 0;
};

} /* namespace sdk */
} /* namespace openrgb2 */
