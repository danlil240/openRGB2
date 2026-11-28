// SPDX-License-Identifier: GPL-2.0-or-later
//
// Scene content out of band (audit I-4) — a real EngineBridge
// over the in-process pipe against the REAL EngineService +
// WorkspaceService + IpcServer:
//
//   - a scene whose document serializes past the 1 MiB body
//     cap still connects: WELCOME/STATE/SCENE carry revision,
//     name and document_sha256 only, and the bridge fetches
//     the document through a `scene` download transfer, then
//     paints bridge.scene() with it — the document shape the
//     UI consumers read never changes
//   - with a forced-tiny inline bound every scene goes out of
//     band; a later apply (scene EVENT) re-fetches, and no
//     sceneChanged ever exposes a loaded scene without its
//     document
//
// CHECK-macro main like the other UI suites — no framework.

#include "Engine.h"
#include "EngineService.h"
#include "IpcProtocol.h"
#include "IpcServer.h"
#include "WorkspaceService.h"

#include "presets/PresetRegistry.h"
#include "scene/SceneJson.h"
#include "scene/SceneTypes.h"

#include "test_pipe.h"

#include "EngineBridge.h"
#include "JsonVariant.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QVariantMap>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

using namespace openrgb2;
using namespace openrgb2::ipc;
using namespace openrgb2::ui;
using namespace ui_test;
using nlohmann::json;
namespace fs = std::filesystem;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

/* A decor-only scene: `objects` x `emitters` positioned
   emitters — big enough documents without any binding. */
static std::shared_ptr<studio::SceneDocument> BigScene(
    const std::string& name, int objects, int emitters)
{
    auto d = std::make_shared<studio::SceneDocument>();
    d->name = name;
    for(int i = 0; i < objects; ++i)
    {
        studio::SceneObject o;
        o.id   = "decor-" + std::to_string(i);
        o.label = "Decor panel " + std::to_string(i);
        o.kind = studio::ObjectKind::Decor;
        for(int e = 0; e < emitters; ++e)
        {
            studio::Emitter em;
            em.local_pos = { 0.001234f * (float)e,
                             0.004321f * (float)i, 0.123456f };
            em.group     = "edge-strip-group";
            o.emitters.push_back(em);
        }
        d->objects.push_back(std::move(o));
    }
    return d;
}

struct RealEngine
{
    openrgb2::Engine                  engine;
    WorkspaceService::Deps            wdeps;
    EngineService::Deps               edeps;
    EngineServiceOptions              eopts;
    std::unique_ptr<WorkspaceService> ws;
    std::unique_ptr<EngineService>    svc;
    std::unique_ptr<IpcServer>        server;

    RealEngine(const fs::path& root, const EngineServiceOptions& o)
        : eopts(o)
    {
        ws  = std::make_unique<WorkspaceService>(root.string(),
                                                 wdeps);
        edeps.workspace = ws.get();
        svc    = std::make_unique<EngineService>(engine, edeps,
                                                 eopts);
        server = std::make_unique<IpcServer>(*svc);
        svc->SetEventSink(
            [this](const IpcEvent& ev) { server->Emit(ev); });
    }
};

struct ClientRig
{
    std::vector<std::shared_ptr<TestPipe>> pipes;
    IpcServer*   server;
    EngineBridge bridge;
    int          scene_changes   = 0;
    int          loaded_without_doc = 0;

    explicit ClientRig(IpcServer& srv) : server(&srv)
    {
        bridge.setChannelFactoryForTesting(
            [this](std::string& err) -> std::unique_ptr<IIpcChannel>
            {
                (void)err;
                auto p = std::make_shared<TestPipe>();
                pipes.push_back(p);
                server->Serve(p->ServerChannel(),
                              ChannelKind::Command);
                return p->ClientChannel();
            });
        EngineWireClient::Options wopts;
        wopts.keepalive_ms = 0;
        bridge.setWireOptionsForTesting(wopts);
        bridge.setBackoffForTesting({ 25 });
        QObject::connect(&bridge, &EngineBridge::sceneChanged,
                         &bridge, [this] {
            ++scene_changes;
            const QVariantMap s = bridge.scene();
            if(s.value(QStringLiteral("loaded")).toBool() &&
               !s.value(QStringLiteral("document")).toMap()
                    .contains(QStringLiteral("objects")))
            {
                ++loaded_without_doc;
            }
        });
    }

    template <typename Pred>
    bool PumpUntil(Pred pred, int ms = 20000)
    {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(ms);
        while(std::chrono::steady_clock::now() < deadline)
        {
            if(pred())
            {
                return true;
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return pred();
    }
};

static json SceneDocOf(const EngineBridge& b)
{
    const json d = VariantToJson(
        b.scene().value(QStringLiteral("document")));
    return d.is_object() ? d : json::object();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    QTemporaryDir tmp;
    CHECK(tmp.isValid());

    /*---- a scene past the 1 MiB cap still connects ---------*/
    {
        std::printf("-- oversize scene: connect + fetch\n");
        RealEngine eng(fs::path(tmp.filePath("a").toStdWString()),
                       EngineServiceOptions{});
        auto big = BigScene("huge", 150, 100);
        const size_t doc_bytes = studio::ToJson(*big).dump().size();
        CHECK(doc_bytes > kMaxBodyBytes);
        std::string err;
        CHECK(eng.svc->ApplySceneDocument(big, err));

        ClientRig rig(*eng.server);
        rig.bridge.start();
        const bool painted = rig.PumpUntil([&] {
            return rig.bridge.connected() &&
                   SceneDocOf(rig.bridge).value("name", "") == "huge";
        });
        CHECK(painted);
        if(painted)
        {
            const json doc = SceneDocOf(rig.bridge);
            CHECK(doc["objects"].size() == 150);
            CHECK(doc == studio::ToJson(*eng.svc->Scene()));
            CHECK(rig.bridge.scene().value(
                      QStringLiteral("revision")).toULongLong() ==
                  eng.svc->SceneRevision());
        }
        CHECK(rig.loaded_without_doc == 0);
        /* WELCOME's STATE stayed under the cap, metadata only. */
        const std::vector<uint8_t> w = EncodeMessage(
            MsgType::Welcome, 0, eng.svc->OnWelcome());
        CHECK(!w.empty());
        rig.bridge.stop();
    }

    /*---- every scene out of band: re-fetch on a new apply ---*/
    {
        std::printf("-- tiny inline bound: fetch, re-fetch, reuse\n");
        EngineServiceOptions o;
        o.scene_inline_max_bytes = 0;
        RealEngine eng(fs::path(tmp.filePath("b").toStdWString()), o);
        std::string err;
        CHECK(eng.svc->ApplySceneDocument(BigScene("first", 2, 3), err));

        /* Engine side: every body is metadata + hash. */
        IpcReply gs = eng.svc->Dispatch(MsgType::GetScene,
                                        json::object());
        CHECK(gs.code == ErrCode::Ok);
        CHECK(gs.body["document"].is_null());
        CHECK(gs.body.value("document_omitted", false));
        CHECK(gs.body.value("name", "") == "first");
        CHECK(!gs.body.value("document_sha256", "").empty());

        ClientRig rig(*eng.server);
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] {
            return SceneDocOf(rig.bridge).value("name", "") == "first";
        }));
        CHECK(rig.bridge.scene().value(QStringLiteral("name"))
                  .toString() == QLatin1String("first"));

        /* A new apply: the scene EVENT is metadata only; the
           bridge fetches the new document before publishing. */
        CHECK(eng.svc->ApplySceneDocument(BigScene("second", 3, 4),
                                          err));
        CHECK(rig.PumpUntil([&] {
            return SceneDocOf(rig.bridge).value("name", "") == "second";
        }));
        const json doc2 = SceneDocOf(rig.bridge);
        CHECK(doc2 == studio::ToJson(*eng.svc->Scene()));
        CHECK(rig.bridge.scene().value(QStringLiteral("revision"))
                  .toULongLong() == eng.svc->SceneRevision());

        /* A same-document body (GET_SCENE: same hash) reuses the
           held document — published at once, no transfer. */
        const int before = rig.scene_changes;
        rig.bridge.getScene();
        CHECK(rig.PumpUntil([&] {
            return rig.scene_changes > before; }));
        CHECK(SceneDocOf(rig.bridge) == doc2);
        CHECK(rig.loaded_without_doc == 0);
        rig.bridge.stop();
    }

    std::printf("ui_scene_transfer_test: %d checks, %d failures\n",
                checks, failures);
    return failures ? 1 : 0;
}
