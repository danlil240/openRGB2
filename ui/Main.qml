// SPDX-License-Identifier: GPL-2.0-or-later
//
// Main — application shell (Task 4.8): left navigation over
// the five section-5 views, an honest connection banner
// (unreachable / connecting / connected / degraded), and
// UI-only preference persistence (window geometry + last
// view via uiPrefs — engine-owned state is NEVER stored
// here; it returns through WELCOME + subscription replay).
// Closing this window only closes the UI — the engine keeps
// running; engine shutdown lives on Settings → Quit All
// (a confirmed IPC QUIT, plan section 4 UI independence).
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import OpenRGB2.UI.theme
import OpenRGB2.UI.components
import OpenRGB2.UI.pages

ApplicationWindow {
    id: root
    objectName: "mainWindow"

    visible: true
    width: 1180; height: 760
    minimumWidth: 960; minimumHeight: 620
    title: qsTr("OpenRGB2")
    color: Theme.bg

    readonly property bool engineUp:
        typeof bridge !== "undefined" && bridge.connected
    readonly property string connState:
        typeof bridge !== "undefined" ? bridge.connectionState
                                      : "disconnected"
    /* uiPrefs is injected by main.cpp (a UiSettings). Tests
       that only load the shell for composition checks may
       leave it out — every use is guarded so absence renders
       as defaults, never a crash. */
    readonly property bool prefsUp:
        typeof uiPrefs !== "undefined" && uiPrefs !== null

    property int currentView: 0

    Component.onCompleted: {
        if(root.prefsUp) {
            if(uiPrefs.restoreLastView)
                root.currentView =
                    Math.max(0, Math.min(4, uiPrefs.lastView));
            if(uiPrefs.windowWidth >= root.minimumWidth
               && uiPrefs.windowHeight >= root.minimumHeight) {
                root.width  = uiPrefs.windowWidth;
                root.height = uiPrefs.windowHeight;
            }
            if(uiPrefs.windowX >= 0 && uiPrefs.windowY >= 0) {
                root.x = uiPrefs.windowX;
                root.y = uiPrefs.windowY;
            }
        }
    }
    /* Geometry + last view are UI-owned — saved on close and
       on view change (write-through; QSettings batches the
       platform write). */
    onCurrentViewChanged:
        if(root.prefsUp)
            uiPrefs.lastView = root.currentView;
    onClosing: function(close) {
        if(root.prefsUp)
            uiPrefs.saveGeometry(root.x, root.y,
                                 root.width, root.height);
    }

    /* A text editor owns Ctrl+Z/Ctrl+Y for its own undo —
       the app-level layout undo must not shadow it. */
    function textEditingActive() {
        let item = root.activeFocusItem;
        while(item) {
            if(item.hasOwnProperty("selectedText")
               && item.hasOwnProperty("cursorPosition"))
                return true;
            item = item.parent;
        }
        return false;
    }

    /* Ctrl+1..5 view switching — keyboard access. */
    Shortcut { sequence: "Ctrl+1"; onActivated: root.currentView = 0 }
    Shortcut { sequence: "Ctrl+2"; onActivated: root.currentView = 1 }
    Shortcut { sequence: "Ctrl+3"; onActivated: root.currentView = 2 }
    Shortcut { sequence: "Ctrl+4"; onActivated: root.currentView = 3 }
    Shortcut { sequence: "Ctrl+5"; onActivated: root.currentView = 4 }
    /* Ctrl+S saves the Lighting working copy — commits any
       focused field's uncommitted text first (same path as
       the on-screen Save button). */
    Shortcut {
        sequence: "Ctrl+S"
        enabled: root.currentView === 1
                 && typeof sceneEdit !== "undefined"
                 && !sceneEdit.saving
        onActivated: lightingPage.commitPendingEdits()
    }
    /* Ctrl+Z / Ctrl+Shift+Z (+Ctrl+Y) drive the Layout undo
       timeline — only while the Layout view is up and no
       text field owns the keys. */
    Shortcut {
        sequence: "Ctrl+Z"
        enabled: root.currentView === 2
                 && typeof layoutCtl !== "undefined"
                 && layoutCtl.canUndo
                 && !root.textEditingActive()
        onActivated: layoutCtl.undo()
    }
    Shortcut {
        sequence: "Ctrl+Shift+Z"
        enabled: root.currentView === 2
                 && typeof layoutCtl !== "undefined"
                 && layoutCtl.canRedo
                 && !root.textEditingActive()
        onActivated: layoutCtl.redo()
    }
    Shortcut {
        sequence: "Ctrl+Y"
        enabled: root.currentView === 2
                 && typeof layoutCtl !== "undefined"
                 && layoutCtl.canRedo
                 && !root.textEditingActive()
        onActivated: layoutCtl.redo()
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        /*---- Sidebar ------------------------------------------*/
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: Theme.navWidth
            color: Theme.surface
            border.color: Theme.border
            border.width: 0
            Rectangle {
                anchors { top: parent.top; bottom: parent.bottom; right: parent.right }
                width: 1; color: Theme.border
            }

            Column {
                anchors.fill: parent
                anchors.margins: Theme.spaceM
                spacing: Theme.spaceS

                Text {
                    text: "OpenRGB2"
                    color: Theme.accent
                    font.pixelSize: Theme.fontSizeTitle
                    font.weight: Font.Bold
                    leftPadding: Theme.spaceS
                    bottomPadding: Theme.spaceM
                }

                NavButton {
                    objectName: "navDashboard"
                    width: parent.width
                    text: qsTr("Dashboard")
                    selected: root.currentView === 0
                    onClicked: root.currentView = 0
                }
                NavButton {
                    objectName: "navLighting"
                    width: parent.width
                    text: qsTr("Lighting")
                    selected: root.currentView === 1
                    onClicked: root.currentView = 1
                }
                NavButton {
                    objectName: "navLayout"
                    width: parent.width
                    text: qsTr("Layout")
                    selected: root.currentView === 2
                    onClicked: root.currentView = 2
                }
                NavButton {
                    objectName: "navDevices"
                    width: parent.width
                    text: qsTr("Devices && Diagnostics")
                    selected: root.currentView === 3
                    onClicked: root.currentView = 3
                }
                NavButton {
                    objectName: "navSettings"
                    width: parent.width
                    text: qsTr("Settings")
                    selected: root.currentView === 4
                    onClicked: root.currentView = 4
                }

                Item { width: 1; height: Theme.spaceM }

                StatusPill {
                    text: {
                        switch(root.connState) {
                        case "connected":
                            return (typeof bridge !== "undefined"
                                    && bridge.degraded)
                                    ? qsTr("Connected · degraded")
                                    : qsTr("Connected");
                        case "connecting":   return qsTr("Connecting…");
                        case "unreachable":  return qsTr("Engine unreachable");
                        default:             return qsTr("Disconnected");
                        }
                    }
                    tone: root.connState === "connected"
                          ? ((typeof bridge !== "undefined"
                              && bridge.degraded) ? "warn" : "ok")
                          : root.connState === "connecting" ? "accent"
                          : root.connState === "unreachable" ? "bad"
                          : "neutral"
                }
                Text {
                    visible: root.connState === "unreachable"
                             && typeof bridge !== "undefined"
                             && bridge.retryInMs > 0
                    text: qsTr("retrying in %1 s")
                        .arg(Math.ceil(bridge.retryInMs / 1000))
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSizeSmall
                    leftPadding: Theme.spaceS
                }
            }
        }

        /*---- Content -------------------------------------------*/
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            /* Honest state banner — visible whenever the engine
               is not fully usable. */
            Rectangle {
                visible: root.connState !== "connected"
                         || (typeof bridge !== "undefined" && bridge.degraded)
                Layout.fillWidth: true
                implicitHeight: bannerRow.implicitHeight + 2 * Theme.spaceM
                color: root.connState === "connected" ? Theme.surfaceAlt
                                                      : Theme.surface
                border.color: Theme.border
                border.width: 0
                Rectangle {
                    anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                    height: 1; color: Theme.border
                }

                Row {
                    id: bannerRow
                    anchors {
                        left: parent.left; right: parent.right
                        verticalCenter: parent.verticalCenter
                        margins: Theme.spaceL
                        leftMargin: Theme.spaceL; rightMargin: Theme.spaceL
                    }
                    spacing: Theme.spaceL

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - reconnectBtn.width - Theme.spaceL
                        wrapMode: Text.WordWrap
                        text: {
                            if(root.connState === "connected")
                                return qsTr("Connected, but the engine reports a degraded link — lighting output may be stale. See Devices & Diagnostics.");
                            if(root.connState === "connecting")
                                return qsTr("Connecting to the engine…");
                            if(root.connState === "unreachable")
                                return (typeof bridge !== "undefined"
                                        && bridge.lastError)
                                    ? qsTr("Engine unreachable — retrying automatically. %1")
                                        .arg(bridge.lastError)
                                    : qsTr("Engine unreachable — retrying automatically.");
                            return qsTr("Engine disconnected. Lighting keeps running; reconnect to resume control.");
                        }
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSizeBody
                    }
                    AppButton {
                        id: reconnectBtn
                        objectName: "reconnectButton"
                        Accessible.name: qsTr("Reconnect to the engine")
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Reconnect")
                        visible: typeof bridge !== "undefined"
                                 && root.connState !== "connected"
                        onClicked: bridge.reconnectNow()
                    }
                }
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: root.currentView

                DashboardPage {}
                LightingPage { id: lightingPage }
                LayoutPage {}
                DevicesPage {}
                SettingsPage {}
            }
        }
    }
}
