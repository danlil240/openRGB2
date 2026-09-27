// SPDX-License-Identifier: GPL-2.0-or-later
//
// Devices & Diagnostics test (Task 4.7) — a real EngineBridge
// + DiagnosticsController over the in-process wire pipe
// (test_pipe.h) against a scripted engine, proving:
//
//   - the four rate views land with honest labels: configured
//     playback rate from PlaybackStatus effective geometry,
//     physical rate UNMEASURED (provenance "unknown" — never
//     a fabricated number), transport-admitted delivery rate
//     from DELIVERY_STATS interval stats, preview cadence
//     measured UI-side from frame_status arrivals
//   - ownership conflicts surface the ENGINE's report
//     verbatim (conflict list, notices, config findings) —
//     scanned:false/unreported never reads as clean
//   - per-controller rows join topology + delivery stats +
//     resolved scene bindings; missing-device bindings land
//     on unresolvedBindings
//   - export writes a redacted bundle: serials, hardware
//     locations, and MACs never leave the machine
//
// CHECK-macro main like the other UI suites — no framework.

#include "DiagnosticsController.h"
#include "EngineBridge.h"

#include "test_pipe.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QVariantMap>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>

using namespace openrgb2;
using namespace openrgb2::ipc;
using namespace ui_test;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

/* Scripted engine state. */
struct Server
{
    explicit Server(ServerPeer peer) : srv(std::move(peer)) {}
    ServerPeer srv;
};

static nlohmann::json KeyJson(const char* dev, const char* ser,
                            const char* loc)
{
    return nlohmann::json::object({
        { "device", dev }, { "serial", ser },
        { "location", loc }, { "zone", "" } });
}

static nlohmann::json SnapshotsBody()
{
    return nlohmann::json::object({
        { "snapshots", nlohmann::json::array({
            nlohmann::json::object({
                { "key", KeyJson("Lian Li: SL Wireless",
                                 "AABBCCDDEEFF",
                                 "Wireless: AA:BB:CC:DD:EE:FF") },
                { "name",   "Lian Li SL Wireless" },
                { "vendor", "Lian Li" },
                { "device_type", 2 },
                { "zones", nlohmann::json::array({
                    nlohmann::json::object(
                        {{"name","Fan 1"},{"led_count",40}}),
                    nlohmann::json::object(
                        {{"name","Fan 2"},{"led_count",40}}),
                    nlohmann::json::object(
                        {{"name","Fan 3"},{"led_count",40}}) })},
                { "modes", nlohmann::json::array(
                    {"Direct","Spectrum Cycle"}) },
                { "active_mode", 0 },
                { "connection", "connected" },
                { "led_count", 120 },
            }),
            nlohmann::json::object({
                { "key", KeyJson("X870E AORUS ELITE", "", "") },
                { "name",   "X870E AORUS ELITE WiFi7 ICE" },
                { "vendor", "Gigabyte" },
                { "device_type", 1 },
                { "zones", nlohmann::json::array({
                    nlohmann::json::object(
                        {{"name","ARGB_V2_1"},{"led_count",8}}) })},
                { "modes", nlohmann::json::array({"Direct"}) },
                { "active_mode", 0 },
                { "connection", "connected" },
                { "led_count", 8 },
            }) })},
        { "connection", nlohmann::json::object({
            { "sdk_present", true }, { "state", "connected" },
            { "protocol_version", 4 }, { "reconnect_attempt", 0 },
            { "last_error", "" } })},
    });
}

static nlohmann::json SceneBody()
{
    return nlohmann::json::object({
        { "loaded", true }, { "revision", 3 },
        { "applied_revision", 3 }, { "playing", true },
        { "name", "desk" },
        { "document", nlohmann::json::object({
            { "name", "desk" },
            { "objects", nlohmann::json::array({
                nlohmann::json::object(
                    {{"id","fan0"},{"label","Fan 0"},
                     {"kind","device"},{"binding","b_fans"}}),
                nlohmann::json::object(
                    {{"id","fan1"},{"label","Fan 1"},
                     {"kind","device"},{"binding","b_fans"}}) })},
            { "bindings", nlohmann::json::array({
                nlohmann::json::object(
                    {{"id","b_fans"},
                     {"controller_name","Lian Li SL Wireless"},
                     {"zone_name","Fan 1"}}),
                nlohmann::json::object(
                    {{"id","b_gone"},
                     {"controller_name","Missing Controller"},
                     {"zone_name",""}}) })},
        })},
    });
}

static nlohmann::json PlaybackBody()
{
    return nlohmann::json::object({
        { "requested_mode", "animation_playback" },
        { "requested_revision", 1 },
        { "pending", false },
        { "unavailable_reason", nlohmann::json(nullptr) },
        { "detail", nlohmann::json(nullptr) },
        { "capabilities", nlohmann::json::object(
            {{"live_effects",true},
             {"animation_playback",true}}) },
        { "status", nlohmann::json::object({
            { "mode", "animation_playback" },
            { "preset_revision", 1 }, { "parameter_revision", 1 },
            { "upload", "active" }, { "control", "playing" },
            { "effective_frame_count", 300 },
            { "effective_frame_interval_s", 1.0 / 60.0 },
            { "clock_sample_s", 0 }, { "clock_uncertainty_s", 0 },
            { "sync", "unknown" },
            { "physical", nlohmann::json::object({
                { "frame_interval_s", 0 }, { "cycle_s", 0 },
                { "provenance", "unknown" } })},
        })},
    });
}

static nlohmann::json StatsJson(uint64_t submitted,
                                double mean_iv, uint64_t icount)
{
    return nlohmann::json::object({
        { "submitted", submitted }, { "replaced_pending", 12 },
        { "rejected", 0 }, { "transport_completed", submitted },
        { "suppressed", 0 }, { "transport_rejected", 2 },
        { "last_interval_s", mean_iv },
        { "min_interval_s", mean_iv }, { "max_interval_s", mean_iv },
        { "mean_interval_s", mean_iv },
        { "interval_count", icount },
        { "provenance", "transport_measured" },
    });
}

static nlohmann::json DeliveryBody()
{
    return nlohmann::json::object({
        { "available", true },
        { "aggregate", StatsJson(2000, 0.0333, 400) },
        { "pending", 1 },
        { "controllers", nlohmann::json::array({
            nlohmann::json::object({
                { "key", KeyJson("Lian Li: SL Wireless",
                                 "AABBCCDDEEFF",
                                 "Wireless: AA:BB:CC:DD:EE:FF") },
                { "stats", StatsJson(2000, 0.0333, 400) },
                { "pending", 1 }, { "budget_ms", 30 },
            }),
            nlohmann::json::object({
                { "key", KeyJson("X870E AORUS ELITE", "", "") },
                { "stats", StatsJson(500, 0.0167, 100) },
                { "pending", 0 },
            }) })},
    });
}

static nlohmann::json OwnershipBody()
{
    return nlohmann::json::object({
        { "scanned", true },
        { "backend", nlohmann::json::object(
            {{"listening",true},{"attached",true}}) },
        { "conflicts", nlohmann::json::array({
            nlohmann::json::object({
                { "owner", "L-Connect" },
                { "exe",   "LConnectService.exe" },
                { "pid",   4242 },
                { "detail","competing RGB owner (Lian Li)" },
            }) })},
        { "notices", nlohmann::json::array({
            nlohmann::json::object({
                { "owner", "FanControl" },
                { "exe",   "FanControl.exe" },
                { "pid",   5151 },
                { "detail","cooling owner — expected" },
            }) })},
        { "config", nlohmann::json::object({
            { "checked", true },
            { "findings", nlohmann::json::array({
                "sole scene writer verified: no enabled or "
                "loadable plugins",
                /* a private path, as the real engine emits it
                   verbatim (OwnershipScan "backend config dir
                   '<dir>' not found") — the export must scrub it */
                "backend config dir 'C:\\Users\\testuser\\AppData"
                "\\Roaming\\OpenRGB' not found — cannot verify "
                "sole scene writer"}) },
            { "enabled_plugins",  nlohmann::json::array() },
            { "legacy_producers", nlohmann::json::array() },
        })},
    });
}

static nlohmann::json StateBody()
{
    return nlohmann::json::object({
        { "engine", nlohmann::json::object(
            {{"running",true},{"scene_clock_hz",60},
             {"scene_sequence",12345}}) },
        { "scene",    SceneBody() },
        { "sdk",      SnapshotsBody()["connection"] },
        { "playback", PlaybackBody() },
        { "ownership",OwnershipBody() },
        { "ipc", nlohmann::json::object(
            {{"sessions_active",1},{"messages_in",42},
             {"events_dropped",0}}) },
    });
}

static nlohmann::json DiagnosticsBody()
{
    return nlohmann::json::object({
        { "engine", nlohmann::json::object(
            {{"running",true},{"scene_clock_hz",60},
             {"scene_sequence",12345},{"uptime_s",61.5}}) },
        { "scene",     SceneBody() },
        { "playback",  PlaybackBody() },
        { "delivery",  DeliveryBody() },
        { "sdk",       SnapshotsBody()["connection"] },
        { "ownership", OwnershipBody() },
        { "ipc", nlohmann::json::object(
            {{"sessions_active",1},{"events_dropped",0}}) },
        { "clock", nlohmann::json::object({{"steady_ms",1.0}}) },
        { "backend_counters", nlohmann::json(nullptr) },
    });
}

/* One request/response round on the scripted engine. */
static bool ServeOnce(Server& s, uint32_t wait_ms = 15)
{
    Message m;
    if(!s.srv.RecvMsg(m, wait_ms))
    {
        return false;
    }
    switch(m.type)
    {
    case MsgType::Hello:
        s.srv.SendMsg(MsgType::Welcome, 0,
            nlohmann::json::object({
                { "server_version",     1 },
                { "negotiated_version", 1 },
                { "channel",            "command" },
                { "engine",             "openrgb2-engine" },
                { "running",            true },
                { "state",              StateBody() },
            }));
        break;
    case MsgType::Subscribe:
        s.srv.SendMsg(MsgType::Ok, m.request_id,
            nlohmann::json::object(
                { { "subscribed", nlohmann::json::array({
                    "topology", "scene", "playback",
                    "connection", "frame_status" }) } }));
        break;
    case MsgType::GetPresets:
        s.srv.SendMsg(MsgType::Presets, m.request_id,
            nlohmann::json::object(
                { { "presets", nlohmann::json::array() } }));
        break;
    case MsgType::GetSnapshots:
        s.srv.SendMsg(MsgType::Snapshots, m.request_id,
                      SnapshotsBody());
        break;
    case MsgType::GetScene:
        s.srv.SendMsg(MsgType::Scene, m.request_id, SceneBody());
        break;
    case MsgType::GetPlayback:
        s.srv.SendMsg(MsgType::Playback, m.request_id,
                      PlaybackBody());
        break;
    case MsgType::GetDeliveryStats:
        s.srv.SendMsg(MsgType::DeliveryStats, m.request_id,
                      DeliveryBody());
        break;
    case MsgType::GetState:
        s.srv.SendMsg(MsgType::State, m.request_id, StateBody());
        break;
    case MsgType::DiagGet:
        s.srv.SendMsg(MsgType::Diagnostics, m.request_id,
                      DiagnosticsBody());
        break;
    default:
        s.srv.SendMsg(MsgType::Error, m.request_id,
            nlohmann::json::object({
                { "code",    (uint32_t)ErrCode::BadRequest },
                { "error",   "bad_request" },
                { "message", "unexpected request" },
            }));
        break;
    }
    return true;
}

template <typename Pred>
static bool PumpBoth(Server& s, Pred pred, int ms = 12000)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(ms);
    while(std::chrono::steady_clock::now() < deadline)
    {
        if(pred())
        {
            return true;
        }
        ServeOnce(s, 10);
        QCoreApplication::processEvents(QEventLoop::AllEvents,
                                        10);
    }
    return pred();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    auto            pipe = std::make_shared<TestPipe>();
    Server          s(pipe->Peer());
    ui::EngineBridge bridge;
    ui::DiagnosticsController diag(&bridge);

    bridge.setChannelFactoryForTesting(
        [&](std::string& err)
            -> std::unique_ptr<IIpcChannel>
        {
            (void)err;
            return pipe->ClientChannel();
        });
    ui::EngineWireClient::Options wopts;
    wopts.keepalive_ms = 0;
    bridge.setWireOptionsForTesting(wopts);

    bridge.start();

    /*---- handshake + refresh: stores land ------------------*/
    std::printf("-- handshake + refresh\n");
    CHECK(PumpBoth(s, [&] {
        return bridge.connectionState() ==
               QStringLiteral("connected");
    }));
    diag.refresh();
    CHECK(PumpBoth(s, [&] {
        return !bridge.diagnostics().isEmpty()
               && !bridge.deliveryStats().isEmpty()
               && bridge.snapshots().size() == 2;
    }));

    /*---- the four rates, honestly labeled -------------------*/
    std::printf("-- four rates\n");
    {
        const QVariantMap r = diag.rates();

        /* configured playback rate — loop-builder geometry */
        CHECK(r.value(QStringLiteral("configured_available"))
                  .toBool());
        CHECK(r.value(QStringLiteral("configured_hz"))
                  .toDouble() > 59.0 &&
              r.value(QStringLiteral("configured_hz"))
                  .toDouble() < 61.0);
        CHECK(r.value(QStringLiteral("configured_frame_count"))
                  .toInt() == 300);

        /* physical — provenance "unknown" => UNMEASURED,
           never a zero or a fabricated number */
        CHECK(!r.value(QStringLiteral("physical_measured"))
                   .toBool());
        CHECK(r.value(QStringLiteral("physical_hz"))
                  .toDouble() == 0.0);
        CHECK(r.value(QStringLiteral("physical_provenance"))
                  .toString() == QStringLiteral("unknown"));

        /* delivery — transport-admitted, labeled by its own
           provenance string */
        CHECK(r.value(QStringLiteral("delivery_measured"))
                  .toBool());
        const double dhz =
            r.value(QStringLiteral("delivery_hz")).toDouble();
        CHECK(dhz > 29.0 && dhz < 31.0);
        CHECK(r.value(QStringLiteral("delivery_provenance"))
                  .toString() ==
              QStringLiteral("transport_measured"));

        /* preview — no frame_status pushes yet => unmeasured */
        CHECK(!r.value(QStringLiteral("preview_measured"))
                   .toBool());
    }

    /*---- preview cadence measured UI-side --------------------*/
    std::printf("-- preview cadence\n");
    for(int i = 0; i < 4; ++i)
    {
        s.srv.SendMsg(MsgType::Event, 0,
            nlohmann::json::object({
                { "kind", "frame_status" },
                { "data", nlohmann::json::object({
                    { "aggregate", StatsJson(10, 0.033, 4) },
                    { "controllers", nlohmann::json::array() }})},
            }));
        ServeOnce(s, 5);
        QCoreApplication::processEvents(QEventLoop::AllEvents,
                                        25);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(20));
    }
    CHECK(PumpBoth(s, [&] {
        return diag.rates()
            .value(QStringLiteral("preview_measured")).toBool();
    }));

    /*---- ownership: the engine's report verbatim -------------*/
    std::printf("-- ownership\n");
    {
        const QVariantMap o = diag.ownership();
        CHECK(o.value(QStringLiteral("reported")).toBool());
        CHECK(o.value(QStringLiteral("scanned")).toBool());
        CHECK(o.value(QStringLiteral("conflictCount"))
                  .toInt() == 1);
        const QVariantList conflicts =
            o.value(QStringLiteral("conflicts")).toList();
        CHECK(conflicts.size() == 1);
        CHECK(conflicts[0].toMap()
                  .value(QStringLiteral("owner")).toString()
              == QStringLiteral("L-Connect"));
        CHECK(conflicts[0].toMap()
                  .value(QStringLiteral("detail")).toString()
                  .contains(QStringLiteral("competing")));
        const QVariantList notices =
            o.value(QStringLiteral("notices")).toList();
        CHECK(notices.size() == 1);
        CHECK(notices[0].toMap()
                  .value(QStringLiteral("owner")).toString()
              == QStringLiteral("FanControl"));
        CHECK(o.value(QStringLiteral("config")).toMap()
                  .value(QStringLiteral("checked")).toBool());
        CHECK(!o.value(QStringLiteral("config")).toMap()
                   .value(QStringLiteral("findings")).toList()
                   .isEmpty());
    }

    /*---- per-controller rows + binding resolution -------------*/
    std::printf("-- controllers\n");
    {
        const QVariantList rows = diag.controllers();
        CHECK(rows.size() == 2);
        const QVariantMap w = rows[0].toMap();
        CHECK(w.value(QStringLiteral("name")).toString()
                  .contains(QStringLiteral("Lian Li")));
        CHECK(w.value(QStringLiteral("led_count")).toInt()
                  == 120);
        CHECK(w.value(QStringLiteral("zone_count")).toInt()
                  == 3);
        /* the scene's b_fans binding resolves to this
           controller; both fan objects count */
        CHECK(w.value(QStringLiteral("bound_bindings")).toInt()
                  == 1);
        CHECK(w.value(QStringLiteral("bound_objects")).toInt()
                  == 2);
        const QVariantMap st =
            w.value(QStringLiteral("stats")).toMap();
        CHECK(st.value(QStringLiteral("delivery_measured"))
                  .toBool());
        CHECK(st.value(QStringLiteral("suppressed")).toInt()
                  == 0);
        CHECK(st.value(QStringLiteral("replaced_pending"))
                  .toInt() == 12);
        CHECK(w.value(QStringLiteral("pending")).toInt() == 1);
        CHECK(w.value(QStringLiteral("budget_ms")).toInt()
                  == 30);

        /* the missing controller's binding is honestly
           unresolved — never silently re-bound */
        const QVariantList un = diag.unresolvedBindings();
        CHECK(un.size() == 1);
        CHECK(un[0].toMap().value(QStringLiteral("id"))
                  .toString() == QStringLiteral("b_gone"));
    }

    /*---- event journal ---------------------------------------*/
    std::printf("-- event log\n");
    {
        CHECK(!diag.eventLog().isEmpty());
        bool saw_frame_status = false;
        for(const QVariant& e : diag.eventLog())
        {
            if(e.toMap().value(QStringLiteral("kind"))
                   .toString() ==
               QStringLiteral("frame_status"))
            {
                saw_frame_status = true;
            }
        }
        CHECK(saw_frame_status);
    }

    /*---- export: redacted bundle -------------------------------*/
    std::printf("-- export redaction\n");
    {
        QTemporaryDir tmp;
        CHECK(tmp.isValid());
        const QString path =
            tmp.filePath(QStringLiteral("diag.json"));
        CHECK(diag.exportDiagnostics(path));

        std::ifstream in(path.toStdString(),
                         std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        CHECK(!text.empty());
        /* Unique identifiers never leave the machine. */
        CHECK(text.find("AABBCCDDEEFF") == std::string::npos);
        CHECK(text.find("AA:BB:CC:DD:EE:FF") ==
              std::string::npos);
        CHECK(text.find("Wireless:") == std::string::npos);
        CHECK(text.find("[redacted]") != std::string::npos);

        const nlohmann::json bundle =
            nlohmann::json::parse(text, nullptr, false);
        CHECK(!bundle.is_discarded());
        CHECK(bundle.value("bundle", std::string()) ==
              "diagnostics");
        /* Content is real: the engine's ownership report and
           per-controller stats survive redaction. */
        CHECK(bundle["ownership"]["conflictCount"]
                  .get<int>() == 1);
        CHECK(bundle["delivery_stats"]["aggregate"]
                  ["transport_completed"].get<uint64_t>() ==
              2000);
        CHECK(bundle["snapshots"].size() == 2);
        CHECK(bundle["snapshots"][0]["key"]["serial"]
                  .get<std::string>() == "[redacted]");
        CHECK(bundle["snapshots"][0]["key"]["location"]
                  .get<std::string>() == "[redacted]");
        /* ...but device names stay useful. */
        CHECK(bundle["snapshots"][0]["name"]
                  .get<std::string>()
                  .find("Lian Li") != std::string::npos);
        CHECK(!diag.lastExport().isEmpty());

        /* Private paths never leave the machine either: the
           scripted ownership finding carried a C:\Users\
           testuser dir verbatim (the real engine emits
           backend_config_dir exactly like this) — it must
           appear only as C:\Users\<redacted>. */
        CHECK(text.find("testuser") == std::string::npos);
        CHECK(text.find("C:\\\\Users\\\\<redacted>") !=
              std::string::npos);
        /* ...and the rest of the path survived. */
        CHECK(text.find("Roaming") != std::string::npos);
    }

    /*---- export event + profile dir scrub --------------------
           The export event's target lives under the real
           user's profile; the journal is bundled verbatim on
           the NEXT export, so the recorded form must already
           be the redacted one. */
    std::printf("-- export path scrub\n");
    {
        /* the recorded event detail is redacted in the
           journal itself */
        bool saw_export = false;
        for(const QVariant& e : diag.eventLog())
        {
            const QVariantMap ev = e.toMap();
            if(ev.value(QStringLiteral("kind")).toString() ==
               QStringLiteral("export"))
            {
                saw_export = true;
                const QString d =
                    ev.value(QStringLiteral("detail")).toString();
                CHECK(d.contains(QStringLiteral("<profile>")));
                CHECK(!d.contains(QDir::homePath()));
            }
        }
        CHECK(saw_export);

        /* second export: the journal entry lands in the
           bundle — the real username must appear nowhere. */
        QTemporaryDir tmp2;
        CHECK(tmp2.isValid());
        const QString path2 =
            tmp2.filePath(QStringLiteral("diag2.json"));
        CHECK(diag.exportDiagnostics(path2));
        std::ifstream in2(path2.toStdString(),
                          std::ios::binary);
        const std::string text2(
            (std::istreambuf_iterator<char>(in2)),
            std::istreambuf_iterator<char>());
        const QString home_user =
            QFileInfo(QDir::homePath()).fileName();
        if(!home_user.isEmpty())
        {
            CHECK(text2.find(home_user.toStdString()) ==
                  std::string::npos);
        }
        CHECK(text2.find("<profile>") != std::string::npos);
    }

    bridge.stop();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
