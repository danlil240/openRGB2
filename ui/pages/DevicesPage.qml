// SPDX-License-Identifier: GPL-2.0-or-later
//
// Devices & Diagnostics (Task 4.7 — plan section 5, the
// honesty checkpoint): engine-reported measurements with
// their provenance, ownership conflicts verbatim from the
// engine's scan, per-controller transport counters, and the
// redacted diagnostic export.
//
// Every rate is labeled by what it measures and the four are
// never conflated: configured playback rate ≠ measured
// physical rate ≠ transport-admitted delivery rate ≠ preview
// cadence. Unmeasured stays unmeasured — no fabricated
// numbers, no kill/takeover controls (the ownership surface
// is report-only by design).
import QtQuick
import QtQuick.Layouts
import QtQuick.Dialogs
import QtQuick.Window
import OpenRGB2.UI.theme
import OpenRGB2.UI.components

Item {
    id: root
    objectName: "devicesPage"

    readonly property bool engineUp:
        typeof bridge !== "undefined" && bridge.connected
    readonly property bool hasCtl:
        typeof diagCtl !== "undefined"
    readonly property var rates:
        root.hasCtl ? diagCtl.rates : ({})
    readonly property var ownership:
        root.hasCtl ? diagCtl.ownership : ({})
    /* Newest-first tail of the bounded UI-side journal. */
    readonly property var logTail:
        (root.hasCtl ? diagCtl.eventLog : []).slice(-24).reverse()

    function fmt(v, suffix) {
        return (v === undefined || v === null) ? "—" : v + (suffix || "");
    }
    function rateText(hz, measured, suffix) {
        return measured ? hz.toFixed(1) + (suffix || " /s")
                        : qsTr("unmeasured");
    }

    /* The refresh cadence lives in the controller — active
       while this view is on screen AND the window is shown:
       a minimized/hidden window doesn't poll (Task 4.8
       minor — polling work nobody can see isn't honest
       "auto-refresh"). */
    function syncDiagActive() {
        const w = Window.window;
        const shown = !w
                      || (w.visible
                          && w.visibility !== Window.Minimized);
        if(root.hasCtl)
            diagCtl.active = root.visible && shown;
    }
    Component.onCompleted: syncDiagActive()
    onVisibleChanged: syncDiagActive()
    /* The target resolves through the page item — a bare
       `Window.window` here looks the attached property up
       on this (non-Item) Connections object, binds null,
       and the handlers never fire (the pitfall SettingsPage
       documents). */
    Connections {
        target: root.Window.window
        function onVisibleChanged()    { root.syncDiagActive(); }
        function onVisibilityChanged() { root.syncDiagActive(); }
    }

    Flickable {
        anchors.fill: parent
        contentHeight: col.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: col
            width: root.width
            padding: Theme.spaceXL
            spacing: Theme.spaceL

            PageHeader {
                title: qsTr("Devices & Diagnostics")
                subtitle: qsTr("Engine-reported measurements and ownership — every number labeled by what it actually measures")
            }

            /*---- The four rates -------------------------------
               plan section 5: configured playback rate,
               measured physical playback rate, live delivery
               rate, and preview FPS — separately labeled,
               never conflated. */
            Card {
                title: qsTr("RATES — CONFIGURED vs MEASURED vs TRANSPORT vs PREVIEW")
                width: parent.width - 2 * col.padding
                GridLayout {
                    width: parent.width
                    columns: 4
                    columnSpacing: Theme.spaceXL
                    rowSpacing: Theme.spaceM
                    uniformCellWidths: true

                    /* 1 — configured playback rate. */
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spaceXS
                        Text {
                            text: qsTr("CONFIGURED PLAYBACK")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                            font.weight: Font.DemiBold
                            font.letterSpacing: 0.8
                        }
                        Text {
                            objectName: "rateConfigured"
                            text: root.rates.configured_available
                                  ? root.rates.configured_hz.toFixed(1)
                                    + qsTr(" fps")
                                  : qsTr("not configured")
                            color: root.rates.configured_available
                                   ? Theme.textPrimary : Theme.textSecondary
                            font.pixelSize: Theme.fontSizeDisplay
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("uploaded-loop effective rate — the loop builder's geometry, not a measurement")
                            color: Theme.textDisabled
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                        }
                    }

                    /* 2 — measured physical playback rate. */
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spaceXS
                        Text {
                            text: qsTr("PHYSICAL PLAYBACK")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                            font.weight: Font.DemiBold
                            font.letterSpacing: 0.8
                        }
                        Text {
                            objectName: "ratePhysical"
                            text: root.rateText(root.rates.physical_hz,
                                                root.rates.physical_measured,
                                                " fps")
                            color: root.rates.physical_measured
                                   ? Theme.textPrimary : Theme.textSecondary
                            font.pixelSize: Theme.fontSizeDisplay
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("requires a camera/photodiode — no physical measurement channel exists")
                            color: Theme.textDisabled
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                        }
                    }

                    /* 3 — live delivery rate. */
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spaceXS
                        Text {
                            text: qsTr("LIVE DELIVERY")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                            font.weight: Font.DemiBold
                            font.letterSpacing: 0.8
                        }
                        Text {
                            objectName: "rateDelivery"
                            text: root.rateText(root.rates.delivery_hz,
                                                root.rates.delivery_measured)
                            color: root.rates.delivery_measured
                                   ? Theme.textPrimary : Theme.textSecondary
                            font.pixelSize: Theme.fontSizeDisplay
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("transport-admitted frames — accepted by the transport, never \"displayed\"")
                            color: Theme.textDisabled
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                        }
                    }

                    /* 4 — preview cadence. */
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spaceXS
                        Text {
                            text: qsTr("PREVIEW")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                            font.weight: Font.DemiBold
                            font.letterSpacing: 0.8
                        }
                        Text {
                            objectName: "ratePreview"
                            text: root.rateText(root.rates.preview_hz,
                                                root.rates.preview_measured)
                            color: root.rates.preview_measured
                                   ? Theme.textPrimary : Theme.textSecondary
                            font.pixelSize: Theme.fontSizeDisplay
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("UI-side preview feed cadence — not output")
                            color: Theme.textDisabled
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }

            /*---- Device mapping --------------------------------
               Enumerated controllers joined with the scene's
               resolved bindings and the transport counters. */
            Card {
                title: qsTr("CONTROLLERS & MAPPING")
                width: parent.width - 2 * col.padding
                Column {
                    width: parent.width
                    spacing: Theme.spaceS
                    Repeater {
                        model: root.hasCtl ? diagCtl.controllers : []
                        Column {
                            width: parent.width
                            spacing: Theme.spaceXS
                            RowLayout {
                                width: parent.width
                                spacing: Theme.spaceL
                                Column {
                                    Layout.fillWidth: true
                                    Text {
                                        text: modelData.name || qsTr("controller")
                                        color: Theme.textPrimary
                                        font.pixelSize: Theme.fontSizeBody
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        text: qsTr("%1 · %2 LEDs · %3 zone(s): %4 · bound to %5 scene object(s)%6")
                                            .arg(modelData.vendor || qsTr("unknown vendor"))
                                            .arg(root.fmt(modelData.led_count))
                                            .arg(modelData.zone_count || 0)
                                            .arg((modelData.zones || []).join(", "))
                                            .arg(modelData.bound_objects || 0)
                                            .arg(modelData.firmware
                                                 ? " · fw " + modelData.firmware : "")
                                        color: Theme.textSecondary
                                        font.pixelSize: Theme.fontSizeSmall
                                        elide: Text.ElideRight
                                        width: parent.width
                                    }
                                }
                                StatusPill {
                                    text: modelData.connection || qsTr("unknown")
                                    tone: modelData.connection === "connected"
                                          ? "ok" : "warn"
                                }
                            }
                            Text {
                                width: parent.width
                                visible: modelData.stats
                                         && modelData.stats.delivery_measured
                                text: {
                                    const s = modelData.stats || {};
                                    return qsTr("transport-admitted %1/s · submitted %2 · replaced pending %3 · suppressed %4 · rejected %5 · transport-rejected %6 · pending %7")
                                        .arg((s.delivery_hz || 0).toFixed(1))
                                        .arg(root.fmt(s.submitted))
                                        .arg(root.fmt(s.replaced_pending))
                                        .arg(root.fmt(s.suppressed))
                                        .arg(root.fmt(s.rejected))
                                        .arg(root.fmt(s.transport_rejected))
                                        .arg(root.fmt(modelData.pending));
                                }
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSizeSmall
                            }
                            Text {
                                visible: !modelData.stats
                                         || !modelData.stats.delivery_measured
                                text: qsTr("no delivery counters yet — no frames submitted")
                                color: Theme.textDisabled
                                font.pixelSize: Theme.fontSizeSmall
                            }
                            Rectangle {
                                width: parent.width
                                height: 1
                                color: Theme.border
                            }
                        }
                    }
                    /* Bindings that resolve to no live
                       controller — the honest missing-device
                       surface (never silently re-bound). */
                    Repeater {
                        model: root.hasCtl ? diagCtl.unresolvedBindings : []
                        Text {
                            width: parent.width
                            text: qsTr("binding \"%1\" (%2%3) has no live controller — the device is missing or renamed")
                                .arg(modelData.id || "?")
                                .arg(modelData.controller_name || qsTr("unknown"))
                                .arg(modelData.zone_name
                                     ? " · " + modelData.zone_name : "")
                            color: Theme.warn
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                        }
                    }
                    EmptyState {
                        visible: !root.hasCtl
                                 || diagCtl.controllers.length === 0
                        anchors.horizontalCenter: parent.horizontalCenter
                        title: qsTr("No controllers")
                        detail: root.engineUp
                                ? qsTr("The engine enumerated no controllers.")
                                : qsTr("Controllers appear once the engine is connected.")
                    }
                }
            }

            /*---- Engine + backend link + playback -------------*/
            GridLayout {
                width: parent.width - 2 * col.padding
                columns: 2
                columnSpacing: Theme.spaceL
                rowSpacing: Theme.spaceL
                uniformCellWidths: true

                Card {
                    title: qsTr("ENGINE")
                    Layout.fillWidth: true
                    Column {
                        spacing: Theme.spaceS
                        Text {
                            text: qsTr("state: %1").arg(
                                root.engineUp
                                    ? (bridge.serverInfo.running === false
                                       ? qsTr("not running") : qsTr("running"))
                                    : qsTr("offline"))
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontSizeBody
                        }
                        Text {
                            text: qsTr("scene clock: %1 · uptime: %2")
                                .arg(root.engineUp
                                     ? root.fmt(bridge.stateReport.engine
                                                ? bridge.stateReport.engine.scene_clock_hz
                                                : undefined, " Hz")
                                     : "—")
                                .arg(root.fmt(bridge.diagnostics.engine
                                              ? bridge.diagnostics.engine.uptime_s
                                              : undefined, " s"))
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                        }
                        Text {
                            text: qsTr("IPC sessions: %1 · events dropped: %2")
                                .arg(root.fmt(bridge.stateReport.ipc
                                              ? bridge.stateReport.ipc.sessions_active
                                              : undefined))
                                .arg(root.fmt(bridge.stateReport.ipc
                                              ? bridge.stateReport.ipc.events_dropped
                                              : undefined))
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                        }
                        Row {
                            spacing: Theme.spaceM
                            AppButton {
                                text: qsTr("Refresh")
                                enabled: root.engineUp
                                onClicked:
                                    if(root.hasCtl) { diagCtl.refresh(); }
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("auto-refresh every %1 s while this page is visible")
                                    .arg(root.hasCtl
                                         ? Math.round(diagCtl.refreshIntervalMs / 1000)
                                         : "—")
                                color: Theme.textDisabled
                                font.pixelSize: Theme.fontSizeSmall
                            }
                        }
                    }
                }

                Card {
                    title: qsTr("BACKEND LINK")
                    Layout.fillWidth: true
                    Column {
                        spacing: Theme.spaceS
                        StatusPill {
                            text: root.engineUp
                                  ? (bridge.connection.state || qsTr("unknown"))
                                  : qsTr("offline")
                            tone: !root.engineUp ? "neutral"
                                : bridge.connection.state === "connected"
                                  ? "ok" : "warn"
                        }
                        Text {
                            text: qsTr("protocol v%1 · reconnect attempts: %2")
                                .arg(root.fmt(bridge.connection.protocol_version))
                                .arg(root.engineUp
                                     ? root.fmt(bridge.connection.reconnect_attempt)
                                     : "—")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                        }
                        Text {
                            visible: root.engineUp
                                     && !!bridge.connection.last_error
                            width: parent.width
                            text: bridge.connection.last_error || ""
                            color: Theme.warn
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                        }
                        Text {
                            visible: root.engineUp
                                     && !bridge.diagnostics.backend_counters
                            text: qsTr("backend counters: not published by the backend")
                            color: Theme.textDisabled
                            font.pixelSize: Theme.fontSizeSmall
                        }
                    }
                }

                Card {
                    title: qsTr("PLAYBACK & SYNC")
                    Layout.fillWidth: true
                    Column {
                        spacing: Theme.spaceS
                        Text {
                            text: {
                                if(!root.engineUp)
                                    return qsTr("offline");
                                const pv = bridge.playbackView;
                                return qsTr("mode: %1 (requested %2) · %3")
                                    .arg(pv.applied_mode || qsTr("unknown"))
                                    .arg(pv.requested_mode || "—")
                                    .arg(pv.upload || "");
                            }
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontSizeBody
                        }
                        Text {
                            /* Sync honesty: "unknown" means no
                               backend clock channel exists —
                               never a false lock. */
                            text: {
                                if(!root.engineUp)
                                    return qsTr("sync: —");
                                const s = bridge.playbackView.sync || "unknown";
                                if(s === "unknown")
                                    return qsTr("sync: unavailable — no backend clock channel (never a false lock)");
                                return qsTr("sync: %1").arg(s);
                            }
                            color: !root.engineUp
                                   || (bridge.playbackView.sync || "unknown") === "unknown"
                                   ? Theme.textSecondary
                                   : bridge.playbackView.sync === "healthy"
                                     ? Theme.ok : Theme.warn
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                            width: parent.width
                        }
                        Text {
                            visible: !!bridge.playbackView.unavailable_reason
                            width: parent.width
                            text: bridge.playbackView.unavailable_reason || ""
                            color: Theme.warn
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                        }
                        Text {
                            visible: !!bridge.playbackView.phase_detail
                            width: parent.width
                            text: bridge.playbackView.phase_detail || ""
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                Card {
                    title: qsTr("REACTIVE INPUTS")
                    Layout.fillWidth: true
                    Column {
                        spacing: Theme.spaceS
                        Text {
                            visible: !root.engineUp
                                     || !bridge.reactive.available
                            width: parent.width
                            text: root.engineUp
                                  ? (bridge.reactive.reason
                                     || qsTr("reactive inputs unavailable"))
                                  : qsTr("offline")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                        }
                        Text {
                            visible: !!bridge.reactive.capture_paused
                            text: qsTr("capture paused — displays off or suspended")
                            color: Theme.warn
                            font.pixelSize: Theme.fontSizeSmall
                        }
                        Repeater {
                            model: root.engineUp
                                   ? (bridge.reactive.inputs || []) : []
                            RowLayout {
                                width: parent.width
                                spacing: Theme.spaceM
                                StatusPill {
                                    text: modelData.id || "?"
                                    tone: !modelData.available ? "neutral"
                                        : modelData.running ? "ok"
                                        : modelData.enabled ? "warn"
                                        : "neutral"
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.status || "—"
                                    color: Theme.textSecondary
                                    font.pixelSize: Theme.fontSizeSmall
                                    elide: Text.ElideRight
                                }
                            }
                        }
                    }
                }
            }

            /*---- Ownership scan --------------------------------
               The engine's report verbatim — report-only by
               contract (plan section 4): conflicts are
               surfaced with guidance, never kill buttons. */
            Card {
                title: qsTr("OWNERSHIP SCAN")
                width: parent.width - 2 * col.padding
                Column {
                    width: parent.width
                    spacing: Theme.spaceS

                    StatusPill {
                        objectName: "ownershipSummary"
                        text: {
                            const o = root.ownership;
                            if(!o.reported)
                                return qsTr("no ownership report published");
                            if(!o.scanned)
                                return qsTr("startup scan not run — conflicts unknown");
                            if((o.conflictCount || 0) > 0)
                                return qsTr("%1 competing owner(s) detected").arg(o.conflictCount);
                            return qsTr("no competing RGB owners detected");
                        }
                        tone: !root.ownership.reported
                              || !root.ownership.scanned ? "neutral"
                              : root.ownership.conflictCount > 0 ? "bad" : "ok"
                    }

                    Repeater {
                        objectName: "ownershipConflicts"
                        model: root.ownership.conflicts || []
                        Column {
                            width: parent.width
                            spacing: Theme.spaceXS
                            RowLayout {
                                width: parent.width
                                spacing: Theme.spaceM
                                StatusPill { text: qsTr("conflict"); tone: "bad" }
                                Text {
                                    Layout.fillWidth: true
                                    text: qsTr("%1 (%2, pid %3)")
                                        .arg(modelData.owner || qsTr("unknown owner"))
                                        .arg(modelData.exe || "?")
                                        .arg(root.fmt(modelData.pid))
                                    color: Theme.textPrimary
                                    font.pixelSize: Theme.fontSizeSmall
                                }
                            }
                            Text {
                                width: parent.width
                                leftPadding: Theme.spaceL
                                text: modelData.detail || ""
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSizeSmall
                                wrapMode: Text.WordWrap
                            }
                        }
                    }
                    Text {
                        visible: (root.ownership.conflictCount || 0) > 0
                        width: parent.width
                        text: qsTr("Report-only — OpenRGB2 never kills or takes over a competing owner. Close the conflicting application (or move its RGB control off this hardware), then restart the engine.")
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSizeSmall
                        wrapMode: Text.WordWrap
                    }

                    Repeater {
                        model: root.ownership.notices || []
                        Text {
                            width: parent.width
                            text: qsTr("notice: %1 — %2")
                                .arg(modelData.owner || "?")
                                .arg(modelData.detail || "")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                        }
                    }

                    /* Managed-config sole-writer findings. */
                    Repeater {
                        model: (root.ownership.config
                                && root.ownership.config.findings)
                               ? root.ownership.config.findings : []
                        Text {
                            width: parent.width
                            text: modelData
                            color: modelData.startsWith("WARNING")
                                   ? Theme.warn : Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }

            /*---- Event journal ----------------------------------*/
            Card {
                title: qsTr("EVENT LOG")
                width: parent.width - 2 * col.padding
                Column {
                    width: parent.width
                    spacing: Theme.spaceXS
                    Repeater {
                        objectName: "eventLogList"
                        model: root.logTail
                        Text {
                            width: parent.width
                            text: qsTr("%1  %2%3")
                                .arg(modelData.time || "")
                                .arg(modelData.kind || "")
                                .arg(modelData.detail
                                     ? " — " + modelData.detail : "")
                            color: modelData.kind === "error"
                                   ? Theme.warn : Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                            elide: Text.ElideRight
                        }
                    }
                    EmptyState {
                        visible: root.logTail.length === 0
                        anchors.horizontalCenter: parent.horizontalCenter
                        title: qsTr("No events")
                        detail: qsTr("Wire events and connection transitions journal here.")
                    }
                }
            }

            /*---- Diagnostic export ------------------------------*/
            Card {
                title: qsTr("DIAGNOSTIC EXPORT")
                width: parent.width - 2 * col.padding
                Column {
                    width: parent.width
                    spacing: Theme.spaceS
                    Text {
                        width: parent.width
                        text: qsTr("Bundle the engine's state, diagnostics, counters, and this journal to a JSON file for support reports. Unique identifiers (serial numbers, hardware locations, MAC addresses) are redacted.")
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSizeSmall
                        wrapMode: Text.WordWrap
                    }
                    Row {
                        spacing: Theme.spaceM
                        AppButton {
                            objectName: "exportButton"
                            text: qsTr("Export diagnostics…")
                            enabled: root.hasCtl && root.engineUp
                            onClicked: exportDialog.open()
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: root.hasCtl && !!diagCtl.lastExport
                            text: qsTr("written: %1").arg(root.hasCtl ? diagCtl.lastExport : "")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                            elide: Text.ElideMiddle
                            width: 420
                        }
                    }
                    Text {
                        visible: root.hasCtl && !!diagCtl.exportError
                        width: parent.width
                        text: root.hasCtl ? diagCtl.exportError : ""
                        color: Theme.warn
                        font.pixelSize: Theme.fontSizeSmall
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
    }

    FileDialog {
        id: exportDialog
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("JSON (*.json)")]
        defaultSuffix: "json"
        /* selectedFile is a url — the controller's path is a
           plain local path, so it goes through file:///. */
        selectedFile: root.hasCtl ? "file:///" + diagCtl.defaultExportPath() : ""
        onAccepted:
            if(root.hasCtl) { diagCtl.exportDiagnostics(selectedFile); }
    }
}
