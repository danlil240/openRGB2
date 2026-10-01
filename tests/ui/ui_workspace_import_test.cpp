// SPDX-License-Identifier: GPL-2.0-or-later
//
// ui_workspace_import_test — studio-import Task 9 fix round 1:
// the REAL end-to-end portable-bundle flow across the
// QML/controller/engine boundary. Stubs cannot catch contract
// mismatches, so this suite wires:
//
//   WorkspaceImportDialog.qml (real component, ctlOverride set
//   to the REAL WorkspaceController)
//     -> EngineBridge + EngineWireClient over an in-process pipe
//       -> the REAL IpcServer + EngineService + WorkspaceService
//          + WorkspaceStore on a temp managed root.
//
// Every byte crossing a boundary is the production one:
// readLocalFile's base64 is decoded by inspectImport before the
// bounded upload; the engine's report keys ({valid, types,
// conflicts[].id, suggested_id, has_workspace}) are read as the
// engine actually emits them; conflict choices and the
// importFinished(name, ok, error) signal signature are exercised
// through the dialog's own handlers; confirm targets a new
// workspace, preset_import for types-only bundles, and export
// round-trips through the staged-download surface.
//
// CHECK-macro main like the other UI suites — no framework.

/* Qt-free engine/ipc headers FIRST — Qt defines `emit` as a
   macro and WorkspaceCommands.h declares a member named `emit`;
   including the engine side before any Qt header keeps the
   declaration intact. */
#include "Engine.h"
#include "WorkspaceService.h"
#include "EngineService.h"
#include "IpcServer.h"
#include "IpcProtocol.h"

#include "config/StudioConfig.h"
#include "presets/PresetRegistry.h"
#include "presets/EffectRegistry.h"

#include "test_pipe.h"

#include "EngineBridge.h"
#include "WorkspaceController.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QVariantMap>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

using namespace openrgb2;
using namespace openrgb2::ipc;
using namespace openrgb2::ui;
using namespace ui_test;
using nlohmann::json;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

namespace fs = std::filesystem;

/*---------------------------------------------------------*\
||| Fixtures                                                  ||
\*---------------------------------------------------------*/

/* One valid device-type document. `name` varies so a bundled
   type and a library type of the SAME id differ → conflict. */
static void WriteType(const fs::path& dir, const std::string& id,
                      const std::string& name, int leds)
{
    std::error_code ec;
    fs::create_directories(dir, ec);
    const json ent = json::object({
        { "geometry", "fan_body" },
        { "zone",     "ring"     },
    });
    const json doc = json::object({
        { "schema_version", 1    },
        { "id",             id   },
        { "name",           name },
        { "category",       "fan"},
        { "entities", json::object({{ "body", ent }}) },
        { "zones", json::array({ json::object({
              { "id",        "ring" },
              { "entity",    "body" },
              { "led_count", leds   },
              { "layout", json::object({
                    { "type",     "ring" },
                    { "radius_m", 0.05  } }) } }) }) },
    });
    std::ofstream(dir / (id + ".device.json")) << doc.dump(2);
}

static json WorkspaceDoc(const std::string& name)
{
    return json::object({
        { "schema_version", 3    },
        { "name",           name },
        { "devices", json::object({
              { "fan", json::object({
                    { "type", "test-fan" },
                    { "x", 0.1 }, { "y", 0.05 }, { "z", -0.02 },
                    { "rx", 0.0 }, { "ry", 30.0 }, { "rz", 0.0 } }) } }) },
        { "bindings",        json::object() },
        { "device_settings", json::object() },
    });
}

static std::string B64EncodeStr(const std::string& in)
{
    static const char k[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for(size_t i = 0; i < in.size(); i += 3)
    {
        const unsigned a = (unsigned char)in[i];
        const unsigned b = i + 1 < in.size() ? (unsigned char)in[i + 1] : 0;
        const unsigned c = i + 2 < in.size() ? (unsigned char)in[i + 2] : 0;
        out += k[a >> 2];
        out += k[((a & 3) << 4) | (b >> 4)];
        out += (i + 1 < in.size()) ? k[((b & 15) << 2) | (c >> 6)] : '=';
        out += (i + 2 < in.size()) ? k[c & 63] : '=';
    }
    return out;
}

static json JsonFileEntry(const nlohmann::json& doc)
{
    return json::object({{ "encoding", "utf8" },
                         { "data",     doc.dump(2) }});
}

/*---------------------------------------------------------*\
||| The REAL engine side — EngineService + WorkspaceService  ||
||| + IpcServer, served on the TestPipe's B channel (the     ||
||| server sessions run their own threads; only the Qt side  ||
||| pumps).                                                   ||
\*---------------------------------------------------------*/
struct RealEngine
{
    openrgb2::Engine                  engine;
    studio::PresetRegistry            devices;
    WorkspaceService::Deps            wdeps;
    EngineService::Deps               edeps;
    EngineServiceOptions              eopts;
    IpcServerOptions                  sopts;
    std::unique_ptr<WorkspaceService> ws;
    std::unique_ptr<EngineService>    svc;
    std::unique_ptr<IpcServer>        server;

    RealEngine(const fs::path& root, const fs::path& types)
    {
        devices.LoadDirectory(types.string());
        wdeps.devices = devices;
        /* The same scene-apply seam ipc_workspace_test wires —
           lazy because EngineService is constructed after the
           workspace service. */
        wdeps.apply_scene =
            [this](std::shared_ptr<const WorkspaceCandidate> cand,
                   std::string& err) -> bool
            {
                return svc && cand &&
                       svc->ApplyWorkspaceCandidate(*cand, err);
            };
        ws  = std::make_unique<WorkspaceService>(root.string(),
                                                 wdeps);
        edeps.workspace = ws.get();
        svc    = std::make_unique<EngineService>(engine, edeps,
                                                 eopts);
        server = std::make_unique<IpcServer>(*svc, sopts);
        svc->SetEventSink(
            [this](const IpcEvent& ev) { server->Emit(ev); });
    }
};

/*---------------------------------------------------------*\
||| The REAL client side — EngineBridge + WorkspaceController||
||| over a fresh TestPipe per connect (reconnects mint a new ||
||| channel exactly like production).                        ||
\*---------------------------------------------------------*/
struct ClientRig
{
    std::vector<std::shared_ptr<TestPipe>> pipes;
    IpcServer*            server;
    EngineBridge          bridge;
    WorkspaceController   ctl{ &bridge };

    explicit ClientRig(IpcServer& srv) : server(&srv)
    {
        bridge.setChannelFactoryForTesting(
            [this](std::string& err)
                -> std::unique_ptr<IIpcChannel>
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
    }

    /* The engine side lives on IpcServer's own threads — the
       test only pumps the Qt event loop for the client. */
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
            QCoreApplication::processEvents(
                QEventLoop::AllEvents, 20);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1));
        }
        return pred();
    }
};

/*---------------------------------------------------------*\
||| main                                                      ||
\*---------------------------------------------------------*/
int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const fs::path root = tmp.filePath("root").toStdWString();
    const fs::path types_dir =
        tmp.filePath("packaged").toStdWString();

    /* Packaged type (8 LEDs) + a DIFFERING managed-library copy
       of the same id — a bundled "test-fan" is then a real
       conflict row, not a theoretical one. */
    WriteType(types_dir, "test-fan", "Packaged Fan", 8);
    WriteType(root / "library" / "devices", "test-fan",
              "Local Fan", 16);

    RealEngine eng(root, types_dir);
    ClientRig  rig(*eng.server);
    rig.bridge.start();
    CHECK(rig.PumpUntil([&] { return rig.ctl.available(); }));

    /*---- build the bundle on disk ---------------------------*/
    const fs::path bundle_path =
        tmp.filePath("pack.or2studio").toStdWString();
    {
        json files = json::object();
        files["studio.json"] = JsonFileEntry(
            WorkspaceDoc("bundle-desk"));
        files["presets/devices/test-fan.device.json"] =
            JsonFileEntry(json::object({
                { "schema_version", 1             },
                { "id",             "test-fan"     },
                { "name",           "Bundled Fan"  },
                { "category",       "fan"          },
                { "entities", json::object({
                      { "body", json::object({
                            { "geometry", "fan_body" },
                            { "zone",     "ring"     } }) } }) },
                { "zones", json::array({ json::object({
                      { "id",        "ring" },
                      { "entity",    "body" },
                      { "led_count", 24     },
                      { "layout", json::object({
                            { "type",     "ring" },
                            { "radius_m", 0.05  } }) } }) }) },
            }));
        const json bundle = json::object({
            { "format",  "openrgb2-bundle-files" },
            { "version", 1                       },
            { "files",   files                   },
        });
        std::ofstream(bundle_path, std::ios::binary | std::ios::trunc)
            << bundle.dump(2);
    }

    /*---- load the REAL dialog, real controller as ctl -------*/
    QQmlEngine qengine;
    const QString qml_base =
        QStringLiteral("qrc:/qt/qml/OpenRGB2/UI/studio/");
    QObject* dlg = nullptr;
    {
        QQmlComponent comp(&qengine,
            QUrl(qml_base + QStringLiteral(
                     "WorkspaceImportDialog.qml")));
        CHECK(!comp.isError());
        if(comp.isError())
        {
            std::printf("  %s\n",
                comp.errorString().toUtf8().constData());
        }
        else
        {
            dlg = comp.create(qengine.rootContext());
            CHECK(dlg != nullptr);
        }
    }
    if(dlg)
    {
        dlg->setProperty("width", 560);
        dlg->setProperty("height", 600);
        dlg->setProperty("ctlOverride",
                         QVariant::fromValue<QObject*>(&rig.ctl));
        QMetaObject::invokeMethod(dlg, "open");

        /*-- import: read -> inspect -> report -> confirm ----*/
        std::printf("-- e2e: packed bundle inspect+commit\n");
        int finished = 0;
        bool last_ok = false;
        QString last_name, last_err;
        QObject::connect(&rig.ctl, &WorkspaceController::importFinished,
                         &rig.ctl,
                         [&](const QString& n, bool ok,
                             const QString& e) {
                             ++finished;
                             last_ok   = ok;
                             last_name = n;
                             last_err  = e;
                         });

        QObject* pf = dlg->findChild<QObject*>("importPathField");
        CHECK(pf != nullptr);
        if(pf)
        {
            pf->setProperty("text",
                QString::fromStdString(bundle_path.generic_string()));
            CHECK(QMetaObject::invokeMethod(dlg, "readAndInspect"));
            /* phase "report" means: file read, base64 decoded,
               upload staged, import_inspect job ran, the report
               parsed — the four review contract fixes at once. */
            CHECK(rig.PumpUntil([&] {
                return dlg->property("phase").toString()
                       == QLatin1String("report"); }));
            const QVariantMap rep =
                dlg->property("report").toMap();
            CHECK(rep.value("valid").toBool());
            CHECK(rep.value("has_workspace").toBool());
            CHECK(rep.value("workspace").toMap()
                      .value("name").toString()
                  == QLatin1String("bundle-desk"));
            /* Conflicts key on `id` — the engine's real field. */
            const QVariantList confs =
                rep.value("conflicts").toList();
            CHECK(confs.size() == 1);
            if(confs.size() == 1)
            {
                const QVariantMap c0 = confs[0].toMap();
                CHECK(c0.value("id").toString()
                      == QLatin1String("test-fan"));
                CHECK(!c0.value("suggested_id").toString()
                          .isEmpty());
            }
            /* The dialog seeded renameMap off the same key. */
            const QVariantMap seeds =
                dlg->property("renameMap").toMap();
            CHECK(seeds.contains(QStringLiteral("test-fan")));
            CHECK(!seeds.contains(QStringLiteral("undefined")));
            /* The conflict repeater built one row keyed on the
               real `id` — rename_test-fan is its editor field. */
            QObject* crep = dlg->findChild<QObject*>(
                "conflictRepeater");
            CHECK(crep != nullptr);
            if(crep != nullptr)
            {
                CHECK(crep->property("count").toInt() == 1);
                QQuickItem* rowObj = nullptr;
                CHECK(QMetaObject::invokeMethod(
                    crep, "itemAt",
                    Q_RETURN_ARG(QQuickItem*, rowObj),
                    Q_ARG(int, 0)));
                CHECK(rowObj != nullptr);
                if(rowObj)
                {
                    CHECK(rowObj->findChild<QObject*>(
                              "rename_test-fan") != nullptr);
                }
            }

            /* confirm() runs the dialog's own collectChoices +
               mode pick (full import — the bundle has a doc). */
            CHECK(QMetaObject::invokeMethod(dlg, "confirm"));
            CHECK(rig.PumpUntil([&] { return finished >= 1; }));
            CHECK(last_ok);
            CHECK(last_name == QLatin1String("pack.or2studio"));
            /* The dialog's own handler landed phase "done" —
               guards the (name, ok, error) signature. */
            CHECK(dlg->property("phase").toString()
                  == QLatin1String("done"));
        }

        /* The engine REALLY committed: the remapped type file
           landed in the managed library and the new workspace
           generation exists on disk — nothing was applied. */
        const fs::path landed =
            root / "library" / "devices" /
            "test-fan-imported.device.json";
        CHECK(fs::exists(landed));
        CHECK(fs::exists(root / "pack-or2studio"));
        /* The imported workspace lists through the controller
           surface — the same `list` the UI shows. */
        rig.ctl.refreshList();
        CHECK(rig.PumpUntil([&] {
            const QVariantList l = rig.ctl.workspaceList();
            for(const QVariant& v : l)
            {
                if(v.toString() == QLatin1String("pack-or2studio"))
                {
                    return true;
                }
            }
            return false;
        }));

        /*-- open the imported workspace, then export ---------*/
        std::printf("-- e2e: open + export round-trip\n");
        rig.ctl.openWorkspace(QStringLiteral("pack-or2studio"));
        CHECK(rig.PumpUntil([&] {
            return rig.ctl.managed() && rig.ctl.ready(); }));
        CHECK(rig.ctl.workspaceId()
              == QLatin1String("pack-or2studio"));

        const fs::path out_path =
            tmp.filePath("roundtrip.or2studio").toStdWString();
        QObject* ef = dlg->findChild<QObject*>("exportPathField");
        CHECK(ef != nullptr);
        if(ef)
        {
            ef->setProperty("text",
                QString::fromStdString(out_path.generic_string()));
            /* doExport is the production handler: void
               exportBundle + busy phase until exportReady. */
            CHECK(QMetaObject::invokeMethod(dlg, "doExport"));
            CHECK(dlg->property("phase").toString()
                  == QLatin1String("exporting"));
            CHECK(rig.PumpUntil([&] {
                return fs::exists(out_path) &&
                       dlg->property("phase").toString()
                           == QLatin1String("idle"); }));
        }
        /* Parse the written bundle — the packed wire format
           with the workspace doc + its remapped type. */
        {
            std::ifstream in(out_path, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            const json b = json::parse(ss.str(), nullptr, false);
            CHECK(!b.is_discarded());
            CHECK(b.value("format", std::string())
                  == "openrgb2-bundle-files");
            const json f = b.value("files", json::object());
            CHECK(f.contains("studio.json"));
            CHECK(f.contains(
                "presets/devices/test-fan-imported.device.json"));
            /* Sanitized: no verified binding state exports. */
            const std::string sj =
                f.value("studio.json", json::object())
                    .value("data", std::string());
            CHECK(sj.find("\"verified\":true") ==
                  std::string::npos);
        }

        /*-- types-only bundle: preset_import mode ------------*/
        std::printf("-- e2e: types-only bundle (preset_import)\n");
        const fs::path tonly_path =
            tmp.filePath("types-only.or2studio").toStdWString();
        {
            json files = json::object();
            files["presets/devices/solo-strip.device.json"] =
                JsonFileEntry(json::object({
                    { "schema_version", 1            },
                    { "id",             "solo-strip" },
                    { "name",           "Solo Strip" },
                    { "category",       "strip"      },
                    { "entities", json::object({
                          { "body", json::object({
                                { "geometry", "strip" },
                                { "zone",     "leds" } }) } }) },
                    { "zones", json::array({ json::object({
                          { "id",        "leds"  },
                          { "entity",    "body"  },
                          { "led_count", 12      },
                          { "layout", json::object({
                                { "type",      "strip" },
                                { "spacing_m", 0.005   } }) } }) }) },
                }));
            const json bundle = json::object({
                { "format",  "openrgb2-bundle-files" },
                { "version", 1                       },
                { "files",   files                   },
            });
            std::ofstream(tonly_path,
                          std::ios::binary | std::ios::trunc)
                << bundle.dump(2);
        }
        QMetaObject::invokeMethod(dlg, "close");
        QMetaObject::invokeMethod(dlg, "open");
        if(pf)
        {
            pf->setProperty("text",
                QString::fromStdString(tonly_path.generic_string()));
            CHECK(QMetaObject::invokeMethod(dlg, "readAndInspect"));
            CHECK(rig.PumpUntil([&] {
                return dlg->property("phase").toString()
                       == QLatin1String("report"); }));
            const QVariantMap rep =
                dlg->property("report").toMap();
            CHECK(rep.value("valid").toBool());
            /* No studio.json in the bundle — the report says so
               and the dialog's confirm() picks preset_import. */
            CHECK(rep.value("has_workspace").toBool() == false);
            CHECK(QMetaObject::invokeMethod(dlg, "confirm"));
            CHECK(rig.PumpUntil([&] { return finished >= 2; }));
            CHECK(last_ok);
            /* Resource landed under the active workspace's
               scope; no new workspace was minted for it. */
            CHECK(fs::exists(root / "library" / "devices" /
                             "solo-strip.device.json"));
            CHECK(!fs::exists(root / "types-only-or2studio"));
            CHECK(dlg->property("phase").toString()
                  == QLatin1String("done"));
        }
    }

    /*---- N03: the serial profile->workspace import queue over
           ONE session. Three entries must all land — a
           finish-verified upload is a staged token, not an
           open transfer, so the third begin must never answer
           transfer_limit — the banner settles when the queue
           drains, a colliding entry fails identified, and the
           editor stays usable throughout. */
    std::printf("-- e2e: serial import queue (N03)\n");
    {
        int done = 0;
        QStringList failed;
        bool saw_banner = false;
        const QString banner =
            QStringLiteral("Importing workspace…");
        QObject::connect(&rig.ctl,
                         &WorkspaceController::statusTextChanged,
                         &rig.ctl,
                         [&] {
                             if(rig.ctl.statusText() == banner)
                             {
                                 saw_banner = true;
                             }
                         });
        QObject::connect(&rig.ctl,
                         &WorkspaceController::importFinished,
                         &rig.ctl,
                         [&](const QString& n, bool ok,
                             const QString& e) {
                             ++done;
                             if(!ok)
                             {
                                 failed << n + QLatin1String(": ")
                                        + e;
                             }
                         });
        const QString names[3] = {
            QStringLiteral("queue one"),
            QStringLiteral("queue two"),
            QStringLiteral("queue three") };
        for(const QString& n : names)
        {
            rig.ctl.importDocument(n,
                QByteArray::fromStdString(
                    WorkspaceDoc(n.toStdString()).dump(2)));
        }
        CHECK(rig.PumpUntil([&] { return done >= 3; }));
        CHECK(failed.isEmpty());
        if(!failed.isEmpty())
        {
            std::printf("  unexpected import failures: %s\n",
                        failed.join(QLatin1String(", "))
                            .toUtf8().constData());
        }
        /* The banner armed at job-accept must not outlive the
           queue — it settles to the draft truth. */
        CHECK(saw_banner);
        CHECK(rig.PumpUntil([&] {
            return rig.ctl.statusText() != banner;
        }));
        /* Each commit adopts its workspace — the editor's
           document fetch follows the active id, so the draft
           must land on the LAST import and stay ready. */
        CHECK(rig.PumpUntil([&] {
            return rig.ctl.workspaceId()
                       == QLatin1String("queue-three") &&
                   rig.ctl.ready();
        }));
        /* A re-run entry whose slug now exists fails
           identified (the engine's revision check is the
           authority) — and the editor is untouched. */
        rig.ctl.importDocument(QStringLiteral("queue one"),
            QByteArray::fromStdString(
                WorkspaceDoc("queue one").dump(2)));
        CHECK(rig.PumpUntil([&] { return done >= 4; }));
        CHECK(failed.size() == 1);
        CHECK(rig.PumpUntil([&] {
            return rig.ctl.statusText() != banner;
        }));
        CHECK(rig.ctl.ready());
    }

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
