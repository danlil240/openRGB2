// SPDX-License-Identifier: GPL-2.0-or-later
//
// PageFlickable — the scroll surface every page sits in (audit
// Q-12). A plain Flickable gave no visible sign that a page
// scrolls and never followed keyboard focus: Tab could land on a
// control below the fold and the user saw nothing move. This one
// shows a vertical scroll bar when the content overflows and
// scrolls the newly focused control into view.
import QtQuick
import QtQuick.Controls
import OpenRGB2.UI.theme

Flickable {
    id: root

    clip: true
    boundsBehavior: Flickable.StopAtBounds

    ScrollBar.vertical: ScrollBar {
        objectName: "pageScrollBar"
        policy: ScrollBar.AsNeeded
    }

    /* Scroll the minimum needed to show `item` (with a margin);
       an item taller than the view aligns its top. Items outside
       this page (another page, a popup) are ignored. */
    function ensureVisible(item) {
        if(!item || !root.visible || root.height <= 0)
            return;
        let p = item;
        while(p && p !== root.contentItem)
            p = p.parent;
        if(!p)
            return;
        const pos = item.mapToItem(root.contentItem, 0, 0);
        const top = pos.y - Theme.spaceL;
        const bottom = pos.y + item.height + Theme.spaceL;
        const maxY = Math.max(0, root.contentHeight - root.height);
        let y = root.contentY;
        if(bottom - top > root.height || top < y)
            y = top;
        else if(bottom > y + root.height)
            y = bottom - root.height;
        y = Math.max(0, Math.min(maxY, y));
        if(y !== root.contentY)
            root.contentY = y;
    }

    Connections {
        target: root.Window.window
        function onActiveFocusItemChanged() {
            root.ensureVisible(root.Window.window.activeFocusItem);
        }
    }
}
