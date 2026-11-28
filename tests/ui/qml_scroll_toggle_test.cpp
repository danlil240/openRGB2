// SPDX-License-Identifier: GPL-2.0-or-later
//
// Audit Q-12 — the real shell headless (offline engine, like
// qml_smoke_test) in a shown window:
//
//   1. every page's scroll surface carries a vertical scroll bar
//   2. keyboard focus landing on a control below the fold scrolls
//      the page to show it
//   3. the Layout 2D/3D toggle paints its checked state, and a
//      click on the already-active view keeps it checked (the
//      checkable button used to flip itself off)
//
// CHECK-macro main like the other UI suites — no framework.

#include "DiagnosticsController.h"
#include "EngineBridge.h"
#include "LayoutController.h"
#include "SceneEditController.h"

#include <QColor>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>

#include <cstdio>
#include <memory>

using namespace openrgb2;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

static QColor BgColor(QObject* btn)
{
    auto* bg = btn ? btn->property("background").value<QQuickItem*>()
                   : nullptr;
    return bg ? bg->property("color").value<QColor>() : QColor();
}

/* The lowest enabled, visible, Tab-reachable control under
   `root` — a real focus target below the fold. */
static QQuickItem* LowestFocusable(QQuickItem* root, QQuickItem* ref,
                                   qreal& best)
{
    QQuickItem* hit = nullptr;
    for(QQuickItem* c : root->childItems())
    {
        if(!c->isVisible() || !c->isEnabled())
        {
            continue;
        }
        if(c->activeFocusOnTab())
        {
            const qreal y = c->mapToItem(ref, QPointF(0, 0)).y();
            if(y > best)
            {
                best = y;
                hit  = c;
            }
        }
        if(QQuickItem* d = LowestFocusable(c, ref, best))
        {
            hit = d;
        }
    }
    return hit;
}

static void Click(QQuickWindow* win, QQuickItem* item)
{
    const QPointF c = item->mapToScene(
        QPointF(item->width() / 2, item->height() / 2));
    QMouseEvent press(QEvent::MouseButtonPress, c, c,
                      win->mapToGlobal(c), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(win, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, c, c,
                        win->mapToGlobal(c), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(win, &release);
    QCoreApplication::processEvents();
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QQmlEngine engine;
    ui::EngineBridge          bridge;      /* offline */
    ui::SceneEditController   scene_edit(&bridge);
    ui::LayoutController      layout_ctl(&bridge);
    ui::DiagnosticsController diag_ctl(&bridge);
    QQmlContext* rc = engine.rootContext();
    rc->setContextProperty(QStringLiteral("trayAvailable"), false);
    rc->setContextProperty(QStringLiteral("bridge"), &bridge);
    rc->setContextProperty(QStringLiteral("sceneEdit"), &scene_edit);
    rc->setContextProperty(QStringLiteral("layoutCtl"), &layout_ctl);
    rc->setContextProperty(QStringLiteral("diagCtl"), &diag_ctl);
    /* shows the Layout view bar; with no uiPrefs show3d stays
       false, so no 3D viewport is ever loaded */
    rc->setContextProperty(QStringLiteral("quick3dAvailable"), true);

    QQmlComponent comp(&engine, QUrl(QStringLiteral(
        "qrc:/qt/qml/OpenRGB2/UI/Main.qml")));
    if(comp.isError())
    {
        ++failures;
        std::printf("FAIL load Main.qml:\n%s\n",
                    comp.errorString().toUtf8().constData());
        std::printf("%d checks, %d failures\n", checks, failures);
        return 1;
    }
    std::unique_ptr<QObject> win_obj(comp.create(rc));
    auto* win = qobject_cast<QQuickWindow*>(win_obj.get());
    CHECK(win != nullptr);
    if(win == nullptr)
    {
        std::printf("%d checks, %d failures\n", checks, failures);
        return 1;
    }
    win->resize(1180, 760);
    win->show();
    win->requestActivate();
    QCoreApplication::processEvents();

    /*---- 1. scroll bars ------------------------------------*/
    std::printf("-- every page has a vertical scroll bar\n");
    {
        const auto bars = win_obj->findChildren<QObject*>(
            QStringLiteral("pageScrollBar"));
        /* Layout is an editor workspace (UX audit 2026-11-28):
           the page itself never scrolls — its three rail panes
           (Devices, Identify, Inspector) do. The other four
           pages each scroll as a page. */
        int in_layout = 0;
        for(QObject* b : bars)
        {
            for(QObject* p = b; p != nullptr; p = p->parent())
            {
                if(p->objectName() == QStringLiteral("layoutPage"))
                {
                    ++in_layout;
                    break;
                }
            }
        }
        CHECK(in_layout == 3);
        CHECK(bars.size() - in_layout == 4);
        CHECK(win_obj->findChild<QObject*>(
                  QStringLiteral("layoutScroll")) == nullptr);
    }

    /*---- 2. focus scrolls into view ------------------------*/
    std::printf("-- focus below the fold scrolls the page\n");
    {
        win_obj->setProperty("currentView", 1);
        QCoreApplication::processEvents();
        auto* flick = qobject_cast<QQuickItem*>(
            win_obj->findChild<QObject*>(
                QStringLiteral("lightingScroll")));
        qreal best = -1e9;
        QQuickItem* target = flick
            ? LowestFocusable(flick, flick, best) : nullptr;
        CHECK(flick != nullptr && target != nullptr);
        if(flick != nullptr && target != nullptr)
        {
            flick->setProperty("contentY", 0);
            QCoreApplication::processEvents();
            const qreal before =
                target->mapToItem(flick, QPointF(0, 0)).y();
            /* below the fold to begin with */
            CHECK(before + target->height() > flick->height());
            target->forceActiveFocus();
            QCoreApplication::processEvents();
            CHECK(target->hasActiveFocus());
            const qreal y =
                target->mapToItem(flick, QPointF(0, 0)).y();
            CHECK(flick->property("contentY").toReal() > 0);
            CHECK(y >= 0 && y + target->height() <= flick->height());
        }
    }

    /*---- 3. the 2D/3D toggle shows and keeps its state ------*/
    std::printf("-- layout 2D/3D toggle\n");
    {
        win_obj->setProperty("currentView", 2);
        QCoreApplication::processEvents();
        auto* b2 = qobject_cast<QQuickItem*>(
            win_obj->findChild<QObject*>(
                QStringLiteral("view2dButton")));
        auto* b3 = qobject_cast<QQuickItem*>(
            win_obj->findChild<QObject*>(
                QStringLiteral("view3dButton")));
        CHECK(b2 != nullptr && b3 != nullptr);
        if(b2 != nullptr && b3 != nullptr)
        {
            CHECK(b2->isVisible());
            CHECK(b2->property("checked").toBool());
            CHECK(!b3->property("checked").toBool());
            /* checked must LOOK different from unchecked */
            CHECK(BgColor(b2).isValid() && BgColor(b3).isValid());
            CHECK(BgColor(b2) != BgColor(b3));

            /* clicking the active view keeps it the active view */
            Click(win, b2);
            CHECK(b2->property("checked").toBool());
            CHECK(!b3->property("checked").toBool());
        }
    }

    std::printf("qml_scroll_toggle_test: %d checks, %d failures\n",
                checks, failures);
    return failures ? 1 : 0;
}
