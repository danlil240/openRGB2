#include "StartupTaskController.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace openrgb2::ui
{

StartupTaskController::StartupTaskController(const QString& installDir,
                                             QObject* parent)
    : QObject(parent), install_dir_(installDir),
      script_(QDir(installDir).filePath(
          QStringLiteral("scripts/startup-task.ps1")))
{
    connect(&process_, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
        if(error != QProcess::FailedToStart)
            return;
        error_ = tr("Could not start Task Scheduler helper: %1")
                     .arg(process_.errorString());
        busy_ = false;
        emit stateChanged();
    });
    connect(&process_,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus status) {
        const QByteArray out = process_.readAllStandardOutput().trimmed();
        const QString err = QString::fromLocal8Bit(
            process_.readAllStandardError()).trimmed();
        QJsonParseError parse;
        const QJsonDocument doc = QJsonDocument::fromJson(out, &parse);
        if(status != QProcess::NormalExit || code != 0)
            error_ = err.isEmpty()
                ? tr("Task Scheduler command failed (exit %1)").arg(code)
                : err;
        else if(parse.error != QJsonParseError::NoError
                || !doc.isObject()
                || !doc.object().value(QStringLiteral("enabled")).isBool())
            error_ = tr("Task Scheduler returned an invalid status");
        else
        {
            enabled_ = doc.object().value(QStringLiteral("enabled")).toBool();
            error_.clear();
        }
        busy_ = false;
        emit stateChanged();
    });
}

bool StartupTaskController::available() const
{
#ifdef Q_OS_WIN
    return QFileInfo::exists(script_);
#else
    return false;
#endif
}

void StartupTaskController::refresh()
{
    Run(QStringLiteral("status"));
}

void StartupTaskController::setEnabled(bool on)
{
    Run(on ? QStringLiteral("enable") : QStringLiteral("disable"));
}

void StartupTaskController::Run(const QString& mode)
{
    if(busy_)
        return;
    if(!available())
    {
        error_ = tr("Startup task helper is unavailable");
        emit stateChanged();
        return;
    }
    busy_ = true;
    error_.clear();
    emit stateChanged();
    process_.setProgram(QStringLiteral("powershell.exe"));
    process_.setArguments({QStringLiteral("-NoProfile"),
        QStringLiteral("-NonInteractive"),
        QStringLiteral("-ExecutionPolicy"),
        QStringLiteral("Bypass"),
        QStringLiteral("-File"), script_,
        QStringLiteral("-Mode"), mode,
        QStringLiteral("-InstallDir"), install_dir_});
    process_.setWorkingDirectory(install_dir_);
#ifdef Q_OS_WIN
    process_.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments* args) {
            args->flags |= CREATE_NO_WINDOW;
            args->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
            args->startupInfo->wShowWindow = SW_HIDE;
        });
#endif
    process_.start();
}

} /* namespace openrgb2::ui */
