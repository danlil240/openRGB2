// SPDX-License-Identifier: GPL-2.0-or-later
//
// HotkeysCard — R-2 Settings card for the engine-side global
// hotkeys. One row per action: a chord capture field, a state
// pill (the engine's per-binding registration result) and a
// clear button. The engine registers the chords, so they work
// in any app and while this window is closed. Nothing is bound
// until the user binds it; "Use suggested keys" sends the
// engine's proposed set in one go.
//
// Every change sends the WHOLE set (AUTOMATION_COMMAND
// set_hotkeys) through EngineBridge::automationCommand; the
// reply / "automation" event repaints from engine truth.
import QtQuick
import QtQuick.Layouts
import OpenRGB2.UI.theme

Card {
    id: card
    objectName: "hotkeysCard"
    title: qsTr("Global hotkeys")

    readonly property bool up:
        typeof bridge !== "undefined" && bridge !== null
        && bridge.hasAutomation
    readonly property var body:
        card.up && bridge.automation ? bridge.automation : ({})
    readonly property var bindings:
        card.body.hotkeys ? card.body.hotkeys : []
    readonly property var proposed:
        card.body.proposed ? card.body.proposed : ({})

    /* The request in flight (0 = none) and its outcome. */
    property int    pendingId: 0
    property string status: ""
    property bool   statusBad: false

    readonly property var actions: [
        { action: "next_workspace",  label: qsTr("Next workspace") },
        { action: "prev_workspace",  label: qsTr("Previous workspace") },
        { action: "brightness_up",   label: qsTr("Brightness up") },
        { action: "brightness_down", label: qsTr("Brightness down") },
        { action: "lights_toggle",   label: qsTr("Lights off / on") },
        { action: "pause_toggle",    label: qsTr("Pause / resume") },
    ]

    function bindingFor(action) {
        for(let i = 0; i < card.bindings.length; ++i) {
            if(card.bindings[i].action === action)
                return card.bindings[i];
        }
        return null;
    }

    function send(list) {
        const id = card.up
            ? bridge.automationCommand("set_hotkeys", { bindings: list })
            : 0;
        if(id > 0) {
            card.pendingId = id;
            card.status = qsTr("saving…");
            card.statusBad = false;
        } else {
            card.status = qsTr("not sent — no live engine connection");
            card.statusBad = true;
        }
    }

    /* Replace (or with "" remove) one action's chord. */
    function setChord(action, chord) {
        const list = [];
        for(let i = 0; i < card.bindings.length; ++i) {
            const b = card.bindings[i];
            if(b.action !== action)
                list.push({ action: b.action, chord: b.chord });
        }
        if(chord !== "")
            list.push({ action: action, chord: chord });
        card.send(list);
    }

    function useSuggested() {
        const list = [];
        for(let i = 0; i < card.actions.length; ++i) {
            const a = card.actions[i].action;
            if(card.proposed[a])
                list.push({ action: a, chord: card.proposed[a] });
        }
        card.send(list);
    }

    function pillText(b) {
        if(!b) return "";
        switch(b.state) {
        case "ok":          return qsTr("active");
        case "in_use":      return qsTr("in use by another app");
        case "invalid":     return qsTr("cannot be registered");
        default:            return qsTr("unavailable");
        }
    }
    function pillTone(b) {
        if(!b) return "neutral";
        return b.state === "ok" ? "ok"
             : b.state === "in_use" || b.state === "invalid" ? "bad"
             : "warn";
    }

    Connections {
        target: typeof bridge !== "undefined" ? bridge : null
        function onReplyReceived(id, type, body) {
            if(id === 0 || id !== card.pendingId)
                return;
            card.pendingId = 0;
            if(type === "ERROR") {
                card.status = (body && body.message)
                    ? body.message : qsTr("the engine refused the change");
                card.statusBad = true;
            } else {
                card.status = "";
                card.statusBad = false;
            }
        }
        function onEngineLost() {
            if(card.pendingId !== 0) {
                card.pendingId = 0;
                card.status = qsTr("engine connection lost");
                card.statusBad = true;
            }
        }
    }

    ColumnLayout {
        width: parent.width
        spacing: Theme.spaceS

        Text {
            text: qsTr("Work in any app — games included — even with this window closed. Off until you bind a key. Click a field, then press the combination (it needs Ctrl, Alt or Win); Backspace clears it.")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            Accessible.name: text
        }
        Text {
            visible: !card.up
            text: qsTr("Not available — the engine is not connected or does not support hotkeys.")
            color: Theme.textDisabled
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            Accessible.name: text
        }

        Repeater {
            model: card.actions
            delegate: RowLayout {
                id: row
                required property var modelData
                readonly property var binding: card.bindingFor(modelData.action)
                objectName: "hotkeyRow_" + modelData.action
                Layout.fillWidth: true
                spacing: Theme.spaceM

                Text {
                    text: row.modelData.label
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSizeBody
                    Layout.preferredWidth: 160
                }
                ChordCaptureField {
                    objectName: "hotkeyCapture_" + row.modelData.action
                    Layout.preferredWidth: 240
                    enabled: card.up
                    chord: row.binding ? row.binding.chord : ""
                    placeholder: card.proposed[row.modelData.action] || ""
                    onCaptured: (c) => card.setChord(row.modelData.action, c)
                    onCleared: card.setChord(row.modelData.action, "")
                }
                StatusPill {
                    objectName: "hotkeyState_" + row.modelData.action
                    visible: row.binding !== null
                    text: card.pillText(row.binding)
                    tone: card.pillTone(row.binding)
                    Accessible.name: text
                    Accessible.description:
                        row.binding && row.binding.detail
                            ? row.binding.detail : ""
                }
                AppButton {
                    objectName: "hotkeyClear_" + row.modelData.action
                    visible: row.binding !== null
                    enabled: card.up
                    text: qsTr("Clear")
                    Accessible.name: qsTr("Clear the %1 hotkey")
                                         .arg(row.modelData.label)
                    onClicked: card.setChord(row.modelData.action, "")
                }
                Item { Layout.fillWidth: true }
            }
        }

        RowLayout {
            spacing: Theme.spaceM
            AppButton {
                objectName: "hotkeysSuggested"
                text: qsTr("Use suggested keys")
                Accessible.name: qsTr("Bind every action to its suggested key combination")
                enabled: card.up && card.pendingId === 0
                onClicked: card.useSuggested()
            }
            Text {
                visible: card.status !== ""
                text: card.status
                color: card.statusBad ? Theme.bad : Theme.textSecondary
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Accessible.name: text
            }
        }
    }
}
