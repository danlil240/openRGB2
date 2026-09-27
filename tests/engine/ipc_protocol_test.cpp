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
        stream.insert(stream.end(), 64, 'Z');   /* filler     */
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
        /* The oversized "body" is skipped by resync — the next
           message is still read. */
        CHECK(rd.Read(msg, 2000, detail) == ReadResult::Ok);
        CHECK(msg.type == MsgType::Ping);
        CHECK(msg.request_id == 3);
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
