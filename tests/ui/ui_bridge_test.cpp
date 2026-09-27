// SPDX-License-Identifier: GPL-2.0-or-later
//
// EngineBridge tests (src/ui/EngineBridge) — the QObject layer
// QML binds to: injected channel factory instead of real named
// pipes, so connect/drop/reconnect/backoff run deterministically
// inside a QCoreApplication event loop.
//
//   - factory-driven connect -> HELLO/WELCOME -> stores paint
//   - SUBSCRIBE initial EVENTs land in the store properties
//   - pull replies update stores + replyReceived correlator
//   - engine drop -> automatic reconnect on a new channel
//   - unreachable backoff is bounded and interruptible
//   - stop() -> disconnected, no further attempts

#include "EngineBridge.h"

#include "test_pipe.h"

#include <QCoreApplication>
#include <QThread>
#include <QVariantMap>

#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

using namespace openrgb2;
using namespace openrgb2::ipc;
using namespace ui_test;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

/* Pump the event loop until cond() — bridge marshals store
   updates onto this thread's event queue. */
static bool PumpUntil(std::function<bool()> cond, int ms = 6000)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(ms);
    while(std::chrono::steady_clock::now() < deadline)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        if(cond())
        {
            return true;
        }
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return cond();
}

/* Script one server half of a handshake + subscribe on `peer`:
   replies WELCOME (with a STATE body carrying scene/sdk/
   playback) and answers the SUBSCRIBE with OK + an initial
   topology EVENT. */
static bool DriveServerHandshake(ServerPeer& srv)
{
    Message m;
    if(!srv.RecvMsg(m, 4000) || m.type != MsgType::Hello)
    {
        return false;
    }
    srv.SendMsg(MsgType::Welcome, 0,
        nlohmann::json::object({
            { "server_version",     1 },
            { "negotiated_version", 1 },
            { "channel",            "command" },
            { "engine",             "openrgb2-engine" },
            { "running",            true },
            { "state", nlohmann::json::object({
                { "engine", nlohmann::json::object({
                    { "running", true },
                    { "scene_clock_hz", 60 } }) },
                { "scene", nlohmann::json::object({
                    { "loaded", true },
                    { "revision", 3 },
                    { "name", "daniel-desk" },
                    { "playing", true } }) },
                { "sdk", nlohmann::json::object({
                    { "state", "connected" },
                    { "reconnect_attempt", 0 } }) },
                { "playback", nlohmann::json::object({
                    { "requested_mode", "live_effects" },
                    { "pending", false },
                    { "status", nlohmann::json::object({
                        { "control", "playing" } }) } }) },
            }) },
        }));
    if(!srv.RecvMsg(m, 4000) || m.type != MsgType::Subscribe)
    {
        return false;
    }
    srv.SendMsg(MsgType::Ok, m.request_id,
        nlohmann::json::object({
            { "subscribed", nlohmann::json::array({
                "topology", "scene", "playback",
                "connection", "frame_status" }) },
        }));
    srv.SendMsg(MsgType::Event, 0,
        nlohmann::json::object({
            { "kind", "topology" },
            { "data", nlohmann::json::object({
                { "snapshots", nlohmann::json::array({
                    nlohmann::json::object({
                        { "key", nlohmann::json::object({
                            { "device",   "Lian Li SL Wireless" },
                            { "serial",   "s1" },
                            { "location", "usb" },
                            { "zone",     "" } }) },
                        { "name",       "SL Wireless" },
                        { "connection", "connected" },
                        { "led_count",  120 } }),
                }) },
                { "connection", nlohmann::json::object({
                    { "state", "connected" } }) },
            }) },
        }));
    return true;
}

/* After WELCOME the bridge auto-pulls the stores pushes don't
   cover (getPresets/getDeliveryStats/getState — ids 2..4).
   Drain + answer them so scripted request traffic lines up. */
static bool AnswerAutoPulls(ServerPeer& srv)
{
    static const MsgType want[3] = {
        MsgType::GetPresets, MsgType::GetDeliveryStats,
        MsgType::GetState };
    static const MsgType reply[3] = {
        MsgType::Presets, MsgType::DeliveryStats, MsgType::State };
    for(int i = 0; i < 3; ++i)
    {
        Message m;
        if(!srv.RecvMsg(m, 4000) || m.type != want[i])
        {
            std::printf("  auto-pull %d: got type 0x%04x\n", i,
                        (unsigned)m.type);
            return false;
        }
        srv.SendMsg(reply[i], m.request_id,
                    nlohmann::json::object());
    }
    return true;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    /*---- connected session + stores -------------------------*/
    std::printf("-- bridge connect, stores, replies, reconnect\n");
    {
        ui::EngineBridge bridge;
        std::mutex       pm;
        std::vector<std::shared_ptr<TestPipe>> pipes;
        int              attempts = 0;

        bridge.setChannelFactoryForTesting(
            [&](std::string& err)
                -> std::unique_ptr<IIpcChannel>
            {
                (void)err;
                auto p = std::make_shared<TestPipe>();
                {
                    std::lock_guard<std::mutex> lock(pm);
                    pipes.push_back(p);
                }
                ++attempts;
                return p->ClientChannel();
            });
        ui::EngineWireClient::Options wopts;
        wopts.keepalive_ms = 0;
        bridge.setWireOptionsForTesting(wopts);
        bridge.setBackoffForTesting({ 20, 40, 80 });
        bridge.start();

        /* Attempt 1: drive the handshake. */
        CHECK(PumpUntil([&] {
            std::lock_guard<std::mutex> lock(pm);
            return pipes.size() >= 1;
        }));
        std::shared_ptr<TestPipe> p0;
        {
            std::lock_guard<std::mutex> lock(pm);
            p0 = pipes[0];
        }
        ServerPeer srv = p0->Peer();
        CHECK(DriveServerHandshake(srv));
        CHECK(PumpUntil([&] {
            return bridge.connectionState() == "connected";
        }));
        CHECK(PumpUntil([&] { return bridge.snapshots().size() == 1; }));
        CHECK(bridge.scene().value("name").toString()
              == "daniel-desk");
        CHECK(bridge.connection().value("state").toString()
              == "connected");
        CHECK(!bridge.degraded());
        CHECK(AnswerAutoPulls(srv));

        /* Pull reply -> store + correlator. */
        const int pid = bridge.getPresets();
        CHECK(pid > 0);
        Message m;
        CHECK(srv.RecvMsg(m, 4000));
        CHECK(m.type == MsgType::GetPresets);
        CHECK(m.request_id == (uint32_t)pid);
        srv.SendMsg(MsgType::Presets, (uint32_t)pid,
            nlohmann::json::object({
                { "presets", nlohmann::json::array({
                    nlohmann::json::object({
                        { "id",   "rainbow_wave" },
                        { "name", "Rainbow Wave" } }),
                }) },
            }));
        CHECK(PumpUntil([&] { return bridge.presets().size() == 1; }));

        /* ERROR reply -> honest lastError. */
        const int lid = bridge.layoutCommand("identify", {});
        CHECK(lid > 0);
        CHECK(srv.RecvMsg(m, 4000));
        srv.SendMsg(MsgType::Error, (uint32_t)lid,
            nlohmann::json::object({
                { "code",    (uint32_t)ErrCode::Unavailable },
                { "error",   "unavailable" },
                { "message", "reserved for the layout engine" },
            }));
        CHECK(PumpUntil([&] {
            return bridge.lastError().contains("reserved");
        }));

        /* Server drops -> engineLost -> automatic reconnect on
           a fresh channel. */
        srv.Close();
        CHECK(PumpUntil([&] {
            std::lock_guard<std::mutex> lock(pm);
            return pipes.size() >= 2;
        }));
        std::shared_ptr<TestPipe> p1;
        {
            std::lock_guard<std::mutex> lock(pm);
            p1 = pipes[1];
        }
        ServerPeer srv2 = p1->Peer();
        CHECK(DriveServerHandshake(srv2));
        CHECK(PumpUntil([&] {
            return bridge.connectionState() == "connected";
        }));
        CHECK(attempts >= 2);

        bridge.stop();
        CHECK(PumpUntil([&] {
            return bridge.connectionState() == "disconnected";
        }));
    }

    /*---- unreachable + bounded backoff ----------------------*/
    std::printf("-- unreachable backoff\n");
    {
        ui::EngineBridge bridge;
        int              fails = 0;
        bridge.setChannelFactoryForTesting(
            [&](std::string& err)
                -> std::unique_ptr<IIpcChannel>
            {
                err = "pipe not available";
                ++fails;
                return nullptr;
            });
        bridge.setBackoffForTesting({ 10, 20 });
        bridge.start();
        CHECK(PumpUntil([&] { return fails >= 3; }));
        CHECK(PumpUntil([&] {
            return bridge.connectionState() == "unreachable";
        }));
        bridge.stop();
        CHECK(PumpUntil([&] {
            return bridge.connectionState() == "disconnected";
        }));
        const int after_stop = fails;
        QThread::msleep(60);
        QCoreApplication::processEvents();
        CHECK(fails == after_stop);   /* no further attempts   */
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
