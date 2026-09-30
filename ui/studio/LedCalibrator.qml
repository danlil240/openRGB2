/*---------------------------------------------------------*\
|||  LedCalibrator.qml (studio-import Task 11)            ||
|||                                                       ||
|||  Adapted from the preserved source plugin's           ||
|||  ui/components/LedCalibrator.qml (provenance ledger): ||
|||  a modeless overlay inside the device preset editor.  ||
|||  For each hardware LED in the bound zone it lights    ||
|||  exactly one physical LED; the user clicks the dot at ||
|||  the position where it lit. The result lands in the   ||
|||  candidate zone as a `points` layout with explicit    ||
|||  `addresses` — Apply writes the CANDIDATE ONLY; the   ||
|||  editor's normal Save persists the *.device.json.     ||
|||                                                       ||
|||  Hardware side rides the engine-owned session via     ||
|||  the `calibCtl` context property (CalibrationController||
|||  calibBegin/calibShow/calibEnd/calibInfo + diag        ||
|||  pickers — WORKSPACE_COMMAND calibration_* verbs):    ||
|||  the whole wireless group is held for the session so  ||
|||  the zone stays dark between steps.                   ||
|||                                                       ||
|||  Modes:                                               ||
|||    assign  session: click unassigned dot = lit LED;   ||
|||      click mapped dot selects it for the LED-# box    ||
|||    place   click canvas -> new point at the click     ||
|||    verify  click a dot  -> light its mapped LED       ||
|||                                                       ||
|||  Projection: the two widest-spread axes of the zone's ||
|||  points become the canvas (for a fan face that's the  ||
|||  X/Z plane — Y is constant). Flip buttons fix mirror  ||
|||  mismatches between the on-screen face and the        ||
|||  physical device.                                     ||
\*---------------------------------------------------------*/
import QtQuick
import QtQuick.Controls.Basic
import OpenRGB2.UI.theme
import OpenRGB2.UI.components

Rectangle {
    id: cal
    visible: false
    color: Theme.surface
    radius: Theme.radiusL
    border.color: Theme.borderStrong
    z: 60

    /* ---------------- seams ---------------- */
    property var editorRef: null        /* DevicePresetEditor      */
    property var hostOverride: null
    function host() {
        if (hostOverride !== null)
            return hostOverride
        try { return calibCtl } catch (e) { return null }
    }
    function hasHost() { return host() !== null }
    function hostQObj() {
        var h = host()
        return (h !== null && h.objectName !== undefined) ? h : null
    }

    /* ---------------- session / mapping state ---------------- */
    property int    zoneIndex: -1
    property string zoneId: ""
    property bool   sessionActive:   false
    property bool   sessionStarting: false
    property int    hwLeds: 0           /* session LED count       */
    property int    hwZoneLeds: 0       /* picked zone's LED count */
    property int    curLed: 0           /* currently lit hw index  */
    property var    pts: []             /* working [[x,y,z], ...]  */
    property var    assign: []          /* hw addr per point, -1   */
    property bool   verify:  false      /* click dot -> light it   */
    property bool   place:   false      /* click canvas -> new pt  */
    property bool   blinkOn: false
    property bool   chaseOn: false
    property bool   showMore: false     /* advanced controls      */
    property int    chaseLed: 0
    property bool   flipU: false
    property bool   flipV: false
    property int    projU: 0            /* axis -> canvas X        */
    property int    projV: 2            /* axis -> canvas Y (up)   */
    property real   faceVal: 0          /* dropped-axis fill value */
    property int    calibStamp: 0       /* bump => repaint         */
    property string statusLine: ""
    property int    selDot: -1          /* clicked-for-edit dot    */
    /* Native pause held without a session visible — an end
       worker's restore is draining; Start stays gated until
       calibInfo().paused clears. */
    property bool   hwPaused: false
    /* Armed by the first Make click on a non-empty layout — the
       second click confirms the destructive regen. */
    property bool   confirmMake: false
    /* Working copy differs from the saved zone layout — Apply
       keeps it, reopening discards it. */
    property bool   layoutDirty: false
    property real   startDeadline: 0    /* Date.now() cutoff       */
    readonly property int startTimeoutMs: 10000
    /* LED-# box — alias so tests (and Keys handlers) reach the
       field without knowing its id. */
    property alias  ledText: ledField.text
    /* Shape combo index — same test seam (0 = "saved"). */
    property alias  shapeIndex: shapeBox.currentIndex

    /* ================= data plumbing ================= */
    function ed() { return editorRef }
    function selZone() {
        var e = ed()
        if (e)
            e.stamp      /* re-derive when the candidate mutates */
        return (e && e.candidate && zoneIndex >= 0
                && zoneIndex < e.candidate.zones.length)
             ? e.candidate.zones[zoneIndex] : null
    }
    function zoneLayout() {
        var z = selZone()
        return (z && z.layout) || {}
    }

    /* Load the zone's points+addresses into the working copy.
       Missing/short addresses default to the identity mapping —
       that matches the generator's addr_base+i fallback. */
    function loadFromZone() {
        var l = zoneLayout()
        /* Build locally, then assign once — push() on a property's
           array never notifies, which left the Repeater empty. */
        var np = [], na = []
        if (l.type === "points") {
            var src = l.points || []
            var ad  = l.addresses || []
            for (var i = 0; i < src.length; i++) {
                var p = src[i] || [0, 0, 0]
                np.push([p[0] || 0, p[1] || 0, p[2] || 0])
                na.push(i < ad.length ? ad[i] : i)
            }
        }
        pts = np
        assign = na
        pickProjection()
        layoutDirty = false
        calibStamp++
    }

    function openForZone(editor, zi) {
        endSession()                 /* don't leave hw in calib mode */
        editorRef = editor
        zoneIndex = zi
        /* Reflect the pick back at the editor — the candidate-
           zone combo and any future editor selection read the
           same index (writing it is what fires the follow-
           selection Connections for OTHER callers). */
        if (editor && editor.selZoneIdx !== undefined)
            editor.selZoneIdx = zi
        loadedCand = editor ? editor.candidate : null
        panelOpen = true
        var z = selZone()
        zoneId = z ? (z.id || "") : ""
        sessionActive = false
        sessionStarting = false
        verify = false
        place  = false
        blinkOn = false
        chaseOn = false
        confirmMake = false
        curLed = 0
        selDot = -1
        ledField.text = ""
        statusLine = ""
        /* The canvas reloads the SAVED layout — the selector must
           describe it, not the last generated candidate (a stale
           pick made Make destroy the visible shape). */
        shapeBox.currentIndex = 0
        loadFromZone()
        countField.text = String(pts.length > 0 ? pts.length
                                                : defaultCount())
        if (pts.length === 0)
            statusLine = "no saved points — pick a shape + Make, "
                       + "or sketch with place mode"
        refreshCandZones()
        refreshHw()
        visible = true
    }

    function close() {
        endSession()
        panelOpen = false
        visible = false
    }

    /* Follow the editor's selection — switching to another device
       preset or another zone while the panel is open reloads that
       zone's shape and count. Candidate identity changes on
       loadDoc (new device); selZoneIdx on the zone list click.
       Same-zone field edits only bump stamp — deliberately not
       followed, so a mapping in progress isn't wiped. Note: the
       open state tracks panelOpen, not visible — Item.visible is
       the EFFECTIVE flag (false while a parent is hidden). */
    property bool panelOpen: false
    property var  loadedCand: null
    Connections {
        target: cal.editorRef
        enabled: cal.panelOpen
        function onSelZoneIdxChanged() {
            var e = cal.editorRef
            if (e && e.selZoneIdx !== cal.zoneIndex)
                cal.openForZone(e, e.selZoneIdx)
        }
        function onCandidateChanged() {
            var e = cal.editorRef
            if (e && e.candidate !== cal.loadedCand)
                cal.openForZone(e, e.selZoneIdx !== undefined
                               ? e.selZoneIdx : cal.zoneIndex)
        }
    }
    /* Keyboard entry — digits append to the LED-# field, Enter
       commits it to the selected dot (empty Enter steps to the
       next LED). Del clears the selected dot's number, ←/→ step
       the lit LED, Esc peels field → selection → panel. */
    Keys.onPressed: function(e) {
        if (e.key >= Qt.Key_0 && e.key <= Qt.Key_9) {
            ledField.text += String(e.key - Qt.Key_0)
            e.accepted = true
        } else if (e.key === Qt.Key_Backspace) {
            if (ledField.text.length > 0) {
                ledField.text = ledField.text.slice(0, -1)
                e.accepted = true
            }
        } else if (e.key === Qt.Key_Return
                   || e.key === Qt.Key_Enter) {
            commitField()
            e.accepted = true
        } else if (e.key === Qt.Key_Delete) {
            if (selDot >= 0) {
                var d = selDot
                unassign(d)
                statusLine = "position " + d + " cleared"
                e.accepted = true
            }
        } else if (e.key === Qt.Key_Left) {
            stepLed(-1)
            e.accepted = true
        } else if (e.key === Qt.Key_Right) {
            stepLed(1)
            e.accepted = true
        }
    }
    Keys.onEscapePressed: function(e) {
        if (ledField.text.length > 0) {
            ledField.text = ""
            e.accepted = true
        } else if (selDot >= 0) {
            selDot = -1
            calibStamp++
            e.accepted = true
        } else if (visible) {
            cal.close()
            e.accepted = true
        }
    }
    onVisibleChanged: {
        if (visible)
            forceActiveFocus()
        else
            endSession()
    }

    /* ================= projection ================= */
    /* Drop the axis with the least spread — a fan face's points
       all share one coordinate (its thickness axis). */
    function pickProjection() {
        var mn = [1e18, 1e18, 1e18], mx = [-1e18, -1e18, -1e18]
        for (var i = 0; i < pts.length; i++) {
            for (var a = 0; a < 3; a++) {
                if (pts[i][a] < mn[a]) mn[a] = pts[i][a]
                if (pts[i][a] > mx[a]) mx[a] = pts[i][a]
            }
        }
        var drop = 1
        if (pts.length > 0) {
            var s0 = mx[0]-mn[0], s1 = mx[1]-mn[1], s2 = mx[2]-mn[2]
            drop = (s0 <= s1 && s0 <= s2) ? 0 : (s1 <= s2 ? 1 : 2)
        }
        var rest = []
        for (var b = 0; b < 3; b++)
            if (b !== drop)
                rest.push(b)
        projU = rest[0]
        projV = rest[1]
        /* Fill value for placed points: the median of the dropped
           axis keeps new emitters on the same face plane. */
        if (pts.length > 0) {
            var vals = []
            for (var j = 0; j < pts.length; j++)
                vals.push(pts[j][drop])
            vals.sort(function(a, b) { return a - b })
            faceVal = vals[Math.floor(vals.length / 2)]
        } else {
            faceVal = 0
        }
    }

    function uvBounds() {
        var mn = [1e18, 1e18], mx = [-1e18, -1e18]
        for (var i = 0; i < pts.length; i++) {
            var u = pts[i][projU] * (flipU ? -1 : 1)
            var v = pts[i][projV] * (flipV ? -1 : 1)
            if (u < mn[0]) mn[0] = u
            if (u > mx[0]) mx[0] = u
            if (v < mn[1]) mn[1] = v
            if (v > mx[1]) mx[1] = v
        }
        if (mn[0] > mx[0]) { mn = [-0.05, -0.05]; mx = [0.05, 0.05] }
        if (mx[0] - mn[0] < 1e-6) { mn[0] -= 0.01; mx[0] += 0.01 }
        if (mx[1] - mn[1] < 1e-6) { mn[1] -= 0.01; mx[1] += 0.01 }
        return { mn: mn, mx: mx }
    }
    function viewScale(w, h, b) {
        var pad = 30
        return Math.min((w - 2 * pad) / (b.mx[0] - b.mn[0]),
                        (h - 2 * pad) / (b.mx[1] - b.mn[1]))
    }
    /* Point index -> canvas pixel position. */
    function dotPos(i, w, h) {
        var b = uvBounds()
        var s = viewScale(w, h, b)
        var cu = (b.mn[0] + b.mx[0]) / 2, cv = (b.mn[1] + b.mx[1]) / 2
        var u = pts[i][projU] * (flipU ? -1 : 1)
        var v = pts[i][projV] * (flipV ? -1 : 1)
        return { x: w / 2 + (u - cu) * s, y: h / 2 - (v - cv) * s }
    }
    /* Canvas pixel -> 3D local point (dropped axis = faceVal). */
    function screenToPt(sx, sy, w, h) {
        var b = uvBounds()
        var s = viewScale(w, h, b)
        var cu = (b.mn[0] + b.mx[0]) / 2, cv = (b.mn[1] + b.mx[1]) / 2
        var u = (sx - w / 2) / s + cu
        var v = -(sy - h / 2) / s + cv
        var p = [0, 0, 0]
        p[projU] = u * (flipU ? -1 : 1)
        p[projV] = v * (flipV ? -1 : 1)
        p[3 - projU - projV] = faceVal
        return p
    }
    /* Screen position of the entity origin (axis crosshair). */
    function originPos(w, h) {
        var b = uvBounds()
        var s = viewScale(w, h, b)
        var cu = (b.mn[0] + b.mx[0]) / 2, cv = (b.mn[1] + b.mx[1]) / 2
        return { x: w / 2 - cu * s, y: h / 2 + cv * s }
    }
    /* Bounding circle around the entity origin — fan-face outline. */
    function boundCircle(w, h) {
        var b = uvBounds()
        var s = viewScale(w, h, b)
        var r = 0
        for (var i = 0; i < pts.length; i++) {
            var u = pts[i][projU] * (flipU ? -1 : 1)
            var v = pts[i][projV] * (flipV ? -1 : 1)
            var d = Math.sqrt(u * u + v * v)
            if (d > r)
                r = d
        }
        var o = originPos(w, h)
        return { x: o.x, y: o.y, r: (r * 1.08 + 0.004) * s }
    }

    /* ================= mapping ops ================= */
    /* Valid hardware LED range for typed/assigned numbers: the
       live session's count first, then the picked zone's count.
       0 = unknown — no upper bound is enforced (the file layer
       can't know the hardware; resolve bounds it when bound). */
    function ledBound() {
        if (sessionActive && hwLeds > 0)
            return hwLeds
        if (hwZoneLeds > 0)
            return hwZoneLeds
        return 0
    }
    /* Assigned dots whose LED is outside the known bound — only
       reachable through a stale/edited preset file, since
       setLedAt refuses them. */
    function invalidAssigned() {
        var b = ledBound(), n = 0
        if (b <= 0)
            return 0
        for (var i = 0; i < assign.length; i++)
            if (assign[i] >= b)
                n++
        return n
    }
    function assignedCount() {
        var n = 0
        for (var i = 0; i < assign.length; i++)
            if (assign[i] >= 0)
                n++
        return n
    }
    function ledUsed(k) { return assign.indexOf(k) >= 0 }
    function nextFree(from) {
        for (var k = from + 1; k < hwLeds; k++)
            if (!ledUsed(k))
                return k
        return -1
    }
    function showCurrent() {
        if (sessionActive && hasHost()
            && typeof host().calibShow === "function")
            host().calibShow(curLed)
    }
    function advance() {
        var n = nextFree(curLed)
        if (n < 0)
            n = nextFree(-1)        /* wrap to the first gap */
        if (n < 0) {
            statusLine = "all " + hwLeds + " LEDs mapped — "
                       + "verify or Apply"
            return
        }
        curLed = n
        if (!chaseOn)
            showCurrent()
    }
    /* Left-click a dot. Fast path during a session: an UNASSIGNED
       dot takes the lit LED in one click and advances — that's the
       main loop. Clicking a dot that already has a number (or any
       dot with no session) selects it so the LED-# box can fix it;
       its mapped LED flashes on the hardware so you can see what
       it points at. Verify mode keeps its own behaviour. */
    function assignDot(i) {
        if (i < 0 || i >= pts.length)
            return
        if (verify) {
            /* Unassigned dots light nothing — calibShow(-1) is
               "all dark" natively, so sending it while claiming a
               lit LED is the contradictory state the E2E hit. */
            if (assign[i] < 0) {
                statusLine = "position " + i + " is unassigned — "
                           + "no LED to light"
                return
            }
            if (sessionActive && hasHost()
                && typeof host().calibShow === "function")
                host().calibShow(assign[i])
            return
        }
        if (sessionActive && assign[i] < 0) {
            if (setLedAt(i, curLed))
                advance()
            ledField.text = ""
            return
        }
        selDot = i
        ledField.text = assign[i] >= 0 ? String(assign[i])
                      : sessionActive ? String(curLed)
                      : ""
        ledField.forceActiveFocus()
        ledField.selectAll()
        calibStamp++
        statusLine = "position " + i + " selected — LED number "
                   + "goes in the box, Enter commits"
        /* Show which physical LED this dot points at right now. */
        if (sessionActive && assign[i] >= 0
            && hasHost() && typeof host().calibShow === "function")
            host().calibShow(assign[i])
    }
    /* Force dot i's mapping to a typed LED number. Steal semantics:
       the number can live on only one position — any other dot
       holding it is cleared. Values past the zone's real LED count
       are refused: they can never map to hardware and persisted
       exactly that kind of impossible mapping in the E2E. Returns
       false on rejection so callers keep the edit state. */
    function setLedAt(i, led) {
        if (i < 0 || i >= assign.length || led < 0)
            return false
        var bound = ledBound()
        if (bound > 0 && led >= bound) {
            statusLine = "LED " + led + " is out of range — the zone "
                       + "has " + bound + " LEDs (0.." + (bound - 1) + ")"
            return false
        }
        for (var k = 0; k < assign.length; k++)
            if (k !== i && assign[k] === led)
                assign[k] = -1
        assign[i] = led
        layoutDirty = true
        calibStamp++
        statusLine = "position " + i + " ← LED " + led
        return true
    }
    /* Remove a point outright — for mis-clicks in place mode. */
    function removeDot(i) {
        if (i < 0 || i >= pts.length)
            return
        pts = pts.slice(0, i).concat(pts.slice(i + 1))
        assign = assign.slice(0, i).concat(assign.slice(i + 1))
        if (selDot === i)
            selDot = -1
        else if (selDot > i)
            selDot--
        layoutDirty = true
        calibStamp++
        statusLine = "removed point " + i
    }
    /* Enter commits the LED-# field to the selected dot (steal:
       a duplicate number elsewhere is cleared). With nothing
       selected the number selects that position; empty field
       steps to the next hardware LED. */
    function commitField() {
        var n = parseInt(ledField.text)
        if (selDot >= 0) {
            if (!(n >= 0)) {
                statusLine = "type the LED number first"
                return
            }
            if (!setLedAt(selDot, n))
                return              /* rejected — keep the dot selected */
            selDot = -1
            ledField.text = ""
            if (sessionActive)
                advance()
            return
        }
        if (!(n >= 0)) {
            if (sessionActive)
                stepLed(1)
            return
        }
        ledField.text = ""
        if (n < pts.length) {
            selDot = n
            calibStamp++
            statusLine = "position " + n + " selected — type its "
                       + "LED number, Enter commits"
        } else {
            statusLine = "no position " + n
        }
    }
    /* Place mode: drop a new emitter at the clicked position. With
       a live session it belongs to the lit LED and advances; idle,
       it lands unassigned so a shape can be sketched first. */
    function placeAt(sx, sy) {
        var p = screenToPt(sx, sy, canvasArea.width, canvasArea.height)
        /* concat reassigns — push() would mutate without notifying. */
        pts = pts.concat([p])
        if (sessionActive) {
            assign = assign.concat([curLed])
            statusLine = "placed point " + (pts.length - 1)
                       + " ← LED " + curLed
            calibStamp++
            advance()
        } else {
            assign = assign.concat([-1])
            statusLine = "placed point " + (pts.length - 1)
            calibStamp++
        }
        layoutDirty = true
    }
    function unassign(i) {
        if (i >= 0 && i < assign.length) {
            assign[i] = -1
            layoutDirty = true
            calibStamp++
        }
    }
    function clearAssign() {
        for (var i = 0; i < assign.length; i++)
            assign[i] = -1
        layoutDirty = true
        calibStamp++
    }
    function stepLed(dir) {
        var n = curLed + dir
        if (n < 0)
            n = hwLeds - 1
        if (n >= hwLeds)
            n = 0
        curLed = n
        if (!chaseOn)
            showCurrent()
    }

    /* ================= shape presets ================= */
    /* Radius of the current layout in projected units — new shapes
       inherit it so they match the device's real scale. */
    function layoutRadius() {
        var r = 0
        for (var i = 0; i < pts.length; i++) {
            var u = pts[i][projU], v = pts[i][projV]
            var d = Math.sqrt(u * u + v * v)
            if (d > r)
                r = d
        }
        return r > 1e-4 ? r : 0.056
    }
    function defaultCount() {
        var z = selZone()
        if (hwLeds > 0)
            return hwLeds
        if (z && z.led_count > 0)
            return z.led_count
        return pts.length > 0 ? pts.length : 8
    }
    /* Make button entry — generating over a non-empty layout
       destroys the working copy (saved points AND any mapping
       progress), so the first click only arms; the second within
       the armed state actually regenerates. Picking another
       shape or reloading resets the arm. */
    function requestMake() {
        if (shapeBox.currentIndex === 0) {
            confirmMake = false
            generateShape(0)
            return
        }
        if (!confirmMake && pts.length > 0) {
            confirmMake = true
            statusLine = "replace the current " + pts.length
                       + "-point layout? Click Make again to confirm"
            return
        }
        confirmMake = false
        generateShape()
    }
    /* Rebuild pts from the shape picker — index 0 at top, then
       clockwise. Generated points start unassigned: the whole
       point is discovering the hardware order. */
    function generateShape(kind, n, cols) {
        if (kind === undefined)
            kind = shapeBox.currentIndex
        if (kind === 0) {
            confirmMake = false
            loadFromZone()
            statusLine = "reloaded saved points"
            return
        }
        if (!(n > 0))
            n = parseInt(countField.text)
        if (!(n > 0))
            n = defaultCount()
        var r = layoutRadius()
        var np = []
        var mk = function(u, v) {
            var p = [0, 0, 0]
            p[cal.projU] = u
            p[cal.projV] = v
            p[3 - cal.projU - cal.projV] = cal.faceVal
            np.push(p)
        }
        var i, a
        if (kind === 1) {                          /* ring */
            for (i = 0; i < n; i++) {
                a = Math.PI / 2 - i * 2 * Math.PI / n
                mk(r * Math.cos(a), r * Math.sin(a))
            }
        } else if (kind === 2) {                   /* double ring */
            var nIn  = Math.floor(n / 2)
            var nOut = n - nIn
            for (i = 0; i < nOut; i++) {
                a = Math.PI / 2 - i * 2 * Math.PI / nOut
                mk(r * Math.cos(a), r * Math.sin(a))
            }
            for (i = 0; i < nIn; i++) {
                a = Math.PI / 2 - i * 2 * Math.PI / nIn
                mk(r * 0.74 * Math.cos(a), r * 0.74 * Math.sin(a))
            }
        } else if (kind === 3) {                   /* strip */
            for (i = 0; i < n; i++)
                mk(n > 1 ? -r + 2 * r * i / (n - 1) : 0, 0)
        } else {                                   /* grid */
            if (!(cols > 0))
                cols = parseInt(colsField.text)
            if (!(cols > 0))
                cols = Math.ceil(Math.sqrt(n))
            var rows = Math.ceil(n / cols)
            var su = 2 * r / Math.max(1, cols - 1)
            var sv = 2 * r / Math.max(1, rows - 1)
            for (i = 0; i < n; i++)
                mk(-r + (i % cols) * su,
                   r - Math.floor(i / cols) * sv)
        }
        var na = []
        for (i = 0; i < np.length; i++)
            na.push(-1)
        pts = np
        assign = na
        layoutDirty = true
        calibStamp++
        statusLine = "generated " + np.length + " points — start a "
                   + "session and click where each LED lights"
    }
    /* Sequential assign — quick sanity check / devices already
       in order. Capped at the hardware bound: identity LEDs past
       the zone's real count could never be lit. */
    function assignIdentity() {
        var na = []
        var bound = ledBound()
        for (var i = 0; i < pts.length; i++)
            na.push(bound > 0 && i >= bound ? -1 : i)
        assign = na
        layoutDirty = true
        calibStamp++
        statusLine = "identity mapping — use verify to spot-check"
    }

    /* ---- hardware zone follow ----
       The canvas should always reflect the PICKED hardware zone:
       its LED count drives the shape. Reading it from the zone
       combo's model (leds field; label "(N LEDs)" as fallback). */
    function zoneLedCount() {
        var m = zoneBox.model, i = zoneBox.currentIndex
        if (m && i >= 0 && i < m.length) {
            var row = m[i]
            if (row && row.leds !== undefined)
                return row.leds
            if (row && row.label) {
                var mm = /\((\d+) LEDs?\)/.exec(String(row.label))
                if (mm)
                    return parseInt(mm[1])
            }
        }
        return 0
    }
    /* Picked zone changed: refresh the count field and, when no
       mapping work is in progress, reshape the canvas to match —
       saved points only if their count agrees with the zone,
       otherwise a generated shape of the right size. */
    function onHwZoneChanged() {
        var n = zoneLedCount()
        hwZoneLeds = n
        if (n > 0)
            countField.text = String(n)
        if (assignedCount() > 0)
            return                      /* don't trash a mapping */
        var l = zoneLayout()
        var saved = (l.type === "points" && l.points)
                  ? l.points.length : 0
        if (shapeBox.currentIndex === 0) {
            if (saved > 0 && (n <= 0 || saved === n)) {
                loadFromZone()
                return
            }
            if (n > 0) {
                /* Saved shape doesn't fit this zone — offer a
                   strip of the right count instead. */
                generateShape(3, n)
                statusLine = "zone has " + n + " LEDs — generated "
                           + "a strip (saved layout had " + saved + ")"
            } else {
                loadFromZone()
            }
            return
        }
        if (n > 0)
            generateShape(shapeBox.currentIndex, n)
    }
    /* Same idea once a session reveals hwLeds — covers stub hosts
       and zone lists without a count. Never touches a mapping in
       progress. */
    function fitShapeToSession() {
        if (hwLeds <= 0 || assignedCount() > 0)
            return
        if (pts.length === hwLeds)
            return
        generateShape(shapeBox.currentIndex === 0 ? 3
                                                  : shapeBox.currentIndex,
                      hwLeds)
        statusLine = "zone has " + hwLeds + " LEDs — reshaped"
    }

    /* ================= session ops ================= */
    /* The OpenRGB2 editor is a JSON document, not a zone
       list — the panel offers its own candidate-zone row:
       the same zones[] Apply writes back to. Model rebuild
       keeps the current pick selected. */
    function refreshCandZones() {
        var e = ed()
        var rows = []
        if (e && e.candidate && e.candidate.zones) {
            var zs = e.candidate.zones
            for (var i = 0; i < zs.length; i++) {
                var id = zs[i].id || ("zone " + i)
                var n = (zs[i].layout && zs[i].layout.points)
                      ? zs[i].layout.points.length
                      : (zs[i].led_count || 0)
                rows.push({ index: i, label: id + " (" + n + ")" })
            }
        }
        candZoneBox.model = rows
        candZoneBox.currentIndex =
            (zoneIndex >= 0 && zoneIndex < rows.length)
            ? zoneIndex : -1
    }
    function refreshHw() { ctrlBox.refresh() }
    /* Start on explicit controller/zone indices — the picker path
       (startSession) and the test seam both land here. */
    function startSessionOn(ctrl, zone) {
        var h = host()
        if (!h) {
            statusLine = "studio host unavailable"
            return
        }
        if (typeof h.calibBegin !== "function") {
            statusLine = "the running plugin predates calibration "
                       + "— relaunch OpenRGB to load the new build"
            return
        }
        var r = h.calibBegin(ctrl, zone)
        if (!r || !r.ok) {
            statusLine = (r && r.error) ? String(r.error)
                                        : "calibBegin refused"
            return
        }
        sessionStarting = true
        startDeadline = Date.now() + startTimeoutMs
        statusLine = "starting…"
        pollTimer.restart()
    }
    function startSession() {
        if (ctrlBox.currentValue === undefined
            || zoneBox.currentValue === undefined) {
            statusLine = "pick a controller and zone first"
            return
        }
        startSessionOn(ctrlBox.currentValue, zoneBox.currentValue)
    }
    function endSession() {
        blinkTimer.stop()
        chaseTimer.stop()
        blinkOn = false
        chaseOn = false
        var h = host()
        /* A PENDING begin is owned too — natively calibEnd flags
           the worker to abort before it activates (calib_cancel).
           Skipping it here leaked a live session to the hardware
           every time the panel closed inside the starting gap. */
        if ((sessionActive || sessionStarting) && h
            && typeof h.calibEnd === "function")
            h.calibEnd()
        sessionActive = false
        sessionStarting = false
        startDeadline = 0
        /* Pull the post-end state — calib_paused drains async, so
           the picker gate needs info.paused, not just active. */
        syncSession()
    }
    /* Session state sync — the C++ side notifies via
       calibrationChanged; the 100 ms poll covers stub hosts in
       tests (plain JS objects can't emit). */
    function syncSession() {
        var h = host()
        if (!h || typeof h.calibInfo !== "function")
            return
        var info = h.calibInfo()
        if (!info)
            return
        if (info.active && !sessionActive) {
            sessionActive = true
            sessionStarting = false
            selDot = -1          /* typed numbers = positions now */
            hwLeds = info.leds || 0
            hwZoneLeds = hwLeds
            fitShapeToSession()
            curLed = 0
            showCurrent()
            statusLine = "LED 0 is lit — click its position"
        } else if (!info.active && sessionActive) {
            sessionActive = false
            statusLine = info.message ? String(info.message)
                                      : "session ended"
        } else if (!info.active && !info.starting && sessionStarting) {
            /* Begin's worker finished without activating — the
               reason only exists in info.message (it otherwise
               lands in the diagnostics drawer, out of sight). */
            statusLine = info.message ? String(info.message)
                                      : "calibration failed to start"
        }
        sessionStarting = info.starting === true
        /* The native pause outlives the session flag — the end
           worker's color/mode restore holds it briefly. Starting
           again in that window is refused (calibBegin gates on
           paused), so the button stays gated too. */
        hwPaused = info.paused === true
    }
    Connections {
        target: cal.hostQObj()
        function onCalibrationChanged() { cal.syncSession() }
    }
    Timer {
        id: pollTimer
        interval: 100
        repeat: true
        onTriggered: {
            cal.syncSession()
            /* Bounded start — a driver wedged inside pausePushes
               would otherwise hold "starting…" forever with no
               way out but closing OpenRGB. */
            if (cal.sessionStarting && cal.startDeadline > 0
                && Date.now() > cal.startDeadline) {
                cal.endSession()
                cal.statusLine = "start timed out after "
                               + (cal.startTimeoutMs / 1000)
                               + " s — cancelled"
            }
            if (!cal.sessionStarting)
                stop()
        }
    }
    /* Blink the current LED — easier to spot on the device. */
    Timer {
        id: blinkTimer
        interval: 450
        repeat: true
        property bool phase: false
        onTriggered: {
            phase = !phase
            var h = cal.host()
            if (!cal.sessionActive || !h
                || typeof h.calibShow !== "function")
                return
            /* Always the session's current LED — blink is the "which
               one is lit" aid; selecting a dot must not retarget it. */
            h.calibShow(phase ? cal.curLed : -1)
        }
    }
    /* Chase the hardware in LED order — direction/order check. */
    Timer {
        id: chaseTimer
        interval: 320
        repeat: true
        onTriggered: {
            var h = cal.host()
            if (!cal.sessionActive || !h
                || typeof h.calibShow !== "function") {
                stop(); cal.chaseOn = false
                return
            }
            h.calibShow(cal.chaseLed)
            cal.chaseLed = (cal.chaseLed + 1) % Math.max(1, cal.hwLeds)
        }
    }

    /* ================= apply ================= */
    function applyMapping() {
        var e = ed()
        var z = selZone()
        if (!e || !z)
            return
        /* Defense for working maps that bypassed setLedAt —
           a stale preset file can hold addresses past the zone's
           real LED count; persisting them is exactly the bug the
           E2E shipped. */
        var bad = invalidAssigned()
        if (bad > 0) {
            statusLine = "not applied — " + bad + " position(s) map "
                       + "LEDs out of range (zone has " + ledBound()
                       + "): fix or unassign them first"
            return
        }
        var outPts = []
        for (var i = 0; i < pts.length; i++)
            outPts.push(pts[i].slice())
        z.layout = { type: "points",
                     points: outPts,
                     addresses: assign.slice() }
        e.syncZoneLedCount(zoneIndex)
        e.touch()
        layoutDirty = false
        var un = pts.length - assignedCount()
        statusLine = "applied " + pts.length + " points, "
                   + assignedCount() + " mapped"
                   + (un > 0 ? " (" + un + " unassigned)" : "")
                   + " — Save writes the preset file"
    }

    /* ================= small components ================= */
    component Btn: Rectangle {
        property string text: ""
        property string tip: ""
        property bool primary: false
        signal clicked()
        height: 24; radius: Theme.radiusS
        width: btxt.implicitWidth + Theme.spaceM
        color: !enabled ? Theme.bg
             : primary ? Theme.accentMuted
             : (bma.containsMouse ? Theme.surfaceAlt : Theme.bg)
        border.color: primary ? Theme.accent : Theme.borderStrong
        opacity: enabled ? 1 : 0.45
        Text {
            id: btxt
            anchors.centerIn: parent
            text: parent.text
            color: parent.enabled ? Theme.textPrimary : Theme.textDisabled
            font.pixelSize: Theme.fontSizeBody
        }
        MouseArea {
            id: bma; anchors.fill: parent; hoverEnabled: true
            enabled: parent.enabled
            onClicked: parent.clicked()
        }
        ToolTip.visible: bma.containsMouse && tip !== ""
        ToolTip.text: tip
        ToolTip.delay: 400
    }
    component Chk: Rectangle {
        property bool on: false
        property string label: ""
        signal toggled(bool v)
        width: chkRow.implicitWidth; height: 22
        color: "transparent"
        Row {
            id: chkRow
            spacing: Theme.spaceXS
            Rectangle {
                width: 16; height: 16; radius: 3
                anchors.verticalCenter: parent.verticalCenter
                color: parent.parent.on ? Theme.accentMuted : Theme.bg
                border.color: parent.parent.on ? Theme.accent : Theme.borderStrong
                Text {
                    anchors.centerIn: parent
                    text: parent.parent.parent.on ? "✓" : ""
                    color: Theme.textPrimary; font.pixelSize: Theme.fontSizeSmall
                }
            }
            Text {
                text: label
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.textSecondary; font.pixelSize: Theme.fontSizeSmall
            }
        }
        MouseArea {
            anchors.fill: parent
            onClicked: parent.toggled(!parent.on)
        }
    }
    component NumField: TextField {
        property string tip: ""
        width: 46; height: 24
        padding: 4
        hoverEnabled: true
        color: Theme.textPrimary; font.pixelSize: Theme.fontSizeSmall
        horizontalAlignment: TextInput.AlignHCenter
        selectByMouse: true
        validator: IntValidator { bottom: 0; top: 512 }
        background: Rectangle {
            radius: Theme.radiusS; color: Theme.bg
            border.color: parent.activeFocus ? Theme.accent : Theme.borderStrong
        }
        ToolTip.visible: hovered && tip !== ""
        ToolTip.text: tip
        ToolTip.delay: 400
    }

    /* ================= layout ================= */
    Column {
        anchors.fill: parent
        anchors.margins: Theme.spaceM
        spacing: Theme.spaceS

        /* Title bar. */
        Rectangle {
            width: parent.width; height: 30
            color: Theme.surfaceAlt; radius: Theme.radiusM
            Text {
                x: Theme.spaceS
                anchors.verticalCenter: parent.verticalCenter
                text: "LED calibration — zone '" + cal.zoneId + "'"
                    + (cal.sessionActive ? "  ·  LIVE" : "")
                color: Theme.textPrimary; font.pixelSize: Theme.fontSizeTitle
                font.bold: true
            }
            Btn {
                x: parent.width - width - Theme.spaceXS
                anchors.verticalCenter: parent.verticalCenter
                text: "Close"
                onClicked: cal.close()
            }
        }

        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            text: "Pick the device + zone, Start — one LED lights. "
                + "Click the dot where it physically lit; it takes "
                + "that number and the next LED lights. To fix a "
                + "mapped dot, click it and type a new number in the "
                + "box. Right-click removes a dot."
            color: Theme.textDisabled; font.pixelSize: Theme.fontSizeSmall
        }

        /* Candidate-zone picker — which draft zone Apply
           writes (the editor's zone list this replaced). */
        Row {
            spacing: Theme.spaceXS
            height: 26
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "draft zone"
                color: Theme.textDisabled
                font.pixelSize: Theme.fontSizeSmall
            }
            ComboBox {
                id: candZoneBox
                height: 24; width: 170
                anchors.verticalCenter: parent.verticalCenter
                enabled: !cal.sessionActive && !cal.sessionStarting
                model: []
                textRole: "label"
                valueRole: "index"
                onActivated: {
                    var e = cal.ed()
                    if (e && e.selZoneIdx !== undefined)
                        e.selZoneIdx = candZoneBox.currentIndex
                    else
                        cal.openForZone(e, candZoneBox.currentIndex)
                }
                contentItem: Text {
                    text: candZoneBox.displayText
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontSizeSmall
                    verticalAlignment: Text.AlignVCenter
                    leftPadding: 8; elide: Text.ElideRight
                }
                background: Rectangle {
                    radius: Theme.radiusS; color: Theme.bg
                    border.color: candZoneBox.visualFocus
                                  ? Theme.accent : Theme.borderStrong
                }
            }
        }

        /* Hardware pickers + session control. */
        Row {
            spacing: Theme.spaceXS
            height: 26
            ComboBox {
                id: ctrlBox
                height: 24; width: 230
                anchors.verticalCenter: parent.verticalCenter
                enabled: !cal.sessionActive && !cal.sessionStarting
                model: []
                textRole: "label"
                valueRole: "index"
                function refresh() {
                    var h = cal.host()
                    model = (h && typeof h.diagControllers === "function")
                          ? (h.diagControllers() || []) : []
                    refreshZones()
                }
                function refreshZones() {
                    var h = cal.host()
                    zoneBox.model = (h && ctrlBox.currentValue !== undefined
                            && typeof h.diagZones === "function")
                        ? h.diagZones(ctrlBox.currentValue) : []
                    cal.onHwZoneChanged()
                }
                onActivated: refreshZones()
                onCurrentIndexChanged: refreshZones()
                contentItem: Text {
                    text: ctrlBox.displayText
                    color: Theme.textPrimary; font.pixelSize: Theme.fontSizeSmall
                    verticalAlignment: Text.AlignVCenter
                    leftPadding: 8; elide: Text.ElideRight
                }
                background: Rectangle {
                    radius: Theme.radiusS; color: Theme.bg
                    border.color: ctrlBox.visualFocus ? Theme.accent
                                                      : Theme.borderStrong
                }
            }
            ComboBox {
                id: zoneBox
                height: 24; width: 180
                anchors.verticalCenter: parent.verticalCenter
                enabled: !cal.sessionActive && !cal.sessionStarting
                model: []
                textRole: "label"
                valueRole: "index"
                onActivated: cal.onHwZoneChanged()
                onCurrentIndexChanged: cal.onHwZoneChanged()
                contentItem: Text {
                    text: zoneBox.displayText
                    color: Theme.textPrimary; font.pixelSize: Theme.fontSizeSmall
                    verticalAlignment: Text.AlignVCenter
                    leftPadding: 8; elide: Text.ElideRight
                }
                background: Rectangle {
                    radius: Theme.radiusS; color: Theme.bg
                    border.color: zoneBox.visualFocus ? Theme.accent
                                                      : Theme.borderStrong
                }
            }
            Btn {
                text: "Refresh"
                enabled: cal.hasHost() && !cal.sessionActive
                onClicked: {
                    var h = cal.host()
                    if (h && typeof h.diagRefresh === "function")
                        h.diagRefresh()
                    ctrlBox.refresh()
                }
            }
            Btn {
                text: cal.sessionActive ? "End session"
                      : cal.sessionStarting ? "Cancel" : "Start"
                primary: !cal.sessionActive
                /* hwPaused gates Start only — a draining end
                   worker still owns the pause; calibBegin would
                   refuse, so don't offer a dead click. */
                enabled: cal.hasHost()
                         && (cal.sessionActive || cal.sessionStarting
                             || !cal.hwPaused)
                tip: cal.sessionStarting
                   ? "Abort the pending start — the begin worker "
                   + "stops at its next checkpoint"
                   : "Pauses live output, blacks out the zone and "
                   + "lights one LED at a time"
                onClicked: {
                    if (cal.sessionActive || cal.sessionStarting)
                        cal.endSession()
                    else
                        cal.startSession()
                }
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: !cal.hasHost()
                text: "host unavailable"
                color: Theme.textDisabled; font.pixelSize: Theme.fontSizeSmall
            }
        }

        /* Non-points layouts need conversion first. */
        Row {
            spacing: Theme.spaceXS
            visible: cal.zoneLayout().type !== "points"
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "zone uses a generated layout — calibration "
                    + "needs explicit points"
                color: Theme.warn; font.pixelSize: Theme.fontSizeSmall
            }
            Btn {
                text: "Convert to points"
                enabled: cal.zoneLayout().type !== "matrix"
                         || cal.zoneLayout().dynamic !== true
                onClicked: {
                    var e = cal.ed()
                    if (e && typeof e.convertZone === "function") {
                        e.convertZone(cal.zoneIndex)
                        cal.loadFromZone()
                    }
                }
            }
        }

        /* Canvas + side controls. */
        Row {
            width: parent.width
            height: parent.height - y - Theme.spaceS
            spacing: Theme.spaceM

            /* ---- face diagram ---- */
            Rectangle {
                id: canvasArea
                width: parent.width - ctrlCol.width - Theme.spaceM
                height: parent.height
                color: Theme.bg; radius: Theme.radiusS
                border.color: Theme.border
                clip: true

                /* Origin crosshair + bounding circle. */
                Item {
                    anchors.fill: parent
                    Repeater {
                        model: 2
                        Rectangle {
                            required property int index
                            property var o: {
                                cal.calibStamp
                                return cal.originPos(canvasArea.width,
                                                     canvasArea.height)
                            }
                            width:  index === 0 ? parent.width : 1
                            height: index === 0 ? 1 : parent.height
                            x: index === 0 ? 0 : o.x
                            y: index === 0 ? o.y : 0
                            color: Theme.border
                        }
                    }
                    Rectangle {
                        property var c: {
                            cal.calibStamp
                            return cal.boundCircle(canvasArea.width,
                                                   canvasArea.height)
                        }
                        x: c.x - c.r; y: c.y - c.r
                        width: c.r * 2; height: c.r * 2
                        radius: c.r
                        color: "transparent"
                        border.color: Theme.borderStrong
                    }
                }

                /* Emitter dots — label = assigned hardware LED. */
                Repeater {
                    model: {
                        cal.calibStamp
                        return cal.pts.length
                    }
                    delegate: Rectangle {
                        required property int index
                        property var pos: {
                            cal.calibStamp
                            return cal.dotPos(index, canvasArea.width,
                                              canvasArea.height)
                        }
                        property int addr: {
                            cal.calibStamp
                            return cal.assign[index] !== undefined
                                 ? cal.assign[index] : -1
                        }
                        width: 18; height: 18; radius: 9
                        x: pos.x - 9; y: pos.y - 9
                        color: addr === cal.curLed && cal.sessionActive
                               ? Theme.accentMuted
                               : index === cal.selDot ? Theme.bg
                               : addr >= 0 ? "#2a4030" : Theme.surfaceAlt
                        border.color: index === cal.selDot ? Theme.warn
                                    : addr === cal.curLed ? Theme.accent
                                    : addr >= 0 ? Theme.ok : Theme.borderStrong
                        border.width: (index === cal.selDot
                                       || addr === cal.curLed) ? 2 : 1
                        Text {
                            anchors.centerIn: parent
                            /* Unassigned dots show their position
                               index — the shape stays readable
                               before any LED is mapped. */
                            text: parent.addr >= 0 ? parent.addr
                                                   : parent.index
                            color: parent.addr >= 0 ? Theme.textPrimary
                                                    : Theme.textDisabled
                            font.pixelSize: 8
                        }
                        MouseArea {
                            anchors.fill: parent
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            hoverEnabled: true
                            onClicked: function(m) {
                                if (m.button === Qt.RightButton)
                                    cal.removeDot(parent.index)
                                else
                                    cal.assignDot(parent.index)
                            }
                            ToolTip.visible: containsMouse
                            ToolTip.text: {
                                /* Explicit stamp dep — plain
                                   array-element writes inside
                                   setLedAt don't retrigger this
                                   binding on their own, and the
                                   tip kept showing the pre-assign
                                   state after a click. */
                                cal.calibStamp
                                return "position " + parent.index
                                    + (parent.addr >= 0
                                       ? " ← LED " + parent.addr
                                       : " (unassigned)")
                                    + " · right-click removes"
                            }
                            ToolTip.delay: 300
                        }
                    }
                }

                /* Click-to-place (place mode). Without a session the
                   dot lands unassigned; with one it takes the lit
                   LED and advances. */
                MouseArea {
                    anchors.fill: parent
                    z: -1
                    enabled: cal.place && !cal.verify
                    onClicked: function(m) { cal.placeAt(m.x, m.y) }
                }

                /* Selected-dot readout. */
                Text {
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 10
                    visible: cal.selDot >= 0
                    text: "pos " + cal.selDot
                    color: Theme.warn
                    font.pixelSize: Theme.fontSizeTitle; font.bold: true
                }
            }

            /* ---- controls ---- */
            Column {
                id: ctrlCol
                width: 250
                spacing: Theme.spaceS

                Text {
                    text: cal.sessionActive
                          ? "LED " + cal.curLed + " / " + (cal.hwLeds - 1)
                            + " is lit"
                          : cal.sessionStarting ? "starting…"
                          : cal.hwPaused ? "ending…"
                          : "no session"
                    color: cal.sessionActive ? Theme.textPrimary : Theme.textDisabled
                    font.pixelSize: Theme.fontSizeTitle; font.bold: true
                }
                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    visible: cal.sessionActive && !cal.verify && !cal.place
                    text: "LED " + cal.curLed + " is lit — click the "
                        + "dot at its physical position"
                    color: Theme.textSecondary; font.pixelSize: Theme.fontSizeSmall
                }
                /* Canvas/zone mismatch — the classic "wrong shape"
                   moment: hardware says N LEDs, canvas shows M. */
                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    visible: {
                        cal.calibStamp
                        var n = cal.sessionActive ? cal.hwLeds
                                                  : cal.hwZoneLeds
                        return n > 0 && cal.pts.length !== n
                    }
                    text: {
                        cal.calibStamp
                        var n = cal.sessionActive ? cal.hwLeds
                                                  : cal.hwZoneLeds
                        return "zone has " + n + " LEDs but the canvas "
                             + "has " + cal.pts.length + " positions — "
                             + "pick a shape + Make to refit"
                    }
                    color: Theme.warn; font.pixelSize: Theme.fontSizeSmall
                }

                /* LED-# box — click a dot to select it, then the
                   number here is written as its hardware LED. */
                Row {
                    spacing: Theme.spaceXS
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "LED #"
                        color: Theme.textSecondary; font.pixelSize: Theme.fontSizeSmall
                    }
                    NumField {
                        id: ledField
                        width: 64
                        enabled: cal.selDot >= 0 || cal.sessionActive
                        tip: "Hardware LED number for the selected dot"
                        /* Cap at the real bound when known — the
                           field can't commit what it can't type. */
                        validator: IntValidator {
                            bottom: 0
                            top: cal.ledBound() > 0
                                 ? cal.ledBound() - 1 : 512
                        }
                        onAccepted: cal.commitField()
                    }
                    Btn {
                        text: "Set"
                        enabled: cal.selDot >= 0
                        tip: "Write the number to the selected dot"
                        onClicked: cal.commitField()
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: cal.selDot >= 0
                        text: "→ pos " + cal.selDot
                        color: Theme.warn; font.pixelSize: Theme.fontSizeSmall
                    }
                }

                Row {
                    spacing: Theme.spaceXS
                    Btn {
                        text: "◀"; enabled: cal.sessionActive
                        tip: "Previous LED"
                        onClicked: cal.stepLed(-1)
                    }
                    Btn {
                        text: "▶"; enabled: cal.sessionActive
                        tip: "Next LED (skip this one)"
                        onClicked: cal.stepLed(1)
                    }
                    Btn {
                        text: cal.blinkOn ? "Blink ●" : "Blink"
                        enabled: cal.sessionActive
                        tip: "Flash the current LED on/off"
                        onClicked: {
                            cal.blinkOn = !cal.blinkOn
                            if (cal.blinkOn) {
                                blinkTimer.phase = false
                                blinkTimer.start()
                            } else {
                                blinkTimer.stop()
                                cal.showCurrent()
                            }
                        }
                    }
                    Btn {
                        text: cal.chaseOn ? "Chase ●" : "Chase"
                        enabled: cal.sessionActive
                        tip: "Cycle all LEDs in hardware order"
                        onClicked: {
                            cal.chaseOn = !cal.chaseOn
                            if (cal.chaseOn) {
                                cal.chaseLed = 0
                                chaseTimer.start()
                            } else {
                                chaseTimer.stop()
                                cal.showCurrent()
                            }
                        }
                    }
                }

                Btn {
                    text: cal.showMore ? "Less ▴" : "More ▾"
                    tip: "verify / place / flip / clear"
                    onClicked: cal.showMore = !cal.showMore
                }

                Column {
                    width: parent.width
                    spacing: Theme.spaceS
                    visible: cal.showMore

                    Chk {
                        label: "verify — click a dot to light its LED"
                        on: cal.verify
                        onToggled: function(v) {
                            cal.verify = v
                            if (v)
                                cal.place = false
                        }
                    }
                    Chk {
                        label: "place — click empty space to add a point"
                        on: cal.place
                        onToggled: function(v) {
                            cal.place = v
                            if (v)
                                cal.verify = false
                        }
                    }
                    Row {
                        spacing: Theme.spaceXS
                        Btn {
                            text: "Flip ⇄"
                            tip: "Mirror the view horizontally "
                               + "(axis " + "XYZ"[cal.projU] + ")"
                            onClicked: { cal.flipU = !cal.flipU; cal.calibStamp++ }
                        }
                        Btn {
                            text: "Flip ⇅"
                            tip: "Mirror the view vertically "
                               + "(axis " + "XYZ"[cal.projV] + ")"
                            onClicked: { cal.flipV = !cal.flipV; cal.calibStamp++ }
                        }
                        Btn {
                            text: "Clear"
                            tip: "Unassign every dot"
                            enabled: cal.assignedCount() > 0
                            onClicked: cal.clearAssign()
                        }
                    }
                }

                /* Shape presets — generate the dot layout first,
                   then only the LED numbers need calibrating. */
                Row {
                    spacing: Theme.spaceXS
                    ComboBox {
                        id: shapeBox
                        height: 24; width: 96
                        anchors.verticalCenter: parent.verticalCenter
                        /* Changing the pick disarms a pending
                           replace-confirm — the next Make click
                           must re-ask for the NEW shape. */
                        onActivated: cal.confirmMake = false
                        model: ["saved", "ring", "2 rings", "strip", "grid"]
                        contentItem: Text {
                            text: shapeBox.displayText
                            color: Theme.textPrimary; font.pixelSize: Theme.fontSizeSmall
                            verticalAlignment: Text.AlignVCenter
                            leftPadding: 8; elide: Text.ElideRight
                        }
                        background: Rectangle {
                            radius: Theme.radiusS; color: Theme.bg
                            border.color: shapeBox.visualFocus ? Theme.accent
                                                              : Theme.borderStrong
                        }
                    }
                    NumField {
                        id: countField
                        tip: "point count (default: zone LED count)"
                    }
                    NumField {
                        id: colsField
                        visible: shapeBox.currentIndex === 4
                        tip: "grid columns"
                    }
                    Btn {
                        text: cal.confirmMake ? "Make — sure?"
                                              : "Make"
                        tip: "Generate the shape as unassigned "
                           + "points — replaces the current "
                           + "layout (asks first)"
                        onClicked: cal.requestMake()
                    }
                    Btn {
                        text: "1:1"
                        tip: "Identity map — position i = LED i "
                           + "(spot-check with verify)"
                        onClicked: cal.assignIdentity()
                    }
                }

                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: {
                        cal.calibStamp      /* re-eval on mapping edits */
                        return cal.assignedCount() + " of " + cal.pts.length
                            + " positions mapped · view "
                            + "XYZ"[cal.projU] + "→, "
                            + "XYZ"[cal.projV] + "↑"
                            + (cal.layoutDirty
                               ? " · unsaved layout edits" : "")
                    }
                    color: Theme.textSecondary; font.pixelSize: Theme.fontSizeSmall
                }

                Btn {
                    text: "Apply to zone"
                    primary: true
                    enabled: cal.pts.length > 0
                    tip: "Write points + addresses into the zone "
                       + "layout — Save persists the preset file"
                    onClicked: cal.applyMapping()
                }

                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: cal.statusLine
                    color: Theme.textSecondary; font.pixelSize: Theme.fontSizeSmall
                }
            }
        }
    }
}
