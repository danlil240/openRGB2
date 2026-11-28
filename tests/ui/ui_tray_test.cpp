// SPDX-License-Identifier: GPL-2.0-or-later
//
// R-1 tray quick controls (src/ui/TrayController): the tray menu
// is DATA — main.cpp only renders it into a QMenu — so the whole
// surface is testable offscreen:
//
//   - model entries: a Workspaces submenu, Brightness presets
//     100/75/50/25 % plus Off, and Pause/Resume, with the
//     current state checked
//   - a disabled state while the engine is disconnected; the
//     output controls hidden when a connected engine lacks
//     output_control_v1
//   - every action issues the right OUTPUT_COMMAND on the wire
//     (a real EngineBridge over the in-process pipe)
//   - a workspace pick goes through openWorkspace; a dirty draft
//     (replaceOffered latched) asks for the window
//   - the tooltip names workspace, brightness and paused/off

#include "EngineBridge.h"
#include "TrayController.h"

#include "test_pipe.h"

#include <QCoreApplication>
#include <QThread>
#include <QVariantList>
#include <QVariantMap>

#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>

using namespace openrgb2;
using namespace openrgb2::ipc;
using namespace ui_test;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

static bool PumpUntil(std::function<bool()> cond, int ms = 6000)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(ms);
    while(std::chrono::steady_clock::now() < deadline)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        if(cond())
        {
            return true;
        }
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return cond();
}

/* The WorkspaceController surface the tray uses, duck-typed the
   way QML binds it: properties + openWorkspace(). `dirty` makes
   openWorkspace latch the replace confirmation like the real
   controller does for a dirty draft. */
class FakeWorkspace : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY availabilityChanged)
    Q_PROPERTY(QVariantList workspaceList READ workspaceList
                   NOTIFY workspaceListChanged)
    Q_PROPERTY(QString workspaceId READ workspaceId
                   NOTIFY workspaceChanged)
    Q_PROPERTY(QString workspaceName READ workspaceName
                   NOTIFY workspaceChanged)
    Q_PROPERTY(bool replaceOffered READ replaceOffered
                   NOTIFY replaceChanged)

public:
    bool         available_ = true;
    QVariantList list_ = { QStringLiteral("desk"),
                           QStringLiteral("shelf") };
    QString      id_   = QStringLiteral("desk");
    QString      name_ = QStringLiteral("Desk");
    bool         replace_ = false;
    bool         dirty = false;
    QStringList  opened;

    bool         available() const      { return available_; }
    QVariantList workspaceList() const  { return list_; }
    QString      workspaceId() const    { return id_; }
    QString      workspaceName() const  { return name_; }
    bool         replaceOffered() const { return replace_; }

    Q_INVOKABLE void openWorkspace(const QString& id)
    {
        opened << id;
        if(dirty)
        {
            replace_ = true;
            emit replaceChanged();
            return;
        }
        id_   = id;
        name_ = id;
        emit workspaceChanged();
    }

    void SetList(const QVariantList& l)
    {
        list_ = l;
        emit workspaceListChanged();
    }

signals:
    void availabilityChanged();
    void workspaceListChanged();
    void workspaceChanged();
    void replaceChanged();
};

static QVariantMap Find(const QVariantList& entries, const QString& id)
{
    for(const QVariant& v : entries)
    {
        const QVariantMap e = v.toMap();
        if(e.value(QStringLiteral("id")).toString() == id)
        {
            return e;
        }
        const QVariantMap hit =
            Find(e.value(QStringLiteral("children")).toList(), id);
        if(!hit.isEmpty())
        {
            return hit;
        }
    }
    return {};
}

static nlohmann::json OutputBody(double master, bool blackout,
                                 bool hold, bool uploaded = false)
{
    return nlohmann::json::object({
        { "master", master }, { "blackout", blackout },
        { "hold", hold },
        { "effective", blackout ? 0.0 : master },
        { "contributors", nlohmann::json::object() },
        { "uploaded_unscaled", uploaded } });
}

/* WELCOME (optionally advertising output_control_v1) + SUBSCRIBE
   OK + the three auto-pulls. */
static bool Connect(ui::EngineBridge& bridge, ServerPeer& srv,
                    bool with_cap)
{
    Message m;
    if(!srv.RecvMsg(m, 4000) || m.type != MsgType::Hello)
    {
        return false;
    }
    srv.SendMsg(MsgType::Welcome, 0, nlohmann::json::object({
        { "server_version", 1 }, { "negotiated_version", 1 },
        { "channel", "command" }, { "engine", "openrgb2-engine" },
        { "running", true },
        { "capabilities", with_cap
              ? nlohmann::json::array({ "output_control_v1" })
              : nlohmann::json::array() },
        { "state", nlohmann::json::object({
            { "output", with_cap ? OutputBody(0.75, false, false)
                                 : nlohmann::json() } }) },
    }));
    if(!srv.RecvMsg(m, 4000) || m.type != MsgType::Subscribe)
    {
        return false;
    }
    srv.SendMsg(MsgType::Ok, m.request_id, nlohmann::json::object({
        { "subscribed", nlohmann::json::array() } }));
    if(!PumpUntil([&] { return bridge.connected(); }))
    {
        return false;
    }
    static const MsgType want[3] = {
        MsgType::GetPresets, MsgType::GetDeliveryStats,
        MsgType::GetState };
    static const MsgType reply[3] = {
        MsgType::Presets, MsgType::DeliveryStats, MsgType::State };
    for(int i = 0; i < 3; ++i)
    {
        if(!srv.RecvMsg(m, 4000) || m.type != want[i])
        {
            return false;
        }
        srv.SendMsg(reply[i], m.request_id, nlohmann::json::object());
    }
    return true;
}

/* The next OUTPUT_COMMAND body on the wire (answered with `reply`
   so the bridge store follows). */
static nlohmann::json NextOutput(ServerPeer& srv,
                                 const nlohmann::json& reply)
{
    Message m;
    if(!srv.RecvMsg(m, 4000) || m.type != MsgType::OutputCommand)
    {
        return nlohmann::json();
    }
    srv.SendMsg(MsgType::Output, m.request_id, reply);
    return m.Body();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    /*---- disconnected: everything present, nothing enabled ----*/
    std::printf("-- disconnected\n");
    {
        ui::EngineBridge bridge;
        FakeWorkspace    ws;
        ui::TrayController tray(&bridge, &ws);
        const QVariantList menu = tray.menu();
        const QVariantMap wsm = Find(menu, QStringLiteral("workspaces"));
        CHECK(!wsm.isEmpty());
        CHECK(wsm.value("kind").toString() == "submenu");
        CHECK(!wsm.value("enabled").toBool());
        CHECK(wsm.value("children").toList().size() == 2);
        const QVariantMap b50 = Find(menu, QStringLiteral("brightness:50"));
        CHECK(!b50.isEmpty() && !b50.value("enabled").toBool());
        const QVariantMap pause = Find(menu, QStringLiteral("pause"));
        CHECK(!pause.isEmpty() && !pause.value("enabled").toBool());
        CHECK(tray.toolTip().contains("not connected"));
        /* Triggers while disconnected do nothing (no crash, no
           workspace switch). */
        tray.trigger(QStringLiteral("workspace:shelf"));
        tray.trigger(QStringLiteral("pause"));
        CHECK(ws.opened.isEmpty());
    }

    /*---- connected with output_control_v1 ----------------------*/
    std::printf("-- connected: model, verbs, workspace pick\n");
    {
        ui::EngineBridge bridge;
        auto pipe = std::make_shared<TestPipe>();
        bridge.setChannelFactoryForTesting(
            [&](std::string&) -> std::unique_ptr<IIpcChannel>
            { return pipe->ClientChannel(); });
        ui::EngineWireClient::Options wopts;
        wopts.keepalive_ms = 0;
        bridge.setWireOptionsForTesting(wopts);
        FakeWorkspace      ws;
        ui::TrayController tray(&bridge, &ws);
        int menu_changes = 0;
        int show_requests = 0;
        QObject::connect(&tray, &ui::TrayController::menuChanged,
                         [&] { ++menu_changes; });
        QObject::connect(&tray, &ui::TrayController::showWindowRequested,
                         [&] { ++show_requests; });
        bridge.start();
        ServerPeer srv = pipe->Peer();
        CHECK(Connect(bridge, srv, true));
        CHECK(PumpUntil([&] {
            return bridge.output().value("master").toDouble() == 0.75;
        }));
        CHECK(menu_changes >= 1);

        QVariantList menu = tray.menu();
        /* Order: Workspaces, Brightness, Pause. */
        CHECK(menu.size() == 3);
        if(menu.size() == 3)
        {
            CHECK(menu[0].toMap().value("id") == "workspaces");
            CHECK(menu[1].toMap().value("id") == "brightness");
            CHECK(menu[2].toMap().value("id") == "pause");
        }
        const QVariantMap wsm = Find(menu, QStringLiteral("workspaces"));
        CHECK(wsm.value("enabled").toBool());
        CHECK(Find(menu, "workspace:desk").value("checked").toBool());
        CHECK(!Find(menu, "workspace:shelf").value("checked").toBool());
        CHECK(Find(menu, "workspace:shelf").value("enabled").toBool());
        /* Presets 100/75/50/25 + Off; 75 is the current one. */
        const QVariantList bright =
            Find(menu, "brightness").value("children").toList();
        QStringList ids;
        for(const QVariant& v : bright)
        {
            ids << v.toMap().value("id").toString();
        }
        CHECK(ids.contains("brightness:100") &&
              ids.contains("brightness:75") &&
              ids.contains("brightness:50") &&
              ids.contains("brightness:25") &&
              ids.contains("brightness:off"));
        CHECK(Find(menu, "brightness:75").value("checked").toBool());
        CHECK(!Find(menu, "brightness:50").value("checked").toBool());
        CHECK(!Find(menu, "brightness:off").value("checked").toBool());
        CHECK(Find(menu, "brightness:50").value("enabled").toBool());
        CHECK(Find(menu, "pause").value("text").toString()
                  .contains("Pause"));
        CHECK(tray.toolTip().contains("Desk"));
        CHECK(tray.toolTip().contains("75%"));

        /* Brightness preset -> set_master. */
        tray.trigger(QStringLiteral("brightness:50"));
        nlohmann::json b = NextOutput(srv, OutputBody(0.5, false, false));
        CHECK(b.value("verb", std::string()) == "set_master");
        CHECK(b.value("value", -1.0) == 0.5);
        CHECK(PumpUntil([&] {
            return Find(tray.menu(), "brightness:50")
                       .value("checked").toBool();
        }));

        /* Off -> set_blackout on. */
        tray.trigger(QStringLiteral("brightness:off"));
        b = NextOutput(srv, OutputBody(0.5, true, false));
        CHECK(b.value("verb", std::string()) == "set_blackout");
        CHECK(b.value("on", false) == true);
        CHECK(PumpUntil([&] {
            return Find(tray.menu(), "brightness:off")
                       .value("checked").toBool();
        }));
        CHECK(!Find(tray.menu(), "brightness:50")
                   .value("checked").toBool());
        CHECK(tray.toolTip().contains("Off"));

        /* A preset while Off turns the lights back on first. */
        tray.trigger(QStringLiteral("brightness:25"));
        b = NextOutput(srv, OutputBody(0.5, false, false));
        CHECK(b.value("verb", std::string()) == "set_blackout");
        CHECK(b.value("on", true) == false);
        b = NextOutput(srv, OutputBody(0.25, false, false));
        CHECK(b.value("verb", std::string()) == "set_master");
        CHECK(b.value("value", -1.0) == 0.25);

        /* Off is a toggle: picking it again while Off restores. */
        bridge.setOutputForTesting(QVariantMap({
            { "master", 0.25 }, { "blackout", true },
            { "hold", false }, { "effective", 0.0 } }));
        tray.trigger(QStringLiteral("brightness:off"));
        b = NextOutput(srv, OutputBody(0.25, false, false));
        CHECK(b.value("verb", std::string()) == "set_blackout");
        CHECK(b.value("on", true) == false);

        /* Pause / Resume -> set_hold. */
        tray.trigger(QStringLiteral("pause"));
        b = NextOutput(srv, OutputBody(0.25, false, true));
        CHECK(b.value("verb", std::string()) == "set_hold");
        CHECK(b.value("on", false) == true);
        CHECK(PumpUntil([&] {
            return Find(tray.menu(), "pause").value("text").toString()
                       .contains("Resume");
        }));
        CHECK(tray.toolTip().contains("Paused"));
        tray.trigger(QStringLiteral("pause"));
        b = NextOutput(srv, OutputBody(0.25, false, false));
        CHECK(b.value("verb", std::string()) == "set_hold");
        CHECK(b.value("on", true) == false);

        /* An uploaded loop is not dimmed — the submenu says so. */
        bridge.setOutputForTesting(QVariantMap({
            { "master", 0.25 }, { "blackout", false },
            { "hold", false }, { "effective", 0.25 },
            { "uploaded_unscaled", true } }));
        const QVariantMap note = Find(tray.menu(), "brightness:note");
        CHECK(!note.isEmpty() && !note.value("enabled").toBool());

        /* R-4: an active dimming rule (an OUTPUT contributor)
           names itself in the tooltip — the strongest one. */
        bridge.setOutputForTesting(QVariantMap({
            { "master", 1.0 }, { "blackout", false },
            { "hold", false }, { "effective", 0.3 },
            { "contributors", QVariantMap({ { "schedule", 0.3 },
                                            { "idle", 0.5 } }) } }));
        CHECK(tray.toolTip().contains("Dimmed by schedule (30%)"));
        bridge.setOutputForTesting(QVariantMap({
            { "master", 1.0 }, { "blackout", false },
            { "hold", false }, { "effective", 0.0 },
            { "contributors", QVariantMap({ { "lock", 0.0 } }) } }));
        CHECK(tray.toolTip().contains("Off while locked"));
        bridge.setOutputForTesting(QVariantMap({
            { "master", 1.0 }, { "blackout", false },
            { "hold", false }, { "effective", 1.0 },
            { "contributors", QVariantMap() } }));
        CHECK(!tray.toolTip().contains("Dimmed"));

        /* Workspace pick, clean draft: opens, no window. */
        tray.trigger(QStringLiteral("workspace:shelf"));
        CHECK(ws.opened == QStringList({ "shelf" }));
        CHECK(show_requests == 0);
        CHECK(Find(tray.menu(), "workspace:shelf")
                  .value("checked").toBool());
        /* Picking the current workspace is a no-op. */
        tray.trigger(QStringLiteral("workspace:shelf"));
        CHECK(ws.opened.size() == 1);

        /* Dirty draft: openWorkspace latches the confirmation —
           the tray asks for the window so the dialog is seen. */
        ws.dirty = true;
        tray.trigger(QStringLiteral("workspace:desk"));
        CHECK(ws.opened.size() == 2);
        CHECK(show_requests == 1);

        /* A list change rebuilds the model. */
        const int before = menu_changes;
        ws.SetList({ QStringLiteral("desk"), QStringLiteral("shelf"),
                     QStringLiteral("night") });
        CHECK(menu_changes > before);
        CHECK(!Find(tray.menu(), "workspace:night").isEmpty());

        /* Engine lost: everything disabled again. */
        srv.Close();
        CHECK(PumpUntil([&] { return !bridge.connected(); }));
        CHECK(!Find(tray.menu(), "workspaces")
                   .value("enabled").toBool());
        CHECK(!Find(tray.menu(), "pause").value("enabled").toBool());
        bridge.stop();
    }

    /*---- connected without the capability: controls hidden ----*/
    std::printf("-- connected without output_control_v1\n");
    {
        ui::EngineBridge bridge;
        auto pipe = std::make_shared<TestPipe>();
        bridge.setChannelFactoryForTesting(
            [&](std::string&) -> std::unique_ptr<IIpcChannel>
            { return pipe->ClientChannel(); });
        ui::EngineWireClient::Options wopts;
        wopts.keepalive_ms = 0;
        bridge.setWireOptionsForTesting(wopts);
        FakeWorkspace      ws;
        ui::TrayController tray(&bridge, &ws);
        bridge.start();
        ServerPeer srv = pipe->Peer();
        CHECK(Connect(bridge, srv, false));
        const QVariantList menu = tray.menu();
        CHECK(menu.size() == 1);
        CHECK(!Find(menu, "workspaces").isEmpty());
        CHECK(Find(menu, "brightness").isEmpty());
        CHECK(Find(menu, "pause").isEmpty());
        tray.trigger(QStringLiteral("brightness:50"));  /* ignored */
        bridge.stop();
    }

    /*---- no workspace controller (legacy engine) ---------------*/
    {
        ui::EngineBridge   bridge;
        ui::TrayController tray(&bridge, nullptr);
        const QVariantMap wsm = Find(tray.menu(), "workspaces");
        CHECK(!wsm.isEmpty() && !wsm.value("enabled").toBool());
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

#include "ui_tray_test.moc"
