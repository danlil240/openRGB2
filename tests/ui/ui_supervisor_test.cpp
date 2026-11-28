// SPDX-License-Identifier: GPL-2.0-or-later
//
// R-9: the UI's engine supervisor (src/ui/EngineSupervisor) with
// an injected process-exit source, a fake clock + scheduler and a
// fake state directory — no process is started or watched:
//
//   - after a successful start it watches the run file's
//     engine_pid (the PowerShell run file carries a UTF-8 BOM)
//   - an intended stop (the UI ran the stop script / sent Quit),
//     a clean exit 0 and a fresh state\openrgb2.stopping marker
//     never restart; a stale marker (> 10 min) does not block
//   - a crash restarts through the injected start, after 2 s,
//     10 s, 30 s; the 4th crash inside 10 minutes gives a notice
//     instead (and "exhausted" until the next successful start)
//   - a stop that begins while a restart is pending cancels it
//   - an exit of a pid it does not watch is ignored
//   - history/lastReason feed the Diagnostics card

#include "EngineSupervisor.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <sys/utime.h>
#include <ctime>
#include <cstdio>
#include <functional>
#include <vector>

using namespace openrgb2::ui;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

struct Pending
{
    int                   ms;
    std::function<void()> fn;
};

struct Rig
{
    QTemporaryDir                 dir;
    qint64                        now = 1000000;
    std::vector<quint32>          watched;
    std::vector<Pending>          pending;
    int                           restarts_called = 0;
    std::unique_ptr<EngineSupervisor> sup;
    QStringList                   notices;
    QStringList                   restarting;

    QString StateDir() const { return dir.path() + "/state"; }

    void WriteRun(int engine_pid)
    {
        QDir().mkpath(StateDir());
        QFile f(StateDir() + "/openrgb2.run.json");
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write("\xEF\xBB\xBF");    /* Set-Content -Encoding UTF8 */
        f.write(QByteArray("{\r\n  \"install_dir\": \"C:\\\\x\",\r\n"
                           "  \"engine_pid\": ") +
                QByteArray::number(engine_pid) +
                ",\r\n  \"backend_pid\": 77\r\n}");
    }

    void Marker(bool present, int age_minutes = 0)
    {
        const QString p = StateDir() + "/openrgb2.stopping";
        if(!present)
        {
            QFile::remove(p);
            return;
        }
        QDir().mkpath(StateDir());
        QFile f(p);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write("{\"pid\":1}");
        f.close();
        if(age_minutes > 0)
        {
            struct _utimbuf t;
            t.actime = t.modtime =
                std::time(nullptr) - age_minutes * 60;
            _wutime(reinterpret_cast<const wchar_t*>(p.utf16()), &t);
        }
    }

    void Make()
    {
        EngineSupervisor::Deps d;
        d.state_dir = StateDir();
        d.watch     = [this](quint32 pid) { watched.push_back(pid);
                                            return true; };
        d.restart   = [this] { ++restarts_called; };
        d.now_ms    = [this] { return now; };
        d.schedule  = [this](int ms, std::function<void()> fn)
                      { pending.push_back({ ms, std::move(fn) }); };
        sup = std::make_unique<EngineSupervisor>(std::move(d));
        QObject::connect(sup.get(), &EngineSupervisor::gaveUp,
                         [this](const QString& s) { notices << s; });
        QObject::connect(sup.get(), &EngineSupervisor::restarting,
                         [this](const QString& r, int)
                         { restarting << r; });
    }

    /* Fire every pending callback (the delays are asserted
       separately; time advances by the delay). */
    void Fire()
    {
        std::vector<Pending> p;
        p.swap(pending);
        for(Pending& e : p)
        {
            now += e.ms;
            e.fn();
        }
    }
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    /*---- watch after start; intended stop never restarts ---*/
    {
        Rig r;
        r.WriteRun(1234);
        r.Make();
        CHECK(r.sup->enabled());
        r.sup->onStartSucceeded();
        CHECK(r.watched.size() == 1 && r.watched[0] == 1234);
        r.sup->noteIntendedStop();
        r.sup->onEngineExited(1234, 1);       /* -Force kill */
        CHECK(r.pending.empty());
        CHECK(r.restarts_called == 0);
        CHECK(r.sup->restarts() == 0);
    }

    /*---- clean exit 0 never restarts -----------------------*/
    {
        Rig r;
        r.WriteRun(1234);
        r.Make();
        r.sup->onStartSucceeded();
        r.sup->onEngineExited(1234, 0);
        CHECK(r.pending.empty());
        CHECK(r.restarts_called == 0);
    }

    /*---- the stop marker: fresh blocks, stale does not -----*/
    {
        Rig r;
        r.WriteRun(1234);
        r.Make();
        r.sup->onStartSucceeded();
        r.Marker(true);
        r.sup->onEngineExited(1234, 0xC0000005u);
        CHECK(r.pending.empty());
        CHECK(r.restarts_called == 0);

        Rig s;
        s.WriteRun(1234);
        s.Make();
        s.sup->onStartSucceeded();
        s.Marker(true, 20);                   /* a stop that died */
        s.sup->onEngineExited(1234, 0xC0000005u);
        CHECK(s.pending.size() == 1);
    }

    /*---- unwatched pid ignored -----------------------------*/
    {
        Rig r;
        r.WriteRun(1234);
        r.Make();
        r.sup->onStartSucceeded();
        r.sup->onEngineExited(9999, 0xC0000005u);
        CHECK(r.pending.empty());
    }

    /*---- crash -> restart; 4th in 10 min -> notice ---------*/
    {
        Rig r;
        r.WriteRun(1000);
        r.Make();
        r.sup->onStartSucceeded();
        const int want_delay[] = { 2000, 10000, 30000 };
        for(int i = 0; i < 3; ++i)
        {
            const quint32 pid = 1000u + (quint32)i;
            r.sup->onEngineExited(pid, 0xC0000005u);
            CHECK(r.pending.size() == 1);
            CHECK(!r.pending.empty() && r.pending[0].ms == want_delay[i]);
            CHECK(r.restarting.size() == i + 1);
            r.Fire();
            CHECK(r.restarts_called == i + 1);
            r.WriteRun((int)pid + 1);
            r.sup->onStartSucceeded();
            CHECK(r.watched.back() == pid + 1);
            r.now += 60 * 1000;               /* a minute apart */
        }
        CHECK(r.sup->restarts() == 3);
        CHECK(r.sup->lastReason().contains(QStringLiteral("0xC0000005")));
        CHECK(r.sup->history().size() == 3);
        r.sup->onEngineExited(1003, 0xC0000005u);
        CHECK(r.pending.empty());
        CHECK(r.restarts_called == 3);
        CHECK(r.notices.size() == 1);
        CHECK(r.sup->exhausted());
        CHECK(r.sup->history().size() == 4);
        /* A later successful (manual) start re-arms; the window
           has moved on by then. */
        r.now += 11 * 60 * 1000;
        r.WriteRun(1100);
        r.sup->onStartSucceeded();
        CHECK(!r.sup->exhausted());
        r.sup->onEngineExited(1100, 3);
        CHECK(r.pending.size() == 1);
    }

    /*---- a stop that starts while a restart is pending -----*/
    {
        Rig r;
        r.WriteRun(1234);
        r.Make();
        r.sup->onStartSucceeded();
        r.sup->onEngineExited(1234, 0xC0000409u);
        CHECK(r.pending.size() == 1);
        r.Marker(true);
        r.Fire();
        CHECK(r.restarts_called == 0);

        Rig q;
        q.WriteRun(1234);
        q.Make();
        q.sup->onStartSucceeded();
        q.sup->onEngineExited(1234, 1);
        q.sup->noteIntendedStop();
        q.Fire();
        CHECK(q.restarts_called == 0);
    }

    /*---- no run file / no pid: nothing watched -------------*/
    {
        Rig r;
        r.Make();
        r.sup->onStartSucceeded();
        CHECK(r.watched.empty());
    }

    std::printf("ui_supervisor_test: %d checks, %d failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
