// SPDX-License-Identifier: GPL-2.0-or-later
//
// ui_studio_objects_test — studio-import Task 5 suite, adapted
// from the source-of-record editor QML tests
// (tests/editor_qml/tst_library.qml, tst_tree.qml,
// tst_inspector.qml) onto the OpenRGB2 architecture:
//
//   Part A — the REAL C++ surface offline (no engine, no
//   display): EngineBridge + WorkspaceController with a loaded
//   compact draft, driving ObjectEditorController → the shared
//   WorkspaceEditSession. Asserts the one-undo-record contract,
//   gesture preview not touching history, rename re-keying,
//   mirrored duplicates never minting bindings, the device-
//   library floor lift, favorites staying out of history, and
//   both read models' row contracts.
//
//   Part B — the adapted QML surfaces loaded headless against
//   a plain-JS stub injected through bridgeOverride: the same
//   tree filtering/selection/delete-preview and library
//   sort/search/favorite pipeline the source tests exercised,
//   plus the inspector's numeric-commit path.
//
// Runs fully offline: DLS_PRESET_DIR (set by CMake) points the
// controller's registry at src/core/presets/devices.

#include "EngineBridge.h"
#include "WorkspaceController.h"
#include "WorkspaceEditSession.h"
#include "studio/ObjectEditorController.h"
#include "studio/PresetListModel.h"
#include "studio/SceneObjectModel.h"

#include "config/StudioConfig.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QSignalSpy>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace openrgb2;
using namespace openrgb2::ui;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

/*---------------------------------------------------------*\
|| Fixture — a compact v3 draft with two fan-slw           ||
|| instances, one zone binding (fan0 -> b0), one painted   ||
|| color. fan-slw's packaged preset contributes the "ring" ||
|| zone (40 point emitters) and entity "body".            ||
\*---------------------------------------------------------*/
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
            { "fan1", nlohmann::json::object({
                { "type", "fan-slw" }, { "x", 0.3 },
                { "y", 0.0 }, { "z", 0.2 } }) },
        }) },
        { "bindings", nlohmann::json::object({
            { "b0", nlohmann::json::object({
                { "controller_name", "UNI FAN" },
                { "zone_name", "Fan 1" }, { "zone_leds", 40 } }) },
        }) },
        { "device_settings", nlohmann::json::object({
            { "fan0", nlohmann::json::object({
                { "zones", nlohmann::json::object({
                    { "ring", nlohmann::json::object({
                        { "binding", "b0" }, { "addr_base", 0 },
                        { "verified", true } }) } }) } }) },
        }) },
        { "colors", nlohmann::json::object({
            { "objects",  nlohmann::json::object(
                              {{ "fan0", "#00B4A0" }}) },
            { "emitters", nlohmann::json::object() },
        }) },
        { "effects", nlohmann::json::object({
            { "preset", "" }, { "seed", 0 }, { "speed", 1.0 },
            { "intensity", 1.0 }, { "playing", false },
            { "layers", nlohmann::json::array() } }) },
    });
}

static studio::StudioDocument ParseCompact(const nlohmann::json& j)
{
    studio::StudioDocument d;
    std::vector<std::string> errs;
    const bool ok = studio::FromJson(j, d, &errs);
    if(!ok)
    {
        std::printf("  FromJson errors: %s\n",
                    errs.empty() ? "?" : errs.front().c_str());
    }
    CHECK(ok);
    return d;
}

/* A draft-owning rig: real bridge (never connected), real
   WorkspaceController, real controller on the shared session. */
struct Rig
{
    EngineBridge          bridge;
    WorkspaceController   ws{ &bridge };
    ObjectEditorController ctl{ &bridge, &ws };

    const studio::StudioDocument& Working() const
        { return ws.Session().Working(); }
    size_t UndoCount() const { return ws.Session().UndoCount(); }

    void LoadDoc(const nlohmann::json& j)
    {
        ws.Session().LoadApplied(ParseCompact(j), 1, 1);
    }
};

int main(int argc, char** argv)
{
    /* QGuiApplication up front — the QML half needs it; the C++
       half only needs the event loop it carries. */
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    const char* preset_dir = std::getenv("DLS_PRESET_DIR");
    CHECK(preset_dir != nullptr && preset_dir[0] != '\0');

    /*======================================================*\
    || A. ObjectEditorController + shared session (offline) ||
    \*======================================================*/
    std::printf("-- controller: load, resolve, model rows\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc());
        if(preset_dir != nullptr)
        {
            const auto errs =
                rig.ctl.LoadPresetDirectory(preset_dir);
            CHECK(errs.empty());
            for(const std::string& e : errs)
            {
                std::printf("  preset error: %s\n", e.c_str());
            }
        }
        CHECK(rig.ctl.ready());
        CHECK(rig.ctl.Presets().Contains("fan-slw"));

        auto* model = static_cast<SceneObjectModel*>(
            rig.ctl.objectModel());
        /* Two instance rows + one entity row each ("fan0/body",
           "fan1/body") — parents precede children. */
        CHECK(model->rowCount() == 4);
        CHECK(model->RowOf("fan0") < model->RowOf("fan0/body"));
        CHECK(model->RowOf("fan1") < model->RowOf("fan1/body"));
        const QVariantMap r0 = model->rowAt(model->RowOf("fan0"));
        CHECK(r0.value("id").toString() == "fan0");
        CHECK(r0.value("kind").toString() == "device");
        /* Bound chip: the binding exists but nothing reports it —
           honest "unresolved", not fabricated "ok". */
        CHECK(r0.value("bound").toString() == "unresolved");
        const QVariantMap r1 = model->rowAt(model->RowOf("fan1"));
        CHECK(r1.value("bound").toString() == "none");
        const QVariantMap rb = model->rowAt(model->RowOf("fan0/body"));
        CHECK(rb.value("instancePath").toString() == "fan0");
        CHECK(rb.value("emitters").toInt() == 40);
        CHECK(rb.value("verified").toBool());
        CHECK(rb.value("geometry").toString() == "fan_body");
    }

    std::printf("-- controller: selection\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc());
        rig.ctl.LoadPresetDirectory(preset_dir ? preset_dir : "");
        QSignalSpy sel(&rig.ctl, &ObjectEditorController::selectionChanged);

        /* A resolved child id selects its owning root instance —
           the viewport-click rule. */
        rig.ctl.select("fan0/body");
        CHECK(rig.ctl.selectedInstances() ==
              QVariantList({ "fan0" }));
        rig.ctl.selectInstance("fan1", true);
        CHECK(rig.ctl.selectedInstances() ==
              QVariantList({ "fan0", "fan1" }));
        rig.ctl.clearEditorSelection();
        CHECK(rig.ctl.selectedInstances().isEmpty());
        CHECK(sel.count() >= 3);
    }

    std::printf("-- controller: gesture = one record, preview "
                "is none\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc());
        rig.ctl.LoadPresetDirectory(preset_dir ? preset_dir : "");
        rig.ctl.selectInstance("fan0");

        rig.ctl.beginTransformGesture();
        CHECK(rig.ctl.gestureActive());
        /* Preview moves the DRAFT (the canvas follows live) but
           history stays empty and the draft is not dirty. */
        rig.ctl.updateTransformGesture(0.05, 0, 0, 1, false);
        rig.ctl.updateTransformGesture(0.10, 0, 0, 1, false);
        rig.ctl.updateTransformGesture(0.15, 0, 0, 1, false);
        CHECK(rig.UndoCount() == 0);
        CHECK(rig.Working().devices.at("fan0").position.x > 0.2f);
        rig.ctl.commitTransformGesture();
        CHECK(!rig.ctl.gestureActive());
        CHECK(rig.UndoCount() == 1);
        CHECK(rig.ws.dirty());

        /* Cancel restores the snapshot and records nothing. */
        rig.ctl.beginTransformGesture();
        rig.ctl.updateTransformGesture(-0.4, 0, 0, 1, false);
        rig.ctl.cancelTransformGesture();
        CHECK(rig.UndoCount() == 1);

        rig.ctl.undo();
        CHECK(rig.Working().devices.at("fan0").position.x ==
              0.1f);
        CHECK(!rig.ws.dirty());
        rig.ctl.redo();
        CHECK(rig.Working().devices.at("fan0").position.x > 0.2f);
    }

    std::printf("-- controller: discrete ops, one record each\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc());
        rig.ctl.LoadPresetDirectory(preset_dir ? preset_dir : "");

        CHECK(rig.ctl.setInstancePosition("fan0", 0.5, 0.1, 0.4));
        CHECK(rig.UndoCount() == 1);
        CHECK(rig.Working().devices.at("fan0").position.x ==
              0.5f);
        CHECK(rig.ctl.setInstanceRotation("fan0", 0, 90, 0));
        CHECK(rig.UndoCount() == 2);
        CHECK(rig.Working().devices.at("fan0").rotation_deg.y ==
              90.0f);
        /* World == parent-local for a root instance. */
        CHECK(rig.ctl.setInstancePositionWorld("fan1", 0.7, 0, 0.3));
        CHECK(rig.Working().devices.at("fan1").position.x ==
              0.7f);
        /* Rejection never mutates and never records. */
        const size_t u = rig.UndoCount();
        CHECK(!rig.ctl.renameInstance("fan0", "fan1"));  /* taken */
        CHECK(rig.UndoCount() == u);
        CHECK(rig.Working().devices.count("fan0") == 1);
    }

    std::printf("-- controller: rename re-keys settings, colors, "
                "selection\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc());
        rig.ctl.LoadPresetDirectory(preset_dir ? preset_dir : "");
        rig.ctl.selectInstance("fan0");
        CHECK(rig.ctl.renameInstance("fan0", "fan_a"));
        const studio::StudioDocument& d = rig.Working();
        CHECK(d.devices.count("fan_a") == 1);
        CHECK(d.devices.count("fan0") == 0);
        CHECK(d.device_settings.count("fan_a") == 1);
        CHECK(d.object_colors.count("fan_a") == 1);
        /* Selection follows the new id. */
        CHECK(rig.ctl.selectedInstances() ==
              QVariantList({ "fan_a" }));
        /* Undo restores the old id everywhere. */
        rig.ctl.undo();
        CHECK(rig.Working().devices.count("fan0") == 1);
        CHECK(rig.Working().device_settings.count("fan0") == 1);
        CHECK(rig.Working().object_colors.count("fan0") == 1);
    }

    std::printf("-- controller: visible/locked\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc());
        rig.ctl.LoadPresetDirectory(preset_dir ? preset_dir : "");
        rig.ctl.setInstanceVisible("fan0", false);
        CHECK(rig.Working().device_settings.at("fan0").visible
              == false);
        rig.ctl.setInstanceLocked("fan0", true);
        CHECK(rig.Working().device_settings.at("fan0").locked);
        CHECK(rig.UndoCount() == 2);
        /* Locked instances refuse transform edits honestly. */
        const size_t u = rig.UndoCount();
        CHECK(!rig.ctl.setInstancePosition("fan0", 9, 9, 9));
        CHECK(rig.UndoCount() == u);
    }

    std::printf("-- controller: group / delete cascade\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc());
        rig.ctl.LoadPresetDirectory(preset_dir ? preset_dir : "");
        rig.ctl.selectInstance("fan0");
        rig.ctl.selectInstance("fan1", true);
        rig.ctl.groupSelected();
        const studio::StudioDocument& d = rig.Working();
        CHECK(d.devices.size() == 3);
        /* The group row exists and both fans parent into it. */
        std::string gid;
        for(const auto& kv : d.devices)
        {
            if(kv.second.type == "group")
            {
                gid = kv.first;
            }
        }
        CHECK(!gid.empty());
        if(!gid.empty())
        {
            CHECK(d.devices.at("fan0").parent == gid);
            CHECK(d.devices.at("fan1").parent == gid);
            /* Preview lists the cascade before it happens. */
            rig.ctl.selectInstance(QString::fromStdString(gid));
            const QStringList kill = rig.ctl.deletePreview();
            CHECK(kill.size() == 3);
            rig.ctl.deleteSelected();
            CHECK(rig.Working().devices.empty());
            /* Undo restores all three. */
            rig.ctl.undo();
            CHECK(rig.Working().devices.size() == 3);
            /* Ungroup acts on the selected GROUP instance (not
               its children) — children return to root. */
            rig.ctl.selectInstance(QString::fromStdString(gid));
            rig.ctl.ungroupSelected();
            for(const auto& kv : rig.Working().devices)
            {
                CHECK(kv.second.parent.empty());
            }
        }
    }

    std::printf("-- controller: mirrored duplicate mints no "
                "binding\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc());
        rig.ctl.LoadPresetDirectory(preset_dir ? preset_dir : "");
        rig.ctl.selectInstance("fan0");
        rig.ctl.duplicateMirrored();
        const studio::StudioDocument& d = rig.Working();
        CHECK(d.devices.size() == 3);
        /* One hardware producer per wireless group: the copy
           shares fan0's output via mirror_of — NO new DeviceBinding
           row may appear. */
        CHECK(d.bindings.size() == 1);
        std::string mirror_id;
        for(const auto& kv : d.devices)
        {
            if(kv.first != "fan0" && kv.first != "fan1")
            {
                mirror_id = kv.first;
            }
        }
        CHECK(!mirror_id.empty());
        if(!mirror_id.empty())
        {
            const auto sit = d.device_settings.find(mirror_id);
            CHECK(sit != d.device_settings.end());
            CHECK(sit->second.mirror_of == "fan0");
        }
        CHECK(rig.UndoCount() == 1);
    }

    std::printf("-- controller: align / distribute\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc());
        rig.ctl.LoadPresetDirectory(preset_dir ? preset_dir : "");
        rig.ctl.addDeviceInstance("fan-slw", 0.2, 0, 0.6);
        CHECK(rig.Working().devices.size() == 3);
        rig.ctl.selectInstance("fan0");
        rig.ctl.selectInstance("fan1", true);
        rig.ctl.alignSelected(0, 0);      /* min X */
        CHECK(rig.Working().devices.at("fan0").position.x ==
              rig.Working().devices.at("fan1").position.x);
        /* Distribute needs 3+ movable. */
        rig.ctl.distributeSelected(0);
        CHECK(rig.UndoCount() >= 2);
    }

    std::printf("-- controller: library add sits on the floor, "
                "unknown refused\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc());
        rig.ctl.LoadPresetDirectory(preset_dir ? preset_dir : "");
        auto* pm = static_cast<PresetListModel*>(
            rig.ctl.presetModel());
        const studio::PresetRegistry::PresetInfo* info =
            pm->InfoFor("fan-slw");
        CHECK(info != nullptr);
        rig.ctl.addDeviceInstance("fan-slw", 0.5, 0.0, 0.25);
        CHECK(rig.Working().devices.size() == 3);
        /* Find the new instance: type fan-slw at the drop point,
           y lifted so the type's footprint floor rests at 0. */
        bool found = false;
        for(const auto& kv : rig.Working().devices)
        {
            if(kv.first != "fan0" && kv.first != "fan1")
            {
                found = true;
                CHECK(kv.second.type == "fan-slw");
                CHECK(std::fabs(kv.second.position.x - 0.5f)
                      < 1e-4f);
                CHECK(std::fabs(kv.second.position.z - 0.25f)
                      < 1e-4f);
                if(info != nullptr)
                {
                    CHECK(std::fabs(kv.second.position.y
                                    - (0.0f - info->floor_y))
                          < 1e-4f);
                }
            }
        }
        CHECK(found);
        /* The add becomes the selection. */
        CHECK(rig.ctl.selectedInstances().size() == 1);
        /* Unknown / invalid ids refuse without mutating. */
        const size_t n = rig.Working().devices.size();
        rig.ctl.addDeviceInstance("no-such-type", 0, 0, 0);
        rig.ctl.addDeviceInstance("bad id!", 0, 0, 0);
        CHECK(rig.Working().devices.size() == n);
        CHECK(!rig.ctl.statusText().isEmpty());
    }

    std::printf("-- controller: favorites are prefs, never "
                "history\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc());
        rig.ctl.LoadPresetDirectory(preset_dir ? preset_dir : "");
        QSignalSpy spy(&rig.ctl,
                       &ObjectEditorController::presetLibraryChanged);
        const size_t u = rig.UndoCount();
        rig.ctl.setTypeFavorite("fan-slw", true);
        const auto& favs = rig.Working().meta.ui.favorites;
        CHECK(std::find(favs.begin(), favs.end(), "fan-slw")
              != favs.end());
        CHECK(rig.UndoCount() == u);           /* not undoable */
        CHECK(spy.count() == 1);
        /* The model row badges it. */
        auto* pm = static_cast<PresetListModel*>(
            rig.ctl.presetModel());
        bool fav_row = false;
        for(int i = 0; i < pm->count(); ++i)
        {
            const QVariantMap r = pm->rowAt(i);
            if(r.value("typeId").toString() == "fan-slw")
            {
                fav_row = r.value("favorite").toBool();
            }
        }
        CHECK(fav_row);
        /* Toggle off — still no record, draft still dirty-flagged
           as edited. */
        rig.ctl.setTypeFavorite("fan-slw", false);
        CHECK(std::find(favs.begin(), favs.end(), "fan-slw")
              == rig.Working().meta.ui.favorites.end());
        CHECK(rig.UndoCount() == u);
    }

    std::printf("-- controller: paint object + emitter\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc());
        rig.ctl.LoadPresetDirectory(preset_dir ? preset_dir : "");
        rig.ctl.paintObject("fan1", QColor(255, 0, 0));
        CHECK(rig.Working().object_colors.at("fan1") ==
              studio::MakeSceneColor(255, 0, 0));
        rig.ctl.paintEmitter("fan0/body", 3, QColor(0, 255, 0));
        CHECK(rig.Working().emitter_colors.at("fan0/body").at(3) ==
              studio::MakeSceneColor(0, 255, 0));
        CHECK(rig.UndoCount() == 2);
        /* Emitter color reads back through the resolved view. */
        const QVariantList cols =
            rig.ctl.emitterColorsOf("fan0/body");
        CHECK(cols.size() == 40);
        CHECK(cols.at(3).toString().toLower() == "#00ff00");
        rig.ctl.undo();
        CHECK(rig.Working().emitter_colors.count("fan0/body") == 0
              || rig.Working().emitter_colors.at("fan0/body")
                         .count(3) == 0);
    }

    std::printf("-- model: preset rows, engine-catalog merge, "
                "InfoFor\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc());
        rig.ctl.LoadPresetDirectory(preset_dir ? preset_dir : "");
        auto* pm = static_cast<PresetListModel*>(
            rig.ctl.presetModel());
        CHECK(pm->count() >= 10);   /* packaged devices dir */
        /* Sorted by id; rowAt carries the library facts. */
        QString prev;
        for(int i = 0; i < pm->count(); ++i)
        {
            const QVariantMap r = pm->rowAt(i);
            const QString id = r.value("typeId").toString();
            CHECK(!id.isEmpty());
            if(i > 0)
            {
                CHECK(prev <= id);
            }
            prev = id;
        }
        const studio::PresetRegistry::PresetInfo* info =
            pm->InfoFor("fan-slw");
        CHECK(info != nullptr);
        if(info != nullptr)
        {
            CHECK(info->zone_count == 1);
            CHECK(info->led_total == 40);
            /* fan-slw authors size_m {0,0,0} — canonical fallback
               is a render-time rule, so the listing's bounds are
               legitimately zero; never negative. */
            CHECK(info->bounds_m.x >= 0.0f);
            CHECK(info->floor_y <= 0.0f);
        }
        /* Engine-catalog ids merge as engineOnly rows — the wire
           shape is the engine's own {id,name,zone_count,...}. */
        pm->SetEngineCatalog(QVariantList({
            QVariantMap({
                { "id",         "engine-only-thing" },
                { "name",       "Engine-only thing" },
                { "category",   "aux" },
                { "zone_count", 1 },
                { "led_total",  7 },
            }),
            QVariantMap({ { "id", "fan-slw" } }),
        }));
        bool seen_engine_row = false;
        bool dup = false;
        int fan_rows = 0;
        for(int i = 0; i < pm->count(); ++i)
        {
            const QVariantMap r = pm->rowAt(i);
            const QString id = r.value("typeId").toString();
            if(id == "engine-only-thing")
            {
                seen_engine_row = true;
                CHECK(r.value("engineOnly").toBool());
                CHECK(r.value("ledTotal").toInt() == 7);
            }
            if(id == "fan-slw")
            {
                ++fan_rows;
            }
        }
        CHECK(seen_engine_row);
        CHECK(fan_rows == 1);
        CHECK(!dup);
    }

    /*======================================================*\
    || B. Adapted QML surfaces headless, stub bridges       ||
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
        const bool ok = QMetaObject::invokeMethod(
            o, fn, Q_RETURN_ARG(QVariant, r));
        CHECK(ok);
        return r;
    };

    std::printf("-- qml: DeviceTree stub\n");
    {
        QObject* tree = load("DeviceTree.qml");
        if(tree != nullptr)
        {
            tree->setProperty("width", 360);
            tree->setProperty("height", 500);
            /* JS stub — the bare-array model path + call log. */
            QJSValue stub = engine.evaluate(R"JS(
                (function(){
                    var calls = { selected: [], deleted: 0,
                                  vis: [], lock: [], renamed: [] };
                    return {
                        selectedInstances: ["fan0"],
                        objectList: [
                            { id: "fan0", instancePath: "fan0",
                              label: "fan0", kind: "device",
                              bound: "ok", visible: true,
                              locked: false, parentId: "" },
                            { id: "fan0/body",
                              instancePath: "fan0",
                              label: "body", kind: "device",
                              bound: "ok", visible: true,
                              locked: false, parentId: "fan0" },
                            { id: "grp", instancePath: "grp",
                              label: "grp", kind: "group",
                              bound: "none", visible: true,
                              locked: false, parentId: "" },
                        ],
                        selectInstance: function(id, add) {
                            calls.selected.push([id, !!add]); },
                        select: function(id) {
                            calls.selected.push([id, false]); },
                        deletePreview: function() {
                            return ["grp", "fan0"]; },
                        deleteSelected: function() { calls.deleted++; },
                        setInstanceVisible: function(i, v) {
                            calls.vis.push([i, v]); },
                        setInstanceLocked: function(i, v) {
                            calls.lock.push([i, v]); },
                        renameInstance: function(i, n) {
                            calls.renamed.push([i, n]); return true; },
                        _calls: calls,
                    };
                })()
            )JS");
            CHECK(stub.isObject());
            tree->setProperty("bridgeOverride",
                              QVariant::fromValue(stub));

            CHECK(callv(tree, "hasBr").toBool());
            /* statusText(kind,bound) — the two-arg variant. */
            {
                QVariant r;
                CHECK(QMetaObject::invokeMethod(
                    tree, "statusText", Q_RETURN_ARG(QVariant, r),
                    Q_ARG(QVariant, "linked"),
                    Q_ARG(QVariant, "none")));
                CHECK(r.toString() == "Mirrored");
                CHECK(QMetaObject::invokeMethod(
                    tree, "statusText", Q_RETURN_ARG(QVariant, r),
                    Q_ARG(QVariant, "device"),
                    Q_ARG(QVariant, "unresolved")));
                CHECK(r.toString() == "Missing");
                CHECK(QMetaObject::invokeMethod(
                    tree, "statusText", Q_RETURN_ARG(QVariant, r),
                    Q_ARG(QVariant, "device"),
                    Q_ARG(QVariant, "none")));
                CHECK(r.toString() == "Unmapped");
                CHECK(QMetaObject::invokeMethod(
                    tree, "statusText", Q_RETURN_ARG(QVariant, r),
                    Q_ARG(QVariant, "group"),
                    Q_ARG(QVariant, "none")));
                CHECK(r.toString() == "Group");
            }
            /* modelData prefers objectModel, falls back to the
               stub array — 3 rows. */
            CHECK(callv(tree, "modelData").toList().size() == 3);
            CHECK(callv(tree, "selCount").toInt() == 1);

            /* requestDelete with kill(2) > sel(1) asks first —
               deleteSelected is NOT called. */
            QMetaObject::invokeMethod(tree, "requestDelete");
            CHECK(stub.property("_calls")
                      .property("deleted").toInt() == 0);
            /* Without deletePreview the plain path deletes. */
            QJSValue stub2 = engine.evaluate(R"JS(
                (function(){
                    var calls = { deleted: 0 };
                    return {
                        selectedInstances: ["x"],
                        objectList: [],
                        deleteSelected: function(){ calls.deleted++; },
                        _calls: calls,
                    };
                })()
            )JS");
            tree->setProperty("bridgeOverride",
                              QVariant::fromValue(stub2));
            QMetaObject::invokeMethod(tree, "requestDelete");
            CHECK(stub2.property("_calls")
                      .property("deleted").toInt() == 1);
            delete tree;
        }
    }

    std::printf("-- qml: DeviceLibrary stub\n");
    {
        QObject* lib = load("DeviceLibrary.qml");
        if(lib != nullptr)
        {
            lib->setProperty("width", 340);
            lib->setProperty("height", 500);
            QJSValue stub = engine.evaluate(R"JS(
                (function(){
                    var calls = { fav: [], add: [], del: [] };
                    return {
                        presetModel: [
                            { typeId: "b-fan", name: "B fan",
                              category: "fan", favorite: false,
                              fromFile: false, engineOnly: false,
                              ledTotal: 20,
                              bounds: { x: 0.12, y: 0.02,
                                        z: 0.12 } },
                            { typeId: "a-kbd", name: "A kbd",
                              category: "keyboard",
                              favorite: true, fromFile: true,
                              engineOnly: false, ledTotal: 104,
                              bounds: { x: 0.4, y: 0.03,
                                        z: 0.15 } },
                            { typeId: "c-eng", name: "C eng",
                              category: "misc", favorite: false,
                              fromFile: false, engineOnly: true,
                              ledTotal: 0 },
                        ],
                        selectedInstances: [],
                        setTypeFavorite: function(t, on) {
                            calls.fav.push([t, on]); },
                        addDeviceInstance: function(t, x, y, z) {
                            calls.add.push([t, x, y, z]); },
                        deletePresetType: undefined,
                        _calls: calls,
                    };
                })()
            )JS");
            lib->setProperty("bridgeOverride",
                             QVariant::fromValue(stub));
            /* sceneView stub — canvasCenter() drives the button
               add position. */
            lib->setProperty("sceneView", QVariant::fromValue(
                engine.evaluate(
                    "(function(){ return { canvasCenter: function()"
                    " { return { x: 0.5, y: 0, z: 0.3 } } } })()")));

            CHECK(callv(lib, "hasBr").toBool());
            CHECK(callv(lib, "allTypeIds").toList().size() == 3);
            /* favorites first, then alphabetical. */
            const QVariantList fr = callv(lib, "filteredRows")
                                        .toList();
            CHECK(fr.size() == 3);
            CHECK(fr.at(0).toMap().value("typeId") == "a-kbd");
            CHECK(fr.at(1).toMap().value("typeId") == "b-fan");
            /* categories derived + sorted. */
            const QVariantList cats = callv(lib, "categories")
                                          .toList();
            CHECK(cats.contains("fan"));
            CHECK(cats.contains("keyboard"));
            /* search filter */
            lib->setProperty("search", "kbd");
            CHECK(callv(lib, "filteredRows").toList().size() == 1);
            lib->setProperty("search", "");
            /* category filter */
            lib->setProperty("category", "fan");
            const QVariantList fc = callv(lib, "filteredRows")
                                        .toList();
            CHECK(fc.size() == 1);
            CHECK(fc.at(0).toMap().value("typeId") == "b-fan");
            lib->setProperty("category", "");
            /* favorite toggle routes the verb (void JS function —
               no return arg). */
            {
                QMetaObject::invokeMethod(
                    lib, "toggleFav",
                    Q_ARG(QVariant, "b-fan"), Q_ARG(QVariant, true));
                CHECK(stub.property("_calls")
                          .property("fav").property("length")
                          .toInt() >= 1);
            }
            /* addAtCenter uses canvasCenter()'s desk point. */
            QMetaObject::invokeMethod(
                lib, "addAtCenter", Q_ARG(QVariant, "b-fan"));
            {
                const QJSValue add = stub.property("_calls")
                                         .property("add");
                CHECK(add.property("length").toInt() == 1);
                const QJSValue a0 = add.property(0);
                CHECK(a0.property(0).toString() == "b-fan");
                CHECK(std::fabs(a0.property(1).toNumber() - 0.5)
                      < 1e-6);
                CHECK(std::fabs(a0.property(3).toNumber() - 0.3)
                      < 1e-6);
            }
            /* deleteType refuses honestly when the controller
               lacks the verb. */
            {
                QVariant r;
                CHECK(QMetaObject::invokeMethod(
                    lib, "deleteType", Q_RETURN_ARG(QVariant, r),
                    Q_ARG(QVariant, "b-fan")));
                CHECK(!r.toString().isEmpty());
            }
            /* authoring stays hidden without the verbs */
            CHECK(!callv(lib, "canAuthorTypes").toBool());
            CHECK(!callv(lib, "canDeleteTypes").toBool());
            /* dialog validation */
            {
                QVariant r;
                CHECK(QMetaObject::invokeMethod(
                    lib, "dlgError", Q_RETURN_ARG(QVariant, r),
                    Q_ARG(QVariant, ""), Q_ARG(QVariant, "x")));
                CHECK(r.toString() != "");
                CHECK(QMetaObject::invokeMethod(
                    lib, "dlgError", Q_RETURN_ARG(QVariant, r),
                    Q_ARG(QVariant, "b-fan"),   /* id taken */
                    Q_ARG(QVariant, "x")));
                CHECK(r.toString().contains("exists"));
                CHECK(QMetaObject::invokeMethod(
                    lib, "dlgError", Q_RETURN_ARG(QVariant, r),
                    Q_ARG(QVariant, "new_id"),
                    Q_ARG(QVariant, "Name")));
                CHECK(r.toString() == "");
            }
            delete lib;
        }
    }

    std::printf("-- qml: TransformInspector stub\n");
    {
        QObject* insp = load("TransformInspector.qml");
        if(insp != nullptr)
        {
            QJSValue stub = engine.evaluate(R"JS(
                (function(){
                    var calls = { pos: [], rot: [] };
                    return {
                        selectedInstances: ["fan0"],
                        instanceState: function(id) {
                            return { id: id, x: 0.12, y: 0.02,
                                     z: 0.34, rx: 0, ry: 45,
                                     rz: 0, locked: false,
                                     visible: true };
                        },
                        setInstancePosition: function(i,x,y,z){
                            calls.pos.push([i,x,y,z]); },
                        setInstanceRotation: function(i,a,b,c){
                            calls.rot.push([i,a,b,c]); },
                        _calls: calls,
                    };
                })()
            )JS");
            insp->setProperty("bridgeOverride",
                              QVariant::fromValue(stub));
            QMetaObject::invokeMethod(insp, "reload");
            /* meters -> mm/deg display values */
            CHECK(std::fabs(insp->property("posX").toDouble()
                            - 120.0) < 0.01);
            CHECK(std::fabs(insp->property("posZ").toDouble()
                            - 340.0) < 0.01);
            CHECK(std::fabs(insp->property("rotY").toDouble()
                            - 45.0) < 0.01);
            CHECK(insp->property("hasTarget").toBool());
            /* commitPos maps mm back to meters for the verb. */
            QMetaObject::invokeMethod(
                insp, "commitPos",
                Q_ARG(QVariant, 0), Q_ARG(QVariant, 250.0));
            {
                const QJSValue pos =
                    stub.property("_calls").property("pos");
                CHECK(pos.property("length").toInt() == 1);
                const QJSValue p0 = pos.property(0);
                CHECK(std::fabs(p0.property(1).toNumber() - 0.25)
                      < 1e-6);
                CHECK(std::fabs(p0.property(3).toNumber() - 0.34)
                      < 1e-6);
            }
            /* +90 yaw stepper routes through setInstanceRotation. */
            QMetaObject::invokeMethod(
                insp, "stepYaw", Q_ARG(QVariant, 90.0));
            {
                const QJSValue rot =
                    stub.property("_calls").property("rot");
                CHECK(rot.property("length").toInt() == 1);
                CHECK(std::fabs(rot.property(0)
                                    .property(2).toNumber() - 135.0)
                      < 1e-6);
            }
            delete insp;
        }
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
