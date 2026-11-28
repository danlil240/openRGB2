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

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QSet>
#include <QQuickStyle>

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

    /*---- R-10: the Ownership card is a jump target -----------*/
    {
        QObject* card = page->findChild<QObject*>("ownershipCard");
        CHECK(card != nullptr);
        /* the banner's "How to fix" lands here */
        CHECK(QMetaObject::invokeMethod(page.get(), "showOwnership"));
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
