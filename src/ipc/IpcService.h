/*---------------------------------------------------------*\
|| IpcService.h                                              ||
||                                                           ||
||   The engine-side service contract behind the IPC       ||
||   session layer. The session machinery (handshake,      ||
||   framing, bounds, subscription fanout) is generic;     ||
||   IIpcService is the seam where the real engine         ||
||   (EngineService.h) or a scripted test double answers   ||
||   requests.                                               ||
||                                                           ||
||   Contract for implementers: every handler MUST be      ||
||   non-blocking — dispatch runs on the session's reader  ||
||   thread and there is NO enforced dispatch deadline    ||
||   (a thread cannot be safely timed out or cancelled     ||
||   mid-call). The per-command bound (plan section 6) is  ||
||   enforced by construction only: handlers must touch    ||
||   in-memory engine state, never I/O or unbounded        ||
||   waits. Honest residual: a future blocking handler     ||
||   pins its OWN session — one bounded slot — never       ||
||   other sessions or the engine. An exception escaping   ||
||   Dispatch is caught by the session layer and answered  ||
||   with a bounded ERROR; a throwing handler is a bug.    ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "IpcProtocol.h"

#include <nlohmann/json.hpp>

#include <string>

namespace openrgb2
{
namespace ipc
{

enum class ChannelKind
{
    Command,       /* full verb catalogue                    */
    Diagnostics,   /* read-only: DIAG_GET only               */
};

/*---------------------------------------------------------*\
|| IpcEvent — a server->client push. `kind` is one of the   ||
|| event_kind tokens; `data` is kind-specific JSON.         ||
\*---------------------------------------------------------*/
struct IpcEvent
{
    std::string    kind;
    nlohmann::json data = nlohmann::json::object();
};

struct IpcReply
{
    /* code != ErrCode::Ok is sent as ERROR{code,error,message}
       regardless of reply_type. `message` is human-readable
       detail — also carried for success replies when set. */
    ErrCode       code       = ErrCode::Ok;
    MsgType       reply_type = MsgType::Ok;
    nlohmann::json body      = nlohmann::json::object();
    std::string    message;

    static IpcReply Success(MsgType reply_type, nlohmann::json body)
    {
        IpcReply r;
        r.reply_type = reply_type;
        r.body       = std::move(body);
        return r;
    }
    static IpcReply Fail(ErrCode code, std::string message)
    {
        IpcReply r;
        r.code    = code;
        r.message = std::move(message);
        return r;
    }
};

class IIpcService
{
public:
    virtual ~IIpcService() = default;

    /* The WELCOME body (server fills protocol fields itself).
       Called once per accepted handshake. */
    virtual nlohmann::json OnWelcome() = 0;

    /* Request dispatch. `type` is one of the request codes in
       MsgType; body is the parsed JSON body (object or a
       discarded-parse sentinel — services must check
       is_discarded()). The session layer has already handled
       HELLO/SUBSCRIBE/UNSUBSCRIBE/PING and the diagnostics
       channel's read-only rule. Unknown types must answer
       UnknownCommand — never crash. */
    virtual IpcReply Dispatch(MsgType type,
                              const nlohmann::json& body) = 0;

    /* Initial-state snapshot for a freshly SUBSCRIBEd kind —
       the UI should not need a second request to paint
       current state. Return false when the kind cannot
       produce a snapshot (subscriber gets none). */
    virtual bool SnapshotEvent(const std::string& kind,
                               nlohmann::json& out) = 0;
};

} /* namespace ipc */
} /* namespace openrgb2 */
