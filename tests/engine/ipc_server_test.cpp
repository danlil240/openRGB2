// SPDX-License-Identifier: GPL-2.0-or-later
//
// Session/server tests for the engine IPC layer
// (src/ipc/IpcServer.h over injected in-memory channels,
// plus a real restricted named-pipe round-trip):
//
//   - version handshake accept/mismatch
//   - malformed/oversized/truncated/unknown-version
//     rejection with bounded errors — connection survives
//     what framing allows it to survive
//   - flood bound (rate limit)
//   - event queue bound under a non-draining client
//   - diagnostics pipe read-only-ness
//   - connection cap
//   - ACL intent on the SDDL + a real pipe round-trip

#include "IpcProtocol.h"
#include "IpcServer.h"
#include "IpcService.h"
#include "EngineService.h"
#include "NamedPipe.h"

#include "fake_ipc_channel.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#include <aclapi.h>
#endif

using namespace openrgb2::ipc;
using namespace ipc_test;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

/*---------------------------------------------------------*\
|| Scripted service — records dispatch, canned replies.     ||
\*---------------------------------------------------------*/
class ScriptedService final : public IIpcService
{
public:
    nlohmann::json OnWelcome() override
    {
        return nlohmann::json::object({{"service", "scripted"}});
    }

    /* When set, Dispatch throws — models a handler bug; the
       session layer must convert it into a bounded ERROR. */
    bool throw_on_dispatch = false;

    /* When non-zero, GetState replies carry a padding blob —
       over kMaxBodyBytes it forces the oversized-reply path. */
    size_t reply_pad = 0;

    /* DIAG_GET stand-ins for the real DiagnosticsJson cost +
       ~70 KB reply: the delay keeps the pipe empty between
       request and reply (the PeekNamedPipe-poll wedge window),
       the pad makes the reply multi-fragment. */
    uint32_t diag_delay_ms = 0;
    size_t   diag_pad      = 0;

    IpcReply Dispatch(MsgType type,
                      const nlohmann::json& body) override
    {
        (void)body;
        {
            std::lock_guard<std::mutex> lock(m_);
            dispatched_.push_back(type);
        }
        if(throw_on_dispatch)
        {
            throw std::runtime_error("injected handler failure");
        }
        if(type == MsgType::DiagGet)
        {
            if(diag_delay_ms)
            {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(diag_delay_ms));
            }
            nlohmann::json out =
                nlohmann::json::object({{"diag", true}});
            if(diag_pad)
            {
                out["blob"] = std::string(diag_pad, 'x');
            }
            return IpcReply::Success(MsgType::Diagnostics,
                                     std::move(out));
        }
        if(type == MsgType::GetState)
        {
            nlohmann::json out =
                nlohmann::json::object({{"state", true}});
            if(reply_pad)
            {
                out["blob"] = std::string(reply_pad, 'x');
            }
            return IpcReply::Success(MsgType::State,
                                     std::move(out));
        }
        if(type == MsgType::GetScene)
        {
            return IpcReply::Fail(ErrCode::Unavailable, "no scene");
        }
        return IpcReply::Success(MsgType::Ok,
            nlohmann::json::object(
                {{"handled", (int)type}}));
    }

    bool SnapshotEvent(const std::string& kind,
                       nlohmann::json& out) override
    {
        /* Taken BEFORE the hook: models a state read that a
           live change (emitted by the hook) then supersedes. */
        out = nlohmann::json::object(
            {{"kind", kind}, {"initial", true}});
        if(on_snapshot)
        {
            on_snapshot(kind);
        }
        return true;
    }

    /* Runs inside SnapshotEvent on the session reader thread —
       lets a test emit a newer live event mid-snapshot. */
    std::function<void(const std::string&)> on_snapshot;

    int DispatchedCount()
    {
        std::lock_guard<std::mutex> lock(m_);
        return (int)dispatched_.size();
    }

private:
    std::mutex            m_;
    std::vector<MsgType>  dispatched_;
};

static bool IsError(const Message& m, ErrCode code)
{
    if(m.type != MsgType::Error)
    {
        return false;
    }
    const nlohmann::json b = m.Body();
    return !b.is_discarded()
        && b.value("code", (uint32_t)0xFFFFFFFF)
               == (uint32_t)code;
}

/*---------------------------------------------------------*\
| Integrity probe (audit I-9): the suite runs at medium     |
| integrity and cannot elevate, so the elevated / low /     |
| unreadable-token paths are driven through the injected    |
| probe. -1 = "token cannot be read".                        |
\*---------------------------------------------------------*/
static long g_il_self   = (long)kIntegrityMedium;
static long g_il_server = (long)kIntegrityMedium;
static long g_il_client = (long)kIntegrityMedium;

static bool TestIntegrityProbe(IntegritySubject subject,
                               unsigned long /*pid*/, uint32_t& rid)
{
    const long v = subject == IntegritySubject::Self       ? g_il_self
                 : subject == IntegritySubject::PipeServer ? g_il_server
                                                           : g_il_client;
    if(v < 0)
    {
        return false;
    }
    rid = (uint32_t)v;
    return true;
}

/* Connect + HELLO over a real listener; true when WELCOME
   arrives. `err` carries the client-side refusal, if any. */
static bool PipeHandshake(const std::string& pipe, std::string& err)
{
    std::unique_ptr<IIpcChannel> ch =
        ConnectNamedPipeChannel(pipe, 3000, err);
    if(!ch)
    {
        return false;
    }
    MessageReader rd(*ch);
    const std::vector<uint8_t> hello = EncodeMessage(
        MsgType::Hello, 0,
        nlohmann::json::object({{ "min_version", 1 },
                                { "max_version", 1 }}));
    if(ch->SendAll(hello.data(), hello.size(), 3000, err)
       != IpcIoResult::Ok)
    {
        return false;
    }
    Message m;
    std::string detail;
    if(rd.Read(m, 3000, detail) != ReadResult::Ok)
    {
        err = "read: " + detail;
        return false;
    }
    return m.type == MsgType::Welcome;
}

/* The mandatory label ACE on a pipe object, as (rid, policy);
   false when the object carries no label. */
static bool ReadPipeLabel(const std::string& pipe, DWORD& rid,
                          DWORD& policy)
{
    HANDLE h = CreateFileA(pipe.c_str(), GENERIC_READ | GENERIC_WRITE,
                           0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, nullptr);
    if(h == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    PACL sacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    bool found = false;
    if(GetSecurityInfo(h, SE_KERNEL_OBJECT, LABEL_SECURITY_INFORMATION,
                       nullptr, nullptr, nullptr, &sacl, &sd)
           == ERROR_SUCCESS && sacl)
    {
        for(DWORD i = 0; i < sacl->AceCount && !found; ++i)
        {
            ACE_HEADER* ace = nullptr;
            if(GetAce(sacl, i, (void**)&ace) &&
               ace->AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE)
            {
                SYSTEM_MANDATORY_LABEL_ACE* ml =
                    (SYSTEM_MANDATORY_LABEL_ACE*)ace;
                PSID sid = (PSID)&ml->SidStart;
                rid    = *GetSidSubAuthority(
                    sid, *GetSidSubAuthorityCount(sid) - 1);
                policy = ml->Mask;
                found  = true;
            }
        }
    }
    if(sd)
    {
        LocalFree(sd);
    }
    CloseHandle(h);
    return found;
}

int main()
{
    /*---- handshake: accept --------------------------------*/
    {
        ScriptedService svc;
        IpcServer server(svc);
        TestClient c;
        CHECK(server.Serve(c.MakeChannel(), ChannelKind::Command));
        c.Hello();
        Message m;
        CHECK(c.RecvMsg(m, 5000));
        CHECK(m.type == MsgType::Welcome);
        const nlohmann::json b = m.Body();
        CHECK(b["negotiated_version"].get<int>() == 1);
        CHECK(b["channel"].get<std::string>() == "command");
        CHECK(b["service"].get<std::string>() == "scripted");
    }

    /*---- handshake: version mismatch ----------------------*/
    {
        ScriptedService svc;
        IpcServer server(svc);
        TestClient c;
        server.Serve(c.MakeChannel(), ChannelKind::Command);
        c.Hello(/*min*/2, /*max*/3);       /* server speaks 1 */
        Message m;
        CHECK(c.RecvMsg(m, 5000));
        CHECK(IsError(m, ErrCode::VersionMismatch));
        CHECK(WaitFor([&] { return c.State()->server_closed; }));
        const IpcServerStats st = server.Stats();
        CHECK(st.rejected_hellos == 1);
    }

    /*---- handshake: non-HELLO first ------------------------*/
    {
        ScriptedService svc;
        IpcServer server(svc);
        TestClient c;
        server.Serve(c.MakeChannel(), ChannelKind::Command);
        c.SendMsg(MsgType::GetState, 1, nlohmann::json::object());
        Message m;
        CHECK(c.RecvMsg(m, 5000));
        CHECK(IsError(m, ErrCode::ExpectedHello));
        CHECK(WaitFor([&] { return c.State()->server_closed; }));
        CHECK(svc.DispatchedCount() == 0);
    }

    /*---- wrong negotiated version mid-session -------------*/
    {
        ScriptedService svc;
        IpcServer server(svc);
        TestClient c;
        server.Serve(c.MakeChannel(), ChannelKind::Command);
        c.Hello();
        Message m;
        CHECK(c.RecvMsg(m, 5000));
        c.SendMsg(MsgType::Ping, 7, nlohmann::json::object(),
                  /*version*/9);
        CHECK(c.RecvMsg(m, 5000));
        CHECK(IsError(m, ErrCode::VersionMismatch));
        CHECK(m.request_id == 7);
        /* connection survives — a correct PING still works */
        c.SendMsg(MsgType::Ping, 8, nlohmann::json::object());
        CHECK(c.RecvMsg(m, 5000));
        CHECK(m.type == MsgType::Pong);
        CHECK(m.request_id == 8);
    }

    /*---- unknown type / malformed body ---------------------*/
    {
        ScriptedService svc;
        IpcServer server(svc);
        TestClient c;
        server.Serve(c.MakeChannel(), ChannelKind::Command);
        c.Hello();
        Message m;
        CHECK(c.RecvMsg(m, 5000));

        c.SendMsg((MsgType)0x7FFF, 10, nlohmann::json::object());
        CHECK(c.RecvMsg(m, 5000));
        CHECK(IsError(m, ErrCode::UnknownCommand));

        /* valid frame, invalid JSON body */
        std::vector<uint8_t> bad;
        auto put = [&](uint32_t v, int bytes)
        { for(int i = 0; i < bytes; ++i)
              bad.push_back((uint8_t)(v >> (8 * i))); };
        put(kMagic, 4); put(kProtocolVersion, 2);
        put((uint16_t)MsgType::GetState, 2); put(11, 4);
        put(8, 4);
        const char* junk = "not json";
        bad.insert(bad.end(), junk, junk + 8);
        c.SendBytes(bad);
        CHECK(c.RecvMsg(m, 5000));
        CHECK(IsError(m, ErrCode::Malformed));
        CHECK(m.request_id == 11);

        /* service-level unavailable error still survives */
        c.SendMsg(MsgType::GetScene, 12, nlohmann::json::object());
        CHECK(c.RecvMsg(m, 5000));
        CHECK(IsError(m, ErrCode::Unavailable));

        c.SendMsg(MsgType::Ping, 13, nlohmann::json::object());
        CHECK(c.RecvMsg(m, 5000));
        CHECK(m.type == MsgType::Pong);
    }

    /*---- garbage mid-stream: resync, survives --------------*/
    {
        ScriptedService svc;
        IpcServer server(svc);
        TestClient c;
        server.Serve(c.MakeChannel(), ChannelKind::Command);
        c.Hello();
        Message m;
        CHECK(c.RecvMsg(m, 5000));
        c.SendRaw("GARBAGE!!not-a-message");
        c.SendMsg(MsgType::Ping, 21, nlohmann::json::object());
        CHECK(c.RecvMsg(m, 5000));
        CHECK(m.type == MsgType::Pong);
        CHECK(m.request_id == 21);
        /* Resync bytes land in Stats() at session teardown —
           close the peer to surface them deterministically
           (relying on the session's read deadline to fire
           before WaitFor's own window is a race). */
        c.CloseInbound();
        CHECK(WaitFor([&]
            { return server.Stats().resync_bytes >= 20; }));
    }

    /*---- oversized message: bounded reject, survives -------*/
    {
        ScriptedService svc;
        IpcServer server(svc);
        TestClient c;
        server.Serve(c.MakeChannel(), ChannelKind::Command);
        c.Hello();
        Message m;
        CHECK(c.RecvMsg(m, 5000));

        std::vector<uint8_t> big;
        auto put = [&](uint32_t v, int bytes)
        { for(int i = 0; i < bytes; ++i)
              big.push_back((uint8_t)(v >> (8 * i))); };
        put(kMagic, 4); put(kProtocolVersion, 2);
        put((uint16_t)MsgType::SetScene, 2); put(31, 4);
        put(kMaxBodyBytes + 1, 4);
        c.SendBytes(big);
        /* The declared body — with a smuggled PING (id 666)
           inside that must NOT be answered: the body is
           discarded by length, never rescanned for magic. */
        const std::vector<uint8_t> smuggled =
            EncodeMessage(MsgType::Ping, 666, nlohmann::json::object());
        std::vector<uint8_t> body(16, '#');
        body.insert(body.end(), smuggled.begin(), smuggled.end());
        body.resize((size_t)kMaxBodyBytes + 1, '#');
        c.SendBytes(body);
        c.SendMsg(MsgType::Ping, 32, nlohmann::json::object());

        CHECK(c.RecvMsg(m, 5000));
        CHECK(IsError(m, ErrCode::TooLarge));
        CHECK(m.request_id == 31);
        CHECK(c.RecvMsg(m, 5000));
        CHECK(m.type == MsgType::Pong);   /* connection lives  */
        CHECK(m.request_id == 32);        /* not the smuggled one */
    }

    /*---- too_large counts toward the rate window ------------*/
    {
        IpcServerOptions opts;
        opts.rate_limit_per_sec = 2;
        ScriptedService svc;
        IpcServer server(svc, opts);
        TestClient c;
        server.Serve(c.MakeChannel(), ChannelKind::Command);
        c.Hello();
        Message m;
        CHECK(c.RecvMsg(m, 5000));
        /* Over-cap headers with a skippable (but still sent)
           body: each one used to get a free TooLarge reply
           outside the flood bound. */
        for(uint32_t i = 0; i < 4; ++i)
        {
            std::vector<uint8_t> hdr;
            auto put = [&](uint32_t v, int bytes)
            { for(int k = 0; k < bytes; ++k)
                  hdr.push_back((uint8_t)(v >> (8 * k))); };
            put(kMagic, 4); put(kProtocolVersion, 2);
            put((uint16_t)MsgType::SetScene, 2); put(200 + i, 4);
            put(kMaxBodyBytes + 1, 4);
            c.SendBytes(hdr);
            c.SendBytes(std::vector<uint8_t>(
                (size_t)kMaxBodyBytes + 1, '#'));
        }
        int too_large = 0, rate_limited = 0;
        while(c.RecvMsg(m, 3000))
        {
            if(IsError(m, ErrCode::TooLarge))
            {
                ++too_large;
            }
            else if(IsError(m, ErrCode::RateLimited))
            {
                ++rate_limited;
                break;
            }
        }
        CHECK(too_large == 2);
        CHECK(rate_limited == 1);
        CHECK(WaitFor([&] { return c.State()->server_closed; }));
    }

    /*---- over-cap body beyond the skip bound: closes -------*/
    {
        ScriptedService svc;
        IpcServer server(svc);
        TestClient c;
        server.Serve(c.MakeChannel(), ChannelKind::Command);
        c.Hello();
        Message m;
        CHECK(c.RecvMsg(m, 5000));
        std::vector<uint8_t> hdr;
        auto put = [&](uint32_t v, int bytes)
        { for(int k = 0; k < bytes; ++k)
              hdr.push_back((uint8_t)(v >> (8 * k))); };
        put(kMagic, 4); put(kProtocolVersion, 2);
        put((uint16_t)MsgType::SetScene, 2); put(300, 4);
        put(kMaxSkipBytes + 1, 4);
        c.SendBytes(hdr);
        CHECK(c.RecvMsg(m, 5000));
        CHECK(IsError(m, ErrCode::TooLarge));
        CHECK(m.request_id == 300);
        CHECK(WaitFor([&] { return c.State()->server_closed; }));
    }

    /*---- deeply nested body: malformed, session survives ---*/
    {
        ScriptedService svc;
        IpcServer server(svc);
        TestClient c;
        server.Serve(c.MakeChannel(), ChannelKind::Command);
        c.Hello();
        Message m;
        CHECK(c.RecvMsg(m, 5000));
        /* ~1 MiB of nesting in SUBSCRIBE's "events" and in a
           dispatched verb body: rejected at parse time by the
           depth cap — never deep-copied on the reader thread. */
        const size_t depth = kMaxBodyBytes / 2 - 64;
        const std::string nest =
            std::string(depth, '[') + std::string(depth, ']');
        const std::string sub = "{\"events\":" + nest + "}";
        std::vector<uint8_t> frame;
        auto put = [&](uint32_t v, int bytes)
        { for(int k = 0; k < bytes; ++k)
              frame.push_back((uint8_t)(v >> (8 * k))); };
        put(kMagic, 4); put(kProtocolVersion, 2);
        put((uint16_t)MsgType::Subscribe, 2); put(401, 4);
        put((uint32_t)sub.size(), 4);
        frame.insert(frame.end(), sub.begin(), sub.end());
        c.SendBytes(frame);
        CHECK(c.RecvMsg(m, 5000));
        CHECK(IsError(m, ErrCode::Malformed));
        CHECK(m.request_id == 401);

        frame.clear();
        const std::string lay = "{\"verb\":" + nest + "}";
        put(kMagic, 4); put(kProtocolVersion, 2);
        put((uint16_t)MsgType::LayoutCommand, 2); put(402, 4);
        put((uint32_t)lay.size(), 4);
        frame.insert(frame.end(), lay.begin(), lay.end());
        c.SendBytes(frame);
        CHECK(c.RecvMsg(m, 5000));
        CHECK(IsError(m, ErrCode::Malformed));
        CHECK(m.request_id == 402);

        c.SendMsg(MsgType::Ping, 403, nlohmann::json::object());
        CHECK(c.RecvMsg(m, 5000));
        CHECK(m.type == MsgType::Pong);
    }

    /*---- idle session outlives the per-message deadline ----*/
    {
        IpcServerOptions opts;
        opts.message_deadline_ms = 100;   /* per message        */
        opts.idle_deadline_ms    = 5000;  /* first-byte wait    */
        ScriptedService svc;
        IpcServer server(svc, opts);
        TestClient c;
        server.Serve(c.MakeChannel(), ChannelKind::Command);
        c.Hello();
        Message m;
        CHECK(c.RecvMsg(m, 5000));
        /* Idle for several message deadlines: the session must
           still be there (the deadline starts at a request's
           first byte, not at the previous reply). */
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        CHECK(!c.State()->server_closed);
        c.SendMsg(MsgType::Ping, 501, nlohmann::json::object());
        CHECK(c.RecvMsg(m, 5000));
        CHECK(m.type == MsgType::Pong);
        /* ...but a request that STARTS and then stalls still
           hits the per-message deadline. */
        const std::vector<uint8_t> full =
            EncodeMessage(MsgType::GetState, 502,
                          nlohmann::json::object());
        c.SendBytes(std::vector<uint8_t>(full.begin(),
                                         full.begin() + 6));
        CHECK(WaitFor([&] { return c.State()->server_closed; }));
    }

    /*---- truncated then peer close -------------------------*/
    {
        ScriptedService svc;
        IpcServer server(svc);
        TestClient c;
        server.Serve(c.MakeChannel(), ChannelKind::Command);
        c.Hello();
        Message m;
        CHECK(c.RecvMsg(m, 5000));

        const std::vector<uint8_t> full =
            EncodeMessage(MsgType::GetState, 41,
                          nlohmann::json::object());
        c.SendBytes(std::vector<uint8_t>(full.begin(),
                                         full.begin() + 10));
        c.CloseInbound();                 /* peer vanishes     */
        CHECK(WaitFor([&]
            {
                server.Emit(IpcEvent{ "topology",
                                      nlohmann::json::object() });
                return server.Stats().sessions_active == 0;
            }));
        /* server still accepts new clients */
        TestClient c2;
        CHECK(server.Serve(c2.MakeChannel(), ChannelKind::Command));
        c2.Hello();
        CHECK(c2.RecvMsg(m, 5000));
        CHECK(m.type == MsgType::Welcome);
    }

    /*---- flood bound ---------------------------------------*/
    {
        IpcServerOptions opts;
        opts.rate_limit_per_sec = 5;
        ScriptedService svc;
        IpcServer server(svc, opts);
        TestClient c;
        server.Serve(c.MakeChannel(), ChannelKind::Command);
        c.Hello();
        Message m;
        CHECK(c.RecvMsg(m, 5000));
        for(int i = 0; i < 20; ++i)
        {
            c.SendMsg(MsgType::Ping, 100 + i,
                      nlohmann::json::object());
        }
        int pongs = 0, rate_limited = 0;
        while(c.RecvMsg(m, 3000))
        {
            if(m.type == MsgType::Pong)
            {
                ++pongs;
            }
            else if(IsError(m, ErrCode::RateLimited))
            {
                ++rate_limited;
                break;                    /* then closes       */
            }
        }
        CHECK(pongs == 5);
        CHECK(rate_limited == 1);
        CHECK(WaitFor([&] { return c.State()->server_closed; }));
        CHECK(server.Stats().rate_limited == 1);
    }

    /*---- subscribe: initial snapshot push ------------------*/
    {
        ScriptedService svc;
        IpcServer server(svc);
        TestClient c;
        server.Serve(c.MakeChannel(), ChannelKind::Command);
        c.Hello();
        Message m;
        CHECK(c.RecvMsg(m, 5000));
        c.SendMsg(MsgType::Subscribe, 50,
                  nlohmann::json::object(
                      {{"events", nlohmann::json::array(
                                      {event_kind::kTopology})}}));
        CHECK(c.RecvMsg(m, 5000));
        CHECK(m.type == MsgType::Ok);
        /* the initial-state EVENT arrives without another
           request */
        CHECK(c.RecvMsg(m, 5000));
        CHECK(m.type == MsgType::Event);
        const nlohmann::json b = m.Body();
        CHECK(b["kind"].get<std::string>() == "topology");
        CHECK(b["data"]["initial"].get<bool>() == true);
    }

    /*---- audit I-4: an over-cap event is counted, never
       silently lost ----------------------------------------*/
    {
        ScriptedService svc;
        IpcServer server(svc);
        TestClient c;
        server.Serve(c.MakeChannel(), ChannelKind::Command);
        c.Hello();
        Message m;
        CHECK(c.RecvMsg(m, 5000));
        c.SendMsg(MsgType::Subscribe, 70,
                  nlohmann::json::object({{"events", "all"}}));
        CHECK(c.RecvMsg(m, 5000));
        CHECK(m.type == MsgType::Ok);
        const IpcServerStats before = server.Stats();
        server.Emit(IpcEvent{ event_kind::kScene,
            nlohmann::json::object({
                { "pad", std::string(kMaxBodyBytes + 16, 'x') } }) });
        const IpcServerStats after = server.Stats();
        CHECK(after.events_oversize == before.events_oversize + 1);
        CHECK(after.events_dropped  == before.events_dropped + 1);
    }

    /*---- event queue bound ---------------------------------*/
    {
        IpcServerOptions opts;
        opts.max_queued_events = 4;
        ScriptedService svc;
        IpcServer server(svc, opts);
        TestClient c;
        server.Serve(c.MakeChannel(), ChannelKind::Command);
        c.Hello();
        Message m;
        CHECK(c.RecvMsg(m, 5000));
        c.SendMsg(MsgType::Subscribe, 60,
                  nlohmann::json::object({{"events", "all"}}));
        CHECK(c.RecvMsg(m, 5000));
        CHECK(m.type == MsgType::Ok);
        /* Drain the initial snapshot pushes. */
        int initial = 0;
        while(initial < 5 && c.RecvMsg(m, 2000))
        {
            if(m.type == MsgType::Event)
            {
                ++initial;
            }
        }
        /* Freeze outbound: the writer wedges mid-send, so the
           bounded queue must absorb/coalesce the burst. */
        {
            std::lock_guard<std::mutex> lock(c.State()->m);
            c.State()->send_paused = true;
        }
        static const char* kinds[] = {
            event_kind::kTopology, event_kind::kScene,
            event_kind::kPlayback, event_kind::kConnection,
            event_kind::kFrameStatus,
        };
        for(int i = 0; i < 100; ++i)
        {
            server.Emit(IpcEvent{ kinds[i % 5],
                                  nlohmann::json::object(
                                      {{"n", i}}) });
        }
        const IpcServerStats st = server.Stats();
        CHECK(st.queued_events <= 4);
        CHECK(st.events_dropped > 0);
        /* Unfreeze: bounded backlog drains. */
        {
            std::lock_guard<std::mutex> lock(c.State()->m);
            c.State()->send_paused = false;
        }
        c.State()->cv_out.notify_all();
        int events = 0;
        while(events < 4 && c.RecvMsg(m, 5000))
        {
            if(m.type == MsgType::Event)
            {
                ++events;
            }
        }
        CHECK(events > 0);
    }

    /*---- overflow keeps low-rate events (audit I-8) --------*/
    {
        /* A full queue evicts high-rate telemetry (preview
           frames, frame_status) before anything else, and a
           workspace job-terminal record (coalesce key
           "job/<id>") neither replaces nor is replaced by the
           published snapshot queued just before it. Before the
           fix the job record overwrote the snapshot (latest-
           wins per KIND) and then a burst of preview chunks
           evicted the queue head — the job record. */
        IpcServerOptions opts;
        opts.max_queued_events = 4;
        ScriptedService svc;
        IpcServer server(svc, opts);
        TestClient c;
        server.Serve(c.MakeChannel(), ChannelKind::Command);
        c.Hello();
        Message m;
        CHECK(c.RecvMsg(m, 5000));
        c.SendMsg(MsgType::Subscribe, 61,
                  nlohmann::json::object({{"events",
                      nlohmann::json::array({
                          event_kind::kTopology,
                          event_kind::kWorkspace,
                          event_kind::kPreview,
                          event_kind::kFrameStatus })}}));
        CHECK(c.RecvMsg(m, 5000));
        CHECK(m.type == MsgType::Ok);
        while(c.RecvMsg(m, 500)) {}   /* initial snapshots */

        {
            std::lock_guard<std::mutex> lock(c.State()->m);
            c.State()->send_paused = true;
        }
        /* Wedge the writer on one event so everything after it
           stays queued. */
        server.Emit(IpcEvent{ event_kind::kTopology,
                              nlohmann::json::object({{"wedge", 1}}) });
        for(int i = 0; i < 200 && server.Stats().queued_events; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        server.Emit(IpcEvent{ event_kind::kWorkspace,
                              nlohmann::json::object({{"snap", 1}}) });
        server.Emit(IpcEvent{ event_kind::kWorkspace,
                              nlohmann::json::object({{"job_id", 7}}),
                              "job/7" });
        for(int i = 0; i < 20; ++i)
        {
            server.Emit(IpcEvent{ event_kind::kFrameStatus,
                                  nlohmann::json::object({{"n", i}}) });
            /* old-style per-sequence keys: distinct slots */
            server.EmitTo(1, IpcEvent{ event_kind::kPreview,
                              nlohmann::json::object({{"n", i}}) },
                          "frame/" + std::to_string(i) + "/0");
        }
        CHECK(server.Stats().queued_events <= 4);
        {
            std::lock_guard<std::mutex> lock(c.State()->m);
            c.State()->send_paused = false;
        }
        c.State()->cv_out.notify_all();
        bool snap = false, job = false;
        int  last_preview = -1;
        while(c.RecvMsg(m, 1000))
        {
            if(m.type != MsgType::Event)
            {
                continue;
            }
            const nlohmann::json b = m.Body();
            const nlohmann::json& d = b["data"];
            if(b.value("kind", "") == "workspace")
            {
                snap |= d.value("snap", 0) == 1;
                job  |= d.value("job_id", 0) == 7;
            }
            if(b.value("kind", "") == "preview")
            {
                last_preview = d.value("n", -1);
            }
        }
        CHECK(snap);
        CHECK(job);
        CHECK(last_preview == 19);   /* the newest frame survives */
    }

    /*---- SUBSCRIBE snapshot never lands after a newer event
           (audit I-13) -------------------------------------*/
    {
        /* The subscription is live before the initial snapshot
           is built, so a change emitted meanwhile is queued
           FIRST — the older snapshot pushed behind it would be
           the last word the client sees. The hook emits that
           newer event from inside the snapshot read. */
        ScriptedService svc;
        IpcServer server(svc);
        svc.on_snapshot = [&server](const std::string& kind)
        {
            if(kind == event_kind::kScene)
            {
                server.Emit(IpcEvent{ event_kind::kScene,
                    nlohmann::json::object({{"live", 2}}) });
            }
        };
        TestClient c;
        server.Serve(c.MakeChannel(), ChannelKind::Command);
        c.Hello();
        Message m;
        CHECK(c.RecvMsg(m, 5000));
        c.SendMsg(MsgType::Subscribe, 62,
                  nlohmann::json::object({{"events",
                      nlohmann::json::array({
                          event_kind::kTopology,
                          event_kind::kScene })}}));
        CHECK(c.RecvMsg(m, 5000));
        CHECK(m.type == MsgType::Ok);
        nlohmann::json last_scene, last_topology;
        while(c.RecvMsg(m, 700))
        {
            if(m.type != MsgType::Event)
            {
                continue;
            }
            const nlohmann::json b = m.Body();
            if(b.value("kind", "") == "scene")
            {
                last_scene = b["data"];
            }
            if(b.value("kind", "") == "topology")
            {
                last_topology = b["data"];
            }
        }
        /* the live change is the final scene state ... */
        CHECK(last_scene.is_object() &&
              last_scene.value("live", 0) == 2);
        /* ... and an undisturbed kind still gets its snapshot */
        CHECK(last_topology.is_object() &&
              last_topology.value("initial", false));
    }

    /*---- diagnostics channel: read-only by construction ---*/
    {
        ScriptedService svc;
        IpcServer server(svc);
        TestClient c;
        server.Serve(c.MakeChannel(), ChannelKind::Diagnostics);
        c.Hello();
        Message m;
        CHECK(c.RecvMsg(m, 5000));
        CHECK(m.Body()["channel"].get<std::string>() ==
              "diagnostics");

        /* the ONE allowed verb */
        c.SendMsg(MsgType::DiagGet, 70, nlohmann::json::object());
        CHECK(c.RecvMsg(m, 5000));
        CHECK(m.type == MsgType::Diagnostics);
        CHECK(m.Body()["diag"].get<bool>() == true);

        /* every command verb is rejected — even harmless
           ones — and the connection survives */
        c.SendMsg(MsgType::Ping, 71, nlohmann::json::object());
        CHECK(c.RecvMsg(m, 5000));
        CHECK(IsError(m, ErrCode::ReadOnly));

        c.SendMsg(MsgType::GetState, 72, nlohmann::json::object());
        CHECK(c.RecvMsg(m, 5000));
        CHECK(IsError(m, ErrCode::ReadOnly));

        c.SendMsg(MsgType::SetScene, 73,
                  nlohmann::json::object());
        CHECK(c.RecvMsg(m, 5000));
        CHECK(IsError(m, ErrCode::ReadOnly));

        c.SendMsg(MsgType::Subscribe, 74,
                  nlohmann::json::object({{"events", "all"}}));
        CHECK(c.RecvMsg(m, 5000));
        CHECK(IsError(m, ErrCode::ReadOnly));

        /* still serving its one verb */
        c.SendMsg(MsgType::DiagGet, 75, nlohmann::json::object());
        CHECK(c.RecvMsg(m, 5000));
        CHECK(m.type == MsgType::Diagnostics);
        CHECK(svc.DispatchedCount() == 2);   /* both DIAG_GETs */
    }

    /*---- QUIT reaches dispatch (wire level, Task 3.7) -------*\
    | Regression: IsRequestType once lacked Quit — the verb was|
    | rejected as UnknownCommand before EngineService ever saw  |
    | it. A real session must deliver the documented reply AND  |
    | fire the shutdown hook.                                   |
    \*----------------------------------------------------------*/
    {
        openrgb2::Engine engine;
        EngineService::Deps deps;
        bool shutdown_requested = false;
        int  stop_backend_seen  = -1;
        deps.request_shutdown =
            [&](bool stop_backend) -> nlohmann::json
            {
                shutdown_requested = true;
                stop_backend_seen  = stop_backend ? 1 : 0;
                return nlohmann::json::object(
                    { { "backend", "closing" } });
            };
        EngineService svc(engine, deps);
        IpcServer server(svc);
        TestClient c;
        CHECK(server.Serve(c.MakeChannel(), ChannelKind::Command));
        c.Hello();
        Message m;
        CHECK(c.RecvMsg(m, 5000));
        CHECK(m.type == MsgType::Welcome);

        c.SendMsg(MsgType::Quit, 90, nlohmann::json::object());
        CHECK(c.RecvMsg(m, 5000));
        CHECK(m.type == MsgType::Ok);       /* NOT UnknownCommand */
        CHECK(m.request_id == 90);
        const nlohmann::json b = m.Body();
        CHECK(b["shutting_down"].get<bool>() == true);
        CHECK(b["ordered"].get<bool>() == true);
        CHECK(shutdown_requested);
        CHECK(stop_backend_seen == 0);
        CHECK(b["stop_backend"].get<bool>() == false);
        /* The hook's returned fields merge into the same OK. */
        CHECK(b["backend"].get<std::string>() == "closing");

        /* The opt-in flag reaches the hook verbatim. */
        shutdown_requested = false;
        c.SendMsg(MsgType::Quit, 93, nlohmann::json::object(
            { { "stop_backend", true } }));
        CHECK(c.RecvMsg(m, 5000));
        CHECK(m.type == MsgType::Ok);
        CHECK(shutdown_requested);
        CHECK(stop_backend_seen == 1);
        CHECK(m.Body()["stop_backend"].get<bool>() == true);

        /* ...and on the read-only diagnostics pipe the verb is
           refused before dispatch — the hook never fires. */
        shutdown_requested = false;
        TestClient d;
        CHECK(server.Serve(d.MakeChannel(),
                           ChannelKind::Diagnostics));
        d.Hello();
        CHECK(d.RecvMsg(m, 5000));
        d.SendMsg(MsgType::Quit, 91, nlohmann::json::object());
        CHECK(d.RecvMsg(m, 5000));
        CHECK(IsError(m, ErrCode::ReadOnly));
        CHECK(!shutdown_requested);
    }

    /*---- QUIT unwired -> honest Unavailable -----------------*/
    {
        openrgb2::Engine engine;
        EngineService::Deps deps;   /* no request_shutdown */
        EngineService svc(engine, deps);
        IpcServer server(svc);
        TestClient c;
        server.Serve(c.MakeChannel(), ChannelKind::Command);
        c.Hello();
        Message m;
        CHECK(c.RecvMsg(m, 5000));
        c.SendMsg(MsgType::Quit, 92, nlohmann::json::object());
        CHECK(c.RecvMsg(m, 5000));
        CHECK(IsError(m, ErrCode::Unavailable));
    }

    /*---- REACTIVE_COMMAND reaches dispatch (wire level) ------*\
    | Regression: IsRequestType lacked ReactiveCommand — every    |
    | audio/screen/key toggle was rejected as UnknownCommand      |
    | before EngineService ever saw it (dispatch-level tests      |
    | bypass the gate, so only a real session catches it).        |
    \*------------------------------------------------------------*/
    {
        openrgb2::Engine engine;
        EngineService::Deps deps;   /* no reactive manager */
        EngineService svc(engine, deps);
        IpcServer server(svc);
        TestClient c;
        CHECK(server.Serve(c.MakeChannel(), ChannelKind::Command));
        c.Hello();
        Message m;
        CHECK(c.RecvMsg(m, 5000));
        CHECK(m.type == MsgType::Welcome);

        /* Well-formed command, unwired deps -> the handler's own
           honest Unavailable, NOT UnknownCommand. */
        c.SendMsg(MsgType::ReactiveCommand, 95, nlohmann::json::object(
            { { "verb",    "set_enabled" },
              { "input",   "audio"       },
              { "enabled", true          } }));
        CHECK(c.RecvMsg(m, 5000));
        CHECK(IsError(m, ErrCode::Unavailable));
        CHECK(!IsError(m, ErrCode::UnknownCommand));

        /* Malformed verb space -> BadRequest from dispatch,
           again proving the message crossed the gate. */
        c.SendMsg(MsgType::ReactiveCommand, 96, nlohmann::json::object(
            { { "verb", "nonsense" } }));
        CHECK(c.RecvMsg(m, 5000));
        CHECK(IsError(m, ErrCode::BadRequest));

        /* ...and on the read-only diagnostics pipe the verb is
           refused before dispatch like every command verb. */
        TestClient d;
        CHECK(server.Serve(d.MakeChannel(),
                           ChannelKind::Diagnostics));
        d.Hello();
        CHECK(d.RecvMsg(m, 5000));
        d.SendMsg(MsgType::ReactiveCommand, 97, nlohmann::json::object(
            { { "verb",    "set_enabled" },
              { "input",   "audio"       },
              { "enabled", true          } }));
        CHECK(d.RecvMsg(m, 5000));
        CHECK(IsError(m, ErrCode::ReadOnly));
    }

    /*---- connection cap ------------------------------------*/
    {
        IpcServerOptions opts;
        opts.max_connections = 1;
        ScriptedService svc;
        IpcServer server(svc, opts);
        TestClient c1;
        CHECK(server.Serve(c1.MakeChannel(), ChannelKind::Command));
        TestClient c2;
        CHECK(!server.Serve(c2.MakeChannel(),
                            ChannelKind::Command));
        Message m;
        CHECK(c2.RecvMsg(m, 5000));
        CHECK(IsError(m, ErrCode::Busy));
        CHECK(WaitFor([&] { return c2.State()->server_closed; }));
        CHECK(server.Stats().refused_busy == 1);
        /* first session unaffected */
        c1.Hello();
        CHECK(c1.RecvMsg(m, 5000));
        CHECK(m.type == MsgType::Welcome);
    }

    /*---- session cap is per channel kind --------------------*/
    {
        IpcServerOptions opts;
        opts.max_connections = 1;
        ScriptedService svc;
        IpcServer server(svc, opts);
        TestClient cmd;
        CHECK(server.Serve(cmd.MakeChannel(), ChannelKind::Command));
        /* command pipe saturated — diagnostics is NOT starved:
           each channel kind gets its own cap (doc §bounds) */
        TestClient diag;
        CHECK(server.Serve(diag.MakeChannel(),
                           ChannelKind::Diagnostics));
        diag.Hello();
        Message m;
        CHECK(diag.RecvMsg(m, 5000));
        CHECK(m.type == MsgType::Welcome);
        diag.SendMsg(MsgType::DiagGet, 85, nlohmann::json::object());
        CHECK(diag.RecvMsg(m, 5000));
        CHECK(m.type == MsgType::Diagnostics);
        /* a second session of the SAME kind is still refused */
        TestClient cmd2;
        CHECK(!server.Serve(cmd2.MakeChannel(),
                            ChannelKind::Command));
        CHECK(cmd2.RecvMsg(m, 5000));
        CHECK(IsError(m, ErrCode::Busy));
        TestClient diag2;
        CHECK(!server.Serve(diag2.MakeChannel(),
                            ChannelKind::Diagnostics));
        CHECK(diag2.RecvMsg(m, 5000));
        CHECK(IsError(m, ErrCode::Busy));
        CHECK(server.Stats().refused_busy == 2);
    }

    /*---- HELLO with wrong-typed fields: bounded error -------*/
    {
        ScriptedService svc;
        IpcServer server(svc);
        TestClient c;
        server.Serve(c.MakeChannel(), ChannelKind::Command);
        /* {"min_version":"x"} — nlohmann::value() would throw
           type_error through the session thread. Must be a
           bounded ERROR, never std::terminate. */
        c.SendMsg(MsgType::Hello, 0,
                  nlohmann::json::object(
                      {{"client_name", "bad"},
                       {"min_version", "x"}}));
        Message m;
        CHECK(c.RecvMsg(m, 5000));
        CHECK(IsError(m, ErrCode::Malformed));
        CHECK(WaitFor([&] { return c.State()->server_closed; }));
        CHECK(server.Stats().rejected_hellos == 1);

        /* array-valued version field — same treatment */
        TestClient c2;
        server.Serve(c2.MakeChannel(), ChannelKind::Command);
        c2.SendMsg(MsgType::Hello, 0,
                   nlohmann::json::object(
                       {{"max_version",
                         nlohmann::json::array({1})}}));
        CHECK(c2.RecvMsg(m, 5000));
        CHECK(IsError(m, ErrCode::Malformed));

        /* the engine is alive — a clean client still
           handshakes */
        TestClient c3;
        CHECK(server.Serve(c3.MakeChannel(), ChannelKind::Command));
        c3.Hello();
        CHECK(c3.RecvMsg(m, 5000));
        CHECK(m.type == MsgType::Welcome);
    }

    /*---- throwing handler: bounded error, session survives --*/
    {
        ScriptedService svc;
        svc.throw_on_dispatch = true;
        IpcServer server(svc);
        TestClient c;
        server.Serve(c.MakeChannel(), ChannelKind::Command);
        c.Hello();
        Message m;
        CHECK(c.RecvMsg(m, 5000));

        c.SendMsg(MsgType::GetState, 86, nlohmann::json::object());
        CHECK(c.RecvMsg(m, 5000));
        CHECK(IsError(m, ErrCode::Internal));
        CHECK(m.request_id == 86);
        /* the session thread survived — still serving */
        c.SendMsg(MsgType::Ping, 87, nlohmann::json::object());
        CHECK(c.RecvMsg(m, 5000));
        CHECK(m.type == MsgType::Pong);
        CHECK(m.request_id == 87);
    }

    /*---- ACL intent -----------------------------------------*/
    {
        std::string sddl, err;
        CHECK(BuildRestrictedSddl(sddl, err));
        /* Network logons are DENIED first (the creating user
           over SMB included), then the two allow ACEs. */
        CHECK(sddl.find("D:P(D;;GA;;;NU)(A;;GA;;;") == 0);
        CHECK(sddl.find("(A;;GA;;;SY)") != std::string::npos);
        /* no broad-identity ACEs — not default-open */
        CHECK(sddl.find("S-1-1-0") == std::string::npos); /*Everyone*/
        CHECK(sddl.find(";;;BU")  == std::string::npos);  /* Users   */
        CHECK(sddl.find(";;;AN")  == std::string::npos);  /* Anon    */
        CHECK(sddl.find("(A;;GA;;;NU)") == std::string::npos); /* no
                                                    Network allow */
        /* the DACL names the CURRENT user explicitly */
        {
            HANDLE tok = nullptr;
            CHECK(OpenProcessToken(GetCurrentProcess(),
                                   TOKEN_QUERY, &tok));
            char info[256];
            DWORD n = 0;
            CHECK(GetTokenInformation(tok, TokenUser, info,
                                      sizeof(info), &n));
            CloseHandle(tok);
            LPSTR sid = nullptr;
            CHECK(ConvertSidToStringSidA(
                ((TOKEN_USER*)info)->User.Sid, &sid));
            const std::string user_sid = sid;
            LocalFree(sid);
            CHECK(sddl.find("(A;;GA;;;" + user_sid + ")")
                  != std::string::npos);
        }
        /* and the descriptor materializes — the pipe never
           falls back to a default ACL */
        PipeSecurityAttributes psa;
        CHECK(psa.Ok());
        CHECK(psa.Raw() != nullptr);
    }

    /*---- integrity policy (audit I-9) ------------------------*\
    | The pipe carries an explicit Medium no-write-up label (a  |
    | medium UI must keep reaching an elevated engine; low /    |
    | sandboxed callers never can), and the client refuses a    |
    | server below its own integrity level.                     |
    \*---------------------------------------------------------*/
    {
        std::string sddl, err;
        CHECK(BuildRestrictedSddl(sddl, err));
        CHECK(sddl.find("D:P(D;;GA;;;NU)(A;;GA;;;") == 0);
        CHECK(sddl.find("S:(ML;;NW;;;ME)") != std::string::npos);
    }
    {
        /* Pure decision table. */
        auto decide = [](PipeOwnerKind owner, bool user_self,
                         long label, long token, uint32_t client)
        {
            PipeServerFacts f;
            f.owner               = owner;
            f.server_user_is_self = user_self;
            f.label_known         = label >= 0;
            f.label_integrity     = label >= 0 ? (uint32_t)label : 0;
            f.token_known         = token >= 0;
            f.token_integrity     = token >= 0 ? (uint32_t)token : 0;
            f.client_integrity    = client;
            std::string e;
            return EvaluatePipeServerTrust(f, e);
        };
        const long ME = (long)kIntegrityMedium, HI = (long)kIntegrityHigh,
                   LO = (long)kIntegrityLow,    SY = (long)kIntegritySystem;
        const uint32_t cME = kIntegrityMedium, cHI = kIntegrityHigh;
        /* shipped: medium UI -> elevated engine whose token the
           UI cannot read (Administrators owner, ME label) */
        CHECK( decide(PipeOwnerKind::Administrators, true, ME, -1, cME));
        /* elevated UI -> elevated engine */
        CHECK( decide(PipeOwnerKind::Administrators, true, ME, -1, cHI));
        CHECK( decide(PipeOwnerKind::Administrators, true, ME, HI, cHI));
        /* service engine */
        CHECK( decide(PipeOwnerKind::LocalSystem, false, ME, SY, cME));
        CHECK( decide(PipeOwnerKind::LocalSystem, false, ME, SY, cHI));
        /* medium <-> medium */
        CHECK( decide(PipeOwnerKind::Self, true, ME, ME, cME));
        CHECK( decide(PipeOwnerKind::Self, true, ME, -1, cME));
        /* elevated UI vs a medium same-user squatter: REFUSE */
        CHECK(!decide(PipeOwnerKind::Self, true, ME, ME, cHI));
        CHECK(!decide(PipeOwnerKind::Self, true, ME, -1, cHI));
        CHECK(!decide(PipeOwnerKind::Self, true, -1, -1, cHI));
        /* creator-owner policy: elevated engine owned by the
           user SID, token read as High */
        CHECK( decide(PipeOwnerKind::Self, true, ME, HI, cHI));
        /* elevated UI needs an Administrators/SYSTEM owner (or
           the creator-owner case above) */
        CHECK(!decide(PipeOwnerKind::Other, true, ME, HI, cHI));
        /* a contradiction resolves to the token's lower level */
        CHECK( decide(PipeOwnerKind::Administrators, true, ME, ME, cME));
        CHECK(!decide(PipeOwnerKind::Administrators, true, ME, ME, cHI));
        /* low squatter: label or token says low -> REFUSE */
        CHECK(!decide(PipeOwnerKind::Self, true, LO, -1, cME));
        CHECK(!decide(PipeOwnerKind::Self, true, ME, LO, cME));
        /* nothing readable -> REFUSE (fail closed) */
        CHECK(!decide(PipeOwnerKind::Self, true, -1, -1, cME));
        /* another user's squat -> REFUSE (pre-existing rule) */
        CHECK(!decide(PipeOwnerKind::Other, false, ME, ME, cME));
        CHECK(!decide(PipeOwnerKind::Unknown, false, ME, HI, cME));
    }
    {
        /* Real listener: the label materializes on the pipe
           object, and the injected probe drives both ends. */
        const std::string pipe =
            "\\\\.\\pipe\\openrgb2-test-integrity-" +
            std::to_string(GetCurrentProcessId());
        ScriptedService svc;
        IpcServer server(svc);
        NamedPipeListener listener(server, ChannelKind::Command, pipe);
        std::string err;
        CHECK(listener.Start(err));

        DWORD rid = 0, policy = 0;
        CHECK(ReadPipeLabel(pipe, rid, policy));
        CHECK(rid == kIntegrityMedium);
        CHECK((policy & SYSTEM_MANDATORY_LABEL_NO_WRITE_UP) != 0);

        /* no probe: the real tokens (this medium test) */
        err.clear();
        CHECK(PipeHandshake(pipe, err));

        SetIntegrityProbeForTesting(&TestIntegrityProbe);
        auto set = [](long self, long server_il, long client_il)
        {
            g_il_self = self; g_il_server = server_il;
            g_il_client = client_il;
        };
        const long ME = (long)kIntegrityMedium, HI = (long)kIntegrityHigh,
                   LO = (long)kIntegrityLow;

        /* medium client, server token unreadable (an elevated
           engine seen from a medium UI): the ME label suffices */
        set(ME, -1, ME);
        err.clear();
        CHECK(PipeHandshake(pipe, err));

        /* ELEVATED client vs this medium-owned server: refused
           before a byte goes out */
        set(HI, ME, ME);
        err.clear();
        CHECK(!PipeHandshake(pipe, err));
        CHECK(err.find("integrity") != std::string::npos);
        set(HI, -1, ME);
        err.clear();
        CHECK(!PipeHandshake(pipe, err));

        /* elevated client, server token read as High (creator-
           owner policy elevated engine): allowed */
        set(HI, HI, ME);
        err.clear();
        CHECK(PipeHandshake(pipe, err));

        /* medium client vs a LOW server: refused */
        set(ME, LO, ME);
        err.clear();
        CHECK(!PipeHandshake(pipe, err));

        /* engine side: a low-integrity client is dropped
           without a WELCOME; an unreadable one too (fail
           closed); a medium one is served */
        set(ME, ME, LO);
        err.clear();
        CHECK(!PipeHandshake(pipe, err));
        set(ME, ME, -1);
        err.clear();
        CHECK(!PipeHandshake(pipe, err));
        set(ME, ME, ME);
        err.clear();
        CHECK(PipeHandshake(pipe, err));

        SetIntegrityProbeForTesting(nullptr);
        listener.Stop();
    }

    /*---- client-side server verification --------------------*\
    | A pipe owned by this user passes VerifyPipeServer (the   |
    | listener-backed round trips below exercise it through    |
    | ConnectNamedPipeChannel too).                            |
    \*---------------------------------------------------------*/
    {
        const std::string pipe =
            "\\\\.\\pipe\\openrgb2-test-verify-" +
            std::to_string(GetCurrentProcessId());
        PipeSecurityAttributes psa;
        HANDLE srv = CreateNamedPipeA(
            pipe.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                PIPE_REJECT_REMOTE_CLIENTS,
            1, 4096, 4096, 0,
            (LPSECURITY_ATTRIBUTES)psa.Raw());
        CHECK(srv != INVALID_HANDLE_VALUE);
        if(srv != INVALID_HANDLE_VALUE)
        {
            HANDLE cli = CreateFileA(
                pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                nullptr, OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT |
                    SECURITY_IDENTIFICATION,
                nullptr);
            CHECK(cli != INVALID_HANDLE_VALUE);
            if(cli != INVALID_HANDLE_VALUE)
            {
                std::string verr;
                CHECK(VerifyPipeServer((void*)cli, verr));
                if(!verr.empty())
                {
                    std::printf("verify: %s\n", verr.c_str());
                }
                CloseHandle(cli);
            }
            CloseHandle(srv);
        }
    }

    /*---- Stop() racing Sweep() on dying sessions -----------*\
    | Emit() sweeps (joins) dead sessions on the caller's       |
    | thread while Stop() joins every session: the two must     |
    | never join the same thread concurrently.                  |
    \*---------------------------------------------------------*/
    for(int round = 0; round < 10; ++round)
    {
        ScriptedService svc;
        IpcServer server(svc);
        std::vector<std::unique_ptr<TestClient>> clients;
        for(int i = 0; i < 6; ++i)
        {
            clients.push_back(std::make_unique<TestClient>());
            server.Serve(clients.back()->MakeChannel(),
                         ChannelKind::Command);
        }
        std::atomic<bool> go{ true };
        std::thread emitter([&]
        {
            while(go.load())
            {
                server.Emit(IpcEvent{ "topology",
                                      nlohmann::json::object() });
            }
        });
        for(auto& c : clients)
        {
            c->CloseInbound();            /* sessions die       */
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(round));
        server.Stop();
        go.store(false);
        emitter.join();
        CHECK(server.SessionCount() == 0);
    }

    /*---- real pipe round-trip -------------------------------*/
    {
        const std::string pipe =
            "\\\\.\\pipe\\openrgb2-test-" +
            std::to_string(GetCurrentProcessId());
        ScriptedService svc;
        IpcServer server(svc);
        NamedPipeListener listener(server, ChannelKind::Command,
                                   pipe);
        std::string err;
        CHECK(listener.Start(err));
        if(!listener.Running())
        {
            std::printf("pipe start failed: %s\n", err.c_str());
        }
        else
        {
            std::unique_ptr<IIpcChannel> ch =
                ConnectNamedPipeChannel(pipe, 5000, err);
            CHECK(ch != nullptr);
            if(ch)
            {
                Message m;
                MessageReader rd(*ch);
                std::string detail;
                const std::vector<uint8_t> hello =
                    EncodeMessage(MsgType::Hello, 0,
                        nlohmann::json::object(
                            {{"min_version", 1},
                             {"max_version", 1}}));
                CHECK(ch->SendAll(hello.data(), hello.size(),
                                  5000, err) == IpcIoResult::Ok);
                CHECK(rd.Read(m, 5000, detail) == ReadResult::Ok);
                CHECK(m.type == MsgType::Welcome);
                CHECK(m.Body()["channel"].get<std::string>()
                      == "command");

                const std::vector<uint8_t> ping =
                    EncodeMessage(MsgType::Ping, 99,
                                  nlohmann::json::object());
                CHECK(ch->SendAll(ping.data(), ping.size(),
                                  5000, err) == IpcIoResult::Ok);
                CHECK(rd.Read(m, 5000, detail) == ReadResult::Ok);
                CHECK(m.type == MsgType::Pong);
                CHECK(m.request_id == 99);
                ch->Close();
            }
        }
        listener.Stop();
        server.Stop();
    }

    /*---- oversized reply: bounded ERROR, not silent close --*\
    | A reply body over kMaxBodyBytes used to fail            |
    | EncodeMessage -> send() -> silent session close; the    |
    | client saw an empty read error (qa F12 symptom shape). |
    | Now the client gets ERROR(too_large) for its request    |
    | id and the session survives.                           |
    \*--------------------------------------------------------*/
    {
        ScriptedService svc;
        svc.reply_pad = kMaxBodyBytes + 16;   /* encodes > cap */
        IpcServer server(svc);
        TestClient c;
        server.Serve(c.MakeChannel(), ChannelKind::Command);
        c.Hello();
        Message m;
        CHECK(c.RecvMsg(m, 5000));

        c.SendMsg(MsgType::GetState, 77, nlohmann::json::object());
        CHECK(c.RecvMsg(m, 5000));
        CHECK(IsError(m, ErrCode::TooLarge));
        CHECK(m.request_id == 77);
        CHECK(m.Body()["error"].get<std::string>() ==
              "too_large");

        /* session survives — a normal request still answers */
        c.SendMsg(MsgType::Ping, 78, nlohmann::json::object());
        CHECK(c.RecvMsg(m, 5000));
        CHECK(m.type == MsgType::Pong);
        CHECK(m.request_id == 78);
    }

    /*---- channel Close vs in-flight I/O storm (F01 suspect) -*\
    | RecvSome/SendAll used to race Close() on a raw HANDLE:    |
    | a close mid-call could free + recycle the handle under   |
    | a live ReadFile/WriteFile (heap-corruption class). The   |
    | channel now pins the handle across every Win32 call —   |
    | this storm must produce only sane results, never a      |
    | crash. Real pipe pair via listener + client connect.    |
    \*---------------------------------------------------------*/
    {
        const std::string pipe =
            "\\\\.\\pipe\\openrgb2-test-storm-" +
            std::to_string(GetCurrentProcessId());
        ScriptedService svc;
        IpcServer server(svc);
        NamedPipeListener listener(server, ChannelKind::Command,
                                   pipe);
        std::string err;
        CHECK(listener.Start(err));
        if(!listener.Running())
        {
            std::printf("storm pipe start failed: %s\n",
                        err.c_str());
        }
        else
        {
            std::unique_ptr<IIpcChannel> ch =
                ConnectNamedPipeChannel(pipe, 5000, err);
            CHECK(ch != nullptr);
            if(ch)
            {
                const std::vector<uint8_t> ping =
                    EncodeMessage(MsgType::Ping, 7,
                                  nlohmann::json::object());
                std::atomic<bool> closing{ false };
                std::atomic<bool> reader_stop{ false };
                std::atomic<int>  bad{ 0 };

                std::vector<std::thread> senders;
                for(int i = 0; i < 4; ++i)
                {
                    senders.emplace_back([&]
                    {
                        for(int j = 0; j < 5000 &&
                                       !closing.load(); ++j)
                        {
                            std::string e;
                            const IpcIoResult r =
                                ch->SendAll(ping.data(),
                                            ping.size(), 250, e);
                            /* A transport Error or a detail-less
                               Closed both mean the race still
                               leaks — only Ok/Timeout/Closed-
                               with-detail are sane. */
                            if(r == IpcIoResult::Error ||
                               (r == IpcIoResult::Closed &&
                                e.empty()))
                            {
                                ++bad;
                            }
                        }
                    });
                }
                std::thread reader([&]
                {
                    MessageReader rd(*ch);
                    Message       m;
                    std::string   detail;
                    while(!reader_stop.load())
                    {
                        if(rd.Read(m, 30, detail)
                               != ReadResult::Ok &&
                           detail.empty())
                        {
                            ++bad;   /* every failure must
                                        carry a reason */
                        }
                    }
                });
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(120));
                ch->Close();                /* mid-storm close */
                closing.store(true);
                for(std::thread& t : senders)
                {
                    t.join();
                }
                reader_stop.store(true);
                reader.join();

                CHECK(bad.load() == 0);
                /* Post-close I/O is a clean Closed + reason. */
                std::string e2;
                CHECK(ch->SendAll(ping.data(), ping.size(), 50,
                                  e2) == IpcIoResult::Closed);
                CHECK(!e2.empty());
                uint8_t b[16];
                size_t  got = 0;
                e2.clear();
                CHECK(ch->RecvSome(b, sizeof b, got, 50, e2)
                      == IpcIoResult::Closed);
                CHECK(!e2.empty());
                CHECK(!ch->IsOpen());
            }
        }
        listener.Stop();
        server.Stop();
    }

    /*---- real pipe: delayed reply under concurrent load ----*\
    | Peek-poll wedge regression (qa pipefix): RecvSome used    |
    | to PeekNamedPipe an EMPTY pipe at ~1 ms cadence while     |
    | the server's reply WriteFile was still pending — on       |
    | Windows that tight peek wedged the pending write until    |
    | the server's own send deadline fired, and the client's    |
    | 5 s read deadline lost the race ("read failed: timeout"). |
    | The overlapped read path never touches the pipe between   |
    | request and reply: this pins the functional contract — a  |
    | ~1.5 s-delayed, multi-fragment DIAG_GET reply MUST land   |
    | inside its deadline while a second session streams        |
    | ping/pong traffic through the same server.                |
    \*---------------------------------------------------------*/
    {
        const std::string pipe =
            "\\\\.\\pipe\\openrgb2-test-slow-" +
            std::to_string(GetCurrentProcessId());
        ScriptedService svc;
        svc.diag_delay_ms = 1500;      /* reply stays pending  */
        svc.diag_pad      = 70 * 1024; /* ~Diagnostics size    */
        IpcServer server(svc);
        NamedPipeListener listener(server, ChannelKind::Command,
                                   pipe);
        std::string err;
        CHECK(listener.Start(err));
        if(!listener.Running())
        {
            std::printf("slow-reply pipe start failed: %s\n",
                        err.c_str());
        }
        else
        {
            /* Concurrent traffic on a second session — requests
               and replies flow while the diag reply is still
               being computed. */
            std::atomic<bool> noise_go{ true };
            std::atomic<int>  noise_bad{ 0 };
            std::thread noise([&]
            {
                std::string e2;
                std::unique_ptr<IIpcChannel> c2 =
                    ConnectNamedPipeChannel(pipe, 5000, e2);
                if(!c2)
                {
                    ++noise_bad;
                    return;
                }
                MessageReader rd2(*c2);
                Message       m2;
                std::string   det2;
                const std::vector<uint8_t> hello2 =
                    EncodeMessage(MsgType::Hello, 0,
                        nlohmann::json::object(
                            {{"min_version", 1},
                             {"max_version", 1}}));
                if(c2->SendAll(hello2.data(), hello2.size(),
                               5000, e2) != IpcIoResult::Ok ||
                   rd2.Read(m2, 5000, det2) != ReadResult::Ok)
                {
                    ++noise_bad;
                    return;
                }
                const std::vector<uint8_t> ping =
                    EncodeMessage(MsgType::Ping, 1,
                                  nlohmann::json::object());
                while(noise_go.load())
                {
                    if(c2->SendAll(ping.data(), ping.size(),
                                   5000, e2) != IpcIoResult::Ok ||
                       rd2.Read(m2, 5000, det2) != ReadResult::Ok)
                    {
                        ++noise_bad;
                        break;
                    }
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(5));
                }
                c2->Close();
            });

            std::unique_ptr<IIpcChannel> ch =
                ConnectNamedPipeChannel(pipe, 5000, err);
            CHECK(ch != nullptr);
            if(ch)
            {
                Message m;
                MessageReader rd(*ch);
                std::string detail;
                const std::vector<uint8_t> hello =
                    EncodeMessage(MsgType::Hello, 0,
                        nlohmann::json::object(
                            {{"min_version", 1},
                             {"max_version", 1}}));
                CHECK(ch->SendAll(hello.data(), hello.size(),
                                  5000, err) == IpcIoResult::Ok);
                CHECK(rd.Read(m, 5000, detail) == ReadResult::Ok);
                CHECK(m.type == MsgType::Welcome);

                const std::vector<uint8_t> diag =
                    EncodeMessage(MsgType::DiagGet, 42,
                                  nlohmann::json::object());
                const auto t0 =
                    std::chrono::steady_clock::now();
                CHECK(ch->SendAll(diag.data(), diag.size(),
                                  5000, err) == IpcIoResult::Ok);
                /* The wedge shape: the reply arrives ~1.5 s
                   after the request with the pipe empty in
                   between — this read must NOT time out. */
                CHECK(rd.Read(m, 5000, detail) == ReadResult::Ok);
                const auto ms =
                    std::chrono::duration_cast<
                        std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
                CHECK(m.type == MsgType::Diagnostics);
                CHECK(m.request_id == 42);
                CHECK(ms >= 1400);   /* the delay really ran  */
                CHECK(ms < 5000);    /* and never wedged      */
                ch->Close();
            }
            noise_go.store(false);
            noise.join();
            CHECK(noise_bad.load() == 0);
        }
        listener.Stop();
        server.Stop();
    }

    std::printf("ipc_server_test: %d checks, %d failures\n",
                checks, failures);
    return failures ? 1 : 0;
}
