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

#include "CrashDump.h"
#include "DiagnosticsController.h"
#include "EngineBridge.h"
#include "LaunchHardening.h"
#include "OwnershipNotice.h"
#include "LayoutController.h"
#include "QaServer.h"
#include "SceneEditController.h"
#include "StartupTaskController.h"
#include "StudioViewportModel.h"
#include "TrayController.h"
#include "UiSettings.h"
#include "WorkspaceController.h"
#include "studio/EffectEditorController.h"
#include "studio/ObjectEditorController.h"
#include "studio/DevicePresetController.h"
#include "studio/CalibrationController.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMenu>
#include <QMessageBox>
#include <QPalette>
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
#include <functional>

namespace
{

/* QA port text -> 0..65535 (0 = ephemeral). Anything else —
   non-numeric, signed, out of range — is rejected instead of
   silently becoming 0 or wrapping modulo 65536. */
bool ParseQaPort(const QString& text, int& port)
{
    bool ok = false;
    const uint v = text.trimmed().toUInt(&ok, 10);
    if(!ok || v > 65535u)
    {
        return false;
    }
    port = (int)v;
    return true;
}

/* Bring the window back from the tray / minimized state
   without dropping a maximized state (showNormal() would). */
void ShowRestored(QQuickWindow* window)
{
    const Qt::WindowStates st =
        window->windowStates() & ~Qt::WindowMinimized;
    if(st & Qt::WindowMaximized)
    {
        window->showMaximized();
    }
    else
    {
        window->showNormal();
    }
    window->raise();
    window->requestActivate();
}

/* R-1: render the TrayController's menu model (data) into a
   QMenu. Submenus are created here and recorded in `owned` so
   the next rebuild deletes them (QMenu::clear() only removes
   actions). Every action routes back through trigger(id). */
void RenderTrayEntries(QMenu* menu, const QVariantList& entries,
                       openrgb2::ui::TrayController* ctl,
                       QList<QMenu*>& owned)
{
    for(const QVariant& v : entries)
    {
        const QVariantMap e    = v.toMap();
        const QString     kind = e.value(QStringLiteral("kind")).toString();
        const QString     text = e.value(QStringLiteral("text")).toString();
        if(kind == QLatin1String("separator"))
        {
            menu->addSeparator();
            continue;
        }
        if(kind == QLatin1String("submenu"))
        {
            QMenu* sub = new QMenu(text, menu);
            owned << sub;
            sub->setEnabled(e.value(QStringLiteral("enabled")).toBool());
            RenderTrayEntries(sub,
                              e.value(QStringLiteral("children")).toList(),
                              ctl, owned);
            menu->addMenu(sub);
            continue;
        }
        QAction* a = menu->addAction(text);
        a->setEnabled(e.value(QStringLiteral("enabled")).toBool());
        a->setCheckable(e.value(QStringLiteral("checkable")).toBool());
        a->setChecked(e.value(QStringLiteral("checked")).toBool());
        const QString id = e.value(QStringLiteral("id")).toString();
        QObject::connect(a, &QAction::triggered, ctl,
                         [ctl, id] { ctl->trigger(id); });
    }
}

} /* anonymous namespace */

int main(int argc, char** argv)
{
    /* R-9: crash dumps first (<install>\state\crash, newest 5).
       A failure only costs the dump. */
    {
        std::string cerr;
        if(!openrgb2::supervise::InstallCrashDumpHandler(
               std::string(), "openrgb2-ui", 5, &cerr))
        {
            std::fprintf(stderr, "crash dumps disabled: %s\n",
                         cerr.c_str());
        }
    }
    /* An elevated UI must not load modules from locations a
       medium-integrity process controls: Qt's plugin / QML-import
       / style search vars are inherited from the (non-elevated)
       user environment. Cleared before Qt loads anything. */
    const bool elevated = openrgb2::ui::launch::ProcessIsElevated();
    if(elevated)
    {
        openrgb2::ui::launch::ScrubModuleSearchEnv();
    }
    /* --pipe <name> overrides the well-known command pipe —
       used by manual verification against a fixture engine. */
    QString pipe;
    bool managed = false;
    bool unmanaged = false;
    bool background = false;
    /* --qa[=port] (or OPENRGB2_QA=1, optional OPENRGB2_QA_PORT)
       starts the loopback QA automation socket. Off by
       default — the shipped UI never opens a port, and an
       elevated or managed UI refuses it (see DecideQa below). */
    bool qa_enabled =
        qEnvironmentVariableIsSet("OPENRGB2_QA") &&
        qEnvironmentVariable("OPENRGB2_QA") != "0";
    int  qa_port = openrgb2::ui::QaServer::kDefaultPort;
    bool qa_port_ok = true;
    QString qa_port_text;
    if(qEnvironmentVariableIsSet("OPENRGB2_QA_PORT"))
    {
        qa_port_text = qEnvironmentVariable("OPENRGB2_QA_PORT");
        qa_port_ok = ParseQaPort(qa_port_text, qa_port);
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
                qa_port_text = QString::fromUtf8(argv[++i]);
                qa_port_ok = ParseQaPort(qa_port_text, qa_port);
            }
        }
        else if(std::strncmp(argv[i], "--qa=", 5) == 0)
        {
            qa_enabled = true;
            qa_port_text = QString::fromUtf8(argv[i] + 5);
            qa_port_ok = ParseQaPort(qa_port_text, qa_port);
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

    /* Never serve QA from an elevated / managed UI: OPENRGB2_QA
       is settable by any non-elevated process (HKCU\Environment)
       and the token file is readable at medium integrity, so the
       socket would let unelevated code drive an admin process.
       Decided before anything else reads qa_enabled, so a refused
       request behaves exactly like no request. */
    {
        const auto qa_decision = openrgb2::ui::launch::DecideQa(
            qa_enabled, managed, elevated);
        if(qa_decision == openrgb2::ui::launch::QaDecision::RefusedElevated
           || qa_decision == openrgb2::ui::launch::QaDecision::RefusedManaged)
        {
            std::fprintf(stderr, "%s\n",
                openrgb2::ui::launch::QaRefusalMessage(qa_decision)
                    .toUtf8().constData());
            qa_enabled = false;
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
    /* F10: any Basic control that still falls back to stock
       palette roles (native file dialogs, the odd unstyled
       control) must default to the dark theme rather than
       light-chrome black-text-on-white. Values mirror
       ui/theme/Theme.qml — keep them in step. */
    {
        QPalette dark;
        const QColor bg(0x0C, 0x10, 0x17);
        const QColor surface(0x14, 0x1B, 0x25);
        const QColor surfaceAlt(0x1A, 0x23, 0x30);
        const QColor border(0x3B, 0x4C, 0x61);
        const QColor text(0xE9, 0xEE, 0xF4);
        const QColor textDim(0x7B, 0x87, 0x94);
        const QColor accent(0x7F, 0xCF, 0xFF);
        dark.setColor(QPalette::Window, surface);
        dark.setColor(QPalette::WindowText, text);
        dark.setColor(QPalette::Base, surfaceAlt);
        dark.setColor(QPalette::AlternateBase, bg);
        dark.setColor(QPalette::ToolTipBase, surfaceAlt);
        dark.setColor(QPalette::ToolTipText, text);
        dark.setColor(QPalette::Text, text);
        dark.setColor(QPalette::Button, surfaceAlt);
        dark.setColor(QPalette::ButtonText, text);
        dark.setColor(QPalette::BrightText, accent);
        dark.setColor(QPalette::Link, accent);
        dark.setColor(QPalette::Highlight, accent);
        dark.setColor(QPalette::HighlightedText,
                      QColor(0x06, 0x12, 0x1D));
        dark.setColor(QPalette::PlaceholderText, textDim);
        dark.setColor(QPalette::Mid, border);
        dark.setColor(QPalette::Dark, bg);
        dark.setColor(QPalette::Light, surfaceAlt);
        dark.setColor(QPalette::Disabled, QPalette::WindowText,
                      textDim);
        dark.setColor(QPalette::Disabled, QPalette::Text, textDim);
        dark.setColor(QPalette::Disabled, QPalette::ButtonText,
                      textDim);
        app.setPalette(dark);
    }

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
    /* The shared workspace draft (studio-import Task 4): when
       the engine advertises studio_workspace_v1 and manages a
       workspace, both editing pages adapt to this ONE draft —
       Lighting and Layout share dirty/conflict/undo state and
       the durable crash-recovery record. On a legacy engine
       the controller reports unavailable and both pages keep
       their SET_SCENE / LAYOUT_COMMAND paths. */
    openrgb2::ui::WorkspaceController workspace_ctl(&bridge);
    /* The Lighting view's scene-edit session — owns the working
       document, the save flow, and the named-scene library. */
    openrgb2::ui::SceneEditController scene_edit(&bridge);
    /* The Layout view's edit session — arrangement ops ride
       LAYOUT_COMMAND, undo/redo reverts via SET_SCENE. */
    openrgb2::ui::LayoutController layout_ctl(&bridge);
    scene_edit.setWorkspace(&workspace_ctl);
    layout_ctl.setWorkspace(&workspace_ctl);
    /* studio-import Task 5: the scene/object editor — selection,
       transform gestures, hierarchy ops, painting and the device
       library ALL edit the same shared draft through the
       session's own EditorController (never a private document
       or undo stack). */
    openrgb2::ui::ObjectEditorController object_ctl(
        &bridge, &workspace_ctl);
    /* studio-import Task 6: the effect-layer editor — stack,
       palette, personal-look shelf and persisted input
       settings all edit the same shared draft through the
       session's EditorController (never a private document
       or undo stack). */
    openrgb2::ui::EffectEditorController effect_ctl(
        &bridge, &workspace_ctl);
    /* studio-import Task 9: device-TYPE authoring — variant/
       assembly creation, the preset editor's
       validate/preview/save/delete, all through the shared
       draft + the engine's bounded resource jobs (never a
       private document or a direct file write). */
    openrgb2::ui::DevicePresetController preset_ctl(
        &bridge, &workspace_ctl, &object_ctl);
    /* studio-import Task 11: the physical LED-calibration
       wizard's engine bridge — WORKSPACE_COMMAND
       calibration_* verbs behind an opaque session token;
       the engine owns every hardware write. */
    openrgb2::ui::CalibrationController calib_ctl(&bridge);
    /* studio-import Task 7: the bounded draft preview — the
       UNSAVED workspace draft evaluated engine-side and
       pushed back over the session-bound "preview" event
       feed. Distinct from frame_status (transport-accepted
       output): draft colors never claim physical delivery. */
    openrgb2::ui::StudioViewportModel viewport_model(
        &bridge, &workspace_ctl);
    /* Normal close flushes the debounced draft record so a
       crash-free exit still preserves unsaved work. */
    QObject::connect(&app, &QCoreApplication::aboutToQuit,
                     &workspace_ctl,
                     [&workspace_ctl] {
                         workspace_ctl.FlushDrafts();
                     });
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
    engine.rootContext()->setContextProperty(QStringLiteral("workspace"),
                                             &workspace_ctl);
    engine.rootContext()->setContextProperty(QStringLiteral("objectCtl"),
                                             &object_ctl);
    engine.rootContext()->setContextProperty(QStringLiteral("effectEdit"),
                                             &effect_ctl);
    engine.rootContext()->setContextProperty(QStringLiteral("presetCtl"),
                                             &preset_ctl);
    engine.rootContext()->setContextProperty(QStringLiteral("calibCtl"),
                                             &calib_ctl);
    engine.rootContext()->setContextProperty(QStringLiteral("viewport"),
                                             &viewport_model);
    engine.rootContext()->setContextProperty(QStringLiteral("diagCtl"),
                                             &diag_ctl);
    engine.rootContext()->setContextProperty(QStringLiteral("uiPrefs"),
                                             &ui_prefs);
    engine.rootContext()->setContextProperty(
        QStringLiteral("startupTask"), &startup_task);
    /* studio-import Task 8: the Layout 3D viewport's capability
       gate — the Quick3D subtree is only compiled into the module
       in the full-Studio build; basic builds read false and keep
       the 2D canvas (the page's Loader also fails honestly if the
       runtime module is missing). */
    engine.rootContext()->setContextProperty(
        QStringLiteral("quick3dAvailable"),
#ifdef OPENRGB2_WITH_QUICK3D
        true
#else
        false
#endif
    );
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
    /* Main.qml restores the NORMAL geometry; the maximized flag
       is applied here because setWindowStates() does not show a
       hidden (tray-start) window — the state takes effect when
       it is first shown. */
    if(ui_prefs.windowMaximized())
    {
        window->setWindowStates(Qt::WindowMaximized);
    }
    if(startup_task.available())
        startup_task.refresh();

    /* R-10: a competing writer that appears while the window is
       hidden gets one tray message per writer key per session.
       Declared before the tray and the Quit-script QProcess so
       they outlive every handler that captures them (a still-
       running `shutdown` can run its finished handler — and a
       nested event loop — from ~QProcess). */
    openrgb2::ui::ConflictNoticeTracker conflict_notices;
    bool ownership_msg_pending = false;
    /* R-1: the tray's quick controls (workspace, brightness,
       pause) as a data model — declared before the menu whose
       actions call back into it. */
    openrgb2::ui::TrayController tray_ctl(&bridge, &workspace_ctl);
    QList<QMenu*> tray_submenus;
    std::function<void()> quit_action;
    QMenu tray_menu;
    QSystemTrayIcon tray(app_icon);
    QProcess shutdown;
    if(tray_available)
    {
        auto show_ui = [window] { ShowRestored(window); };
        tray.setToolTip(tray_ctl.toolTip());
        tray.setContextMenu(&tray_menu);
        auto exit_ui = [window] {
            window->setProperty("exitRequested", true);
            window->close();
        };
        const QString install_dir = QCoreApplication::applicationDirPath();
        const QString stop_script = QDir(install_dir).filePath(
            QStringLiteral("scripts/stop-openrgb2.ps1"));
        if(QFileInfo::exists(stop_script))
        {
            quit_action =
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
                /* System powershell.exe, sanitised env, hidden. */
                openrgb2::ui::launch::PrepareHiddenPowerShell(shutdown);
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
                shutdown.start();
            };
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
        /* The menu: Open, the TrayController's quick controls
           (rendered from its data model), then the existing Exit
           UI / Quit actions. Rebuilt just before it shows, and on
           a model change while it is closed — never under the
           user's cursor. */
        auto rebuild_tray_menu =
            [&tray_menu, &tray_submenus, &tray_ctl, &quit_action,
             show_ui, exit_ui] {
            tray_menu.clear();
            qDeleteAll(tray_submenus);
            tray_submenus.clear();
            tray_menu.addAction(QObject::tr("Open OpenRGB2"), show_ui);
            tray_menu.addSeparator();
            RenderTrayEntries(&tray_menu, tray_ctl.menu(), &tray_ctl,
                              tray_submenus);
            tray_menu.addSeparator();
            tray_menu.addAction(QObject::tr("Exit UI (keep lighting on)"),
                                exit_ui);
            if(quit_action)
                tray_menu.addAction(QObject::tr("Quit OpenRGB2"),
                                    quit_action);
        };
        rebuild_tray_menu();
        QObject::connect(&tray_menu, &QMenu::aboutToShow,
                         &app, rebuild_tray_menu);
        /* Queued: a model change raised from inside an action's
           triggered() (a workspace pick latching the replace
           confirmation) must not delete that action mid-emit. */
        QObject::connect(&tray_ctl,
                         &openrgb2::ui::TrayController::menuChanged,
                         &app, [&tray, &tray_ctl, &tray_menu,
                                rebuild_tray_menu] {
            tray.setToolTip(tray_ctl.toolTip());
            if(!tray_menu.isVisible())
                rebuild_tray_menu();
        }, Qt::QueuedConnection);
        /* A tray workspace pick over a dirty draft latched the
           replace confirmation — it lives in the window. */
        QObject::connect(&tray_ctl,
                         &openrgb2::ui::TrayController::showWindowRequested,
                         &app, show_ui);
        QObject::connect(&tray, &QSystemTrayIcon::activated,
                         &app, [show_ui](QSystemTrayIcon::ActivationReason reason) {
            if(reason == QSystemTrayIcon::Trigger
               || reason == QSystemTrayIcon::DoubleClick)
                show_ui();
        });
        /* The background takeover notification opens the window,
           where the confirmation dialog is waiting; the R-10
           ownership notice opens it at the fix steps. */
        QObject::connect(&tray, &QSystemTrayIcon::messageClicked,
                         &app, [show_ui, window,
                                &ownership_msg_pending] {
            show_ui();
            if(ownership_msg_pending)
            {
                ownership_msg_pending = false;
                QMetaObject::invokeMethod(window, "showOwnershipFix");
            }
        });
        QObject::connect(&bridge,
                         &openrgb2::ui::EngineBridge::ownershipChanged,
                         &app, [&bridge, &tray, window,
                                &conflict_notices,
                                &ownership_msg_pending] {
            const QVariantMap own = bridge.ownership();
            const QStringList fresh = conflict_notices.Pending(own);
            if(fresh.isEmpty())
                return;
            const bool hidden = !window->isVisible()
                || window->visibility() == QWindow::Minimized;
            if(!hidden)
            {
                /* The banner already told the user. */
                conflict_notices.MarkSeen(own);
                return;
            }
            /* At sign-in the tray may not exist yet: keep the keys
               pending rather than spend them on a message nobody
               can see. */
            if(!QSystemTrayIcon::isSystemTrayAvailable()
               || !tray.isVisible())
                return;
            conflict_notices.MarkSeen(own);
            ownership_msg_pending = true;
            tray.showMessage(
                QObject::tr("Another app is controlling your lights"),
                QObject::tr("%1. Open OpenRGB2 for the steps to turn "
                            "it off - OpenRGB2 never closes it for you.")
                    .arg(fresh.join(QStringLiteral(", "))),
                QSystemTrayIcon::Warning, 10000);
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
                    ShowRestored(window);
            });
#endif
    }
    else
    {
        QObject::connect(&instance_server, &QLocalServer::newConnection,
                         &app, [&instance_server, window] {
            while(auto* client = instance_server.nextPendingConnection())
            {
                ShowRestored(window);
                client->deleteLater();
            }
        });
    }

    /* The desktop executable starts the existing managed stack.
       PowerShell is a child of the GUI process with CREATE_NO_WINDOW;
       the script starts the backend and engine with hidden windows.
       -NoUI avoids a second copy of this executable.

       Takeover is never silent (RELEASE.md / install-recovery.md):
       the first run goes WITHOUT -TakeOver. When the ownership gate
       refuses (exit 3 + "OWNERSHIP CONFLICT" in the script output)
       the competing owners are listed in a dialog and the start is
       retried with -TakeOver only after the user confirms. A
       background (sign-in) launch has no window to ask from: it
       raises a tray notification and asks when the window is first
       shown. Exit 4 = an elevated stack this process cannot verify. */
    QProcess startup;
    bool     startup_takeover = false;
    /* This run's output, captured from the child's stdout (and
       appended to startup.log here). Parsing what was captured —
       not re-reading the log from a remembered offset — survives
       the script's in-place log rotation (truncate to 0 > 1 MB). */
    QByteArray startup_output;
    bool     takeover_ask_pending = false;
    QStringList takeover_conflicts;
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
            /* System powershell.exe, sanitised env (no CLR profiler /
               user PSModulePath / Qt search vars), hidden window. */
            openrgb2::ui::launch::PrepareHiddenPowerShell(startup);
            startup.setWorkingDirectory(install_dir);
            startup.setProcessChannelMode(QProcess::MergedChannels);
            /* Tee each chunk to startup.log (opened per chunk in
               append mode, so an in-place truncate by the script
               just restarts the file) and to this run's capture
               (bounded; the conflict banner ends a short run). */
            auto drain_startup = [&startup, &startup_output, log] {
                const QByteArray chunk = startup.readAllStandardOutput();
                if(chunk.isEmpty())
                    return;
                QFile f(log);
                if(f.open(QIODevice::WriteOnly | QIODevice::Append))
                    f.write(chunk);
                constexpr qsizetype kMaxCapture = 1024 * 1024;
                startup_output += chunk;
                if(startup_output.size() > kMaxCapture)
                    startup_output.remove(
                        0, startup_output.size() - kMaxCapture);
            };
            QObject::connect(&startup, &QProcess::readyReadStandardOutput,
                             &app, drain_startup);
            auto run_start = [&startup, &startup_takeover,
                              &startup_output, script,
                              install_dir](bool take_over) {
                if(startup.state() != QProcess::NotRunning)
                    return;
                startup_takeover = take_over;
                QStringList args{QStringLiteral("-NoProfile"),
                    QStringLiteral("-NonInteractive"),
                    QStringLiteral("-ExecutionPolicy"),
                    QStringLiteral("Bypass"),
                    QStringLiteral("-File"), script,
                    QStringLiteral("-InstallDir"), install_dir};
                if(take_over)
                    args << QStringLiteral("-TakeOver");
                args << QStringLiteral("-HideWindows")
                     << QStringLiteral("-NoUI");
                startup.setArguments(args);
                startup_output.clear();
                startup.start();
            };
            /* Shown once the window can host it (now, or on the first
               show after a background start). */
            auto ask_takeover = [&takeover_conflicts, run_start, window] {
                /* R-10: each writer once, plus a short hint per
                   finding key. */
                const QString list =
                    openrgb2::ui::TakeoverConflictText(takeover_conflicts);
                QMessageBox box(QMessageBox::Warning,
                    QObject::tr("OpenRGB2 - competing RGB software"),
                    QObject::tr("Other RGB software is running and controls "
                                "the same devices:\n\n%1\n"
                                "Take over anyway? OpenRGB2 never closes "
                                "these programs - they keep running and may "
                                "fight over the lights. Choose No to close "
                                "them yourself first (recommended).")
                        .arg(list),
                    QMessageBox::Yes | QMessageBox::No);
                box.setDefaultButton(QMessageBox::No);
                if(window && window->isVisible())
                    box.setWindowModality(Qt::ApplicationModal);
                if(box.exec() == QMessageBox::Yes)
                    run_start(true);
            };
            QObject::connect(window, &QWindow::visibleChanged, &app,
                [&takeover_ask_pending, ask_takeover](bool visible) {
                if(visible && takeover_ask_pending)
                {
                    takeover_ask_pending = false;
                    QTimer::singleShot(0, ask_takeover);
                }
            });
            QObject::connect(&startup, &QProcess::errorOccurred,
                             &app, [log](QProcess::ProcessError error) {
                if(error == QProcess::FailedToStart)
                    QMessageBox::warning(nullptr, QObject::tr("OpenRGB2 startup"),
                        QObject::tr("Could not start the managed launcher.\nDetails: %1")
                            .arg(log));
            });
            QObject::connect(&startup,
                qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                &app, [log, window, &tray, tray_available,
                       &startup_takeover, &startup_output, drain_startup,
                       &takeover_conflicts, &takeover_ask_pending,
                       ask_takeover](int code, QProcess::ExitStatus status) {
                /* Pick up anything readyRead has not delivered yet. */
                drain_startup();
                if(status == QProcess::NormalExit && code == 0)
                    return;
                if(status == QProcess::NormalExit && code == 4)
                {
                    QMessageBox::warning(nullptr,
                        QObject::tr("OpenRGB2 startup"),
                        QObject::tr("OpenRGB2 appears to be already running "
                                    "with administrator rights, or a lighting "
                                    "process could not be verified from this "
                                    "launch.\n\nStart OpenRGB2 as "
                                    "administrator.\n\nDetails: %1").arg(log));
                    return;
                }
                if(status == QProcess::NormalExit && code == 3
                   && !startup_takeover
                   && openrgb2::ui::launch::ParseOwnershipConflict(
                          QString::fromLocal8Bit(startup_output),
                          &takeover_conflicts))
                {
                    if(window->isVisible())
                    {
                        ask_takeover();
                    }
                    else
                    {
                        /* Background start: never take over (and
                           never pop a dialog from nowhere) — notify,
                           ask when the user opens the window. */
                        takeover_ask_pending = true;
                        if(tray_available)
                            tray.showMessage(
                                QObject::tr("OpenRGB2 needs your confirmation"),
                                QObject::tr("Other RGB software is running, so "
                                            "lighting was not started. Open "
                                            "OpenRGB2 to review."),
                                QSystemTrayIcon::Warning, 15000);
                    }
                    return;
                }
                QMessageBox::warning(nullptr, QObject::tr("OpenRGB2 startup"),
                    QObject::tr("Lighting services could not start (exit code %1).\nDetails: %2")
                        .arg(code).arg(log));
            });
            QTimer::singleShot(0, &startup, [run_start] { run_start(false); });
        }
    }

    /* QA harness is opt-in and needs the loaded root object —
       it walks the live item tree and synthesizes input. */
    openrgb2::ui::QaServer qa_server(engine);
    if(qa_enabled && !qa_port_ok)
    {
        /* Fail closed: an unparseable/out-of-range port never
           falls back to some other port. */
        std::fprintf(stderr,
            "QA socket disabled: invalid port \"%s\" "
            "(expected 0..65535)\n",
            qa_port_text.toUtf8().constData());
    }
    else if(qa_enabled)
    {
        QString qa_err;
        if(qa_server.Start((quint16)qa_port, qa_err))
        {
            /* Every request must carry this per-launch token.
               tools/qa/qa.py reads the per-user token file. */
            const QString token_file =
                openrgb2::ui::QaServer::TokenFilePath(
                    qa_server.Port());
            QString tok_err;
            const bool wrote =
                qa_server.WriteTokenFile(token_file, tok_err);
            std::fprintf(stderr,
                "QA socket: 127.0.0.1:%u\n"
                "QA token: %s\n"
                "QA token file: %s%s%s\n",
                (unsigned)qa_server.Port(),
                qa_server.Token().toLatin1().constData(),
                QDir::toNativeSeparators(token_file)
                    .toUtf8().constData(),
                wrote ? "" : " (write failed: ",
                wrote ? "" : (tok_err + QStringLiteral(")"))
                                 .toUtf8().constData());
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
