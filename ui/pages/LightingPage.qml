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
    readonly property string requestedMode:
        engineUp ? (bridge.playback.requested_mode || "") : ""

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
                        Keys.onPressed: e => {
                            if(e.key === Qt.Key_Space || e.key === Qt.Key_Return) {
                                if(root.editorUp && root.engineUp)
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
                                text: qsTr("reactive input arrives with the live-inputs release")
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
                   Spec wording verbatim. Capability-gated —
                   animation_playback is honest about scenes
                   that can't upload.                       */
                RowLayout {
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
                            readonly property bool usable:
                                root.engineUp
                                && bridge.playback.capabilities
                                && bridge.playback.capabilities[modelData.key] !== false
                            border.color: root.requestedMode === modelData.key
                                          ? Theme.accent : Theme.border
                            border.width: root.requestedMode === modelData.key ? 2 : 1

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
                                    text: bridge.playback.unavailable_reason
                                          || qsTr("not available for this scene")
                                    color: Theme.warn
                                    font.pixelSize: Theme.fontSizeSmall
                                    wrapMode: Text.WordWrap
                                    width: parent.width
                                }
                                StatusPill {
                                    visible: root.requestedMode === modelData.key
                                    text: bridge.playback.pending
                                          ? qsTr("applying…")
                                          : qsTr("selected")
                                    tone: bridge.playback.pending ? "warn" : "accent"
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                enabled: usable && root.editorUp
                                         && root.requestedMode !== modelData.key
                                cursorShape: enabled ? Qt.PointingHandCursor
                                                     : Qt.ArrowCursor
                                onClicked: sceneEdit.requestMode(modelData.key)
                            }
                        }
                    }
                }

                /* Upload / transport state — Uploading, Active,
                   Failed, paused/stopped as the engine reports. */
                Card {
                    title: qsTr("PLAYBACK STATE")
                    width: parent.width - 2 * col.padding
                    visible: root.engineUp

                    RowLayout {
                        width: parent.width
                        spacing: Theme.spaceL

                        StatusPill {
                            text: {
                                const st = bridge.playback.status;
                                if(bridge.playback.pending)
                                    return qsTr("applying…");
                                if(st && st.upload && st.upload !== "none")
                                    return st.upload;
                                if(st && st.control)
                                    return st.control;
                                return qsTr("unknown");
                            }
                            tone: {
                                const st = bridge.playback.status;
                                if(bridge.playback.pending)
                                    return "warn";
                                if(st && st.upload === "failed")
                                    return "bad";
                                if(st && st.control === "playing")
                                    return "ok";
                                return "neutral";
                            }
                        }
                        Text {
                            visible: !!bridge.playback.detail
                            text: bridge.playback.detail || ""
                            color: Theme.bad
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                        Item { Layout.fillWidth: true }
                        Repeater {
                            model: [
                                { key: "playing", label: qsTr("Play") },
                                { key: "paused",  label: qsTr("Pause") },
                                { key: "stopped", label: qsTr("Stop") }
                            ]
                            AppButton {
                                text: modelData.label
                                enabled: root.editorUp && root.engineUp
                                         && bridge.playback.status
                                         && bridge.playback.status.control !== modelData.key
                                onClicked: sceneEdit.setPlaybackControl(modelData.key)
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
                                AppSlider {
                                    id: palH
                                    width: parent.width
                                    from: 0; to: 1; stepSize: 0.001
                                    value: 0.55
                                    enabled: root.editorUp
                                    Accessible.name: qsTr("hue")
                                }
                                AppSlider {
                                    id: palS
                                    width: parent.width
                                    from: 0; to: 1; stepSize: 0.001
                                    value: 0.6
                                    enabled: root.editorUp
                                    Accessible.name: qsTr("saturation")
                                }
                                AppSlider {
                                    id: palV
                                    width: parent.width
                                    from: 0; to: 1; stepSize: 0.001
                                    value: 1.0
                                    enabled: root.editorUp
                                    Accessible.name: qsTr("brightness")
                                }
                            }
                            AppButton {
                                text: qsTr("Apply to selected")
                                primary: true
                                enabled: root.editorUp
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
                                    enabled: root.editorUp
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
                                Layout.fillWidth: true
                                enabled: root.editorUp && sceneEdit.hasWorking
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
                                Layout.fillWidth: true
                                enabled: root.editorUp && sceneEdit.hasWorking
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
                                Layout.fillWidth: true
                                enabled: root.editorUp && sceneEdit.hasWorking
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
                   Breathing. Sent through the controller so the
                   current transport control is preserved — a
                   speed drag can never silently resume a
                   paused upload.                       */
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

                        RowLayout {
                            width: parent.width
                            spacing: Theme.spaceL
                            enabled: root.engineUp
                                     && root.requestedMode === "animation_playback"
                            Text {
                                text: qsTr("Speed")
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSizeBody
                                Layout.preferredWidth: 110
                            }
                            AppSlider {
                                id: upSpeedSlider
                                Layout.fillWidth: true
                                enabled: root.editorUp && parent.enabled
                                         && sceneEdit.presetSupport.speed === true
                                from: 10; to: 200; stepSize: 1
                                onMoved: paramTimer.restart()
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
                                         && root.requestedMode === "animation_playback"
                                         && sceneEdit.presetSupport.direction === true
                                text: qsTr("reverse")
                                onToggled: if(enabled) paramTimer.restart()
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
                                    width: parent.width
                                    from: 0; to: 1; stepSize: 0.001; value: 0
                                    enabled: root.editorUp && root.engineUp
                                             && root.requestedMode === "animation_playback"
                                             && sceneEdit.presetSupport.color === true
                                    onMoved: paramTimer.restart()
                                }
                                AppSlider {
                                    id: upS
                                    width: parent.width
                                    from: 0; to: 1; stepSize: 0.001; value: 1
                                    enabled: upH.enabled
                                    onMoved: paramTimer.restart()
                                }
                                AppSlider {
                                    id: upV
                                    width: parent.width
                                    from: 0; to: 1; stepSize: 0.001; value: 1
                                    enabled: upH.enabled
                                    onMoved: paramTimer.restart()
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

                /*---- Reactive inputs (placeholder) --------------
                   Audio/screen/key capture arrives with the
                   live-inputs task (4.6) — honest disabled
                   controls, never fake toggles.           */
                Card {
                    title: qsTr("REACTIVE INPUTS")
                    width: parent.width - 2 * col.padding
                    Column {
                        spacing: Theme.spaceS
                        Text {
                            text: qsTr("Audio, screen, and keyboard reactive capture arrive with the live-inputs release. Presets that need an input show a badge above.")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                            width: parent.width
                        }
                        Repeater {
                            model: [ qsTr("Audio reactive"), qsTr("Screen reactive"),
                                     qsTr("Keyboard reactive") ]
                            CheckBox {
                                text: modelData
                                enabled: false
                            }
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
                                Layout.fillWidth: true
                                enabled: root.editorUp
                                placeholderText: qsTr("name this scene")
                                text: root.editorUp ? sceneEdit.sceneName : ""
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
                                        if(!nameField.activeFocus
                                           && nameField.text !== sceneEdit.sceneName)
                                            nameField.text = sceneEdit.sceneName;
                                    }
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
                         || sceneEdit.saving || sceneEdit.saveError !== "")
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
                            return qsTr("Scene saved.");
                        }
                        color: (root.editorUp && sceneEdit.conflict)
                               ? Theme.warn : Theme.textPrimary
                        font.pixelSize: Theme.fontSizeBody
                    }
                    Text {
                        visible: root.editorUp && sceneEdit.saveError !== ""
                        text: root.editorUp ? sceneEdit.saveError : ""
                        color: Theme.bad
                        font.pixelSize: Theme.fontSizeSmall
                        wrapMode: Text.WordWrap
                        width: parent.width
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
                    enabled: root.editorUp && root.engineUp
                             && sceneEdit.dirty && !sceneEdit.saving
                    onClicked: sceneEdit.save()
                }
            }
        }
    }
}
