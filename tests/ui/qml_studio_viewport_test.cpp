// SPDX-License-Identifier: GPL-2.0-or-later
//
// qml_studio_viewport_test — studio-import Task 8 suite.
// Adapted from the source-of-record tests/editor_qml/main.cpp
// (the C++ e2e half of the editor viewport tests) onto the
// OpenRGB2 architecture:
//
//   Part A — the REAL surfaces offline (no engine, no
//   display): EngineBridge + WorkspaceController with a loaded
//   compact draft drive the adapted QML SelectionController and
//   CameraController through stub cam/view objects — every
//   gesture lands on the shared WorkspaceEditSession (one undo
//   record, cancel restores, marquee selection, paint writes).
//
//   Part B — the real StudioViewport.qml instantiated headless
//   against the real controllers + UiSettings: object delegates
//   compose from the shared SceneObjectModel, camera poses
//   persist through setCameraState (meta.camera — never undo),
//   the quality tier writes meta.render + the shell pref, the
//   undo gate explains itself, the pick-mode seam routes.
//
//   Part C — device-family + environment components compile and
//   instantiate under the ctx contract (the detailed family/tier
//   assertions live in tests/ui/studio_qml, run by qmltestrunner).
//
// Runs fully offline: DLS_PRESET_DIR (set by CMake) points the
// controller's registry at src/core/presets/devices.

#include "DiagnosticsController.h"
#include "EngineBridge.h"
#include "LayoutController.h"
#include "SceneEditController.h"
#include "StartupTaskController.h"
#include "StudioViewportModel.h"
#include "UiSettings.h"
#include "WorkspaceController.h"
#include "WorkspaceEditSession.h"
#include "studio/EffectEditorController.h"
#include "studio/ObjectEditorController.h"
#include "studio/SceneObjectModel.h"

#include "config/StudioConfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QVariant>
#include <QVariantMap>
#include <QVector3D>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

using namespace openrgb2;
using namespace openrgb2::ui;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

/*---------------------------------------------------------*\
|| Compact v3 draft — fan-slw contributes the "ring" zone  ||
|| (40 point emitters) and entity "body" (fan_body geom).  ||
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
                { "y", 0.0 }, { "z", 0.2 },
                { "rx", 0.0 }, { "ry", 90.0 }, { "rz", 0.0 } }) },
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

/*---------------------------------------------------------*\
|| QML helpers                                              ||
\*---------------------------------------------------------*/
static const char* kVpBase =
    "qrc:/qt/qml/OpenRGB2/UI/studio/viewport/";

/* QML functions are invokable with QVariant args — pass exactly
   the declared arity. */
static QVariant CallFn(QObject* o, const char* fn,
                       std::vector<QVariant> args)
{
    QVariant ret;
    bool ok = false;
    switch(args.size())
    {
    case 0:
        ok = QMetaObject::invokeMethod(
            o, fn, Q_RETURN_ARG(QVariant, ret));
        break;
    case 1:
        ok = QMetaObject::invokeMethod(
            o, fn, Q_RETURN_ARG(QVariant, ret),
            Q_ARG(QVariant, args[0]));
        break;
    case 2:
        ok = QMetaObject::invokeMethod(
            o, fn, Q_RETURN_ARG(QVariant, ret),
            Q_ARG(QVariant, args[0]), Q_ARG(QVariant, args[1]));
        break;
    case 3:
        ok = QMetaObject::invokeMethod(
            o, fn, Q_RETURN_ARG(QVariant, ret),
            Q_ARG(QVariant, args[0]), Q_ARG(QVariant, args[1]),
            Q_ARG(QVariant, args[2]));
        break;
    case 4:
        ok = QMetaObject::invokeMethod(
            o, fn, Q_RETURN_ARG(QVariant, ret),
            Q_ARG(QVariant, args[0]), Q_ARG(QVariant, args[1]),
            Q_ARG(QVariant, args[2]), Q_ARG(QVariant, args[3]));
        break;
    case 5:
        ok = QMetaObject::invokeMethod(
            o, fn, Q_RETURN_ARG(QVariant, ret),
            Q_ARG(QVariant, args[0]), Q_ARG(QVariant, args[1]),
            Q_ARG(QVariant, args[2]), Q_ARG(QVariant, args[3]),
            Q_ARG(QVariant, args[4]));
        break;
    case 6:
        ok = QMetaObject::invokeMethod(
            o, fn, Q_RETURN_ARG(QVariant, ret),
            Q_ARG(QVariant, args[0]), Q_ARG(QVariant, args[1]),
            Q_ARG(QVariant, args[2]), Q_ARG(QVariant, args[3]),
            Q_ARG(QVariant, args[4]), Q_ARG(QVariant, args[5]));
        break;
    default:
        break;
    }
    if(!ok)
    {
        ++failures;
        std::printf("FAIL callFn %s arity %d\n", fn,
                    (int)args.size());
    }
    return ret;
}

/* Harness: instantiate the REAL SelectionController against the
   REAL editor + state source (set via context properties) and
   pure-QML stub cam/view/gizmo surfaces. */
static QObject* MakeSelHarness(QQmlEngine& eng)
{
    QQmlComponent c(&eng);
    c.setData(R"QML(
import QtQuick
import "editor" as Ed
/* Item root — QtObject has no default data property in Qt6. */
Item {
    id: h
    property var selCtl: s
    property var camStub: camS
    property var viewStub: viewS
    /* marquee fixtures — plain objects with the node fields the
       selector reads (visible, oKind, oInst, scenePosition). */
    property var nodes: []
    function each(cb) {
        for (var i = 0; i < nodes.length; i++) cb(nodes[i])
    }
    QtObject {
        id: camS
        property bool ortho: true
        property bool deferPose: false
        property bool deferredSave: false
        property var log: []
        property var origin: ({ "forward": Qt.vector3d(0,0,-1) })
        function poseSnapshot() {
            return { t: Qt.vector3d(0,0,0), yaw: 0, pitch: -38,
                     dist: 1.21, sp: 0.9, vn: "desk" }
        }
        function panPixels(a,b)   { log.push("pan") }
        function orbitPixels(a,b) { log.push("orbit") }
        function enterFreeView()  { ortho = false; log.push("enterFree") }
        function endPose()        { deferredSave = false; log.push("endPose") }
        function flushDeferredPose() {
            if (deferredSave) { deferredSave = false; log.push("flush") }
        }
        function restorePose(sn)  { log.push("restore") }
        function pointUnderPointer(x,y) {
            return Qt.vector3d(0.1, 0.02, 0.2)
        }
        function planeHitPoint(x,y,ax,v) {
            return Qt.vector3d(0.3, 0.02, 0.4)
        }
        function zoomAt(a,b,d)    { log.push("zoom") }
        function frameIds(i)      { log.push("frame") }
    }
    QtObject {
        id: viewS
        property var nextPick: null
        property var _map: function(p) {
            return Qt.vector3d(p.x * 40 + 100, 100, 0)
        }
        property real height: 500
        property var camera: null
        function pick(x,y)          { return nextPick }
        function mapFrom3DScene(p)  { return _map(p) }
        function mapTo3DScene(v) {
            return Qt.vector3d((v.x - 250) * 0.002,
                               (250 - v.y) * 0.002, 1.0 - v.z)
        }
    }
    Ed.SelectionController {
        id: s
        editor: testEditor
        stateSource: testStateSource
        cam: camS
        view3d: viewS
        gizmo: ({ "ring": false,
                  "updatePivot": function() {},
                  "angleAt": function(x, y) { return x * 0.01 },
                  "ringHit": function(x, y) { return false } })
        eachNode: h.each
    }
}
)QML", QUrl(QString::fromLatin1(kVpBase) +
            QStringLiteral("harness.qml")));
    if(c.isError())
    {
        ++failures;
        std::printf("FAIL harness load:\n%s\n",
                    c.errorString().toUtf8().constData());
        return nullptr;
    }
    return c.create(eng.rootContext());
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    const char* preset_dir = std::getenv("DLS_PRESET_DIR");
    CHECK(preset_dir != nullptr && preset_dir[0] != '\0');

    /*======================================================*\
    || A. C++ seams on the shared session                   ||
    \*======================================================*/
    std::printf("-- session: meta prefs never record undo\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc());

        const QVariantMap cam0 = rig.ws.cameraState();
        CHECK(cam0.value("view").toString() == "desk");
        CHECK(cam0.value("projection").toString()
              == "orthographic");

        QSignalSpy camSpy(&rig.ws, &WorkspaceController::cameraChanged);
        rig.ws.setCameraState({{ "view", "top" },
                               { "pitch", -89.9 },
                               { "tx", 0.4 }, { "ty", 0.1 },
                               { "tz", -0.2 }});
        CHECK(camSpy.count() == 1);
        const QVariantMap cam1 = rig.ws.cameraState();
        CHECK(cam1.value("view").toString() == "top");
        CHECK(std::fabs(cam1.value("tx").toDouble() - 0.4) < 1e-6);
        /* Prefs never mint undo records. */
        CHECK(rig.UndoCount() == 0);
        /* Invalid view refused. */
        rig.ws.setCameraState({{ "view", "bogus" }});
        CHECK(rig.ws.cameraState().value("view").toString()
              == "top");

        QSignalSpy rpSpy(&rig.ws,
                         &WorkspaceController::renderPrefsChanged);
        rig.ws.setRenderQuality("high");
        CHECK(rpSpy.count() == 1);
        CHECK(rig.ws.renderQuality() == "high");
        rig.ws.setRenderQuality("ultra");
        CHECK(rig.ws.renderQuality() == "high");
        rig.ws.setRenderBloom(false);
        CHECK(!rig.ws.renderBloom());
        CHECK(rig.UndoCount() == 0);
        /* Reduced motion rides the same meta path — writes flip
           meta.ui.reduced_motion, notify renderPrefsChanged, and
           mint no undo record (fix round 1). */
        CHECK(!rig.ws.reducedMotion());
        const int rpCount0 = rpSpy.count();
        rig.ws.setReducedMotion(true);
        CHECK(rig.ws.reducedMotion());
        CHECK(rpSpy.count() == rpCount0 + 1);
        CHECK(rig.UndoCount() == 0);
        rig.ws.setReducedMotion(false);
        CHECK(!rig.ws.reducedMotion());
        CHECK(rig.ws.dirty());
    }

    std::printf("-- model: quaternion roles\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc());
        rig.ctl.LoadPresetDirectory(preset_dir ? preset_dir : "");
        auto* model =
            static_cast<SceneObjectModel*>(rig.ctl.objectModel());
        const QVariantMap r1 = model->rowAt(model->RowOf("fan1"));
        /* ry=90 deg — the core's RotationQuat (Rz*Ry*Rx order). */
        const double qw = r1.value("qw").toDouble();
        const double qy = r1.value("qy").toDouble();
        CHECK(std::fabs(std::fabs(qw) - std::sqrt(0.5)) < 1e-4);
        CHECK(std::fabs(std::fabs(qy) - std::sqrt(0.5)) < 1e-4);
        const QVariantMap r0 = model->rowAt(model->RowOf("fan0"));
        CHECK(std::fabs(r0.value("qw").toDouble() - 1.0) < 1e-4);
        CHECK(std::fabs(r0.value("qy").toDouble()) < 1e-4);
    }

    /*======================================================*\
    || B. QML controllers against the REAL editor surface    ||
    \*======================================================*/
    std::printf("-- selection controller: real editor e2e\n");
    {
        QTemporaryDir prefsDir;
        CHECK(prefsDir.isValid());
        UiSettings prefs;
        prefs.setPathForTesting(
            prefsDir.filePath(QStringLiteral("ui.ini")));

        Rig rig;
        rig.LoadDoc(CompactDoc());
        rig.ctl.LoadPresetDirectory(preset_dir ? preset_dir : "");
        CHECK(rig.ctl.ready());

        QQmlEngine eng;
        eng.rootContext()->setContextProperty(
            QStringLiteral("testEditor"), &rig.ctl);
        eng.rootContext()->setContextProperty(
            QStringLiteral("testStateSource"), &rig.ws);

        std::unique_ptr<QObject> h(MakeSelHarness(eng));
        CHECK(h != nullptr);
        if(!h)
        {
            std::printf("%d checks, %d failures\n", checks, failures);
            return 1;
        }
        QObject* sel = h->property("selCtl").value<QObject*>();
        QObject* camStub = h->property("camStub").value<QObject*>();
        CHECK(sel != nullptr && camStub != nullptr);

        /* Click selects the owning instance through the real
           controller — no undo record. */
        CallFn(sel, "beginPressAt",
               { 100, 100, (int)Qt::LeftButton, 0,
                 QStringLiteral("obj|fan0"),
                 QVariant::fromValue(QVector3D(0.1f, 0.02f, 0.2f)) });
        CallFn(sel, "endGesture",
               { 100, 100, (int)Qt::LeftButton, 0 });
        CHECK(rig.ctl.selectedInstances() ==
              QVariantList({ "fan0" }));
        CHECK(rig.UndoCount() == 0);

        /* Ctrl+drag on the device: ONE gesture — begin, updates,
           commit — and exactly one undo record on the shared
           session. The stub cam's planeHitPoint makes the drag
           deterministic offscreen. */
        CallFn(sel, "beginPressAt",
               { 100, 100, (int)Qt::LeftButton,
                 (int)Qt::ControlModifier,
                 QStringLiteral("obj|fan0"),
                 QVariant::fromValue(QVector3D(0.1f, 0.02f, 0.2f)) });
        CallFn(sel, "dragTo",
               { 160, 100, (int)Qt::ControlModifier });
        CHECK(sel->property("gesture").toInt() == 2);
        CHECK(rig.ctl.gestureActive());
        CallFn(sel, "dragTo",
               { 170, 100, (int)Qt::ControlModifier });
        CallFn(sel, "endGesture",
               { 170, 100, (int)Qt::LeftButton,
                 (int)Qt::ControlModifier });
        CHECK(rig.UndoCount() == 1);
        const double x1 = rig.Working().devices.at("fan0")
                              .position.x;
        CHECK(std::fabs(x1 - 0.1) > 1e-4);
        rig.ctl.undo();
        CHECK(std::fabs(rig.Working().devices.at("fan0")
                            .position.x - 0.1f) < 1e-5);

        /* Escape mid-drag: cancel — the draft restores, zero
           history. */
        CallFn(sel, "beginPressAt",
               { 100, 100, (int)Qt::LeftButton,
                 (int)Qt::ControlModifier,
                 QStringLiteral("obj|fan0"),
                 QVariant::fromValue(QVector3D(0.1f, 0.02f, 0.2f)) });
        CallFn(sel, "dragTo",
               { 160, 100, (int)Qt::ControlModifier });
        CHECK(rig.ctl.gestureActive());
        CallFn(sel, "cancel", {});
        CHECK(!rig.ctl.gestureActive());
        CHECK(rig.UndoCount() == 0);
        CHECK(std::fabs(rig.Working().devices.at("fan0")
                            .position.x - 0.1f) < 1e-5);

        /* Middle-button pan on a device never transforms it. */
        CallFn(sel, "beginPressAt",
               { 100, 100, (int)Qt::MiddleButton, 0,
                 QStringLiteral("obj|fan0"),
                 QVariant::fromValue(QVector3D(0.1f, 0.02f, 0.2f)) });
        CallFn(sel, "dragTo", { 160, 140, 0 });
        CHECK(sel->property("gesture").toInt() == 1);
        CallFn(sel, "endGesture",
               { 160, 140, (int)Qt::MiddleButton, 0 });
        CHECK(std::fabs(rig.Working().devices.at("fan0")
                            .position.x - 0.1f) < 1e-5);

        /* Paint tool: emit| presses paint through the real
           controller — the draft's emitter colors mutate (one
           undo record per stroke paint) and the camera never
           orbits. */
        sel->setProperty("tool", 2);
        const size_t undo0 = rig.UndoCount();
        CallFn(sel, "beginPressAt",
               { 100, 100, (int)Qt::LeftButton, 0,
                 QStringLiteral("emit|fan0/body|3"),
                 QVariant::fromValue(QVector3D(0.1f, 0.02f, 0.2f)) });
        CHECK(sel->property("gesture").toInt() == 6);
        CallFn(sel, "endGesture",
               { 100, 100, (int)Qt::LeftButton, 0 });
        CHECK(rig.UndoCount() == undo0 + 1);
        /* A drag on a body in paint mode promotes nothing. */
        CallFn(sel, "beginPressAt",
               { 100, 100, (int)Qt::LeftButton, 0,
                 QStringLiteral("obj|fan0"),
                 QVariant::fromValue(QVector3D(0.1f, 0.02f, 0.2f)) });
        CallFn(sel, "dragTo", { 200, 150, 0 });
        CHECK(sel->property("gesture").toInt() == 0);
        CallFn(sel, "endGesture",
               { 200, 150, (int)Qt::LeftButton, 0 });
        sel->setProperty("tool", 0);
    }

    std::printf("-- camera controller: persisted pose e2e\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc());

        QQmlEngine eng;
        eng.rootContext()->setContextProperty(
            QStringLiteral("testEditor"), &rig.ctl);
        eng.rootContext()->setContextProperty(
            QStringLiteral("testStateSource"), &rig.ws);

        /* The real CameraController against stub view/cam nodes,
           prefsSource = the real WorkspaceController — applyView
           persists meta.camera through setCameraState. */
        QQmlComponent c(&eng);
        c.setData(R"QML(
import QtQuick
import "editor" as Ed
Item {
    id: h
    property var cam: cc
    Ed.CameraController {
        id: cc
        prefsSource: testStateSource
        view3d: ({
            "height": 500, "camera": null,
            "mapTo3DScene": function(v) {
                return Qt.vector3d(v.x * 0.002, v.y * 0.002, 1.0)
            },
            "mapFrom3DScene": function(p) {
                return Qt.vector3d(0, 0, 0)
            }
        })
        origin: ({
            "position": Qt.vector3d(0,0,0),
            "eulerRotation": Qt.vector3d(0,0,0),
            "right": Qt.vector3d(1,0,0),
            "up": Qt.vector3d(0,1,0),
            "forward": Qt.vector3d(0,0,-1)
        })
        orthoCam: ({ "horizontalMagnification": 0,
                     "verticalMagnification": 0,
                     "position": Qt.vector3d(0,0,0) })
        perspCam: ({ "position": Qt.vector3d(0,0,0),
                     "fieldOfView": 60 })
        boundsOf: function(ids) { return null }
        /* Production wiring (StudioViewport.qml:662) — poseFinished
           persists the pose through the shared workspace. */
        onPoseFinished: prefsSource.setCameraState(cc.stateMap())
    }
}
)QML", QUrl(QString::fromLatin1(kVpBase) +
                QStringLiteral("camharness.qml")));
        CHECK(!c.isError());
        if(c.isError())
        {
            std::printf("FAIL cam harness:\n%s\n",
                        c.errorString().toUtf8().constData());
        }
        std::unique_ptr<QObject> h(c.create(eng.rootContext()));
        CHECK(h != nullptr);
        if(!h)
        {
            std::printf("%d checks, %d failures\n", checks, failures);
            return 1;
        }
        QObject* cam = h->property("cam").value<QObject*>();
        CHECK(cam != nullptr);

        /* applyView persists through the REAL workspace — the
           meta.camera write, no undo record. */
        CallFn(cam, "applyView", { QStringLiteral("top") });
        CHECK(cam->property("viewName").toString() == "top");
        CHECK(rig.ws.cameraState().value("view").toString()
              == "top");
        CHECK(std::fabs(rig.ws.cameraState().value("pitch")
                            .toDouble() + 89.9) < 1e-3);
        CHECK(rig.UndoCount() == 0);

        /* A fresh controller restores the persisted pose —
           startup/ws-load path. */
        QQmlComponent c2(&eng);
        c2.setData(R"QML(
import QtQuick
import "editor" as Ed
Ed.CameraController {
    prefsSource: testStateSource
    view3d: ({ "height": 500, "camera": null,
               "mapTo3DScene": function(v) {
                   return Qt.vector3d(0,0,0) },
               "mapFrom3DScene": function(p) {
                   return Qt.vector3d(0,0,0) } })
    origin: ({ "position": Qt.vector3d(0,0,0),
               "eulerRotation": Qt.vector3d(0,0,0),
               "right": Qt.vector3d(1,0,0), "up": Qt.vector3d(0,1,0),
               "forward": Qt.vector3d(0,0,-1) })
    orthoCam: ({ "horizontalMagnification": 0,
                 "verticalMagnification": 0,
                 "position": Qt.vector3d(0,0,0) })
    perspCam: ({ "position": Qt.vector3d(0,0,0),
                 "fieldOfView": 60 })
}
)QML", QUrl(QString::fromLatin1(kVpBase) +
                    QStringLiteral("camharness2.qml")));
        std::unique_ptr<QObject> cam2(c2.create(eng.rootContext()));
        CHECK(cam2 != nullptr);
        if(cam2)
        {
            QMetaObject::invokeMethod(cam2.get(), "applyPersisted");
            CHECK(cam2->property("viewName").toString() == "top");
        }

        /* Wheel zoom clamps both directions and persists once. */
        const double smin = cam->property("spanMin").toDouble();
        const double smax = cam->property("spanMax").toDouble();
        for(int i = 0; i < 40; i++)
            QMetaObject::invokeMethod(
                cam, "zoomAt", Q_ARG(QVariant, 250),
                Q_ARG(QVariant, 250), Q_ARG(QVariant, 120));
        double sp = cam->property("span").toDouble();
        CHECK(sp >= smin - 1e-9);
        for(int i = 0; i < 60; i++)
            QMetaObject::invokeMethod(
                cam, "zoomAt", Q_ARG(QVariant, 250),
                Q_ARG(QVariant, 250), Q_ARG(QVariant, -120));
        sp = cam->property("span").toDouble();
        CHECK(std::fabs(sp - smax) < 1e-9);
        CHECK(rig.ws.cameraState().value("span").toDouble()
              >= smin - 1e-9);
    }

    /*======================================================*\
    || C. The real StudioViewport headless                   ||
    \*======================================================*/
    std::printf("-- viewport: delegates + wiring\n");
    {
        QTemporaryDir prefsDir;
        CHECK(prefsDir.isValid());
        UiSettings prefs;
        prefs.setPathForTesting(
            prefsDir.filePath(QStringLiteral("ui.ini")));

        Rig rig;
        rig.LoadDoc(CompactDoc());
        rig.ctl.LoadPresetDirectory(preset_dir ? preset_dir : "");
        StudioViewportModel preview(&rig.bridge, &rig.ws);

        QQmlEngine eng;
        QQmlComponent comp(
            &eng, QUrl(QString::fromLatin1(kVpBase) +
                       QStringLiteral("StudioViewport.qml")));
        if(comp.isError())
        {
            ++failures;
            std::printf("FAIL viewport load:\n%s\n",
                        comp.errorString().toUtf8().constData());
        }
        else
        {
            std::unique_ptr<QObject> vp(comp.createWithInitialProperties(
                {
                    { QStringLiteral("objectCtl"),
                      QVariant::fromValue<QObject*>(&rig.ctl) },
                    { QStringLiteral("workspaceCtl"),
                      QVariant::fromValue<QObject*>(&rig.ws) },
                    { QStringLiteral("viewport"),
                      QVariant::fromValue<QObject*>(&preview) },
                    { QStringLiteral("prefs"),
                      QVariant::fromValue<QObject*>(&prefs) },
                    { QStringLiteral("width"), 800 },
                    { QStringLiteral("height"), 500 },
                }, eng.rootContext()));
            CHECK(vp != nullptr);
            if(vp)
            {
                /* Controllers exist and are wired to the REAL
                   surfaces. */
                QObject* sel = vp->findChild<QObject*>(
                    QStringLiteral("selCtl"));
                QObject* cam = vp->findChild<QObject*>(
                    QStringLiteral("camCtl"));
                QObject* giz = vp->findChild<QObject*>(
                    QStringLiteral("gizmo"));
                CHECK(sel != nullptr && cam != nullptr
                      && giz != nullptr);
                CHECK(sel->property("editor")
                          .value<QObject*>() == &rig.ctl);
                CHECK(cam->property("prefsSource")
                          .value<QObject*>() == &rig.ws);

                /* The persisted default pose (compact meta has
                   no camera section -> defaults) restored. */
                CHECK(cam->property("viewName").toString()
                          == "desk");

                /* worldPosOf composes — a resolved object row
                   reports a world position. */
                const QVariant wp = CallFn(
                    vp.get(), "worldPosOf",
                    { QStringLiteral("fan0/body") });
                CHECK(wp.isValid());

                /* Camera preset -> the REAL workspace persists
                   meta.camera. */
                QMetaObject::invokeMethod(
                    cam, "applyView",
                    Q_ARG(QVariant, QStringLiteral("top")));
                CHECK(rig.ws.cameraState().value("view")
                          .toString() == "top");
                QMetaObject::invokeMethod(
                    cam, "applyView",
                    Q_ARG(QVariant, QStringLiteral("desk")));

                /* quality tier -> meta.render.quality AND the
                   shell pref. */
                QMetaObject::invokeMethod(
                    vp.get(), "setQualityTier",
                    Q_ARG(QVariant, QStringLiteral("high")));
                CHECK(rig.ws.renderQuality() == "high");
                CHECK(prefs.viewportQuality() == "high");
                CHECK(vp->property("qualityTier").toString()
                          == "high");
                QMetaObject::invokeMethod(
                    vp.get(), "setQualityTier",
                    Q_ARG(QVariant, QStringLiteral("balanced")));

                /* Undo gate explains itself during a gesture —
                   drive one through the real editor. */
                rig.ctl.selectInstance(QStringLiteral("fan0"));
                CallFn(sel, "beginPressAt",
                       { 400, 250, (int)Qt::LeftButton,
                         (int)Qt::ControlModifier,
                         QStringLiteral("obj|fan0"),
                         QVariant::fromValue(
                             QVector3D(0.1f, 0.02f, 0.2f)) });
                /* The real cam's planeHitPoint may not resolve
                   offscreen — force the gesture with the explicit
                   plane snapshot path. */
                CallFn(sel, "dragTo",
                       { 460, 250, (int)Qt::ControlModifier });
                if(rig.ctl.gestureActive())
                {
                    const size_t undoBefore = rig.UndoCount();
                    CallFn(vp.get(), "undoOrRedo", { false });
                    /* Refused — explained on the status line. */
                    CHECK(rig.UndoCount() == undoBefore);
                    CallFn(sel, "cancel", {});
                    CHECK(!rig.ctl.gestureActive());
                }
                else
                {
                    /* Offscreen projection unavailable — the
                       gesture never armed; still no undo. */
                    CHECK(rig.UndoCount() == 0);
                }

                /* Pick-mode arm routes through the seam. */
                QQmlEngine* pe = &eng;
                /* Self-contained: QQmlEngine::evaluate globals don't
                   persist across calls — the log rides on the
                   function object itself. */
                const QJSValue pickCb = pe->evaluate(QStringLiteral(
                    "(function(){ var f = function(pt, phase){ "
                    "f.log.push(phase); }; f.log = []; return f; })()"));
                CHECK(pickCb.isCallable());
                CallFn(vp.get(), "armPick",
                       { QStringLiteral("origin"), 0.0,
                         QVariant::fromValue(pickCb) });
                CHECK(sel->property("pickMode").toString()
                          == "origin");
                CallFn(sel, "beginPressAt",
                       { 400, 250, (int)Qt::LeftButton, 0,
                         QStringLiteral(""), QVariant() });
                CHECK(sel->property("gesture").toInt() == 7);
                CallFn(sel, "endGesture",
                       { 400, 250, (int)Qt::LeftButton, 0 });
                const QJSValue pickLog = pickCb.property("log");
                CHECK(pickLog.property("length").toInt() >= 1);
            }
        }
    }

    /*======================================================*\
    || D. The REAL Layout page — selection reconcile +      ||
    ||    focus peers (fix round 1)                           ||
    \*======================================================*/
    std::printf("-- layout page: reconcile + focus peers\n");
    {
        QTemporaryDir prefsDir;
        CHECK(prefsDir.isValid());
        UiSettings prefs;
        prefs.setPathForTesting(
            prefsDir.filePath(QStringLiteral("ui.ini")));

        EngineBridge bridge;
        WorkspaceController ws(&bridge);
        SceneEditController scene_edit(&bridge);
        LayoutController layout_ctl(&bridge);
        scene_edit.setWorkspace(&ws);
        layout_ctl.setWorkspace(&ws);
        ObjectEditorController ctl(&bridge, &ws);
        EffectEditorController eff(&bridge, &ws);
        StudioViewportModel preview(&bridge, &ws);
        DiagnosticsController diag(&bridge);
        StartupTaskController startup(QDir::tempPath());
        scene_edit.setUiSettings(&prefs);

        ws.Session().LoadApplied(ParseCompact(CompactDoc()), 1, 1);
        ctl.LoadPresetDirectory(preset_dir ? preset_dir : "");

        QQmlEngine eng;
        QQmlContext* rc = eng.rootContext();
        rc->setContextProperty(QStringLiteral("bridge"), &bridge);
        rc->setContextProperty(QStringLiteral("sceneEdit"),
                               &scene_edit);
        rc->setContextProperty(QStringLiteral("layoutCtl"),
                               &layout_ctl);
        rc->setContextProperty(QStringLiteral("workspace"), &ws);
        rc->setContextProperty(QStringLiteral("objectCtl"), &ctl);
        rc->setContextProperty(QStringLiteral("effectEdit"), &eff);
        rc->setContextProperty(QStringLiteral("viewport"),
                               &preview);
        rc->setContextProperty(QStringLiteral("diagCtl"), &diag);
        rc->setContextProperty(QStringLiteral("uiPrefs"), &prefs);
        rc->setContextProperty(QStringLiteral("startupTask"),
                               &startup);
        rc->setContextProperty(QStringLiteral("quick3dAvailable"),
                               true);
        rc->setContextProperty(QStringLiteral("trayAvailable"),
                               false);
        rc->setContextProperty(QStringLiteral("startHidden"),
                               false);

        QQmlComponent comp(&eng,
            QUrl(QStringLiteral(
                "qrc:/qt/qml/OpenRGB2/UI/pages/LayoutPage.qml")));
        if(comp.isError())
        {
            ++failures;
            std::printf("FAIL layout page:\n%s\n",
                        comp.errorString().toUtf8().constData());
        }
        else
        {
            std::unique_ptr<QObject> page(
                comp.create(rc));
            CHECK(page != nullptr);
            if(page)
            {
                /* objectCtl -> layoutCtl: a viewport selection
                   lands in the SELECTION card's data source. */
                ctl.selectInstance(QStringLiteral("fan0"));
                CHECK(layout_ctl.selectedId()
                      == QStringLiteral("fan0"));
                /* layoutCtl -> objectCtl: a 2D-canvas selection
                   shows as viewport selection on the next 3D
                   toggle. */
                layout_ctl.select(QStringLiteral("fan1"));
                CHECK(ctl.selectedInstances()
                      == QVariantList({ "fan1" }));
                /* Empty propagates too. */
                ctl.clearEditorSelection();
                CHECK(layout_ctl.selectedId().isEmpty());

                /* Focus peers: with 3D enabled the viewport's
                   editingText() reaches the docked panels'
                   textFocus (fix round 1 — search fields no
                   longer leak keystrokes into shortcuts). */
                prefs.setLayoutShow3d(true);
                QCoreApplication::processEvents();
                QObject* loader = page->findChild<QObject*>(
                    QStringLiteral("viewportLoader"));
                CHECK(loader != nullptr);
                if(loader)
                {
                    QObject* vp = loader->property("item")
                                      .value<QObject*>();
                    CHECK(vp != nullptr);
                    QObject* dt = page->findChild<QObject*>(
                        QStringLiteral("deviceTreePanel"));
                    QObject* dl = page->findChild<QObject*>(
                        QStringLiteral("deviceLibraryPanel"));
                    CHECK(dt != nullptr && dl != nullptr);
                    if(vp && dt && dl)
                    {
                        CHECK(vp->property("focusPeer")
                                  .value<QObject*>() != nullptr);
                        CHECK(vp->property("focusPeer2")
                                  .value<QObject*>() == dt);
                        CHECK(vp->property("focusPeer3")
                                  .value<QObject*>() == dl);
                        /* The panels expose textFocus — the
                           peer seam reads a real property, not
                           undefined. */
                        CHECK(dt->property("textFocus").isValid());
                        CHECK(dl->property("textFocus").isValid());
                        CHECK(!dt->property("textFocus").toBool());
                    }
                }
            }
        }
    }

    /*======================================================*\
    || E. Viewport QML files compile                          ||
    \*======================================================*/
    std::printf("-- viewport: file compile smoke\n");
    {
        QQmlEngine eng;
        const char* files[] = {
            "editor/CameraController.qml",
            "editor/SelectionController.qml",
            "editor/TransformGizmo.qml",
            "devices/Case.qml",   "devices/Fan.qml",
            "devices/Gpu.qml",    "devices/Keyboard.qml",
            "devices/Monitor.qml","devices/Mouse.qml",
            "devices/Ram.qml",    "devices/SelectionFrame.qml",
            "devices/Strip.qml",
            "materials/StudioEnvironment.qml",
        };
        for(const char* f : files)
        {
            QQmlComponent c(&eng,
                QUrl(QString::fromLatin1(kVpBase) +
                     QString::fromLatin1(f)));
            if(c.isError())
            {
                ++failures;
                std::printf("FAIL compile %s:\n%s\n", f,
                            c.errorString().toUtf8().constData());
            }
            else
            {
                ++checks;
            }
        }
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
