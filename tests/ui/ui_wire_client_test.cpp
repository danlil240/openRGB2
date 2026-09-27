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

#include <atomic>
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

/*---------------------------------------------------------*\
||| 7. Non-object / unparseable bodies                       ||
|||                                                          ||
||| .value() on a non-object json throws type_error.302 —   ||
||| before the guards this terminated the client. A non-    ||
||| object WELCOME must fail the handshake; junk bodies     ||
||| mid-session must pass through without ending it.        ||
\*---------------------------------------------------------*/
std::vector<uint8_t> RawFrame(uint16_t type, uint32_t id,
                              const std::string& body)
{
    std::vector<uint8_t> f;
    const uint32_t magic = kMagic;
    for(int i = 0; i < 4; ++i) f.push_back((uint8_t)(magic >> (8 * i)));
    f.push_back(1); f.push_back(0);                   /* version */
    f.push_back((uint8_t)(type & 0xFF));
    f.push_back((uint8_t)((type >> 8) & 0xFF));
    for(int i = 0; i < 4; ++i) f.push_back((uint8_t)(id >> (8 * i)));
    for(int i = 0; i < 4; ++i)
    {
        f.push_back((uint8_t)((uint32_t)body.size() >> (8 * i)));
    }
    f.insert(f.end(), body.begin(), body.end());
    return f;
}

void Test_NonObjectWelcome()
{
    std::printf("-- non-object WELCOME body\n");
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
    CHECK(m.type == MsgType::Hello);
    /* Parses fine, but a JSON array — not the contract's
       object. Must fail cleanly, not throw. */
    srv.SendMsg(MsgType::Welcome, 0,
                nlohmann::json::array({ 1, 2, 3 }));
    th.join();
    CHECK(end == EngineWireClient::EndReason::HandshakeFailed);
    CHECK(cap.welcomes == 0);
}

void Test_NonObjectSessionBodies()
{
    std::printf("-- non-object EVENT/reply bodies mid-session\n");
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

    /* Non-object EVENT body — the wire layer passes it to the
       handler; the bridge guards it. Neither may throw. */
    srv.SendMsg(MsgType::Event, 0,
                nlohmann::json::array({ "junk" }));
    /* Unparseable body: valid framing, garbage JSON — the
       reader produces a discarded value. */
    srv.SendRaw(RawFrame((uint16_t)MsgType::Event, 0,
                         "{not json"));
    /* A valid event afterwards proves the stream survived. */
    srv.SendMsg(MsgType::Event, 0,
                nlohmann::json::object({
                    { "kind", "scene" },
                    { "data", nlohmann::json::object({
                        { "name", "after" } }) },
                }));
    CHECK(WaitFor([&] {
        std::lock_guard<std::mutex> lock(cap.m);
        for(const nlohmann::json& e : cap.events)
        {
            if(e.is_object() &&
               e.value("kind", std::string()) == "scene")
            {
                return true;
            }
        }
        return false;
    }));
    /* The junk bodies were delivered verbatim, never
       reinterpreted or dropped silently. */
    {
        std::lock_guard<std::mutex> lock(cap.m);
        bool saw_array = false, saw_discarded = false;
        for(const nlohmann::json& e : cap.events)
        {
            if(e.is_array())     { saw_array     = true; }
            if(e.is_discarded()) { saw_discarded = true; }
        }
        CHECK(saw_array);
        CHECK(saw_discarded);
    }

    /* Non-object reply body — correlates like any reply. */
    uint32_t    rid = 0;
    std::string err;
    CHECK(client.SendRequest(MsgType::GetState,
                             nlohmann::json::object(), rid, err));
    Message m;
    CHECK(srv.RecvMsg(m, 4000));
    srv.SendMsg(MsgType::State, rid,
                nlohmann::json::array({ "not", "an", "object" }));
    CHECK(WaitFor([&] { return cap.HasReply(rid); }));
    CHECK(cap.GetReply(rid).body.is_array());

    client.Stop();
    th.join();
    CHECK(end == EngineWireClient::EndReason::Stopped);
}

/*---------------------------------------------------------*\
||| 8. Queued writes: caller never blocks on the pipe        ||
|||                                                          ||
||| SendRequest encodes + enqueues to the bounded outbound  ||
||| queue — a session writer thread does the pipe I/O. A    ||
||| wedged send holds the WRITER, never the caller; queue   ||
||| overflow refuses honestly and is counted.               ||
\*---------------------------------------------------------*/
void Test_QueuedWritesUnderWedge()
{
    std::printf("-- queued writes under a wedged channel\n");
    auto      st  = std::make_shared<PipeShared>();
    ServerPeer srv(st);
    EngineWireClient client;
    Capture    cap;
    EngineWireClient::Options opts;
    opts.keepalive_ms      = 0;
    opts.write_deadline_ms = 3000;   /* generous — we control it */
    opts.max_outbound_queue = 4;

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
    srv.SetClientSendWedge(true);    /* peer stops draining    */

    /* The first request must come back FAST — encode + queue
       only, even though the writer is now parked in SendAll. */
    uint32_t    rid = 0;
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(client.SendRequest(MsgType::GetState,
                             nlohmann::json::object(), rid, err));
    const auto enqueue_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
    CHECK(enqueue_ms < 1000);   /* bounded by encode, not I/O */

    Message m;
    CHECK(!srv.RecvMsg(m, 250));   /* nothing on the wire yet  */

    /* Fill the bounded queue past its cap — overflow refuses
       with an honest error instead of blocking or growing. */
    int accepted = 0, refused = 0;
    for(int i = 0; i < 6; ++i)
    {
        uint32_t id = 0;
        if(client.SendRequest(MsgType::Ping,
                              nlohmann::json::object(), id, err))
        {
            ++accepted;
        }
        else
        {
            ++refused;
            CHECK(err.find("queue") != std::string::npos ||
                  err.find("outbound") != std::string::npos);
        }
    }
    CHECK(refused > 0);
    CHECK(accepted <= 4);
    CHECK(client.DroppedWrites() >= 1);

    /* Unwedge: the wedged send completes, the queue drains in
       order, and every accepted request reaches the wire. */
    srv.SetClientSendWedge(false);
    int received = 0;
    for(int i = 0; i < 1 + accepted; ++i)
    {
        if(srv.RecvMsg(m, 4000))
        {
            ++received;
            srv.SendMsg(MsgType::Ok, m.request_id,
                        nlohmann::json::object());
        }
    }
    CHECK(received == 1 + accepted);
    CHECK(WaitFor([&] { return cap.HasReply(rid); }));

    client.Stop();
    th.join();
    CHECK(end == EngineWireClient::EndReason::Stopped);
}

/*---------------------------------------------------------*\
||| 9. External cancellation honored before the run          ||
|||                                                          ||
||| A stop flag set BEFORE Run() begins must end it without  ||
||| a byte on the wire — Run() clears its own stop_ for      ||
||| reuse, so the caller-owned flag is the authority.       ||
\*---------------------------------------------------------*/
void Test_KeepRunningCancel()
{
    std::printf("-- keep_running cancel before session start\n");
    auto      st  = std::make_shared<PipeShared>();
    ServerPeer srv(st);
    EngineWireClient client;
    Capture    cap;

    std::atomic<bool> keep{ false };   /* already cancelled    */
    std::string detail;
    const EngineWireClient::EndReason end =
        client.Run(
            std::unique_ptr<IIpcChannel>(new PipeEnd(st, true)),
            MakeHandlers(cap), EngineWireClient::Options(),
            detail, &keep);
    CHECK(end == EngineWireClient::EndReason::Stopped);

    Message m;
    CHECK(!srv.RecvMsg(m, 300));   /* no HELLO ever sent       */
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
    Test_NonObjectWelcome();
    Test_NonObjectSessionBodies();
    Test_QueuedWritesUnderWedge();
    Test_KeepRunningCancel();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
