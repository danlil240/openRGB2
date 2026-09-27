/*---------------------------------------------------------*\
||| DiagnosticsController.cpp                                ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
||\*---------------------------------------------------------*/

#include "DiagnosticsController.h"

#include "EngineBridge.h"
#include "JsonVariant.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTime>
#include <QUrl>

namespace openrgb2
{
namespace ui
{

namespace
{

/* Mirrors studio::NameMatches (the Layout model's display
   hint): case-insensitive equality or containment; an empty
   wanted matches everything. Display-side resolution only —
   the engine resolves bindings authoritatively. */
bool NameMatches(const QString& wanted, const QString& actual)
{
    if(wanted.isEmpty())
    {
        return true;
    }
    const QString w = wanted.toLower();
    const QString a = actual.toLower();
    return a == w || a.contains(w);
}

/* Does the scene binding resolve to this controller snapshot?
   Same field precedence as LayoutEditModel::MatchSnapshots:
   name (or key.device) -> vendor -> serial -> location. */
bool BindingMatchesSnapshot(const QVariantMap& binding,
                            const QVariantMap& snap)
{
    const QVariantMap key = snap.value(QStringLiteral("key")).toMap();
    const QString cname  =
        binding.value(QStringLiteral("controller_name")).toString();
    const QString vendor =
        binding.value(QStringLiteral("vendor")).toString();
    const QString serial =
        binding.value(QStringLiteral("serial")).toString();
    const QString loc    =
        binding.value(QStringLiteral("location")).toString();

    if(!NameMatches(cname, snap.value(QStringLiteral("name")).toString()) &&
       !NameMatches(cname, key.value(QStringLiteral("device")).toString()))
    {
        return false;
    }
    if(!vendor.isEmpty() &&
       !NameMatches(vendor,
                    snap.value(QStringLiteral("vendor")).toString()))
    {
        return false;
    }
    if(!serial.isEmpty() &&
       serial != key.value(QStringLiteral("serial")).toString())
    {
        return false;
    }
    if(!loc.isEmpty() &&
       loc != key.value(QStringLiteral("location")).toString())
    {
        return false;
    }
    return true;
}

/* Redaction patterns — unique hardware identifiers that must
   never leave the machine in an export (committed-profiles
   rule). MAC forms (colon/hyphen separated or 12 bare hex
   digits) cover the wireless receiver identity the engine
   reports inside location/detail strings. */
const QRegularExpression& MacPattern()
{
    static const QRegularExpression re(
        QStringLiteral(
            "\\b(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}\\b"
            "|\\b[0-9A-Fa-f]{12}\\b"));
    return re;
}

/* Private-path scrub — a support bundle never carries the
   user's profile location (the Milestone 6 redaction rule
   applies to exports, and engine findings embed the managed
   config/exe dirs verbatim — both live under the profile).
   Applied to EVERY string value, not just keyed identifier
   fields. Two layers: the resolved home dir verbatim in each
   separator spelling a decoded string can carry — forward
   slash (QDir form), single backslash (native), doubled
   backslash (a JSON-encoded path inside an already-decoded
   string) — and the generic <drive>:\Users\<name> form for
   any user. Only the NAME segment is private: the rest of
   the path stays readable for the support reader. */
const QRegularExpression& UsersDirPattern()
{
    static const QRegularExpression re(QStringLiteral(
        R"((\b[A-Za-z]:(?:\\\\|\\|/)Users(?:\\\\|\\|/))[^\\/\s"']+)"));
    return re;
}

void ScrubPrivatePaths(QString& s)
{
    /* Exact-match scrub of THIS machine's home — covers
       profiles outside <drive>:\Users (redirected or
       non-standard homes) the generic pattern cannot name.
       Guarded: a drive-root or "/" home must never blank
       every path. */
    const QString home = QDir::homePath();
    if(home.size() > 3)
    {
        QString bs = home;
        bs.replace(QLatin1Char('/'), QLatin1Char('\\'));
        QString es = bs;
        es.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
        const QString variants[] = {home, bs, es};
        for(const QString& v : variants)
        {
            s.replace(v, QStringLiteral("<profile>"),
                      Qt::CaseInsensitive);
        }
    }
    s.replace(UsersDirPattern(),
              QStringLiteral("\\1<redacted>"));
}

QString RedactString(const QString& s)
{
    QString out = s;
    out.replace(MacPattern(), QStringLiteral("[redacted]"));
    ScrubPrivatePaths(out);
    return out;
}

} /* anonymous namespace */

DiagnosticsController::DiagnosticsController(EngineBridge* bridge,
                                             QObject* parent)
    : QObject(parent), bridge_(bridge)
{
    refresh_timer_.setSingleShot(false);
    refresh_timer_.setInterval(interval_ms_);
    connect(&refresh_timer_, &QTimer::timeout,
            this, [this]
            {
                if(active_ && bridge_ && bridge_->connected())
                {
                    refresh();
                }
                /* Preview cadence ages out on its own — tick
                   the display even without new pushes. */
                RecomputeRates();
            });

    if(bridge_)
    {
        /* Every store the derived views join on. */
        connect(bridge_, &EngineBridge::playbackChanged,
                this, [this] { RecomputeRates(); });
        connect(bridge_, &EngineBridge::deliveryStatsChanged,
                this, [this]
                { RecomputeRates(); RecomputeControllers(); });
        connect(bridge_, &EngineBridge::snapshotsChanged,
                this, [this] { RecomputeControllers(); });
        connect(bridge_, &EngineBridge::sceneChanged,
                this, [this] { RecomputeControllers(); });
        connect(bridge_, &EngineBridge::diagnosticsChanged,
                this, [this]
                { RecomputeOwnership(); RecomputeRates(); });
        connect(bridge_, &EngineBridge::stateReportChanged,
                this, [this] { RecomputeOwnership(); });
        connect(bridge_, &EngineBridge::frameStatusChanged,
                this, [this] { OnFrameStatus(); });

        /* The bounded journal. */
        connect(bridge_, &EngineBridge::eventReceived,
                this, &DiagnosticsController::OnEventReceived);
        connect(bridge_, &EngineBridge::replyReceived,
                this, [this](int, const QString& type,
                             const QVariantMap& body)
                {
                    /* Only failures are journal-worthy — the
                       stores already carry every reply. */
                    if(type == QLatin1String("ERROR"))
                    {
                        AppendEvent(QStringLiteral("error"),
                            body.value(QStringLiteral("message"))
                                .toString());
                    }
                });
        connect(bridge_, &EngineBridge::connectionStateChanged,
                this, [this]
                {
                    AppendEvent(QStringLiteral("connection"),
                                bridge_->connectionState());
                });
        connect(bridge_, &EngineBridge::engineLost,
                this, [this](const QString& reason)
                {
                    AppendEvent(QStringLiteral("connection"),
                                QStringLiteral("lost: ") + reason);
                });
        connect(bridge_, &EngineBridge::protocolError,
                this, [this](const QString& detail)
                {
                    AppendEvent(QStringLiteral("error"), detail);
                });
    }

    RecomputeRates();
    RecomputeControllers();
    RecomputeOwnership();
}

/*---------------------------------------------------------*\
||| Properties / refresh cadence                           ||
\*---------------------------------------------------------*/

void DiagnosticsController::setActive(bool a)
{
    if(active_ == a)
    {
        return;
    }
    active_ = a;
    if(a)
    {
        refresh();            /* paint immediately on show */
        refresh_timer_.start();
    }
    else
    {
        refresh_timer_.stop();
    }
    emit activeChanged();
}

void DiagnosticsController::setRefreshIntervalMs(int ms)
{
    if(ms < 250 || interval_ms_ == ms)
    {
        return;   /* floor: a status surface, never a flood */
    }
    interval_ms_ = ms;
    refresh_timer_.setInterval(ms);
    emit refreshIntervalMsChanged();
}

void DiagnosticsController::refresh()
{
    if(!bridge_)
    {
        return;
    }
    /* Read-only pulls only (the page's design contract):
       DIAG_GET, GET_STATE, GET_DELIVERY_STATS, GET_SNAPSHOTS. */
    bridge_->getDiagnostics();
    bridge_->getState();
    bridge_->getDeliveryStats();
    bridge_->getSnapshots();
}

/*---------------------------------------------------------*\
||| Derived views                                          ||
\*---------------------------------------------------------*/

void DiagnosticsController::RecomputeRates()
{
    const QVariantMap pb = bridge_
        ? bridge_->playback() : QVariantMap();
    const QVariantMap st =
        pb.value(QStringLiteral("status")).toMap();

    /* 1 — Configured playback rate: the uploaded loop's
       effective geometry (loop-builder output). Configured,
       never measured. */
    const double interval =
        st.value(QStringLiteral("effective_frame_interval_s"))
            .toDouble();
    const int frames =
        st.value(QStringLiteral("effective_frame_count")).toInt();
    const bool configured = interval > 0.0;

    /* 2 — Physical playback rate: only what the wire
       measured PHYSICALLY. The wire contract
       (MeasurementProvenance -> ProvenanceName) is that only
       "physically_measured" claims a physical measurement —
       "configured" and "transport_measured" describe other
       channels — so anything else, or a zero interval, is
       UNMEASURED, never rendered as 0 FPS. */
    const QVariantMap phys =
        st.value(QStringLiteral("physical")).toMap();
    const double  pint =
        phys.value(QStringLiteral("frame_interval_s")).toDouble();
    const QString pprov =
        phys.value(QStringLiteral("provenance")).toString();
    const bool pmeasured =
        pint > 0.0 &&
        pprov == QLatin1String("physically_measured");

    /* 3 — Live delivery rate: DELIVERY_STATS aggregate
       interval stats — frames the transport ACCEPTED
       (transport-admitted), never "displayed". */
    const QVariantMap ds = bridge_
        ? bridge_->deliveryStats() : QVariantMap();
    const QVariantMap agg =
        ds.value(QStringLiteral("aggregate")).toMap();
    const double mean_iv =
        agg.value(QStringLiteral("mean_interval_s")).toDouble();
    const qulonglong icount =
        agg.value(QStringLiteral("interval_count"))
            .toULongLong();
    const bool dmeasured = mean_iv > 0.0 && icount > 0;

    /* 4 — Preview cadence: the UI-side frame_status feed the
       preview repaints on — labeled preview, never physical
       output. */
    double preview_hz = 0.0;
    bool   preview_measured = false;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if(preview_stamps_.size() >= 2)
    {
        const qint64 span =
            preview_stamps_.back() - preview_stamps_.front();
        /* Stale feed -> unmeasured, not a frozen last value. */
        if(span > 0 &&
           now - preview_stamps_.back() <= kPreviewStaleMs)
        {
            preview_hz = 1000.0 *
                (double)(preview_stamps_.size() - 1) / (double)span;
            preview_measured = true;
        }
    }

    QVariantMap out;
    out.insert(QStringLiteral("configured_available"), configured);
    out.insert(QStringLiteral("configured_hz"),
               configured ? 1.0 / interval : 0.0);
    out.insert(QStringLiteral("configured_frame_count"), frames);
    out.insert(QStringLiteral("configured_frame_interval_s"),
               interval);
    out.insert(QStringLiteral("configured_cycle_s"),
               configured && frames > 0 ? interval * frames : 0.0);
    out.insert(QStringLiteral("physical_measured"), pmeasured);
    out.insert(QStringLiteral("physical_hz"),
               pmeasured ? 1.0 / pint : 0.0);
    out.insert(QStringLiteral("physical_frame_interval_s"), pint);
    out.insert(QStringLiteral("physical_cycle_s"),
               phys.value(QStringLiteral("cycle_s")));
    out.insert(QStringLiteral("physical_provenance"), pprov);
    out.insert(QStringLiteral("delivery_measured"), dmeasured);
    out.insert(QStringLiteral("delivery_hz"),
               dmeasured ? 1.0 / mean_iv : 0.0);
    out.insert(QStringLiteral("delivery_mean_interval_s"), mean_iv);
    out.insert(QStringLiteral("delivery_provenance"),
               agg.value(QStringLiteral("provenance")));
    out.insert(QStringLiteral("delivery_pending"),
               ds.value(QStringLiteral("pending")));
    out.insert(QStringLiteral("preview_measured"), preview_measured);
    out.insert(QStringLiteral("preview_hz"), preview_hz);

    if(out != rates_)
    {
        rates_ = out;
        emit ratesChanged();
    }
}

void DiagnosticsController::RecomputeControllers()
{
    const QVariantList snaps = bridge_
        ? bridge_->snapshots() : QVariantList();
    const QVariantMap ds = bridge_
        ? bridge_->deliveryStats() : QVariantMap();
    const QVariantList dstats =
        ds.value(QStringLiteral("controllers")).toList();
    const QVariantMap scene = bridge_
        ? bridge_->scene() : QVariantMap();
    const QVariantMap doc =
        scene.value(QStringLiteral("document")).toMap();
    const QVariantList bindings =
        doc.value(QStringLiteral("bindings")).toList();
    const QVariantList objects =
        doc.value(QStringLiteral("objects")).toList();

    /* Resolve every declared binding against live topology —
       the same display-side match the Layout model uses
       (name/vendor/serial/location). Bindings with no live
       controller land on unresolvedBindings — the honest
       "missing device" surface. */
    QVariantList unresolved;
    /* binding-id -> resolved snapshot index */
    QHash<QString, int> resolved_to;
    for(const QVariant& bv : bindings)
    {
        const QVariantMap b = bv.toMap();
        int found = -1;
        for(int i = 0; i < snaps.size(); ++i)
        {
            if(BindingMatchesSnapshot(b, snaps[i].toMap()))
            {
                found = i;
                break;
            }
        }
        const QString bid =
            b.value(QStringLiteral("id")).toString();
        if(found >= 0)
        {
            resolved_to.insert(bid, found);
        }
        else
        {
            QVariantMap u;
            u.insert(QStringLiteral("id"), bid);
            u.insert(QStringLiteral("controller_name"),
                     b.value(QStringLiteral("controller_name")));
            u.insert(QStringLiteral("zone_name"),
                     b.value(QStringLiteral("zone_name")));
            unresolved.push_back(u);
        }
    }

    /* Objects per binding id. */
    QHash<QString, int> objects_per_binding;
    for(const QVariant& ov : objects)
    {
        const QString bid =
            ov.toMap().value(QStringLiteral("binding")).toString();
        if(!bid.isEmpty())
        {
            ++objects_per_binding[bid];
        }
    }

    QVariantList rows;
    int idx = 0;
    for(const QVariant& sv : snaps)
    {
        const QVariantMap s = sv.toMap();
        const QVariantMap key =
            s.value(QStringLiteral("key")).toMap();

        /* Join on the delivery row's DeviceKey — match on the
           full composite (device+serial+location), falling
           back to device alone when identity fields are
           empty. */
        QVariantMap stat_row;
        for(const QVariant& cv : dstats)
        {
            const QVariantMap c = cv.toMap();
            const QVariantMap ck =
                c.value(QStringLiteral("key")).toMap();
            if(ck.value(QStringLiteral("device")).toString() !=
               key.value(QStringLiteral("device")).toString())
            {
                continue;
            }
            const QString cser =
                ck.value(QStringLiteral("serial")).toString();
            const QString kser =
                key.value(QStringLiteral("serial")).toString();
            if(!kser.isEmpty() && !cser.isEmpty() && cser != kser)
            {
                continue;
            }
            const QString cloc =
                ck.value(QStringLiteral("location")).toString();
            const QString kloc =
                key.value(QStringLiteral("location")).toString();
            if(!kloc.isEmpty() && !cloc.isEmpty() && cloc != kloc)
            {
                continue;
            }
            stat_row = c;
            break;
        }

        QVariantMap stats =
            stat_row.value(QStringLiteral("stats")).toMap();
        const double mean_iv =
            stats.value(QStringLiteral("mean_interval_s"))
                .toDouble();
        const qulonglong icount =
            stats.value(QStringLiteral("interval_count"))
                .toULongLong();
        QVariantMap stv = stats;
        stv.insert(QStringLiteral("delivery_measured"),
                   mean_iv > 0.0 && icount > 0);
        stv.insert(QStringLiteral("delivery_hz"),
                   (mean_iv > 0.0 && icount > 0)
                       ? 1.0 / mean_iv : 0.0);

        int bound_bindings = 0;
        int bound_objects  = 0;
        for(auto it = resolved_to.constBegin();
            it != resolved_to.constEnd(); ++it)
        {
            if(it.value() == idx)
            {
                ++bound_bindings;
                bound_objects +=
                    objects_per_binding.value(it.key());
            }
        }

        QStringList zone_names;
        for(const QVariant& zv :
                s.value(QStringLiteral("zones")).toList())
        {
            zone_names.push_back(
                zv.toMap().value(QStringLiteral("name"))
                    .toString());
        }

        QVariantMap row;
        row.insert(QStringLiteral("name"),       s.value(QStringLiteral("name")));
        row.insert(QStringLiteral("vendor"),     s.value(QStringLiteral("vendor")));
        row.insert(QStringLiteral("device_type"),s.value(QStringLiteral("device_type")));
        row.insert(QStringLiteral("connection"), s.value(QStringLiteral("connection")));
        row.insert(QStringLiteral("led_count"),  s.value(QStringLiteral("led_count")));
        row.insert(QStringLiteral("zone_count"), zone_names.size());
        row.insert(QStringLiteral("zones"),      zone_names);
        row.insert(QStringLiteral("modes"),      s.value(QStringLiteral("modes")));
        /* Firmware is reported only when the wire carries it
           (no firmware field exists in v1 — forward-compat
           pass-through; absent stays absent, never a guess). */
        row.insert(QStringLiteral("firmware"),   s.value(QStringLiteral("firmware")));
        row.insert(QStringLiteral("bound_bindings"), bound_bindings);
        row.insert(QStringLiteral("bound_objects"),  bound_objects);
        row.insert(QStringLiteral("stats"),      stv);
        row.insert(QStringLiteral("pending"),
                   stat_row.value(QStringLiteral("pending")));
        row.insert(QStringLiteral("budget_ms"),
                   stat_row.value(QStringLiteral("budget_ms")));
        rows.push_back(row);
        ++idx;
    }

    controllers_ = rows;
    unresolved_  = unresolved;
    emit controllersChanged();
}

void DiagnosticsController::RecomputeOwnership()
{
    /* DIAGNOSTICS carries the freshest pull; STATE (welcome +
       refresh) is the fallback. Null/absent -> reported:false,
       the honest "nothing published" surface. */
    QVariantMap own;
    if(bridge_)
    {
        own = bridge_->diagnostics()
                  .value(QStringLiteral("ownership")).toMap();
        if(own.isEmpty())
        {
            own = bridge_->stateReport()
                      .value(QStringLiteral("ownership")).toMap();
        }
    }

    QVariantMap out;
    const bool reported = !own.isEmpty();
    const QVariantList conflicts =
        own.value(QStringLiteral("conflicts")).toList();
    out.insert(QStringLiteral("reported"), reported);
    out.insert(QStringLiteral("scanned"),
               own.value(QStringLiteral("scanned")).toBool());
    out.insert(QStringLiteral("conflicts"), conflicts);
    out.insert(QStringLiteral("notices"),
               own.value(QStringLiteral("notices")).toList());
    out.insert(QStringLiteral("conflictCount"), conflicts.size());
    out.insert(QStringLiteral("backend"),
               own.value(QStringLiteral("backend")).toMap());
    out.insert(QStringLiteral("config"),
               own.value(QStringLiteral("config")).toMap());

    if(out != ownership_)
    {
        ownership_ = out;
        emit ownershipChanged();
    }
}

/*---------------------------------------------------------*\
||| Preview cadence + journal                              ||
\*---------------------------------------------------------*/

void DiagnosticsController::OnFrameStatus()
{
    preview_stamps_.push_back(QDateTime::currentMSecsSinceEpoch());
    while(preview_stamps_.size() > (size_t)kPreviewWindowBound)
    {
        preview_stamps_.pop_front();
    }
    RecomputeRates();
}

void DiagnosticsController::OnEventReceived(const QString& kind,
                                            const QVariantMap&)
{
    AppendEvent(kind, QString());
}

void DiagnosticsController::AppendEvent(const QString& kind,
                                        const QString& detail)
{
    QVariantMap e;
    e.insert(QStringLiteral("time"),
             QTime::currentTime().toString(QStringLiteral("HH:mm:ss")));
    e.insert(QStringLiteral("kind"),   kind);
    e.insert(QStringLiteral("detail"), detail);
    event_log_.push_back(e);
    while(event_log_.size() > kEventLogBound)
    {
        event_log_.removeFirst();
    }
    emit eventLogChanged();
}

void DiagnosticsController::clearEventLog()
{
    if(event_log_.isEmpty())
    {
        return;
    }
    event_log_.clear();
    emit eventLogChanged();
}

/*---------------------------------------------------------*\
||| Export — redacted diagnostic bundle                    ||
\*---------------------------------------------------------*/

void DiagnosticsController::RedactJson(nlohmann::json& j)
{
    if(j.is_object())
    {
        for(auto it = j.begin(); it != j.end(); ++it)
        {
            /* serial/location members are unique hardware
               identifiers whatever their shape — redact
               unconditionally. */
            if(it.key() == "serial" || it.key() == "location")
            {
                it.value() = "[redacted]";
                continue;
            }
            RedactJson(it.value());
        }
    }
    else if(j.is_array())
    {
        for(auto& e : j)
        {
            RedactJson(e);
        }
    }
    else if(j.is_string())
    {
        const QString s =
            QString::fromStdString(j.get<std::string>());
        const QString r = RedactString(s);
        if(r != s)
        {
            j = r.toStdString();
        }
    }
}

nlohmann::json DiagnosticsController::ExportBundle() const
{
    nlohmann::json bundle = nlohmann::json::object({
        { "tool",            "openrgb2-ui" },
        { "bundle",          "diagnostics" },
        { "wire_version",    1 },
        { "generated_at",
          QDateTime::currentDateTime()
              .toString(Qt::ISODateWithMs)
              .toStdString() },
    });
    if(bridge_)
    {
        bundle["connection_state"] =
            bridge_->connectionState().toStdString();
        bundle["server"]         = VariantToJson(bridge_->serverInfo());
        bundle["state"]          = VariantToJson(bridge_->stateReport());
        bundle["diagnostics"]    = VariantToJson(bridge_->diagnostics());
        bundle["playback_view"]  = VariantToJson(bridge_->playbackView());
        bundle["delivery_stats"] = VariantToJson(bridge_->deliveryStats());
        bundle["snapshots"]      = VariantToJson(bridge_->snapshots());
        bundle["reactive"]       = VariantToJson(bridge_->reactive());
    }
    bundle["rates"]     = VariantToJson(rates_);
    bundle["ownership"] = VariantToJson(ownership_);
    {
        nlohmann::json log = nlohmann::json::array();
        for(const QVariant& e : event_log_)
        {
            log.push_back(VariantToJson(e));
        }
        bundle["event_log"] = std::move(log);
    }
    bundle["notes"] =
        "Unique identifiers (serial numbers, hardware "
        "locations, MAC addresses) and user-profile filesystem "
        "paths are redacted in this export. Rates labeled "
        "transport-admitted are frames "
        "accepted by the transport, never displayed-physical "
        "rates.";
    RedactJson(bundle);
    return bundle;
}

QString DiagnosticsController::defaultExportPath() const
{
    QString dir =
        QStandardPaths::writableLocation(
            QStandardPaths::DocumentsLocation);
    if(dir.isEmpty())
    {
        dir = QDir::homePath();
    }
    return QDir(dir).filePath(
        QStringLiteral("openrgb2-diagnostics-%1.json")
            .arg(QDateTime::currentDateTime()
                     .toString(QStringLiteral("yyyyMMdd-HHmmss"))));
}

bool DiagnosticsController::exportDiagnostics(const QString& path)
{
    QString target = path;
    if(target.startsWith(QLatin1String("file://")))
    {
        target = QUrl(target).toLocalFile();
    }
    if(target.isEmpty())
    {
        target = defaultExportPath();
    }

    const nlohmann::json bundle = ExportBundle();
    QFile f(target);
    if(!f.open(QIODevice::WriteOnly | QIODevice::Truncate |
               QIODevice::Text))
    {
        export_error_ =
            QStringLiteral("cannot write ") + target +
            QStringLiteral(": ") + f.errorString();
        emit exportErrorChanged();
        return false;
    }
    const std::string text = bundle.dump(2);
    f.write(text.data(), (qint64)text.size());
    f.close();

    export_error_.clear();
    emit exportErrorChanged();
    last_export_ = target;
    emit lastExportChanged();
    /* The journal is bundled verbatim on the next export — the
       target lives under the user's profile, so the recorded
       form is already the redacted one (the local display form
       keeps the real path via last_export_). */
    AppendEvent(QStringLiteral("export"),
                QStringLiteral("bundle written to ") +
                RedactString(target));
    return true;
}

} /* namespace ui */
} /* namespace openrgb2 */
