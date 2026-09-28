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

uint64_t EngineWireClient::DroppedWrites() const
{
    return dropped_writes_.load();
}

bool EngineWireClient::Cancelled() const
{
    /* A Stop() issued before Run() cleared stop_ — OR the caller's
       own cancellation flag going false — must end the session;
       Run() may reset stop_ for reuse, never keep_running_. */
    return stop_.load() ||
           (keep_running_ != nullptr && !keep_running_->load());
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
    out_cv_.notify_all();   /* wake a parked writer so finish()
                               joins promptly */
}

/*---------------------------------------------------------*\
| Session writer — drains the bounded outbound queue onto  |
| the channel. Runs inside the session (started by Run()); |
| a failed or wedged send closes the channel so the reader |
| loop ends the session honestly instead of silently       |
| swallowing requests.                                     |
\*---------------------------------------------------------*/
void EngineWireClient::WriterMain(
    std::shared_ptr<ipc::IIpcChannel> ch)
{
    for(;;)
    {
        std::vector<uint8_t> frame;
        {
            std::unique_lock<std::mutex> lock(out_mu_);
            out_cv_.wait(lock, [this] {
                return !out_queue_.empty() || !live_.load();
            });
            if(out_queue_.empty())
            {
                break;              /* !live_ and nothing pending */
            }
            frame = std::move(out_queue_.front());
            out_queue_.pop_front();
        }
        std::string werr;
        if(ch->SendAll(frame.data(), frame.size(),
                       options_.write_deadline_ms, werr)
               != ipc::IpcIoResult::Ok)
        {
            /* Bounded write still failed — the peer is wedged
               or gone. Closing unblocks the read loop; whatever
               remains queued is counted as dropped by finish(). */
            ch->Close();
            break;
        }
    }
    writer_up_.store(false);
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
    if(!writer_up_.load())
    {
        err = "session closing";
        return false;
    }
    const uint32_t id = next_id_.fetch_add(1);
    const std::vector<uint8_t> wire =
        ipc::EncodeMessage(type, id, body, ver);
    if(wire.empty())
    {
        err = "encode failed (body over 1 MiB bound)";
        return false;
    }
    /* Encode + enqueue only — the writer thread owns the pipe
       write so a caller on a GUI thread can never be blocked by
       a wedged peer. Bounded: a full queue refuses honestly. */
    {
        std::lock_guard<std::mutex> lock(out_mu_);
        /* Re-check under the queue mutex: the session can die
           between the live_ test above and this enqueue (finish()
           drains the queue under the same lock). Without the
           re-check a request could land in the queue after the
           session ended — counted as dropped but reported to the
           caller as sent. */
        if(!live_.load() || !writer_up_.load())
        {
            err = "session ended";
            return false;
        }
        if(out_queue_.size() >= options_.max_outbound_queue)
        {
            dropped_writes_.fetch_add(1);
            err = "outbound queue full (" +
                  std::to_string(options_.max_outbound_queue) +
                  " pending) — engine not draining";
            return false;
        }
        out_queue_.push_back(wire);
    }
    out_cv_.notify_one();
    id_out = id;
    return true;
}

EngineWireClient::EndReason EngineWireClient::Run(
    std::unique_ptr<ipc::IIpcChannel> channel,
    const Handlers& handlers, const Options& options,
    std::string& detail,
    const std::atomic<bool>* keep_running)
{
    if(!channel)
    {
        detail = "no channel";
        return EndReason::HandshakeFailed;
    }
    /* stop_ is cleared for session reuse — a Stop() issued while
       the caller was preparing this run must NOT be lost, which is
       why the caller's keep_running flag is consulted alongside
       (and first). */
    stop_.store(false);
    negotiated_.store(0);
    options_       = options;
    keep_running_  = keep_running;
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
        out_cv_.notify_all();
        if(writer_.joinable())
        {
            /* The writer is bounded: parked on the cv it wakes on
               !live_; mid-send it unwedges on the closed channel
               or the write deadline. */
            writer_.join();
        }
        {
            /* Whatever never made it onto the wire is loss the
               caller can see — count it, never claim it sent. */
            std::lock_guard<std::mutex> lock(out_mu_);
            dropped_writes_.fetch_add(out_queue_.size());
            out_queue_.clear();
        }
        keep_running_ = nullptr;
        detail = d;
        return r;
    };

    /* A stop that landed before this Run began — bail before any
       byte goes on the wire. */
    if(Cancelled())
    {
        return finish(EndReason::Stopped, "stopped before HELLO");
    }

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
            const std::string code =
                b.is_object() ? b.value("error", std::string("?"))
                              : std::string("?");
            const std::string text =
                b.is_object() ? b.value("message", std::string())
                              : std::string();
            return finish(EndReason::HandshakeFailed,
                          "engine refused handshake: " + code +
                              " — " + text);
        }
        if(msg.type != ipc::MsgType::Welcome)
        {
            return finish(EndReason::HandshakeFailed,
                          std::string("expected WELCOME, got ") +
                              ipc::MsgTypeName(msg.type));
        }
        const nlohmann::json welcome = msg.Body();
        /* The contract demands a JSON OBJECT body — a discarded
           parse or any other value kind must fail the handshake,
           never reach .value() unchecked (it throws). */
        if(!welcome.is_object())
        {
            return finish(EndReason::HandshakeFailed,
                          "WELCOME body is not a JSON object");
        }
        if(Cancelled())
        {
            return finish(EndReason::Stopped,
                          "stopped during handshake");
        }
        if(welcome.contains("negotiated_version") &&
           (welcome["negotiated_version"].is_number_integer() ||
            welcome["negotiated_version"].is_number_unsigned()))
        {
            /* get<uint16_t>() on an out-of-range value throws
               (nlohmann out_of_range) — a hostile/buggy peer must
               fail the handshake, never the process. */
            const long long nv =
                welcome["negotiated_version"].get<long long>();
            if(nv < 0 || nv > 0xFFFF)
            {
                return finish(EndReason::HandshakeFailed,
                              "WELCOME negotiated_version out of "
                              "range");
            }
            negotiated_.store((uint16_t)nv);
        }
        else
        {
            negotiated_.store(msg.version);
        }
        if(negotiated_.load() == 0)
        {
            negotiated_.store(msg.version);
        }
        /* The diagnostics pipe answers every verb with
           read_only — a session that negotiated there can never
           serve this client's requests. Refuse it up front
           instead of degrading into a stream of read_only
           errors (docs/ipc-protocol.md §6). */
        {
            /* value() with a string default throws type_error on a
               wrong-typed field — guard the lookup instead. */
            const auto ch_it = welcome.find("channel");
            if(ch_it != welcome.end() && ch_it->is_string() &&
               ch_it->get<std::string>() == "diagnostics")
            {
                return finish(EndReason::HandshakeFailed,
                              "connected to the read-only "
                              "diagnostics pipe");
            }
        }
        /* The writer owns the pipe from here: SUBSCRIBE and every
           later request enqueue to it — SendRequest callers never
           touch the channel. */
        writer_up_.store(true);
        writer_ = std::thread(&EngineWireClient::WriterMain, this,
                              channel_);
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
      The engine closes sessions after five seconds without an
      inbound request. Check the PING cadence on every iteration:
      frequent inbound EVENTs must not starve the outbound ping.
      A read Timeout means an idle stream, not a dead peer.  */
    auto last_ping = std::chrono::steady_clock::now();
    for(;;)
    {
        if(Cancelled())
        {
            return finish(EndReason::Stopped, "stopped by client");
        }
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
