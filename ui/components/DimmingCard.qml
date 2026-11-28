// SPDX-License-Identifier: GPL-2.0-or-later
//
// DimmingCard — R-4 Settings card for the engine-side dimming
// rules: a nightly schedule (days, from, to, level), the lock
// screen, and user idle. The engine runs the rules (they work
// with this window closed); this card edits a local copy and
// sends the WHOLE set with AUTOMATION_COMMAND set_dimming
// (EngineBridge::automationCommand) on Apply. Engine truth —
// the AUTOMATION body's dimming.config / dimming.active —
// re-seeds the editor whenever nothing is being edited.
import QtQuick
import QtQuick.Layouts
import OpenRGB2.UI.theme

Card {
    id: card
    objectName: "dimmingCard"
    title: qsTr("Automatic dimming")

    readonly property bool up:
        typeof bridge !== "undefined" && bridge !== null
        && bridge.hasAutomation
    readonly property var dimming:
        card.up && bridge.automation && bridge.automation.dimming
            ? bridge.automation.dimming : ({})
    readonly property var engineConfig:
        card.dimming.config ? card.dimming.config : ({})
    readonly property var active:
        card.dimming.active ? card.dimming.active : ({})

    /* The local editor (seeded from engine truth while clean). */
    property bool editing: false
    property bool lockEnabled: false
    property real lockLevel: 0.3
    property bool idleEnabled: false
    property int  idleMinutes: 10
    property real idleLevel: 0.3
    property var  windows: []

    property int    pendingId: 0
    property string status: ""
    property bool   statusBad: false

    readonly property var dayNames: [
        qsTr("Sun"), qsTr("Mon"), qsTr("Tue"), qsTr("Wed"),
        qsTr("Thu"), qsTr("Fri"), qsTr("Sat") ]

    function seed() {
        if(card.editing)
            return;
        const c = card.engineConfig;
        card.lockEnabled = !!(c.lock && c.lock.enabled);
        card.lockLevel   = c.lock && c.lock.level !== undefined
                           ? c.lock.level : 0.3;
        card.idleEnabled = !!(c.idle && c.idle.enabled);
        card.idleMinutes = Math.max(1, Math.round(
            (c.idle && c.idle.after_s ? c.idle.after_s : 600) / 60));
        card.idleLevel   = c.idle && c.idle.level !== undefined
                           ? c.idle.level : 0.3;
        const w = [];
        const s = c.schedule || [];
        for(let i = 0; i < s.length; ++i) {
            w.push({ days: (s[i].days || []).slice(),
                     start: s[i].start, end: s[i].end,
                     level: s[i].level });
        }
        card.windows = w;
    }
    onEngineConfigChanged: seed()
    Component.onCompleted: seed()

    function touch() {
        card.editing = true;
        card.status = "";
    }
    function addWindow() {
        if(card.windows.length >= 8)
            return;
        const w = card.windows.slice();
        w.push({ days: [], start: "23:00", end: "07:00", level: 0.3 });
        card.windows = w;
        touch();
    }
    function removeWindow(i) {
        const w = card.windows.slice();
        w.splice(i, 1);
        card.windows = w;
        touch();
    }
    function setWindow(i, key, value) {
        const w = card.windows.slice();
        const o = Object.assign({}, w[i]);
        o[key] = value;
        w[i] = o;
        card.windows = w;
        touch();
    }
    function toggleDay(i, d) {
        const days = (card.windows[i].days || []).slice();
        const at = days.indexOf(d);
        if(at >= 0)
            days.splice(at, 1);
        else
            days.push(d);
        days.sort();
        setWindow(i, "days", days);
    }

    /* The set_dimming body (also what tests read back). */
    function buildConfig() {
        const sched = [];
        for(let i = 0; i < card.windows.length; ++i) {
            const w = card.windows[i];
            sched.push({ days: (w.days || []).slice().sort(),
                         start: w.start, end: w.end,
                         level: Math.round(w.level * 100) / 100 });
        }
        return {
            schedule: sched,
            lock: { enabled: card.lockEnabled,
                    level: Math.round(card.lockLevel * 100) / 100 },
            idle: { enabled: card.idleEnabled,
                    after_s: Math.max(60, card.idleMinutes * 60),
                    level: Math.round(card.idleLevel * 100) / 100,
                    skip_fullscreen: true },
            ramp_ms: card.engineConfig.ramp_ms !== undefined
                     ? card.engineConfig.ramp_ms : 600,
            restore_ms: card.engineConfig.restore_ms !== undefined
                        ? card.engineConfig.restore_ms : 0
        };
    }
    function apply() {
        const body = buildConfig();
        const id = card.up
            ? bridge.automationCommand("set_dimming", body) : 0;
        if(id > 0) {
            card.pendingId = id;
            card.status = qsTr("saving…");
            card.statusBad = false;
        } else {
            card.status = qsTr("not sent — no live engine connection");
            card.statusBad = true;
        }
    }
    function revert() {
        card.editing = false;
        card.status = "";
        seed();
    }

    function activeText() {
        const parts = [];
        const names = { schedule: qsTr("schedule"),
                        lock: qsTr("lock screen"),
                        idle: qsTr("idle") };
        for(const k of ["schedule", "lock", "idle"]) {
            const v = card.active[k];
            if(v !== undefined && v !== null)
                parts.push(v <= 0 ? qsTr("%1 (off)").arg(names[k])
                                  : qsTr("%1 (%2%)").arg(names[k])
                                        .arg(Math.round(v * 100)));
        }
        return parts.length > 0
            ? qsTr("Active now: %1").arg(parts.join(", "))
            : qsTr("Not dimming right now.");
    }

    Connections {
        target: typeof bridge !== "undefined" ? bridge : null
        function onReplyReceived(id, type, body) {
            if(id === 0 || id !== card.pendingId)
                return;
            card.pendingId = 0;
            if(type === "ERROR") {
                card.status = (body && body.message)
                    ? body.message : qsTr("the engine refused the rules");
                card.statusBad = true;
            } else {
                card.status = "";
                card.statusBad = false;
                card.editing = false;
                card.seed();
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
            text: qsTr("The engine dims the lights on its own — also with this window closed. Each rule has its own level (0% = off); when several apply, the strongest wins. A fullscreen game never counts as idle. 0% also stops an uploaded wireless loop; other levels do not reach it.")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            Accessible.name: text
        }
        Text {
            visible: !card.up
            text: qsTr("Not available — the engine is not connected or does not support dimming.")
            color: Theme.textDisabled
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        Text {
            objectName: "dimActive"
            visible: card.up
            text: card.activeText()
            color: Theme.accent
            font.pixelSize: Theme.fontSizeSmall
            Layout.fillWidth: true
            Accessible.name: text
        }

        /*---- lock --------------------------------------------*/
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceM
            AppCheckBox {
                objectName: "dimLockEnabled"
                text: qsTr("While this PC is locked")
                Accessible.name: qsTr("Dim while this PC is locked")
                enabled: card.up
                checked: card.lockEnabled
                onToggled: { card.lockEnabled = checked; card.touch(); }
                Layout.preferredWidth: 300
            }
            AppSlider {
                objectName: "dimLockLevel"
                from: 0; to: 1; stepSize: 0.05
                enabled: card.up && card.lockEnabled
                value: card.lockLevel
                onMoved: { card.lockLevel = value; card.touch(); }
                Layout.preferredWidth: 180
                Accessible.name: qsTr("Locked level")
            }
            Text {
                text: Math.round(card.lockLevel * 100) + "%"
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSizeBody
            }
            Item { Layout.fillWidth: true }
        }

        /*---- idle --------------------------------------------*/
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceM
            AppCheckBox {
                objectName: "dimIdleEnabled"
                text: qsTr("After")
                Accessible.name: qsTr("Dim after a period without keyboard or mouse input")
                enabled: card.up
                checked: card.idleEnabled
                onToggled: { card.idleEnabled = checked; card.touch(); }
            }
            AppTextField {
                objectName: "dimIdleMinutes"
                enabled: card.up && card.idleEnabled
                text: String(card.idleMinutes)
                validator: IntValidator { bottom: 1; top: 1440 }
                Layout.preferredWidth: 64
                Accessible.name: qsTr("Idle minutes")
                onEditingFinished:
                    if(acceptableInput) {
                        card.idleMinutes = parseInt(text);
                        card.touch();
                    }
            }
            Text {
                text: qsTr("min without keyboard or mouse")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSizeBody
                Layout.preferredWidth: 190
            }
            AppSlider {
                objectName: "dimIdleLevel"
                from: 0; to: 1; stepSize: 0.05
                enabled: card.up && card.idleEnabled
                value: card.idleLevel
                onMoved: { card.idleLevel = value; card.touch(); }
                Layout.preferredWidth: 180
                Accessible.name: qsTr("Idle level")
            }
            Text {
                text: Math.round(card.idleLevel * 100) + "%"
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSizeBody
            }
            Item { Layout.fillWidth: true }
        }

        /*---- schedule ----------------------------------------*/
        Text {
            text: qsTr("Schedule (local time; a window past midnight belongs to the day it starts; no day picked = every day)")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        Repeater {
            model: card.windows
            delegate: RowLayout {
                id: wrow
                required property var modelData
                required property int index
                objectName: "dimWindow_" + index
                Layout.fillWidth: true
                spacing: Theme.spaceS

                Repeater {
                    model: 7
                    delegate: AppButton {
                        required property int index
                        objectName: "dimWindowDay_" + wrow.index + "_" + index
                        text: card.dayNames[index]
                        checkable: true
                        checked: (wrow.modelData.days || []).indexOf(index) >= 0
                        enabled: card.up
                        padding: Theme.spaceS
                        Accessible.name: qsTr("Window %1, %2")
                                             .arg(wrow.index + 1)
                                             .arg(card.dayNames[index])
                        onClicked: card.toggleDay(wrow.index, index)
                    }
                }
                AppTextField {
                    objectName: "dimWindowStart_" + wrow.index
                    enabled: card.up
                    text: wrow.modelData.start
                    Layout.preferredWidth: 70
                    validator: RegularExpressionValidator {
                        regularExpression: /([01][0-9]|2[0-3]):[0-5][0-9]/
                    }
                    Accessible.name: qsTr("Window %1 from").arg(wrow.index + 1)
                    onEditingFinished:
                        if(acceptableInput && text !== wrow.modelData.start)
                            card.setWindow(wrow.index, "start", text)
                }
                Text {
                    text: "–"
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSizeBody
                }
                AppTextField {
                    objectName: "dimWindowEnd_" + wrow.index
                    enabled: card.up
                    text: wrow.modelData.end
                    Layout.preferredWidth: 70
                    validator: RegularExpressionValidator {
                        regularExpression: /([01][0-9]|2[0-3]):[0-5][0-9]/
                    }
                    Accessible.name: qsTr("Window %1 to").arg(wrow.index + 1)
                    onEditingFinished:
                        if(acceptableInput && text !== wrow.modelData.end)
                            card.setWindow(wrow.index, "end", text)
                }
                AppSlider {
                    objectName: "dimWindowLevel_" + wrow.index
                    from: 0; to: 1; stepSize: 0.05
                    enabled: card.up
                    value: wrow.modelData.level
                    Layout.preferredWidth: 140
                    Accessible.name: qsTr("Window %1 level").arg(wrow.index + 1)
                    onMoved: card.setWindow(wrow.index, "level", value)
                }
                Text {
                    text: Math.round(wrow.modelData.level * 100) + "%"
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontSizeBody
                }
                AppButton {
                    objectName: "dimWindowRemove_" + wrow.index
                    text: qsTr("Remove")
                    enabled: card.up
                    Accessible.name: qsTr("Remove window %1").arg(wrow.index + 1)
                    onClicked: card.removeWindow(wrow.index)
                }
                Item { Layout.fillWidth: true }
            }
        }

        RowLayout {
            spacing: Theme.spaceM
            AppButton {
                objectName: "dimAddWindow"
                text: qsTr("Add window")
                enabled: card.up && card.windows.length < 8
                onClicked: card.addWindow()
            }
            AppButton {
                objectName: "dimApply"
                primary: true
                text: qsTr("Apply")
                enabled: card.up && card.editing && card.pendingId === 0
                onClicked: card.apply()
            }
            AppButton {
                objectName: "dimRevert"
                text: qsTr("Revert")
                enabled: card.editing
                onClicked: card.revert()
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
