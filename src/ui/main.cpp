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
#include "QaServer.h"
#include "SceneEditController.h"
#include "UiSettings.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv)
{
    /* --pipe <name> overrides the well-known command pipe —
       used by manual verification against a fixture engine. */
    QString pipe;
    /* --qa[=port] (or OPENRGB2_QA=1, optional OPENRGB2_QA_PORT)
       starts the loopback QA automation socket. Off by
       default — the shipped UI never opens a port. */
    bool qa_enabled =
        qEnvironmentVariableIsSet("OPENRGB2_QA") &&
        qEnvironmentVariable("OPENRGB2_QA") != "0";
    int  qa_port = openrgb2::ui::QaServer::kDefaultPort;
    if(qEnvironmentVariableIsSet("OPENRGB2_QA_PORT"))
    {
        qa_port = qEnvironmentVariable("OPENRGB2_QA_PORT").toInt();
    }
    for(int i = 1; i < argc; ++i)
    {
        if(std::strcmp(argv[i], "--pipe") == 0 && i + 1 < argc)
        {
            pipe = QString::fromUtf8(argv[++i]);
        }
        else if(std::strcmp(argv[i], "--qa") == 0)
        {
            qa_enabled = true;
            if(i + 1 < argc && std::isdigit(
                   (unsigned char)argv[i + 1][0]))
            {
                qa_port = std::atoi(argv[++i]);
            }
        }
        else if(std::strncmp(argv[i], "--qa=", 5) == 0)
        {
            qa_enabled = true;
            qa_port = std::atoi(argv[i] + 5);
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

    /* QA harness is opt-in and needs the loaded root object —
       it walks the live item tree and synthesizes input. */
    openrgb2::ui::QaServer qa_server(engine);
    if(qa_enabled)
    {
        QString qa_err;
        if(qa_server.Start((quint16)qa_port, qa_err))
        {
            std::fprintf(stderr,
                "QA socket: 127.0.0.1:%u\n",
                (unsigned)qa_server.Port());
        }
        else
        {
            std::fprintf(stderr,
                "QA socket failed: %s\n",
                qa_err.toUtf8().constData());
        }
    }

    /* Connect AFTER the UI is up: degraded/offline states must
       render honestly even when the engine never answers. */
    bridge.start();
    return app.exec();
}
