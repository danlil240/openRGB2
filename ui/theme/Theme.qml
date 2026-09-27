// SPDX-License-Identifier: GPL-2.0-or-later
//
// Theme — the shared design tokens for the OpenRGB2 UI (plan
// section 5): neutral dark surfaces, an ICE/white accent, clear
// typography, generous spacing, restrained animation. Every
// component and page reads its values from here — no hex
// literals outside this file.
pragma Singleton

import QtQuick

QtObject {
    /*---- Surfaces (neutral dark) ---------------------------*/
    readonly property color bg:            "#0C1017"
    readonly property color surface:       "#141B25"
    readonly property color surfaceAlt:    "#1A2330"
    readonly property color surfaceHover:  "#212D3E"
    readonly property color border:        "#2A3646"
    readonly property color borderStrong:  "#3B4C61"

    /*---- Text ----------------------------------------------*/
    readonly property color textPrimary:   "#E9EEF4"
    readonly property color textSecondary: "#9AA8B8"
    readonly property color textDisabled:  "#5B6774"

    /*---- Accent — ICE (ice-blue / white) --------------------*/
    readonly property color accent:        "#7FCFFF"
    readonly property color accentStrong:  "#BDE6FF"
    readonly property color accentMuted:   "#3D5E78"
    readonly property color accentText:    "#06121D"

    /*---- Status ---------------------------------------------*/
    readonly property color ok:            "#4CC38A"
    readonly property color warn:          "#E0A458"
    readonly property color bad:           "#E5534B"
    readonly property color neutral:       "#8494A5"

    /*---- Lighting palette ------------------------------------
       The scene-palette quick picks offered next to the HSV
       controls on the Lighting page. These are design tokens
       like everything else here — the ONLY hex literals in
       the module outside this file are wire colors arriving
       from the engine ("#RRGGBB" strings). */
    readonly property var paletteSwatches: [
        "#7FCFFF", "#BDE6FF", "#F2F5F8", "#4CC38A",
        "#E0A458", "#E5534B", "#9B6BD3", "#1E6FD9"
    ]

    /*---- Typography -----------------------------------------*/
    readonly property int fontSizeSmall:   12
    readonly property int fontSizeBody:    14
    readonly property int fontSizeTitle:   18
    readonly property int fontSizeDisplay: 24

    /*---- Spacing scale (generous) ---------------------------*/
    readonly property int spaceXS:  4
    readonly property int spaceS:   8
    readonly property int spaceM:   12
    readonly property int spaceL:   16
    readonly property int spaceXL:  24
    readonly property int spaceXXL: 32

    /*---- Radii ----------------------------------------------*/
    readonly property int radiusS: 6
    readonly property int radiusM: 10
    readonly property int radiusL: 14

    /*---- Restrained animation --------------------------------*/
    readonly property int animFast:   120
    readonly property int animNormal: 220

    /*---- Component sizing ------------------------------------*/
    readonly property int navWidth:       220
    readonly property int controlHeight:  34

    /* Tone name -> color, shared by pills/dots/labels. */
    function toneColor(tone) {
        switch(tone) {
        case "ok":      return ok;
        case "warn":    return warn;
        case "bad":     return bad;
        case "accent":  return accent;
        default:        return neutral;
        }
    }
}
