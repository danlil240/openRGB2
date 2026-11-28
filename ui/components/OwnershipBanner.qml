// SPDX-License-Identifier: GPL-2.0-or-later
//
// OwnershipBanner — R-10: the shell-wide warning that another
// program is writing the same RGB hardware (G Hub, the LampArray
// service, Windows Dynamic Lighting, ...). It shows on every page,
// driven by the engine's pushed OWNERSHIP report, and offers one
// action: open the steps to turn that writer off. Report-only, like
// the rest of the ownership surface: nothing here stops a process,
// a service or a setting.
//
// Flap guard: a writer that restarts often (the LampArray service)
// would make the banner blink, so a clear is held for `holdMs`
// before the banner goes; a conflict that returns inside the hold
// keeps it up without a gap.
import QtQuick
import QtQuick.Layouts
import OpenRGB2.UI.theme

Rectangle {
    id: root
    objectName: "ownershipBanner"

    /* The OWNERSHIP body (bridge.ownership). */
    property var ownership: ({})
    /* false hides the banner outright — e.g. while the engine is
       not connected, when the report may be stale. */
    property bool active: true
    property int holdMs: 10000

    signal fixRequested()

    readonly property var conflicts:
        (root.ownership && root.ownership.conflicts)
            ? root.ownership.conflicts : []
    /* What the banner names: the live conflicts, or the last ones
       while a clear is held. */
    property var heldConflicts: []
    readonly property bool shown:
        root.active && root.heldConflicts.length > 0

    function ownerNames(list) {
        const seen = [];
        for(let i = 0; i < list.length; ++i) {
            const o = list[i].owner || qsTr("unknown app");
            if(seen.indexOf(o) < 0)
                seen.push(o);
        }
        return seen;
    }

    function sync() {
        if(root.conflicts.length > 0) {
            holdTimer.stop();
            root.heldConflicts = root.conflicts;
        } else if(root.heldConflicts.length > 0) {
            if(root.holdMs <= 0) {
                holdTimer.stop();
                root.heldConflicts = [];
            } else if(!holdTimer.running) {
                holdTimer.interval = root.holdMs;
                holdTimer.start();
            }
        }
    }
    onConflictsChanged: sync()
    Component.onCompleted: sync()

    Timer {
        id: holdTimer
        repeat: false
        onTriggered: {
            if(root.conflicts.length === 0)
                root.heldConflicts = [];
        }
    }

    visible: root.shown
    implicitHeight: row.implicitHeight + 2 * Theme.spaceM
    color: Theme.surfaceAlt
    Rectangle {
        anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
        width: 3
        color: Theme.warn
    }
    Rectangle {
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: 1
        color: Theme.border
    }

    RowLayout {
        id: row
        anchors {
            left: parent.left; right: parent.right
            verticalCenter: parent.verticalCenter
            leftMargin: Theme.spaceL; rightMargin: Theme.spaceL
        }
        spacing: Theme.spaceL

        Text {
            objectName: "ownershipBannerText"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Another app is controlling your lights: %1")
                .arg(root.ownerNames(root.heldConflicts).join(", "))
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSizeBody
        }
        AppButton {
            objectName: "ownershipFixButton"
            Accessible.name: qsTr("Show how to turn off the competing app")
            text: qsTr("How to fix")
            onClicked: root.fixRequested()
        }
    }
}
