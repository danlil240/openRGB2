// SPDX-License-Identifier: GPL-2.0-or-later
//
// ui_workspace_recovery_test — studio-import Task 10: the
// file-action + external-change recovery surface end-to-end
// over the REAL engine. Same harness conviction as
// ui_workspace_import_test — no stubs across a boundary the
// task is about:
//
//   EngineBridge + WorkspaceController over an in-process pipe
//     -> the REAL IpcServer + EngineService + WorkspaceService
//        + WorkspaceStore on a temp managed root.
//
// The ported ConfigStore cases (source:
// tests/studio_config_store_test) land here as:
//
//   external replacement reported as "modified" — via the
//     passive ExternalScan compare on the polled `get` reply;
//   external deletion reported as "deleted" — marker removal;
//   same-content rewrite reproduces the signature — NO
//     repeated notice (source's watcher suppression);
//   detection NEVER applies: an apply_scene counter proves
//     nothing reached the hardware path;
//   a dirty draft latches reload/open/restore for an explicit
//     confirm (source/destination text shown first);
//   Restore Backup commits the marker backup's generation
//     forward through the real store;
//   a dirty draft survives open-workspace switching in the
//     durable draft store, and a staged record outlives the
//     session (recovery offer on the next attach);
//   exportBundle on an unmanaged engine reports an honest
//     error instead of wedging the dialog.
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

#include "test_pipe.h"

#include "EngineBridge.h"
#include "WorkspaceController.h"
#include "WorkspaceDraftStore.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QStringList>
#include <QVariantMap>

#include <chrono>
#include <cstdio>
#include <filesystem>
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

static void WriteType(const fs::path& dir, const std::string& id,
                      int leds)
{
    std::error_code ec;
    fs::create_directories(dir, ec);
    const json ent = json::object({
        { "geometry", "fan_body" },
        { "zone",     "ring"     },
    });
    const json doc = json::object({
        { "schema_version", 1       },
        { "id",             id      },
        { "name",           "Fan"   },
        { "category",       "fan"   },
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

static studio::StudioDocument ParseDocForStore()
{
    studio::StudioDocument d;
    std::vector<std::string> errs;
    studio::FromJson(WorkspaceDoc("recovered-doc"), d, &errs);
    return d;
}

static std::string ReadAll(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

static void WriteAll(const fs::path& p, const std::string& s)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << s;
}

/*---------------------------------------------------------*\
||| The REAL engine side — EngineService + WorkspaceService  ||
||| + IpcServer, served on the TestPipe's B channel.          ||
||| apply_scene is a counting stub — the "detection never     ||
||| reaches hardware" assertion is a counter read, not a      ||
||| hoped-for side effect.                                    ||
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
    int                               applies = 0;

    RealEngine(const fs::path& root, const fs::path& types)
    {
        devices.LoadDirectory(types.string());
        wdeps.devices = devices;
        wdeps.apply_scene =
            [this](std::shared_ptr<const WorkspaceCandidate>,
                   std::string&) -> bool
            {
                ++applies;
                return true;
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
\*---------------------------------------------------------*/
struct ClientRig
{
    std::vector<std::shared_ptr<TestPipe>> pipes;
    IpcServer*            server;
    EngineBridge          bridge;
    WorkspaceController   ctl{ &bridge };
    int                   notices = 0;
    QString               last_notice;

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
        QObject::connect(&ctl, &WorkspaceController::notice,
                         &ctl, [this](const QString& t) {
                             ++notices;
                             last_notice = t;
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
            QCoreApplication::processEvents(
                QEventLoop::AllEvents, 20);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1));
        }
        return pred();
    }

    /* A short settle — pump for `ms` and confirm `stay` never
       became true (the "no notice / no commit" assertions). */
    template <typename Pred>
    bool PumpSettle(Pred stay, int ms = 400)
    {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(ms);
        while(std::chrono::steady_clock::now() < deadline)
        {
            QCoreApplication::processEvents(
                QEventLoop::AllEvents, 15);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1));
            if(stay())
            {
                return false;
            }
        }
        return true;
    }
};

/*---------------------------------------------------------*\
||| main                                                      ||
\*---------------------------------------------------------*/
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const fs::path root = tmp.filePath("root").toStdWString();
    const fs::path types_dir =
        tmp.filePath("packaged").toStdWString();
    WriteType(types_dir, "test-fan", 8);

    RealEngine eng(root, types_dir);

    /* Seed "shelf" (imported — never applies, never adopts)
       then "desk" twice (gen1 "first", gen2 "second"; the
       marker backup names gen1 for the restore case). */
    CHECK(eng.ws->Import("shelf", WorkspaceDoc("shelf-doc"),
                         0).ok);
    {
        const auto r1 =
            eng.ws->Save("desk", WorkspaceDoc("first"), 0);
        CHECK(r1.ok);
        CHECK(r1.workspace_revision == 1);
        const auto r2 =
            eng.ws->Save("desk", WorkspaceDoc("second"), 1);
        CHECK(r2.ok);
        CHECK(r2.generation == 2);
    }
    /* (The two explicit saves above may or may not apply —
       the apply counter is only asserted as a DELTA around
       detection, which is the Task-10 contract.) */

    const fs::path wsdir   = root / "desk";
    const fs::path gen2doc =
        wsdir / "generations" / "gen-000002" / "document.json";
    const fs::path marker  = wsdir / "active";
    CHECK(fs::exists(gen2doc));
    CHECK(fs::exists(marker));

    const QString drafts = tmp.filePath("drafts");

    /*========================================================*\
    || A. attach + managed paths                               ||
    \*========================================================*/
    std::printf("-- attach + workspace paths\n");
    ClientRig rig(*eng.server);
    rig.ctl.setDraftDirForTesting(drafts);
    rig.ctl.setDebounceForTesting(10);
    rig.ctl.setExternalPollForTesting(40);
    rig.bridge.start();
    CHECK(rig.PumpUntil([&] {
        return rig.ctl.managed() && rig.ctl.ready(); }));
    CHECK(rig.ctl.workspaceId() == QLatin1String("desk"));
    CHECK(rig.ctl.workspaceRevision() == 2);
    /* The managed paths cross as verbatim engine strings —
       compare as paths, not text (slash convention differs). */
    CHECK(fs::path(rig.ctl.workspaceRoot().toStdWString()) ==
          root);
    CHECK(fs::path(rig.ctl.workspaceDir().toStdWString()) ==
          wsdir);
    CHECK(rig.ctl.externalChange().isEmpty());
    CHECK(!rig.ctl.dirty());

    /*========================================================*\
    || B. external replacement -> reported once, never applied ||
    \*========================================================*/
    std::printf("-- external modify -> notice once, no apply\n");
    const int notices_before = rig.notices;
    const int applies_before = eng.applies;
    const std::string gen2_bytes = ReadAll(gen2doc);
    WriteAll(gen2doc, gen2_bytes + "\n/* outside edit */");
    CHECK(rig.PumpUntil([&] {
        return rig.ctl.externalChange() ==
               QLatin1String("modified"); }, 8000));
    /* The engine names what moved when it can; a pure content
       change reports the generic "changed on disk" detail —
       either way the surface is honest and non-empty. */
    CHECK(!rig.ctl.externalDetail().isEmpty());
    /* Reported ONCE per transition. */
    CHECK(rig.notices == notices_before + 1);
    CHECK(rig.last_notice.contains(
        QLatin1String("nothing was applied"),
        Qt::CaseInsensitive));
    /* Detection is read-only: no commit, no apply, no fetch
       adoption — the draft stays exactly clean. */
    CHECK(eng.applies == applies_before);
    CHECK(rig.ctl.workspaceRevision() == 2);
    CHECK(!rig.ctl.dirty());
    CHECK(rig.ctl.Session().Working().meta.name == "second");

    /*========================================================*\
    || C. same-content rewrite -> suppressed (report once)     ||
    \*========================================================*/
    std::printf("-- same-content rewrite -> no repeat notice\n");
    WriteAll(gen2doc, gen2_bytes + "\n/* outside edit */");
    CHECK(rig.PumpSettle([&] {
        return rig.notices > notices_before + 1; }, 500));
    CHECK(rig.notices == notices_before + 1);
    CHECK(rig.ctl.externalChange() ==
          QLatin1String("modified"));

    /* Restoring the exact bytes re-matches the baseline —
       the notice state clears with no new notice. */
    WriteAll(gen2doc, gen2_bytes);
    CHECK(rig.PumpUntil([&] {
        return rig.ctl.externalChange().isEmpty(); }, 8000));
    CHECK(rig.notices == notices_before + 1);

    /*========================================================*\
    || D. external deletion -> "deleted"                       ||
    \*========================================================*/
    std::printf("-- external marker deletion -> deleted\n");
    const std::string marker_bytes = ReadAll(marker);
    fs::remove(marker);
    CHECK(rig.PumpUntil([&] {
        return rig.ctl.externalChange() ==
               QLatin1String("deleted"); }, 8000));
    CHECK(rig.notices == notices_before + 2);
    CHECK(eng.applies == applies_before);   /* still nothing */
    /* Restoring the marker byte-for-byte clears it. */
    WriteAll(marker, marker_bytes);
    CHECK(rig.PumpUntil([&] {
        return rig.ctl.externalChange().isEmpty(); }, 8000));

    /*========================================================*\
    || E. dirty draft latches reload — explicit confirm        ||
    \*========================================================*/
    std::printf("-- dirty draft: reload latches for confirm\n");
    CHECK(rig.ctl.Session().CommitDocEdit("rename",
        [](studio::StudioDocument& d) {
            d.meta.name = "drafted"; return true; }));
    rig.ctl.NoteDraftEdited();
    CHECK(rig.ctl.dirty());

    rig.ctl.reload();
    CHECK(rig.ctl.replaceOffered());
    /* Source + destination before anything is discarded. */
    CHECK(rig.ctl.replaceText().contains(
        QLatin1String("Source:")));
    CHECK(rig.ctl.replaceText().contains(
        QLatin1String("desk")));
    /* Nothing ran — no job, no fetch, revision untouched. */
    CHECK(rig.PumpSettle([&] {
        return rig.ctl.workspaceRevision() != 2 ||
               !rig.ctl.dirty(); }, 400));
    CHECK(rig.ctl.dirty());

    /* Cancel keeps the draft verbatim. */
    rig.ctl.cancelReplace();
    CHECK(!rig.ctl.replaceOffered());
    CHECK(rig.ctl.dirty());
    CHECK(rig.ctl.Session().Working().meta.name == "drafted");

    /* Confirm runs the reload — disk truth replaces edits. */
    rig.ctl.reload();
    CHECK(rig.ctl.replaceOffered());
    rig.ctl.confirmReplace();
    CHECK(rig.PumpUntil([&] { return !rig.ctl.dirty(); }));
    CHECK(rig.ctl.Session().Working().meta.name == "second");
    CHECK(rig.ctl.workspaceRevision() == 2);

    /*========================================================*\
    || F. Restore Backup — commits gen-000001 forward          ||
    \*========================================================*/
    std::printf("-- restore backup -> new generation\n");
    /* Corrupt the active generation's document so the state
       is visibly "modified" going in (detection again — and
       the restore still takes the HONEST backup, not the
       corrupted file). */
    WriteAll(gen2doc, "{ corrupt ");
    CHECK(rig.PumpUntil([&] {
        return rig.ctl.externalChange() ==
               QLatin1String("modified"); }, 8000));

    CHECK(rig.ctl.Session().CommitDocEdit("rename",
        [](studio::StudioDocument& d) {
            d.meta.name = "restore-test"; return true; }));
    rig.ctl.NoteDraftEdited();
    CHECK(rig.ctl.dirty());
    rig.ctl.restoreBackup();
    CHECK(rig.ctl.replaceOffered());
    CHECK(rig.ctl.replaceText().contains(
        QLatin1String("last-valid")));
    rig.ctl.confirmReplace();
    /* gen-000001's document ("first") commits forward as
       gen-000003; the draft adopts it wholesale. */
    CHECK(rig.PumpUntil([&] {
        return !rig.ctl.dirty() &&
               rig.ctl.workspaceRevision() == 3; }));
    CHECK(rig.ctl.Session().Working().meta.name == "first");
    CHECK(fs::exists(wsdir / "generations" / "gen-000003" /
                     "document.json"));
    /* The committed-forward generation re-baselines the
       compare — the corrupt gen2 stops being "the change". */
    CHECK(rig.PumpUntil([&] {
        return rig.ctl.externalChange().isEmpty(); }, 8000));

    /*========================================================*\
    || F2. tamper + reload -> RECOVERED publish clears the     ||
    ||     latch (the dev-QA wedge: a recovered reload         ||
    ||     publishes a LOWER revision — the stale-echo guard   ||
    ||     must never freeze externalChange; the engine's      ||
    ||     live compare always lands)                          ||
    \*========================================================*/
    std::printf("-- tamper+reload recovers -> external clears\n");
    {
        /* F ended on gen-000003 (rev 3). Corrupt the active
           generation's document — the scan surfaces
           "modified", then a clean-session reload recovers
           the marker backup and publishes a revision LOWER
           than the held 3. */
        const fs::path gen3dir =
            wsdir / "generations" / "gen-000003";
        WriteAll(gen3dir / "document.json", "{ corrupt ");
        CHECK(rig.PumpUntil([&] {
            return rig.ctl.externalChange() ==
                   QLatin1String("modified"); }, 8000));
        rig.ctl.reload();
        CHECK(rig.PumpUntil([&] {
            return rig.ctl.externalChange().isEmpty(); }, 10000));
        /* The recovered publish adopted — the revision
           rewound to the backup marker's, and the doc the
           session holds is what the backup carries. The
           fetch must pull the RECOVERED candidate — a build
           whose DocumentBytes still read the corrupt
           marker-selected generation surfaced
           "workspace document failed to parse" here. */
        CHECK(rig.PumpUntil([&] {
            return rig.ctl.workspaceRevision() < 3; }, 8000));
        CHECK(rig.ctl.error().isEmpty());
        CHECK(rig.ctl.Session().Working().meta.name == "first");
    }

    /*========================================================*\
    || G. openWorkspace over a dirty draft latches; the draft  ||
    ||    survives the switch in the durable store             ||
    \*========================================================*/
    std::printf("-- openWorkspace dirty latch + draft survives\n");
    CHECK(rig.ctl.Session().CommitDocEdit("rename",
        [](studio::StudioDocument& d) {
            d.meta.name = "keep-me"; return true; }));
    rig.ctl.NoteDraftEdited();
    rig.ctl.FlushDrafts();          /* durable NOW */
    CHECK(rig.ctl.dirty());
    rig.ctl.openWorkspace(QStringLiteral("shelf"));
    CHECK(rig.ctl.replaceOffered());
    CHECK(rig.ctl.replaceText().contains(
        QLatin1String("shelf")));          /* destination */
    CHECK(rig.ctl.replaceText().contains(
        QLatin1String("desk")));           /* source      */
    rig.ctl.confirmReplace();
    /* `ready` stays true on the pre-switch draft — wait for
       the SHELF document to actually adopt, not just the id. */
    CHECK(rig.PumpUntil([&] {
        return rig.ctl.workspaceId() == QLatin1String("shelf") &&
               rig.ctl.Session().HasWorking() &&
               rig.ctl.Session().Working().meta.name ==
                   "shelf-doc"; }));
    CHECK(rig.ctl.Session().Working().meta.name == "shelf-doc");
    /* The abandoned "desk" draft is still on disk — never
       silently dropped by the switch. */
    CHECK(rig.ctl.Drafts().Read(QStringLiteral("desk")).exists);

    /*========================================================*\
    || H. crash recovery — staged record survives the session  ||
    \*========================================================*/
    std::printf("-- recovery offer on next attach\n");
    {
        /* A dirty draft on the CURRENT workspace, flushed to
           the draft store, then the client "dies". */
        CHECK(rig.ctl.Session().CommitDocEdit("rename",
            [](studio::StudioDocument& d) {
                d.meta.name = "crashed"; return true; }));
        rig.ctl.NoteDraftEdited();
        rig.ctl.FlushDrafts();
        CHECK(rig.ctl.dirty());
        rig.bridge.stop();
    }
    {
        /* A fresh controller over the same drafts dir — the
           offer must arrive without any replay. */
        ClientRig rig2(*eng.server);
        rig2.ctl.setDraftDirForTesting(drafts);
        rig2.ctl.setDebounceForTesting(10);
        rig2.ctl.setExternalPollForTesting(40);
        rig2.bridge.start();
        CHECK(rig2.PumpUntil([&] {
            return rig2.ctl.managed() && rig2.ctl.ready() &&
                   rig2.ctl.recoveryOffered(); }));
        /* Offered, NEVER applied — the engine saw nothing. */
        CHECK(!rig2.ctl.dirty());

        /* DISCARD first — the record drops and the offer
           clears without ever staging a draft. */
        rig2.ctl.discardDraft();
        CHECK(!rig2.ctl.recoveryOffered());
        CHECK(!rig2.ctl.dirty());
        CHECK(!rig2.ctl.Drafts().Read(
            QStringLiteral("shelf")).exists);
        /* No re-offer — discard is final. */
        CHECK(rig2.PumpSettle([&] {
            return rig2.ctl.recoveryOffered(); }, 300));

        /* RECOVER: stage a second record (the store's own
           path); the poll's next snapshot re-offers it, and
           recovering stages the draft LOCALLY — nothing
           reaches the engine without an explicit save. */
        const studio::StudioDocument rec = ParseDocForStore();
        rig2.ctl.Drafts().Stage(QStringLiteral("shelf"), 1, 1,
            QString(), studio::ToJson(rec));
        rig2.ctl.Drafts().Flush();
        CHECK(rig2.PumpUntil([&] {
            return rig2.ctl.recoveryOffered(); }));
        rig2.ctl.recoverDraft();
        CHECK(rig2.PumpUntil([&] { return rig2.ctl.dirty(); }));
        CHECK(rig2.ctl.Session().Working().meta.name ==
              "recovered-doc");
        rig2.bridge.stop();
    }

    /*========================================================*\
    || I. exportBundle with no managed workspace -> error      ||
    ||    (the dialog-unstick fix — never a silent no-op)      ||
    \*========================================================*/
    std::printf("-- unmanaged export reports honestly\n");
    {
        QTemporaryDir t2;
        CHECK(t2.isValid());
        RealEngine eng2(t2.filePath("root").toStdWString(),
                        types_dir);
        ClientRig rig3(*eng2.server);
        rig3.bridge.start();
        CHECK(rig3.PumpUntil([&] {
            return rig3.ctl.available() && !rig3.ctl.managed(); }));
        rig3.ctl.exportBundle();
        CHECK(rig3.PumpUntil([&] {
            return !rig3.ctl.error().isEmpty(); }));
        rig3.bridge.stop();
    }

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
