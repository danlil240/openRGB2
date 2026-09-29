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
#include "StartupTaskController.h"
#include "UiSettings.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSystemTrayIcon>
#include <QTimer>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1
#define DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1 19
#endif
#endif

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv)
{
    /* --pipe <name> overrides the well-known command pipe —
       used by manual verification against a fixture engine. */
    QString pipe;
    bool managed = false;
    bool unmanaged = false;
    bool background = false;
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
        else if(std::strcmp(argv[i], "--managed") == 0)
        {
            managed = true;
        }
        else if(std::strcmp(argv[i], "--unmanaged") == 0)
        {
            unmanaged = true;
        }
        else if(std::strcmp(argv[i], "--background") == 0)
        {
            background = true;
        }
    }

    QApplication app(argc, argv);
    QApplication::setApplicationName("OpenRGB2");
    QApplication::setOrganizationName("OpenRGB2");
    const QString managed_script = QDir(
        QCoreApplication::applicationDirPath()).filePath(
            QStringLiteral("scripts/start-openrgb2.ps1"));
    if(!unmanaged && !qa_enabled && pipe.isEmpty()
       && QFileInfo::exists(managed_script))
        managed = true;
    if(unmanaged)
        managed = false;
    const QIcon app_icon(QStringLiteral(":/OpenRGB2.ico"));
    QApplication::setWindowIcon(app_icon);
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    /* The packaged desktop entry is a single UI. A second click
       restores the existing window, including from the tray. QA and
       custom-pipe sessions stay independent for offline testing. */
    QLocalServer instance_server;
    if(!qa_enabled && pipe.isEmpty()
       && QFileInfo::exists(managed_script))
    {
        const QByteArray key = QCryptographicHash::hash(
            (QCoreApplication::applicationDirPath().toLower()
             + QDir::homePath().toLower()).toUtf8(),
            QCryptographicHash::Sha256).toHex().left(20);
        const QString name = QStringLiteral("openrgb2-ui-")
                             + QString::fromLatin1(key);
        if(!instance_server.listen(name))
        {
            QLocalSocket existing;
            existing.connectToServer(name);
            if(existing.waitForConnected(500))
            {
                existing.write("show\n");
                existing.waitForBytesWritten(500);
                return 0;
            }
            QLocalServer::removeServer(name);
            instance_server.listen(name);
        }
    }

#ifdef Q_OS_WIN
    /* Elevate only the packaged managed launch. Offline QA and custom
       pipe sessions remain ordinary executables without a UAC prompt. */
    if(managed && !qa_enabled && pipe.isEmpty())
    {
        HANDLE token = nullptr;
        TOKEN_ELEVATION elevation{};
        DWORD bytes = 0;
        const bool known = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY,
                                             &token)
            && GetTokenInformation(token, TokenElevation, &elevation,
                                   sizeof(elevation), &bytes);
        if(token)
            CloseHandle(token);
        if(!known)
        {
            QMessageBox::warning(nullptr, QObject::tr("OpenRGB2 startup"),
                QObject::tr("Could not determine administrator status."));
            return 1;
        }
        if(!elevation.TokenIsElevated)
        {
            instance_server.close();
            const QString exe = QCoreApplication::applicationFilePath();
            const QString args = background
                ? QStringLiteral("--managed --background")
                : QStringLiteral("--managed");
            const QString dir = QCoreApplication::applicationDirPath();
            SHELLEXECUTEINFOW launch{};
            launch.cbSize = sizeof(launch);
            launch.fMask = SEE_MASK_NOCLOSEPROCESS;
            launch.lpVerb = L"runas";
            launch.lpFile = reinterpret_cast<LPCWSTR>(exe.utf16());
            launch.lpParameters = reinterpret_cast<LPCWSTR>(args.utf16());
            launch.lpDirectory = reinterpret_cast<LPCWSTR>(dir.utf16());
            launch.nShow = background ? SW_HIDE : SW_SHOWNORMAL;
            if(!ShellExecuteExW(&launch))
            {
                if(GetLastError() != ERROR_CANCELLED)
                    QMessageBox::warning(nullptr,
                        QObject::tr("OpenRGB2 startup"),
                        QObject::tr("Administrator launch failed."));
                return 1;
            }
            if(launch.hProcess)
                CloseHandle(launch.hProcess);
            return 0;
        }
    }
#endif

    /* Windows can start a logon task before Explorer creates the tray.
       A visible QSystemTrayIcon registers itself when the tray appears. */
#ifdef Q_OS_WIN
    const bool tray_available = true;
#else
    const bool tray_available = QSystemTrayIcon::isSystemTrayAvailable();
#endif
    app.setQuitOnLastWindowClosed(!tray_available);

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
    openrgb2::ui::StartupTaskController startup_task(
        QCoreApplication::applicationDirPath());
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
    engine.rootContext()->setContextProperty(
        QStringLiteral("startupTask"), &startup_task);
    engine.rootContext()->setContextProperty(
        QStringLiteral("trayAvailable"), tray_available);
    engine.rootContext()->setContextProperty(
        QStringLiteral("startHidden"), background && tray_available);
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, [] { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);
    engine.loadFromModule(QStringLiteral("OpenRGB2.UI"),
                          QStringLiteral("Main"));

    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().value(0));
    if(!window)
    {
        return 1;
    }
#ifdef Q_OS_WIN
    /* Dark native title bar: ask DWM to render the frame in the
       immersive dark scheme so the chrome matches the QML theme.
       Attribute 20 covers 20H1+; fall back to 19 on older builds. */
    {
        const BOOL dark = TRUE;
        const HWND hwnd = reinterpret_cast<HWND>(window->winId());
        if(FAILED(DwmSetWindowAttribute(hwnd,
                                        DWMWA_USE_IMMERSIVE_DARK_MODE,
                                        &dark, sizeof(dark))))
        {
            DwmSetWindowAttribute(hwnd,
                DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1,
                &dark, sizeof(dark));
        }
    }
#endif
    if(startup_task.available())
        startup_task.refresh();

    QMenu tray_menu;
    QSystemTrayIcon tray(app_icon);
    QProcess shutdown;
    if(tray_available)
    {
        auto show_ui = [window] {
            window->showNormal();
            window->raise();
            window->requestActivate();
        };
        tray.setToolTip(QStringLiteral("OpenRGB2"));
        tray.setContextMenu(&tray_menu);
        tray_menu.addAction(QObject::tr("Open OpenRGB2"), show_ui);
        tray_menu.addSeparator();
        auto exit_ui = [window] {
            window->setProperty("exitRequested", true);
            window->close();
        };
        tray_menu.addAction(QObject::tr("Exit UI (keep lighting on)"),
                            exit_ui);
        const QString install_dir = QCoreApplication::applicationDirPath();
        const QString stop_script = QDir(install_dir).filePath(
            QStringLiteral("scripts/stop-openrgb2.ps1"));
        if(QFileInfo::exists(stop_script))
        {
            tray_menu.addAction(QObject::tr("Quit OpenRGB2"),
                [&shutdown, install_dir, stop_script, exit_ui] {
                if(shutdown.state() != QProcess::NotRunning)
                    return;
                if(QMessageBox::question(nullptr,
                    QObject::tr("Quit OpenRGB2"),
                    QObject::tr("Stop lighting and close OpenRGB2?"),
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No) != QMessageBox::Yes)
                    return;
                const QString state_dir = QDir(install_dir).filePath(
                    QStringLiteral("state"));
                QDir().mkpath(state_dir);
                shutdown.setProgram(QStringLiteral("powershell.exe"));
                shutdown.setArguments({QStringLiteral("-NoProfile"),
                    QStringLiteral("-NonInteractive"),
                    QStringLiteral("-ExecutionPolicy"),
                    QStringLiteral("Bypass"),
                    QStringLiteral("-File"), stop_script,
                    QStringLiteral("-InstallDir"), install_dir});
                shutdown.setWorkingDirectory(install_dir);
                shutdown.setProcessChannelMode(QProcess::MergedChannels);
                shutdown.setStandardOutputFile(QDir(state_dir).filePath(
                    QStringLiteral("shutdown.log")), QIODevice::Append);
#ifdef Q_OS_WIN
                shutdown.setCreateProcessArgumentsModifier(
                    [](QProcess::CreateProcessArguments* args) {
                        args->flags |= CREATE_NO_WINDOW;
                        args->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
                        args->startupInfo->wShowWindow = SW_HIDE;
                    });
#endif
                shutdown.start();
            });
            QObject::connect(&shutdown,
                qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                &app, [exit_ui, install_dir](int code,
                                            QProcess::ExitStatus status) {
                if(status == QProcess::NormalExit && code == 0)
                    exit_ui();
                else
                    QMessageBox::warning(nullptr,
                        QObject::tr("OpenRGB2 shutdown"),
                        QObject::tr("Lighting services did not stop cleanly.\nDetails: %1")
                            .arg(QDir(install_dir).filePath(
                                QStringLiteral("state/shutdown.log"))));
            });
            QObject::connect(&shutdown, &QProcess::errorOccurred,
                &app, [install_dir](QProcess::ProcessError error) {
                if(error == QProcess::FailedToStart)
                    QMessageBox::warning(nullptr,
                        QObject::tr("OpenRGB2 shutdown"),
                        QObject::tr("Could not start the shutdown command in %1")
                            .arg(install_dir));
            });
        }
        QObject::connect(&tray, &QSystemTrayIcon::activated,
                         &app, [show_ui](QSystemTrayIcon::ActivationReason reason) {
            if(reason == QSystemTrayIcon::Trigger
               || reason == QSystemTrayIcon::DoubleClick)
                show_ui();
        });
        QObject::connect(&instance_server, &QLocalServer::newConnection,
                         &app, [&instance_server, show_ui] {
            while(auto* client = instance_server.nextPendingConnection())
            {
                show_ui();
                client->deleteLater();
            }
        });
        tray.show();
#ifdef Q_OS_WIN
        if(background && !QSystemTrayIcon::isSystemTrayAvailable())
            QTimer::singleShot(20000, window, [window] {
                if(!QSystemTrayIcon::isSystemTrayAvailable())
                    window->showNormal();
            });
#endif
    }
    else
    {
        QObject::connect(&instance_server, &QLocalServer::newConnection,
                         &app, [&instance_server, window] {
            while(auto* client = instance_server.nextPendingConnection())
            {
                window->showNormal();
                window->raise();
                window->requestActivate();
                client->deleteLater();
            }
        });
    }

    /* The desktop executable starts the existing managed stack.
       PowerShell is a child of the GUI process with CREATE_NO_WINDOW;
       the script starts the backend and engine with hidden windows.
       -NoUI avoids a second copy of this executable. */
    QProcess startup;
    if(managed)
    {
        const QString install_dir = QCoreApplication::applicationDirPath();
        const QString script = managed_script;
        if(!QFileInfo::exists(script))
        {
            QMessageBox::warning(nullptr, QObject::tr("OpenRGB2 startup"),
                QObject::tr("Managed startup script is missing:\n%1")
                    .arg(script));
        }
        else
        {
            const QString state_dir = QDir(install_dir).filePath(
                QStringLiteral("state"));
            QDir().mkpath(state_dir);
            const QString log = QDir(state_dir).filePath(
                QStringLiteral("startup.log"));
            startup.setProgram(QStringLiteral("powershell.exe"));
            startup.setArguments({QStringLiteral("-NoProfile"),
                QStringLiteral("-NonInteractive"),
                QStringLiteral("-ExecutionPolicy"),
                QStringLiteral("Bypass"),
                QStringLiteral("-File"), script,
                QStringLiteral("-InstallDir"), install_dir,
                QStringLiteral("-TakeOver"),
                QStringLiteral("-HideWindows"),
                QStringLiteral("-NoUI")});
            startup.setWorkingDirectory(install_dir);
            startup.setProcessChannelMode(QProcess::MergedChannels);
            startup.setStandardOutputFile(log, QIODevice::Append);
#ifdef Q_OS_WIN
            startup.setCreateProcessArgumentsModifier(
                [](QProcess::CreateProcessArguments* args) {
                    args->flags |= CREATE_NO_WINDOW;
                    args->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
                    args->startupInfo->wShowWindow = SW_HIDE;
                });
#endif
            QObject::connect(&startup, &QProcess::errorOccurred,
                             &app, [log](QProcess::ProcessError error) {
                if(error == QProcess::FailedToStart)
                    QMessageBox::warning(nullptr, QObject::tr("OpenRGB2 startup"),
                        QObject::tr("Could not start the managed launcher.\nDetails: %1")
                            .arg(log));
            });
            QObject::connect(&startup,
                qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                &app, [log](int code, QProcess::ExitStatus status) {
                if(status != QProcess::NormalExit || code != 0)
                    QMessageBox::warning(nullptr, QObject::tr("OpenRGB2 startup"),
                        QObject::tr("Lighting services could not start (exit code %1).\nDetails: %2")
                            .arg(code).arg(log));
            });
            QTimer::singleShot(0, &startup, [&startup] { startup.start(); });
        }
    }

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
