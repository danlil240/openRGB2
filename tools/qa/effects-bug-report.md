# OpenRGB2 Effects — QA Bug Report

Date: 2025 (session via `tools/qa/qa.py` against packaged build
`build/package/OpenRGB2-0.1.0-desktop-win64/openrgb2-ui.exe --qa`,
live engine + SDK backend, 7 controllers / 321 LEDs).

Method: scripted UI driving over the QA socket (`127.0.0.1:5583`),
engine-side truth via `openrgb2-ctl` (`scene`, `playback`, `diag`,
`raw`, `subscribe`), physical output verified by `sdk-save` LED
readback. No product code was changed; only `tools/qa/qa.py` was
extended (see "Harness notes").

## Resolution status (post-fix)

| Bug | Fix | Commit | Verified |
| --- | --- | --- | --- |
| BUG-1 | Engine refuses the live switch for uploaded-only presets (symmetric with the existing animation refusal); reason surfaces via `unavailable_reason` / card gating | `81fabe2` | Unit + e2e tests; live: `capabilities.live_effects=false` + reason while preset-only scene applied |
| BUG-2 | `MsgType::ReactiveCommand` added to `IsRequestType`; wire-level regression test | `2896921` | Live: audio checkbox → `enabled:true, running:true, "audio: listening"` |
| BUG-3 | `frame_status` filtered out of `event_log_` | `c36d47b` | Live: journal holds real events only |
| BUG-4 | Refused name reverts field; `nameResyncPending` latch covers Discard/Reload; `saveError` clears | `b4f0137` | Live: refused name → field resyncs → next save applies |

Also landed: `test(engine)` `4a3af46` (e2e fixtures opt out of
scene restore — leftover snapshots voided clean-start assumptions)
and cherry-picked `1540a3d` (UI test fixtures got a real device
object; the empty-objects fixtures had been red since `0b6916c`'s
save guard — ui_playback/ui_hardening/qml_focused_save+qml_hardening).

---

## Confirmed bugs

### BUG-1 — Switching to Live effects after a playback preset blacks out every LED (severe)

**Repro**

1. Lighting page → pick an uploaded/playback preset, e.g.
   `preset_spectrum_cycle` or `preset_breathing` → **Save**
   (engine auto-requests `animation_playback`, upload goes active).
2. Click the **Live effects** mode card (`mode_live_effects`).

**Expected**: live effects render, or the mode switch is refused /
the scene preset is replaced with a valid live preset.

**Actual**: applied scene still carries `effect.preset:
"spectrum_cycle"`, which is **not in the live-effects registry**
(`aurora…ambient`). Live evaluation resolves the preset only via
`studio::FindPreset()`/`BuildPreset()` (`src/ipc/EngineService.cpp`),
so the layer stack is empty → **all output is black**.

Meanwhile the UI reports healthy live operation:

- mode card: "Live effects", status "on device"
- delivery telemetry: ~388 updates/s
- no warning, no error

**Evidence**

- `openrgb2-ctl sdk-save` after the switch: `total 321, nonblack 0`
  (`shots/sdk-live-breathing.json`; `shots/bug1-live-after-anim.png`,
  `shots/bug1-live-breathing.png`).
- Control: saving live preset `embers` immediately after →
  `nonblack 321` — the readback path is valid; the blackout is real.
- Reproduced 3× with two different playback presets.
- Detail: static `object_colors` overrides survive the blackout —
  the one lit LED (`#FC6764` on `X870E AORUS ELITE WIFI7 ICE`)
  was a palette override, so the failure is in the live-layer
  evaluation, not the delivery path.

**Inverse path is handled**: picking a *live* preset while
animation playback is active and saving correctly reverts to
`live_effects` (engine reason: `preset 'embers' is not an uploaded
playback preset`). The hole is specifically the **mode-card
switch** — nothing rewrites or rejects the playback-only preset in
the scene.

**Suggested fix area**: the `live_effects` request path
(`SceneEditController::requestMode` / engine `SetPlaybackMode`)
should either refuse the switch while `effect.preset` resolves to
no live preset, or substitute/fall back — and the UI should flag
"current preset is playback-only" instead of claiming live
delivery.

---

### BUG-2 — Reactive input toggles are dead: `REACTIVE_COMMAND` rejected at the session layer (severe)

**Repro**

1. Apply `shockwave` (needs audio) or `ambient` (needs screen).
2. Click the audio/screen reactive-input checkbox.

**Actual**: the checkbox snaps back, input stays
`enabled:false, running:false`. Zero inline feedback.

**Root cause (structural, confirmed in source)**:
`IsRequestType()` in `src/ipc/IpcServer.cpp:31` enumerates every
request type **except `MsgType::ReactiveCommand` (0x010d)**. The
session layer therefore rejects every reactive command with
`unknown message type` before `EngineService::Dispatch` ever sees
it — the handler (`src/ipc/EngineService.cpp:1186`,
`HandleReactiveCommand` at :2155) is unreachable over the wire.

Wire proof: `openrgb2-ctl raw '{"op":"reactive","verb":
"set_enabled","args":{"input":"audio","enabled":true}}'` →
reply error `unknown message type`. `lastError` on the bridge also
captures it. `IsRequestType` contains every other 0x01xx request —
`0x010d` is the only gap.

**Why tests missed it**: `tests/engine/ipc_reactive_test.cpp`
exercises `svc.Dispatch(MsgType::ReactiveCommand, …)` directly —
it bypasses the `IpcServer::IsRequestType` gate. Notably, this
exact regression class already happened once: `Quit` was
previously missing from `IsRequestType` and got a wire-level
regression test (`tests/engine/ipc_server_test.cpp:477`, comment:
"IsRequestType once lacked Quit — the verb was rejected as
UnknownCommand before EngineService ever saw it"). `ReactiveCommand`
needs the same wire-level coverage.

**UX side**: even when the engine replies with an error, the
control gives no inline indication — the error lands only in
`bridge.lastError` / the diagnostics journal (which BUG-3 makes
unreadable).

**Suggested fix area**: add `MsgType::ReactiveCommand` to
`IsRequestType`, add a wire-level regression test mirroring the
Quit test, and surface the error reply on the checkbox itself.

---

### BUG-3 — Diagnostics event journal is drowned by `frame_status` spam (medium)

`DiagnosticsController::OnFrameStatus()` appends one `frame_status`
entry per frame; the ring is `kEventLogBound = 128`
(`src/ui/DiagnosticsController.h:128`,
`src/ui/DiagnosticsController.cpp:655`). At live-effects frame
rates (~16–30 Hz) the journal holds roughly **4–8 seconds of
identical spam** — the BUG-2 `unknown message type` error was
evicted before it could be read in the UI.

Observed: `diagCtl.eventLog` contained only blank-detail
`frame_status` rows; no error entries retrievable.

**Suggested fix area**: don't journal `frame_status` (or rate-limit /
coalesce it); errors should never be evicted by telemetry noise.

---

### BUG-4 — One refused scene-name edit permanently poisons Save (medium-high)

**Repro**

1. Clear the scene-name field (`nameField`), click Save → refused
   correctly: `saveError: "scene name is empty"`, `sent: 0`.
2. The field is now stuck showing `""` while `sceneEdit.sceneName`
   is still the applied name. It never re-syncs (see below).
3. Pick any preset / move any slider → click Save.

**Actual**: `commitPendingEdits()`
(`ui/pages/LightingPage.qml:93`) calls
`sceneEdit.setSceneName(nameField.text)` first; the empty text
fails validation → **the whole save aborts**. Every subsequent
Save is refused — preset picks, sliders, everything — with
`dirty:true` persisting and the engine doc untouched (verified:
applied stayed `ambient` rev 20 while the UI held unsaved `comet`).
Typing a valid name unblocks it; **Discard does not** — after
Discard the field still shows `""` and `saveError` persists
(verified).

**Root cause**: the re-sync sentinel — `onWorkingChanged` only
writes the field when `want !== syncedSceneName`
(`LightingPage.qml:1666-1676`). A *refused* edit never changes the
model name, so `want` still equals `syncedSceneName` and the
invalid text is never pushed back out. The comment even claims
"a refused edit's revert" triggers resync — it doesn't.

**Impact**: a single accidental clear of the name field silently
breaks **all saving** until the user retypes a valid name or
reloads the page. This is a plausible contributor to
"effects changes don't apply" reports.

**Suggested fix area**: re-sync the field after a refused name
commit (or track a separate "field dirty" flag), and clear
`saveError` on Discard/Reload.

---

---

## Second pass — 2026-09-29 (post-fix verification + continued hunting)

Drove the same flows again against `build/src/ui/Release/openrgb2-ui.exe
--qa` (rebuilt after `81fabe2`..`b4f0137`), live engine, same 7×321
topology. All four fixes verified end-to-end in the live UI (table
above). Two **new** confirmed defects below; neither is a regression of
the earlier fixes.

### BUG-5 — `topology` events churn continuously while effects animate (medium)

`EngineService::PollEvents` (`src/ipc/EngineService.cpp:2268`) diffs
`SnapshotsJson()` to decide whether a `topology` event is pending — but
`SnapshotsJson()` builds each entry with
`SnapshotToJson(snap, /*colors*/ true)` (`:588`), so the diffed payload
**includes per-frame LED colors**. Any animated frame flips the
signature → a full `topology` EVENT (all controllers + all colors)
pushes at every poll tick while effects run.

Wire proof (`openrgb2-ctl subscribe 6` while `aurora` animates):
4 `topology` events in ~5.5 s; diffing consecutive payloads shows the
only changed fields are `colors` (`shots/r2-events2.log`).

Consequences:

- The diagnostics journal fills with `topology` rows at ~0.7 Hz —
  BUG-3's `frame_status` fix removed one spam source but the same
  evict-the-real-events class persists: the 128-entry ring cycles in
  ~3 min of animation.
- Continuous multi-KB pushes on the wire, plus `snapshotsChanged` →
  `RecomputeControllers()` churn in the UI, for what is structurally a
  no-op (topology = devices/zones, not pixels).

**Suggested fix area**: diff topology on a structure-only signature
(`SnapshotToJson(snap, colors=false)`) — colors already stream in
`frame_status` at frame rate; nothing needs them in the topology event.

### BUG-6 — Event journal drops every event payload (low)

`DiagnosticsController::OnEventReceived(const QString& kind,
const QVariantMap&)` (`src/ui/DiagnosticsController.cpp`) ignores the
payload and calls `AppendEvent(kind, QString())` — so **every** journal
entry has an empty `detail`. Scene/playback/topology rows are
indistinguishable and carry no context (no preset name, revision, or
controller count); only `error`/`connection` rows ever show detail.

Deterministic from source; observed live: all journal rows had blank
detail while events flowed.

**Suggested fix area**: render a compact per-kind detail (e.g. scene
name + revision, mode, snapshot count) — the payload is already on hand.

### Second-pass verified working (in addition to the first-pass list)

- Engine bounce: `quit` → UI degrades honestly (banner, retry,
  controls inert, edits kept) → relaunch → reconnects and restores the
  last applied scene; conflict flag still correct afterwards.
- Transport **Stop** on an upload, then picking a live preset + Save →
  `live_effects`/`playing`, all 321 LEDs lit (no blackout carry-over;
  `shots/r2-sdk-live-after-stop.json`).
- Layout editing via `layoutCtl.moveObject` → undo/redo both apply.
- Keyboard: Tab focus moves sensibly; Space activates the focused
  control; `type`/`key` reach `nameField`.
- Rapid preset→save race: intermediate picks serialize, last wins
  (`chrome`/`comet` sequence ended with the final pick applied).
- Unicode scene name (`灯光 fx ✨ español`) persists correctly — the
  mangled console glyphs were a terminal-decoding artifact (raw JSON
  bytes are valid UTF-8).
- Dashboard preview carries real per-LED colors (comet gradient, not
  placeholders).
- Diagnostics export writes a sane bundle; no `frame_status` in the
  journal post-fix.
- Reactive inputs round-trip: audio off→on (`audio: listening`),
  screen (`sampling \\.\DISPLAY11`), key (`keys: listening`); reactive
  scenes remain honestly live-only.

### Second-pass harness changes (`tools/qa/qa.py`, uncommitted)

- `pick X Y` op — deepest-first ancestor chain under a window point via
  `childAt` (implemented through `eval`; no server change needed).
- `click` now auto-reveals clipped targets and **verifies the click
  landed on the intended item** by comparing the `pick` chain before
  and after — catches silent misses into occluding layers.
- `reveal` hardened: a single unmoved scroll no longer aborts (page may
  still be settling post-nav); requires three consecutive stills before
  `fixed_offscreen`.
- Worked around (not fixed) one gap: wheel `scroll` needs a
  plain-number `dy` arg in position 3 (`scroll X Y DY`), and
  `set <flickable> contentY N` is a reliable fallback when wheel
  delivery is unreliable mid-transition.

## Low-confidence observations (not confirmed)

- **Conflict banner vs. `conflict:false`** — one screenshot
  (`shots/conflict-state.png`, later `bug4-saveblocked.png`) showed
  the "applied scene changed" headline while eval reported
  `conflict:false`. `conflict` is latched in `LoadApplied` and only
  updates on baseline-change pushes, so this was likely a
  mid-settle frame. Worth one manual look.
- **Mode-card click that "didn't take"** — in one rapid-toggle
  sequence the final Live-effects click appeared to land but the
  system settled on `animation_playback`. The engine event log
  (`shots/events-rapid-toggle.log`) showed all four requests
  serialized correctly with last-wins semantics, so this was most
  likely a missed QA click on a stale-enabled card rather than a
  dropped request. Engine behavior under rapid toggling is clean.

## Verified working (no bug found)

- Live preset switch + save; rapid re-picking before save;
  last-writer-wins save race (`saving_` guard holds).
- Picking an uploaded preset → save → auto-request
  `animation_playback` → upload active (and the inverse).
- Uploaded-preset params: `upSpeed` slider and `upHue` color reach
  the engine (`parameter_revision` bumps; `param_speed`/color in
  `playback` view).
- Transport: **Pause** holds a bit-identical frame (321/321 pixels
  equal across 2 s), **Play** resumes motion, **Stop** → black is
  *documented* ("Stop replaces playback with black") and the status
  line is honest. Minor nit: the phase pill still says "Active"
  while stopped.
- Scene brightness slider → applied + physically dimmer output.
- Group chip select + palette apply → `object_colors` lands on
  exactly the selected group (`o-mb1 = #FF6966`).
- Conflict detection on remote `set-scene` while dirty →
  `conflict:true`, edits kept; Discard reloads applied state.
- Profiles: save to library, change scene, load back — clean.
- Reactive presets (`shockwave`, `ambient`) degrade gracefully to
  static output when their inputs are off.
- Empty-name save is refused before send (`src:"input"`, `sent:0`)
  — the refusal itself is correct; only the recovery is broken
  (BUG-4).
- Devices page: per-controller rates plausible (fan lane ~25/s,
  wired ~48/s); `stateReport` staleness is per-page refresh, not a
  bug.

## Harness notes — `tools/qa/qa.py` changes

Extended during this session (all in `tools/qa/qa.py`):

- `reveal SEL` op + `click SEL --reveal` flag: scrolls the target
  into view and requires the rect to be **stable across two
  samples** before clicking. Without this, `click` resolves
  coordinates once while the Flickable is still decelerating —
  clicks silently miss, and off-window items report
  `visible:true` and get "clicked" at negative Y with `ok:true`.
- `shot PATH` now makes the parent dir and resolves the path
  absolutely — the app resolves relative paths in *its* cwd and
  will not `mkdir`.

Known remaining harness caveats: selectors match the first item in
tree order (`text:o-mb1` hits a ColorWell before the GroupChip —
use `{"text":"o-mb1","type":"GroupChip"}`); `eval` can't reach
arbitrary QML ids (use `objectName` + `get`/`eval` on context
objects); AppSliders only commit on pointer interaction
(`clickxy` on the groove), not `set value`.

## Artifacts

`tools/qa/shots/` — screenshots (`bug1-*`, `bug4-saveblocked`,
`transport-stopped`, `devices`, second pass `r2-*`), SDK readbacks
(`sdk-*.json`, `r2-sdk-*.json`), engine event captures (`events-*.log`,
`r2-events*.log`), scene docs (`scene-*.json`).
