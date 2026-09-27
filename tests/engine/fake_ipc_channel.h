// SPDX-License-Identifier: GPL-2.0-or-later
//
// In-process IPC test fixture for tests/engine: a shared
// FakePipeState carries both byte directions between the
// injected server-side channel (FakeIpcChannel, fed to
// IpcServer::Serve) and the test's client stub (TestClient).
// No real pipes, no sockets — server sessions still run
// their real reader/writer threads, so handshake, resync,
// bounds, and event fanout are exercised end to end while
// the byte supply stays fully deterministic (condition-
// variable waits, no sleeps).

#pragma once

#include "IpcChannel.h"
#include "IpcProtocol.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ipc_test
{

using openrgb2::ipc::IpcIoResult;
using openrgb2::ipc::Message;

struct FakePipeState
{
    std::mutex              m;
    std::condition_variable cv_in;
    std::condition_variable cv_out;

    std::deque<uint8_t>     inbound;     /* client -> server */
    std::vector<uint8_t>    outbound;    /* server -> client */

    bool                    inbound_closed = false; /* client gone    */
    bool                    server_closed  = false; /* server Closed  */
    bool                    send_paused    = false; /* wedged reader  */
    bool                    send_fails     = false; /* forced failure */
};

/*---------------------------------------------------------*\
|| FakeIpcChannel — server end. The session owns it; the    ||
|| shared state outlives the session so the test can keep   ||
|| poking after a close.                                    ||
\*---------------------------------------------------------*/
class FakeIpcChannel final : public openrgb2::ipc::IIpcChannel
{
public:
    explicit FakeIpcChannel(std::shared_ptr<FakePipeState> st)
        : st_(std::move(st))
    {
    }

    IpcIoResult RecvSome(void* buf, size_t max_len, size_t& got,
                         uint32_t deadline_ms,
                         std::string& err) override
    {
        (void)err;
        got = 0;
        std::unique_lock<std::mutex> lock(st_->m);
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(deadline_ms);
        while(st_->inbound.empty() && !st_->inbound_closed
              && !st_->server_closed)
        {
            if(st_->cv_in.wait_until(lock, deadline)
               == std::cv_status::timeout)
            {
                return IpcIoResult::Timeout;
            }
        }
        if(st_->inbound.empty())
        {
            return IpcIoResult::Closed;
        }
        size_t n = st_->inbound.size() < max_len ? st_->inbound.size()
                                                 : max_len;
        uint8_t* out = (uint8_t*)buf;
        for(size_t i = 0; i < n; ++i)
        {
            out[i] = st_->inbound.front();
            st_->inbound.pop_front();
        }
        got = n;
        return IpcIoResult::Ok;
    }

    IpcIoResult RecvExact(void* buf, size_t len,
                          uint32_t deadline_ms,
                          std::string& err) override
    {
        uint8_t* out = (uint8_t*)buf;
        size_t   got = 0;
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(deadline_ms);
        while(got < len)
        {
            const auto left =
                deadline - std::chrono::steady_clock::now();
            const auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    left);
            if(ms.count() <= 0)
            {
                return IpcIoResult::Timeout;
            }
            size_t n = 0;
            const IpcIoResult r =
                RecvSome(out + got, len - got, n, (uint32_t)ms.count(),
                         err);
            if(r != IpcIoResult::Ok)
            {
                return r;
            }
            got += n;
        }
        return IpcIoResult::Ok;
    }

    IpcIoResult SendAll(const void* data, size_t len,
                        uint32_t deadline_ms,
                        std::string& err) override
    {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(deadline_ms);
        std::unique_lock<std::mutex> lock(st_->m);
        while(st_->send_paused)
        {
            if(st_->cv_out.wait_until(lock, deadline)
               == std::cv_status::timeout)
            {
                return IpcIoResult::Timeout;
            }
        }
        if(st_->server_closed)
        {
            return IpcIoResult::Closed;
        }
        if(st_->send_fails)
        {
            err = "injected send failure";
            return IpcIoResult::Error;
        }
        const uint8_t* p = (const uint8_t*)data;
        st_->outbound.insert(st_->outbound.end(), p, p + len);
        lock.unlock();
        st_->cv_out.notify_all();
        return IpcIoResult::Ok;
    }

    void Close() override
    {
        {
            std::lock_guard<std::mutex> lock(st_->m);
            st_->server_closed = true;
        }
        st_->cv_in.notify_all();
        st_->cv_out.notify_all();
    }

    bool IsOpen() const override
    {
        std::lock_guard<std::mutex> lock(st_->m);
        return !st_->server_closed;
    }

private:
    std::shared_ptr<FakePipeState> st_;
};

/*---------------------------------------------------------*\
|| TestClient — the test-side stub: push framed messages    ||
|| into the server's inbound queue, parse framed replies    ||
|| out of outbound.                                          ||
\*---------------------------------------------------------*/
class TestClient
{
public:
    TestClient() : st_(std::make_shared<FakePipeState>()) {}

    std::shared_ptr<FakePipeState> State() const { return st_; }

    std::unique_ptr<openrgb2::ipc::IIpcChannel> MakeChannel()
    {
        return std::unique_ptr<openrgb2::ipc::IIpcChannel>(
            new FakeIpcChannel(st_));
    }

    void SendBytes(const std::vector<uint8_t>& bytes)
    {
        {
            std::lock_guard<std::mutex> lock(st_->m);
            st_->inbound.insert(st_->inbound.end(), bytes.begin(),
                                bytes.end());
        }
        st_->cv_in.notify_all();
    }

    void SendRaw(const std::string& bytes)
    {
        SendBytes(std::vector<uint8_t>(bytes.begin(), bytes.end()));
    }

    void SendMsg(openrgb2::ipc::MsgType type, uint32_t id,
                 const nlohmann::json& body,
                 uint16_t version = openrgb2::ipc::kProtocolVersion)
    {
        SendBytes(openrgb2::ipc::EncodeMessage(type, id, body,
                                             version));
    }

    void Hello(long long min_v = 1, long long max_v = 1)
    {
        SendMsg(openrgb2::ipc::MsgType::Hello, 0,
                nlohmann::json::object({
                    { "client_name", "test-client" },
                    { "min_version", min_v         },
                    { "max_version", max_v         },
                }));
    }

    /* Models a peer that vanished mid-stream. */
    void CloseInbound()
    {
        {
            std::lock_guard<std::mutex> lock(st_->m);
            st_->inbound_closed = true;
        }
        st_->cv_in.notify_all();
    }

    /* Parses the next complete server->client message (server
       output is always frame-aligned). Waits up to
       timeout_ms for the full frame; returns false when it
       never completes. rx magic is asserted by callers. */
    bool RecvMsg(openrgb2::ipc::Message& out, uint32_t timeout_ms)
    {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(timeout_ms);
        for(;;)
        {
            if(rx_.size() >= openrgb2::ipc::kHeaderBytes)
            {
                const uint32_t magic = GetU32(&rx_[0]);
                if(magic != openrgb2::ipc::kMagic)
                {
                    return false;    /* caller checks desync  */
                }
                const uint32_t blen = GetU32(&rx_[12]);
                if(blen > openrgb2::ipc::kMaxBodyBytes)
                {
                    return false;
                }
                if(rx_.size() >= openrgb2::ipc::kHeaderBytes + blen)
                {
                    out.version    = (uint16_t)GetU16(&rx_[4]);
                    out.type       = (openrgb2::ipc::MsgType)
                        GetU16(&rx_[6]);
                    out.request_id = GetU32(&rx_[8]);
                    out.body_raw.assign(
                        (const char*)&rx_[openrgb2::ipc::kHeaderBytes],
                        blen);
                    rx_.erase(rx_.begin(),
                              rx_.begin() +
                                  openrgb2::ipc::kHeaderBytes + blen);
                    return true;
                }
            }
            {
                std::unique_lock<std::mutex> lock(st_->m);
                const size_t have = st_->outbound.size() - rx_pull_;
                if(have == 0)
                {
                    if(st_->cv_out.wait_until(lock, deadline)
                       == std::cv_status::timeout)
                    {
                        return false;
                    }
                    continue;
                }
                rx_.insert(rx_.end(),
                           st_->outbound.begin() + rx_pull_,
                           st_->outbound.end());
                rx_pull_ = st_->outbound.size();
            }
        }
    }

    /* Bytes the server already wrote (consumed or not). */
    size_t OutboundTotal()
    {
        std::lock_guard<std::mutex> lock(st_->m);
        return st_->outbound.size();
    }

private:
    static uint16_t GetU16(const uint8_t* p)
    {
        return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
    }
    static uint32_t GetU32(const uint8_t* p)
    {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
             | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }

    std::shared_ptr<FakePipeState> st_;
    std::vector<uint8_t>           rx_;
    size_t                         rx_pull_ = 0;
};

/* Poll a predicate for up to timeout_ms (server-side state is
   asynchronous to the test thread). */
template <typename F>
bool WaitFor(F&& fn, uint32_t timeout_ms = 5000)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeout_ms);
    while(std::chrono::steady_clock::now() < deadline)
    {
        if(fn())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return fn();
}

} /* namespace ipc_test */
