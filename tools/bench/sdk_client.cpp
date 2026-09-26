/*---------------------------------------------------------*\
|| sdk_client.cpp                                            ||
||                                                           ||
||   Minimal synchronous OpenRGB SDK client. See           ||
||   sdk_client.h.                                          ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "sdk_client.h"

#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

namespace openrgb_bench
{
namespace
{
    constexpr size_t MAX_PACKET = 8 * 1024 * 1024;

    void Fail(const std::string& what)
    {
        throw std::runtime_error(what + " (WSA " +
                                 std::to_string(WSAGetLastError()) + ")");
    }

    bool RecvExact(SOCKET s, uint8_t* buf, size_t len, int timeout_ms)
    {
        size_t got = 0;
        while(got < len)
        {
            int n = recv(s, (char*)buf + got, (int)(len - got), 0);
            if(n == 0)
            {
                return false;               /* server closed */
            }
            if(n < 0)
            {
                int err = WSAGetLastError();
                if(err == WSAETIMEDOUT)
                {
                    return false;
                }
                throw std::runtime_error("recv failed (WSA " +
                                         std::to_string(err) + ")");
            }
            got += (size_t)n;
        }
        (void)timeout_ms;
        return true;
    }
}

SdkClient::SdkClient()
    : sock((uintptr_t)INVALID_SOCKET), protocol_version(0)
{
    WSADATA wsa;
    if(WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        throw std::runtime_error("WSAStartup failed");
    }
}

SdkClient::~SdkClient()
{
    Disconnect();
    WSACleanup();
}

void SdkClient::Connect(const std::string& host, uint16_t port,
                        const std::string& client_name, uint32_t timeout_ms)
{
    if(sock != INVALID_SOCKET)
    {
        throw std::runtime_error("client already connected");
    }

    ADDRINFOA hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    ADDRINFOA* result = nullptr;
    if(getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints,
                   &result) != 0 || result == nullptr)
    {
        throw std::runtime_error("cannot resolve " + host);
    }

    SOCKET s = INVALID_SOCKET;
    for(ADDRINFOA* ai = result; ai != nullptr; ai = ai->ai_next)
    {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if(s == INVALID_SOCKET)
        {
            continue;
        }
        if(connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0)
        {
            break;
        }
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(result);
    if(s == INVALID_SOCKET)
    {
        throw std::runtime_error("cannot connect to " + host + ":"
                                 + std::to_string(port));
    }

    DWORD timeout = timeout_ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    /* keep unsolicited async packets from filling the kernel buffer */
    int rcvbuf = 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvbuf, sizeof(rcvbuf));
    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));

    sock = (uintptr_t)s;

    /* version handshake: send our cap, take the server's minimum */
    std::vector<uint8_t> version_reply = Request(
        0, PKT_REQUEST_PROTOCOL_VERSION,
        BuildProtocolVersionRequest(SDK_PROTOCOL_CAP),
        PKT_REQUEST_PROTOCOL_VERSION);
    if(version_reply.size() != 4)
    {
        throw std::runtime_error("malformed protocol version reply");
    }
    uint32_t server_version = 0;
    memcpy(&server_version, version_reply.data(), 4);
    protocol_version = (server_version > SDK_PROTOCOL_CAP) ? SDK_PROTOCOL_CAP
                                                            : server_version;

    if(protocol_version == 0)
    {
        throw std::runtime_error("server does not support a protocol version");
    }

    /* the server sends its name right after the version reply */
    SendPacket(0, PKT_SET_CLIENT_NAME, BuildClientNameRequest(client_name));
    DrainPending();
}

void SdkClient::Disconnect()
{
    if(sock != INVALID_SOCKET)
    {
        closesocket((SOCKET)sock);
        sock = INVALID_SOCKET;
    }
    protocol_version = 0;
}

ClientStats SdkClient::Stats() const
{
    return stats;
}

void SdkClient::SendPacket(uint32_t dev_id, uint32_t pkt_id,
                           const std::vector<uint8_t>& body)
{
    if(sock == INVALID_SOCKET)
    {
        throw std::runtime_error("client not connected");
    }

    PacketHeader hdr{};
    memcpy(hdr.magic, "ORGB", 4);
    hdr.dev_id = dev_id;
    hdr.pkt_id = pkt_id;
    hdr.size   = (uint32_t)body.size();

    int n = send((SOCKET)sock, (const char*)&hdr, sizeof(hdr), 0);
    if(n != sizeof(hdr))
    {
        stats.send_errors++;
        Fail("packet header send failed");
    }
    if(!body.empty())
    {
        n = send((SOCKET)sock, (const char*)body.data(), (int)body.size(), 0);
        if(n != (int)body.size())
        {
            stats.send_errors++;
            Fail("packet body send failed");
        }
    }
}

bool SdkClient::RecvPacket(PacketHeader& header, std::vector<uint8_t>& body)
{
    if(!RecvExact((SOCKET)sock, (uint8_t*)&header, sizeof(header), 4000))
    {
        return false;
    }
    if(memcmp(header.magic, "ORGB", 4) != 0)
    {
        throw std::runtime_error("lost packet synchronization (bad magic)");
    }
    if(header.size > MAX_PACKET)
    {
        throw std::runtime_error("oversized packet from server");
    }
    body.resize(header.size);
    if(header.size > 0
       && !RecvExact((SOCKET)sock, body.data(), body.size(), 4000))
    {
        return false;
    }
    stats.packets_received++;
    stats.bytes_received += header.size;
    return true;
}

std::vector<uint8_t> SdkClient::Request(uint32_t dev_id, uint32_t pkt_id,
                                        const std::vector<uint8_t>& body,
                                        uint32_t expected_reply_id)
{
    SendPacket(dev_id, pkt_id, body);

    PacketHeader    header;
    std::vector<uint8_t> reply;
    for(;;)
    {
        if(!RecvPacket(header, reply))
        {
            throw std::runtime_error("timeout waiting for reply to packet "
                                     + std::to_string(pkt_id));
        }
        if(header.pkt_id == expected_reply_id)
        {
            return reply;
        }
        /* unexpected async packet: counted and dropped */
    }
}

void SdkClient::DrainPending()
{
    /* only meaningful between synchronous requests; at protocol < 6 the
       server sends nothing for UpdateLEDs, so this is a cheap no-op check */
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET((SOCKET)sock, &read_set);
    timeval zero{ 0, 0 };
    if(select(0, &read_set, nullptr, nullptr, &zero) <= 0)
    {
        return;
    }

    PacketHeader header;
    std::vector<uint8_t> body;
    while(select(0, &read_set, nullptr, nullptr, &zero) > 0)
    {
        if(!RecvPacket(header, body))
        {
            return;
        }
        FD_ZERO(&read_set);
        FD_SET((SOCKET)sock, &read_set);
    }
}

uint32_t SdkClient::ControllerCount()
{
    std::vector<uint8_t> reply = Request(0, PKT_REQUEST_CONTROLLER_COUNT, {},
                                         PKT_REQUEST_CONTROLLER_COUNT);
    if(reply.size() != 4)
    {
        throw std::runtime_error("malformed controller count reply");
    }
    uint32_t count = 0;
    memcpy(&count, reply.data(), 4);
    return count;
}

SdkController SdkClient::ControllerData(uint32_t dev_id)
{
    std::vector<uint8_t> reply = Request(
        dev_id, PKT_REQUEST_CONTROLLER_DATA,
        BuildControllerDataRequest(protocol_version),
        PKT_REQUEST_CONTROLLER_DATA);

    SdkController out;
    if(!ParseControllerData(reply.data(), reply.size(), protocol_version, out))
    {
        throw std::runtime_error("malformed controller data for device "
                                 + std::to_string(dev_id));
    }
    return out;
}

void SdkClient::SetCustomMode(uint32_t dev_id)
{
    SendPacket(dev_id, PKT_SETCUSTOMMODE, {});
    DrainPending();
}

void SdkClient::UpdateMode(uint32_t dev_id, int32_t mode_index,
                           const std::vector<uint8_t>& raw_mode)
{
    SendPacket(dev_id, PKT_UPDATEMODE, BuildUpdateModeBody(mode_index, raw_mode));
    DrainPending();
}

void SdkClient::UpdateLEDs(uint32_t dev_id, const std::vector<uint32_t>& colors)
{
    SendPacket(dev_id, PKT_UPDATELEDS, BuildUpdateLEDsBody(colors));
}
}
