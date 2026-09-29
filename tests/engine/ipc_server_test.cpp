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

#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
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
            return IpcReply::Success(MsgType::Diagnostics,
                nlohmann::json::object({{"diag", true}}));
        }
        if(type == MsgType::GetState)
        {
            return IpcReply::Success(MsgType::State,
                nlohmann::json::object({{"state", true}}));
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
        out = nlohmann::json::object(
            {{"kind", kind}, {"initial", true}});
        return true;
    }

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
        c.SendRaw(std::string(256, '#'));   /* part of "body"  */
        c.SendMsg(MsgType::Ping, 32, nlohmann::json::object());

        CHECK(c.RecvMsg(m, 5000));
        CHECK(IsError(m, ErrCode::TooLarge));
        CHECK(m.request_id == 31);
        CHECK(c.RecvMsg(m, 5000));
        CHECK(m.type == MsgType::Pong);   /* connection lives  */
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
        CHECK(sddl.find("D:P(A;;GA;;;") == 0);
        CHECK(sddl.find("(A;;GA;;;SY)") != std::string::npos);
        /* no broad-identity ACEs — not default-open */
        CHECK(sddl.find("S-1-1-0") == std::string::npos); /*Everyone*/
        CHECK(sddl.find(";;;BU")  == std::string::npos);  /* Users   */
        CHECK(sddl.find(";;;AN")  == std::string::npos);  /* Anon    */
        CHECK(sddl.find(";;;NU")  == std::string::npos);  /* Network */
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

    std::printf("ipc_server_test: %d checks, %d failures\n",
                checks, failures);
    return failures ? 1 : 0;
}
