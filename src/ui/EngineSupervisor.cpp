/*---------------------------------------------------------*\
|| EngineSupervisor.cpp — see EngineSupervisor.h.            ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "EngineSupervisor.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QVariantMap>

#ifdef Q_OS_WIN
#include <QWinEventNotifier>
#include <windows.h>
#endif

namespace openrgb2
{
namespace ui
{

namespace
{

supervise::BasicRestartLedger<std::chrono::steady_clock>::Budget
EngineBudget()
{
    supervise::BasicRestartLedger<std::chrono::steady_clock>::Budget b;
    b.max_restarts = 3;
    b.window       = std::chrono::minutes(10);
    b.backoff      = { std::chrono::seconds(2), std::chrono::seconds(10),
                       std::chrono::seconds(30) };
    return b;
}

QString ExitText(quint32 code)
{
    /* NTSTATUS-style codes read better in hex (0xC0000005). */
    if(code >= 0x80000000u)
    {
        return QStringLiteral("0x%1")
            .arg(code, 8, 16, QLatin1Char('0')).toUpper()
            .replace(QStringLiteral("0X"), QStringLiteral("0x"));
    }
    return QString::number(code);
}

} /* anonymous namespace */

/* The live process watch: one handle + notifier at a time. */
struct EngineSupervisor::LiveWatch
{
#ifdef Q_OS_WIN
    HANDLE             handle   = nullptr;
    QWinEventNotifier* notifier = nullptr;
    quint32            pid      = 0;

    void Reset()
    {
        if(notifier)
        {
            notifier->setEnabled(false);
            notifier->deleteLater();
            notifier = nullptr;
        }
        if(handle)
        {
            CloseHandle(handle);
            handle = nullptr;
        }
        pid = 0;
    }
    ~LiveWatch() { Reset(); }
#endif
};

EngineSupervisor::EngineSupervisor(Deps deps, QObject* parent)
    : QObject(parent),
      deps_(std::move(deps)),
      ledger_(EngineBudget()),
      live_(std::make_unique<LiveWatch>())
{
    if(!deps_.now_ms)
    {
        auto timer = std::make_shared<QElapsedTimer>();
        timer->start();
        deps_.now_ms = [timer] { return timer->elapsed(); };
    }
    if(!deps_.schedule)
    {
        deps_.schedule = [this](int ms, std::function<void()> fn)
        { QTimer::singleShot(ms, this, std::move(fn)); };
    }
    if(!deps_.watch)
    {
        deps_.watch = [this](quint32 pid) -> bool
        {
#ifdef Q_OS_WIN
            live_->Reset();
            HANDLE h = OpenProcess(
                SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                FALSE, pid);
            if(!h)
            {
                return false;
            }
            /* Verified-own: only our engine image is watched — a
               recycled pid never becomes "the engine". */
            wchar_t img[1024];
            DWORD   n = 1024;
            auto norm = [](const QString& p)
            { return QDir::cleanPath(QDir::fromNativeSeparators(p)); };
            if(!QueryFullProcessImageNameW(h, 0, img, &n) ||
               deps_.engine_exe.isEmpty() ||
               norm(QString::fromWCharArray(img, (int)n))
                   .compare(norm(deps_.engine_exe),
                            Qt::CaseInsensitive) != 0)
            {
                CloseHandle(h);
                return false;
            }
            live_->handle   = h;
            live_->pid      = pid;
            live_->notifier = new QWinEventNotifier(h, this);
            connect(live_->notifier, &QWinEventNotifier::activated,
                    this, [this](HANDLE hh) {
                DWORD code = 0;
                GetExitCodeProcess(hh, &code);
                const quint32 p = live_->pid;
                live_->Reset();
                onEngineExited(p, (quint32)code);
            });
            return true;
#else
            Q_UNUSED(pid);
            return false;
#endif
        };
    }
}

EngineSupervisor::~EngineSupervisor() = default;

EngineSupervisor::Ledger::TimePoint EngineSupervisor::Now() const
{
    return Ledger::TimePoint(std::chrono::milliseconds(deps_.now_ms()));
}

quint32 EngineSupervisor::ReadEnginePid() const
{
    QFile f(QDir(deps_.state_dir).filePath(
        QStringLiteral("openrgb2.run.json")));
    if(!f.open(QIODevice::ReadOnly))
    {
        return 0;
    }
    QByteArray data = f.readAll();
    /* Windows PowerShell's Set-Content -Encoding UTF8 writes a
       BOM, which QJsonDocument refuses. */
    if(data.startsWith("\xEF\xBB\xBF"))
    {
        data.remove(0, 3);
    }
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if(!doc.isObject())
    {
        return 0;
    }
    const qint64 pid = (qint64)doc.object()
        .value(QStringLiteral("engine_pid")).toDouble(0);
    return pid > 0 && pid <= 0xFFFFFFFFll ? (quint32)pid : 0;
}

bool EngineSupervisor::StopInProgress() const
{
    const QFileInfo fi(QDir(deps_.state_dir).filePath(
        QStringLiteral("openrgb2.stopping")));
    if(!fi.exists())
    {
        return false;
    }
    /* A marker older than 10 minutes is a stop that died. */
    return fi.lastModified().secsTo(QDateTime::currentDateTime()) < 600;
}

void EngineSupervisor::AddHistory(const QString& action,
                                  const QString& reason,
                                  const QString& detail)
{
    const QString at = QDateTime::currentDateTimeUtc()
                           .toString(Qt::ISODate);
    history_.append(QVariantMap{
        { QStringLiteral("at"),     at     },
        { QStringLiteral("source"), QStringLiteral("engine") },
        { QStringLiteral("action"), action },
        { QStringLiteral("reason"), reason },
        { QStringLiteral("detail"), detail } });
    while(history_.size() > 16)
    {
        history_.removeFirst();
    }
    last_at_ = at;
    /* The log: one line per action in state\supervisor.log,
       beside the engine's backend-supervisor lines. */
    if(!deps_.state_dir.isEmpty())
    {
        QFile f(QDir(deps_.state_dir).filePath(
            QStringLiteral("supervisor.log")));
        if(f.open(QIODevice::WriteOnly | QIODevice::Append))
        {
            f.write(QStringLiteral("%1 ui engine-supervisor %2 (%3) %4\r\n")
                        .arg(at, action, reason, detail).toUtf8());
        }
    }
    emit changed();
}

void EngineSupervisor::onStartSucceeded()
{
    intended_stop_ = false;
    if(exhausted_)
    {
        exhausted_ = false;
        emit changed();
    }
    const quint32 pid = ReadEnginePid();
    if(pid == 0)
    {
        return;
    }
    if(deps_.watch && deps_.watch(pid))
    {
        watched_pid_ = pid;
    }
}

void EngineSupervisor::noteIntendedStop()
{
    intended_stop_ = true;
}

void EngineSupervisor::onEngineExited(quint32 pid, quint32 exit_code)
{
    if(pid == 0 || pid != watched_pid_)
    {
        return;
    }
    watched_pid_ = 0;
    if(exit_code == 0 || intended_stop_ || StopInProgress())
    {
        return;     /* an ordered or intended stop */
    }
    const QString reason =
        tr("engine crashed (exit %1)").arg(ExitText(exit_code));
    last_reason_ = reason;
    const Ledger::TimePoint now = Now();
    if(ledger_.Exhausted(now))
    {
        exhausted_ = true;
        const QString detail =
            tr("The lighting engine crashed %1 times in 10 minutes and "
               "was not restarted again. Restart OpenRGB2; if it keeps "
               "crashing, restore a workspace backup and keep the "
               "dumps in state\\crash.")
                .arg(ledger_.CountInWindow(now) + 1);
        AddHistory(QStringLiteral("gave_up"), reason, detail);
        emit gaveUp(detail);
        return;
    }
    const int delay_ms = (int)std::chrono::duration_cast<
        std::chrono::milliseconds>(ledger_.NextDelay(now)).count();
    ledger_.Record(now);
    AddHistory(QStringLiteral("restart"), reason,
               tr("restarting in %1 s").arg(delay_ms / 1000));
    emit restarting(reason, delay_ms);
    deps_.schedule(delay_ms, [this] {
        if(intended_stop_ || StopInProgress())
        {
            AddHistory(QStringLiteral("restart_cancelled"),
                       tr("a stop began"), QString());
            return;
        }
        if(deps_.restart)
        {
            deps_.restart();
        }
    });
}

} /* namespace ui */
} /* namespace openrgb2 */
