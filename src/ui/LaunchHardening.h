/*---------------------------------------------------------*\
||| LaunchHardening.h — openrgb2-ui                           ||
|||                                                           ||
|||   Process-boundary policy for a UI that usually runs      ||
|||   ELEVATED (the managed stack): which user-controlled     ||
|||   environment never reaches Qt's module search or the     ||
|||   PowerShell children, whether the QA automation socket   ||
|||   may open, and how the launcher's ownership-conflict     ||
|||   output is parsed. Pure/QtCore-only so it is unit-tested ||
|||   (tests/ui/ui_launch_hardening_test.cpp).                ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#pragma once

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

class QProcess;

namespace openrgb2::ui::launch
{

/* Elevation probe usable before QApplication exists. Unknown
   counts as elevated (fail closed). Always false off Windows. */
bool ProcessIsElevated();

/* Env vars that steer Qt's plugin / QML-import / style search.
   An elevated process must not honour them: they are inherited
   from the medium-integrity user environment. */
const QStringList& ModuleSearchEnvVars();

/* qunsetenv() every ModuleSearchEnvVars() entry — call before
   QApplication/QML load anything. */
void ScrubModuleSearchEnv();

/* The QA automation socket synthesizes input and evaluates
   JS (eval can reach workspace.writeLocalFile), so it must
   never be served by an elevated UI — a medium-integrity
   process could otherwise read the token file and drive an
   admin process (UIPI bypass, admin file writes). The managed
   launch (explicit --managed, incl. the sign-in task's
   --managed --background) is refused too: it is (or is about
   to become) the elevated stack. */
enum class QaDecision
{
    NotRequested,
    Allowed,
    RefusedElevated,
    RefusedManaged,
};
QaDecision DecideQa(bool requested, bool explicitManaged, bool elevated);
/* One stderr line for a refusal; empty for NotRequested/Allowed. */
QString QaRefusalMessage(QaDecision decision);

/* Environment for PowerShell children of the (elevated) UI:
   `base` minus CLR profiler hooks, user PSModulePath (reset to
   the system module dirs) and the Qt search vars. */
QProcessEnvironment SanitizedChildEnvironment(
    const QProcessEnvironment& base);
QProcessEnvironment SanitizedChildEnvironment();
/* The system module dirs PSModulePath is reset to (resolved
   from the OS, never from %SystemRoot%/%ProgramFiles%). */
QString SystemPsModulePath();
/* Absolute Windows PowerShell path from the system directory —
   never a PATH / current-directory lookup. */
QString SystemPowerShellPath();
/* Program + sanitised environment + hidden window for a
   PowerShell child. Arguments are the caller's. */
void PrepareHiddenPowerShell(QProcess& process);

/* Absolute explorer.exe from the Windows directory — never a
   PATH / current-directory / app-directory lookup. */
QString SystemExplorerPath();

/* Audit U-6: a folder path that came from the engine (e.g.
   workspace_dir) before the (possibly elevated) UI hands it to
   the shell. Accepted only as an absolute drive-letter path
   ("X:\..." or "X:/...") to an EXISTING directory — no URL
   scheme, no UNC / device path (the syntactic checks run
   before any filesystem call, so a \\host\share never even
   triggers an SMB lookup), no wildcard / quote / stream
   characters. On success `native` is the cleaned native path;
   on refusal `why` says what failed. */
bool ValidateShellFolder(const QString& reported, QString* native,
                         QString* why);

/* The start-openrgb2.ps1 ownership-gate contract: a refused
   run exits 3 and prints "OWNERSHIP CONFLICT"; each competitor
   is one line containing "ownership CONFLICT:". Returns true
   when the banner is present and fills `conflicts` (trimmed). */
bool ParseOwnershipConflict(const QString& output,
                            QStringList* conflicts);

} /* namespace openrgb2::ui::launch */
