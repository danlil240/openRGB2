// SPDX-License-Identifier: GPL-2.0-or-later
//
// FrameStrip — one controller's delivered colors as a compact
// strip of swatches. `colors` is the wire array ("#RRGGBB")
// from frame_status — the LAST TRANSPORT-ACCEPTED frame, i.e.
// the same sampled colors output uses (protocol §7, plan
// section 5 preview parity). Downsampling is for display only.
import QtQuick
import OpenRGB2.UI.theme

Item {
    id: root

    /* "#RRGGBB" array straight from the wire. */
    property var colors: []
    property int maxSwatches: 64

    implicitWidth:  row.implicitWidth
    implicitHeight: 16

    function sampled() {
        const c = root.colors || [];
        if(c.length === 0)
            return [];
        if(c.length <= root.maxSwatches)
            return c;
        const step = c.length / root.maxSwatches;
        const out = [];
        for(let i = 0; i < root.maxSwatches; ++i)
            out.push(c[Math.floor(i * step)]);
        return out;
    }

    Row {
        id: row
        spacing: 1
        Repeater {
            model: root.sampled()
            Rectangle {
                width: 10; height: 16
                color: modelData || "black"
            }
        }
    }
}
