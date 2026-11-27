// SPDX-License-Identifier: GPL-2.0-or-later
//
// AppSlider — themed slider (groove + ICE handle/fill).
import QtQuick
import QtQuick.Controls
import OpenRGB2.UI.theme

Slider {
    id: root

    implicitHeight: Theme.controlHeight

    /* Commit-on-release support. A gesture is one pressed..
       released span — a pointer drag, or an arrow/Page key
       press (QQuickSlider sets `pressed` while a step key is
       down, so keyboard steps commit on key RELEASE).
       gestureCommitted(v) fires only when the gesture ends
       while the slider is still enabled and was not cancelled
       (disabled or hidden during the gesture); `v` is the last value the USER produced
       (captured while pressed), so a model Binding that
       re-asserts itself on release cannot swap in the old
       value. Disabling/hiding the slider mid-gesture (which
       drops Qt's mouse grab and clears `pressed` without a
       release) fires gestureCancelled() instead. Pages pair
       this with `Binding { when: !slider.pressed;
       restoreMode: Binding.RestoreNone }` so the handle is not
       snapped back mid-drag, and set `modelValue` so a
       cancelled gesture reverts the handle to the model. */
    /* Optional: the value the model holds. When set, a
       cancelled gesture snaps the handle back to it
       (a `when`-gated Binding does not re-write an unchanged
       value on reactivation). */
    property var modelValue: undefined
    property bool gestureActive: false
    property real gestureValue: value
    property bool _gestureCancelled: false
    signal gestureCommitted(real value)
    signal gestureCancelled()

    onPressedChanged: {
        if(pressed)
        {
            gestureActive = true;
            _gestureCancelled = false;
            gestureValue = value;
            return;
        }
        if(!gestureActive)
            return;
        gestureActive = false;
        /* Hidden mid-gesture is caught by onVisibleChanged (a
           slider that was never on screen can still be driven
           programmatically, e.g. by tests/QA). */
        if(!_gestureCancelled && enabled)
            gestureCommitted(gestureValue);
        else
        {
            if(modelValue !== undefined)
                value = modelValue;
            gestureCancelled();
        }
    }
    onValueChanged: if(pressed) gestureValue = value
    onEnabledChanged: if(!enabled && gestureActive) _gestureCancelled = true
    onVisibleChanged: if(!visible && gestureActive) _gestureCancelled = true

    background: Rectangle {
        x: root.leftPadding
        y: root.topPadding + root.availableHeight / 2 - height / 2
        width: root.availableWidth
        height: 4
        radius: 2
        color: Theme.surfaceAlt

        Rectangle {
            width: root.visualPosition * parent.width
            height: parent.height
            radius: 2
            color: root.enabled ? Theme.accent : Theme.textDisabled
        }
    }

    handle: Rectangle {
        x: root.leftPadding + root.visualPosition
           * (root.availableWidth - width)
        y: root.topPadding + root.availableHeight / 2 - height / 2
        width: 16; height: 16; radius: 8
        color: root.enabled ? Theme.accent : Theme.textDisabled
        border.width: root.activeFocus ? 2 : 0
        border.color: Theme.accentStrong
    }
}
