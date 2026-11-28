/*---------------------------------------------------------*\
||| LaunchHardening.cpp — openrgb2-ui                         ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#include "LaunchHardening.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#endif

namespace openrgb2::ui::launch
{

bool ProcessIsElevated()
{
#ifdef Q_OS_WIN
    HANDLE token = nullptr;
    TOKEN_ELEVATION elevation{};
    DWORD bytes = 0;
    const bool known =
        OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)
        && GetTokenInformation(token, TokenElevation, &elevation,
                               sizeof(elevation), &bytes);
    if(token)
        CloseHandle(token);
    return !known || elevation.TokenIsElevated;
#else
    return false;
#endif
}

const QStringList& ModuleSearchEnvVars()
{
    static const QStringList kVars{
        QStringLiteral("QT_PLUGIN_PATH"),
        QStringLiteral("QML_IMPORT_PATH"),
        QStringLiteral("QML2_IMPORT_PATH"),
        QStringLiteral("QT_QPA_PLATFORM_PLUGIN_PATH"),
        QStringLiteral("QT_QUICK_CONTROLS_STYLE_PATH"),
        QStringLiteral("QT_QPA_PLATFORMTHEME"),
        QStringLiteral("QT_STYLE_OVERRIDE"),
        QStringLiteral("QT_QUICK_CONTROLS_STYLE"),
    };
    return kVars;
}

void ScrubModuleSearchEnv()
{
    for(const QString& v : ModuleSearchEnvVars())
        qunsetenv(v.toLatin1().constData());
}

QaDecision DecideQa(bool requested, bool explicitManaged, bool elevated)
{
    if(!requested)
        return QaDecision::NotRequested;
    if(elevated)
        return QaDecision::RefusedElevated;
    if(explicitManaged)
        return QaDecision::RefusedManaged;
    return QaDecision::Allowed;
}

QString QaRefusalMessage(QaDecision decision)
{
    switch(decision)
    {
    case QaDecision::RefusedElevated:
        return QStringLiteral(
            "QA socket refused: this process is elevated (administrator); "
            "the QA harness only runs in a non-elevated, unmanaged UI");
    case QaDecision::RefusedManaged:
        return QStringLiteral(
            "QA socket refused: managed launch (--managed); the QA "
            "harness only runs in a non-elevated, unmanaged UI");
    case QaDecision::NotRequested:
    case QaDecision::Allowed:
        break;
    }
    return QString();
}

namespace
{
#ifdef Q_OS_WIN
QString SystemDir()
{
    wchar_t buf[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(buf, MAX_PATH);
    if(n == 0 || n >= MAX_PATH)
        return QStringLiteral("C:\\Windows\\System32");
    return QString::fromWCharArray(buf, (int)n);
}

QString ProgramFilesDir()
{
    PWSTR path = nullptr;
    QString out;
    if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0,
                                      nullptr, &path)) && path)
        out = QString::fromWCharArray(path);
    if(path)
        CoTaskMemFree(path);
    if(out.isEmpty())
        out = QStringLiteral("C:\\Program Files");
    return out;
}
#endif
} /* anonymous namespace */

QString SystemPowerShellPath()
{
#ifdef Q_OS_WIN
    return QDir::toNativeSeparators(SystemDir()
        + QStringLiteral("\\WindowsPowerShell\\v1.0\\powershell.exe"));
#else
    return QStringLiteral("powershell");
#endif
}

QString SystemPsModulePath()
{
#ifdef Q_OS_WIN
    return QDir::toNativeSeparators(ProgramFilesDir()
               + QStringLiteral("\\WindowsPowerShell\\Modules"))
        + QLatin1Char(';')
        + QDir::toNativeSeparators(SystemDir()
               + QStringLiteral("\\WindowsPowerShell\\v1.0\\Modules"));
#else
    return QString();
#endif
}

QProcessEnvironment SanitizedChildEnvironment(
    const QProcessEnvironment& base)
{
    QProcessEnvironment env = base;
    /* CLR profiler hooks load an arbitrary DLL into every .NET
       process — PowerShell included — from a user-set path. */
    static const char* const kDrop[] = {
        "COR_ENABLE_PROFILING",
        "COR_PROFILER",
        "COR_PROFILER_PATH",
        "COR_PROFILER_PATH_32",
        "COR_PROFILER_PATH_64",
        "CORECLR_ENABLE_PROFILING",
        "CORECLR_PROFILER",
        "CORECLR_PROFILER_PATH",
        "CORECLR_PROFILER_PATH_32",
        "CORECLR_PROFILER_PATH_64",
        "PSModulePath",
    };
    for(const char* v : kDrop)
        env.remove(QString::fromLatin1(v));
    for(const QString& v : ModuleSearchEnvVars())
        env.remove(v);
    const QString modules = SystemPsModulePath();
    if(!modules.isEmpty())
        env.insert(QStringLiteral("PSModulePath"), modules);
    return env;
}

QProcessEnvironment SanitizedChildEnvironment()
{
    return SanitizedChildEnvironment(
        QProcessEnvironment::systemEnvironment());
}

QString SystemExplorerPath()
{
#ifdef Q_OS_WIN
    wchar_t buf[MAX_PATH] = {};
    const UINT n = GetWindowsDirectoryW(buf, MAX_PATH);
    const QString win = (n == 0 || n >= MAX_PATH)
        ? QStringLiteral("C:\\Windows")
        : QString::fromWCharArray(buf, (int)n);
    return QDir::toNativeSeparators(win
        + QStringLiteral("\\explorer.exe"));
#else
    return QString();
#endif
}

bool ValidateShellFolder(const QString& reported, QString* native,
                         QString* why)
{
    auto refuse = [why](const QString& reason)
    {
        if(why)
            *why = reason;
        return false;
    };
    /* Syntactic gate first — nothing below touches the
       filesystem until the string is a plain local drive path
       (a UNC path would make even isDir() reach out over SMB). */
    const bool drive_path = reported.size() >= 3
        && reported.at(0).isLetter() && reported.at(0).unicode() < 0x80
        && reported.at(1) == QLatin1Char(':')
        && (reported.at(2) == QLatin1Char('\\')
            || reported.at(2) == QLatin1Char('/'));
    if(!drive_path)
        return refuse(QStringLiteral(
            "not an absolute local drive path"));
    for(int i = 0; i < reported.size(); ++i)
    {
        const QChar c = reported.at(i);
        if(c.unicode() < 0x20
           || (i > 1 && c == QLatin1Char(':'))
           || QStringLiteral("*?\"<>|").contains(c))
            return refuse(QStringLiteral(
                "path contains a reserved character"));
    }
    const QString clean = QDir::cleanPath(reported);
    const QFileInfo info(clean);
    if(!info.isDir())
        return refuse(QStringLiteral("not an existing folder"));
    if(native)
        *native = QDir::toNativeSeparators(clean);
    if(why)
        why->clear();
    return true;
}

void PrepareHiddenPowerShell(QProcess& process)
{
    process.setProgram(SystemPowerShellPath());
    process.setProcessEnvironment(SanitizedChildEnvironment());
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments* args) {
            args->flags |= CREATE_NO_WINDOW;
            args->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
            args->startupInfo->wShowWindow = SW_HIDE;
        });
#endif
}

bool ParseOwnershipConflict(const QString& output, QStringList* conflicts)
{
    if(!output.contains(QLatin1String("OWNERSHIP CONFLICT")))
        return false;
    if(conflicts)
    {
        conflicts->clear();
        for(const QString& line : output.split(QLatin1Char('\n')))
        {
            if(line.contains(QLatin1String("ownership CONFLICT:")))
                *conflicts << line.trimmed();
        }
    }
    return true;
}

} /* namespace openrgb2::ui::launch */
