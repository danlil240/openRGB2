/*---------------------------------------------------------*\
||| TcpSocket.cpp                                             |
|||                                                           |
|||   Blocking TCP IStreamTransport. See TcpSocket.h.        ||
|||   Winsock is confined to this translation unit; code     ||
|||   that never references TcpSocket never takes a ws2_32   ||
|||   dependency.                                            ||
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "TcpSocket.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

namespace openrgb2
{
namespace sdk
{

struct TcpSocket::Impl
{
    SOCKET sock = INVALID_SOCKET;
};

namespace
{
    /* One Winsock init for the process; never torn down (the engine
       is a long-lived process and several clients may exist). */
    bool WsaReady()
    {
        static const bool ready = [] {
            WSADATA wsa;
            return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
        }();
        return ready;
    }

    std::string WsaError(const char* what)
    {
        return std::string(what) + " (WSA " +
               std::to_string(WSAGetLastError()) + ")";
    }
}

TcpSocket::TcpSocket()
    : impl_(new Impl)
{
}

TcpSocket::~TcpSocket()
{
    Close();
    delete impl_;
}

bool TcpSocket::Connect(const std::string& host, uint16_t port,
                        uint32_t connect_timeout_ms,
                        uint32_t io_timeout_ms, std::string& err)
{
    if(!WsaReady())
    {
        err = "WSAStartup failed";
        return false;
    }
    if(impl_->sock != INVALID_SOCKET)
    {
        err = "socket already connected";
        return false;
    }

    ADDRINFOA hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    ADDRINFOA* result = nullptr;
    if(getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints,
                   &result) != 0 || result == nullptr)
    {
        err = "cannot resolve " + host;
        return false;
    }

    SOCKET s        = INVALID_SOCKET;
    bool    timed   = false;
    for(ADDRINFOA* ai = result; ai != nullptr && !timed; ai = ai->ai_next)
    {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if(s == INVALID_SOCKET)
        {
            continue;
        }

        /* Non-blocking connect bounded by select(). */
        u_long nonblocking = 1;
        ioctlsocket(s, FIONBIO, &nonblocking);
        if(connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0)
        {
            break;                      /* connected immediately */
        }
        if(WSAGetLastError() != WSAEWOULDBLOCK)
        {
            closesocket(s);
            s = INVALID_SOCKET;
            continue;
        }

        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(s, &write_set);
        fd_set err_set;
        FD_ZERO(&err_set);
        FD_SET(s, &err_set);
        timeval tv;
        tv.tv_sec  = (long)(connect_timeout_ms / 1000);
        tv.tv_usec = (long)((connect_timeout_ms % 1000) * 1000);
        const int ready = select(0, nullptr, &write_set, &err_set, &tv);
        if(ready <= 0)
        {
            timed = (ready == 0);
            closesocket(s);
            s = INVALID_SOCKET;
            continue;
        }
        int       so_err = 0;
        int       optlen = sizeof(so_err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&so_err, &optlen);
        if(so_err != 0)
        {
            closesocket(s);
            s = INVALID_SOCKET;
            continue;
        }

        u_long blocking = 0;
        ioctlsocket(s, FIONBIO, &blocking);
        break;
    }
    freeaddrinfo(result);

    if(s == INVALID_SOCKET)
    {
        err = timed ? "connect timed out"
                    : "cannot connect to " + host + ":" +
                          std::to_string(port);
        return false;
    }

    /* One I/O timeout both ways. SO_SNDTIMEO bounds every send()
       call in SendAll: without it a wedged peer could hold a
       blocked send for the OS TCP retransmit timeout, stalling
       the serialized client send path AND pipeline lane joins
       (Stop/RemoveDevice bound = in-flight send + join). */
    DWORD io_timeout = io_timeout_ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               (const char*)&io_timeout, sizeof(io_timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
               (const char*)&io_timeout, sizeof(io_timeout));
    int rcvbuf = 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF,
               (const char*)&rcvbuf, sizeof(rcvbuf));
    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
               (const char*)&nodelay, sizeof(nodelay));

    impl_->sock = s;
    return true;
}

void TcpSocket::Close()
{
    if(impl_->sock != INVALID_SOCKET)
    {
        closesocket(impl_->sock);
        impl_->sock = INVALID_SOCKET;
    }
}

bool TcpSocket::IsOpen() const
{
    return impl_->sock != INVALID_SOCKET;
}

bool TcpSocket::SendAll(const void* data, size_t len, std::string& err)
{
    const char* p    = (const char*)data;
    size_t      left = len;
    while(left > 0)
    {
        const int n = send(impl_->sock, p, (int)left, 0);
        if(n <= 0)
        {
            err = WsaError("send failed");
            return false;
        }
        p    += n;
        left -= (size_t)n;
    }
    return true;
}

RecvResult TcpSocket::RecvExact(void* buf, size_t len, std::string& err)
{
    uint8_t* p    = (uint8_t*)buf;
    size_t   left = len;
    while(left > 0)
    {
        const int n = recv(impl_->sock, (char*)p, (int)left, 0);
        if(n == 0)
        {
            return RecvResult::Closed;
        }
        if(n < 0)
        {
            const int wsa = WSAGetLastError();
            if(wsa == WSAETIMEDOUT)
            {
                return RecvResult::Timeout;
            }
            err = WsaError("recv failed");
            return RecvResult::Error;
        }
        p    += n;
        left -= (size_t)n;
    }
    return RecvResult::Ok;
}

bool TcpSocket::WaitReadable(uint32_t timeout_ms, bool& readable,
                             std::string& err)
{
    readable = false;
    if(impl_->sock == INVALID_SOCKET)
    {
        return true;                       /* nothing to read on */
    }
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(impl_->sock, &read_set);
    timeval tv;
    tv.tv_sec  = (long)(timeout_ms / 1000);
    tv.tv_usec = (long)((timeout_ms % 1000) * 1000);
    const int n = select(0, &read_set, nullptr, nullptr, &tv);
    if(n < 0)
    {
        err = WsaError("select failed");
        return false;
    }
    readable = (n > 0);
    return true;
}

bool TcpSocket::PeerClosed()
{
    if(impl_->sock == INVALID_SOCKET)
    {
        return false;
    }
    bool readable = false;
    std::string err;
    if(!WaitReadable(0, readable, err) || !readable)
    {
        return false;
    }
    /* Readable can mean queued data or a close/reset; peek
       decides. n == 0 is a graceful close; an error (e.g.
       WSAECONNRESET after an abortive close) is equally a
       dead connection — only pending data is not. */
    char peek;
    const int n = recv(impl_->sock, &peek, 1, MSG_PEEK);
    if(n == 0)
    {
        return true;
    }
    if(n < 0 && WSAGetLastError() != WSAEWOULDBLOCK)
    {
        return true;
    }
    return false;
}

} /* namespace sdk */
} /* namespace openrgb2 */
