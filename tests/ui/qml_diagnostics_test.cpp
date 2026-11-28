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
#include <QQuickStyle>

#include <cstdio>
#include <cstring>
#include <memory>

using namespace openrgb2;

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
