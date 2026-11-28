// SPDX-License-Identifier: GPL-2.0-or-later
//
// Audit P-13 step 1: the REAL TcpSocket (src/backends/openrgb/
// TcpSocket.cpp) against a loopback listener. Every other `sdk`
// suite injects FakeStream, so the IStreamTransport contract
// (Transport.h) -- above all RecvExact's "Timeout only when no
// byte was consumed; a stall after a partial read is an Error" --
// was never exercised on a real socket.
//
// 127.0.0.1 only, port 0 (the OS picks a free port): offline, no
// backend, no hardware. Mostly single-threaded: Connect() completes
// against the listen backlog, the test then accept()s the server
// side and drives it directly (send / graceful close / abortive
// close). Only the 1 MiB round trip uses a helper thread.
//
// If the host refuses the loopback bind/listen (local policy), the
// suite prints SKIP and exits 77 (CTest SKIP_RETURN_CODE), never a
// failure.

#include "TcpSocket.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace openrgb2::sdk;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

static constexpr int kSkip = 77;

using SteadyClock = std::chrono::steady_clock;

static long long MsSince(SteadyClock::time_point t0)
{
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        SteadyClock::now() - t0).count();
}

static bool Contains(const std::string& s, const char* needle)
{
    return s.find(needle) != std::string::npos;
}

/*---------------------------------------------------------*\
||| Loopback listener on 127.0.0.1:0.                      ||
\*---------------------------------------------------------*/
class Listener
{
public:
    ~Listener() { Close(); }

    /* false + why when the host refuses the bind/listen. */
    bool Open(std::string& why)
    {
        sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if(sock_ == INVALID_SOCKET)
        {
            why = "socket() failed, WSA " + std::to_string(WSAGetLastError());
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;
        if(bind(sock_, (sockaddr*)&addr, sizeof(addr)) != 0)
        {
            why = "bind(127.0.0.1:0) refused, WSA " +
                  std::to_string(WSAGetLastError());
            return false;
        }
        if(listen(sock_, 4) != 0)
        {
            why = "listen() refused, WSA " + std::to_string(WSAGetLastError());
            return false;
        }
        int len = sizeof(addr);
        getsockname(sock_, (sockaddr*)&addr, &len);
        port_ = ntohs(addr.sin_port);
        return true;
    }

    /* Accepts the connection the client just completed. */
    SOCKET Accept()
    {
        return accept(sock_, nullptr, nullptr);
    }

    void Close()
    {
        if(sock_ != INVALID_SOCKET)
        {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
    }

    uint16_t Port() const { return port_; }

private:
    SOCKET   sock_ = INVALID_SOCKET;
    uint16_t port_ = 0;
};

static bool SendRaw(SOCKET s, const void* data, int len)
{
    return send(s, (const char*)data, len, 0) == len;
}

static void AbortiveClose(SOCKET s)
{
    linger lg;
    lg.l_onoff  = 1;
    lg.l_linger = 0;        /* RST instead of FIN */
    setsockopt(s, SOL_SOCKET, SO_LINGER, (const char*)&lg, sizeof(lg));
    closesocket(s);
}

/* Bounded wait until the client side sees bytes or a close. */
static bool WaitUntilReadable(TcpSocket& c, uint32_t total_ms)
{
    bool        readable = false;
    std::string err;
    return c.WaitReadable(total_ms, readable, err) && readable;
}

static constexpr uint32_t kConnectMs = 2000;
static constexpr uint32_t kIoMs      = 250;

/* Connects a fresh client to the listener and accepts the server
   side. Returns INVALID_SOCKET (and fails a check) on error. */
static SOCKET ConnectPair(Listener& l, TcpSocket& c,
                          uint32_t io_ms = kIoMs)
{
    std::string err;
    const bool ok = c.Connect("127.0.0.1", l.Port(), kConnectMs, io_ms, err);
    CHECK(ok);
    CHECK(err.empty());
    if(!ok)
    {
        std::printf("  connect error: %s\n", err.c_str());
        return INVALID_SOCKET;
    }
    const SOCKET s = l.Accept();
    CHECK(s != INVALID_SOCKET);
    return s;
}

/*---------------------------------------------------------*\
||| Connect to a closed port fails within the bound.        ||
\*---------------------------------------------------------*/
static void TestConnectToClosedPortIsBounded()
{
    /* A port that was just free: bind, read it, close. */
    uint16_t closed_port = 0;
    {
        Listener tmp;
        std::string why;
        CHECK(tmp.Open(why));
        closed_port = tmp.Port();
    }

    TcpSocket c;
    std::string err;
    const auto t0 = SteadyClock::now();
    const bool ok = c.Connect("127.0.0.1", closed_port, 300, kIoMs, err);
    const long long ms = MsSince(t0);
    CHECK(!ok);
    CHECK(!c.IsOpen());
    /* Windows retries a refused loopback SYN for ~2 s, so the
       300 ms bound usually ends it as a timeout; a fast refusal
       is equally a clean failure. Either way: bounded. */
    CHECK(Contains(err, "connect timed out") || Contains(err, "cannot connect"));
    CHECK(ms < 1500);
    std::printf("  closed port: \"%s\" after %lld ms\n", err.c_str(), ms);
}

static void TestDoubleConnectAndClose(Listener& l)
{
    /* (No unresolvable-host case: resolving one could leave the
       machine as a DNS query, and this suite stays offline.) */
    TcpSocket c;
    const SOCKET s = ConnectPair(l, c);
    std::string err;
    CHECK(!c.Connect("127.0.0.1", l.Port(), kConnectMs, kIoMs, err));
    CHECK(err == "socket already connected");
    CHECK(c.IsOpen());
    c.Close();
    CHECK(!c.IsOpen());
    c.Close();                               /* idempotent */
    if(s != INVALID_SOCKET) closesocket(s);
}

/*---------------------------------------------------------*\
||| SendAll / RecvExact round trips.                         ||
\*---------------------------------------------------------*/
static void TestRoundTrip(Listener& l)
{
    TcpSocket c;
    const SOCKET s = ConnectPair(l, c);
    if(s == INVALID_SOCKET) return;

    /* Small: client -> server -> client. */
    const char msg[16] = "0123456789abcde";
    std::string err;
    CHECK(c.SendAll(msg, sizeof(msg), err));
    char got[16] = {};
    CHECK(recv(s, got, sizeof(got), MSG_WAITALL) == (int)sizeof(got));
    CHECK(std::memcmp(got, msg, sizeof(msg)) == 0);
    CHECK(SendRaw(s, msg, sizeof(msg)));
    char back[16] = {};
    CHECK(c.RecvExact(back, sizeof(back), err) == RecvResult::Ok);
    CHECK(std::memcmp(back, msg, sizeof(msg)) == 0);

    /* Byte-dribbled reply: RecvExact loops over short recv()s. */
    for(int i = 0; i < 16; ++i)
    {
        CHECK(SendRaw(s, msg + i, 1));
    }
    std::memset(back, 0, sizeof(back));
    CHECK(c.RecvExact(back, sizeof(back), err) == RecvResult::Ok);
    CHECK(std::memcmp(back, msg, sizeof(msg)) == 0);

    closesocket(s);
}

static void TestLargeRoundTrip(Listener& l)
{
    /* 1 MiB each way: larger than the socket buffers, so SendAll
       and RecvExact both loop over partial transfers. The echo
       runs in a helper thread (the server must read while the
       client is still sending). */
    TcpSocket c;
    const SOCKET s = ConnectPair(l, c, 5000);
    if(s == INVALID_SOCKET) return;

    const size_t kLen = 1024 * 1024;
    std::vector<uint8_t> out(kLen);
    for(size_t i = 0; i < kLen; ++i)
    {
        out[i] = (uint8_t)(i * 31u + 7u);
    }
    std::thread echo([s, kLen] {
        std::vector<char> buf(kLen);
        size_t have = 0;
        while(have < kLen)
        {
            const int n = recv(s, buf.data() + have, (int)(kLen - have), 0);
            if(n <= 0) return;
            have += (size_t)n;
        }
        size_t sent = 0;
        while(sent < kLen)
        {
            const int n = send(s, buf.data() + sent, (int)(kLen - sent), 0);
            if(n <= 0) return;
            sent += (size_t)n;
        }
    });
    std::string err;
    CHECK(c.SendAll(out.data(), out.size(), err));
    std::vector<uint8_t> in(kLen, 0);
    CHECK(c.RecvExact(in.data(), in.size(), err) == RecvResult::Ok);
    CHECK(in == out);
    echo.join();
    closesocket(s);
}

/*---------------------------------------------------------*\
||| The Timeout-versus-Error contract (Transport.h).         ||
\*---------------------------------------------------------*/
static void TestTimeoutWithZeroBytes(Listener& l)
{
    TcpSocket c;
    const SOCKET s = ConnectPair(l, c);
    if(s == INVALID_SOCKET) return;

    char buf[16];
    std::string err;
    const auto t0 = SteadyClock::now();
    CHECK(c.RecvExact(buf, sizeof(buf), err) == RecvResult::Timeout);
    const long long ms = MsSince(t0);
    CHECK(err.empty());                       /* a clean outcome */
    CHECK(ms >= (long long)kIoMs - 50);       /* SO_RCVTIMEO applied */
    CHECK(ms < (long long)kIoMs + 1500);      /* ...and bounded   */
    CHECK(c.IsOpen());

    /* The client keeps using the socket after a clean Timeout (an
       unsupported wireless-status probe, OpenRgbClient.cpp): the
       next whole record must still arrive intact. */
    const char rec[16] = "after-timeout..";
    CHECK(SendRaw(s, rec, sizeof(rec)));
    CHECK(c.RecvExact(buf, sizeof(buf), err) == RecvResult::Ok);
    CHECK(std::memcmp(buf, rec, sizeof(rec)) == 0);
    closesocket(s);
}

static void TestPartialReadThenStallIsError(Listener& l)
{
    TcpSocket c;
    const SOCKET s = ConnectPair(l, c);
    if(s == INVALID_SOCKET) return;

    /* 3 of 16 bytes, then silence: the stream is desynchronized. */
    CHECK(SendRaw(s, "ORG", 3));
    char buf[16];
    std::string err;
    const auto t0 = SteadyClock::now();
    CHECK(c.RecvExact(buf, sizeof(buf), err) == RecvResult::Error);
    CHECK(Contains(err, "partial read"));
    CHECK(MsSince(t0) < (long long)kIoMs + 1500);
    closesocket(s);
}

/*---------------------------------------------------------*\
||| PeerClosed: graceful FIN, abortive RST, pending data.   ||
\*---------------------------------------------------------*/
static void TestGracefulClose(Listener& l)
{
    TcpSocket c;
    const SOCKET s = ConnectPair(l, c);
    if(s == INVALID_SOCKET) return;

    CHECK(!c.PeerClosed());                   /* live and quiet */
    shutdown(s, SD_SEND);
    closesocket(s);
    CHECK(WaitUntilReadable(c, 2000));
    CHECK(c.PeerClosed());
    CHECK(c.PeerClosed());                    /* MSG_PEEK: sticky */
    char buf[4];
    std::string err;
    CHECK(c.RecvExact(buf, sizeof(buf), err) == RecvResult::Closed);
    CHECK(err.empty());
}

static void TestPendingDataIsNotClosed(Listener& l)
{
    TcpSocket c;
    const SOCKET s = ConnectPair(l, c);
    if(s == INVALID_SOCKET) return;

    /* Data then FIN: unread data must not count as closed. */
    CHECK(SendRaw(s, "abcd", 4));
    shutdown(s, SD_SEND);
    closesocket(s);
    CHECK(WaitUntilReadable(c, 2000));
    CHECK(!c.PeerClosed());
    char buf[4];
    std::string err;
    CHECK(c.RecvExact(buf, sizeof(buf), err) == RecvResult::Ok);
    CHECK(std::memcmp(buf, "abcd", 4) == 0);
    /* Drained: now the close is visible. */
    CHECK(WaitUntilReadable(c, 2000));
    CHECK(c.PeerClosed());
}

static void TestCloseMidRecordIsClosed(Listener& l)
{
    /* Part of a record, then FIN: RecvExact reports Closed (the
       caller tears the wire down), not Ok and not a hang. */
    TcpSocket c;
    const SOCKET s = ConnectPair(l, c);
    if(s == INVALID_SOCKET) return;
    CHECK(SendRaw(s, "ab", 2));
    shutdown(s, SD_SEND);
    closesocket(s);
    char buf[8];
    std::string err;
    CHECK(c.RecvExact(buf, sizeof(buf), err) == RecvResult::Closed);
}

static void TestAbortiveClose(Listener& l)
{
    TcpSocket c;
    const SOCKET s = ConnectPair(l, c);
    if(s == INVALID_SOCKET) return;

    AbortiveClose(s);                         /* SO_LINGER 0 -> RST */
    CHECK(WaitUntilReadable(c, 2000));
    CHECK(c.PeerClosed());                    /* WSAECONNRESET = dead */
    char buf[4];
    std::string err;
    CHECK(c.RecvExact(buf, sizeof(buf), err) == RecvResult::Error);
    CHECK(Contains(err, "recv failed"));
    std::printf("  abortive close recv: \"%s\"\n", err.c_str());
}

/*---------------------------------------------------------*\
||| WaitReadable                                             ||
\*---------------------------------------------------------*/
static void TestWaitReadable(Listener& l)
{
    /* Not connected: success, nothing readable, no wait. */
    {
        TcpSocket idle;
        bool readable = true;
        std::string err;
        CHECK(idle.WaitReadable(1000, readable, err));
        CHECK(!readable);
        CHECK(!idle.PeerClosed());
    }

    TcpSocket c;
    const SOCKET s = ConnectPair(l, c);
    if(s == INVALID_SOCKET) return;

    bool readable = true;
    std::string err;
    const auto t0 = SteadyClock::now();
    CHECK(c.WaitReadable(150, readable, err));
    const long long ms = MsSince(t0);
    CHECK(!readable);
    CHECK(ms >= 100);
    CHECK(ms < 1500);

    /* Zero timeout is a pure poll. */
    CHECK(c.WaitReadable(0, readable, err));
    CHECK(!readable);

    CHECK(SendRaw(s, "x", 1));
    CHECK(c.WaitReadable(2000, readable, err));
    CHECK(readable);
    char b;
    CHECK(c.RecvExact(&b, 1, err) == RecvResult::Ok);
    CHECK(b == 'x');
    closesocket(s);
}

static void TestSendBoundedOnDeadPeer(Listener& l)
{
    /* After a RST, a send fails instead of hanging. The first send
       after the reset may still be accepted by the local stack, so
       a few are allowed; the bound is what matters. */
    TcpSocket c;
    const SOCKET s = ConnectPair(l, c);
    if(s == INVALID_SOCKET) return;
    AbortiveClose(s);
    CHECK(WaitUntilReadable(c, 2000));

    std::string err;
    bool failed = false;
    const char data[64] = {};
    for(int i = 0; i < 8 && !failed; ++i)
    {
        failed = !c.SendAll(data, sizeof(data), err);
    }
    CHECK(failed);
    CHECK(Contains(err, "send failed"));
}

int main()
{
    WSADATA wsa;
    if(WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        std::printf("SKIP: WSAStartup failed\n");
        return kSkip;
    }

    Listener l;
    std::string why;
    if(!l.Open(why))
    {
        std::printf("SKIP: loopback listener unavailable (%s)\n", why.c_str());
        WSACleanup();
        return kSkip;
    }

    TestConnectToClosedPortIsBounded();
    TestDoubleConnectAndClose(l);
    TestRoundTrip(l);
    TestLargeRoundTrip(l);
    TestTimeoutWithZeroBytes(l);
    TestPartialReadThenStallIsError(l);
    TestGracefulClose(l);
    TestPendingDataIsNotClosed(l);
    TestCloseMidRecordIsClosed(l);
    TestAbortiveClose(l);
    TestWaitReadable(l);
    TestSendBoundedOnDeadPeer(l);

    l.Close();
    WSACleanup();
    std::printf("sdk_tcp_socket_test: %d checks, %d failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
