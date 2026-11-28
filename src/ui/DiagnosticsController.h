/*---------------------------------------------------------*\
||| DiagnosticsController.h                                  ||
|||                                                           ||
|||   QObject half of the Devices & Diagnostics view       ||
|||   (Task 4.7, plan section 5 — the honesty checkpoint). ||
|||   Composes the read-only diagnostic surface on top of  ||
|||   EngineBridge stores:                                 ||
|||                                                           ||
|||   - FOUR labeled rates, never conflated (plan section  ||
|||     5): configured playback rate (the uploaded loop's  ||
|||     effective geometry), physical playback rate (the   ||
|||     wire's own measurement — unmeasured stays           ||
|||     unmeasured, no camera/photodiode exists), live      ||
|||     delivery rate (DELIVERY_STATS transport-admitted,   ||
|||     never "displayed"), preview cadence (the UI-side    ||
|||     frame_status feed — labeled preview)               ||
|||   - per-controller device/mapping rows: topology,      ||
|||     resolved bindings, transport counters              ||
|||   - the engine's ownership report verbatim (competing  ||
|||     owners + managed-config sole-writer findings —     ||
|||     report-only; no kill/takeover controls exist here) ||
|||   - playback/sync state with the engine's own reasons  ||
|||   - a bounded UI-side event journal                    ||
|||   - diagnostic export: one JSON bundle, unique         ||
|||     identifiers (serials / MACs / locations) redacted  ||
|||     per the committed-profiles rule                    ||
|||                                                           ||
|||   READ-ONLY by design: every verb issued here is a     ||
|||   pull (GET_STATE / DIAG_GET / GET_DELIVERY_STATS /    ||
|||   GET_SNAPSHOTS). Nothing on this page mutates engine  ||
|||   or hardware state — identify-style probes belong to  ||
|||   the Layout view. The one exception (E-12c):          ||
|||   resetTiming() clears the engine's measurement        ||
|||   histograms — no output/scene/hardware effect.        ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
||\*---------------------------------------------------------*/

#pragma once

#include <nlohmann/json.hpp>

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <deque>
#include <functional>

namespace openrgb2
{
namespace ui
{

class EngineBridge;

class DiagnosticsController : public QObject
{
    Q_OBJECT

    /* Page-visible gate — the refresh timer only polls while
       the view is showing (bounded cadence; pushes still land
       regardless). */
    Q_PROPERTY(bool active READ active WRITE setActive
                   NOTIFY activeChanged)
    /* Pull cadence while active (default 2000 ms — the page
       is a status surface, not a per-frame feed). */
    Q_PROPERTY(int refreshIntervalMs READ refreshIntervalMs
                   WRITE setRefreshIntervalMs
                   NOTIFY refreshIntervalMsChanged)

    /* The four labeled rates — see the file banner. Keys:
       configured_hz/configured_available,
       configured_frame_count/configured_cycle_s,
       physical_hz/physical_measured/physical_provenance/
       physical_cycle_s,
       delivery_hz/delivery_measured/delivery_provenance/
       delivery_pending,
       preview_hz/preview_measured. A *_measured:false /
       *_available:false field means UNMEASURED — QML renders
       it as such, never as zero. */
    Q_PROPERTY(QVariantMap rates READ rates NOTIFY ratesChanged)

    /* Per-controller rows joining SNAPSHOTS topology with
       DELIVERY_STATS counters and scene-binding resolution:
       [{name,vendor,device_type,connection,led_count,
         zone_count,zones:[names], bound_objects,
         bound_bindings, stats:{submitted,transport_completed,
         replaced_pending,rejected,suppressed,
         transport_rejected,last_interval_s,mean_interval_s,
         delivery_hz,provenance}, pending,budget_ms}]. */
    Q_PROPERTY(QVariantList controllers READ controllers
                   NOTIFY controllersChanged)

    /* Scene bindings the document declares that resolve to NO
       live controller — the honest "device missing" list. */
    Q_PROPERTY(QVariantList unresolvedBindings
                   READ unresolvedBindings NOTIFY controllersChanged)

    /* The engine's ownership report (STATE/DIAGNOSTICS
       "ownership") verbatim plus derived counts:
       {reported, scanned, conflicts:[{owner,exe,pid,detail}],
        notices:[...], conflictCount, config:{checked,
        findings[],legacy_producers[],enabled_plugins[]},
        backend:{listening,attached}}. reported:false means
       the engine published nothing — honest absence, not a
       clean scan. scanned:false means the engine skipped the
       scan — NOT "no conflicts". */
    Q_PROPERTY(QVariantMap ownership READ ownership
                   NOTIFY ownershipChanged)

    /* Bounded UI-side journal (newest last, capped at
       kEventLogBound): [{time,kind,detail}] — push kinds,
       ERROR replies, connection transitions, protocol
       errors. This is the UI's own journal, not the
       engine's log. Per-frame frame_status pushes are NOT
       journaled — telemetry that would evict every real
       event within seconds at live-effects rates; the
       bridge's frameStatus store and the preview-cadence
       window carry it. Draft-preview `frame` events are
       skipped for the same reason (their `status` events
       stay). */
    Q_PROPERTY(QVariantList eventLog READ eventLog
                   NOTIFY eventLogChanged)

    /* Audit E-12c timing card: the engine's cumulative timing
       histograms, pulled with STATE / DELIVERY_STATS on the
       page's refresh cadence:
         {can_reset,
          tick:<hist>,                       STATE scene.tick_hist
          aggregate:{queue:<hist>, send:<hist>},
          lanes:[{name, wireless, queue:<hist>, send:<hist>}]}
       <hist> = HistogramView: {available, measured, total,
       p50/p95/p99 (ms or null), p50_text/p95_text/p99_text,
       provenance, bars:[{label, count, frac}]}. Percentiles are
       bucket upper edges ("17 ms"); a null percentile with
       samples reads "> 250 ms"; no samples reads "—" —
       unmeasured, never 0. Transport-measured, never
       "displayed". */
    Q_PROPERTY(QVariantMap timing READ timing NOTIFY timingChanged)

    Q_PROPERTY(QString lastExport READ lastExport
                   NOTIFY lastExportChanged)
    Q_PROPERTY(QString exportError READ exportError
                   NOTIFY exportErrorChanged)

public:
    explicit DiagnosticsController(EngineBridge* bridge,
                                   QObject* parent = nullptr);

    static constexpr int kEventLogBound       = 128;
    static constexpr int kPreviewWindowBound  = 16;
    /* The preview cadence is only meaningful while events
       actually arrive — older than this the measured rate
       is reported stale/unmeasured. */
    static constexpr qint64 kPreviewStaleMs   = 10000;

    /*---- requests (all read-only pulls) --------------------*/
    Q_INVOKABLE void refresh();     /* state + diagnostics +
                                       delivery + snapshots  */
    /* Writes the redacted bundle (ExportBundle) to `path` —
       overwrites. Returns false + exportError on I/O
       failure. */
    Q_INVOKABLE bool exportDiagnostics(const QString& path);
    Q_INVOKABLE QString defaultExportPath() const;
    Q_INVOKABLE void   clearEventLog();
    /* E-12c: OUTPUT_COMMAND reset_stats, then a re-pull — the
       ONE non-read verb on this page. It clears measurement
       histograms only (no output, scene or hardware effect) so
       a before/after comparison starts clean. False when the
       engine does not advertise output_control_v1. */
    Q_INVOKABLE bool   resetTiming();

    /*---- R-10 guided fix steps ------------------------------*/
    /* Opens one WHITELISTED system surface for the ownership
       guide — never an engine-supplied or arbitrary string:
         "dynamic_lighting_settings" -> Settings > Personalization
                                        > Dynamic Lighting
         "services"                  -> the Services console
       Settings opens via absolute explorer.exe (a URI needs the
       shell), Services via absolute mmc.exe; both with a sanitised
       environment (the UI usually runs elevated — audit U-6).
       Returns false for any other target. The user takes the
       action there; OpenRGB2 changes nothing itself. */
    Q_INVOKABLE bool openOwnershipTarget(const QString& target);
    /* Puts a guide command (e.g. `sc stop ...`) on the
       clipboard for the user to run themselves. */
    Q_INVOKABLE bool copyText(const QString& text);
    /* Replaces the process launch: (absolute program,
       args) -> started. */
    void setShellLauncherForTesting(
        std::function<bool(const QString&, const QStringList&)>
            launcher) { shell_launcher_ = std::move(launcher); }

    /*---- state ----------------------------------------------*/
    bool         active() const            { return active_; }
    int          refreshIntervalMs() const { return interval_ms_; }
    QVariantMap  rates() const             { return rates_; }
    QVariantList controllers() const       { return controllers_; }
    QVariantList unresolvedBindings() const{ return unresolved_; }
    QVariantMap  ownership() const         { return ownership_; }
    QVariantMap  timing() const            { return timing_; }
    QVariantList eventLog() const          { return event_log_; }
    QString      lastExport() const        { return last_export_; }
    QString      exportError() const       { return export_error_; }

    void setActive(bool a);
    void setRefreshIntervalMs(int ms);

    /*---- export (exposed for the offline suite) -------------*/
    /* Recursive redaction applied to every value in the
       bundle: object members named "serial" or "location" and
       any MAC-shaped / raw-hex-identifier substring inside
       string values become "[redacted]", and private
       filesystem paths — the machine's profile dir (every
       separator spelling) and any <drive>:\Users\<name> —
       collapse to "<profile>" / "<drive>:…\<redacted>" so
       engine findings and journaled export paths never leak
       the user's directory (the committed-profiles rule;
       plan section 6/Milestone 6 redaction requirement
       applies to exports too). */
    static void     RedactJson(nlohmann::json& j);
    nlohmann::json  ExportBundle() const;

    /* E-12c: one wire histogram ({edges_ms, counts, total, p50,
       p95, p99, provenance?}) -> the QML view map documented on
       the `timing` property. An absent/empty map (an engine
       without the fields) -> available:false. */
    static QVariantMap HistogramView(const QVariantMap& wire);

signals:
    void activeChanged();
    void refreshIntervalMsChanged();
    void ratesChanged();
    void controllersChanged();
    void ownershipChanged();
    void timingChanged();
    void eventLogChanged();
    void lastExportChanged();
    void exportErrorChanged();

private:
    void RecomputeRates();
    void RecomputeControllers();
    void RecomputeOwnership();
    void RecomputeTiming();
    void AppendEvent(const QString& kind, const QString& detail);
    void OnEventReceived(const QString& kind,
                         const QVariantMap& data);
    void OnFrameStatus();

    EngineBridge*  bridge_;
    bool           active_      = false;
    int            interval_ms_ = 2000;
    QTimer         refresh_timer_;

    QVariantMap    rates_;
    QVariantList   controllers_;
    QVariantList   unresolved_;
    QVariantMap    ownership_;
    QVariantMap    timing_;
    QVariantList   event_log_;
    /* One change signal per event-loop turn — a wire burst
       rebuilds the QML tail once, not once per event
       (Task 4.8 minor). */
    bool           log_signal_pending_ = false;
    QString        last_export_;
    QString        export_error_;
    std::function<bool(const QString&, const QStringList&)>
                   shell_launcher_;   /* test seam (R-10) */

    /* UI-side preview cadence basis: arrival stamps of the
       frame_status pushes the preview repaints on. */
    std::deque<qint64> preview_stamps_;
};

} /* namespace ui */
} /* namespace openrgb2 */
