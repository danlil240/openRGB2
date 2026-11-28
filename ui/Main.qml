// SPDX-License-Identifier: GPL-2.0-or-later
//
// Main — application shell (Task 4.8): left navigation over
// the five section-5 views, an honest connection banner
// (unreachable / connecting / connected / degraded), and
// UI-only preference persistence (window geometry + last
// view via uiPrefs — engine-owned state is NEVER stored
// here; it returns through WELCOME + subscription replay).
// Closing this window hides the UI when a tray is available;
// engine shutdown lives on Settings → Quit All
// (a confirmed IPC QUIT, plan section 4 UI independence).
import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import QtQuick.Controls
import OpenRGB2.UI.theme
import OpenRGB2.UI.components
import OpenRGB2.UI.pages

ApplicationWindow {
    id: root
    objectName: "mainWindow"

    visible: !(typeof startHidden !== "undefined" && startHidden)
    width: 1180; height: 760
    minimumWidth: 960; minimumHeight: 620
    title: qsTr("OpenRGB2")
    color: Theme.bg
    property bool exitRequested: false

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

    /* The saved rect is accepted only when its title-bar strip
       lands on a screen that exists NOW (monitors get unplugged
       or rearranged). Coordinates may be negative — a monitor
       left of / above the primary is a normal setup. */
    function screenFor(x, y, w) {
        const screens = Qt.application.screens;
        for(let i = 0; i < screens.length; ++i) {
            const sc = screens[i];
            const sx = sc.virtualX, sy = sc.virtualY;
            if(x + w - 80 > sx && x + 80 < sx + sc.width
               && y >= sy - 8 && y + 40 < sy + sc.height)
                return sc;
        }
        return null;
    }
    function restoreGeometry() {
        let w = uiPrefs.windowWidth, h = uiPrefs.windowHeight;
        const sized = w >= root.minimumWidth
                      && h >= root.minimumHeight;
        const sc = uiPrefs.hasWindowPosition
                   ? root.screenFor(uiPrefs.windowX, uiPrefs.windowY,
                                    sized ? w : root.width)
                   : null;
        const screens = Qt.application.screens;
        const fit = sc || (screens.length > 0 ? screens[0] : null);
        if(sized) {
            /* Never larger than the screen it opens on. */
            if(fit) {
                w = Math.min(w, Math.max(root.minimumWidth, fit.width));
                h = Math.min(h, Math.max(root.minimumHeight, fit.height));
            }
            root.width  = w;
            root.height = h;
        }
        if(sc) {
            root.x = Math.min(uiPrefs.windowX,
                              sc.virtualX + sc.width - 80);
            root.y = Math.max(sc.virtualY,
                              Math.min(uiPrefs.windowY,
                                       sc.virtualY + sc.height - 40));
        }
        root.normalGeom = Qt.rect(root.x, root.y,
                                  root.width, root.height);
    }

    Component.onCompleted: {
        if(root.prefsUp) {
            if(uiPrefs.restoreLastView)
                root.currentView =
                    Math.max(0, Math.min(4, uiPrefs.lastView));
            root.restoreGeometry();
            /* The maximized state itself is applied by main.cpp
               (QWindow::setWindowStates) so a tray-start keeps the
               window hidden. */
        }
        root.syncViewportVisible();
    }

    /* The last NORMAL (windowed) geometry — what is saved, so a
       maximized/minimized close never overwrites it with the
       maximized frame. Sampled after geometry settles: during a
       maximize the size can change before visibility flips. */
    property rect normalGeom: Qt.rect(0, 0, 0, 0)
    Timer {
        id: geomSettle
        interval: 400
        onTriggered: {
            if(root.visibility === Window.Windowed)
                root.normalGeom = Qt.rect(root.x, root.y,
                                          root.width, root.height);
        }
    }
    onXChanged: geomSettle.restart()
    onYChanged: geomSettle.restart()
    onWidthChanged: geomSettle.restart()
    onHeightChanged: geomSettle.restart()

    /* The window can actually be seen: shown AND not minimized
       (root.visible stays true while minimized). */
    readonly property bool windowShown:
        root.visible && root.visibility !== Window.Minimized

    /* studio-import Task 7: the draft preview publishes only
       while a surface that can show it is on screen AND the
       window itself is up — tray-hide or minimize pauses the
       feed (engine reports paused/hidden) just like a page
       switch does. The transport-accepted frame_status feed is
       held back at the bridge under the same condition (only
       the newest is kept, and repainted when shown again). */
    function syncViewportVisible() {
        if(typeof viewport !== "undefined")
            viewport.visible =
                root.windowShown
                && (root.currentView === 1
                    || root.currentView === 2);
        if(typeof bridge !== "undefined")
            bridge.frameFeedActive = root.windowShown;
    }
    onWindowShownChanged: syncViewportVisible()
    /* Geometry + last view are UI-owned — saved on close and
       on view change (write-through; QSettings batches the
       platform write). */
    onCurrentViewChanged: {
        syncViewportVisible();
        if(root.prefsUp)
            uiPrefs.lastView = root.currentView;
    }
    onClosing: function(close) {
        if(root.prefsUp) {
            /* Only a windowed rect is a restorable geometry; a
               maximized close keeps the last normal one and
               records the state separately. */
            if(root.visibility === Window.Windowed)
                root.normalGeom = Qt.rect(root.x, root.y,
                                          root.width, root.height);
            if(root.normalGeom.width > 0 && root.normalGeom.height > 0)
                uiPrefs.saveGeometry(root.normalGeom.x,
                                     root.normalGeom.y,
                                     root.normalGeom.width,
                                     root.normalGeom.height);
            if(root.visibility === Window.Maximized)
                uiPrefs.setWindowMaximized(true);
            else if(root.visibility === Window.Windowed)
                uiPrefs.setWindowMaximized(false);
            /* minimized/hidden: keep the stored flag */
        }
        if(!root.exitRequested
           && typeof trayAvailable !== "undefined"
           && trayAvailable) {
            close.accepted = false;
            root.hide();
        } else if(root.exitRequested) {
            Qt.quit();
        }
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
       the on-screen Save button). In a managed workspace the
       Layout view edits the SAME shared draft, so Ctrl+S
       commits it from there too (audit Q-4); legacy layout
       ops apply immediately and have nothing to save. */
    Shortcut {
        sequence: "Ctrl+S"
        enabled: (root.currentView === 1
                  || (root.currentView === 2 && root.wsManaged))
                 && typeof sceneEdit !== "undefined"
                 && !sceneEdit.saving
        onActivated: lightingPage.commitPendingEdits()
    }
    /* The shared workspace draft (studio-import Task 4):
       when the engine manages a workspace, ONE undo timeline
       covers both editing views — Lighting field edits and
       Layout ops interleave on the same stack. Legacy engines
       keep the layoutCtl-only path. */
    readonly property bool wsManaged:
        typeof workspace !== "undefined"
        && workspace.managed

    /* Ctrl+Z / Ctrl+Shift+Z (+Ctrl+Y) drive the undo
       timeline — the shared workspace one while managed,
       else the legacy Layout-only one; never while a text
       field owns the keys. */
    Shortcut {
        sequence: "Ctrl+Z"
        enabled: !root.textEditingActive()
                 && ((root.wsManaged
                      && (root.currentView === 1
                          || root.currentView === 2)
                      && workspace.canUndo)
                     || (!root.wsManaged
                         && root.currentView === 2
                         && typeof layoutCtl !== "undefined"
                         && layoutCtl.canUndo))
        onActivated: root.shellUndo(false)
    }
    Shortcut {
        sequence: "Ctrl+Shift+Z"
        enabled: !root.textEditingActive()
                 && ((root.wsManaged
                      && (root.currentView === 1
                          || root.currentView === 2)
                      && workspace.canRedo)
                     || (!root.wsManaged
                         && root.currentView === 2
                         && typeof layoutCtl !== "undefined"
                         && layoutCtl.canRedo))
        onActivated: root.shellUndo(true)
    }
    Shortcut {
        sequence: "Ctrl+Y"
        enabled: !root.textEditingActive()
                 && ((root.wsManaged
                      && (root.currentView === 1
                          || root.currentView === 2)
                      && workspace.canRedo)
                     || (!root.wsManaged
                         && root.currentView === 2
                         && typeof layoutCtl !== "undefined"
                         && layoutCtl.canRedo))
        onActivated: root.shellUndo(true)
    }
    /* The one owner of the undo keys (audit Q-2: the 3D viewport
       declared them too, which made them ambiguous). The
       viewport's mid-drag refusal moves here: an undo under a
       live transform gesture would write beneath the snapshot
       the drag previews against, so it is refused and the
       status line says why. */
    function shellUndo(redo) {
        if(root.wsManaged && typeof objectCtl !== "undefined"
           && objectCtl && objectCtl.gestureActive()) {
            objectCtl.setStatusMessage(qsTr("finish the drag first"));
            return;
        }
        if(root.wsManaged)
            redo ? workspace.redo() : workspace.undo();
        else
            redo ? layoutCtl.redo() : layoutCtl.undo();
    }

    /* Workspace notices (foreign commits, import/export
       results) surface as a transient line in the strip. */
    /* R-10: the banner's "How to fix" and the tray notice land
       on the Ownership card. */
    function showOwnershipFix() {
        root.currentView = 3;
        Qt.callLater(devicesPage.showOwnership);
    }

    property string wsNotice: ""
    Connections {
        target: typeof workspace !== "undefined" ? workspace : null
        function onNotice(text) { root.wsNotice = text }
    }
    Timer {
        id: wsNoticeTimer
        interval: 8000
        onTriggered: root.wsNotice = ""
    }
    onWsNoticeChanged: if(wsNotice !== "") wsNoticeTimer.restart()

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

            /* R-10: another program is writing the same lights —
               shown on every page from the engine's pushed
               ownership report; only while connected, when the
               report is current. */
            OwnershipBanner {
                Layout.fillWidth: true
                ownership: typeof bridge !== "undefined"
                           ? bridge.ownership : ({})
                active: root.connState === "connected"
                onFixRequested: root.showOwnershipFix()
            }

            /* Shared-workspace strip (Task 4): one banner for
               the states the shared draft can surface —
               a recovered unsaved draft (offered, NEVER
               auto-applied), an engine-side conflict, a save
               in flight, or a controller notice. Every action
               is an explicit user pick. */
            Rectangle {
                id: wsStrip
                /* Recovery is reachable even when nothing is
                   managed — the missing-engine draft is the
                   case this strip exists for. */
                visible: typeof workspace !== "undefined"
                         && (workspace.recoveryOffered
                             || (root.wsManaged
                                 && (workspace.conflict
                                     || workspace.saving
                                     || (workspace.externalChange
                                         !== undefined
                                         && workspace.externalChange
                                            !== "")
                                     || workspace.replaceOffered
                                         === true))
                             || root.wsNotice !== "")
                Layout.fillWidth: true
                implicitHeight: wsRow.implicitHeight
                                + 2 * Theme.spaceM
                color: (typeof workspace !== "undefined"
                        && workspace.conflict)
                       ? Qt.rgba(0.878, 0.643, 0.345, 0.14)
                       : Theme.surfaceAlt
                Rectangle {
                    anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                    height: 1; color: Theme.border
                }

                Row {
                    id: wsRow
                    anchors {
                        left: parent.left; right: parent.right
                        verticalCenter: parent.verticalCenter
                        leftMargin: Theme.spaceL
                        rightMargin: Theme.spaceL
                    }
                    spacing: Theme.spaceL

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - wsBtns.width
                               - parent.spacing
                        wrapMode: Text.WordWrap
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSizeBody
                        text: {
                            if(typeof workspace === "undefined")
                                return "";
                            if(workspace.recoveryOffered)
                                return workspace.recoveryStale
                                    ? qsTr("An unsaved draft was found for this workspace — it was made against an older revision. Recover it as pending edits or discard it.")
                                    : qsTr("An unsaved draft was found for this workspace. Recover it as pending edits or discard it.");
                            if(root.wsManaged && workspace.replaceOffered)
                                return workspace.replaceText;
                            if(root.wsManaged
                               && workspace.externalChange !== undefined
                               && workspace.externalChange !== "")
                                return workspace.externalDetail !== ""
                                    ? qsTr("Workspace files changed on disk (%1: %2). Nothing was applied — Reload takes the disk copy, or keep working and Save.")
                                          .arg(workspace.externalChange)
                                          .arg(workspace.externalDetail)
                                    : qsTr("Workspace files changed on disk (%1). Nothing was applied — Reload takes the disk copy, or keep working and Save.")
                                          .arg(workspace.externalChange);
                            if(root.wsManaged && workspace.conflict)
                                return qsTr("The workspace changed on the engine under your edits. Save commits your draft on top; Reload takes the engine copy.");
                            if(root.wsManaged && workspace.saving)
                                return qsTr("Saving workspace…");
                            return root.wsNotice;
                        }
                    }
                    Row {
                        id: wsBtns
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: Theme.spaceS
                        AppButton {
                            id: recBtn
                            objectName: "wsRecover"
                            visible: typeof workspace !== "undefined"
                                     && workspace.recoveryOffered
                            text: qsTr("Recover draft")
                            onClicked: workspace.recoverDraft()
                        }
                        AppButton {
                            id: dropBtn
                            objectName: "wsDiscardDraft"
                            visible: typeof workspace !== "undefined"
                                     && workspace.recoveryOffered
                            text: qsTr("Discard")
                            onClicked: workspace.discardDraft()
                        }
                        /* Task 10: a draft-replacing action latched
                           for confirmation — the text on the left
                           carries the source/destination summary. */
                        AppButton {
                            id: confirmBtn
                            objectName: "wsConfirmReplace"
                            visible: root.wsManaged
                                     && workspace.replaceOffered === true
                            text: qsTr("Replace my draft")
                            onClicked: workspace.confirmReplace()
                        }
                        AppButton {
                            id: cancelBtn
                            objectName: "wsCancelReplace"
                            visible: root.wsManaged
                                     && workspace.replaceOffered === true
                            text: qsTr("Keep my draft")
                            onClicked: workspace.cancelReplace()
                        }
                        AppButton {
                            id: reloadDiskBtn
                            objectName: "wsReloadDisk"
                            /* External-change notice only offers the
                               reload while no replace is latched — a
                               dirty draft confirms through the strip
                               above first. */
                            visible: root.wsManaged
                                     && workspace.externalChange
                                        !== undefined
                                     && workspace.externalChange !== ""
                                     && !workspace.replaceOffered
                            text: qsTr("Reload from disk")
                            onClicked: workspace.reload()
                        }
                        AppButton {
                            id: keepBtn
                            objectName: "wsKeepDraft"
                            visible: root.wsManaged
                                     && workspace.conflict
                            text: qsTr("Keep my draft (Save)")
                            /* Same path as Lighting's Save — the
                               focused text field's uncommitted
                               text flushes into the draft before
                               the commit, never silently lost. */
                            onClicked: lightingPage.commitPendingEdits()
                        }
                        AppButton {
                            id: takeBtn
                            objectName: "wsTakeEngine"
                            visible: root.wsManaged
                                     && workspace.conflict
                                     && !workspace.replaceOffered
                            text: qsTr("Take engine copy")
                            onClicked: workspace.reload()
                        }
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
                DevicesPage { id: devicesPage }
                SettingsPage {}
            }
        }
    }
}
