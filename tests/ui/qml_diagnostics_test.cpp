// SPDX-License-Identifier: GPL-2.0-or-later
//
// Devices & Diagnostics page test (Task 4.7) — the REAL
// DevicesPage.qml loaded headless (offscreen) against an
// offline bridge, proving the honesty contract renders:
//
//   - all four rate fields exist with their own objectNames
//     and labels (configured / physical / delivery / preview —
//     the plan's four distinct rates, never conflated)
//   - unmeasured and unknown values render as "unmeasured" /
//     "not configured" — never zeros or fabricated numbers
//   - the ownership surface reports honest absence when the
//     engine published nothing (no fake "clean" state)
//   - export is gated on a live engine connection
//
// CHECK-macro main like the other UI suites — no framework.

#include "DiagnosticsController.h"
#include "EngineBridge.h"
#include "EngineSupervisor.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QSet>
#include <QQuickStyle>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>
#include <cstring>
#include <memory>

using namespace openrgb2;

static void CollectVisibleByName(QObject* root, const QString& name,
                                 QSet<QObject*>& out)
{
    if(!root)
        return;
    if(root->objectName() == name && root->property("visible").toBool())
        out.insert(root);
    for(QObject* c : root->children())
        CollectVisibleByName(c, name, out);
    if(auto* item = qobject_cast<QQuickItem*>(root))
        for(QQuickItem* c : item->childItems())
            CollectVisibleByName(c, name, out);
}

/* Repeater delegates are visual children (parentItem), not QObject
   children — findChild never sees them. Walk both trees. */
static QObject* FindByName(QObject* root, const QString& name)
{
    if(!root)
        return nullptr;
    if(root->objectName() == name)
        return root;
    for(QObject* c : root->children())
        if(QObject* f = FindByName(c, name))
            return f;
    if(auto* item = qobject_cast<QQuickItem*>(root))
        for(QQuickItem* c : item->childItems())
            if(c->parent() != root)
                if(QObject* f = FindByName(c, name))
                    return f;
    return nullptr;
}

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QQmlEngine engine;
    /* Never start()ed — the bridge sits in its honest
       disconnected state and every derived view is
       unmeasured/unreported. */
    ui::EngineBridge bridge;
    ui::DiagnosticsController diag(&bridge);
    engine.rootContext()->setContextProperty(
        QStringLiteral("bridge"), &bridge);
    engine.rootContext()->setContextProperty(
        QStringLiteral("diagCtl"), &diag);

    QQmlComponent comp(&engine,
        QUrl(QStringLiteral(
            "qrc:/qt/qml/OpenRGB2/UI/pages/DevicesPage.qml")));
    if(comp.isError())
    {
        std::printf("FAIL load DevicesPage.qml:\n%s\n",
                    comp.errorString().toUtf8().constData());
        return 1;
    }
    std::unique_ptr<QObject> page(
        comp.create(engine.rootContext()));
    CHECK(page != nullptr);
    if(!page)
    {
        std::printf("%d checks, %d failures\n", checks, failures);
        return 1;
    }

    /*---- all four rate fields render with honest values ------*/
    const char* names[] = {
        "rateConfigured", "ratePhysical",
        "rateDelivery",   "ratePreview",
    };
    for(const char* n : names)
    {
        QObject* field = page->findChild<QObject*>(n);
        CHECK(field != nullptr);
    }

    QObject* configured = page->findChild<QObject*>("rateConfigured");
    QObject* physical   = page->findChild<QObject*>("ratePhysical");
    QObject* delivery   = page->findChild<QObject*>("rateDelivery");
    QObject* preview    = page->findChild<QObject*>("ratePreview");
    if(configured && physical && delivery && preview)
    {
        /* Unmeasured/unknown render as such — never "0.0"
           or a fabricated number. */
        CHECK(configured->property("text").toString()
                  .contains(QStringLiteral("not configured")));
        CHECK(physical->property("text").toString()
                  .contains(QStringLiteral("unmeasured")));
        CHECK(delivery->property("text").toString()
                  .contains(QStringLiteral("unmeasured")));
        CHECK(preview->property("text").toString()
                  .contains(QStringLiteral("unmeasured")));
    }

    /*---- ownership: honest absence, not a fake clean scan ----*/
    QObject* summary = page->findChild<QObject*>("ownershipSummary");
    CHECK(summary != nullptr);
    if(summary)
    {
        const QString t = summary->property("text").toString();
        CHECK(t.contains(QStringLiteral("no ownership report"))
              || t.contains(QStringLiteral("scan not run")));
        /* an empty report must never read as "no competing
           owners detected" — that's a scan RESULT, not the
           absence of one */
        CHECK(!t.contains(QStringLiteral("no competing")));
    }

    /*---- export gated on a live connection --------------------*/
    QObject* exportBtn = page->findChild<QObject*>("exportButton");
    CHECK(exportBtn != nullptr);
    if(exportBtn)
    {
        CHECK(!exportBtn->property("enabled").toBool());
    }

    /*---- R-10: guided steps per writer key --------------------*/
    {
        QString     launched_program;
        QStringList launched_args;
        diag.setShellLauncherForTesting(
            [&](const QString& program, const QStringList& args)
            {
                launched_program = program;
                launched_args    = args;
                return true;
            });
        auto conflict = [](const char* owner, const char* key)
        {
            QVariantMap c;
            c.insert(QStringLiteral("owner"), QString::fromUtf8(owner));
            c.insert(QStringLiteral("key"), QString::fromUtf8(key));
            c.insert(QStringLiteral("exe"), QString());
            c.insert(QStringLiteral("pid"), 0);
            c.insert(QStringLiteral("detail"), QStringLiteral("d"));
            return QVariant(c);
        };
        QVariantMap rep;
        rep.insert(QStringLiteral("scanned"), true);
        rep.insert(QStringLiteral("conflicts"), QVariantList{
            conflict("Windows Dynamic Lighting", "dynamic_lighting"),
            conflict("LampArray service (Dynamic Lighting)",
                     "lamparray"),
            conflict("Logitech G Hub / Options", "logitech"),
            /* G Hub runs several matching processes */
            conflict("Logitech G Hub / Options", "logitech"),
            conflict("Some Future RGB", "future_key") });
        rep.insert(QStringLiteral("notices"), QVariantList{});
        bridge.setOwnershipForTesting(rep);
        QCoreApplication::processEvents();

        auto steps_text = [&page](const char* key)
        {
            QObject* g = FindByName(page.get(),
                QStringLiteral("ownershipGuide_") +
                QString::fromUtf8(key));
            return g ? g->property("stepsText").toString()
                     : QStringLiteral("<no guide>");
        };
        const QString dl = steps_text("dynamic_lighting");
        CHECK(dl.contains(QStringLiteral("Dynamic Lighting")) &&
              dl.contains(QStringLiteral("Use Dynamic Lighting on my devices")));
        const QString la = steps_text("lamparray");
        CHECK(la.contains(QStringLiteral("sc stop logi_lamparray_service")));
        CHECK(la.contains(
            QStringLiteral("sc config logi_lamparray_service start= demand")));
        CHECK(la.contains(QStringLiteral("Windows Update")));
        /* R-10 review: one guide per writer key, not per process */
        {
            QSet<QObject*> guides;
            CollectVisibleByName(page.get(),
                QStringLiteral("ownershipGuide_logitech"), guides);
            CHECK(guides.size() == 1);
        }
        const QString lg = steps_text("logitech");
        CHECK(lg.contains(QStringLiteral("G HUB")) &&
              lg.contains(QStringLiteral("startup")));
        /* an unknown key still gets steps, never a blank */
        const QString fu = steps_text("future_key");
        CHECK(fu.contains(QStringLiteral("Some Future RGB")));

        /* "Open Settings" exists for Dynamic Lighting and goes
           through the whitelisted launcher */
        QObject* settingsBtn = FindByName(page.get(),
            QStringLiteral("openLightingSettingsButton"));
        CHECK(settingsBtn != nullptr);
        if(settingsBtn)
        {
            QMetaObject::invokeMethod(settingsBtn, "clicked");
            CHECK(launched_program.endsWith(
                      QStringLiteral("explorer.exe"),
                      Qt::CaseInsensitive));
            CHECK(launched_args ==
                  QStringList{ QStringLiteral(
                      "ms-settings:personalization-lighting") });
        }
        QObject* servicesBtn = FindByName(page.get(),
            QStringLiteral("openServicesButton"));
        CHECK(servicesBtn != nullptr);
        if(servicesBtn)
        {
            launched_args.clear();
            QMetaObject::invokeMethod(servicesBtn, "clicked");
            CHECK(launched_args.size() == 1 &&
                  launched_args[0].endsWith(
                      QStringLiteral("services.msc"),
                      Qt::CaseInsensitive));
            /* R-10 review: mmc.exe by absolute path (no shell), so
               the console keeps the UI's elevation and can stop
               the service; explorer would hand it to the
               de-elevated shell */
            CHECK(launched_program.endsWith(
                      QStringLiteral("\\System32\\mmc.exe"),
                      Qt::CaseInsensitive));
        }
        /* only the whitelisted targets ever launch */
        launched_args.clear();
        CHECK(!diag.openOwnershipTarget(QStringLiteral("cmd.exe")));
        CHECK(launched_args.isEmpty());

        /* the copy buttons put the exact command on the clipboard */
        CHECK(diag.copyText(
            QStringLiteral("sc stop logi_lamparray_service")));

        /* E-5 is merged: no "restart the engine" advice */
        QObject* note = page->findChild<QObject*>("ownershipReportOnlyNote");
        CHECK(note != nullptr &&
              !note->property("text").toString().contains(
                  QStringLiteral("restart the engine")) &&
              note->property("text").toString().contains(
                  QStringLiteral("re-applies")));
    }

    /*---- engine state reads the LIVE report, not WELCOME -------
       A UI that connects while the engine is still starting gets
       WELCOME running:false; the STATE report refreshed every 2 s
       says running:true. The page showed "not running" for the
       whole session. */
    {
        auto state = [&page](bool connected, const QVariantMap& info,
                             const QVariantMap& report)
        {
            QVariant out;
            QMetaObject::invokeMethod(page.get(), "engineStateText",
                Q_RETURN_ARG(QVariant, out),
                Q_ARG(QVariant, connected),
                Q_ARG(QVariant, info),
                Q_ARG(QVariant, report));
            return out.toString();
        };
        QVariantMap welcomeStopped{ { QStringLiteral("running"), false } };
        QVariantMap welcomeRunning{ { QStringLiteral("running"), true } };
        QVariantMap liveRunning{ { QStringLiteral("engine"),
            QVariantMap{ { QStringLiteral("running"), true } } } };
        QVariantMap liveStopped{ { QStringLiteral("engine"),
            QVariantMap{ { QStringLiteral("running"), false } } } };
        /* the screenshot case */
        CHECK(state(true, welcomeStopped, liveRunning) ==
              QStringLiteral("running"));
        CHECK(state(true, welcomeRunning, liveStopped) ==
              QStringLiteral("not running"));
        /* no STATE yet -> WELCOME is the fallback */
        CHECK(state(true, welcomeStopped, QVariantMap()) ==
              QStringLiteral("not running"));
        CHECK(state(true, welcomeRunning, QVariantMap()) ==
              QStringLiteral("running"));
        CHECK(state(false, welcomeRunning, liveRunning) ==
              QStringLiteral("offline"));
    }

    /*---- R-10: the Ownership card is a jump target -----------*/
    {
        QObject* card = page->findChild<QObject*>("ownershipCard");
        CHECK(card != nullptr);
        /* the banner's "How to fix" lands here */
        CHECK(QMetaObject::invokeMethod(page.get(), "showOwnership"));
    }

    /*---- R-9: the restarts & crash-dumps card ----------------
       Honest absence first (no report, no UI supervisor), then
       the engine's backend-supervisor status and the UI's own
       engine-restart history render — exhausted budgets say
       so, and the crash-dump location is named. */
    {
        auto text = [](QObject* root, const char* n) {
            QObject* o = FindByName(root, n);
            return o ? o->property("text").toString()
                     : QStringLiteral("<missing>");
        };
        CHECK(FindByName(page.get(), "supervisorCard") != nullptr);
        CHECK(text(page.get(), "supervisorBackendLine")
                  .contains(QStringLiteral("no supervisor report")));
        CHECK(text(page.get(), "supervisorEngineLine")
                  .contains(QStringLiteral("not supervised")));
        QObject* ex = FindByName(page.get(), "supervisorExhausted");
        CHECK(ex && !ex->property("visible").toBool());
        CHECK(text(page.get(), "crashDumpNote")
                  .contains(QStringLiteral("state\\crash")));

        QVariantList hist;
        hist << QVariantMap{
            { QStringLiteral("at"), QStringLiteral("2026-11-28T09:59:00Z") },
            { QStringLiteral("action"), QStringLiteral("backend_spawned") },
            { QStringLiteral("reason"), QStringLiteral("backend_dead") },
            { QStringLiteral("detail"), QStringLiteral("link down") } };
        hist << QVariantMap{
            { QStringLiteral("at"), QStringLiteral("2026-11-28T10:00:00Z") },
            { QStringLiteral("action"), QStringLiteral("backend_closed_for_wedge") },
            { QStringLiteral("reason"), QStringLiteral("wireless_wedge") },
            { QStringLiteral("detail"), QStringLiteral("group wedged") } };
        bridge.setSupervisorForTesting(QVariantMap{
            { QStringLiteral("enabled"),          true },
            { QStringLiteral("backend_restarts"), 2 },
            { QStringLiteral("wedge_restarts"),   1 },
            { QStringLiteral("last_reason"),      QStringLiteral("wireless_wedge") },
            { QStringLiteral("last_at"),          QStringLiteral("2026-11-28T10:00:00Z") },
            { QStringLiteral("budget_exhausted"), true },
            { QStringLiteral("seq"),              2 },
            { QStringLiteral("history"),          hist } });
        QCoreApplication::processEvents();
        const QString bl = text(page.get(), "supervisorBackendLine");
        CHECK(bl.contains(QStringLiteral("restarted 2")));
        CHECK(bl.contains(QStringLiteral("wireless_wedge")));
        CHECK(ex && ex->property("visible").toBool());
        QSet<QObject*> rows;
        CollectVisibleByName(page.get(), "supervisorHistoryRow", rows);
        CHECK(rows.size() == 2);

        /* The UI's own engine supervisor (managed mode) in a
           child context: its restarts join the same card. */
        QTemporaryDir tmp;
        QDir().mkpath(tmp.path() + "/state");
        {
            QFile f(tmp.path() + "/state/openrgb2.run.json");
            f.open(QIODevice::WriteOnly);
            f.write("{\"engine_pid\": 4321}");
        }
        qint64 fake_now = 5000000;
        ui::EngineSupervisor::Deps sd;
        sd.state_dir = tmp.path() + "/state";
        sd.watch     = [](quint32) { return true; };
        sd.restart   = [] {};
        sd.now_ms    = [&fake_now] { return fake_now; };
        sd.schedule  = [](int, std::function<void()>) {};
        ui::EngineSupervisor sup(std::move(sd));
        QQmlContext child(engine.rootContext());
        child.setContextProperty(QStringLiteral("engineSupervisor"), &sup);
        std::unique_ptr<QObject> page2(comp.create(&child));
        CHECK(page2 != nullptr);
        if(page2)
        {
            CHECK(text(page2.get(), "supervisorEngineLine")
                      .contains(QStringLiteral("no restarts")));
            sup.onStartSucceeded();
            sup.onEngineExited(4321, 0xC0000005u);
            QCoreApplication::processEvents();
            const QString el = text(page2.get(), "supervisorEngineLine");
            CHECK(el.contains(QStringLiteral("restarted 1")));
            CHECK(el.contains(QStringLiteral("0xC0000005")));
            QSet<QObject*> rows2;
            CollectVisibleByName(page2.get(), "supervisorHistoryRow", rows2);
            CHECK(rows2.size() == 3);
        }
    }

    /*---- R-10: the shell's conflict banner --------------------*/
    {
        QQmlComponent bcomp(&engine,
            QUrl(QStringLiteral(
                "qrc:/qt/qml/OpenRGB2/UI/components/OwnershipBanner.qml")));
        if(bcomp.isError())
        {
            std::printf("FAIL load OwnershipBanner.qml:\n%s\n",
                        bcomp.errorString().toUtf8().constData());
        }
        std::unique_ptr<QObject> banner(
            bcomp.isError() ? nullptr
                            : bcomp.create(engine.rootContext()));
        CHECK(banner != nullptr);
        if(banner)
        {
            auto conflict = [](const char* owner, const char* key)
            {
                QVariantMap c;
                c.insert(QStringLiteral("owner"),
                         QString::fromUtf8(owner));
                c.insert(QStringLiteral("key"),
                         QString::fromUtf8(key));
                return QVariant(c);
            };
            auto report = [](const QVariantList& conflicts)
            {
                QVariantMap m;
                m.insert(QStringLiteral("scanned"), true);
                m.insert(QStringLiteral("conflicts"), conflicts);
                m.insert(QStringLiteral("notices"), QVariantList{});
                return m;
            };
            banner->setProperty("holdMs", 0);

            /* no report / a clean report -> hidden */
            CHECK(!banner->property("shown").toBool());
            banner->setProperty("ownership", report({}));
            CHECK(!banner->property("shown").toBool());

            /* a conflict -> shown, naming the writers */
            banner->setProperty("ownership", report({
                conflict("Windows Dynamic Lighting",
                         "dynamic_lighting"),
                conflict("LampArray service (Dynamic Lighting)",
                         "lamparray") }));
            CHECK(banner->property("shown").toBool());
            QObject* msg = banner->findChild<QObject*>(
                "ownershipBannerText");
            CHECK(msg != nullptr &&
                  msg->property("text").toString().contains(
                      QStringLiteral("Windows Dynamic Lighting")));
            CHECK(banner->findChild<QObject*>(
                      "ownershipFixButton") != nullptr);

            /* the surface can be switched off (engine down) */
            banner->setProperty("active", false);
            CHECK(!banner->property("shown").toBool());
            banner->setProperty("active", true);
            CHECK(banner->property("shown").toBool());
            QCoreApplication::processEvents();   /* reconnect settled */

            /* cleared -> hidden (no hold) */
            banner->setProperty("ownership", report({}));
            CHECK(!banner->property("shown").toBool());

            /* flap guard: with a hold the banner stays up for a
               moment after the clear, then goes */
            banner->setProperty("holdMs", 60);
            banner->setProperty("ownership", report({
                conflict("Logitech G Hub / Options", "logitech") }));
            CHECK(banner->property("shown").toBool());
            banner->setProperty("ownership", report({}));
            CHECK(banner->property("shown").toBool());
            QElapsedTimer t;
            t.start();
            while(banner->property("shown").toBool() &&
                  t.elapsed() < 2000)
            {
                QCoreApplication::processEvents(
                    QEventLoop::AllEvents, 20);
            }
            CHECK(!banner->property("shown").toBool());

            /* R-10 review: across a disconnect the hold does not
               apply — a reconnect whose first report is clean
               hides the banner at once, even though the engine
               went "connected" one moment before the report. */
            banner->setProperty("holdMs", 5000);
            banner->setProperty("ownership", report({
                conflict("Logitech G Hub / Options", "logitech") }));
            CHECK(banner->property("shown").toBool());
            banner->setProperty("active", false);
            banner->setProperty("active", true);     /* stale report */
            banner->setProperty("ownership", report({})); /* WELCOME */
            CHECK(!banner->property("shown").toBool());
            /* once settled, a clear is held again */
            banner->setProperty("ownership", report({
                conflict("Logitech G Hub / Options", "logitech") }));
            QCoreApplication::processEvents();
            banner->setProperty("ownership", report({}));
            CHECK(banner->property("shown").toBool());

            /* a conflict that returns inside the hold keeps it
               up without a gap */
            banner->setProperty("holdMs", 5000);
            banner->setProperty("ownership", report({
                conflict("Logitech G Hub / Options", "logitech") }));
            banner->setProperty("ownership", report({}));
            banner->setProperty("ownership", report({
                conflict("Logitech G Hub / Options", "logitech") }));
            CHECK(banner->property("shown").toBool());
        }
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
