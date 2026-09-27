/*---------------------------------------------------------*\
||| EngineWireClient.h                                        ||
|||                                                           ||
|||   The UI-side half of the engine IPC contract           ||
|||   (docs/ipc-protocol.md): one EngineWireClient speaks   ||
|||   one session on the command pipe — HELLO/WELCOME       ||
|||   handshake, correlated request/reply, SUBSCRIBE event  ||
|||   pushes — all deadline- and bound-limited by the       ||
|||   shared codec (src/ipc/IpcProtocol.h). Runs on a       ||
|||   caller-owned thread; EngineBridge (QObject) wraps it  ||
|||   with reconnect + backoff for QML.                     ||
|||                                                           ||
|||   Deliberately Qt-free: the wire layer stays a plain    ||
|||   C++ class so the offline suite exercises it without   ||
|||   an event loop (tests/ui). Qt lives in EngineBridge    ||
|||   and above only.                                       ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#pragma once

#include "IpcChannel.h"
#include "IpcProtocol.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace openrgb2
{
namespace ui
{

class EngineWireClient
{
public:
    struct Options
    {
        std::string client_name       = "openrgb2-ui";
        uint16_t    min_version       = ipc::kProtocolVersion;
        uint16_t    max_version       = ipc::kProtocolVersion;
        bool        subscribe_all     = true;
        uint32_t    hello_deadline_ms = 5000;   /* protocol: 5 s from connect */
        uint32_t    write_deadline_ms = 5000;   /* protocol: 5 s per message  */
        uint32_t    read_deadline_ms  = 5000;   /* per-message read bound     */
        uint32_t    keepalive_ms      = 15000;  /* PING cadence; 0 = disabled */
        /* Bounded outbound queue: SendRequest only encodes + enqueues
           — a session writer thread does the (deadline-bounded) pipe
           I/O so callers on a GUI thread are never blocked by a
           wedged peer. Overflow refuses the request honestly and is
           counted in DroppedWrites(). */
        size_t      max_outbound_queue = 64;
    };

    struct Handlers
    {
        /* Every handler runs on the Run() thread — marshalling to
           the UI thread is the caller's job (EngineBridge queues). */
        std::function<void(const nlohmann::json& welcome)>   onWelcome;
        std::function<void(const nlohmann::json& event_body)> onEvent;
        std::function<void(uint32_t request_id, ipc::MsgType type,
                           const nlohmann::json& body)>       onReply;
        std::function<void(const std::string& detail)>       onProtocolError;
    };

    enum class EndReason
    {
        Stopped,         /* Stop() requested                          */
        PeerClosed,      /* orderly engine close / dropped pipe       */
        HandshakeFailed, /* connect ok but HELLO/WELCOME refused or
                            malformed (incl. version_mismatch)         */
        TransportError,  /* channel error; detail set                 */
        ResyncExceeded,  /* unrecoverable garbage past the scan bound */
    };

    EngineWireClient();
    ~EngineWireClient();

    EngineWireClient(const EngineWireClient&)            = delete;
    EngineWireClient& operator=(const EngineWireClient&) = delete;

    /* Full session on the CALLING thread: HELLO -> WELCOME ->
       (optional) SUBSCRIBE {events:"all"} -> read loop until
       Stop(), peer close, or a fatal condition. Takes ownership
       of `channel` for the duration of the run.

       keep_running (optional) is an EXTERNAL cancellation flag the
       caller owns: the session also ends when it goes false — even
       if Stop() was issued while the caller was still deciding to
       start the run. Run never clears it. */
    EndReason Run(std::unique_ptr<ipc::IIpcChannel> channel,
                  const Handlers& handlers,
                  const Options& options,
                  std::string& detail,
                  const std::atomic<bool>* keep_running = nullptr);

    /* Safe from ANY thread while a session is live — and FAST: the
       body is encoded (1 MiB bound enforced) and enqueued; the
       session writer thread performs the deadline-bounded pipe I/O.
       id_out receives the correlator the engine echoes in its reply.
       False when no session is live, the handshake has not
       completed, the serialized body exceeds the bound, or the
       bounded outbound queue is full (rejects are counted). */
    bool SendRequest(ipc::MsgType type, const nlohmann::json& body,
                     uint32_t& id_out, std::string& err);

    /* Ends the session promptly: closes the channel so the read
       loop exits without waiting out its deadline. */
    void Stop();

    bool     Live() const;
    uint16_t NegotiatedVersion() const;
    /* Requests refused by the bounded outbound queue plus frames
       still queued when a session ended — honest loss, never
       silent. */
    uint64_t DroppedWrites() const;

private:
    bool Cancelled() const;
    void WriterMain(std::shared_ptr<ipc::IIpcChannel> ch);

    Options               options_;
    std::atomic<bool>     live_{ false };
    std::atomic<bool>     stop_{ false };
    std::atomic<bool>     writer_up_{ false };
    std::atomic<uint16_t> negotiated_{ 0 };
    std::atomic<uint32_t> next_id_{ 1 };
    std::atomic<uint64_t> dropped_writes_{ 0 };

    /* Owned by the Run() caller; null when not supplied. Points at
       the caller's atomic — valid for the duration of the run. */
    const std::atomic<bool>* keep_running_ = nullptr;

    std::mutex                            ch_mu_;
    std::shared_ptr<ipc::IIpcChannel>     channel_;

    std::mutex                            out_mu_;
    std::condition_variable               out_cv_;
    std::deque<std::vector<uint8_t>>      out_queue_;
    std::thread                           writer_;
};

} /* namespace ui */
} /* namespace openrgb2 */
