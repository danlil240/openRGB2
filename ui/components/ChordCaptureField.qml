// SPDX-License-Identifier: GPL-2.0-or-later
//
// ChordCaptureField — R-2: reads one key combination from a
// local key event and turns it into the engine's chord text
// ("Ctrl+Alt+Shift+PgDn": modifiers in Ctrl, Alt, Shift, Win
// order, then the key name the engine's parser knows). Only
// the string leaves this field; the engine validates and
// registers it (RegisterHotKey) — the UI never installs a hook.
//
//   click / Tab in   -> capturing (activeFocus)
//   a chord          -> captured(chord), lastCaptured = chord
//   a bare key       -> refused in place (needs Ctrl, Alt or Win)
//   Backspace/Delete -> cleared()
//   Esc              -> leave without a change
import QtQuick
import OpenRGB2.UI.theme

Rectangle {
    id: root

    /* The bound chord (engine truth) and a suggestion shown
       while unbound. */
    property string chord: ""
    property string placeholder: ""
    /* The last chord this field read (QA/tests observe it). */
    property string lastCaptured: ""
    /* Transient guidance while capturing. */
    property string hint: ""
    readonly property bool capturing: activeFocus

    signal captured(string chord)
    signal cleared()

    implicitWidth: 220
    implicitHeight: Theme.controlHeight
    radius: Theme.radiusS
    color: root.enabled ? Theme.surfaceAlt : Theme.surface
    border.width: root.activeFocus ? 2 : 1
    border.color: root.activeFocus ? Theme.accent : Theme.borderStrong
    activeFocusOnTab: true

    Accessible.role: Accessible.EditableText
    Accessible.name: root.chord !== ""
        ? qsTr("Hotkey %1 — press a new combination to change it").arg(root.chord)
        : qsTr("No hotkey — press a key combination to bind one")

    onActiveFocusChanged: if(!activeFocus) root.hint = ""

    Text {
        anchors.fill: parent
        anchors.leftMargin: Theme.spaceS
        anchors.rightMargin: Theme.spaceS
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        font.pixelSize: Theme.fontSizeBody
        color: root.capturing ? Theme.accent
             : root.chord !== "" ? (root.enabled ? Theme.textPrimary
                                                 : Theme.textDisabled)
             : Theme.textDisabled
        text: root.capturing
              ? (root.hint !== "" ? root.hint
                                  : qsTr("Press a key combination…"))
              : root.chord !== "" ? root.chord
              : root.placeholder !== ""
                ? qsTr("not bound (suggested: %1)").arg(root.placeholder)
                : qsTr("not bound")
    }

    MouseArea {
        anchors.fill: parent
        enabled: root.enabled
        onClicked: root.forceActiveFocus()
    }

    function modifierText(mods) {
        let s = "";
        if(mods & Qt.ControlModifier) s += "Ctrl+";
        if(mods & Qt.AltModifier)     s += "Alt+";
        if(mods & Qt.ShiftModifier)   s += "Shift+";
        if(mods & Qt.MetaModifier)    s += "Win+";
        return s;
    }

    /* Qt key -> the engine's canonical key name ("" = none). */
    function keyName(k, mods) {
        const pad = (mods & Qt.KeypadModifier) !== 0;
        if(k >= Qt.Key_0 && k <= Qt.Key_9)
            return (pad ? "Num" : "") + String.fromCharCode(k);
        if(k >= Qt.Key_A && k <= Qt.Key_Z)
            return String.fromCharCode(k);
        if(k >= Qt.Key_F1 && k <= Qt.Key_F24)
            return "F" + (k - Qt.Key_F1 + 1);
        switch(k) {
        case Qt.Key_PageUp:    return "PgUp";
        case Qt.Key_PageDown:  return "PgDn";
        case Qt.Key_Home:      return "Home";
        case Qt.Key_End:       return "End";
        case Qt.Key_Insert:    return "Insert";
        case Qt.Key_Delete:    return "Delete";
        case Qt.Key_Up:        return "Up";
        case Qt.Key_Down:      return "Down";
        case Qt.Key_Left:      return "Left";
        case Qt.Key_Right:     return "Right";
        case Qt.Key_Space:     return "Space";
        case Qt.Key_Tab:       return "Tab";
        case Qt.Key_Backtab:   return "Tab";
        case Qt.Key_Return:
        case Qt.Key_Enter:     return "Enter";
        case Qt.Key_Backspace: return "Backspace";
        case Qt.Key_Escape:    return "Esc";
        case Qt.Key_Pause:     return "Pause";
        case Qt.Key_Print:     return "PrintScreen";
        case Qt.Key_ScrollLock:return "ScrollLock";
        case Qt.Key_Plus:      return pad ? "NumAdd" : "Plus";
        case Qt.Key_Equal:     return "Plus";
        case Qt.Key_Minus:     return pad ? "NumSub" : "Minus";
        case Qt.Key_Asterisk:  return pad ? "NumMul" : "";
        case Qt.Key_Slash:     return pad ? "NumDiv" : "Slash";
        case Qt.Key_Period:    return pad ? "NumDecimal" : "Period";
        case Qt.Key_Comma:     return "Comma";
        case Qt.Key_Semicolon: return "Semicolon";
        case Qt.Key_Apostrophe:return "Quote";
        case Qt.Key_BracketLeft:  return "LeftBracket";
        case Qt.Key_BracketRight: return "RightBracket";
        case Qt.Key_Backslash: return "Backslash";
        case Qt.Key_QuoteLeft: return "Backquote";
        case Qt.Key_VolumeMute:return "VolumeMute";
        case Qt.Key_VolumeDown:return "VolumeDown";
        case Qt.Key_VolumeUp:  return "VolumeUp";
        case Qt.Key_MediaNext: return "MediaNext";
        case Qt.Key_MediaPrevious: return "MediaPrev";
        case Qt.Key_MediaStop: return "MediaStop";
        case Qt.Key_MediaPlay:
        case Qt.Key_MediaTogglePlayPause: return "MediaPlayPause";
        }
        return "";
    }

    /* Window shortcuts (Ctrl+1..5, Ctrl+S) must not steal the
       chord while capturing. */
    Keys.onShortcutOverride: (event) => { event.accepted = true; }

    Keys.onPressed: (event) => {
        event.accepted = true;
        const k = event.key;
        const mods = event.modifiers;
        if(k === Qt.Key_Control || k === Qt.Key_Shift ||
           k === Qt.Key_Alt || k === Qt.Key_Meta ||
           k === Qt.Key_AltGr || k === Qt.Key_Super_L ||
           k === Qt.Key_Super_R) {
            root.hint = root.modifierText(mods) + "…";
            return;
        }
        const plain = (mods & (Qt.ControlModifier | Qt.AltModifier |
                               Qt.ShiftModifier | Qt.MetaModifier)) === 0;
        if(plain && k === Qt.Key_Escape) {
            root.focus = false;
            return;
        }
        if(plain && (k === Qt.Key_Backspace || k === Qt.Key_Delete)) {
            root.cleared();
            root.focus = false;
            return;
        }
        if((mods & (Qt.ControlModifier | Qt.AltModifier |
                    Qt.MetaModifier)) === 0) {
            root.hint = qsTr("Add Ctrl, Alt or Win — a bare key would fire while typing");
            return;
        }
        const name = root.keyName(k, mods);
        if(name === "") {
            root.hint = qsTr("That key cannot be used for a hotkey");
            return;
        }
        const c = root.modifierText(mods) + name;
        root.lastCaptured = c;
        root.hint = "";
        root.captured(c);
        root.focus = false;
    }
}
