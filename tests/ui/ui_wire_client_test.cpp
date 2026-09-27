// SPDX-License-Identifier: GPL-2.0-or-later
//
// Wire-level tests for the UI IPC client (src/ui/
// EngineWireClient) over the scripted in-process pipe
// (test_pipe.h — the test plays the server). Mirrors the
// server-side suite's coverage from the client's seat:
//
//   - HELLO shape + WELCOME accept, SUBSCRIBE all
//   - request/reply correlation with interleaved EVENT pushes
//   - byte-at-a-time fragmentation
//   - ERROR replies surfaced (never hidden)
//   - garbage resync + oversized-body recovery
//   - wrong-version messages skipped honestly
//   - version_mismatch on HELLO -> HandshakeFailed
//   - peer close mid-session, Stop() -> Stopped
//   - encode bound + request-when-offline rejection
//
// CHECK-macro main like the engine suites — no framework.

#include "EngineWireClient.h"

#include "test_pipe.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace openrgb2;
using namespace openrgb2::ipc;
using namespace openrgb2::ui;
using namespace ui_test;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

namespace
{

struct Capture
{
    std::mutex m;
    int                       welcomes = 0;
    nlohmann::json            welcome;
    std::vector<nlohmann::json> events;
    struct Reply { uint32_t id; MsgType type; nlohmann::json body; };
    std::vector<Reply>        replies;
    std::vector<std::string>  proto_errors;

    int EventCount()
    {
        std::lock_guard<std::mutex> lock(m);
        return (int)events.size();
    }
    bool HasReply(uint32_t id)
    {
        std::lock_guard<std::mutex> lock(m);
        for(const Reply& r : replies)
        {
            if(r.id == id)
            {
                return true;
            }
        }
        return false;
    }
    Reply GetReply(uint32_t id)
    {
        std::lock_guard<std::mutex> lock(m);
        for(const Reply& r : replies)
        {
            if(r.id == id)
            {
                return r;
            }
        }
        return { 0, MsgType::Error, nlohmann::json() };
    }
};

EngineWireClient::Handlers MakeHandlers(Capture& cap)
{
    EngineWireClient::Handlers h;
    h.onWelcome = [&cap](const nlohmann::json& w)
    {
        std::lock_guard<std::mutex> lock(cap.m);
        cap.welcome = w;
        ++cap.welcomes;
    };
    h.onEvent = [&cap](const nlohmann::json& e)
    {
        std::lock_guard<std::mutex> lock(cap.m);
        cap.events.push_back(e);
    };
    h.onReply = [&cap](uint32_t id, MsgType t,
                       const nlohmann::json& b)
    {
        std::lock_guard<std::mutex> lock(cap.m);
        cap.replies.push_back({ id, t, b });
    };
    h.onProtocolError = [&cap](const std::string& s)
    {
        std::lock_guard<std::mutex> lock(cap.m);
        cap.proto_errors.push_back(s);
    };
    return h;
}

/* Handshake helper shared by the positive tests: drives the
   server half of HELLO/SUBSCRIBE and leaves the session live. */
bool DriveHandshake(ServerPeer& srv, Capture& cap,
                    uint32_t& sub_id, uint32_t timeout = 4000)
{
    Message m;
    if(!srv.RecvMsg(m, timeout))
    {
        std::printf("  no HELLO received\n");
        return false;
    }
    if(m.type != MsgType::Hello || m.request_id != 0)
    {
        std::printf("  first message was not HELLO\n");
        return false;
    }
    srv.SendMsg(MsgType::Welcome, 0,
                nlohmann::json::object({
                    { "server_version",     1 },
                    { "negotiated_version", 1 },
                    { "channel",            "command" },
                    { "max_body_bytes",     1048576 },
                    { "engine",             "openrgb2-engine" },
                    { "running",            true },
                    { "state", nlohmann::json::object({
                        { "engine", nlohmann::json::object({
                            { "running", true } }) },
                    }) },
                }));
    if(!WaitFor([&] { return cap.welcomes == 1; }, 3000))
    {
        std::printf("  onWelcome never fired\n");
        return false;
    }
    /* Client must SUBSCRIBE {events:"all"} next. */
    if(!srv.RecvMsg(m, timeout))
    {
        std::printf("  no SUBSCRIBE received\n");
        return false;
    }
    if(m.type != MsgType::Subscribe ||
       m.Body().value("events", std::string()) != "all")
    {
        std::printf("  expected SUBSCRIBE all\n");
        return false;
    }
    sub_id = m.request_id;
    srv.SendMsg(MsgType::Ok, sub_id,
                nlohmann::json::object({
                    { "subscribed", nlohmann::json::array({
                        "topology", "scene", "playback",
                        "connection", "frame_status" }) },
                }));
    return true;
}

/*---------------------------------------------------------*\
||| 1. Handshake + correlated replies + pushes               ||
\*---------------------------------------------------------*/
void Test_HandshakeAndReplies()
{
    std::printf("-- handshake + request/reply + events\n");
    auto      st   = std::make_shared<PipeShared>();
    ServerPeer srv(st);
    EngineWireClient client;
    Capture    cap;
    EngineWireClient::Options opts;
    opts.keepalive_ms = 0;

    EngineWireClient::EndReason end =
        EngineWireClient::EndReason::Stopped;
    std::string detail;
    std::thread th([&] {
        end = client.Run(
            std::unique_ptr<IIpcChannel>(new PipeEnd(st, true)),
            MakeHandlers(cap), opts, detail);
    });

    /* Assert the HELLO wire shape before answering. */
    Message m;
    CHECK(srv.RecvMsg(m, 4000));
    CHECK(m.type == MsgType::Hello);
    CHECK(m.request_id == 0);
    CHECK(m.version == kProtocolVersion);
    const nlohmann::json hb = m.Body();
    CHECK(hb.value("client_name", std::string()) == "openrgb2-ui");
    CHECK(hb.value("min_version", 0) == 1);
    CHECK(hb.value("max_version", 0) == 1);

    srv.SendMsg(MsgType::Welcome, 0,
                nlohmann::json::object({
                    { "server_version",     1 },
                    { "negotiated_version", 1 },
                    { "channel",            "command" },
                    { "engine",             "openrgb2-engine" },
                    { "running",            true },
                    { "state", nlohmann::json::object() },
                }));
    CHECK(WaitFor([&] { return cap.welcomes == 1; }));
    CHECK(cap.welcome.value("engine", std::string())
          == "openrgb2-engine");

    CHECK(srv.RecvMsg(m, 4000));
    CHECK(m.type == MsgType::Subscribe);
    const uint32_t sub_id = m.request_id;
    srv.SendMsg(MsgType::Ok, sub_id,
                nlohmann::json::object({
                    { "subscribed", nlohmann::json::array({
                        "topology" }) },
                }));
    srv.SendMsg(MsgType::Event, 0,
                nlohmann::json::object({
                    { "kind", "topology" },
                    { "data", nlohmann::json::object({
                        { "snapshots", nlohmann::json::array() } }) },
                }));
    CHECK(WaitFor([&] { return cap.EventCount() == 1 &&
                                 cap.HasReply(sub_id); }));

    /* Request/reply correlation with an EVENT pushed in
       between: the reply must still pair on request_id. */
    uint32_t    rid = 0;
    std::string err;
    CHECK(client.SendRequest(MsgType::GetState,
                             nlohmann::json::object(), rid, err));
    CHECK(srv.RecvMsg(m, 4000));
    CHECK(m.type == MsgType::GetState);
    CHECK(m.request_id == rid);
    srv.SendMsg(MsgType::Event, 0,
                nlohmann::json::object({
                    { "kind", "scene" },
                    { "data", nlohmann::json::object({
                        { "loaded", true }, { "name", "demo" } }) },
                }));
    srv.SendMsg(MsgType::State, rid,
                nlohmann::json::object({
                    { "engine", nlohmann::json::object({
                        { "running", true } }) },
                }));
    CHECK(WaitFor([&] { return cap.HasReply(rid); }));
    const Capture::Reply rep = cap.GetReply(rid);
    CHECK(rep.type == MsgType::State);
    CHECK(rep.body["engine"].value("running", false));
    CHECK(WaitFor([&] { return cap.EventCount() == 2; }));

    /* ERROR reply surfaces verbatim — never swallowed. */
    uint32_t eid = 0;
    CHECK(client.SendRequest(MsgType::GetDeliveryStats,
                             nlohmann::json::object(), eid, err));
    CHECK(srv.RecvMsg(m, 4000));
    srv.SendMsg(MsgType::Error, eid,
                nlohmann::json::object({
                    { "code",    (uint32_t)ErrCode::Unavailable },
                    { "error",   "unavailable" },
                    { "message", "no delivery pipeline" },
                }));
    CHECK(WaitFor([&] { return cap.HasReply(eid); }));
    const Capture::Reply erep = cap.GetReply(eid);
    CHECK(erep.type == MsgType::Error);
    CHECK(erep.body.value("code", 0) == (int)ErrCode::Unavailable);
    CHECK(erep.body.value("error", std::string()) == "unavailable");

    /* Encode bound: a body over 1 MiB is refused locally —
       nothing may reach the wire. */
    nlohmann::json huge = nlohmann::json::object();
    huge["blob"] = std::string(kMaxBodyBytes + 1024, 'x');
    uint32_t hid = 0;
    CHECK(!client.SendRequest(MsgType::SetScene, huge, hid, err));
    CHECK(err.find("bound") != std::string::npos ||
          err.find("encode") != std::string::npos);
    CHECK(!srv.RecvMsg(m, 250));   /* nothing leaked */

    client.Stop();
    th.join();
    CHECK(end == EngineWireClient::EndReason::Stopped);
}

/*---------------------------------------------------------*\
||| 2. Fragmented delivery                                   ||
\*---------------------------------------------------------*/
void Test_Fragmentation()
{
    std::printf("-- fragmentation\n");
    auto      st  = std::make_shared<PipeShared>();
    ServerPeer srv(st);
    EngineWireClient client;
    Capture    cap;
    EngineWireClient::Options opts;
    opts.keepalive_ms = 0;

    EngineWireClient::EndReason end =
        EngineWireClient::EndReason::Stopped;
    std::string detail;
    std::thread th([&] {
        end = client.Run(
            std::unique_ptr<IIpcChannel>(new PipeEnd(st, true)),
            MakeHandlers(cap), opts, detail);
    });

    Message m;
    CHECK(srv.RecvMsg(m, 4000));
    /* WELCOME + SUBSCRIBE-OK + initial EVENT, all delivered
       one byte at a time. */
    srv.SendFragmented(
        EncodeMessage(MsgType::Welcome, 0,
                      nlohmann::json::object({
                          { "server_version",     1 },
                          { "negotiated_version", 1 },
                          { "channel",            "command" },
                          { "running",            true },
                          { "state", nlohmann::json::object() },
                      })));
    CHECK(WaitFor([&] { return cap.welcomes == 1; }));
    CHECK(srv.RecvMsg(m, 4000));
    CHECK(m.type == MsgType::Subscribe);
    srv.SendFragmented(
        EncodeMessage(MsgType::Ok, m.request_id,
                      nlohmann::json::object({
                          { "subscribed",
                            nlohmann::json::array({ "scene" }) },
                      })));
    srv.SendFragmented(
        EncodeMessage(MsgType::Event, 0,
                      nlohmann::json::object({
                          { "kind", "scene" },
                          { "data", nlohmann::json::object({
                              { "name", "frag" } }) },
                      })));
    CHECK(WaitFor([&] { return cap.EventCount() == 1; }));
    client.Stop();
    th.join();
    CHECK(end == EngineWireClient::EndReason::Stopped);
}

/*---------------------------------------------------------*\
||| 3. Version mismatch on HELLO                             ||
\*---------------------------------------------------------*/
void Test_HelloVersionMismatch()
{
    std::printf("-- HELLO version_mismatch\n");
    auto      st  = std::make_shared<PipeShared>();
    ServerPeer srv(st);
    EngineWireClient client;
    Capture    cap;

    EngineWireClient::EndReason end =
        EngineWireClient::EndReason::Stopped;
    std::string detail;
    std::thread th([&] {
        end = client.Run(
            std::unique_ptr<IIpcChannel>(new PipeEnd(st, true)),
            MakeHandlers(cap),
            EngineWireClient::Options(), detail);
    });

    Message m;
    CHECK(srv.RecvMsg(m, 4000));
    srv.SendMsg(MsgType::Error, 0,
                nlohmann::json::object({
                    { "code",    (uint32_t)ErrCode::VersionMismatch },
                    { "error",   "version_mismatch" },
                    { "message", "no common protocol version" },
                }));
    srv.Close();
    th.join();
    CHECK(end == EngineWireClient::EndReason::HandshakeFailed);
    CHECK(detail.find("version_mismatch") != std::string::npos);
    CHECK(cap.welcomes == 0);
}

/*---------------------------------------------------------*\
||| 4. Garbage resync + oversized body                        ||
\*---------------------------------------------------------*/
void Test_ResyncAndTooLarge()
{
    std::printf("-- garbage resync + too_large\n");
    auto      st  = std::make_shared<PipeShared>();
    ServerPeer srv(st);
    EngineWireClient client;
    Capture    cap;
    EngineWireClient::Options opts;
    opts.keepalive_ms = 0;

    EngineWireClient::EndReason end =
        EngineWireClient::EndReason::Stopped;
    std::string detail;
    std::thread th([&] {
        end = client.Run(
            std::unique_ptr<IIpcChannel>(new PipeEnd(st, true)),
            MakeHandlers(cap), opts, detail);
    });

    uint32_t sub_id = 0;
    CHECK(DriveHandshake(srv, cap, sub_id));

    /* Garbage burst, then a valid EVENT — resync must recover
       the frame (bounded scan, counted in resync_bytes). */
    const std::string junk = "GARBAGE-GARBAGE-GARBAGE";
    srv.SendRaw(std::vector<uint8_t>(junk.begin(), junk.end()));
    srv.SendMsg(MsgType::Event, 0,
                nlohmann::json::object({
                    { "kind", "playback" },
                    { "data", nlohmann::json::object({
                        { "pending", false } }) },
                }));
    CHECK(WaitFor([&] { return cap.EventCount() == 1; }));

    /* Crafted oversized header: valid framing, body_len over
       the 1 MiB cap — reader reports TooLarge and resyncs
       onto the NEXT real message. */
    {
        std::vector<uint8_t> hdr;
        const uint32_t magic = kMagic;
        const uint32_t blen  = kMaxBodyBytes + 1;
        for(int i = 0; i < 4; ++i) hdr.push_back((uint8_t)(magic >> (8 * i)));
        hdr.push_back(1); hdr.push_back(0);          /* version */
        hdr.push_back(0x00); hdr.push_back(0x81);    /* type    */
        for(int i = 0; i < 4; ++i) hdr.push_back(0); /* req id  */
        for(int i = 0; i < 4; ++i) hdr.push_back((uint8_t)(blen >> (8 * i)));
        srv.SendRaw(hdr);
    }
    srv.SendMsg(MsgType::Event, 0,
                nlohmann::json::object({
                    { "kind", "connection" },
                    { "data", nlohmann::json::object({
                        { "state", "connected" } }) },
                }));
    CHECK(WaitFor([&] { return cap.EventCount() == 2; }));
    CHECK(WaitFor([&] {
        std::lock_guard<std::mutex> lock(cap.m);
        return !cap.proto_errors.empty();
    }));

    /* Wrong negotiated version is skipped honestly; the
       following valid message still lands. */
    srv.SendMsg(MsgType::Event, 0,
                nlohmann::json::object({
                    { "kind", "scene" },
                    { "data", nlohmann::json::object({
                        { "name", "v2" } }) },
                }),
                2 /* bogus version */);
    srv.SendMsg(MsgType::Event, 0,
                nlohmann::json::object({
                    { "kind", "frame_status" },
                    { "data", nlohmann::json::object({
                        { "aggregate",
                          nlohmann::json::object() } }) },
                }));
    CHECK(WaitFor([&] { return cap.EventCount() == 3; }));
    {
        std::lock_guard<std::mutex> lock(cap.m);
        CHECK(cap.events[2].value("kind", std::string())
              == "frame_status");
        CHECK(cap.proto_errors.size() >= 2);
    }

    client.Stop();
    th.join();
    CHECK(end == EngineWireClient::EndReason::Stopped);
}

/*---------------------------------------------------------*\
||| 5. Peer close mid-session                                ||
\*---------------------------------------------------------*/
void Test_PeerClose()
{
    std::printf("-- peer close mid-session\n");
    auto      st  = std::make_shared<PipeShared>();
    ServerPeer srv(st);
    EngineWireClient client;
    Capture    cap;
    EngineWireClient::Options opts;
    opts.keepalive_ms = 0;

    EngineWireClient::EndReason end =
        EngineWireClient::EndReason::Stopped;
    std::string detail;
    std::thread th([&] {
        end = client.Run(
            std::unique_ptr<IIpcChannel>(new PipeEnd(st, true)),
            MakeHandlers(cap), opts, detail);
    });

    uint32_t sub_id = 0;
    CHECK(DriveHandshake(srv, cap, sub_id));
    srv.Close();                    /* engine vanished         */
    th.join();
    CHECK(end == EngineWireClient::EndReason::PeerClosed);
    /* Requests after a dead session refuse locally. */
    uint32_t    rid = 99;
    std::string err;
    CHECK(!client.SendRequest(MsgType::Ping,
                              nlohmann::json::object(), rid, err));
}

/*---------------------------------------------------------*\
||| 6. Request when never connected                          ||
\*---------------------------------------------------------*/
void Test_OfflineRequest()
{
    std::printf("-- request while offline\n");
    EngineWireClient client;
    uint32_t    rid = 0;
    std::string err;
    CHECK(!client.SendRequest(MsgType::Ping,
                              nlohmann::json::object(), rid, err));
    CHECK(err.find("not connected") != std::string::npos);
    client.Stop();   /* safe with no session */
}

} /* anonymous namespace */

int main()
{
    Test_HandshakeAndReplies();
    Test_Fragmentation();
    Test_HelloVersionMismatch();
    Test_ResyncAndTooLarge();
    Test_PeerClose();
    Test_OfflineRequest();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
