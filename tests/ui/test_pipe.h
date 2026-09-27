// SPDX-License-Identifier: GPL-2.0-or-later
//
// In-process duplex channel fixture for tests/ui — the mirror
// image of tests/engine/fake_ipc_channel.h. There the injected
// channel was the SERVER end and the test was the client stub;
// here the client under test (EngineWireClient / EngineBridge)
// holds a PipeEnd (A side) and the test drives the peer (B
// side) — either scripted byte-level control via ServerPeer
// (fragmentation, garbage, crafted headers) or the REAL
// IpcServer::Serve machinery.
//
// Both directions are condition-variable buffered deques — no
// real pipes, no sleeps on the data path, fully deterministic.

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

namespace ui_test
{

using openrgb2::ipc::IpcIoResult;
using openrgb2::ipc::Message;
using openrgb2::ipc::MsgType;

struct PipeShared
{
    std::mutex              m;
    std::condition_variable cv;

    std::deque<uint8_t> a_to_b;      /* client -> server */
    std::deque<uint8_t> b_to_a;      /* server -> client */

    bool a_closed = false;           /* client end closed */
    bool b_closed = false;           /* server end closed */
};

/*---------------------------------------------------------*\
||| PipeEnd — one end of the shared pipe as IIpcChannel.    ||
||| is_a=true is the CLIENT end (used by the code under     ||
||| test); is_a=false is the SERVER end (for a real         ||
||| IpcServer).                                             ||
\*---------------------------------------------------------*/
class PipeEnd final : public openrgb2::ipc::IIpcChannel
{
public:
    PipeEnd(std::shared_ptr<PipeShared> st, bool is_a)
        : st_(std::move(st)), a_(is_a)
    {
    }

    IpcIoResult RecvSome(void* buf, size_t max_len, size_t& got,
                         uint32_t deadline_ms,
                         std::string& err) override
    {
        (void)err;
        got = 0;
        std::unique_lock<std::mutex> lock(st_->m);
        std::deque<uint8_t>& inbox = a_ ? st_->b_to_a : st_->a_to_b;
        const bool peer_closed = a_ ? st_->b_closed : st_->a_closed;
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(deadline_ms);
        while(inbox.empty() && !(a_ ? st_->b_closed : st_->a_closed))
        {
            if(st_->cv.wait_until(lock, deadline)
               == std::cv_status::timeout)
            {
                return IpcIoResult::Timeout;
            }
        }
        if(inbox.empty())
        {
            return IpcIoResult::Closed;
        }
        size_t n = inbox.size() < max_len ? inbox.size() : max_len;
        uint8_t* out = (uint8_t*)buf;
        for(size_t i = 0; i < n; ++i)
        {
            out[i] = inbox.front();
            inbox.pop_front();
        }
        got = n;
        (void)peer_closed;
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
            const auto ms = std::chrono::duration_cast<
                std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if(ms.count() <= 0)
            {
                return IpcIoResult::Timeout;
            }
            size_t n = 0;
            const IpcIoResult r =
                RecvSome(out + got, len - got, n,
                         (uint32_t)ms.count(), err);
            if(r != IpcIoResult::Ok)
            {
                return r;
            }
            got += n;
        }
        return IpcIoResult::Ok;
    }

    IpcIoResult SendAll(const void* data, size_t len,
                        uint32_t /*deadline_ms*/,
                        std::string& err) override
    {
        {
            std::lock_guard<std::mutex> lock(st_->m);
            if(a_ ? st_->a_closed : st_->b_closed)
            {
                return IpcIoResult::Closed;
            }
            if(a_ ? st_->b_closed : st_->a_closed)
            {
                /* Peer gone — a real pipe fails the write. */
                return IpcIoResult::Closed;
            }
            std::deque<uint8_t>& outbox =
                a_ ? st_->a_to_b : st_->b_to_a;
            const uint8_t* p = (const uint8_t*)data;
            outbox.insert(outbox.end(), p, p + len);
        }
        (void)err;
        st_->cv.notify_all();
        return IpcIoResult::Ok;
    }

    void Close() override
    {
        {
            std::lock_guard<std::mutex> lock(st_->m);
            if(a_)
            {
                st_->a_closed = true;
            }
            else
            {
                st_->b_closed = true;
            }
        }
        st_->cv.notify_all();
    }

    bool IsOpen() const override
    {
        std::lock_guard<std::mutex> lock(st_->m);
        return !(a_ ? st_->a_closed : st_->b_closed);
    }

private:
    std::shared_ptr<PipeShared> st_;
    bool                        a_;
};

/*---------------------------------------------------------*\
||| ServerPeer — the test-side driver at the B end: parses  ||
||| framed client messages out of a_to_b, injects raw or    ||
||| framed bytes into b_to_a (fragmented on demand).        ||
\*---------------------------------------------------------*/
class ServerPeer
{
public:
    explicit ServerPeer(std::shared_ptr<PipeShared> st)
        : st_(std::move(st))
    {
    }

    void SendRaw(const std::vector<uint8_t>& bytes)
    {
        {
            std::lock_guard<std::mutex> lock(st_->m);
            st_->b_to_a.insert(st_->b_to_a.end(),
                               bytes.begin(), bytes.end());
        }
        st_->cv.notify_all();
    }

    void SendMsg(MsgType type, uint32_t id,
                 const nlohmann::json& body,
                 uint16_t version = openrgb2::ipc::kProtocolVersion)
    {
        SendRaw(openrgb2::ipc::EncodeMessage(type, id, body,
                                           version));
    }

    /* Byte-at-a-time delivery — the client must reassemble. */
    void SendFragmented(const std::vector<uint8_t>& bytes,
                        size_t chunk = 1)
    {
        for(size_t i = 0; i < bytes.size(); i += chunk)
        {
            const size_t n = (bytes.size() - i) < chunk
                                 ? bytes.size() - i
                                 : chunk;
            SendRaw(std::vector<uint8_t>(bytes.begin() + i,
                                         bytes.begin() + i + n));
        }
    }

    /* Parses the next complete client->server frame. False on
       timeout or desync. */
    bool RecvMsg(Message& out, uint32_t timeout_ms)
    {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(timeout_ms);
        for(;;)
        {
            if(rx_.size() >= openrgb2::ipc::kHeaderBytes)
            {
                if(GetU32(&rx_[0]) != openrgb2::ipc::kMagic)
                {
                    return false;
                }
                const uint32_t blen = GetU32(&rx_[12]);
                if(blen > openrgb2::ipc::kMaxBodyBytes)
                {
                    return false;
                }
                if(rx_.size() >=
                   openrgb2::ipc::kHeaderBytes + blen)
                {
                    out.version    = (uint16_t)GetU16(&rx_[4]);
                    out.type       = (MsgType)GetU16(&rx_[6]);
                    out.request_id = GetU32(&rx_[8]);
                    out.body_raw.assign(
                        (const char*)&rx_[openrgb2::ipc::kHeaderBytes],
                        blen);
                    rx_.erase(rx_.begin(),
                              rx_.begin() +
                                  openrgb2::ipc::kHeaderBytes +
                                  blen);
                    return true;
                }
            }
            std::vector<uint8_t> take;
            {
                std::unique_lock<std::mutex> lock(st_->m);
                if(st_->a_to_b.empty())
                {
                    if(st_->a_closed)
                    {
                        return false;
                    }
                    if(st_->cv.wait_until(lock, deadline)
                       == std::cv_status::timeout)
                    {
                        return false;
                    }
                    continue;
                }
                take.assign(st_->a_to_b.begin(), st_->a_to_b.end());
                st_->a_to_b.clear();
            }
            rx_.insert(rx_.end(), take.begin(), take.end());
        }
    }

    /* Models the engine vanishing: client reads hit Closed. */
    void Close()
    {
        {
            std::lock_guard<std::mutex> lock(st_->m);
            st_->b_closed = true;
        }
        st_->cv.notify_all();
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

    std::shared_ptr<PipeShared> st_;
    std::vector<uint8_t>        rx_;
};

/*---------------------------------------------------------*\
||| TestPipe — one shared pipe; hands out both ends.        ||
\*---------------------------------------------------------*/
class TestPipe
{
public:
    TestPipe() : st_(std::make_shared<PipeShared>()) {}

    std::unique_ptr<openrgb2::ipc::IIpcChannel> ClientChannel()
    {
        return std::unique_ptr<openrgb2::ipc::IIpcChannel>(
            new PipeEnd(st_, true));
    }
    std::unique_ptr<openrgb2::ipc::IIpcChannel> ServerChannel()
    {
        return std::unique_ptr<openrgb2::ipc::IIpcChannel>(
            new PipeEnd(st_, false));
    }
    ServerPeer Peer() { return ServerPeer(st_); }

private:
    std::shared_ptr<PipeShared> st_;
};

/* Poll a predicate (callbacks arrive on other threads). */
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

} /* namespace ui_test */
