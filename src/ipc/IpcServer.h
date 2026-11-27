/*---------------------------------------------------------*\
|| IpcServer.h                                               ||
||                                                           ||
||   Transport-agnostic IPC server core (plan section 4).  ||
||   Serve() takes an already-connected IIpcChannel — the  ||
||   production path hands it accepted named-pipe          ||
||   connections (NamedPipe.h), tests hand it in-process   ||
||   channels. Each accepted channel becomes a session:    ||
||   one reader thread (handshake -> bounded read ->       ||
||   dispatch -> reply) and one writer thread draining a   ||
||   bounded event queue.                                  ||
||                                                           ||
||   Bounds (plan section 6 — bounded everything):         ||
||   - max_connections live sessions; extras get a bounded │
||     ERROR(busy) and are closed                          ||
||   - message bodies capped (IpcProtocol.h), every read/  │
||     write deadline-bounded; an idle session (no request │
||     byte for idle_deadline_ms) is closed                │
||   - per-session request rate cap; a flooding client is  │
||     answered ERROR(rate_limited) and closed             │
||   - per-session event queue, per-kind latest-wins       │
||     coalescing, oldest dropped on overflow — drops are  │
||     counted, never queued unboundedly                   ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "IpcChannel.h"
#include "IpcService.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace openrgb2
{
namespace ipc
{

struct IpcServerOptions
{
    uint32_t max_connections     = 8;
    uint32_t max_queued_events   = 64;   /* per session             */
    uint32_t rate_limit_per_sec  = 128;  /* requests per session    */
    uint32_t hello_deadline_ms   = 5000; /* time allowed for HELLO  */
    uint32_t message_deadline_ms = 5000; /* per message, from its
                                            first byte              */
    uint32_t idle_deadline_ms    = 60000;/* max wait for the first
                                            byte of the next request
                                            (idle session close)    */
    uint32_t send_deadline_ms    = 5000; /* per outbound write      */
};

struct IpcServerStats
{
    uint64_t sessions_active   = 0;
    uint64_t sessions_total    = 0;
    uint64_t messages_in       = 0;
    uint64_t responses_out     = 0;
    uint64_t events_sent       = 0;
    uint64_t events_dropped    = 0;   /* queue overflow/coalesce   */
    uint64_t resync_bytes      = 0;   /* skipped garbage           */
    uint64_t rejected_hellos   = 0;
    uint64_t rate_limited      = 0;
    uint64_t refused_busy      = 0;
    uint64_t queued_events     = 0;   /* currently queued          */
};

/* Frames one EVENT ({"data":...,"kind":...}, byte-identical to
   encoding that object) serializing `data` at most once and
   never copying it into an envelope: the producer's
   pre-serialized data_json is used verbatim when present
   (audit I-7). Empty when the body exceeds the cap. */
std::vector<uint8_t> EncodeEventMessage(const IpcEvent& ev);

class IpcServer
{
public:
    explicit IpcServer(IIpcService& service,
                       IpcServerOptions options = IpcServerOptions{});
    ~IpcServer();                                  /* Stop() */

    IpcServer(const IpcServer&)            = delete;
    IpcServer& operator=(const IpcServer&) = delete;

    /* Adopts a connected channel on its own session threads.
       `kind` selects the verb surface: Diagnostics channels
       answer DIAG_GET only — read-only by construction.
       Returns false when the connection cap is reached (the
       peer still gets a bounded ERROR(busy) before close). */
    bool    Serve(std::unique_ptr<IIpcChannel> channel,
                  ChannelKind kind);

    /* Fans an event out to every subscribed session (bounded
       per-session queue; see options). Safe from any thread —
       including inside a Dispatch handler. */
    void    Emit(const IpcEvent& ev);

    /* True when at least one live session subscribed to
       `kind` — lets a producer skip BUILDING an event body
       nobody will receive. Emit/EmitTo check it themselves
       before encoding. */
    bool    HasSubscriber(const std::string& kind) const;

    /* Targeted push to ONE session (studio-import Task 7 —
       the draft-preview feed is per-session state, never a
       broadcast). Delivers only when that session subscribed
       to ev.kind. `coalesce_key` partitions the latest-wins
       replacement INSIDE the kind: two queued messages with
       the same key replace, different keys coexist — chunked
       frames pass "frame/<seq>/<chunk>" so one superseded
       frame never silently eats a newer chunk. Same bounded
       queue + counted drops as Emit. */
    void    EmitTo(uint64_t session_id, const IpcEvent& ev,
                   const std::string& coalesce_key = "");

    void    Stop();
    bool    Stopped() const;

    IpcServerStats Stats() const;
    size_t         SessionCount() const;

private:
    struct Session;

    void   Sweep();               /* join + erase dead sessions   */
    static void JoinSession(Session& s);   /* under s.join_mutex */
    void   ReaderMain(std::shared_ptr<Session> s);
    void   WriterMain(std::shared_ptr<Session> s);

    IIpcService&        service_;
    IpcServerOptions    options_;

    mutable std::mutex  mutex_;                /* sessions_ + stats */
    std::vector<std::shared_ptr<Session>> sessions_;
    std::atomic<bool>   stopped_{ false };
    uint64_t            next_id_ = 1;

    IpcServerStats      stats_;                /* under mutex_      */
};

} /* namespace ipc */
} /* namespace openrgb2 */
