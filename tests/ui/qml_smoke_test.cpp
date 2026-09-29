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

#include "DiagnosticsController.h"
#include "EngineBridge.h"
#include "LayoutController.h"
#include "SceneEditController.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQuickStyle>

#include <cmath>
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
    engine.rootContext()->setContextProperty(
        QStringLiteral("trayAvailable"), true);
    ui::EngineBridge bridge;   /* never start()ed — offline */
    engine.rootContext()->setContextProperty(
        QStringLiteral("bridge"), &bridge);
    /* The Lighting page's edit session — empty baseline, so the
       page renders its honest "no scene" surfaces while every
       sceneEdit binding is live (the 4.2 controls). */
    ui::SceneEditController scene_edit(&bridge);
    engine.rootContext()->setContextProperty(
        QStringLiteral("sceneEdit"), &scene_edit);
    /* Task 4.4: the Layout page's controller — empty model, so
       the page renders its honest no-scene surfaces. */
    ui::LayoutController layout_ctl(&bridge);
    engine.rootContext()->setContextProperty(
        QStringLiteral("layoutCtl"), &layout_ctl);
    /* Task 4.7: the Devices & Diagnostics controller —
       offline bridge, so every rate renders unmeasured. */
    ui::DiagnosticsController diag_ctl(&bridge);
    engine.rootContext()->setContextProperty(
        QStringLiteral("diagCtl"), &diag_ctl);

    const QString base = QStringLiteral("qrc:/qt/qml/OpenRGB2/UI/");
    const char*   files[] = {
        "Main.qml",
        "pages/DashboardPage.qml",
        "pages/LightingPage.qml",
        "pages/LayoutPage.qml",
        "pages/DevicesPage.qml",
        "pages/SettingsPage.qml",
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

        if(std::strcmp(f, "Main.qml") == 0)
        {
            auto* window = qobject_cast<QQuickWindow*>(obj);
            CHECK(window != nullptr);
            if(window)
            {
                CHECK(window->isVisible());
                CHECK(!window->close());
                CHECK(!window->isVisible());
                window->setProperty("exitRequested", true);
                CHECK(window->close());
            }
        }

        /* Card geometry — ui-first-look-2026-09-28 regression:
           a Card's content root must fill the card body. A
           Column/ColumnLayout root WITHOUT `width` falls back
           to implicitWidth, which resolves circularly through
           children bound to parent.width — the positioner then
           treats the zero-width children as unpositioned, the
           body collapses to one line, and the card paints its
           controls stacked on the title. */
        if(std::strncmp(f, "pages/", 6) == 0)
        {
            obj->setProperty("width", 1440);
            obj->setProperty("height", 1000);
            QCoreApplication::processEvents();
            const QList<QObject*> bodies =
                obj->findChildren<QObject*>("cardBody");
            CHECK(!bodies.isEmpty());
            for(QObject* b : bodies)
            {
                QQuickItem* body = qobject_cast<QQuickItem*>(b);
                QQuickItem* card =
                    body && body->parentItem()
                        ? body->parentItem()->parentItem()
                        : nullptr;
                if(!body || !card || !card->isVisible())
                    continue;
                /* A zero-width body is a headless artifact —
                   layout (RowLayout fillWidth) distribution is
                   polish-gated and never runs on a windowless
                   item. The defect signature this checks is
                   the opposite: a WIDE body whose container
                   content root collapsed to ~0 width. */
                if(body->width() <= 0)
                    continue;
                /* Only the WIDTH contract is asserted: a
                   positioner's implicitHeight is polish-gated
                   (unreliable on a windowless item), but the
                   defect signature — a container content root
                   collapsing to ~0 width inside a wide body —
                   resolves eagerly through the width binding. */
                for(QQuickItem* k : body->childItems())
                {
                    /* centered helpers (EmptyState) may sit
                       narrower; the CONTAINER content root is
                       the part that must span the card. */
                    const QString cn =
                        QString::fromLatin1(
                            k->metaObject()->className());
                    const bool isContainer =
                        cn.startsWith(QLatin1String("QQuickRow"))
                        || cn.startsWith(
                            QLatin1String("QQuickColumn"))
                        || cn.startsWith(
                            QLatin1String("QQuickGrid"))
                        || cn.startsWith(
                            QLatin1String("QQuickFlow"))
                        || cn.contains(QLatin1String("Layout"));
                    if(isContainer && k->isVisible())
                    {
                        CHECK(std::fabs(k->width() - body->width())
                              < 1.5);
                    }
                }
            }

            /* Layout page: the canvas instruction must live
               INSIDE the arrangement card's reserved area —
               anchored below a fixed-height canvas it painted
               past the card edge and the next card hid it. */
            if(std::strcmp(f, "pages/LayoutPage.qml") == 0)
            {
                QQuickItem* hint = qobject_cast<QQuickItem*>(
                    obj->findChild<QObject*>("canvasInstructions"));
                CHECK(hint != nullptr);
                if(hint && hint->parentItem())
                {
                    /* wrapper reserves the hint below the
                       ~430px canvas — not a fixed 430 total */
                    CHECK(hint->parentItem()->height()
                          >= hint->implicitHeight() + 400);
                }
            }
        }

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
