# R-1: Tray quick controls (profile, brightness, pause)

Status: proposed · Size: M · Source: audit 2026-11-28 R-1

## Goal

From the tray icon, without opening the window, Daniel can switch the
active workspace, set a master brightness (for example 100/75/50/25 %,
off), and pause or resume the lighting. The settings take effect on the
hardware within one tick and survive an engine restart. They never
leave the open workspace draft dirty.

## Current state

- The tray menu is built inline in `main()` from lambdas
  (`src/ui/main.cpp:488-576`). It has only Open, "Exit UI (keep
  lighting on)", and "Quit OpenRGB2". Nothing in it can be tested.
- **Profiles.** Two concepts exist. The legacy `ProfileLibrary`
  (`src/ui/ProfileLibrary.h`) holds UI-side named scenes and only
  applies outside workspace mode. Managed workspaces are the current
  model: `WorkspaceController::workspaceList`, and `openWorkspace(id)`
  (`src/ui/WorkspaceController.cpp:1097`), which latches a confirmation
  when the draft is dirty. In the engine, the `reload {id}` job is
  `Open` plus `apply_scene` (`src/engine/WorkspaceService.cpp:2190-2252`).
- **Brightness** exists only as a document field
  (`SceneDocument::brightness`, `src/core/scene/SceneTypes.h:260`).
  `DeviceMapper::MapFrame` applies it through `ScaleColor`
  (`src/engine/DeviceMapper.cpp:46`, `:341-343`). In workspace mode,
  `SceneEditController::setBrightness` is a draft edit (`WsCommit`,
  `src/ui/SceneEditController.cpp:772`). Nothing reaches the engine
  until Save (`src/ui/WorkspaceEditSession.h` header). A tray slider
  wired to it would dirty the draft and would not change the LEDs.
- **Pause.** Only uploaded loops have a transport control
  (`SET_PLAYBACK_MODE {control:"paused"}`). In live mode the engine
  forces Playing (`ui/pages/LightingPage.qml:563-590`). The power gate
  (`PowerLifecycle`, `DeliveryPipeline::SetOutputPaused`) freezes
  output for display-off. It is not a user control.
- **Missing:** an engine-side runtime output control that is not part
  of the document. R-2 (hotkeys) and R-4 (dimming) need the same
  control.

## Design

**Engine: `OutputControl`** (new, Qt-free, `src/engine/OutputControl.h/.cpp`).
It holds runtime output overrides that live outside any document:

- `master` (0..1) and `blackout` (bool). The effective scale is
  `blackout ? 0 : master × min(contributor scales)`. Contributors are
  named slots (`"schedule"`, `"lock"`, `"idle"` from R-4; none in this
  plan), so the arbitration is decided once, here.
- `hold` (bool), the user's pause.
- A change callback, and a `PersistedOutput {master, blackout}`.
  `hold` is not persisted: a restart resumes.

Wiring:

- `DeviceMapper` reads one `std::atomic<float>` effective scale. It
  multiplies that scale into the `ScaleColor(…, doc->brightness)` call,
  so live lanes reflect a change on the next tick (the worker evaluates
  every tick; the pipeline suppresses frames that did not change).
- **Hold, live mode:** a new `SceneWorker::SetHold(bool)`. While held,
  `Step` keeps `scene_time_s` at the held value and skips the input
  pull. On release the epoch shifts by the time spent held, so the
  animation resumes from the frame where it paused, and queued input
  events are cleared (`InputBus::ClearEvents`).
- **Hold, playback mode:** route to the existing
  `PlaybackController` control `paused`/`playing`. Uploaded loops are
  not scaled by `master` (`PlaybackParams` has no brightness,
  `src/engine/PlaybackPresets.h:58`). Blackout therefore uses control
  `stopped` (stop-black) and restores the previous control on release.
- `EngineState` gets an optional `output` member. Older builds keep
  unknown keys in `extensions` (`src/engine/ProfileStore.cpp:764-796`),
  so no schema bump is needed.

**IPC:** a new `OUTPUT_COMMAND = 0x010f`
(`src/ipc/IpcProtocol.h:93-139`) with these verbs:

- `get`
- `set_master {value}`
- `set_blackout {on}`
- `set_hold {on}`

Each verb replies with the OUTPUT body
`{master, blackout, hold, effective, contributors:{...},
uploaded_unscaled:bool}`. A new event kind `output` carries the same
body. The welcome reply advertises capability `output_control_v1`
(`EngineService::OnWelcome`, `src/ipc/EngineService.cpp:1247`). Clients
that do not see it hide the controls.

**UI:**

- `EngineBridge` gets `output` (a `Q_PROPERTY`) and `outputCommand(verb, args)`.
- A new `TrayController` QObject (`src/ui/TrayController.h/.cpp`)
  builds the menu model: a Workspaces submenu, Brightness presets plus
  Off, and Pause/Resume. The model is data. `main.cpp` only renders it
  into `QMenu` and keeps the existing three actions.
- A workspace pick calls `WorkspaceController::openWorkspace`. If that
  latches the dirty-draft confirmation (`replaceOffered`), the tray
  opens the window so the dialog is visible.
- The tooltip shows the workspace, brightness, and paused or off.

**Rejected alternatives**

- Tray brightness as a document edit: it dirties the draft and reaches
  no LEDs until Save.
- Reusing the power gate for pause: it freezes the transport, so
  another writer can take the device and the UI reports
  "display_off" semantics.
- UI-only state: it is lost when the UI exits ("keep lighting on"),
  and R-2/R-4 run without the UI.

## Steps

1. `OutputControl` class, with contributor arbitration and
   persistence types. Test: new `output_control_test` (engine).
2. Mapper scale hook in `DeviceMapper` and wiring in `EngineHost`.
   Test: `device_mapping_test`. A master of 0.5 halves the mapped
   colors, and `blackout` produces black at the same sequence cadence.
3. `SceneWorker::SetHold` (frozen t, no input pull, epoch shift on
   release). Test: `scene_worker_test` with synthetic `Step(now)` times.
4. Playback routing for hold and blackout (paused, stopped, then
   restore the prior control). Test: `playback_mode_test`.
5. `OUTPUT_COMMAND`, the `output` event, the capability, and
   `EngineState.output` round-trip and restore. Tests:
   `ipc_engine_service_test` and `profile_store_test` (an older file
   loads, and an unknown `output` key survives a save). Update
   `docs/ipc-protocol.md` §4, §5 and §7.
6. `EngineBridge.output` and `outputCommand`. Test: `ui_bridge_test`
   over the in-process pipe.
7. `TrayController`, with the menu model and actions, rendered in
   `main.cpp`. Test: new `ui_tray_test` (model entries, a disabled state
   while disconnected, the right bridge verb per action, a dirty-draft
   pick that opens the window).

## Tests

- Engine label: `output_control_test` (new), `device_mapping_test`,
  `scene_worker_test`, `playback_mode_test`, `ipc_engine_service_test`,
  `profile_store_test`.
- UI label: `ui_bridge_test`, `ui_tray_test` (new; offscreen, with the
  menu as data).
- **Manual:**
  - Tray switching between two workspaces on real hardware.
  - Brightness 25 % on the G512 and the SL wireless fans in live mode.
  - Pause in live mode, then pause and Off with an uploaded loop on the
    wireless group: the loop is paused or stop-black, never scaled.
  - Exit the UI and confirm the state holds.
  - Restart the engine and confirm master and blackout restore while
    hold does not.

## Risks and open questions

- **Daniel:** should "profile" in the tray mean a workspace (this plan)
  or a look inside the active workspace? Looks would need either a
  commit per switch (a new generation each time) or another runtime
  override.
- **Daniel:** should master brightness show in the draft preview? This
  plan says no. The preview shows the document, and `frame_status`
  colors already show what was sent.
- Uploaded loops cannot be dimmed without a re-upload. The OUTPUT body
  reports `uploaded_unscaled:true` so the UI can say so.
- Two brightness controls (document and master) may confuse. The
  Lighting page should show the master next to the document slider
  (small QML follow-up, not in this plan).
- An engine-initiated workspace switch while the UI draft is dirty
  relies on the existing foreign-adopt path in
  `WorkspaceController` (`:573`). R-2 and R-3 add the test for it.
