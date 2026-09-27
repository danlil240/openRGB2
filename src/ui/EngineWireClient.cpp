/*---------------------------------------------------------*\
||| EngineWireClient.cpp                                      ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#include "EngineWireClient.h"

#include <chrono>

namespace openrgb2
{
namespace ui
{

EngineWireClient::EngineWireClient()  = default;
EngineWireClient::~EngineWireClient()
{
    Stop();
}

bool EngineWireClient::Live() const
{
    return live_.load();
}

uint16_t EngineWireClient::NegotiatedVersion() const
{
    return negotiated_.load();
}

void EngineWireClient::Stop()
{
    stop_.store(true);
    std::shared_ptr<ipc::IIpcChannel> ch;
    {
        std::lock_guard<std::mutex> lock(ch_mu_);
        ch = channel_;
    }
    /* Close cancels in-flight I/O (deadline polls observe the
       cleared handle; CancelIoEx + CloseHandle inside). The
       read loop unwinds on its own — never joined here. */
    if(ch)
    {
        ch->Close();
    }
}

bool EngineWireClient::SendFrame(ipc::MsgType type, uint32_t request_id,
                                 const nlohmann::json& body,
                                 uint16_t version, std::string& err)
{
    const std::vector<uint8_t> wire =
        ipc::EncodeMessage(type, request_id, body, version);
    if(wire.empty())
    {
        err = "encode failed (body over 1 MiB bound)";
        return false;
    }
    std::shared_ptr<ipc::IIpcChannel> ch;
    {
        std::lock_guard<std::mutex> lock(ch_mu_);
        ch = channel_;
    }
    if(!ch)
    {
        err = "session closed";
        return false;
    }
    /* Duplex pipe: a concurrent SendAll while the reader polls
       is safe — guard only against interleaved writes. */
    std::lock_guard<std::mutex> send_lock(send_mu_);
    return ch->SendAll(wire.data(), wire.size(),
                       options_.write_deadline_ms,
                       err) == ipc::IpcIoResult::Ok;
}

bool EngineWireClient::SendRequest(ipc::MsgType type,
                                   const nlohmann::json& body,
                                   uint32_t& id_out, std::string& err)
{
    if(!live_.load())
    {
        err = "not connected";
        return false;
    }
    const uint16_t ver = negotiated_.load();
    if(ver == 0)
    {
        err = "handshake incomplete";
        return false;
    }
    const uint32_t id = next_id_.fetch_add(1);
    if(!SendFrame(type, id, body, ver, err))
    {
        return false;
    }
    id_out = id;
    return true;
}

EngineWireClient::EndReason EngineWireClient::Run(
    std::unique_ptr<ipc::IIpcChannel> channel,
    const Handlers& handlers, const Options& options,
    std::string& detail)
{
    if(!channel)
    {
        detail = "no channel";
        return EndReason::HandshakeFailed;
    }
    stop_.store(false);
    negotiated_.store(0);
    options_ = options;
    {
        std::lock_guard<std::mutex> lock(ch_mu_);
        channel_ = std::shared_ptr<ipc::IIpcChannel>(std::move(channel));
    }

    auto finish = [&](EndReason r, const std::string& d)
    {
        live_.store(false);
        negotiated_.store(0);
        {
            std::lock_guard<std::mutex> lock(ch_mu_);
            if(channel_)
            {
                channel_->Close();
                channel_.reset();
            }
        }
        detail = d;
        return r;
    };

    ipc::IIpcChannel* ch = channel_.get();

    /*---- HELLO (must be the first message on the stream) ----*/
    std::string err;
    const std::vector<uint8_t> hello = ipc::EncodeMessage(
        ipc::MsgType::Hello, 0,
        nlohmann::json::object({
            { "client_name", options.client_name },
            { "min_version", options.min_version },
            { "max_version", options.max_version },
        }));
    if(hello.empty() ||
       ch->SendAll(hello.data(), hello.size(),
                   options.hello_deadline_ms, err)
           != ipc::IpcIoResult::Ok)
    {
        return finish(EndReason::HandshakeFailed,
                      "HELLO write failed: " + err);
    }

    ipc::MessageReader reader(*ch);
    {
        ipc::Message msg;
        std::string rd;
        if(reader.Read(msg, options.hello_deadline_ms, rd)
           != ipc::ReadResult::Ok)
        {
            return finish(EndReason::HandshakeFailed,
                          "WELCOME read failed: " + rd);
        }
        if(msg.type == ipc::MsgType::Error)
        {
            /* version_mismatch / expected_hello / busy — all
               surface honestly; the engine closes after this. */
            const nlohmann::json b = msg.Body();
            return finish(EndReason::HandshakeFailed,
                          "engine refused handshake: " +
                              b.value("error", std::string("?")) +
                              " — " +
                              b.value("message", std::string()));
        }
        if(msg.type != ipc::MsgType::Welcome)
        {
            return finish(EndReason::HandshakeFailed,
                          std::string("expected WELCOME, got ") +
                              ipc::MsgTypeName(msg.type));
        }
        const nlohmann::json welcome = msg.Body();
        if(welcome.is_discarded())
        {
            return finish(EndReason::HandshakeFailed,
                          "WELCOME body is not valid JSON");
        }
        negotiated_.store(
            (uint16_t)welcome.value("negotiated_version",
                                    (int)msg.version));
        if(negotiated_.load() == 0)
        {
            negotiated_.store(msg.version);
        }
        live_.store(true);
        if(handlers.onWelcome)
        {
            handlers.onWelcome(welcome);
        }
    }

    /*---- SUBSCRIBE — the push channel; each kind answers with
           an initial EVENT so the UI never needs a second pull
           to paint. Replies/EVENTs flow through the loop.  ----*/
    if(options.subscribe_all)
    {
        uint32_t    id = 0;
        std::string e2;
        if(!SendRequest(ipc::MsgType::Subscribe,
                        nlohmann::json::object({ { "events", "all" } }),
                        id, e2))
        {
            return finish(EndReason::TransportError,
                          "SUBSCRIBE write failed: " + e2);
        }
    }

    /*---- Read loop -------------------------------------------
      Deadline semantics: a Timeout means no complete message
      arrived inside the bound — idle stream, not a dead peer;
      loop (and send a bounded keepalive PING on cadence to
      prove the pipe). Peer close / transport error end it.  */
    auto last_ping = std::chrono::steady_clock::now();
    for(;;)
    {
        if(stop_.load())
        {
            return finish(EndReason::Stopped, "stopped by client");
        }
        ipc::Message in;
        std::string det;
        const ipc::ReadResult rr =
            reader.Read(in, options.read_deadline_ms, det);
        switch(rr)
        {
        case ipc::ReadResult::Ok:
            if(negotiated_.load() != 0 &&
               in.version != negotiated_.load())
            {
                if(handlers.onProtocolError)
                {
                    handlers.onProtocolError(
                        "message version " +
                        std::to_string(in.version) +
                        " != negotiated " +
                        std::to_string(negotiated_.load()));
                }
                continue;
            }
            if(in.type == ipc::MsgType::Event)
            {
                if(handlers.onEvent)
                {
                    handlers.onEvent(in.Body());
                }
            }
            else if(in.request_id != 0)
            {
                if(handlers.onReply)
                {
                    handlers.onReply(in.request_id, in.type,
                                     in.Body());
                }
            }
            else if(handlers.onProtocolError)
            {
                handlers.onProtocolError(
                    std::string("unsolicited message ") +
                    ipc::MsgTypeName(in.type));
            }
            break;
        case ipc::ReadResult::Timeout:
            if(options.keepalive_ms != 0)
            {
                const auto now = std::chrono::steady_clock::now();
                if(std::chrono::duration_cast<
                       std::chrono::milliseconds>(now - last_ping)
                       .count() >= (long long)options.keepalive_ms)
                {
                    last_ping = now;
                    uint32_t    id = 0;
                    std::string e2;
                    if(!SendRequest(ipc::MsgType::Ping,
                                    nlohmann::json::object(), id, e2))
                    {
                        return finish(EndReason::TransportError,
                                      "keepalive write failed: " + e2);
                    }
                }
            }
            break;
        case ipc::ReadResult::TooLarge:
            /* Body skipped by resync; stream survives. */
            if(handlers.onProtocolError)
            {
                handlers.onProtocolError(det);
            }
            break;
        case ipc::ReadResult::Closed:
            return finish(EndReason::PeerClosed,
                          det.empty()
                              ? "engine closed the pipe"
                              : det);
        case ipc::ReadResult::ResyncExceeded:
            return finish(EndReason::ResyncExceeded, det);
        case ipc::ReadResult::TransportError:
        default:
            return finish(EndReason::TransportError, det);
        }
    }
}

} /* namespace ui */
} /* namespace openrgb2 */
