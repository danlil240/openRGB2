// SPDX-License-Identifier: GPL-2.0-or-later
//
// Wire-codec tests for the engine IPC protocol
// (src/ipc/IpcProtocol.h): framing round-trips, resync
// after framing violations, the oversized-body bound, the
// resync byte bound, and error encoding. Exercises the
// real MessageReader over an injected in-memory channel —
// no threads, no pipes.

#include "IpcProtocol.h"

#include "fake_ipc_channel.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace openrgb2::ipc;
using namespace ipc_test;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

int main()
{
    /*---- round-trip --------------------------------------*/
    {
        const std::vector<uint8_t> raw =
            EncodeMessage(MsgType::Ping, 42,
                          nlohmann::json::object({{"a", 1}}));
        auto st = std::make_shared<FakePipeState>();
        st->inbound.assign(raw.begin(), raw.end());
        FakeIpcChannel  ch(st);
        MessageReader   rd(ch);
        Message         msg;
        std::string     detail;
        CHECK(rd.Read(msg, 2000, detail) == ReadResult::Ok);
        CHECK(msg.version == kProtocolVersion);
        CHECK(msg.type == MsgType::Ping);
        CHECK(msg.request_id == 42);
        CHECK(msg.Body()["a"].get<int>() == 1);
        CHECK(rd.ResyncBytes() == 0);
    }

    /*---- garbage prefix resync ----------------------------*/
    {
        std::vector<uint8_t> stream = { 'x', 'x', 'x', 'G', 'A' };
        const std::vector<uint8_t> good =
            EncodeMessage(MsgType::GetState, 7,
                          nlohmann::json::object());
        stream.insert(stream.end(), good.begin(), good.end());

        auto st = std::make_shared<FakePipeState>();
        st->inbound.assign(stream.begin(), stream.end());
        FakeIpcChannel ch(st);
        MessageReader  rd(ch);
        Message        msg;
        std::string    detail;
        CHECK(rd.Read(msg, 2000, detail) == ReadResult::Ok);
        CHECK(msg.type == MsgType::GetState);
        CHECK(msg.request_id == 7);
        CHECK(rd.ResyncBytes() == 5);   /* five garbage bytes skipped */
    }

    /*---- magic inside garbage + back-to-back messages -----*/
    {
        /* Garbage that ends with three bytes of the magic so
           the real magic spans the sliding window. */
        std::vector<uint8_t> stream = { 0x11, 0x22, 'O', '2', 'I' };
        const std::vector<uint8_t> m1 =
            EncodeMessage(MsgType::Ping, 1, nlohmann::json::object());
        const std::vector<uint8_t> m2 =
            EncodeMessage(MsgType::Ping, 2, nlohmann::json::object());
        stream.insert(stream.end(), m1.begin(), m1.end());
        stream.insert(stream.end(), m2.begin(), m2.end());

        auto st = std::make_shared<FakePipeState>();
        st->inbound.assign(stream.begin(), stream.end());
        FakeIpcChannel ch(st);
        MessageReader  rd(ch);
        Message        msg;
        std::string    detail;
        CHECK(rd.Read(msg, 2000, detail) == ReadResult::Ok);
        CHECK(msg.request_id == 1);
        CHECK(rd.Read(msg, 2000, detail) == ReadResult::Ok);
        CHECK(msg.request_id == 2);
    }

    /*---- oversized body bound -----------------------------*/
    {
        /* Header claiming a >cap body, then a valid message.
           The reader must report TooLarge and recover. */
        std::vector<uint8_t> stream;
        {
            /* hand-build: magic + version + type + id + len */
            auto put32 = [&](uint32_t v)
            { for(int i = 0; i < 4; ++i)
                  stream.push_back((uint8_t)(v >> (8 * i))); };
            auto put16 = [&](uint16_t v)
            { for(int i = 0; i < 2; ++i)
                  stream.push_back((uint8_t)(v >> (8 * i))); };
            put32(kMagic);
            put16(kProtocolVersion);
            put16((uint16_t)MsgType::SetScene);
            put32(9);
            put32(kMaxBodyBytes + 1);
        }
        /* The declared body: it SMUGGLES a well-formed message
           (request id 666) — skipping by length must never
           parse it. */
        const std::vector<uint8_t> smuggled =
            EncodeMessage(MsgType::Ping, 666, nlohmann::json::object());
        stream.insert(stream.end(), 64, 'Z');
        stream.insert(stream.end(), smuggled.begin(), smuggled.end());
        stream.insert(stream.end(),
                      (size_t)kMaxBodyBytes + 1 - 64 - smuggled.size(),
                      'Z');
        const std::vector<uint8_t> good =
            EncodeMessage(MsgType::Ping, 3, nlohmann::json::object());
        stream.insert(stream.end(), good.begin(), good.end());

        auto st = std::make_shared<FakePipeState>();
        st->inbound.assign(stream.begin(), stream.end());
        FakeIpcChannel ch(st);
        MessageReader  rd(ch);
        Message        msg;
        std::string    detail;
        CHECK(rd.Read(msg, 2000, detail) == ReadResult::TooLarge);
        CHECK(msg.request_id == 9);
        /* The oversized body is skipped by its declared length
           — the smuggled frame inside it is never parsed; the
           next real message is read. */
        CHECK(rd.Read(msg, 2000, detail) == ReadResult::Ok);
        CHECK(msg.type == MsgType::Ping);
        CHECK(msg.request_id == 3);
        CHECK(!rd.Poisoned());
        CHECK(rd.ResyncBytes() == 0);
    }

    /*---- oversized body beyond the skip bound: close ------*/
    {
        std::vector<uint8_t> stream;
        auto put32 = [&](uint32_t v)
        { for(int i = 0; i < 4; ++i)
              stream.push_back((uint8_t)(v >> (8 * i))); };
        auto put16 = [&](uint16_t v)
        { for(int i = 0; i < 2; ++i)
              stream.push_back((uint8_t)(v >> (8 * i))); };
        put32(kMagic);
        put16(kProtocolVersion);
        put16((uint16_t)MsgType::SetScene);
        put32(10);
        put32(kMaxSkipBytes + 1);
        auto st = std::make_shared<FakePipeState>();
        st->inbound.assign(stream.begin(), stream.end());
        FakeIpcChannel ch(st);
        MessageReader  rd(ch);
        Message        msg;
        std::string    detail;
        CHECK(rd.Read(msg, 2000, detail) == ReadResult::TooLarge);
        CHECK(msg.request_id == 10);
        CHECK(rd.Poisoned());
        CHECK(rd.Read(msg, 50, detail) == ReadResult::ResyncExceeded);
    }

    /*---- a timeout keeps partial progress -----------------*/
    {
        /* The polling-client case: a message arrives in pieces
           across several short-deadline Reads. Nothing may be
           lost between them. */
        const std::vector<uint8_t> good =
            EncodeMessage(MsgType::Ping, 77, nlohmann::json::object(
                {{"payload", std::string(300, 'p')}}));
        auto st = std::make_shared<FakePipeState>();
        st->inbound.assign(good.begin(), good.begin() + 2);
        FakeIpcChannel ch(st);
        MessageReader  rd(ch);
        Message        msg;
        std::string    detail;
        CHECK(rd.Read(msg, 20, detail) == ReadResult::Timeout);
        {
            std::lock_guard<std::mutex> lock(st->m);
            st->inbound.insert(st->inbound.end(), good.begin() + 2,
                               good.begin() + 30);
        }
        CHECK(rd.Read(msg, 20, detail) == ReadResult::Timeout);
        {
            std::lock_guard<std::mutex> lock(st->m);
            st->inbound.insert(st->inbound.end(), good.begin() + 30,
                               good.end());
        }
        CHECK(rd.Read(msg, 2000, detail) == ReadResult::Ok);
        CHECK(msg.request_id == 77);
        CHECK(msg.Body()["payload"].get<std::string>().size() == 300);
    }

    /*---- idle bound: the message deadline starts at the ---*\
    |     first byte                                          |
    \*-------------------------------------------------------*/
    {
        const std::vector<uint8_t> good =
            EncodeMessage(MsgType::Ping, 88, nlohmann::json::object());
        auto st = std::make_shared<FakePipeState>();
        FakeIpcChannel ch(st);
        MessageReader  rd(ch);
        Message        msg;
        std::string    detail;
        /* Nothing arrives: the idle bound (not the 30 ms message
           deadline) decides — still a Timeout, nothing lost. */
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(rd.Read(msg, 30, detail, 200) == ReadResult::Timeout);
        CHECK(std::chrono::steady_clock::now() - t0 >=
              std::chrono::milliseconds(150));
        /* A request arriving after an idle gap longer than the
           message deadline is read normally. */
        std::thread late([&]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            std::lock_guard<std::mutex> lock(st->m);
            st->inbound.insert(st->inbound.end(), good.begin(),
                               good.end());
            st->cv_in.notify_all();
        });
        CHECK(rd.Read(msg, 30, detail, 5000) == ReadResult::Ok);
        late.join();
        CHECK(msg.request_id == 88);
    }

    /*---- JSON nesting cap ---------------------------------*/
    {
        Message ok;
        ok.body_raw = std::string(10, '[') + std::string(10, ']');
        CHECK(!ok.Body().is_discarded());
        Message edge;
        edge.body_raw = std::string(kMaxJsonDepth, '[') +
                        std::string(kMaxJsonDepth, ']');
        CHECK(!edge.Body().is_discarded());
        Message deep;
        deep.body_raw = std::string(kMaxJsonDepth + 1, '[') +
                        std::string(kMaxJsonDepth + 1, ']');
        CHECK(deep.Body().is_discarded());
        /* The attack shape: a full-size body of nothing but
           nesting. Must be rejected without recursing (no stack
           overflow), objects included. */
        Message bomb;
        bomb.body_raw = std::string(kMaxBodyBytes / 2, '[') +
                        std::string(kMaxBodyBytes / 2, ']');
        CHECK(bomb.Body().is_discarded());
        std::string obj;
        for(int i = 0; i < 100000; ++i) { obj += "{\"a\":"; }
        obj += "1";
        obj += std::string(100000, '}');
        Message objbomb;
        objbomb.body_raw = obj;
        CHECK(objbomb.Body().is_discarded());

        /* FieldOrNull: reference lookup, no throw on non-objects. */
        const nlohmann::json o = nlohmann::json::object({{"k", 5}});
        CHECK(FieldOrNull(o, "k") == 5);
        CHECK(FieldOrNull(o, "missing").is_null());
        CHECK(FieldOrNull(nlohmann::json::array(), "k").is_null());
    }

    /*---- truncated message --------------------------------*/
    {
        const std::vector<uint8_t> good =
            EncodeMessage(MsgType::Ping, 5, nlohmann::json::object(
                {{"payload", "0123456789"}}));
        const std::vector<uint8_t> part(good.begin(),
                                        good.begin() + 10);
        auto st = std::make_shared<FakePipeState>();
        st->inbound.assign(part.begin(), part.end());
        FakeIpcChannel ch(st);
        MessageReader  rd(ch);
        Message        msg;
        std::string    detail;
        /* Nothing more ever arrives — the read deadline fires. */
        CHECK(rd.Read(msg, 50, detail) == ReadResult::Timeout);
    }

    /*---- resync byte bound --------------------------------*/
    {
        auto st = std::make_shared<FakePipeState>();
        /* > kMaxResyncBytes of pure garbage, no magic inside. */
        st->inbound.assign(kMaxResyncBytes + 8, 0x77);
        FakeIpcChannel ch(st);
        MessageReader  rd(ch);
        Message        msg;
        std::string    detail;
        CHECK(rd.Read(msg, 30000, detail) ==
              ReadResult::ResyncExceeded);
        CHECK(rd.ResyncBytes() >= kMaxResyncBytes);
    }

    /*---- closed channel ------------------------------------*/
    {
        auto st = std::make_shared<FakePipeState>();
        st->inbound_closed = true;
        FakeIpcChannel ch(st);
        MessageReader  rd(ch);
        Message        msg;
        std::string    detail;
        CHECK(rd.Read(msg, 2000, detail) == ReadResult::Closed);
    }

    /*---- empty body / encode bounds ------------------------*/
    {
        const std::vector<uint8_t> raw =
            EncodeMessage(MsgType::GetSnapshots, 0,
                          nlohmann::json::object());
        auto st = std::make_shared<FakePipeState>();
        st->inbound.assign(raw.begin(), raw.end());
        FakeIpcChannel ch(st);
        MessageReader  rd(ch);
        Message        msg;
        std::string    detail;
        CHECK(rd.Read(msg, 2000, detail) == ReadResult::Ok);
        CHECK(msg.Body().is_object());

        /* An encoder-side bound too: bodies over the cap must
           never produce a frame. */
        nlohmann::json huge = nlohmann::json::object(
            {{"blob", std::string(kMaxBodyBytes + 4, 'x')}});
        CHECK(EncodeMessage(MsgType::SetScene, 0, huge).empty());
    }

    /*---- error encoding ------------------------------------*/
    {
        const std::vector<uint8_t> raw =
            EncodeError(11, ErrCode::ReadOnly, "nope");
        auto st = std::make_shared<FakePipeState>();
        st->inbound.assign(raw.begin(), raw.end());
        FakeIpcChannel ch(st);
        MessageReader  rd(ch);
        Message        msg;
        std::string    detail;
        CHECK(rd.Read(msg, 2000, detail) == ReadResult::Ok);
        CHECK(msg.type == MsgType::Error);
        const nlohmann::json b = msg.Body();
        CHECK(b["code"].get<uint32_t>() ==
              (uint32_t)ErrCode::ReadOnly);
        CHECK(b["error"].get<std::string>() == "read_only");
        CHECK(msg.request_id == 11);
    }

    std::printf("ipc_protocol_test: %d checks, %d failures\n",
                checks, failures);
    return failures ? 1 : 0;
}
