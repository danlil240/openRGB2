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
#include <QJSValue>
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

/* Repeater delegates are not QObject children of the page —
   walk the visual item tree instead of findChild. */
static QObject* FindItem(QObject* root, const QString& name)
{
    QQuickItem* it = qobject_cast<QQuickItem*>(root);
    if(!it)
    {
        return nullptr;
    }
    if(it->objectName() == name)
    {
        return it;
    }
    for(QQuickItem* k : it->childItems())
    {
        if(QObject* hit = FindItem(k, name))
        {
            return hit;
        }
    }
    return nullptr;
}

/* A QML function's JS-object return arrives as a QJSValue. */
static QVariantMap AsMap(const QVariant& v)
{
    if(v.metaType() == QMetaType::fromType<QJSValue>())
    {
        return v.value<QJSValue>().toVariant().toMap();
    }
    return v.toMap();
}

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
                /* R-10: the shell banner is wired to the bridge's
                   ownership store, stays down while the engine is
                   not connected (the report may be stale), and
                   "How to fix" lands on Devices. */
                QObject* ob =
                    window->findChild<QObject*>("ownershipBanner");
                CHECK(ob != nullptr);
                if(ob)
                {
                    QVariantMap c;
                    c.insert(QStringLiteral("owner"),
                             QStringLiteral("Windows Dynamic Lighting"));
                    c.insert(QStringLiteral("key"),
                             QStringLiteral("dynamic_lighting"));
                    QVariantMap rep;
                    rep.insert(QStringLiteral("scanned"), true);
                    rep.insert(QStringLiteral("conflicts"),
                               QVariantList{ c });
                    bridge.setOwnershipForTesting(rep);
                    CHECK(ob->property("conflicts").toList().size()
                          == 1);
                    CHECK(!ob->property("shown").toBool());
                    CHECK(QMetaObject::invokeMethod(
                        window, "showOwnershipFix"));
                    CHECK(window->property("currentView").toInt()
                          == 3);
                    window->setProperty("currentView", 0);
                }
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
               INSIDE the stage. Anchored below a fixed-height
               canvas it once painted past the card edge and the
               next card hid it; the editor workspace (UX audit
               2026-11-28) puts it in the stage footer that the
               canvas/viewport anchor ABOVE, so it can't be
               covered — and the editor no longer scrolls as a
               page (viewport + rail fill the view). */
            if(std::strcmp(f, "pages/LayoutPage.qml") == 0)
            {
                QQuickItem* hint = qobject_cast<QQuickItem*>(
                    obj->findChild<QObject*>("canvasInstructions"));
                CHECK(hint != nullptr);
                CHECK(hint && hint->parentItem()
                      && hint->parentItem()->objectName()
                         == QStringLiteral("stageFooter"));
                CHECK(obj->findChild<QObject*>("layoutScroll")
                      == nullptr);
                CHECK(obj->findChild<QObject*>("layoutEditorSplit")
                      != nullptr);
                CHECK(obj->findChild<QObject*>("inspectorPane")
                      != nullptr);
            }
        }

        /* R-2: the Settings page's Hotkeys card renders one row
           per action from the AUTOMATION body; a bound chord
           shows in its capture field with a state pill. */
        if(std::strcmp(f, "pages/SettingsPage.qml") == 0)
        {
            QObject* card = obj->findChild<QObject*>("hotkeysCard");
            CHECK(card != nullptr);
            QObject* cap = FindItem(obj,
                "hotkeyCapture_lights_toggle");
            CHECK(cap != nullptr);
            for(const char* a : { "next_workspace", "prev_workspace",
                                  "brightness_up", "brightness_down",
                                  "pause_toggle" })
            {
                CHECK(FindItem(obj,
                    QStringLiteral("hotkeyRow_") +
                    QString::fromLatin1(a)) != nullptr);
            }
            if(card && cap)
            {
                /* Offline: no capability -> controls disabled. */
                CHECK(!card->property("up").toBool());
                CHECK(!cap->property("enabled").toBool());

                QVariantMap b;
                b.insert(QStringLiteral("action"),
                         QStringLiteral("lights_toggle"));
                b.insert(QStringLiteral("chord"),
                         QStringLiteral("Ctrl+Alt+Shift+End"));
                b.insert(QStringLiteral("state"),
                         QStringLiteral("in_use"));
                QVariantMap body;
                body.insert(QStringLiteral("hotkeys"), QVariantList{ b });
                body.insert(QStringLiteral("registrar"), true);
                bridge.setAutomationCapabilityForTesting(true);
                bridge.setAutomationForTesting(body);
                QCoreApplication::processEvents();
                CHECK(card->property("up").toBool());
                CHECK(cap->property("enabled").toBool());
                CHECK(cap->property("chord").toString() ==
                      QLatin1String("Ctrl+Alt+Shift+End"));
                QObject* pill = FindItem(obj,
                    "hotkeyState_lights_toggle");
                CHECK(pill != nullptr);
                if(pill)
                {
                    CHECK(pill->property("visible").toBool());
                    CHECK(pill->property("tone").toString() ==
                          QLatin1String("bad"));
                    CHECK(pill->property("text").toString()
                              .contains(QLatin1String("in use")));
                }
                /* Unbound rows show no pill. */
                QObject* none = FindItem(obj,
                    "hotkeyState_pause_toggle");
                CHECK(none && !none->property("visible").toBool());
                bridge.setAutomationCapabilityForTesting(false);
            }

            /* R-4: the Dimming card — engine config seeds the
               editor, buildConfig() is the set_dimming body. */
            QObject* dim = FindItem(obj, "dimmingCard");
            CHECK(dim != nullptr);
            if(dim)
            {
                CHECK(!dim->property("up").toBool());
                QVariantMap win;
                win.insert(QStringLiteral("days"), QVariantList{ 5, 6 });
                win.insert(QStringLiteral("start"), QStringLiteral("23:30"));
                win.insert(QStringLiteral("end"), QStringLiteral("07:00"));
                win.insert(QStringLiteral("level"), 0.3);
                QVariantMap lock;
                lock.insert(QStringLiteral("enabled"), true);
                lock.insert(QStringLiteral("level"), 0.0);
                QVariantMap idle;
                idle.insert(QStringLiteral("enabled"), false);
                idle.insert(QStringLiteral("after_s"), 600);
                idle.insert(QStringLiteral("level"), 0.3);
                idle.insert(QStringLiteral("skip_fullscreen"), true);
                QVariantMap cfg;
                cfg.insert(QStringLiteral("schedule"), QVariantList{ win });
                cfg.insert(QStringLiteral("lock"), lock);
                cfg.insert(QStringLiteral("idle"), idle);
                cfg.insert(QStringLiteral("ramp_ms"), 600);
                cfg.insert(QStringLiteral("restore_ms"), 0);
                QVariantMap active;
                active.insert(QStringLiteral("schedule"), 0.3);
                active.insert(QStringLiteral("lock"), QVariant());
                active.insert(QStringLiteral("idle"), QVariant());
                QVariantMap dimming;
                dimming.insert(QStringLiteral("config"), cfg);
                dimming.insert(QStringLiteral("configured"), true);
                dimming.insert(QStringLiteral("active"), active);
                QVariantMap body;
                body.insert(QStringLiteral("hotkeys"), QVariantList());
                body.insert(QStringLiteral("dimming"), dimming);
                bridge.setAutomationCapabilityForTesting(true);
                bridge.setAutomationForTesting(body);
                QCoreApplication::processEvents();
                CHECK(dim->property("up").toBool());

                QObject* lockBox = FindItem(obj, "dimLockEnabled");
                CHECK(lockBox &&
                      lockBox->property("checked").toBool());
                QObject* start0 = FindItem(obj, "dimWindowStart_0");
                CHECK(start0 && start0->property("text").toString() ==
                      QLatin1String("23:30"));
                QObject* activeText = FindItem(obj, "dimActive");
                CHECK(activeText && activeText->property("text")
                          .toString().contains(QLatin1String("schedule")));

                QVariant built;
                CHECK(QMetaObject::invokeMethod(dim, "buildConfig",
                    Q_RETURN_ARG(QVariant, built)));
                const QVariantMap bm = AsMap(built);
                const QVariantList sched =
                    bm.value(QStringLiteral("schedule")).toList();
                CHECK(sched.size() == 1);
                if(!sched.isEmpty())
                {
                    const QVariantMap w0 = sched[0].toMap();
                    CHECK(w0.value("start").toString() == "23:30");
                    CHECK(w0.value("end").toString() == "07:00");
                    CHECK(w0.value("days").toList().size() == 2);
                    CHECK(std::fabs(w0.value("level").toDouble() - 0.3)
                          < 1e-6);
                }
                CHECK(bm.value("lock").toMap().value("enabled").toBool());
                CHECK(!bm.value("idle").toMap().value("enabled").toBool());
                CHECK(bm.value("idle").toMap().value("after_s").toInt()
                      == 600);

                /* Add a window: one more row, a valid default. */
                CHECK(QMetaObject::invokeMethod(dim, "addWindow"));
                QCoreApplication::processEvents();
                CHECK(FindItem(obj, "dimWindowStart_1") != nullptr);
                CHECK(QMetaObject::invokeMethod(dim, "buildConfig",
                    Q_RETURN_ARG(QVariant, built)));
                CHECK(AsMap(built).value("schedule").toList().size()
                      == 2);
                bridge.setAutomationCapabilityForTesting(false);
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
