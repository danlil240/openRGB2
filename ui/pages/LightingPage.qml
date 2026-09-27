// SPDX-License-Identifier: GPL-2.0-or-later
//
// Lighting — plan section 5, the main flow end to end:
//   choose scene -> select devices -> adjust color/speed -> save.
//
// Everything below is real wire traffic: edits accumulate in the
// SceneEditController's working document (sceneEdit), Save sends
// SET_SCENE through EngineBridge, and playback mode/params go
// through SET_PLAYBACK_MODE with only the fields the selected
// uploaded preset supports (protocol §5). Nothing mutates the
// engine until Save — pending edits are labelled, never lost.
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import OpenRGB2.UI.theme
import OpenRGB2.UI.components

Item {
    id: root
    objectName: "lightingPage"

    readonly property bool engineUp:
        typeof bridge !== "undefined" && bridge.connected
    readonly property bool editorUp:
        typeof sceneEdit !== "undefined"

    /* Derived playback view (EngineBridge::playbackView) —
       requested_mode is the recorded selection, applied_mode
       is what the backend acknowledged, and phase is the
       honest headline: reconnecting / applying / failed /
       uploading / active / live / unknown. */
    readonly property var pv:
        engineUp ? bridge.playbackView : ({})
    readonly property string requestedMode: pv.requested_mode || ""
    readonly property string appliedMode:   pv.applied_mode   || ""
    readonly property bool animationRequested:
        requestedMode === "animation_playback"
    readonly property bool animationApplied:
        appliedMode === "animation_playback"
    /* Supported-control filtering (plan §3.4): while the
       uploaded loop owns output, only the preset's own
       controls apply — palette/layer/scene-multiplier
       controls are live-effects only this release. */
    readonly property bool liveControlsEnabled:
        editorUp && !animationRequested

    /*---- tracked uploaded-preset params (UI-side; the wire
           reports revisions, not values — so the last-sent
           values live in the controller and mirror here) --*/
    readonly property int    upSpeed:   root.editorUp ? sceneEdit.paramSpeed   : 100
    readonly property bool   upReverse: root.editorUp ? sceneEdit.paramReverse : false
    readonly property string upColor:   root.editorUp ? sceneEdit.paramColor   : "red"

    /* Preview event rate — measured here from frame_status
       pushes, labelled separately from output rates (§5). */
    property int frameEvents: 0
    property double previewFps: 0

    /* nameField sync sentinel — the last model name the
       field was shown (see the field's comment). */
    property string syncedSceneName: ""

    /* uiPrefs presence guard — tests may omit the store. */
    readonly property bool prefsUp:
        typeof uiPrefs !== "undefined" && uiPrefs !== null

    Component.onCompleted:
        if(root.prefsUp) {
            palH.value = uiPrefs.paletteHue;
            palS.value = uiPrefs.paletteSat;
            palV.value = uiPrefs.paletteVal;
        }

    Connections {
        target: typeof bridge !== "undefined" ? bridge : null
        function onFrameStatusChanged() { root.frameEvents++ }
    }
    Timer {
        interval: 2000; repeat: true; running: root.engineUp
        onTriggered: {
            root.previewFps = root.frameEvents / 2.0;
            root.frameEvents = 0;
        }
    }

    /* Commit-on-save (calibration-report regression, Task
       4.5): a text field still holding focus owns UNCOMMITTED
       text — its editingFinished hasn't fired, so saving the
       working copy would silently store the stale value.
       Commit every edit-owning field first; a refused commit
       aborts the save and surfaces the refusal rather than
       writing stale data. */
    function commitPendingEdits() {
        if(!root.editorUp)
            return;
        if(!sceneEdit.setSceneName(nameField.text))
            return;
        sceneEdit.save();
    }

    /* Per-input capability gate (Task 4.6): a reactive-input
       toggle is only operable when the WORKING scene consumes
       that input — the preset's declared `needs`, else the
       inline layer stack's reactive primitives (ripple with an
       empty source consumes every event, so it needs either
       event input; level needs audio; screenfield needs
       screen). Never a guess: no scene -> nothing operable. */
    function sceneNeeds(inputId) {
        if(!root.editorUp || !sceneEdit.hasWorking)
            return false;
        const needs = sceneEdit.presetSupport.needs || "";
        if(needs !== "")
            return needs === inputId;
        const doc = sceneEdit.workingDocument || {};
        const eff = doc.effect || {};
        const layers = eff.layers || [];
        for(let i = 0; i < layers.length; ++i) {
            const p   = layers[i].primitive || "";
            const src = layers[i].source    || "";
            if(p === "screenfield" && inputId === "screen")
                return true;
            if(p === "level" && inputId === "audio")
                return true;
            if(p === "ripple" && (src === "" || src === inputId)
               && (inputId === "audio" || inputId === "key"))
                return true;
        }
        return false;
    }

    function deviceNameFor(key) {
        if(!key || !root.engineUp)
            return "controller";
        for(let i = 0; i < bridge.snapshots.length; ++i) {
            const k = bridge.snapshots[i].key;
            if(k && k.device === key.device && k.serial === key.serial
                   && k.location === key.location)
                return bridge.snapshots[i].name || key.device;
        }
        return key.device || "controller";
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Flickable {
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentHeight: col.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: col
                width: root.width
                padding: Theme.spaceXL
                spacing: Theme.spaceL

                PageHeader {
                    title: qsTr("Lighting")
                    subtitle: qsTr("Choose a scene, pick devices, tune, then save")
                }

                /*---- Preset cards ------------------------------
                   Uploaded presets first (they can run on the
                   fans themselves), then the registry looks.
                   A `needs` badge is honest about reactive
                   inputs that aren't ported yet (4.6).    */
                Text {
                    text: qsTr("PRESETS")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSizeSmall
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.1
                }

                Flow {
                    objectName: "presetFlow"
                    width: parent.width - 2 * col.padding
                    spacing: Theme.spaceL

                    Repeater {
                        model: root.editorUp ? sceneEdit.playbackPresets : []
                        delegate: presetCard
                    }
                    Repeater {
                        model: root.engineUp ? bridge.presets : []
                        delegate: presetCard
                    }
                }

                Component {
                    id: presetCard
                    Rectangle {
                        width: 260
                        implicitHeight: cardCol.implicitHeight + 2 * Theme.spaceL
                        radius: Theme.radiusM
                        color: Theme.surface
                        border.width: selected ? 2 : (activeFocus ? 2 : 1)
                        border.color: selected ? Theme.accent
                                    : activeFocus ? Theme.accent
                                    : Theme.border

                        readonly property string pid:
                            modelData.id || ""
                        readonly property bool selected:
                            root.editorUp && sceneEdit.presetId === pid
                        readonly property bool isPlayback:
                            modelData.direction !== undefined
                        readonly property string needs:
                            modelData.needs || ""

                        activeFocusOnTab: true
                        Accessible.name: qsTr("Preset %1")
                            .arg(modelData.name || pid || "")
                        Accessible.role: Accessible.Button
                        /* !selected like the MouseArea: re-picking
                           the already-selected preset is a no-op
                           that must not disarm the pending
                           Animation-playback default (setPreset
                           sees picked == prev and clears it). */
                        Keys.onPressed: e => {
                            /* Space/Return/Enter like the mode
                               cards — Key_Enter is the numpad. */
                            if(e.key === Qt.Key_Space
                               || e.key === Qt.Key_Return
                               || e.key === Qt.Key_Enter) {
                                if(root.editorUp && root.engineUp && !selected)
                                    sceneEdit.setPreset(pid);
                                e.accepted = true;
                            }
                        }

                        Column {
                            id: cardCol
                            anchors { fill: parent; margins: Theme.spaceL }
                            spacing: Theme.spaceXS

                            Text {
                                text: modelData.name || pid || qsTr("preset")
                                color: Theme.textPrimary
                                font.pixelSize: Theme.fontSizeBody
                                font.weight: Font.DemiBold
                            }
                            Text {
                                visible: modelData.description !== undefined
                                         && modelData.description !== ""
                                text: modelData.description || ""
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSizeSmall
                                wrapMode: Text.WordWrap
                                width: parent.width
                            }
                            Row {
                                spacing: Theme.spaceS
                                StatusPill {
                                    visible: text !== ""
                                    text: selected ? qsTr("selected")
                                        : isPlayback ? qsTr("uploaded playback")
                                        : needs === "audio" ? qsTr("needs audio input")
                                        : needs === "screen" ? qsTr("needs screen input")
                                        : needs === "key" ? qsTr("needs key input")
                                        : ""
                                    tone: selected ? "accent"
                                        : isPlayback ? "ok" : "warn"
                                }
                            }
                            Text {
                                visible: needs !== "" && !isPlayback
                                text: qsTr("reactive input required — enable it in REACTIVE INPUTS below")
                                color: Theme.textDisabled
                                font.pixelSize: Theme.fontSizeSmall
                                wrapMode: Text.WordWrap
                                width: parent.width
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            enabled: root.editorUp && root.engineUp && !selected
                            cursorShape: enabled ? Qt.PointingHandCursor
                                                 : Qt.ArrowCursor
                            onClicked: sceneEdit.setPreset(pid)
                        }
                    }
                }

                EmptyState {
                    objectName: "presetsEmpty"
                    visible: !root.engineUp
                    title: qsTr("Engine not connected")
                    detail: qsTr("Preset cards and scene data arrive with the connection.")
                }

                /*---- Playback mode selector --------------------
                   Spec wording verbatim, beside the selected
                   effect. Capability-gated — the border marks
                   the REQUESTED selection and the pill marks
                   what the device actually runs, so a pending
                   transition never reports applied before the
                   engine confirms it.                      */
                RowLayout {
                    objectName: "modeSelector"
                    width: parent.width - 2 * col.padding
                    spacing: Theme.spaceL

                    Repeater {
                        model: [
                            {
                                key: "animation_playback",
                                title: qsTr("Animation playback"),
                                blurb: qsTr("Smooth repeating effects played by the fans")
                            },
                            {
                                key: "live_effects",
                                title: qsTr("Live effects"),
                                blurb: qsTr("Responds to music, screen, and live changes")
                            }
                        ]

                        Card {
                            Layout.fillWidth: true
                            readonly property string modeKey: modelData.key
                            readonly property bool requested:
                                root.requestedMode === modeKey
                            readonly property bool applied:
                                root.appliedMode === modeKey
                            readonly property bool usable:
                                root.engineUp
                                && (modeKey === "animation_playback"
                                    ? root.pv.animation_available !== false
                                    : root.pv.live_available !== false)
                            /* Keyboard parity with the MouseArea:
                               Tab reaches usable cards, Space/Enter
                               requests the mode, the focus ring is
                               visible. */
                            activeFocusOnTab: usable
                            Accessible.name: qsTr("Playback mode: %1")
                                .arg(modelData.title)
                            Accessible.role: Accessible.Button
                            Keys.onPressed: e => {
                                if(e.key === Qt.Key_Space
                                   || e.key === Qt.Key_Return
                                   || e.key === Qt.Key_Enter) {
                                    if(usable && root.editorUp
                                       && !requested)
                                        sceneEdit.requestMode(modeKey);
                                    e.accepted = true;
                                }
                            }
                            border.color: requested ? Theme.accent
                                          : activeFocus ? Theme.accent
                                          : Theme.border
                            border.width: requested || activeFocus ? 2 : 1

                            Column {
                                spacing: Theme.spaceXS
                                Text {
                                    text: modelData.title
                                    color: usable ? Theme.textPrimary
                                                  : Theme.textDisabled
                                    font.pixelSize: Theme.fontSizeBody
                                    font.weight: Font.DemiBold
                                }
                                Text {
                                    text: modelData.blurb
                                    color: Theme.textSecondary
                                    font.pixelSize: Theme.fontSizeSmall
                                    wrapMode: Text.WordWrap
                                    width: parent.width
                                }
                                Text {
                                    visible: root.engineUp && !usable
                                    text: root.pv.unavailable_reason
                                          || qsTr("not available for this scene")
                                    color: Theme.warn
                                    font.pixelSize: Theme.fontSizeSmall
                                    wrapMode: Text.WordWrap
                                    width: parent.width
                                }
                                /* The applied truth — "on device"
                                   only where the engine confirmed
                                   it; a requested-but-unconfirmed
                                   pick reads "applying…", never
                                   "selected". */
                                StatusPill {
                                    visible: requested || applied
                                    text: applied ? qsTr("on device")
                                                  : qsTr("applying…")
                                    tone: applied ? "ok" : "warn"
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                enabled: usable && root.editorUp
                                         && !requested
                                cursorShape: enabled ? Qt.PointingHandCursor
                                                     : Qt.ArrowCursor
                                onClicked: sceneEdit.requestMode(modeKey)
                            }
                        }
                    }
                }

                /* Transition honesty — a mode switch visibly
                   interrupts output while the engine confirms it. */
                Text {
                    width: parent.width - 2 * col.padding
                    wrapMode: Text.WordWrap
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.warn
                    visible: root.engineUp && text !== ""
                    text: {
                        if(!root.engineUp)
                            return "";
                        if(root.pv.phase === "failed")
                            return root.pv.phase_detail
                                   || qsTr("the last mode transition failed");
                        if(root.pv.phase === "reconnecting")
                            return qsTr("The engine's link to the backend is down — playback output may be stale while it reconnects.");
                        if(root.pv.phase === "applying" && root.animationRequested)
                            return qsTr("Switching to Animation playback — live output on the fan group is suppressed until the uploaded loop is confirmed on device.");
                        if(root.pv.phase === "applying")
                            return qsTr("Returning to Live effects — the uploaded loop is replaced with a freshly evaluated frame when the engine confirms.");
                        return "";
                    }
                }
                Text {
                    width: parent.width - 2 * col.padding
                    wrapMode: Text.WordWrap
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.textSecondary
                    visible: root.engineUp && root.animationApplied
                             && root.pv.phase !== "applying"
                    text: qsTr("The uploaded loop owns the fan group — palette, layer, and scene-multiplier controls below are disabled while it runs. Other devices play the matching effect.")
                }

                /* Upload / transport state — Uploading, Active,
                   Reconnecting, or Failed (plan section 5) plus
                   the honest requested-vs-applied split. */
                Card {
                    title: qsTr("PLAYBACK STATE")
                    objectName: "playbackStateCard"
                    width: parent.width - 2 * col.padding
                    visible: root.engineUp

                    Column {
                        width: parent.width
                        spacing: Theme.spaceM

                        RowLayout {
                            width: parent.width
                            spacing: Theme.spaceL

                            StatusPill {
                                objectName: "playbackPhasePill"
                                text: {
                                    switch(root.pv.phase) {
                                    case "reconnecting": return qsTr("Reconnecting");
                                    case "applying":     return qsTr("Applying…");
                                    case "failed":       return qsTr("Failed");
                                    case "uploading":    return qsTr("Uploading");
                                    case "active":       return qsTr("Active");
                                    case "live":         return qsTr("Live");
                                    default:             return qsTr("unknown");
                                    }
                                }
                                tone: {
                                    switch(root.pv.phase) {
                                    case "reconnecting": return "warn";
                                    case "applying":     return "warn";
                                    case "failed":       return "bad";
                                    case "uploading":    return "accent";
                                    case "active":       return "ok";
                                    case "live":         return "ok";
                                    default:             return "neutral";
                                    }
                                }
                            }
                            Column {
                                Layout.fillWidth: true
                                spacing: Theme.spaceXS
                                Text {
                                    wrapMode: Text.WordWrap
                                    width: parent.width
                                    font.pixelSize: Theme.fontSizeSmall
                                    color: Theme.textSecondary
                                    text: {
                                        const label = m =>
                                            m === "animation_playback"
                                                ? qsTr("Animation playback")
                                                : qsTr("Live effects");
                                        if(root.requestedMode && root.appliedMode
                                           && root.requestedMode !== root.appliedMode)
                                            return qsTr("requested: %1 · on device: %2 — waiting for the engine to confirm")
                                                .arg(label(root.requestedMode))
                                                .arg(label(root.appliedMode));
                                        if(!root.appliedMode)
                                            return qsTr("on device: unknown — no playback status reported yet");
                                        const app = label(root.appliedMode);
                                        if(!root.animationApplied)
                                            return qsTr("on device: %1").arg(app);
                                        const c = root.pv.control;
                                        if(c === "paused")
                                            return qsTr("on device: %1 · paused — a sampled frame is held").arg(app);
                                        if(c === "stopped")
                                            return qsTr("on device: %1 · stopped — output replaced with black").arg(app);
                                        return qsTr("on device: %1").arg(app);
                                    }
                                }
                                Text {
                                    visible: !!root.pv.phase_detail
                                    text: root.pv.phase_detail || ""
                                    color: Theme.bad
                                    font.pixelSize: Theme.fontSizeSmall
                                    wrapMode: Text.WordWrap
                                    width: parent.width
                                }
                            }
                            Repeater {
                                model: [
                                    { key: "playing", label: qsTr("Play") },
                                    { key: "paused",  label: qsTr("Pause") },
                                    { key: "stopped", label: qsTr("Stop") }
                                ]
                                AppButton {
                                    /* Transport control only exists
                                       for an uploaded loop — the
                                       engine forces Playing in live
                                       mode, so offering pause there
                                       would fake a dead button. */
                                    visible: root.animationRequested
                                             || root.animationApplied
                                    text: modelData.label
                                    enabled: root.editorUp && root.engineUp
                                             && root.animationApplied
                                             && root.pv.phase !== "applying"
                                             && root.pv.control !== modelData.key
                                    onClicked: sceneEdit.setPlaybackControl(modelData.key)
                                }
                            }
                        }

                        Text {
                            visible: root.animationRequested || root.animationApplied
                            text: qsTr("Pause holds a sampled frame · Stop replaces playback with black · closing the UI does not interrupt the loop.")
                            color: Theme.textDisabled
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                            width: parent.width
                        }

                        /* Wire cadence + sync — exactly what
                           PLAYBACK reports, labelled for what it
                           is: configured geometry, a validated-
                           clock sync state, a separately recorded
                           physical measurement, and (in live
                           mode) the transport-accepted interval.
                           Never a presentation-FPS claim. */
                        Column {
                            width: parent.width
                            spacing: Theme.spaceXS
                            visible: root.animationApplied
                                     || root.appliedMode === "live_effects"

                            Text {
                                visible: root.animationApplied && root.pv.frame_count > 0
                                text: qsTr("configured loop: %1 frames · %2 ms/frame · ≈%3 s cycle — configured rate, not a measured display rate")
                                    .arg(root.pv.frame_count)
                                    .arg((root.pv.frame_interval_s * 1000).toFixed(3))
                                    .arg(root.pv.cycle_s.toFixed(2))
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSizeSmall
                                wrapMode: Text.WordWrap
                                width: parent.width
                            }
                            Text {
                                visible: root.animationApplied
                                text: {
                                    const s = root.pv.sync || "unknown";
                                    if(s === "unknown")
                                        return qsTr("synchronization: unavailable — no validated clock source");
                                    const u = root.pv.clock_uncertainty_s || 0;
                                    return u > 0
                                        ? qsTr("synchronization: %1 · clock uncertainty ±%2 ms")
                                            .arg(s).arg((u * 1000).toFixed(1))
                                        : qsTr("synchronization: %1").arg(s);
                                }
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSizeSmall
                                wrapMode: Text.WordWrap
                                width: parent.width
                            }
                            Text {
                                visible: root.animationApplied
                                text: root.pv.physical_measured
                                      ? qsTr("physical playback: %1 ms/frame · %2 s cycle · %3")
                                        .arg((root.pv.physical_frame_interval_s * 1000).toFixed(2))
                                        .arg((root.pv.physical_cycle_s || 0).toFixed(2))
                                        .arg(root.pv.physical_provenance)
                                      : qsTr("physical playback: not measured")
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSizeSmall
                                wrapMode: Text.WordWrap
                                width: parent.width
                            }
                            Text {
                                visible: root.appliedMode === "live_effects"
                                text: {
                                    const ag = root.engineUp
                                               ? bridge.frameStatus.aggregate
                                               : undefined;
                                    if(ag && ag.mean_interval_s > 0)
                                        return qsTr("live delivery ≈ %1 updates/s — transport-accepted interval (%2), not display FPS")
                                            .arg((1 / ag.mean_interval_s).toFixed(1))
                                            .arg(ag.provenance || "unknown");
                                    return qsTr("live delivery: no transport counters yet");
                                }
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSizeSmall
                                wrapMode: Text.WordWrap
                                width: parent.width
                            }
                        }
                    }
                }

                /*---- Device groups ------------------------------
                   Derived from the scene document's objects +
                   bindings joined with live snapshots — the
                   engine's real mapping data.             */
                Card {
                    title: qsTr("DEVICES")
                    width: parent.width - 2 * col.padding

                    Column {
                        width: parent.width
                        spacing: Theme.spaceM

                        Text {
                            text: qsTr("Palette colors apply to the selected groups. Leave all selected to light everything.")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                            width: parent.width
                        }

                        Flow {
                            objectName: "groupFlow"
                            width: parent.width
                            spacing: Theme.spaceS

                            GroupChip {
                                label: qsTr("All devices")
                                detail: qsTr("every bound zone")
                                state: "resolved"
                                verified: true
                                connected: true
                                selected: root.editorUp
                                          && sceneEdit.selectedGroups.length === 0
                                enabled: root.editorUp
                                onClicked: sceneEdit.selectAllGroups()
                            }

                            Repeater {
                                model: root.editorUp ? sceneEdit.deviceGroups : []
                                GroupChip {
                                    label: modelData.label
                                    detail: {
                                        let d = modelData.controller || "";
                                        if(modelData.zone)
                                            d += (d ? " · " : "") + modelData.zone;
                                        const leds = modelData.led_count || modelData.zone_leds
                                                     || modelData.emitters;
                                        if(leds > 0)
                                            d += (d ? " · " : "") + leds + qsTr(" LEDs");
                                        if(!modelData.verified)
                                            d += (d ? " · " : "") + qsTr("unverified");
                                        if(modelData.state === "absent")
                                            d += (d ? " · " : "") + qsTr("not connected");
                                        else if(modelData.state === "ambiguous")
                                            d += (d ? " · " : "") + qsTr("ambiguous");
                                        return d;
                                    }
                                    state: modelData.state || "resolved"
                                    verified: modelData.verified === true
                                    connected: modelData.connected === true
                                    selected: root.editorUp
                                              && sceneEdit.selectedGroups.indexOf(modelData.id) >= 0
                                    enabled: root.editorUp
                                    onClicked: sceneEdit.toggleGroup(modelData.id)
                                }
                            }
                        }

                        Text {
                            visible: root.editorUp && sceneEdit.hasWorking
                                     && !sceneEdit.groupsTargetable
                            text: qsTr("Named presets cover every device — group selection sets which devices take palette colors. Layer targeting needs an inline layer stack.")
                            color: Theme.textDisabled
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                            width: parent.width
                        }
                        Text {
                            visible: root.editorUp && sceneEdit.groupsTargetable
                            text: qsTr("This scene's inline layers are also scoped to the selection.")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                            width: parent.width
                        }

                        EmptyState {
                            objectName: "groupsEmpty"
                            anchors.horizontalCenter: parent.horizontalCenter
                            visible: !root.editorUp || sceneEdit.deviceGroups.length === 0
                            title: qsTr("No device groups")
                            detail: root.engineUp
                                    ? qsTr("The loaded scene binds no devices yet — the Layout view authors bindings.")
                                    : qsTr("Device groups arrive with the engine scene.")
                        }
                    }
                }

                /*---- Palette ------------------------------------
                   Per-group base colors (object_colors) + an
                   HSV picker + quick swatches. Writes are
                   pending edits — Save sends them.        */
                Card {
                    title: qsTr("PALETTE")
                    width: parent.width - 2 * col.padding

                    Column {
                        spacing: Theme.spaceM

                        /* Supported-control filtering (plan
                           §3.4): while Animation playback is
                           selected the uploaded loop owns
                           output — arbitrary palettes/layers
                           are live-effects only this release. */
                        Text {
                            visible: root.animationRequested
                            text: qsTr("Disabled while Animation playback is selected — the uploaded preset's own controls above are all it supports; palette colors apply to Live effects scenes.")
                            color: Theme.warn
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                            width: parent.width
                        }

                        /* Current group colors — real document
                           values, not placeholders. */
                        Flow {
                            width: parent.width
                            spacing: Theme.spaceM
                            Repeater {
                                model: {
                                    if(!root.editorUp)
                                        return [];
                                    const sel = sceneEdit.selectedGroups;
                                    const groups = sceneEdit.deviceGroups;
                                    const out = [];
                                    for(let i = 0; i < groups.length; ++i) {
                                        if(sel.length === 0
                                           || sel.indexOf(groups[i].id) >= 0)
                                            out.push(groups[i]);
                                    }
                                    return out;
                                }
                                ColorWell {
                                    swatch: {
                                        const doc = sceneEdit.workingDocument;
                                        const oc = doc && doc.object_colors
                                                   ? doc.object_colors : {};
                                        return oc[modelData.id] || "black";
                                    }
                                    label: modelData.label
                                }
                            }
                        }

                        /* HSV picker — computed color, no
                           literals; the hex is what SET_SCENE
                           carries. */
                        RowLayout {
                            width: parent.width
                            spacing: Theme.spaceL

                            Rectangle {
                                width: 44; height: 44; radius: Theme.radiusS
                                color: Qt.hsva(palH.value, palS.value, palV.value, 1)
                                border.color: Theme.borderStrong
                                border.width: 1
                            }
                            Column {
                                Layout.fillWidth: true
                                spacing: Theme.spaceS
                                /* Palette position is a UI-owned
                                   preference (Task 4.8): seeded
                                   once from uiPrefs, written back
                                   on each move. */
                                AppSlider {
                                    id: palH
                                    width: parent.width
                                    from: 0; to: 1; stepSize: 0.001
                                    value: 0.55
                                    enabled: root.liveControlsEnabled
                                    Accessible.name: qsTr("palette hue")
                                    onMoved: if(root.prefsUp)
                                        uiPrefs.paletteHue = value;
                                }
                                AppSlider {
                                    id: palS
                                    width: parent.width
                                    from: 0; to: 1; stepSize: 0.001
                                    value: 0.6
                                    enabled: root.liveControlsEnabled
                                    Accessible.name: qsTr("palette saturation")
                                    onMoved: if(root.prefsUp)
                                        uiPrefs.paletteSat = value;
                                }
                                AppSlider {
                                    id: palV
                                    width: parent.width
                                    from: 0; to: 1; stepSize: 0.001
                                    value: 1.0
                                    enabled: root.liveControlsEnabled
                                    Accessible.name: qsTr("palette brightness")
                                    onMoved: if(root.prefsUp)
                                        uiPrefs.paletteVal = value;
                                }
                            }
                            AppButton {
                                text: qsTr("Apply to selected")
                                primary: true
                                enabled: root.liveControlsEnabled
                                onClicked: sceneEdit.setGroupColor(
                                    Qt.hsva(palH.value, palS.value,
                                            palV.value, 1).toString())
                            }
                        }

                        /* Quick picks — design-token swatches. */
                        Row {
                            spacing: Theme.spaceS
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("Quick")
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSizeSmall
                            }
                            Repeater {
                                model: Theme.paletteSwatches
                                PaletteSwatch {
                                    swatch: modelData
                                    enabled: root.liveControlsEnabled
                                    onPicked: hex => sceneEdit.setGroupColor(hex)
                                }
                            }
                        }
                    }
                }

                /*---- Scene controls -----------------------------
                   Brightness round-trips through SET_SCENE
                   (document.brightness — the wire has no
                   dedicated brightness verb; this is the
                   scene-level path the document carries).
                   Speed/intensity multiply the resolved look. */
                Card {
                    title: qsTr("SCENE CONTROLS")
                    width: parent.width - 2 * col.padding

                    Column {
                        spacing: Theme.spaceM

                        Text {
                            visible: root.animationRequested
                            text: qsTr("Disabled while Animation playback is selected — these multipliers drive the live scene evaluation the uploaded loop replaces.")
                            color: Theme.warn
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                            width: parent.width
                        }

                        RowLayout {
                            width: parent.width
                            spacing: Theme.spaceL
                            Text {
                                text: qsTr("Brightness")
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSizeBody
                                Layout.preferredWidth: 110
                            }
                            AppSlider {
                                id: brightSlider
                                Accessible.name: qsTr("scene brightness")
                                Layout.fillWidth: true
                                enabled: root.liveControlsEnabled && sceneEdit.hasWorking
                                from: 0; to: 100; stepSize: 1
                                onMoved: if(root.editorUp)
                                    sceneEdit.setBrightness(value / 100)
                                Binding {
                                    target: brightSlider
                                    property: "value"
                                    value: root.editorUp
                                           ? sceneEdit.brightness * 100 : 0
                                }
                            }
                            Text {
                                text: Math.round(brightSlider.value) + "%"
                                color: Theme.textPrimary
                                font.pixelSize: Theme.fontSizeBody
                                Layout.preferredWidth: 44
                            }
                        }

                        RowLayout {
                            width: parent.width
                            spacing: Theme.spaceL
                            Text {
                                text: qsTr("Effect speed")
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSizeBody
                                Layout.preferredWidth: 110
                            }
                            AppSlider {
                                id: fxSpeedSlider
                                Accessible.name: qsTr("effect speed")
                                Layout.fillWidth: true
                                enabled: root.liveControlsEnabled && sceneEdit.hasWorking
                                from: 0.05; to: 4.0; stepSize: 0.05
                                onMoved: if(root.editorUp)
                                    sceneEdit.setEffectSpeed(value)
                                Binding {
                                    target: fxSpeedSlider
                                    property: "value"
                                    value: root.editorUp
                                           ? sceneEdit.effectSpeed : 1.0
                                }
                            }
                            Text {
                                text: fxSpeedSlider.value.toFixed(2) + "×"
                                color: Theme.textPrimary
                                font.pixelSize: Theme.fontSizeBody
                                Layout.preferredWidth: 44
                            }
                        }

                        RowLayout {
                            width: parent.width
                            spacing: Theme.spaceL
                            Text {
                                text: qsTr("Intensity")
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSizeBody
                                Layout.preferredWidth: 110
                            }
                            AppSlider {
                                id: fxIntSlider
                                Accessible.name: qsTr("effect intensity")
                                Layout.fillWidth: true
                                enabled: root.liveControlsEnabled && sceneEdit.hasWorking
                                from: 0; to: 1.0; stepSize: 0.01
                                onMoved: if(root.editorUp)
                                    sceneEdit.setEffectIntensity(value)
                                Binding {
                                    target: fxIntSlider
                                    property: "value"
                                    value: root.editorUp
                                           ? sceneEdit.effectIntensity : 1.0
                                }
                            }
                            Text {
                                text: Math.round(fxIntSlider.value * 100) + "%"
                                color: Theme.textPrimary
                                font.pixelSize: Theme.fontSizeBody
                                Layout.preferredWidth: 44
                            }
                        }
                    }
                }

                /*---- Uploaded preset parameters -----------------
                   Only fields the selected preset supports are
                   emitted (protocol §5): speed 10..200 always,
                   direction for Rainbow Wave, color for
                   Breathing — and only fields the user touched
                   this session, so untouched fields keep the
                   engine's recorded per-scene value instead of
                   being clobbered by UI defaults. Sent through
                   the controller so the current transport
                   control is preserved — a speed drag can never
                   silently resume a paused upload.           */
                Card {
                    title: qsTr("UPLOADED PRESET")
                    width: parent.width - 2 * col.padding
                    visible: root.editorUp && sceneEdit.presetSupport.playback === true

                    Column {
                        spacing: Theme.spaceM

                        Text {
                            visible: root.requestedMode !== "animation_playback"
                            text: qsTr("Switch to Animation playback to drive the uploaded preset.")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                            width: parent.width
                        }
                        Text {
                            visible: root.animationRequested
                                     && root.pv.phase === "applying"
                            text: qsTr("The mode transition is in flight — parameter edits resume when the engine confirms.")
                            color: Theme.textDisabled
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                            width: parent.width
                        }

                        RowLayout {
                            width: parent.width
                            spacing: Theme.spaceL
                            enabled: root.engineUp
                                     && root.animationRequested
                                     && root.pv.phase !== "applying"
                            Text {
                                text: qsTr("Speed")
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSizeBody
                                Layout.preferredWidth: 110
                            }
                            AppSlider {
                                id: upSpeedSlider
                                objectName: "upSpeedSlider"
                                Accessible.name: qsTr("uploaded preset speed")
                                Layout.fillWidth: true
                                enabled: root.editorUp && parent.enabled
                                         && sceneEdit.presetSupport.speed === true
                                from: 10; to: 200; stepSize: 1
                                onMoved: {
                                    if(root.editorUp)
                                        sceneEdit.noteParamTouched("speed");
                                    paramTimer.restart();
                                }
                                Binding {
                                    target: upSpeedSlider
                                    property: "value"
                                    value: root.upSpeed
                                }
                            }
                            Text {
                                text: Math.round(upSpeedSlider.value)
                                color: Theme.textPrimary
                                font.pixelSize: Theme.fontSizeBody
                                Layout.preferredWidth: 44
                            }
                        }

                        /* Debounce: one SET_PLAYBACK_MODE per
                           gesture, never a flood in flight. */
                        Timer {
                            id: paramTimer
                            interval: 150
                            onTriggered: if(root.editorUp && root.engineUp)
                                sceneEdit.sendPlaybackParams(
                                    Math.round(upSpeedSlider.value),
                                    dirCheck.checked,
                                    upColorWell.swatch.toString())
                        }

                        RowLayout {
                            width: parent.width
                            spacing: Theme.spaceL
                            Text {
                                text: qsTr("Direction")
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSizeBody
                                Layout.preferredWidth: 110
                            }
                            CheckBox {
                                id: dirCheck
                                enabled: root.editorUp && root.engineUp
                                         && root.animationRequested
                                         && root.pv.phase !== "applying"
                                         && sceneEdit.presetSupport.direction === true
                                text: qsTr("reverse")
                                onToggled: if(enabled) {
                                    if(root.editorUp)
                                        sceneEdit.noteParamTouched("reverse");
                                    paramTimer.restart();
                                }
                                Binding {
                                    target: dirCheck
                                    property: "checked"
                                    value: root.upReverse
                                }
                            }
                            Text {
                                visible: root.editorUp
                                         && sceneEdit.presetSupport.playback === true
                                         && sceneEdit.presetSupport.direction !== true
                                text: qsTr("not supported by this preset")
                                color: Theme.textDisabled
                                font.pixelSize: Theme.fontSizeSmall
                            }
                        }

                        RowLayout {
                            width: parent.width
                            spacing: Theme.spaceL
                            Text {
                                text: qsTr("Color")
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSizeBody
                                Layout.preferredWidth: 110
                            }
                            ColorWell {
                                id: upColorWell
                                swatch: Qt.hsva(upH.value, upS.value, upV.value, 1)
                            }
                            Column {
                                spacing: Theme.spaceS
                                Layout.fillWidth: true
                                AppSlider {
                                    id: upH
                                    Accessible.name: qsTr("uploaded preset color hue")
                                    width: parent.width
                                    from: 0; to: 1; stepSize: 0.001; value: 0
                                    enabled: root.editorUp && root.engineUp
                                             && root.animationRequested
                                             && root.pv.phase !== "applying"
                                             && sceneEdit.presetSupport.color === true
                                    onMoved: {
                                        if(root.editorUp)
                                            sceneEdit.noteParamTouched("color");
                                        paramTimer.restart();
                                    }
                                }
                                AppSlider {
                                    id: upS
                                    Accessible.name: qsTr("uploaded preset color saturation")
                                    width: parent.width
                                    from: 0; to: 1; stepSize: 0.001; value: 1
                                    enabled: upH.enabled
                                    onMoved: {
                                        if(root.editorUp)
                                            sceneEdit.noteParamTouched("color");
                                        paramTimer.restart();
                                    }
                                }
                                AppSlider {
                                    id: upV
                                    Accessible.name: qsTr("uploaded preset color brightness")
                                    width: parent.width
                                    from: 0; to: 1; stepSize: 0.001; value: 1
                                    enabled: upH.enabled
                                    onMoved: {
                                        if(root.editorUp)
                                            sceneEdit.noteParamTouched("color");
                                        paramTimer.restart();
                                    }
                                }
                            }
                            Text {
                                visible: root.editorUp
                                         && sceneEdit.presetSupport.playback === true
                                         && sceneEdit.presetSupport.color !== true
                                text: qsTr("not supported by this preset")
                                color: Theme.textDisabled
                                font.pixelSize: Theme.fontSizeSmall
                            }
                        }
                    }
                }

                /*---- Reactive inputs (Task 4.6) -----------------
                   Enable/disable + honest status for each engine
                   reactive input. Every toggle is real wire
                   traffic: REACTIVE_COMMAND round-trips the
                   engine and `checked` re-lands on the pushed
                   "reactive" event — never on the click. A
                   toggle is operable only when the engine
                   reports the input available AND the working
                   scene consumes it (sceneNeeds); a running
                   input stays operable so capture can always
                   be stopped. */
                Card {
                    id: reactiveCard
                    title: qsTr("REACTIVE INPUTS")
                    objectName: "reactiveCard"
                    width: parent.width - 2 * col.padding

                    readonly property var reactiveState:
                        root.engineUp ? (bridge.reactive || {}) : ({})
                    readonly property bool reactiveAvailable:
                        reactiveState.available === true
                    readonly property var reactiveInputs:
                        reactiveState.inputs || []
                    readonly property var reactiveDisplays:
                        reactiveState.displays || []
                    readonly property bool capturePaused:
                        reactiveState.capture_paused === true

                    function inputState(id) {
                        for(let i = 0;
                            i < reactiveCard.reactiveInputs.length;
                            ++i) {
                            const it = reactiveCard.reactiveInputs[i];
                            if(it.id === id)
                                return it;
                        }
                        return null;
                    }

                    Column {
                        spacing: Theme.spaceM

                        /* Engine surface absent — the honest
                           reason, never a fabricated set of
                           toggles. */
                        Text {
                            objectName: "reactiveUnavailableNote"
                            visible: root.engineUp
                                     && !reactiveCard.reactiveAvailable
                            text: reactiveCard.reactiveState.reason
                                  || qsTr("reactive inputs are not wired on this engine")
                            color: Theme.warn
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                            width: parent.width
                        }

                        /* capture_paused — the power gate the
                           engine applies while displays are
                           off or the system is suspended. */
                        Text {
                            objectName: "reactivePausedNote"
                            visible: reactiveCard.capturePaused
                            text: qsTr("Capture paused — displays are off or the system is suspended; nothing is captured.")
                            color: Theme.warn
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                            width: parent.width
                        }

                        Repeater {
                            objectName: "reactiveInputList"
                            model: reactiveCard.reactiveAvailable
                                   ? [ { "inputId": "audio",
                                         "label": qsTr("Audio reactive") },
                                       { "inputId": "screen",
                                         "label": qsTr("Screen mirror") },
                                       { "inputId": "key",
                                         "label": qsTr("Keyboard reactive") } ]
                                   : []

                            delegate: Column {
                                id: inputRow
                                required property var modelData
                                readonly property string inputId:
                                    modelData.inputId
                                readonly property var st:
                                    reactiveCard.inputState(inputId)
                                readonly property bool present:
                                    st !== null
                                readonly property bool avail:
                                    present && st.available === true
                                readonly property bool on:
                                    present && st.enabled === true
                                    && st.running === true
                                /* Operable only when the scene
                                   consumes this input — or the
                                   input is running (it must
                                   always be stoppable). */
                                readonly property bool usable:
                                    root.engineUp && avail
                                    && (on || root.sceneNeeds(inputId))
                                width: parent.width
                                spacing: Theme.spaceXS

                                RowLayout {
                                    width: parent.width
                                    spacing: Theme.spaceL
                                    CheckBox {
                                        id: inputCheck
                                        objectName: inputId + "Check"
                                        text: modelData.label
                                        enabled: inputRow.usable
                                        onClicked:
                                            bridge.reactiveCommand(
                                                "set_enabled",
                                                { "input": inputId,
                                                  "enabled": inputCheck.checked })
                                        Binding {
                                            target: inputCheck
                                            property: "checked"
                                            value: inputRow.on
                                        }
                                    }
                                    StatusPill {
                                        objectName: inputId + "Pill"
                                        visible: inputRow.present
                                        text: !inputRow.avail
                                              ? qsTr("unavailable")
                                              : inputRow.on
                                              ? (reactiveCard.capturePaused
                                                 ? qsTr("paused") : qsTr("on"))
                                              : qsTr("off")
                                        tone: !inputRow.avail ? "warn"
                                              : inputRow.on
                                              ? (reactiveCard.capturePaused
                                                 ? "warn" : "ok")
                                              : "neutral"
                                    }
                                    Text {
                                        objectName: inputId + "Status"
                                        Layout.fillWidth: true
                                        text: {
                                            if(!inputRow.present)
                                                return qsTr("not reported by the engine");
                                            if(!inputRow.avail)
                                                return inputRow.st.status
                                                    || qsTr("not available on this system");
                                            if(!root.sceneNeeds(inputId)
                                               && !inputRow.on)
                                                return qsTr("the current scene doesn't use this input");
                                            return inputRow.st.status || "";
                                        }
                                        color: Theme.textDisabled
                                        font.pixelSize: Theme.fontSizeSmall
                                        wrapMode: Text.WordWrap
                                    }
                                }

                                /* Audio: sensitivity (the
                                   engine's 0.25..2.0 domain,
                                   shown as percent; debounced
                                   like playback params). */
                                RowLayout {
                                    visible: inputRow.inputId === "audio"
                                             && inputRow.avail
                                    width: parent.width
                                    spacing: Theme.spaceL
                                    Text {
                                        text: qsTr("Sensitivity")
                                        color: Theme.textSecondary
                                        font.pixelSize: Theme.fontSizeBody
                                        Layout.preferredWidth: 110
                                    }
                                    AppSlider {
                                        id: sensSlider
                                        objectName: "audioSensitivity"
                                        Accessible.name: qsTr("audio sensitivity")
                                        Layout.fillWidth: true
                                        from: 25; to: 200; stepSize: 1
                                        enabled: inputRow.usable
                                        onMoved: sensTimer.restart()
                                        Binding {
                                            target: sensSlider
                                            property: "value"
                                            value: inputRow.present
                                                   ? Math.round(
                                                       inputRow.st.sensitivity
                                                       * 100)
                                                   : 100
                                        }
                                    }
                                    Text {
                                        text: Math.round(sensSlider.value) + "%"
                                        color: Theme.textSecondary
                                        font.pixelSize: Theme.fontSizeBody
                                    }
                                    Timer {
                                        id: sensTimer
                                        interval: 150
                                        onTriggered:
                                            bridge.reactiveCommand(
                                                "set_params",
                                                { "input": "audio",
                                                  "sensitivity":
                                                      sensSlider.value / 100.0 })
                                    }
                                }

                                /* Screen: which display the
                                   sampler mirrors. */
                                RowLayout {
                                    visible: inputRow.inputId === "screen"
                                             && inputRow.avail
                                    width: parent.width
                                    spacing: Theme.spaceL
                                    Text {
                                        text: qsTr("Display")
                                        color: Theme.textSecondary
                                        font.pixelSize: Theme.fontSizeBody
                                        Layout.preferredWidth: 110
                                    }
                                    ComboBox {
                                        id: dispCombo
                                        objectName: "screenDisplay"
                                        Accessible.name: qsTr("screen input display")
                                        Layout.fillWidth: true
                                        enabled: inputRow.usable && count > 0
                                        model: reactiveCard.reactiveDisplays
                                        textRole: "name"
                                        onActivated: index =>
                                            bridge.reactiveCommand(
                                                "set_params",
                                                { "input": "screen",
                                                  "screen_index": index })
                                        Binding {
                                            target: dispCombo
                                            property: "currentIndex"
                                            value: inputRow.present
                                                   ? inputRow.st.screen_index
                                                   : 0
                                        }
                                    }
                                    Text {
                                        visible: dispCombo.count === 0
                                        text: qsTr("no displays reported")
                                        color: Theme.textDisabled
                                        font.pixelSize: Theme.fontSizeSmall
                                    }
                                }
                            }
                        }

                        Text {
                            objectName: "reactiveLiveNote"
                            visible: root.engineUp
                                     && reactiveCard.reactiveAvailable
                            text: root.animationRequested
                                  ? qsTr("Animation playback is selected — reactive capture has no visible effect while an uploaded loop owns output. Reactive scenes stay Live effects.")
                                  : qsTr("Reactive scenes stay Live effects — a scene using these inputs can't be uploaded as animation playback.")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                            width: parent.width
                        }

                        /* Bounded, local capture — the privacy
                           surface the plan requires next to the
                           controls. */
                        Text {
                            visible: root.engineUp
                                     && reactiveCard.reactiveAvailable
                            text: qsTr("Capture stays inside the engine process — audio energies (~5 ms chunks), a 12×6 screen grid (≤ 25 Hz), and key edges only. No audio samples, pixels, or typed text are recorded.")
                            color: Theme.textDisabled
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                            width: parent.width
                        }

                        EmptyState {
                            objectName: "reactiveEmpty"
                            anchors.horizontalCenter: parent.horizontalCenter
                            visible: !root.engineUp
                            title: qsTr("Engine not connected")
                            detail: qsTr("Reactive input controls report what the engine exposes.")
                        }
                    }
                }

                /*---- Preview ------------------------------------
                   Same colors as output: frame_status carries
                   the last transport-accepted frame per
                   controller (protocol §7). Rate labelled
                   separately from measured output rates. */
                Card {
                    title: qsTr("PREVIEW")
                    width: parent.width - 2 * col.padding

                    Column {
                        spacing: Theme.spaceM

                        Repeater {
                            model: root.engineUp
                                   ? (bridge.frameStatus.controllers || [])
                                   : []
                            Column {
                                spacing: Theme.spaceXS
                                Text {
                                    text: root.deviceNameFor(modelData.key)
                                    color: Theme.textSecondary
                                    font.pixelSize: Theme.fontSizeSmall
                                }
                                FrameStrip {
                                    objectName: "previewStrip"
                                    colors: modelData.colors || []
                                }
                            }
                        }

                        Text {
                            visible: root.engineUp
                            text: qsTr("preview ≈ %1 updates/s — engine frame_status event cadence (≤ 1 Hz), not the output rate")
                                .arg(root.previewFps.toFixed(1))
                            color: Theme.textDisabled
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                            width: parent.width
                        }

                        EmptyState {
                            objectName: "previewEmpty"
                            anchors.horizontalCenter: parent.horizontalCenter
                            visible: !root.engineUp
                                     || !bridge.frameStatus.controllers
                                     || bridge.frameStatus.controllers.length === 0
                            title: qsTr("No frame data")
                            detail: root.engineUp
                                    ? qsTr("The preview fills in when the engine delivers frames.")
                                    : qsTr("Engine not connected — nothing is being driven.")
                        }
                    }
                }

                /*---- Profiles -----------------------------------
                   Save/load named scenes. The engine has no
                   profile-list verb — the library lives in
                   the UI; loading stages the document and
                   Save applies it through SET_SCENE (the
                   engine validates/migrates). The engine's
                   own profile remembers the playback mode
                   per scene name automatically.           */
                Card {
                    title: qsTr("PROFILES")
                    objectName: "profilesCard"
                    width: parent.width - 2 * col.padding

                    Column {
                        spacing: Theme.spaceM

                        RowLayout {
                            width: parent.width
                            spacing: Theme.spaceL
                            Text {
                                text: qsTr("Scene name")
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSizeBody
                                Layout.preferredWidth: 110
                            }
                            TextField {
                                id: nameField
                                objectName: "nameField"
                                Accessible.name: qsTr("Scene name")
                                Layout.fillWidth: true
                                enabled: root.editorUp
                                placeholderText: qsTr("name this scene")
                                /* No text: binding — an imperative
                                   assignment would kill it forever
                                   (Task 4.8 minor). Instead the page
                                   tracks syncedSceneName: the last
                                   model name this field was shown.
                                   When the model's name moves OFF
                                   that value (discard, reload,
                                   remote apply, a refused edit's
                                   revert) the field follows it —
                                   focused or not — while edits the
                                   model never saw (uncommitted
                                   typing) survive unrelated
                                   workingChanged churn. */
                                onEditingFinished: if(root.editorUp)
                                    sceneEdit.setSceneName(text)
                                color: Theme.textPrimary
                                font.pixelSize: Theme.fontSizeBody
                                background: Rectangle {
                                    radius: Theme.radiusS
                                    color: Theme.surfaceAlt
                                    border.width: nameField.activeFocus ? 2 : 1
                                    border.color: nameField.activeFocus
                                                  ? Theme.accent : Theme.border
                                }
                                Connections {
                                    target: root.editorUp ? sceneEdit : null
                                    function onWorkingChanged() {
                                        const want = root.editorUp
                                            ? sceneEdit.sceneName : "";
                                        if(want !== root.syncedSceneName) {
                                            root.syncedSceneName = want;
                                            if(nameField.text !== want)
                                                nameField.text = want;
                                        }
                                    }
                                }
                                Component.onCompleted: {
                                    const want = root.editorUp
                                        ? sceneEdit.sceneName : "";
                                    root.syncedSceneName = want;
                                    text = want;
                                }
                            }
                            AppButton {
                                text: qsTr("Save to library")
                                enabled: root.editorUp && sceneEdit.hasWorking
                                onClicked: sceneEdit.saveProfile(
                                    nameField.text || sceneEdit.sceneName
                                    || qsTr("Untitled"))
                            }
                        }

                        Text {
                            text: qsTr("Profiles are stored by the UI and applied through the engine's normal scene validation — loading stages the scene, Save sends it. The engine remembers the playback mode per scene name.")
                            color: Theme.textDisabled
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                            width: parent.width
                        }

                        Text {
                            visible: root.editorUp && sceneEdit.libraryError !== ""
                            text: root.editorUp ? sceneEdit.libraryError : ""
                            color: Theme.bad
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                            width: parent.width
                        }

                        Repeater {
                            objectName: "profilesList"
                            model: root.editorUp ? sceneEdit.profiles : []
                            RowLayout {
                                width: parent.width
                                spacing: Theme.spaceL
                                Text {
                                    text: modelData.name
                                    color: Theme.textPrimary
                                    font.pixelSize: Theme.fontSizeBody
                                    Layout.fillWidth: true
                                }
                                Text {
                                    text: modelData.saved_at || ""
                                    color: Theme.textDisabled
                                    font.pixelSize: Theme.fontSizeSmall
                                }
                                AppButton {
                                    text: qsTr("Load")
                                    enabled: root.editorUp
                                    onClicked: sceneEdit.loadProfile(modelData.name)
                                }
                                AppButton {
                                    text: qsTr("Delete")
                                    enabled: root.editorUp
                                    onClicked: sceneEdit.deleteProfile(modelData.name)
                                }
                            }
                        }

                        EmptyState {
                            objectName: "profilesEmpty"
                            anchors.horizontalCenter: parent.horizontalCenter
                            visible: !root.editorUp || sceneEdit.profiles.length === 0
                            title: qsTr("No saved profiles")
                            detail: qsTr("Name the scene and save it to the library.")
                        }
                    }
                }
            }
        }

        /*---- Save bar — pinned footer ---------------------------
           The honest state of uncommitted work is ALWAYS
           visible: pending edits, remote-applied conflicts,
           in-flight saves, and failures with the edit kept. */
        Rectangle {
            id: saveBar
            objectName: "saveBar"
            Layout.fillWidth: true
            visible: root.editorUp
                     && (sceneEdit.dirty || sceneEdit.conflict
                         || sceneEdit.saving || sceneEdit.saveError !== ""
                         || nameField.text !== sceneEdit.sceneName)
            implicitHeight: saveRow.implicitHeight + 2 * Theme.spaceM
            color: Theme.surface
            Rectangle {
                anchors { left: parent.left; right: parent.right; top: parent.top }
                height: 1; color: Theme.border
            }

            RowLayout {
                id: saveRow
                anchors {
                    left: parent.left; right: parent.right
                    verticalCenter: parent.verticalCenter
                    margins: Theme.spaceL
                }
                spacing: Theme.spaceL

                Column {
                    Layout.fillWidth: true
                    spacing: Theme.spaceXS
                    Text {
                        wrapMode: Text.WordWrap
                        width: parent.width
                        text: {
                            if(!root.editorUp)
                                return "";
                            if(sceneEdit.saving)
                                return qsTr("Saving to the engine…");
                            if(sceneEdit.conflict)
                                return qsTr("The applied scene changed while you were editing — your edits are kept. Save overwrites the applied scene; Discard reloads it.");
                            if(sceneEdit.dirty)
                                return qsTr("Unsaved changes — nothing reaches the engine until you save.");
                            if(sceneEdit.saveError !== "")
                                return qsTr("The last save failed — your edits are still here.");
                            /* The bar is also visible for the ONLY-
                               uncommitted-field-text case — say so
                               instead of lying "Scene saved." */
                            if(nameField.text !== sceneEdit.sceneName)
                                return qsTr("The scene name has uncommitted text — press Save to keep it.");
                            return qsTr("Scene saved.");
                        }
                        color: (root.editorUp && sceneEdit.conflict)
                               ? Theme.warn : Theme.textPrimary
                        font.pixelSize: Theme.fontSizeBody
                    }
                    Row {
                        visible: root.editorUp && sceneEdit.saveError !== ""
                        spacing: Theme.spaceS
                        width: parent.width
                        /* The error SOURCE is part of the state —
                           a refused input never reads as an
                           engine rejection (Task 4.5). */
                        StatusPill {
                            id: saveErrSrc
                            visible: root.editorUp
                            text: {
                                if(!root.editorUp) return "";
                                switch(sceneEdit.saveErrorSource) {
                                case "engine":     return qsTr("engine rejected");
                                case "connection": return qsTr("not sent");
                                default:           return qsTr("refused before send");
                                }
                            }
                            tone: root.editorUp
                                  && sceneEdit.saveErrorSource === "engine"
                                  ? "bad" : "warn"
                        }
                        Text {
                            width: parent.width - saveErrSrc.width
                                   - parent.spacing
                            text: root.editorUp ? sceneEdit.saveError : ""
                            color: Theme.bad
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                AppButton {
                    text: qsTr("Reload applied")
                    visible: root.editorUp
                    enabled: root.engineUp
                    onClicked: sceneEdit.reloadFromEngine()
                }
                AppButton {
                    text: qsTr("Discard")
                    visible: root.editorUp && sceneEdit.dirty
                    enabled: root.editorUp && !sceneEdit.saving
                    onClicked: sceneEdit.discardEdits()
                }
                AppButton {
                    objectName: "saveButton"
                    text: (root.editorUp && sceneEdit.saving)
                          ? qsTr("Saving…") : qsTr("Save")
                    primary: true
                    /* Reachable also when the ONLY pending edit
                       is uncommitted field text — an uncommitted
                       name must never make Save unreachable nor
                       save the stale value. */
                    enabled: root.editorUp && root.engineUp
                             && !sceneEdit.saving
                             && (sceneEdit.dirty
                                 || nameField.text
                                    !== sceneEdit.sceneName)
                    /* Commit-on-save: focused fields commit before
                       the body exists — never a stale save. */
                    onClicked: root.commitPendingEdits()
                }
            }
        }
    }
}
