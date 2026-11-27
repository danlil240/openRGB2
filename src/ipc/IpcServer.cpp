/*---------------------------------------------------------*\
|| IpcServer.cpp                                             ||
||                                                           ||
||   Session machinery for the IPC server: handshake,      ||
||   bounded reads, dispatch, replies, and the bounded     ||
||   outbound event queue. See IpcServer.h for the bound   ||
||   inventory; docs/ipc-protocol.md for the wire contract.||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "IpcServer.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <set>
#include <thread>
#include <vector>

namespace openrgb2
{
namespace ipc
{

namespace
{

/* Is `type` a client->server request the session may dispatch?
   Reply/push codes sent by a client are never dispatched. */
bool IsRequestType(MsgType t)
{
    switch(t)
    {
    case MsgType::Hello:
    case MsgType::GetSnapshots:
    case MsgType::GetScene:
    case MsgType::SetScene:
    case MsgType::GetPlayback:
    case MsgType::SetPlaybackMode:
    case MsgType::GetDeliveryStats:
    case MsgType::GetState:
    case MsgType::LayoutCommand:
    case MsgType::Subscribe:
    case MsgType::Unsubscribe:
    case MsgType::Ping:
    case MsgType::GetPresets:
    case MsgType::Quit:
    case MsgType::WorkspaceCommand:
    case MsgType::ReactiveCommand:
    case MsgType::DiagGet:
        return true;
    default:
        return false;
    }
}

} /* anonymous namespace */

struct IpcServer::Session
{
    uint64_t                    id    = 0;
    ChannelKind                 kind  = ChannelKind::Command;
    IpcServer*                  owner = nullptr;
    std::unique_ptr<IIpcChannel> channel;

    std::thread                 reader_thread;
    std::thread                 writer_thread;
    std::atomic<bool>           stop{ false };   /* session teardown   */
    std::atomic<bool>           dead{ false };   /* reader exited      */

    /* Serializes the thread joins: Stop() and Sweep() may both
       reach a dead session (Sweep runs on Emit callers' threads)
       and std::thread::join must never run concurrently on the
       same thread object. Whoever holds it joins what is still
       joinable; the other finds nothing left to join. */
    std::mutex                  join_mutex;
    bool                        sweep_claimed = false;  /* mutex_ */

    /* One writer thread drains the bounded event queue; replies
       go out inline on the reader thread. send_mutex serializes
       channel writes between the two. */
    std::mutex                  send_mutex;

    /* Encoded event bytes are shared: one Emit encodes once
       and every subscribed session's queue holds the same
       immutable buffer. */
    using Bytes = std::shared_ptr<const std::vector<uint8_t>>;
    struct OutMsg
    {
        std::string           kind;      /* event kind ("" = none)   */
        Bytes                 bytes;
    };
    std::mutex                out_mutex;
    std::condition_variable   out_cv;
    std::deque<OutMsg>        out_queue;

    std::mutex                sub_mutex;
    std::set<std::string>     subscribed;

    /* Live-session resync counter — atomic because Stats()
       reads it on another thread while the reader may be
       concurrently handing the total off to stats_ (the
       handoff zeroes it under mutex_ so a dying session is
       never double-counted). */
    std::atomic<uint64_t>     resync_bytes{ 0 };
};

namespace
{

/* High-rate telemetry kinds (~30 Hz) — the first to go when a
   session's queue is full. Everything else (workspace job
   records and snapshots, scene, playback, topology, ...) is
   rare and a lost one leaves the client stale. */
bool IsHighRateKey(const std::string& qkey)
{
    const std::string kind = qkey.substr(0, qkey.find('\x1f'));
    return kind == event_kind::kPreview ||
           kind == event_kind::kFrameStatus;
}

/* Overflow policy for one incoming event at a full queue
   (out_mutex held). Evicts the OLDEST queued high-rate event;
   when none is queued, a high-rate newcomer is refused rather
   than displacing a low-rate event, and a low-rate newcomer
   evicts the queue head (the previous behaviour). Returns
   false when the newcomer must be dropped (audit I-8). */
template<class Queue>
bool MakeRoom(Queue& q, const std::string& incoming_qkey)
{
    for(auto it = q.begin(); it != q.end(); ++it)
    {
        if(IsHighRateKey(it->kind))
        {
            q.erase(it);
            return true;
        }
    }
    if(IsHighRateKey(incoming_qkey))
    {
        return false;
    }
    q.pop_front();
    return true;
}

} /* anonymous namespace */

/*---------------------------------------------------------*\
|| IpcServer                                                 ||
\*---------------------------------------------------------*/

IpcServer::IpcServer(IIpcService& service, IpcServerOptions options)
    : service_(service), options_(options)
{
}

IpcServer::~IpcServer()
{
    Stop();
}

bool IpcServer::Serve(std::unique_ptr<IIpcChannel> channel,
                      ChannelKind kind)
{
    if(!channel)
    {
        return false;
    }
    Sweep();

    std::shared_ptr<Session> s;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        /* The cap is PER CHANNEL KIND (docs/ipc-protocol.md:
           "concurrent sessions per pipe") — a flooded command
           pipe must never starve the diagnostics pipe. */
        size_t alive = 0;
        for(const std::shared_ptr<Session>& it : sessions_)
        {
            if(!it->dead.load() && it->kind == kind)
            {
                ++alive;
            }
        }
        if(stopped_.load() || alive >= options_.max_connections)
        {
            ++stats_.refused_busy;
        }
        else
        {
            s              = std::make_shared<Session>();
            s->id          = next_id_++;
            s->kind        = kind;
            s->owner       = this;
            s->channel     = std::move(channel);
            sessions_.push_back(s);
            ++stats_.sessions_total;
        }
    }

    if(!s)
    {
        /* Bounded refusal: tell the peer why, then close. */
        std::string err;
        const std::vector<uint8_t> msg =
            EncodeError(0, ErrCode::Busy, "connection cap reached");
        if(!msg.empty())
        {
            channel->SendAll(msg.data(), msg.size(),
                             options_.send_deadline_ms, err);
        }
        channel->Close();
        return false;
    }

    /* Thread construction is bounded too: std::thread throws
       under resource exhaustion — the session is torn down
       cleanly and the refusal counted; no escaping
       exception. */
    try
    {
        s->reader_thread =
            std::thread(&IpcServer::ReaderMain, this, s);
        s->writer_thread =
            std::thread(&IpcServer::WriterMain, this, s);
    }
    catch(...)
    {
        s->stop.store(true);
        s->channel->Close();
        s->out_cv.notify_all();
        if(s->reader_thread.joinable())
        {
            s->reader_thread.join();
        }
        if(s->writer_thread.joinable())
        {
            s->writer_thread.join();
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sessions_.erase(
                std::remove(sessions_.begin(), sessions_.end(), s),
                sessions_.end());
            ++stats_.refused_busy;
        }
        return false;
    }
    return true;
}

bool IpcServer::HasSubscriber(const std::string& kind) const
{
    std::vector<std::shared_ptr<Session>> targets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        targets = sessions_;
    }
    for(const std::shared_ptr<Session>& s : targets)
    {
        if(s->dead.load())
        {
            continue;
        }
        std::lock_guard<std::mutex> lock(s->sub_mutex);
        if(s->subscribed.find(kind) != s->subscribed.end())
        {
            return true;
        }
    }
    return false;
}

void IpcServer::Emit(const IpcEvent& ev)
{
    Sweep();

    /* Subscribers first: an event nobody subscribed to (the
       ~30 Hz frame_status with no UI attached) is never
       serialized. */
    std::vector<std::shared_ptr<Session>> targets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        targets.reserve(sessions_.size());
        for(const std::shared_ptr<Session>& s : sessions_)
        {
            if(s->dead.load())
            {
                continue;
            }
            std::lock_guard<std::mutex> slock(s->sub_mutex);
            if(s->subscribed.find(ev.kind) != s->subscribed.end())
            {
                targets.push_back(s);
            }
        }
    }
    if(targets.empty())
    {
        return;
    }

    std::vector<uint8_t> encoded =
        EncodeMessage(MsgType::Event, 0,
            nlohmann::json::object({
                { "kind", ev.kind },
                { "data", ev.data },
            }));
    if(encoded.empty())
    {
        return;
    }
    const Session::Bytes bytes =
        std::make_shared<const std::vector<uint8_t>>(std::move(encoded));

    /* Latest-wins partitions on the event's coalesce key
       inside the kind (same scheme as EmitTo). */
    const std::string qkey =
        ev.coalesce_key.empty()
            ? ev.kind
            : ev.kind + "\x1f" + ev.coalesce_key;

    for(const std::shared_ptr<Session>& s : targets)
    {
        /*---- Bounded enqueue: latest-wins per (kind, key),   ----*\
        | then eviction at the cap — high-rate kinds first (see   |
        | MakeRoom). Drops are counted, never queued unboundedly. |
        \*--------------------------------------------------------*/
        bool coalesced = false;
        bool dropped   = false;
        {
            std::lock_guard<std::mutex> lock(s->out_mutex);
            for(Session::OutMsg& m : s->out_queue)
            {
                if(m.kind == qkey)
                {
                    m.bytes   = bytes;    /* replace, don't grow   */
                    coalesced = true;
                    break;
                }
            }
            if(!coalesced)
            {
                bool room = true;
                if(s->out_queue.size() >= options_.max_queued_events)
                {
                    room    = MakeRoom(s->out_queue, qkey);
                    dropped = true;
                }
                if(room)
                {
                    s->out_queue.push_back(Session::OutMsg{ qkey, bytes });
                }
            }
        }
        s->out_cv.notify_one();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(coalesced)
            {
                ++stats_.events_dropped;   /* superseded = dropped */
            }
            if(dropped)
            {
                ++stats_.events_dropped;
            }
        }
    }
}

void IpcServer::EmitTo(uint64_t session_id, const IpcEvent& ev,
                       const std::string& coalesce_key)
{
    Sweep();

    std::shared_ptr<Session> target;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for(const std::shared_ptr<Session>& s : sessions_)
        {
            if(s->id == session_id && !s->dead.load())
            {
                target = s;
                break;
            }
        }
    }
    if(!target)
    {
        return;                    /* session died — honest drop */
    }
    {
        std::lock_guard<std::mutex> lock(target->sub_mutex);
        if(target->subscribed.find(ev.kind)
           == target->subscribed.end())
        {
            return;                /* not subscribed to this kind */
        }
    }
    /* Encoded only once a subscribed target exists. */
    std::vector<uint8_t> encoded =
        EncodeMessage(MsgType::Event, 0,
            nlohmann::json::object({
                { "kind", ev.kind },
                { "data", ev.data },
            }));
    if(encoded.empty())
    {
        return;
    }
    const Session::Bytes bytes =
        std::make_shared<const std::vector<uint8_t>>(std::move(encoded));
    /* The coalesce key partitions latest-wins INSIDE the kind:
       a queued preview chunk is replaced only by the same chunk
       SLOT of a newer frame (key frame/<chunk>) — never by a
       different chunk. Chunks carry their sequence and the
       client assembles strictly by sequence, so a partial
       frame can never be painted across sequences. */
    const std::string qkey =
        coalesce_key.empty()
            ? ev.kind
            : ev.kind + "\x1f" + coalesce_key;

    bool coalesced = false;
    bool dropped   = false;
    {
        std::lock_guard<std::mutex> lock(target->out_mutex);
        for(Session::OutMsg& m : target->out_queue)
        {
            if(m.kind == qkey)
            {
                m.bytes   = bytes;
                coalesced = true;
                break;
            }
        }
        if(!coalesced)
        {
            bool room = true;
            if(target->out_queue.size() >= options_.max_queued_events)
            {
                room    = MakeRoom(target->out_queue, qkey);
                dropped = true;
            }
            if(room)
            {
                target->out_queue.push_back(
                    Session::OutMsg{ qkey, bytes });
            }
        }
    }
    target->out_cv.notify_one();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(coalesced)
        {
            ++stats_.events_dropped;
        }
        if(dropped)
        {
            ++stats_.events_dropped;
        }
    }
}

void IpcServer::Stop()
{
    std::vector<std::shared_ptr<Session>> all;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_.store(true);
        all = sessions_;
    }
    for(const std::shared_ptr<Session>& s : all)
    {
        s->stop.store(true);
        s->channel->Close();
        s->out_cv.notify_all();
    }
    /* `all` includes sessions a concurrent Sweep() is joining
       right now (Sweep erases only after its joins): the
       join_mutex makes Stop wait for that join instead of
       racing it, so every session thread is joined when Stop
       returns. */
    for(const std::shared_ptr<Session>& s : all)
    {
        JoinSession(*s);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.clear();
    }
}

void IpcServer::JoinSession(Session& s)
{
    std::lock_guard<std::mutex> jl(s.join_mutex);
    if(s.reader_thread.joinable())
    {
        s.reader_thread.join();
    }
    if(s.writer_thread.joinable())
    {
        s.writer_thread.join();
    }
}

bool IpcServer::Stopped() const
{
    return stopped_.load();
}

IpcServerStats IpcServer::Stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    IpcServerStats out = stats_;
    for(const std::shared_ptr<Session>& s : sessions_)
    {
        if(!s->dead.load())
        {
            ++out.sessions_active;
        }
        {
            std::lock_guard<std::mutex> olock(s->out_mutex);
            out.queued_events += s->out_queue.size();
        }
        if(!s->dead.load())
        {
            out.resync_bytes += s->resync_bytes.load();
        }
    }
    return out;
}

size_t IpcServer::SessionCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

void IpcServer::Sweep()
{
    /* Once stopped, Stop() owns every join — Sweep stays out. */
    if(stopped_.load())
    {
        return;
    }
    std::vector<std::shared_ptr<Session>> corpses;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for(const std::shared_ptr<Session>& s : sessions_)
        {
            if(s->dead.load() && !s->sweep_claimed)
            {
                s->sweep_claimed = true;   /* one sweeper each */
                corpses.push_back(s);
            }
        }
    }
    /* Join outside the lock — a joining thread must never hold
       mutex_ (a dying session can be inside Emit). The corpses
       stay in sessions_ until joined, so a concurrent Stop()
       still sees (and waits for) them. */
    for(const std::shared_ptr<Session>& s : corpses)
    {
        JoinSession(*s);
    }
    if(!corpses.empty())
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for(const std::shared_ptr<Session>& s : corpses)
        {
            sessions_.erase(
                std::remove(sessions_.begin(), sessions_.end(), s),
                sessions_.end());
        }
    }
}

/*---------------------------------------------------------*\
|| Writer thread — drains the event queue.                  ||
\*---------------------------------------------------------*/
void IpcServer::WriterMain(std::shared_ptr<Session> s)
{
    for(;;)
    {
        Session::OutMsg msg;
        {
            std::unique_lock<std::mutex> lock(s->out_mutex);
            s->out_cv.wait(lock, [&]
                { return s->stop.load() || !s->out_queue.empty(); });
            if(s->out_queue.empty())   /* stop with nothing left */
            {
                return;
            }
            msg = std::move(s->out_queue.front());
            s->out_queue.pop_front();
        }
        std::string err;
        IpcIoResult r;
        {
            std::lock_guard<std::mutex> lock(s->send_mutex);
            r = s->channel->SendAll(msg.bytes->data(),
                                    msg.bytes->size(),
                                    options_.send_deadline_ms, err);
        }
        if(r != IpcIoResult::Ok)
        {
            s->channel->Close();       /* kills the reader too  */
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++stats_.events_sent;
        }
    }
}

/*---------------------------------------------------------*\
|| Reader thread — handshake, dispatch, replies.            ||
\*---------------------------------------------------------*/
void IpcServer::ReaderMain(std::shared_ptr<Session> s)
{
    MessageReader rd(*s->channel);
    uint16_t      negotiated = 0;

    /* Serialize writes against the writer thread. Returns
       false when the channel failed (session must end). */
    auto send = [&](const std::vector<uint8_t>& bytes)
    {
        if(bytes.empty())
        {
            return false;
        }
        std::string err;
        std::lock_guard<std::mutex> lock(s->send_mutex);
        return s->channel->SendAll(bytes.data(), bytes.size(),
                                   options_.send_deadline_ms,
                                   err) == IpcIoResult::Ok;
    };
    auto send_error = [&](uint32_t id, ErrCode code,
                          const std::string& msg)
    {
        return send(EncodeError(id, code, msg));
    };

    /* A dying session hands its resync total to the server
       counter; Stats() adds live sessions' own counters. The
       store of 0 under mutex_ makes the handoff atomic — a
       Stats() between the add and dead=true can never
       double-count this session. */
    auto account_resync = [&]()
    {
        const uint64_t n = rd.ResyncBytes();
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.resync_bytes += n;
        s->resync_bytes.store(0);
    };

    /*---- Handshake ---------------------------------------*\
    | First message must be HELLO; anything else gets a      |
    | bounded error and a close. The version negotiated is   |
    | pinned for the life of the connection.                 |
    \*--------------------------------------------------------*/
    {
        Message      hello;
        std::string  detail;
        const ReadResult rr =
            rd.Read(hello, options_.hello_deadline_ms, detail);
        bool ok = false;
        if(rr == ReadResult::Ok && hello.type == MsgType::Hello)
        {
            const nlohmann::json body = hello.Body();
            if(body.is_discarded() || !body.is_object())
            {
                send_error(0, ErrCode::Malformed,
                             "HELLO body is not a JSON object");
            }
            else
            {
                /* Wrong-typed version fields are malformed
                   input, not a crash: nlohmann::value() throws
                   type_error on non-numbers, and an exception
                   escaping a std::thread entry is
                   std::terminate — the whole engine process.
                   Extract defensively instead. */
                const auto ver_field = [](const nlohmann::json& b,
                                          const char* key,
                                          long long& out) -> bool
                {
                    const auto it = b.find(key);
                    if(it == b.end())
                    {
                        out = (long long)kProtocolVersion;
                        return true;
                    }
                    if(!it->is_number_integer()
                       && !it->is_number_unsigned())
                    {
                        return false;
                    }
                    out = it->get<long long>();
                    return true;
                };
                long long min_v = 0, max_v = 0;
                if(!ver_field(body, "min_version", min_v)
                   || !ver_field(body, "max_version", max_v))
                {
                    send_error(0, ErrCode::Malformed,
                        "HELLO min_version/max_version must be "
                        "integers");
                }
                else
                {
                    const long long neg =
                        std::min<long long>(max_v, kProtocolVersion);
                    if(neg < min_v || neg < 1)
                    {
                        send_error(0, ErrCode::VersionMismatch,
                            "no common protocol version (client [" +
                            std::to_string(min_v) + "," +
                            std::to_string(max_v) + "], server " +
                            std::to_string(kProtocolVersion) + ")");
                    }
                    else
                    {
                        negotiated = (uint16_t)neg;
                        nlohmann::json welcome;
                        try
                        {
                            welcome = service_.OnWelcome();
                        }
                        catch(...)
                        {
                            /* A throwing service loses its
                               welcome fields, not the engine. */
                            welcome = nlohmann::json::object();
                        }
                        if(welcome.is_discarded()
                           || !welcome.is_object())
                        {
                            welcome = nlohmann::json::object();
                        }
                        welcome["server_version"]     = kProtocolVersion;
                        welcome["negotiated_version"] = negotiated;
                        welcome["channel"] =
                            s->kind == ChannelKind::Diagnostics
                                ? "diagnostics" : "command";
                        welcome["max_body_bytes"]     = kMaxBodyBytes;
                        /* A welcome body that encodes over the
                           bound gets a bounded ERROR rather
                           than a silent handshake failure. */
                        const std::vector<uint8_t> wbytes =
                            EncodeMessage(MsgType::Welcome, 0,
                                          welcome);
                        if(wbytes.empty())
                        {
                            send_error(0, ErrCode::TooLarge,
                                "welcome body exceeds the "
                                "1 MiB bound");
                        }
                        else
                        {
                            ok = send(wbytes);
                        }
                    }
                }
            }
        }
        else if(rr == ReadResult::Ok)
        {
            send_error(hello.request_id, ErrCode::ExpectedHello,
                         "first message must be HELLO");
        }
        /* Any other read failure: the peer is already gone or
           wedged — nothing bounded left to say. */
        if(!ok)
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++stats_.rejected_hellos;
            }
            account_resync();
            try { service_.OnSessionEnd(s->id); } catch(...) {}
            s->stop.store(true);
            s->channel->Close();
            s->out_cv.notify_all();
            s->dead.store(true);
            return;
        }
    }

    /*---- Request loop -------------------------------------*/
    uint64_t window_start_ms = 0;
    uint32_t window_count    = 0;
    /* Flood bound: fixed 1 s windows, rate_limit_per_sec
       messages each — oversized (too_large) ones included. The
       limit message is bounded; then the connection closes — a
       flooded session never grows server state. */
    auto over_rate = [&](uint32_t request_id) -> bool
    {
        const uint64_t now_ms = (uint64_t)
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        if(now_ms - window_start_ms >= 1000)
        {
            window_start_ms = now_ms;
            window_count    = 0;
        }
        if(++window_count <= options_.rate_limit_per_sec)
        {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++stats_.rate_limited;
            ++stats_.responses_out;
        }
        send_error(request_id, ErrCode::RateLimited,
                   "request rate exceeded");
        return true;
    };
    for(;;)
    {
        Message     msg;
        std::string detail;
        /* Idle sessions wait up to idle_deadline_ms for the
           first byte of the next request; the per-message
           deadline starts at that first byte (docs §1). */
        const ReadResult rr =
            rd.Read(msg, options_.message_deadline_ms, detail,
                    options_.idle_deadline_ms);
        if(rr != ReadResult::Ok)
        {
            if(rr == ReadResult::TooLarge)
            {
                /* Bounded rejection; the reader discards the
                   oversized body by its declared length (never
                   rescanning it for magic), so the connection
                   survives — unless the body is beyond the skip
                   bound, which closes after the error. */
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++stats_.messages_in;
                }
                if(over_rate(msg.request_id))
                {
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++stats_.responses_out;
                }
                if(!send_error(msg.request_id, ErrCode::TooLarge,
                                 detail) || rd.Poisoned())
                {
                    break;
                }
                continue;
            }
            break;   /* Closed/Timeout/ResyncExceeded/Error */
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++stats_.messages_in;
        }

        if(over_rate(msg.request_id))
        {
            break;
        }

        /* Reply-path helper — every response is bounded and
           counted. When the serialized body exceeds
           kMaxBodyBytes (EncodeMessage returns empty) the
           client gets a bounded ERROR(too_large) instead of a
           silent close — an empty read error was the qa F12
           symptom shape. */
        auto reply = [&](MsgType rt, const nlohmann::json& body)
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++stats_.responses_out;
            }
            std::vector<uint8_t> bytes =
                EncodeMessage(rt, msg.request_id, body);
            if(bytes.empty())
            {
                bytes = EncodeError(msg.request_id,
                                    ErrCode::TooLarge,
                                    "reply exceeds the 1 MiB "
                                    "body bound");
            }
            return send(bytes);
        };
        auto reply_err = [&](ErrCode c, const std::string& m)
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++stats_.responses_out;
            }
            return send_error(msg.request_id, c, m);
        };

        /* The negotiated version is pinned: mismatched
           messages get a bounded rejection; the connection
           survives (docs/ipc-protocol.md). */
        if(msg.version != negotiated)
        {
            if(!reply_err(ErrCode::VersionMismatch,
                          "message version differs from negotiated"))
            {
                break;
            }
            continue;
        }

        /* Diagnostics channel: read-only by construction —
           DiagGet is the ONLY accepted verb. There is no
           dispatch path that can reach a mutation from this
           pipe (plan section 4). */
        if(s->kind == ChannelKind::Diagnostics &&
           msg.type != MsgType::DiagGet)
        {
            if(!reply_err(ErrCode::ReadOnly,
                          "diagnostics channel is read-only"))
            {
                break;
            }
            continue;
        }

        if(!IsRequestType(msg.type))
        {
            if(!reply_err(ErrCode::UnknownCommand,
                          "unknown message type"))
            {
                break;
            }
            continue;
        }

        switch(msg.type)
        {
        case MsgType::Hello:
            if(!reply_err(ErrCode::BadRequest, "duplicate HELLO"))
            {
                goto done;
            }
            continue;

        case MsgType::Ping:
            if(!reply(MsgType::Pong, nlohmann::json::object()))
            {
                goto done;
            }
            continue;

        case MsgType::Subscribe:
        case MsgType::Unsubscribe:
        {
            std::set<std::string> want;
            if(msg.type == MsgType::Subscribe)
            {
                const nlohmann::json body = msg.Body();
                if(body.is_discarded() || !body.is_object())
                {
                    if(!reply_err(ErrCode::Malformed,
                                  "SUBSCRIBE body is not an object"))
                    {
                        goto done;
                    }
                    continue;
                }
                /* By reference — value() would deep-copy an
                   arbitrary client container. */
                static const nlohmann::json kAll("all");
                const auto evit = body.find("events");
                const nlohmann::json& ev =
                    evit == body.end() ? kAll : *evit;
                std::vector<std::string> kinds;
                if(ev.is_string() && ev.get<std::string>() == "all")
                {
                    kinds = EventKinds();
                }
                else if(ev.is_array())
                {
                    for(const nlohmann::json& e : ev)
                    {
                        if(!e.is_string())
                        {
                            kinds.clear();
                            break;
                        }
                        kinds.push_back(e.get<std::string>());
                    }
                }
                else
                {
                    /* events present but neither "all" nor an
                       array — reject, don't silently treat as
                       an empty subscription. */
                    if(!reply_err(ErrCode::BadRequest,
                        "events must be \"all\" or an array "
                        "of kind names"))
                    {
                        goto done;
                    }
                    continue;
                }
                std::set<std::string> bad;
                for(const std::string& k : kinds)
                {
                    const std::vector<std::string>& known = EventKinds();
                    if(std::find(known.begin(), known.end(), k)
                       == known.end())
                    {
                        bad.insert(k);
                    }
                    else
                    {
                        want.insert(k);
                    }
                }
                if(!bad.empty())
                {
                    if(!reply_err(ErrCode::BadRequest,
                                  "unknown event kinds"))
                    {
                        goto done;
                    }
                    continue;
                }
            }
            {
                std::lock_guard<std::mutex> lock(s->sub_mutex);
                s->subscribed = std::move(want);
            }
            nlohmann::json subs = nlohmann::json::array();
            {
                std::lock_guard<std::mutex> lock(s->sub_mutex);
                for(const std::string& k : s->subscribed)
                {
                    subs.push_back(k);
                }
            }
            if(!reply(MsgType::Ok,
                      nlohmann::json::object({{ "subscribed", subs }})))
            {
                goto done;
            }
            /* Initial-state push: the subscriber paints current
               state without a second request. */
            std::set<std::string> kinds_now;
            {
                std::lock_guard<std::mutex> lock(s->sub_mutex);
                kinds_now = s->subscribed;
            }
            for(const std::string& k : kinds_now)
            {
                nlohmann::json snap;
                bool           have = false;
                try
                {
                    have = service_.SnapshotEventFor(s->id, k, snap)
                           && !snap.is_discarded();
                }
                catch(...)
                {
                    have = false;   /* a throwing service loses
                                       only this push */
                }
                if(have)
                {
                    const std::vector<uint8_t> ev =
                        EncodeMessage(MsgType::Event, 0,
                            nlohmann::json::object(
                                {{ "kind", k }, { "data", snap }}));
                    if(!ev.empty())
                    {
                        std::lock_guard<std::mutex> lock(s->out_mutex);
                        if(s->out_queue.size()
                           < options_.max_queued_events)
                        {
                            s->out_queue.push_back(Session::OutMsg{ k,
                                std::make_shared<
                                    const std::vector<uint8_t>>(ev) });
                        }
                    }
                }
            }
            s->out_cv.notify_all();
            continue;
        }

        default:
            break;
        }

        /* Service dispatch — handlers are non-blocking by
           contract (IpcService.h). The try/catch is NOT a
           formality: an exception escaping a std::thread
           entry is std::terminate, i.e. the whole engine
           process — and untrusted bodies reach nlohmann
           accessors that throw type_error on wrong types.
           The boundary is enforced here, not trusted. */
        {
            const nlohmann::json body = msg.Body();
            if(body.is_discarded())
            {
                if(!reply_err(ErrCode::Malformed,
                              "body is not valid JSON"))
                {
                    goto done;
                }
                continue;
            }
            IpcReply r;
            try
            {
                /* DispatchSession carries the session id so
                   session-bound surfaces (workspace transfers
                   and jobs, studio-import Task 3) bind to the
                   caller; services without session state see
                   the plain Dispatch() forward. */
                r = service_.DispatchSession(s->id, msg.type, body);
            }
            catch(const nlohmann::json::exception& e)
            {
                /* Input-shape failure inside the handler —
                   bounded bad_request, session survives. */
                r = IpcReply::Fail(ErrCode::BadRequest,
                    std::string("request body rejected: ")
                    + e.what());
            }
            catch(const std::exception& e)
            {
                r = IpcReply::Fail(ErrCode::Internal,
                    std::string("handler error: ") + e.what());
            }
            catch(...)
            {
                r = IpcReply::Fail(ErrCode::Internal,
                                   "handler error");
            }
            if(r.code != ErrCode::Ok)
            {
                /* ERROR body = {code,error,message} plus any
                   detail fields the service attached (e.g.
                   per-field scene validation errors). */
                nlohmann::json ebody = r.body.is_object()
                    ? r.body : nlohmann::json::object();
                ebody["code"]    = (uint32_t)r.code;
                ebody["error"]   = ErrCodeName(r.code);
                ebody["message"] = r.message;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++stats_.responses_out;
                }
                /* A service can attach oversized detail fields —
                   the same bounded TooLarge fallback as the
                   success path, never a silent close. */
                std::vector<uint8_t> ebytes =
                    EncodeMessage(MsgType::Error, msg.request_id,
                                  ebody);
                if(ebytes.empty())
                {
                    ebytes = EncodeError(msg.request_id,
                                         ErrCode::TooLarge,
                                         "error reply exceeds the "
                                         "1 MiB body bound");
                }
                if(!send(ebytes))
                {
                    goto done;
                }
                continue;
            }
            if(!r.message.empty())
            {
                r.body["message"] = r.message;
            }
            if(!reply(r.reply_type, r.body))
            {
                goto done;
            }
        }
    }

done:
    account_resync();
    /* Session-bound state dies with the session: queued
       workspace jobs and staged transfers belonging to this
       session are cancelled here (studio-import Task 3 — a
       commit already past the durable marker is unaffected;
       the client reconciles by revision/content hash on
       reconnect). Guarded: a throwing service must not kill
       the session teardown. */
    try { service_.OnSessionEnd(s->id); } catch(...) {}
    s->stop.store(true);
    s->channel->Close();
    s->out_cv.notify_all();
    s->dead.store(true);
}

} /* namespace ipc */
} /* namespace openrgb2 */
