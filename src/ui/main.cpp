/*---------------------------------------------------------*\
||| main.cpp — openrgb2-ui                                    ||
|||                                                           ||
|||   Standalone Qt Quick shell (plan sections 4-5): the   ||
|||   UI is a separate process — closing it never stops     ||
|||   the engine, and reconnecting resumes state via        ||
|||   WELCOME + SUBSCRIBE initial pushes. All engine access ||
|||   goes through EngineBridge -> the restricted command   ||
|||   pipe; no hardware/SDK/backend paths exist here.       ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#include "DiagnosticsController.h"
#include "EngineBridge.h"
#include "LayoutController.h"
#include "SceneEditController.h"
#include "UiSettings.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>

#include <cstring>

int main(int argc, char** argv)
{
    /* --pipe <name> overrides the well-known command pipe —
       used by manual verification against a fixture engine. */
    QString pipe;
    for(int i = 1; i < argc; ++i)
    {
        if(std::strcmp(argv[i], "--pipe") == 0 && i + 1 < argc)
        {
            pipe = QString::fromUtf8(argv[++i]);
        }
    }

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName("OpenRGB2");
    QGuiApplication::setOrganizationName("OpenRGB2");
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    openrgb2::ui::EngineBridge bridge;
    if(!pipe.isEmpty())
    {
        bridge.setPipeName(pipe);
    }
    /* The Lighting view's scene-edit session — owns the working
       document, the save flow, and the named-scene library. */
    openrgb2::ui::SceneEditController scene_edit(&bridge);
    /* The Layout view's edit session — arrangement ops ride
       LAYOUT_COMMAND, undo/redo reverts via SET_SCENE. */
    openrgb2::ui::LayoutController layout_ctl(&bridge);
    /* The Devices & Diagnostics view's read-only surface —
       derived rate/ownership/conflict views over the bridge
       stores plus the redacted export bundle (Task 4.7). */
    openrgb2::ui::DiagnosticsController diag_ctl(&bridge);
    /* UI-only preferences (Task 4.8): window geometry, last
       view, palette position, and per-scene playback-param
       seeds. Engine state is never persisted here — restart
       state comes back over WELCOME + subscription replay. */
    openrgb2::ui::UiSettings ui_prefs;
    scene_edit.setUiSettings(&ui_prefs);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("bridge"),
                                             &bridge);
    engine.rootContext()->setContextProperty(QStringLiteral("sceneEdit"),
                                             &scene_edit);
    engine.rootContext()->setContextProperty(QStringLiteral("layoutCtl"),
                                             &layout_ctl);
    engine.rootContext()->setContextProperty(QStringLiteral("diagCtl"),
                                             &diag_ctl);
    engine.rootContext()->setContextProperty(QStringLiteral("uiPrefs"),
                                             &ui_prefs);
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, [] { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);
    engine.loadFromModule(QStringLiteral("OpenRGB2.UI"),
                          QStringLiteral("Main"));

    /* Connect AFTER the UI is up: degraded/offline states must
       render honestly even when the engine never answers. */
    bridge.start();
    return app.exec();
}
