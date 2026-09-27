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

#include "EngineBridge.h"

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

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("bridge"),
                                             &bridge);
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
