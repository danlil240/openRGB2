// SPDX-License-Identifier: GPL-2.0-or-later
//
// SettingsPage — Task 4.8 (plan sections 5-6):
//
//   - Engine connection: honest readouts (state, degraded,
//     retry, pipe, server build) + Reconnect; pipe name is
//     the one engine-side knob the UI legitimately owns.
//   - UI preferences: the ONLY locally-persisted state —
//     restore-last-view, palette position, per-scene
//     playback-param seeds, window geometry. Clear wipes
//     those and nothing else.
//   - Session: "Close UI" exits THIS process (the engine
//     keeps running); "Quit all" asks the ENGINE to shut
//     down AND hand off the managed backend — confirmed,
//     acknowledged, ordered (IPC QUIT stop_backend).
//     The two are deliberately never the same button.
import QtQuick
import QtQuick.Layouts
import OpenRGB2.UI.theme
import OpenRGB2.UI.components

Item {
    id: root
    objectName: "settingsPage"

    readonly property bool engineUp:
        typeof bridge !== "undefined" && bridge.connected
    readonly property bool prefsUp:
        typeof uiPrefs !== "undefined" && uiPrefs !== null
    readonly property bool startupUp:
        typeof startupTask !== "undefined" && startupTask !== null
        && startupTask.available
    /* The engine-side connection state (wire truth). */
    readonly property string connState:
        typeof bridge !== "undefined" ? bridge.connectionState
                                      : "disconnected"

    /* Quit-all bookkeeping: the request correlate in flight
       (0 = idle) and its outcome text. */
    property int    pendingQuitId: 0
    property string quitStatus: ""
    property string quitError: ""

    /* The window, resolved through THIS item — inside a
       Connections handler `ApplicationWindow.window` would
       resolve on the (non-Item) Connections object and come
       back undefined. */
    readonly property var appWindow: root.Window.window

    Connections {
        target: typeof bridge !== "undefined" ? bridge : null
        function onReplyReceived(id, type, body) {
            if(id !== 0 && id === root.pendingQuitId) {
                root.pendingQuitId = 0;
                if(type === "OK"
                   && body && body.ordered === true
                   && body.shutting_down === true) {
                    /* Ordered shutdown acknowledged — the engine
                       is going down; this shell's job is done.
                       backend === "closing" is the engine's
                       verified-own decision reported at request
                       time (a foreign or unverifiable backend
                       is left running — reported honestly). */
                    root.quitStatus = body.backend === "closing"
                        ? qsTr("Engine and managed backend are shutting down.")
                        : qsTr("Engine is shutting down.");
                    if(root.appWindow) {
                        root.appWindow.exitRequested = true;
                        root.appWindow.close();
                    }
                } else if(type === "OK") {
                    /* A bare OK is not a confirmed shutdown — the
                       wire contract carries ordered +
                       shutting_down; anything less keeps the
                       shell up. */
                    root.quitError = qsTr(
                        "engine acknowledged but did not confirm shutdown");
                } else {
                    root.quitError =
                        (body && body.message)
                            ? body.message
                            : qsTr("engine refused the quit request");
                }
            }
        }
        function onEngineLost() {
            /* A severed pipe forfeits the in-flight quit
               request — surface it, don't leave it hanging. */
            if(root.pendingQuitId !== 0) {
                root.pendingQuitId = 0;
                root.quitError = qsTr("engine connection lost");
            }
        }
    }

    Flickable {
        anchors.fill: parent
        contentWidth: col.implicitWidth
        contentHeight: col.implicitHeight + Theme.spaceXL
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: col
            width: Math.max(0, root.width - 2 * Theme.spaceXL)
            x: Theme.spaceXL
            y: Theme.spaceL
            spacing: Theme.spaceL

            PageHeader {
                title: qsTr("Settings")
                subtitle: qsTr("UI preferences and engine session")
            }

            /*---- Engine connection ----------------------------*/
            Card {
                Layout.fillWidth: true
                title: qsTr("Engine connection")

                ColumnLayout {
                    width: parent.width
                    spacing: Theme.spaceS

                    Repeater {
                        model: {
                            const s = (typeof bridge !== "undefined"
                                       ? bridge.serverInfo : {}) || {};
                            return [
                                { label: qsTr("State"),
                                  value: root.connState },
                                { label: qsTr("Link"),
                                  value: (typeof bridge !== "undefined"
                                          && bridge.degraded)
                                         ? qsTr("degraded — output may be stale")
                                         : qsTr("normal") },
                                { label: qsTr("Pipe"),
                                  value: (typeof bridge !== "undefined")
                                         ? bridge.pipeName : "" },
                                { label: qsTr("Engine build"),
                                  value: s.engine
                                      ? ("%1 · protocol %2")
                                        .arg(s.engine)
                                        .arg(s.negotiated_version || "?")
                                      : qsTr("unreachable — nothing reported") },
                                { label: qsTr("Auto-reconnect"),
                                  value: (typeof bridge !== "undefined"
                                          && root.connState !== "connected")
                                         ? qsTr("retrying in %1 s").arg(
                                               Math.ceil(bridge.retryInMs / 1000))
                                         : qsTr("idle") },
                            ];
                        }
                        delegate: RowLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: Theme.spaceM
                            Text {
                                text: parent.modelData.label
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSizeBody
                                Layout.preferredWidth: 140
                            }
                            Text {
                                text: parent.modelData.value
                                color: Theme.textPrimary
                                font.pixelSize: Theme.fontSizeBody
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                                Accessible.name: text
                            }
                        }
                    }

                    Text {
                        visible: typeof bridge !== "undefined"
                                 && bridge.lastError !== ""
                        text: bridge.lastError
                        color: Theme.bad
                        font.pixelSize: Theme.fontSizeSmall
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                        Accessible.name: text
                    }

                    RowLayout {
                        spacing: Theme.spaceM
                        AppButton {
                            objectName: "settingsReconnect"
                            text: qsTr("Reconnect now")
                            Accessible.name: qsTr("Reconnect to the engine now")
                            enabled: typeof bridge !== "undefined"
                                     && root.connState !== "connected"
                            onClicked: bridge.reconnectNow()
                        }
                        Text {
                            text: qsTr("The bridge retries automatically; this only skips the wait.")
                            color: Theme.textDisabled
                            font.pixelSize: Theme.fontSizeSmall
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }

            /*---- UI preferences -------------------------------*/
            Card {
                Layout.fillWidth: true
                title: qsTr("UI preferences")

                ColumnLayout {
                    width: parent.width
                    spacing: Theme.spaceS

                    AppCheckBox {
                        objectName: "restoreViewCheck"
                        text: qsTr("Reopen on the last-used page")
                        Accessible.name: text
                        enabled: root.prefsUp
                        checked: root.prefsUp
                                 ? uiPrefs.restoreLastView : true
                        onToggled:
                            if(root.prefsUp)
                                uiPrefs.restoreLastView = checked;
                    }
                    Text {
                        text: qsTr("Scene and playback state is never stored in the UI — it returns from the engine on reconnect. Only window geometry, this page's preferences, the palette position, and your last-sent playback parameters per scene are kept locally.")
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSizeSmall
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                        Accessible.name: text
                    }
                    RowLayout {
                        spacing: Theme.spaceM
                        AppButton {
                            objectName: "clearPrefsButton"
                            text: qsTr("Reset UI preferences")
                            Accessible.name: text
                            enabled: root.prefsUp
                            onClicked: uiPrefs.clear()
                        }
                        Text {
                            visible: root.prefsUp
                            text: root.prefsUp
                                  ? qsTr("stored in %1")
                                    .arg(uiPrefs.storageLocation)
                                  : ""
                            color: Theme.textDisabled
                            font.pixelSize: Theme.fontSizeSmall
                            elide: Text.ElideMiddle
                            Layout.fillWidth: true
                            Accessible.name: text
                        }
                    }
                }
            }

            /*---- Windows sign-in ------------------------------*/
            Card {
                Layout.fillWidth: true
                title: qsTr("Windows startup")

                ColumnLayout {
                    width: parent.width
                    spacing: Theme.spaceS

                    AppCheckBox {
                        objectName: "startAtLoginCheck"
                        text: qsTr("Open OpenRGB2 when I sign in")
                        Accessible.name: text
                        enabled: root.startupUp && !startupTask.busy
                        checked: root.startupUp
                                 ? startupTask.enabled : false
                        onToggled:
                            if(root.startupUp)
                                startupTask.setEnabled(checked)
                    }
                    Text {
                        text: qsTr("Starts in the tray with administrator privileges. Windows may ask for approval when you launch OpenRGB2 manually.")
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSizeSmall
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                        Accessible.name: text
                    }
                    Text {
                        visible: root.startupUp && startupTask.busy
                        text: qsTr("Checking Windows startup…")
                        color: Theme.textDisabled
                        font.pixelSize: Theme.fontSizeSmall
                        Layout.fillWidth: true
                    }
                    Text {
                        visible: root.startupUp && startupTask.error !== ""
                        text: root.startupUp
                              ? startupTask.error : ""
                        color: Theme.bad
                        font.pixelSize: Theme.fontSizeSmall
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                        Accessible.name: text
                    }
                }
            }

            /*---- Session --------------------------------------*/
            Card {
                Layout.fillWidth: true
                title: qsTr("Session")

                ColumnLayout {
                    width: parent.width
                    spacing: Theme.spaceM

                    /* Close UI — engine keeps running. */
                    RowLayout {
                        spacing: Theme.spaceM
                        AppButton {
                            objectName: "closeUiButton"
                            text: qsTr("Close UI")
                            Accessible.name:
                                qsTr("Close the UI (engine keeps running)")
                            onClicked:
                                if(root.appWindow) {
                                    root.appWindow.exitRequested = true;
                                    root.appWindow.close();
                                }
                        }
                        Text {
                            text: qsTr("Exits this window only — lighting keeps running.")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: Theme.border
                    }

                    /* Quit all — confirmed IPC QUIT. Two-step so
                       a stray click can't kill the engine. */
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spaceS

                        RowLayout {
                            spacing: Theme.spaceM
                            AppButton {
                                objectName: "quitAllButton"
                                text: qsTr("Quit all…")
                                Accessible.name:
                                    qsTr("Shut down the engine, the managed backend, and the UI")
                                enabled: root.engineUp
                                         && root.pendingQuitId === 0
                                onClicked: quitConfirm.visible = true
                            }
                            Text {
                                text: qsTr("Asks the engine to shut down and closes the managed OpenRGB backend — lighting stops. Requires a connected engine.")
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSizeSmall
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                            }
                        }

                        RowLayout {
                            id: quitConfirm
                            visible: false
                            spacing: Theme.spaceM
                            Text {
                                text: qsTr("Shut down the engine and the managed OpenRGB backend now? An ordered shutdown saves engine state first.")
                                color: Theme.warn
                                font.pixelSize: Theme.fontSizeSmall
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                                Accessible.name: text
                            }
                            AppButton {
                                objectName: "quitAllConfirm"
                                text: qsTr("Shut down all")
                                Accessible.name: text
                                onClicked: {
                                    quitConfirm.visible = false;
                                    root.quitError = "";
                                    root.quitStatus = "";
                                    const id = bridge.quitEngine(true);
                                    if(id > 0) {
                                        root.pendingQuitId = id;
                                        root.quitStatus =
                                            qsTr("shutting down…");
                                    } else {
                                        root.quitError = qsTr(
                                            "not sent — no live engine connection");
                                    }
                                }
                            }
                            AppButton {
                                objectName: "quitAllCancel"
                                text: qsTr("Cancel")
                                Accessible.name: text
                                onClicked: quitConfirm.visible = false
                            }
                        }

                        Text {
                            visible: root.quitStatus !== ""
                            text: root.quitStatus
                            color: Theme.accent
                            font.pixelSize: Theme.fontSizeSmall
                            Accessible.name: text
                        }
                        Text {
                            visible: root.quitError !== ""
                            text: qsTr("Quit failed: %1").arg(root.quitError)
                            color: Theme.bad
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                            Accessible.name: text
                        }
                    }
                }
            }

            Item { Layout.preferredHeight: Theme.spaceM }
        }
    }
}
