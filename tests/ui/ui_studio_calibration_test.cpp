// SPDX-License-Identifier: GPL-2.0-or-later
//
// ui_studio_calibration_test — studio-import Task 11 suite:
// the physical LED-calibration wizard.
//
//   Part A — the REAL CalibrationController offline (no
//   engine): honest refusals, one status shape for replies/
//   events, the FeedStatusForTesting session transitions,
//   owner vs. non-owner views, and empty diagnostics
//   pickers.
//
//   Part B — the adapted source tst_calibrator.qml cases,
//   headless: the REAL DevicePresetEditor + LedCalibrator
//   QML against plain-JS stubs (ctlOverride for the preset
//   document, hostOverride for the engine session). Every
//   source case is ported — see the per-test comments for
//   the tst_calibrator.qml parity note.
//
//   - compiles inside the editor, closed by default
//   - openForZone loads points + identity addresses; the
//     projection drops the constant axis (fan: Y)
//   - session start -> assign fast path -> advance ->
//     calibShow follows the lit LED; typed commit steals
//   - verify lights without assigning; unassigned dots
//     never write -1 while claiming a LED
//   - placeAt adds a point; applyMapping writes
//     points+addresses into the CANDIDATE only (draft body)
//   - out-of-range LED rejection (typed AND stale-file)
//   - close-while-starting cancels the pending begin
//   - draft-zone selection follow (selZoneIdx / candidate)
//   - shape regeneration arm + reset, generated shapes,
//     typed entry, remove, safe close without a session

#include "EngineBridge.h"
#include "studio/CalibrationController.h"

#include <QGuiApplication>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QSignalSpy>
#include <QVariantMap>

#include <cmath>
#include <cstdio>

using namespace openrgb2;
using namespace openrgb2::ui;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

/* The source fan preset — four saved points on a fan face
   (constant Y) with the identity address map. */
static QJSValue FanDoc(QQmlEngine& e, int extra_zone = 0)
{
    return e.evaluate(QStringLiteral(R"JS(
        (function(){
            var d = {
                id: "fan-120", name: "120 mm Fan", category: "fan",
                schema_version: 1,
                entities: {
                    body: { geometry: "fan_body",
                            size_m: [0.12, 0.025, 0.12],
                            zone: "ring" }
                },
                zones: [ { id: "ring", entity: "body",
                           led_count: 4,
                           layout: { type: "points",
                               points: [ [-0.05, 0.016,  0.00],
                                         [ 0.00, 0.016,  0.05],
                                         [ 0.05, 0.016,  0.00],
                                         [ 0.00, 0.016, -0.05] ],
                               addresses: [0, 1, 2, 3] } } ],
                binding_hints: []
            };
            if (%1) {
                d.zones.push({ id: "ring2", entity: "body",
                               led_count: 2,
                               layout: { type: "points",
                                   points: [ [0.1, 0.016, 0],
                                             [-0.1, 0.016, 0] ],
                                   addresses: [5, 6] } });
            }
            return d;
        })()
    )JS").arg(extra_zone));
}

/* The editor's controller stub — presetDocument answers the
   fan document; everything else records calls. */
static QJSValue CtlStub(QQmlEngine& e, int extra_zone = 0)
{
    return e.evaluate(QStringLiteral(R"JS(
        (function(){
            var doc = {
                id: "fan-120", name: "120 mm Fan", category: "fan",
                schema_version: 1,
                entities: {
                    body: { geometry: "fan_body",
                            size_m: [0.12, 0.025, 0.12],
                            zone: "ring" }
                },
                zones: [ { id: "ring", entity: "body",
                           led_count: 4,
                           layout: { type: "points",
                               points: [ [-0.05, 0.016,  0.00],
                                         [ 0.00, 0.016,  0.05],
                                         [ 0.05, 0.016,  0.00],
                                         [ 0.00, 0.016, -0.05] ],
                               addresses: [0, 1, 2, 3] } } ],
                binding_hints: []
            };
            if (%1) {
                doc.zones.push({ id: "ring2", entity: "body",
                                 led_count: 2,
                                 layout: { type: "points",
                                     points: [ [0.1, 0.016, 0],
                                               [-0.1, 0.016, 0] ],
                                     addresses: [5, 6] } });
            }
            return {
                presetDocument: function(tid) {
                    return tid === "fan-120"
                        ? { exists: true, packaged: false,
                            instances: 1, doc: doc }
                        : { exists: false };
                },
                fetchPresetDocument: function(tid) {},
                validatePreset: function(c) {
                    return { ok: true, errors: [] }; },
                previewPreset: function(c) {
                    return { ok: true, errors: [], objects: [] }; },
                savePresetType: function(c, ow) {
                    return { ok: true, pending: true }; },
                deletePresetType: function(tid) {
                    return { ok: true }; },
            };
        })()
    )JS").arg(extra_zone));
}

/* The engine-session stub — source-parity surface
   (calibBegin/calibShow/calibEnd/calibInfo + diag pickers).
   `act` flips the session between pending and active;
   `leds` is the zone's hardware count. */
static QJSValue HostStub(QQmlEngine& e)
{
    return e.evaluate(R"JS(
        (function(){
            return {
                act: false,
                paused: false,
                leds: 4,
                begun: null,
                shown: [],
                ended: 0,
                calibBegin: function(c, z) {
                    this.begun = [c, z]
                    return { ok: true, pending: true }
                },
                calibShow: function(l) { this.shown.push(l) },
                calibEnd: function() { this.ended++ },
                calibInfo: function() {
                    return { active: this.act,
                             starting: !this.act && this.begun !== null,
                             paused: this.paused,
                             controller: 0, zone: 0,
                             leds: this.leds,
                             message: "" }
                },
                diagControllers: function() {
                    return [ { index: 0, label: "[0] TestCtrl" } ]
                },
                diagZones: function(c) {
                    return [ { index: 0, leds: 4,
                               label: "0: Z (4 LEDs)" } ]
                },
                diagRefresh: function() {},
            };
        })()
    )JS");
}

/* Build the real editor headless: ctl stub + host stub fed
   through the editor/calibrator override seams. */
static QObject* MakeEd(QQmlEngine& e, const QJSValue& ctl,
                       const QJSValue& host,
                       const QString& base)
{
    QQmlComponent comp(&e,
        QUrl(base + QStringLiteral("DevicePresetEditor.qml")));
    if(comp.isError())
    {
        ++failures;
        std::printf("FAIL load editor:\n%s\n",
                    comp.errorString().toUtf8().constData());
        return nullptr;
    }
    QObject* ed = comp.create(e.rootContext());
    ++checks;
    if(!ed)
    {
        ++failures;
        std::printf("FAIL create editor:\n%s\n",
                    comp.errorString().toUtf8().constData());
        return nullptr;
    }
    ed->setProperty("width", 940);
    ed->setProperty("height", 640);
    ed->setProperty("ctlOverride", QVariant::fromValue(ctl));
    /* Load the fan document through the same function the
       editor's open path uses — the calibrator's candidate
       view lands identical. */
    QMetaObject::invokeMethod(
        ed, "applyDoc",
        Q_ARG(QVariant, ctl.property("presetDocument")
                        .call({ QStringLiteral("fan-120") })
                        .toVariant().toMap().value("doc")),
        Q_ARG(QVariant, false), Q_ARG(QVariant, 1));
    if(host.isObject())
    {
        QObject* cal = ed->property("calibrator")
                           .value<QObject*>();
        if(cal != nullptr)
        {
            cal->setProperty("hostOverride",
                             QVariant::fromValue(host));
        }
    }
    return ed;
}

static QVariant Call(QObject* o, const char* fn,
                     const QVariant& a1 = QVariant(),
                     const QVariant& a2 = QVariant())
{
    QVariant r;
    const bool two = a2.isValid(), one = a1.isValid() || two;
    bool ok;
    if(two)
        ok = QMetaObject::invokeMethod(
            o, fn, Q_RETURN_ARG(QVariant, r),
            Q_ARG(QVariant, a1), Q_ARG(QVariant, a2));
    else if(one)
        ok = QMetaObject::invokeMethod(
            o, fn, Q_RETURN_ARG(QVariant, r),
            Q_ARG(QVariant, a1));
    else
        ok = QMetaObject::invokeMethod(
            o, fn, Q_RETURN_ARG(QVariant, r));
    CHECK(ok);
    return r;
}

/* QML functions register their FULL parameter list in the
   metaobject — a JS-optional third arg still needs a third
   Q_ARG or invokeMethod reports "no such method". */
static QVariant Call3(QObject* o, const char* fn,
                      const QVariant& a1, const QVariant& a2,
                      const QVariant& a3)
{
    QVariant r;
    const bool ok = QMetaObject::invokeMethod(
        o, fn, Q_RETURN_ARG(QVariant, r),
        Q_ARG(QVariant, a1), Q_ARG(QVariant, a2),
        Q_ARG(QVariant, a3));
    CHECK(ok);
    return r;
}

/* JS arrays land as QVariantList of double — compare
   element-wise as ints, never QVariant::operator==. */
static bool IntsEq(const QVariantList& a,
                   std::initializer_list<int> v)
{
    if((size_t)a.size() != v.size())
    {
        return false;
    }
    size_t i = 0;
    for(int x : v)
    {
        if(a[(qsizetype)i].toInt() != x)
        {
            return false;
        }
        ++i;
    }
    return true;
}

static QVariantList Assign(QObject* cal)
{
    return cal->property("assign").toList();
}
static bool AssignEq(QObject* cal, std::initializer_list<int> v)
{
    const QVariantList a = Assign(cal);
    if((size_t)a.size() != v.size())
    {
        return false;
    }
    size_t i = 0;
    for(int x : v)
    {
        if(a[(qsizetype)i].toInt() != x)
        {
            return false;
        }
        ++i;
    }
    return true;
}
static void SetAssign(QObject* cal, int i, int v)
{
    QVariantList a = Assign(cal);
    if(i >= 0 && i < a.size())
    {
        a[i] = v;
        cal->setProperty("assign", a);
    }
}
static bool Near(double a, double b, double eps = 1e-6)
{
    return std::fabs(a - b) < eps;
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    /*======================================================*\
    || A. CalibrationController offline (real object)       ||
    \*======================================================*/
    std::printf("-- calibration: controller offline surface\n");
    {
        EngineBridge bridge;
        CalibrationController ctl(&bridge);
        /* No engine: begin refuses honestly, the pickers are
           empty, show/end are safe no-ops. */
        const QVariantMap begin = ctl.calibBegin(0, 0);
        CHECK(!begin.value("ok").toBool());
        CHECK(begin.value("error").toString().contains(
                  "connected", Qt::CaseInsensitive));
        CHECK(ctl.diagControllers().isEmpty());
        CHECK(ctl.diagZones(0).isEmpty());
        const QVariantMap info0 = ctl.calibInfo();
        CHECK(!info0.value("active").toBool());
        CHECK(!info0.value("owner").toBool());
        CHECK(info0.value("leds").toInt() == 0);
        ctl.calibShow(0);
        ctl.calibEnd();

        /* Status feeds: one shape drives the whole session
           view — starting -> active -> terminal clears the
           token and the LEDs. */
        QSignalSpy spy(&ctl,
                       &CalibrationController::calibrationChanged);
        ctl.FeedStatusForTesting(QVariantMap({
            { "starting", true }, { "active", false },
            { "owner", false },   { "token", "cal-7" } }));
        CHECK(ctl.starting());
        CHECK(!ctl.active());
        ctl.FeedStatusForTesting(QVariantMap({
            { "active", true }, { "starting", false },
            { "owner", false }, { "token", "cal-7" },
            { "led_count", 8 }, { "zone", "Fan 1" },
            { "controller_name", "SL Hub" },
            { "last_shown", -1 }, { "frames_written", 2 },
            { "generation", 4 } }));
        CHECK(ctl.active());
        CHECK(ctl.leds() == 8);
        CHECK(ctl.zoneName() == "Fan 1");
        CHECK(ctl.controllerName() == "SL Hub");
        CHECK(ctl.lastShown() == -1);
        ctl.FeedStatusForTesting(QVariantMap({
            { "active", true }, { "owner", false },
            { "last_shown", 3 }, { "led_count", 8 } }));
        CHECK(ctl.lastShown() == 3);
        /* Terminal status clears the lease material — a
           stale show can never quote a dead token. */
        ctl.FeedStatusForTesting(QVariantMap({
            { "active", false }, { "starting", false },
            { "ending", false }, { "owner", false },
            { "message", "session ended" } }));
        CHECK(!ctl.active());
        CHECK(ctl.message() == "session ended");
        /* A session SOMEONE ELSE holds: owner:true and no
           token — the wizard's busy affordance. */
        ctl.FeedStatusForTesting(QVariantMap({
            { "active", false }, { "owner", true },
            { "message", "held by another client" } }));
        CHECK(ctl.ownerBusy());
        CHECK(!ctl.active());
        CHECK(spy.count() >= 5);
        const QVariantMap info = ctl.calibInfo();
        CHECK(info.value("paused").isValid());
        CHECK(info.contains("frames_written"));
    }

    /*======================================================*\
    || B. QML wizard — the adapted tst_calibrator.qml suite ||
    \*======================================================*/
    QQmlEngine engine;
    const QString base =
        QStringLiteral("qrc:/qt/qml/OpenRGB2/UI/studio/");

    auto make_ed = [&](const QJSValue& host,
                       int extra_zone = 0) -> QObject*
    {
        const QJSValue ctl = CtlStub(engine, extra_zone);
        return MakeEd(engine, ctl, host, base);
    };

    std::printf("-- qml: compiles inside the editor\n");
    {
        QObject* ed = make_ed(QJSValue());
        if(ed != nullptr)
        {
            QObject* cal =
                ed->property("calibrator").value<QObject*>();
            CHECK(cal != nullptr);
            if(cal)
            {
                CHECK(cal->property("visible").toBool()
                      == false);
            }
            ed->deleteLater();
        }
    }

    /* tst_calibrator.qml::test_open_loads_points_and_
       projection — plus the draft-zone picker wiring the
       OpenRGB2 editor needs (JSON-body zones, not a zone
       list). */
    std::printf("-- qml: openForZone loads + projects\n");
    {
        QJSValue host = HostStub(engine);
        QObject* ed = make_ed(host);
        if(ed != nullptr)
        {
            QObject* cal =
                ed->property("calibrator").value<QObject*>();
            Call(cal, "openForZone", QVariant::fromValue(ed),
                 0);
            CHECK(cal->property("zoneId").toString() == "ring");
            CHECK(cal->property("pts").toList().size() == 4);
            CHECK(AssignEq(cal, { 0, 1, 2, 3 }));
            /* Constant Y (0.016) drops — the face is X/Z. */
            CHECK(cal->property("projU").toInt() == 0);
            CHECK(cal->property("projV").toInt() == 2);
            CHECK(Near(cal->property("faceVal").toDouble(),
                       0.016));
            /* The panel's draft-zone row lists the
               candidate's zones (OpenRGB2 seam). */
            CHECK(cal->property("zoneIndex").toInt() == 0);
            Call(cal, "close");
            ed->deleteLater();
        }
    }

    /* tst_calibrator.qml::test_assign_dot_maps_and_advances —
       session start, fast-path assign, steal via the box. */
    std::printf("-- qml: assign + advance + steal\n");
    {
        QJSValue host = HostStub(engine);
        QObject* ed = make_ed(host);
        if(ed != nullptr)
        {
            QObject* cal =
                ed->property("calibrator").value<QObject*>();
            Call(cal, "openForZone", QVariant::fromValue(ed),
                 0);
            Call(cal, "startSessionOn", 0, 0);
            CHECK(IntsEq(host.property("begun")
                                .toVariant().toList(), { 0, 0 }));
            host.setProperty("act", true);
            Call(cal, "syncSession");
            CHECK(cal->property("sessionActive").toBool());
            CHECK(cal->property("hwLeds").toInt() == 4);
            CHECK(cal->property("curLed").toInt() == 0);
            CHECK(IntsEq(host.property("shown")
                                .toVariant().toList(), { 0 }));

            Call(cal, "clearAssign");
            CHECK(AssignEq(cal, { -1, -1, -1, -1 }));

            Call(cal, "assignDot", 2);
            CHECK(Assign(cal)[2].toInt() == 0);
            CHECK(cal->property("selDot").toInt() == -1);
            CHECK(cal->property("curLed").toInt() == 1);
            CHECK(host.property("shown").toVariant().toList()
                  .last().toInt() == 1);

            Call(cal, "assignDot", 0);
            CHECK(Assign(cal)[0].toInt() == 1);
            CHECK(cal->property("curLed").toInt() == 2);

            /* Mapped dot -> box edit; stealing clears the
               first holder. */
            Call(cal, "assignDot", 2);
            CHECK(cal->property("selDot").toInt() == 2);
            CHECK(cal->property("ledText").toString() == "0");
            cal->setProperty("ledText", "1");
            Call(cal, "commitField");
            CHECK(Assign(cal)[2].toInt() == 1);
            CHECK(Assign(cal)[0].toInt() == -1);
            CHECK(cal->property("selDot").toInt() == -1);

            Call(cal, "close");
            CHECK(host.property("ended").toInt() == 1);
            ed->deleteLater();
        }
    }

    /* tst_calibrator.qml::test_verify_lights_without_
       assigning + test_verify_skips_unassigned. */
    std::printf("-- qml: verify mode\n");
    {
        QJSValue host = HostStub(engine);
        QObject* ed = make_ed(host);
        if(ed != nullptr)
        {
            QObject* cal =
                ed->property("calibrator").value<QObject*>();
            Call(cal, "openForZone", QVariant::fromValue(ed),
                 0);
            Call(cal, "startSessionOn", 0, 0);
            host.setProperty("act", true);
            Call(cal, "syncSession");
            cal->setProperty("verify", true);
            host.setProperty("shown",
                              engine.toScriptValue(QVariantList()));
            Call(cal, "assignDot", 1);         /* light its map */
            CHECK(IntsEq(host.property("shown")
                                .toVariant().toList(), { 1 }));
            CHECK(AssignEq(cal, { 0, 1, 2, 3 }));
            /* Unassigned dots NEVER write — a -1 would be
               "all dark" while the UI claimed a lit LED. */
            Call(cal, "clearAssign");
            host.setProperty("shown",
                              engine.toScriptValue(QVariantList()));
            Call(cal, "assignDot", 1);
            CHECK(host.property("shown").toVariant().toList()
                  .isEmpty());
            CHECK(cal->property("statusLine").toString()
                  .contains("unassigned"));
            Call(cal, "close");
            ed->deleteLater();
        }
    }

    /* tst_calibrator.qml::test_place_adds_point +
       test_apply_writes_layout (candidate + draft body). */
    std::printf("-- qml: place + apply\n");
    {
        QJSValue host = HostStub(engine);
        QObject* ed = make_ed(host);
        if(ed != nullptr)
        {
            QObject* cal =
                ed->property("calibrator").value<QObject*>();
            Call(cal, "openForZone", QVariant::fromValue(ed),
                 0);
            Call(cal, "startSessionOn", 0, 0);
            host.setProperty("act", true);
            Call(cal, "syncSession");
            Call(cal, "clearAssign");
            cal->setProperty("place", true);
            const int n0 = cal->property("pts").toList().size();
            Call(cal, "placeAt", 5, 5);
            CHECK(cal->property("pts").toList().size() == n0 + 1);
            CHECK(Assign(cal)[n0].toInt() == 0);
            const QVariantList p =
                cal->property("pts").toList()[n0].toList();
            CHECK(Near(p[1].toDouble(), 0.016));
            /* Idle place lands unassigned. */
            Call(cal, "close");
            Call(cal, "openForZone", QVariant::fromValue(ed),
                 0);
            /* Reopen reloads the SAVED layout — the placed
               point was never applied. */
            CHECK(cal->property("pts").toList().size() == 4);
            /* Apply writes the candidate zone — points +
               addresses + synced led_count. */
            Call(cal, "clearAssign");
            SetAssign(cal, 0, 3);
            SetAssign(cal, 1, 2);
            Call(cal, "applyMapping");
            const QVariantMap z =
                ed->property("candidate").toMap()
                  .value("zones").toList()[0].toMap();
            const QVariantMap lay = z.value("layout").toMap();
            CHECK(lay.value("type").toString() == "points");
            CHECK(IntsEq(lay.value("addresses").toList(),
                            { 3, 2, -1, -1 }));
            CHECK(z.value("led_count").toInt() == 4);
            /* touch() serialized the mutation into the draft
               body — Save persists it like any edit. */
            QObject* bodyf =
                ed->findChild<QObject*>("presetBodyField");
            CHECK(bodyf != nullptr);
            if(bodyf)
            {
                CHECK(bodyf->property("text").toString()
                      .contains("addresses"));
                CHECK(bodyf->property("text").toString()
                      .contains("led_count"));
            }
            Call(cal, "close");
            ed->deleteLater();
        }
    }

    /* tst_calibrator.qml::test_out_of_range_led_rejected. */
    std::printf("-- qml: out-of-range LED rejected\n");
    {
        QJSValue host = HostStub(engine);
        QObject* ed = make_ed(host);
        if(ed != nullptr)
        {
            QObject* cal =
                ed->property("calibrator").value<QObject*>();
            Call(cal, "openForZone", QVariant::fromValue(ed),
                 0);
            Call(cal, "startSessionOn", 0, 0);
            host.setProperty("act", true);
            Call(cal, "syncSession");
            CHECK(cal->property("hwLeds").toInt() == 4);
            Call(cal, "assignDot", 1);
            CHECK(cal->property("selDot").toInt() == 1);
            cal->setProperty("ledText", "4");   /* at bound   */
            Call(cal, "commitField");
            CHECK(Assign(cal)[1].toInt() == 1);
            CHECK(cal->property("selDot").toInt() == 1);
            CHECK(cal->property("statusLine").toString()
                  .contains("out of range"));
            cal->setProperty("ledText", "512");
            Call(cal, "commitField");
            CHECK(Assign(cal)[1].toInt() == 1);
            cal->setProperty("ledText", "3");   /* last valid */
            Call(cal, "commitField");
            CHECK(Assign(cal)[1].toInt() == 3);
            CHECK(cal->property("selDot").toInt() == -1);
            /* A stale-file address past the bound blocks
               Apply too. */
            SetAssign(cal, 2, 9);
            Call(cal, "applyMapping");
            const QVariantMap z =
                ed->property("candidate").toMap()
                  .value("zones").toList()[0].toMap();
            CHECK(IntsEq(z.value("layout").toMap()
                                .value("addresses").toList(),
                            { 0, 1, 2, 3 }));
            Call(cal, "unassign", 2);
            Call(cal, "applyMapping");
            const QVariantMap z2 =
                ed->property("candidate").toMap()
                  .value("zones").toList()[0].toMap();
            CHECK(IntsEq(z2.value("layout").toMap()
                                .value("addresses").toList(),
                            { 0, 3, -1, -1 }));
            Call(cal, "close");
            ed->deleteLater();
        }
    }

    /* tst_calibrator.qml::test_close_while_starting_
       cancels — the pending begin must reach calibEnd. */
    std::printf("-- qml: close while starting cancels\n");
    {
        QJSValue host = HostStub(engine);
        QObject* ed = make_ed(host);
        if(ed != nullptr)
        {
            QObject* cal =
                ed->property("calibrator").value<QObject*>();
            Call(cal, "openForZone", QVariant::fromValue(ed),
                 0);
            Call(cal, "startSessionOn", 0, 0);
            CHECK(cal->property("sessionStarting").toBool());
            CHECK(!cal->property("sessionActive").toBool());
            CHECK(host.property("ended").toInt() == 0);
            Call(cal, "close");
            CHECK(host.property("ended").toInt() == 1);
            ed->deleteLater();
        }
    }

    /* tst_calibrator.qml::test_shape_selector_resets_on_
       reopen + test_make_confirms_replace. */
    std::printf("-- qml: shape selector + confirm\n");
    {
        QJSValue host = HostStub(engine);
        QObject* ed = make_ed(host);
        if(ed != nullptr)
        {
            QObject* cal =
                ed->property("calibrator").value<QObject*>();
            Call(cal, "openForZone", QVariant::fromValue(ed),
                 0);
            cal->setProperty("shapeIndex", 3);    /* strip */
            Call3(cal, "generateShape", 3, 4, 0);
            Call(cal, "close");
            Call(cal, "openForZone", QVariant::fromValue(ed),
                 0);
            CHECK(cal->property("shapeIndex").toInt() == 0);
            CHECK(cal->property("pts").toList().size() == 4);
            /* Make over a non-empty layout arms first. */
            cal->setProperty("shapeIndex", 3);
            Call(cal, "requestMake");
            CHECK(cal->property("pts").toList().size() == 4);
            CHECK(AssignEq(cal, { 0, 1, 2, 3 }));
            CHECK(cal->property("confirmMake").toBool());
            Call(cal, "requestMake");
            CHECK(!cal->property("confirmMake").toBool());
            CHECK(AssignEq(cal, { -1, -1, -1, -1 }));
            CHECK(cal->property("layoutDirty").toBool());
            Call(cal, "close");
            ed->deleteLater();
        }
    }

    /* tst_calibrator.qml::test_generate_shapes. */
    std::printf("-- qml: generated shapes\n");
    {
        QJSValue host = HostStub(engine);
        QObject* ed = make_ed(host);
        if(ed != nullptr)
        {
            QObject* cal =
                ed->property("calibrator").value<QObject*>();
            Call(cal, "openForZone", QVariant::fromValue(ed),
                 0);
            Call3(cal, "generateShape", 1, 6, 0);     /* ring  */
            const QVariantList pts =
                cal->property("pts").toList();
            CHECK(pts.size() == 6);
            CHECK(Assign(cal).size() == 6);
            for(const QVariant& pv : pts)
            {
                const QVariantList p = pv.toList();
                CHECK(Near(p[1].toDouble(), 0.016));
                CHECK(Near(std::sqrt(p[0].toDouble() *
                                     p[0].toDouble() +
                                     p[2].toDouble() *
                                     p[2].toDouble()),
                           0.05));
            }
            /* Index 0 at the top (v max, u ~0). */
            CHECK(Near(pts[0].toList()[0].toDouble(), 0));
            CHECK(Near(pts[0].toList()[2].toDouble(), 0.05));
            Call3(cal, "generateShape", 3, 4, 0);     /* strip */
            const QVariantList sp =
                cal->property("pts").toList();
            CHECK(Near(sp[0].toList()[0].toDouble(), -0.05));
            CHECK(Near(sp[3].toList()[0].toDouble(), 0.05));
            CHECK(Near(sp[0].toList()[2].toDouble(), 0));
            Call(cal, "assignIdentity");
            CHECK(AssignEq(cal, { 0, 1, 2, 3 }));
            Call(cal, "close");
            ed->deleteLater();
        }
    }

    /* tst_calibrator.qml::test_follows_zone_selection — the
       OpenRGB2 editor's selZoneIdx seam + the candidate
       change (new document -> same-zone reload). */
    std::printf("-- qml: follows draft-zone selection\n");
    {
        QJSValue host = HostStub(engine);
        QObject* ed = make_ed(host, /*extra_zone*/1);
        if(ed != nullptr)
        {
            QObject* cal =
                ed->property("calibrator").value<QObject*>();
            Call(cal, "openForZone", QVariant::fromValue(ed),
                 0);
            CHECK(cal->property("pts").toList().size() == 4);
            ed->setProperty("selZoneIdx", 1);
            CHECK(cal->property("zoneId").toString()
                  == "ring2");
            CHECK(cal->property("pts").toList().size() == 2);
            CHECK(AssignEq(cal, { 5, 6 }));
            ed->setProperty("selZoneIdx", 0);
            CHECK(cal->property("pts").toList().size() == 4);
            /* A NEW candidate document reloads the open
               panel on the same zone index. */
            QMetaObject::invokeMethod(
                ed, "applyDoc",
                Q_ARG(QVariant,
                      FanDoc(engine, 1).toVariant()),
                Q_ARG(QVariant, false),
                Q_ARG(QVariant, 1));
            CHECK(cal->property("zoneId").toString() == "ring");
            Call(cal, "close");
            ed->deleteLater();
        }
    }

    /* tst_calibrator.qml::test_typed_entry_and_remove +
       test_end_without_session_is_safe. */
    std::printf("-- qml: typed entry + safe close\n");
    {
        QJSValue host = HostStub(engine);
        QObject* ed = make_ed(host);
        if(ed != nullptr)
        {
            QObject* cal =
                ed->property("calibrator").value<QObject*>();
            Call(cal, "openForZone", QVariant::fromValue(ed),
                 0);
            /* No session: click selects, the box commits,
               duplicates steal. */
            Call(cal, "clearAssign");
            Call(cal, "assignDot", 1);
            CHECK(cal->property("selDot").toInt() == 1);
            cal->setProperty("ledText", "2");
            Call(cal, "commitField");
            CHECK(Assign(cal)[1].toInt() == 2);
            CHECK(cal->property("selDot").toInt() == -1);
            Call(cal, "setLedAt", 3, 2);        /* steal */
            CHECK(Assign(cal)[3].toInt() == 2);
            CHECK(Assign(cal)[1].toInt() == -1);
            Call(cal, "removeDot", 3);
            CHECK(cal->property("pts").toList().size() == 3);
            CHECK(Assign(cal).size() == 3);
            Call(cal, "close");
            /* Closing without a session never calls end. */
            CHECK(host.property("ended").toInt() == 0);
            ed->deleteLater();
        }
    }

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
