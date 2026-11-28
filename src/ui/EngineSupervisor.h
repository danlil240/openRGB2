/*---------------------------------------------------------*\
|| EngineSupervisor.h                                        ||
||                                                           ||
||   R-9: the packaged UI supervises the engine it started  ||
||   (managed mode only).                                    ||
||                                                           ||
||   After a successful managed start it reads engine_pid    ||
||   from state\openrgb2.run.json and watches that process  ||
||   (SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, image ||
||   verified == <install>\openrgb2-engine.exe; passive —    ||
||   nothing is ever signaled). When it exits it restarts    ||
||   the stack through the SAME start script the UI already  ||
||   runs (ownership gate, logging, never a second engine)  ||
||   ONLY when all hold:                                     ||
||     1. the exit code is not 0 (the ordered QUIT path     ||
||        returns 0 from main)                              ||
||     2. this UI did not run the stop script or send QUIT  ||
||     3. no stop is in progress: state\openrgb2.stopping   ||
||        is absent or older than 10 minutes                ||
||   Budget: 3 restarts per 10 minutes, after 2 s / 10 s /  ||
||   30 s. The 4th crash inside the window gives up with a  ||
||   persistent notice (gaveUp) until the next successful   ||
||   start.                                                  ||
||                                                           ||
||   Every seam is injectable (watch, restart, clock,       ||
||   scheduler) so ui_supervisor_test drives it without a   ||
||   process; empty seams are the live implementations.    ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "../supervise/RestartLedger.h"

#include <QObject>
#include <QString>
#include <QVariantList>

#include <chrono>
#include <functional>
#include <memory>

namespace openrgb2
{
namespace ui
{

class EngineSupervisor : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled CONSTANT)
    Q_PROPERTY(int restarts READ restarts NOTIFY changed)
    Q_PROPERTY(bool exhausted READ exhausted NOTIFY changed)
    Q_PROPERTY(QString lastReason READ lastReason NOTIFY changed)
    Q_PROPERTY(QString lastAt READ lastAt NOTIFY changed)
    Q_PROPERTY(QVariantList history READ history NOTIFY changed)

public:
    struct Deps
    {
        QString state_dir;      /* <install>\state                 */
        QString engine_exe;     /* image the watched pid must have */
        /* Start watching `pid`; false when it cannot be opened
           or is not our engine image. Live: OpenProcess +
           QWinEventNotifier, delivering onEngineExited. */
        std::function<bool(quint32 pid)>         watch;
        /* Re-run the managed start (main.cpp's run_start). */
        std::function<void()>                    restart;
        /* Monotonic ms (live: QDeadlineTimer / steady clock). */
        std::function<qint64()>                  now_ms;
        /* Run `fn` after `ms` (live: QTimer::singleShot). */
        std::function<void(int ms, std::function<void()> fn)> schedule;
    };

    explicit EngineSupervisor(Deps deps, QObject* parent = nullptr);
    ~EngineSupervisor() override;

    bool         enabled() const   { return true; }
    int          restarts() const  { return (int)ledger_.Total(); }
    bool         exhausted() const { return exhausted_; }
    QString      lastReason() const { return last_reason_; }
    QString      lastAt() const    { return last_at_; }
    QVariantList history() const   { return history_; }

    /* The managed start finished with exit 0: watch the run
       file's engine. Re-arms after a give-up. */
    void onStartSucceeded();
    /* The watched process ended (from the watcher). */
    void onEngineExited(quint32 pid, quint32 exit_code);
    /* This UI ran the stop script or sent QUIT: whatever exits
       next is intended. */
    void noteIntendedStop();

    /* True while a fresh (< 10 min) stop marker exists. */
    bool StopInProgress() const;

signals:
    void changed();
    /* A restart is scheduled (tray message + Diagnostics). */
    void restarting(const QString& reason, int delay_ms);
    /* The 4th crash in the window: no more restarts. */
    void gaveUp(const QString& detail);

private:
    void AddHistory(const QString& action, const QString& reason,
                    const QString& detail);
    quint32 ReadEnginePid() const;

    using Ledger = supervise::BasicRestartLedger<std::chrono::steady_clock>;
    Ledger::TimePoint Now() const;

    Deps         deps_;
    Ledger       ledger_;
    quint32      watched_pid_ = 0;
    bool         intended_stop_ = false;
    bool         exhausted_ = false;
    QString      last_reason_;
    QString      last_at_;
    QVariantList history_;
    struct LiveWatch;
    std::unique_ptr<LiveWatch> live_;
};

} /* namespace ui */
} /* namespace openrgb2 */
