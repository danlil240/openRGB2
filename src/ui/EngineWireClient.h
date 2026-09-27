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
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

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
       of `channel` for the duration of the run. */
    EndReason Run(std::unique_ptr<ipc::IIpcChannel> channel,
                  const Handlers& handlers,
                  const Options& options,
                  std::string& detail);

    /* Safe from ANY thread while a session is live. Frames and
       sends a request; id_out receives the correlator the engine
       echoes in its reply. False when no session is live, the
       handshake has not completed, or the serialized body
       exceeds the 1 MiB bound (encode is bounded too). */
    bool SendRequest(ipc::MsgType type, const nlohmann::json& body,
                     uint32_t& id_out, std::string& err);

    /* Ends the session promptly: closes the channel so the read
       loop exits without waiting out its deadline. */
    void Stop();

    bool     Live() const;
    uint16_t NegotiatedVersion() const;

private:
    bool SendFrame(ipc::MsgType type, uint32_t request_id,
                   const nlohmann::json& body, uint16_t version,
                   std::string& err);

    Options               options_;
    std::atomic<bool>     live_{ false };
    std::atomic<bool>     stop_{ false };
    std::atomic<uint16_t> negotiated_{ 0 };
    std::atomic<uint32_t> next_id_{ 1 };

    std::mutex                            send_mu_;
    std::mutex                            ch_mu_;
    std::shared_ptr<ipc::IIpcChannel>     channel_;
};

} /* namespace ui */
} /* namespace openrgb2 */
