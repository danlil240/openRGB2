// SPDX-License-Identifier: GPL-2.0-or-later
//
// Audit Q-11 — Lighting controls must not drift from the state
// they mirror. Loads the REAL LightingPage.qml headless against
// notifying QtObject stubs for `bridge` and `sceneEdit` (only the
// surface these controls read):
//
//   1. A reactive-input checkbox click sends set_enabled, then
//      shows ENGINE truth again — a click the engine has not
//      (yet) acted on must not leave the box claiming "on".
//      When the engine reports the input running, it follows.
//   2. The uploaded-preset colour well and its H/S/V sliders
//      show the tracked colour (sceneEdit.paramColor) — they
//      used to start at red and never read it.
//
// CHECK-macro main like the other UI suites — no framework.

#include <QColor>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>

#include <cmath>
#include <cstdio>
#include <memory>

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

/* Repeater delegates are item-tree children, not QObject
   children — walk the item tree. */
static QQuickItem* FindItem(QQuickItem* root, const QString& name)
{
    if(root == nullptr)
    {
        return nullptr;
    }
    if(root->objectName() == name)
    {
        return root;
    }
    for(QQuickItem* c : root->childItems())
    {
        if(QQuickItem* hit = FindItem(c, name))
        {
            return hit;
        }
    }
    return nullptr;
}

static std::unique_ptr<QObject> Make(QQmlEngine& eng, const char* qml)
{
    QQmlComponent c(&eng);
    c.setData(qml, QUrl());
    if(c.isError())
    {
        std::printf("stub error: %s\n",
                    c.errorString().toUtf8().constData());
        return nullptr;
    }
    return std::unique_ptr<QObject>(c.create());
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QQmlEngine eng;
    std::unique_ptr<QObject> bridge = Make(eng, R"QML(
        import QtQuick
        QtObject {
            objectName: "bridgeStub"
            property bool connected: true
            property var playbackView: ({ "requested_mode": "animation_playback",
                                          "applied_mode": "animation_playback",
                                          "phase": "active" })
            property var reactive: ({ "available": true,
                "inputs": [ { "id": "audio", "available": true,
                              "enabled": false, "running": false } ] })
            property var snapshots: []
            property var frameStatus: ({})
            property var frameAggregate: null
            property int commands: 0
            property var lastArgs: null
            function reactiveCommand(verb, args) {
                commands++; lastArgs = args; return 1 }
        }
    )QML");
    std::unique_ptr<QObject> scene = Make(eng, R"QML(
        import QtQuick
        QtObject {
            objectName: "sceneStub"
            property bool hasWorking: true
            property bool hasScene: true
            property var presetSupport: ({ "needs": "audio",
                "playback": true, "color": true,
                "direction": true, "speed": true })
            property var workingDocument: ({})
            property string paramColor: "#00FF00"
            property int paramSpeed: 100
            property bool paramReverse: false
            property string sceneName: "desk"
            property bool dirty: false
            property bool conflict: false
            property bool saving: false
            property string saveError: ""
            property string saveErrorSource: ""
            property var profiles: []
            function noteParamTouched(f) {}
            function sendPlaybackParams(s, r, c) { return 1 }
        }
    )QML");
    CHECK(bridge != nullptr && scene != nullptr);
    if(!bridge || !scene)
    {
        std::printf("%d checks, %d failures\n", checks, failures);
        return 1;
    }
    eng.rootContext()->setContextProperty(QStringLiteral("bridge"),
                                          bridge.get());
    eng.rootContext()->setContextProperty(QStringLiteral("sceneEdit"),
                                          scene.get());

    QQmlComponent comp(&eng, QUrl(QStringLiteral(
        "qrc:/qt/qml/OpenRGB2/UI/pages/LightingPage.qml")));
    if(comp.isError())
    {
        ++failures;
        std::printf("FAIL load LightingPage.qml:\n%s\n",
                    comp.errorString().toUtf8().constData());
        std::printf("%d checks, %d failures\n", checks, failures);
        return 1;
    }
    std::unique_ptr<QObject> page(comp.create(eng.rootContext()));
    auto* root = qobject_cast<QQuickItem*>(page.get());
    CHECK(root != nullptr);
    if(root == nullptr)
    {
        std::printf("%d checks, %d failures\n", checks, failures);
        return 1;
    }
    root->setWidth(1200);
    root->setHeight(900);
    QCoreApplication::processEvents();

    /*---- 1. reactive checkbox re-asserts engine truth -------*/
    std::printf("-- reactive checkbox follows the engine\n");
    {
        QQuickItem* cb = FindItem(root, QStringLiteral("audioCheck"));
        CHECK(cb != nullptr);
        if(cb != nullptr)
        {
            CHECK(cb->isEnabled());
            CHECK(!cb->property("checked").toBool());
            /* what a real click does: toggle, then clicked() */
            QMetaObject::invokeMethod(cb, "toggle");
            QMetaObject::invokeMethod(cb, "clicked");
            QCoreApplication::processEvents();
            CHECK(bridge->property("commands").toInt() == 1);
            CHECK(bridge->property("lastArgs").toMap()
                      .value(QStringLiteral("enabled")).toBool());
            /* the engine has not acted: still off on screen */
            CHECK(!cb->property("checked").toBool());

            /* the engine reports it running -> on */
            QVariantMap in;
            in[QStringLiteral("id")]        = QStringLiteral("audio");
            in[QStringLiteral("available")] = true;
            in[QStringLiteral("enabled")]   = true;
            in[QStringLiteral("running")]   = true;
            QVariantMap rs;
            rs[QStringLiteral("available")] = true;
            rs[QStringLiteral("inputs")]    = QVariantList{ in };
            bridge->setProperty("reactive", rs);
            QCoreApplication::processEvents();
            CHECK(cb->property("checked").toBool());

            /* a second click (turn off) the engine refuses:
               the box goes back to on */
            QMetaObject::invokeMethod(cb, "toggle");
            QMetaObject::invokeMethod(cb, "clicked");
            QCoreApplication::processEvents();
            CHECK(bridge->property("commands").toInt() == 2);
            CHECK(cb->property("checked").toBool());
        }
    }

    /*---- 2. uploaded-preset colour follows paramColor -------*/
    std::printf("-- uploaded-preset colour well shows paramColor\n");
    {
        QQuickItem* h = FindItem(root, QStringLiteral("upHue"));
        QQuickItem* s = FindItem(root, QStringLiteral("upSat"));
        QQuickItem* v = FindItem(root, QStringLiteral("upVal"));
        CHECK(h && s && v);
        if(h && s && v)
        {
            /* #00FF00: hue 1/3, full saturation and value */
            CHECK(std::fabs(h->property("value").toDouble()
                            - 1.0 / 3.0) < 0.01);
            CHECK(std::fabs(s->property("value").toDouble() - 1.0)
                  < 0.01);
            CHECK(std::fabs(v->property("value").toDouble() - 1.0)
                  < 0.01);

            /* a restored/changed colour lands too */
            scene->setProperty("paramColor",
                               QStringLiteral("#000080"));
            QCoreApplication::processEvents();
            CHECK(std::fabs(h->property("value").toDouble()
                            - 2.0 / 3.0) < 0.01);
            CHECK(std::fabs(v->property("value").toDouble()
                            - 128.0 / 255.0) < 0.01);
        }
    }

    std::printf("qml_lighting_state_test: %d checks, %d failures\n",
                checks, failures);
    return failures ? 1 : 0;
}
