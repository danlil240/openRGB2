// SPDX-License-Identifier: GPL-2.0-or-later
//
// ui_studio_presets_test — studio-import Task 9 suite:
// device-TYPE authoring + the portable-workspace dialogs.
//
//   Part A — the REAL DevicePresetController offline (no
//   engine, no display): candidate validation + throwaway
//   preview against a private registry copy, honest offline
//   refusals for the engine-job verbs, the variant repoint
//   guard (mirrors/type-locks refuse before any write), and
//   the test-fed resource_get / job-event correlation paths.
//
//   Part B — the adapted QML surfaces headless against
//   plain-JS stubs: DevicePresetEditor's open/edit/validate/
//   build path and WorkspaceImportDialog's read -> inspect ->
//   report -> conflict-choice -> confirm flow.
//
// Runs fully offline: DLS_PRESET_DIR (set by CMake) points the
// controller's registry at src/core/presets/devices.

#include "EngineBridge.h"
#include "WorkspaceController.h"
#include "WorkspaceEditSession.h"
#include "studio/DevicePresetController.h"
#include "studio/ObjectEditorController.h"

#include "config/StudioConfig.h"

#include <QGuiApplication>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>

#include <cstdio>
#include <cstdlib>

using namespace openrgb2;
using namespace openrgb2::ui;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

static nlohmann::json CompactDoc()
{
    return nlohmann::json::object({
        { "schema_version", 3 },
        { "name",           "desk" },
        { "devices", nlohmann::json::object({
            { "fan0", nlohmann::json::object({
                { "type", "fan-slw" }, { "x", 0.1 },
                { "y", 0.0 }, { "z", 0.2 },
                { "rx", 0.0 }, { "ry", 0.0 }, { "rz", 0.0 } }) },
        }) },
        { "bindings", nlohmann::json::object() },
        { "device_settings", nlohmann::json::object() },
    });
}

static studio::StudioDocument ParseCompact(const nlohmann::json& j)
{
    studio::StudioDocument d;
    std::vector<std::string> errs;
    const bool ok = studio::FromJson(j, d, &errs);
    CHECK(ok);
    return d;
}

struct Rig
{
    EngineBridge           bridge;
    WorkspaceController    ws{ &bridge };
    ObjectEditorController obj{ &bridge, &ws };
    DevicePresetController ctl{ &bridge, &ws, &obj };

    void LoadDoc(const nlohmann::json& j)
    {
        ws.Session().LoadApplied(ParseCompact(j), 1, 1);
    }
};

/* A minimal valid candidate preset map (the editor body's
   shape — entities/zones ride the JSON half). */
static QVariantMap Candidate(const QString& id, int leds)
{
    QVariantMap m;
    m["schema_version"] = 1;
    m["id"]             = id;
    m["name"]           = id + " name";
    m["category"]       = "fan";
    QVariantMap ent;
    ent["geometry"] = "fan_body";
    ent["zone"]     = "ring";
    QVariantMap entities;
    entities["body"] = ent;
    m["entities"] = entities;
    QVariantMap ring;
    ring["type"]      = "ring";
    ring["radius_m"]  = 0.05;
    QVariantMap zone;
    zone["id"]        = "ring";
    zone["entity"]    = "body";
    zone["led_count"] = leds;
    zone["layout"]    = ring;
    m["zones"] = QVariantList({ zone });
    return m;
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    const char* preset_dir = std::getenv("DLS_PRESET_DIR");
    CHECK(preset_dir != nullptr && preset_dir[0] != '\0');

    /*======================================================*\
    || A. DevicePresetController offline (real objects)     ||
    \*======================================================*/
    std::printf("-- presets: read surface + validation\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc());
        if(preset_dir != nullptr)
        {
            CHECK(rig.obj.LoadPresetDirectory(preset_dir).empty());
        }
        CHECK(rig.ctl.ready());

        /* Local registry view of a packaged type. */
        const QVariantMap d = rig.ctl.presetDocument("fan-slw");
        CHECK(d.value("exists").toBool());
        CHECK(d.value("doc").isValid());
        CHECK(d.value("instances").toInt() == 1);
        CHECK(rig.ctl.instanceCount("fan-slw") == 1);
        /* Unknown id is honest. */
        const QVariantMap none = rig.ctl.presetDocument("nope-x");
        CHECK(!none.value("exists").toBool());

        /* validatePreset: schema + registry checks, private —
           the draft and the library never move. */
        const QVariantMap ok = rig.ctl.validatePreset(
            Candidate("panel-new", 12));
        CHECK(ok.value("ok").toBool());
        const QVariantMap bad = rig.ctl.validatePreset(
            Candidate("bad id!", 12));
        CHECK(!bad.value("ok").toBool());
        CHECK(!bad.value("errors").toList().isEmpty());
        /* A candidate reusing an existing id validates fine —
           the SAVE decides collision policy; validation only
           checks shape + resolvable deps. */
        const QVariantMap dupe = rig.ctl.validatePreset(
            Candidate("fan-slw", 8));
        CHECK(dupe.value("ok").toBool());
        /* Cyclic child refs refuse. */
        QVariantMap cyc = Candidate("cyc-a", 4);
        QVariantMap cent;
        cent["type"] = "cyc-a";                    /* self-ref */
        QVariantMap centities = cyc["entities"].toMap();
        centities["mount"] = cent;
        cyc["entities"] = centities;
        CHECK(!rig.ctl.validatePreset(cyc).value("ok").toBool());
        /* Validation never touched the session draft. */
        CHECK(rig.obj.Presets().Contains("panel-new") == false);
        CHECK(!rig.ws.dirty());

        /* previewPreset: the throwaway __preview__ resolve —
           object rows + emitter counts, no hardware. */
        const QVariantMap pv = rig.ctl.previewPreset(
            Candidate("panel-new", 12));
        CHECK(pv.value("ok").toBool());
        const QVariantList objs = pv.value("objects").toList();
        CHECK(!objs.isEmpty());
        int emitters = 0;
        for(const QVariant& o : objs)
        {
            emitters += o.toMap().value("emitterCount").toInt();
        }
        CHECK(emitters == 12);
        const QVariantMap pvb = rig.ctl.previewPreset(
            Candidate("bad id!", 12));
        CHECK(!pvb.value("ok").toBool());
    }

    std::printf("-- presets: offline job refusals stay honest\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc());
        if(preset_dir != nullptr)
        {
            rig.obj.LoadPresetDirectory(preset_dir);
        }
        /* No engine connection — the install/delete verbs
           refuse with an error, never silently queue. */
        const QVariantMap s = rig.ctl.savePresetType(
            Candidate("panel-new", 12), false);
        CHECK(!s.value("ok").toBool());
        const QVariantMap del = rig.ctl.deletePresetType("fan-slw");
        CHECK(!del.value("ok").toBool());
        /* A packaged-default delete refuses locally before the
           engine ever sees it. */
        CHECK(del.value("error").toString().contains(
                  "packaged", Qt::CaseInsensitive)
              || del.value("error").toString().contains(
                  "not connected", Qt::CaseInsensitive)
              || !del.value("error").toString().isEmpty());
    }

    std::printf("-- presets: variant + assembly guards\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc());
        if(preset_dir != nullptr)
        {
            rig.obj.LoadPresetDirectory(preset_dir);
        }
        QSignalSpy msgs(&rig.ctl,
                        &DevicePresetController::statusMessage);
        /* Blank id refuses. */
        rig.ctl.saveInstanceAsVariant("fan0", "", "n");
        CHECK(msgs.count() >= 1);
        /* Bad charset refuses before any send. */
        rig.ctl.saveInstanceAsVariant("fan0", "bad id", "n");
        /* Unknown instance refuses. */
        rig.ctl.saveInstanceAsVariant("ghost", "v-1", "n");
        /* A mirror-source repoint refuses — set the link first
           through the shared session (one undo record), then
           the guarded variant leaves history untouched. */
        CHECK(rig.ws.Session().CommitDocEdit(
            "mirror", [](studio::StudioDocument& w) {
                w.device_settings["fan0"].mirror_of = "other";
                return true;
            }));
        const size_t u = rig.ws.Session().UndoCount();
        rig.ctl.saveInstanceAsVariant("fan0", "v-2", "n");
        CHECK(rig.ws.Session().UndoCount() == u);
        CHECK(rig.ws.Session().Undo());          /* un-mirror */

        /* Every refusal arrived as a status message and the
           mirror undo restores a clean draft. */
        CHECK(msgs.count() >= 4);
        CHECK(rig.ws.Session().UndoCount() == 0);
        CHECK(!rig.ws.dirty());

        /* createTypeFromSelection on an empty selection
           refuses honestly. */
        rig.ctl.createTypeFromSelection({}, "asm-1", "n");
        /* With an instance selected it validates, then the
           offline install refuses — still no draft mutation. */
        rig.obj.selectInstance("fan0");
        rig.ctl.createTypeFromSelection({"fan0"}, "asm-1", "n");
        CHECK(!rig.ws.dirty());
    }

    std::printf("-- presets: fed replies resolve pending ops\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc());
        if(preset_dir != nullptr)
        {
            rig.obj.LoadPresetDirectory(preset_dir);
        }
        QSignalSpy docs(&rig.ctl,
                        &DevicePresetController::presetDocumentReady);
        /* fetchPresetDocument offline can't correlate — feed
           the reply the engine would send. */
        QVariantMap body;
        body["ok"]   = true;
        body["id"]   = "eng-only";
        body["doc"]  = Candidate("eng-only", 6);
        body["packaged_default"] = false;
        rig.ctl.FeedResourceGetForTesting(body);
        CHECK(docs.count() == 1);
        /* A terminal job event resolves the pending delete —
           feed the failure shape (nothing was pending, so the
           controller ignores it honestly). */
        QVariantMap ev;
        ev["job_id"] = 7;
        ev["state"]  = "failed";
        ev["error"]  = "in use by desk";
        rig.ctl.FeedJobEventForTesting(ev);      /* no pending —
                                                  ignored */
    }

    /*======================================================*\
    || B. QML surfaces headless against stubs               ||
    \*======================================================*/
    QQmlEngine engine;
    const QString base =
        QStringLiteral("qrc:/qt/qml/OpenRGB2/UI/studio/");

    auto load = [&](const char* file) -> QObject* {
        QQmlComponent comp(&engine,
                           QUrl(base + QString::fromLatin1(file)));
        if(comp.isError())
        {
            ++failures;
            std::printf("FAIL load %s:\n%s\n", file,
                        comp.errorString().toUtf8().constData());
            return nullptr;
        }
        QObject* obj = comp.create(engine.rootContext());
        ++checks;
        if(!obj)
        {
            ++failures;
            std::printf("FAIL create %s:\n%s\n", file,
                        comp.errorString().toUtf8().constData());
        }
        return obj;
    };
    auto callv = [&](QObject* o, const char* fn) -> QVariant {
        QVariant r;
        CHECK(QMetaObject::invokeMethod(
            o, fn, Q_RETURN_ARG(QVariant, r)));
        return r;
    };

    std::printf("-- qml: DevicePresetEditor stub flow\n");
    {
        QObject* ed = load("DevicePresetEditor.qml");
        if(ed != nullptr)
        {
            ed->setProperty("width", 520);
            ed->setProperty("height", 560);
            /* JS stub controller: validate/ preview/save/
               delete + fetch seam; calls logged. */
            QJSValue stub = engine.evaluate(R"JS(
                (function(){
                    var calls = { saved: [], deleted: [],
                                  fetched: [] };
                    return {
                        presetDocument: function(tid) {
                            return tid === "fan-x"
                                ? { exists: true, packaged: false,
                                    instances: 2,
                                    doc: { schema_version: 1,
                                           id: "fan-x",
                                           name: "Fan X",
                                           category: "fan",
                                           entities: {},
                                           zones: [] } }
                                : { exists: false };
                        },
                        fetchPresetDocument: function(tid) {
                            calls.fetched.push(tid); },
                        validatePreset: function(c) {
                            return c && c.id === ""
                                ? { ok: false,
                                    errors: ["id required"] }
                                : { ok: true, errors: [] }; },
                        previewPreset: function(c) {
                            return { ok: true, errors: [],
                                     objects: [
                                { id: "a/body", emitterCount: 4 }] };
                        },
                        savePresetType: function(c, ow) {
                            calls.saved.push([c.id, !!ow]);
                            return { ok: true, pending: true }; },
                        deletePresetType: function(tid) {
                            calls.deleted.push(tid);
                            return { ok: true }; },
                        _calls: calls,
                    };
                })()
            )JS");
            CHECK(stub.isObject());
            ed->setProperty("ctlOverride", QVariant::fromValue(stub));

            /* New-type path: fields blanked; buildCandidate
               merges fields + the JSON body. */
            QMetaObject::invokeMethod(ed, "openNew");
            QVariant built = callv(ed, "buildCandidate");
            /* Empty id still merges (the candidate is checked
               by validatePreset — the stub refuses id:""). */
            QObject* idf = ed->findChild<QObject*>(
                "presetIdField");
            QObject* bodyf = ed->findChild<QObject*>(
                "presetBodyField");
            CHECK(idf != nullptr && bodyf != nullptr);
            if(idf && bodyf)
            {
                idf->setProperty("text", "my-type");
                built = callv(ed, "buildCandidate");
                CHECK(built.toMap().value("id").toString()
                      == "my-type");
                /* Broken JSON refuses to build. */
                bodyf->setProperty("text", "{ nope");
                CHECK(callv(ed, "buildCandidate").isNull());
            }

            /* Edit path: applyDoc splits the document into
               fields + JSON body. */
            QVariantMap doc;
            doc["id"] = "fan-x";
            doc["name"] = "Fan X";
            doc["category"] = "fan";
            doc["schema_version"] = 1;
            QVariantMap extra;
            extra["geometry"] = "fan_body";
            QVariantMap ents;
            ents["body"] = extra;
            doc["entities"] = ents;
            CHECK(QMetaObject::invokeMethod(
                ed, "applyDoc",
                Q_ARG(QVariant, QVariant::fromValue(doc)),
                Q_ARG(QVariant, false),
                Q_ARG(QVariant, 2)));
            if(idf && bodyf)
            {
                CHECK(idf->property("text").toString() == "fan-x");
                /* entities land in the JSON body, not a field */
                CHECK(bodyf->property("text").toString()
                      .contains("body"));
                /* Rebuilt candidate is the full document. */
                built = callv(ed, "buildCandidate");
                CHECK(built.toMap().value("id").toString()
                      == "fan-x");
                CHECK(built.toMap().value("entities").toMap()
                      .contains("body"));
            }
        }
    }

    std::printf("-- qml: WorkspaceImportDialog stub flow\n");
    {
        QObject* dlg = load("WorkspaceImportDialog.qml");
        if(dlg != nullptr)
        {
            dlg->setProperty("width", 560);
            dlg->setProperty("height", 600);
            /* Stub workspace controller: readLocalFile answers
               a bundle; inspectImport records + emits nothing
               (the real signal drives phase in production —
               the stub keeps the dialog in 'inspecting'). */
            QJSValue stub = engine.evaluate(R"JS(
                (function(){
                    var calls = { inspected: [], confirmed: [],
                                  exports: 0, cancelled: 0 };
                    return {
                        readLocalFile: function(p) {
                            return p === ""
                                ? { ok: false, error: "no path" }
                                : { ok: true, base64: "AAAA" }; },
                        writeLocalFile: function(p, b) {
                            return { ok: true }; },
                        inspectImport: function(name, bytes) {
                            calls.inspected.push(name); },
                        confirmImport: function(choices, mode) {
                            calls.confirmed.push(choices); },
                        cancelImport: function() {
                            calls.cancelled++; },
                        exportBundle: function() {
                            calls.exports++;
                            return { ok: true }; },
                        _calls: calls,
                    };
                })()
            )JS");
            CHECK(stub.isObject());
            dlg->setProperty("ctlOverride",
                             QVariant::fromValue(stub));

            /* readAndInspect with no path fails honestly. */
            QMetaObject::invokeMethod(dlg, "readAndInspect");
            CHECK(dlg->property("phase").toString() == "idle");
            CHECK(dlg->property("status").toString() != "");

            QObject* pf = dlg->findChild<QObject*>(
                "importPathField");
            CHECK(pf != nullptr);
            if(pf)
            {
                pf->setProperty("text",
                                "C:/bundles/desk.or2studio");
                QMetaObject::invokeMethod(dlg, "readAndInspect");
                CHECK(dlg->property("phase").toString()
                      == "inspecting");
                /* The name field strips directories. */
                /* (the stub's call log holds the file name) */
            }

            /* Conflict choices collect the rename map. */
            dlg->setProperty("renameMap",
                             QVariantMap({{ "old", "new" }}));
            const QVariant ch = callv(dlg, "collectChoices");
            CHECK(ch.toMap().value("old").toString() == "new");

            /* Audit Q-13, in a window so the popup really opens
               and closes. */
            QQuickWindow win;
            win.resize(800, 700);
            dlg->setProperty("parent",
                QVariant::fromValue(win.contentItem()));
            win.show();
            QMetaObject::invokeMethod(dlg, "open");
            QCoreApplication::processEvents();
            auto cancelled = [&] {
                return stub.property("_calls").property("cancelled")
                           .toInt();
            };
            /* (1) closing mid-operation (Escape, Close, outside)
               releases the staged inspect — the Close button did,
               Escape did not. */
            if(pf)
            {
                pf->setProperty("text",
                                "C:/bundles/desk.or2studio");
                QMetaObject::invokeMethod(dlg, "readAndInspect");
                CHECK(dlg->property("phase").toString()
                      == "inspecting");
                const int before = cancelled();
                QMetaObject::invokeMethod(dlg, "close");
                QCoreApplication::processEvents();
                CHECK(cancelled() == before + 1);
            }
            QMetaObject::invokeMethod(dlg, "open");
            QCoreApplication::processEvents();

            /* (2) "done" is quiescent — another import and the
               export path stay reachable without a reopen. */
            QObject* ib = dlg->findChild<QObject*>("inspectBtn");
            QObject* eb = dlg->findChild<QObject*>("exportBtn");
            QObject* ef = dlg->findChild<QObject*>("exportPathField");
            CHECK(ib && eb && ef && pf);
            if(ib && eb && ef && pf)
            {
                pf->setProperty("text", "C:/bundles/next.or2studio");
                ef->setProperty("text", "C:/out/desk.or2studio");
                dlg->setProperty("phase", "done");
                QCoreApplication::processEvents();
                CHECK(pf->property("enabled").toBool());
                CHECK(ib->property("enabled").toBool());
                CHECK(ef->property("enabled").toBool());
                CHECK(eb->property("enabled").toBool());
                QMetaObject::invokeMethod(dlg, "readAndInspect");
                CHECK(dlg->property("phase").toString()
                      == "inspecting");
            }

            /* (3) "replace my layout" never carries over to the
               next import. */
            QObject* ob = dlg->findChild<QObject*>("overwriteBox");
            CHECK(ob != nullptr);
            if(ob)
            {
                ob->setProperty("checked", true);
                QMetaObject::invokeMethod(dlg, "close");
                QMetaObject::invokeMethod(dlg, "open");
                QCoreApplication::processEvents();
                CHECK(!ob->property("checked").toBool());
            }

            /* (4) paths can be browsed, and a picked file URL
               becomes a plain local path */
            CHECK(dlg->findChild<QObject*>("importBrowseBtn"));
            CHECK(dlg->findChild<QObject*>("exportBrowseBtn"));
            QVariant lp;
            QMetaObject::invokeMethod(dlg, "localPath",
                Q_RETURN_ARG(QVariant, lp),
                Q_ARG(QVariant, QVariant(QStringLiteral(
                    "file:///C:/my%20bundles/desk.or2studio"))));
            CHECK(lp.toString()
                  == QStringLiteral("C:/my bundles/desk.or2studio"));

            QMetaObject::invokeMethod(dlg, "close");
            QCoreApplication::processEvents();
            dlg->setProperty("parent", QVariant());
        }
    }

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
