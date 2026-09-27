// SPDX-License-Identifier: GPL-2.0-or-later
//
// QML smoke load — headless (offscreen) verification that the
// application shell and every page instantiate with NO engine
// running: the bridge sits in its disconnected state and each
// view must still render an honest degraded/offline surface.
//
//   - Main window + all four pages load error-free
//   - the Theme singleton resolves (design tokens reachable)
//   - Dashboard's honest empty-preview placeholder is visible
//     when no frame data exists

#include "EngineBridge.h"
#include "SceneEditController.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>

#include <cstdio>
#include <cstring>

using namespace openrgb2;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

int main(int argc, char** argv)
{
    /* No windowing needed — the whole point is loading QML
       without a display (or an engine). */
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QQmlEngine engine;
    ui::EngineBridge bridge;   /* never start()ed — offline */
    engine.rootContext()->setContextProperty(
        QStringLiteral("bridge"), &bridge);
    /* The Lighting page's edit session — empty baseline, so the
       page renders its honest "no scene" surfaces while every
       sceneEdit binding is live (the 4.2 controls). */
    ui::SceneEditController scene_edit(&bridge);
    engine.rootContext()->setContextProperty(
        QStringLiteral("sceneEdit"), &scene_edit);

    const QString base = QStringLiteral("qrc:/qt/qml/OpenRGB2/UI/");
    const char*   files[] = {
        "Main.qml",
        "pages/DashboardPage.qml",
        "pages/LightingPage.qml",
        "pages/LayoutPage.qml",
        "pages/DevicesPage.qml",
    };

    for(const char* f : files)
    {
        const QUrl url(base + QString::fromLatin1(f));
        QQmlComponent comp(&engine, url);
        if(comp.isError())
        {
            ++failures;
            std::printf("FAIL load %s:\n%s\n", f,
                        comp.errorString().toUtf8().constData());
            continue;
        }
        QObject* obj = comp.create(engine.rootContext());
        ++checks;
        if(!obj)
        {
            ++failures;
            std::printf("FAIL create %s:\n%s\n", f,
                        comp.errorString().toUtf8().constData());
            continue;
        }
        std::printf("  loaded %s\n", f);

        /* The Dashboard's honest empty state must be showing
           while there is no frame data at all. */
        if(std::strcmp(f, "pages/DashboardPage.qml") == 0)
        {
            QObject* empty =
                obj->findChild<QObject*>("previewEmpty");
            CHECK(empty != nullptr);
            if(empty)
            {
                CHECK(empty->property("visible").toBool());
            }
        }
        delete obj;
    }

    /* Theme singleton resolves inside the module. */
    {
        QQmlComponent comp(&engine);
        comp.setData(
            "import QtQuick\n"
            "import OpenRGB2.UI.theme\n"
            "Item {\n"
            "    property color a: Theme.accent\n"
            "    property int   s: Theme.spaceL\n"
            "}\n",
            QUrl());
        QObject* obj = comp.create(engine.rootContext());
        ++checks;
        if(!obj)
        {
            ++failures;
            std::printf("FAIL theme probe:\n%s\n",
                        comp.errorString().toUtf8().constData());
        }
        else
        {
            CHECK(obj->property("a").toString().startsWith("#"));
            CHECK(obj->property("s").toInt() > 0);
            delete obj;
        }
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
