# R-2: Global hotkeys (next profile, brightness, lights off)

Status: implemented on `feat/r-2-r-4-hotkeys-dimming` (not merged) · Size: M · Source: audit 2026-11-28 R-2 · Depends on: R-1 (`OutputControl`, `OUTPUT_COMMAND`)

## Goal

System-wide key combinations, active in any foreground app (games
included), for these actions:

- next or previous workspace
- master brightness up or down
- lights off/on
- pause/resume

They work while the UI is closed ("Exit UI (keep lighting on)").
Bindings are opt-in and can be edited, and a combination that cannot
be registered is reported, never silently dropped.

## Current state

- `src/platform/windows/WinKeyHook.cpp` is the reactive key source. It
  installs a `WH_KEYBOARD_LL` hook that only listens: it never consumes
  keys (`:84`, `CallNextHookEx`). It runs only while the "key" reactive
  input is enabled, and pushes VK edges into the `InputBus` for
  ripples. It is the wrong tool for commands.
- `WinPowerMonitor` (`src/platform/windows/WinPowerMonitor.h/.cpp`)
  already shows the pattern this needs: a dedicated thread with a
  message-only window, started from `EngineHost::Start`
  (`src/engine/EngineHost.cpp:1054`) behind an option flag, with events
  queued to the owner loop.
- After R-1, the engine has `OutputControl` (master, blackout, hold).
  A workspace switch is the `reload {id}` job (Open plus apply,
  `src/engine/WorkspaceService.cpp:2190-2252`).
- No hotkey or automation configuration exists anywhere. `EngineState`
  (`src/engine/ProfileStore.h:82`) keeps unknown keys in `extensions`,
  so an optional new member is backward compatible.

## Design

**Where it runs: the engine.** The UI can exit while lighting keeps
running, and the engine already owns a Win32 message thread pattern.
The engine runs elevated in the interactive session
(`packaging/windows/RELEASE.md:67`), so `RegisterHotKey` works with
any foreground window.

**Platform: `WinHotkeys`** (`src/platform/windows/WinHotkeys.h/.cpp`).

- A thread with a message-only window that calls
  `RegisterHotKey(hwnd, id, mods | MOD_NOREPEAT, vk)`.
- `RegisterHotKey` must be called on the owning thread, so
  re-registration is posted to it as a `WM_APP` message carrying the
  new binding set.
- `WM_HOTKEY` calls a sink with the action id. The sink only queues.
- Each binding gets a per-binding result: `ok`, `in_use` (the
  combination is taken by another app or is reserved by Windows), or
  `invalid`.

**Engine: `HotkeyActions`** (Qt-free, `src/engine/Automation.h/.cpp`,
shared with R-3 and R-4).

- `ParseChord("Ctrl+Alt+PgUp") -> {mods, vk}`, with names from the
  `VkDefs.h` table.
- Validation: at least one modifier from Ctrl/Alt/Win, no duplicate
  chords, at most 16 bindings.
- The action dispatcher runs on the owner loop:
  - `next_workspace` / `prev_workspace`: cycle the workspace `list`
    order (or an optional `cycle` id list). It enqueues `reload {id}`
    under internal session 0. IPC session ids start at 1
    (`src/ipc/IpcServer.h:135`).
  - `brightness_up` / `brightness_down`: ±`step` (default 10 %) on the
    user master, clamped.
  - `lights_toggle`: blackout.
  - `pause_toggle`: hold.
  - Repeated presses within the 1 s debounce collapse to the latest
    target workspace, so a burst does not queue five reloads.
- `EngineState.automation.hotkeys`: `[{action, chord}]`. Empty by
  default.

**IPC:** a new `AUTOMATION_COMMAND = 0x0110` with these verbs:

- `get`
- `set_hotkeys {bindings:[{action, chord}]}`

They reply with the AUTOMATION body:
`{hotkeys:[{action, chord, state, detail?}], …}`. A new event kind
`automation` carries the same body, and the welcome reply advertises
capability `automation_v1`. R-3 and R-4 add sections to this same body.

**UI:** a Hotkeys card on `ui/pages/SettingsPage.qml`.

- Per action: a chord capture field and a status pill ("in use by
  another app").
- It is driven through `EngineBridge::automationCommand` and the
  `automation` property.
- Capturing a chord is a UI-local key-event read. It sends only the
  chord string.

**Rejected alternatives**

- Extending the `WH_KEYBOARD_LL` hook: it would have to consume keys,
  it is bound to the reactive "key" toggle, and LL hooks have a
  system timeout that Windows enforces by silently unhooking.
- UI-side `QHotkey`-style registration: it stops working when the UI
  exits.
- Default bindings on by default: they collide with games and other
  apps. Defaults are proposed, not registered.

## Steps

1. `Automation.h/.cpp`: the chord parser, binding validation, and the
   `EngineState.automation` round-trip. Tests: new `automation_test`
   (engine) and `profile_store_test` (an unknown key survives,
   round-trip).
2. The action dispatcher against `OutputControl` and an injected
   `enqueue_reload`, plus the workspace cycle order and burst debounce.
   Test: `automation_test` with a fake clock and a fake workspace list.
3. `WinHotkeys` platform thread, wired in `EngineHost` behind
   `EngineHostOptions::hotkeys` (default true; tests false).
   Manual test only (platform code is not loaded by the offline
   suites, `src/platform/windows/CMakeLists.txt` header).
4. `AUTOMATION_COMMAND` with `get` and `set_hotkeys`, the event and
   the capability. Update `docs/ipc-protocol.md`. Test:
   `ipc_engine_service_test` with a fake registrar that reports
   `in_use` for one chord.
5. Bridge property and verb. Test: `ui_bridge_test`.
6. Settings page card. Tests: `qml_smoke_test` loads the page, and
   `ui_qa_harness_test` covers the capture field.
7. An engine-initiated workspace switch while the UI draft is dirty:
   the draft stays in the draft store and the UI flags it. Test:
   `ui_workspace_recovery_test`.

## Tests

- Engine label: `automation_test` (new), `profile_store_test`,
  `ipc_engine_service_test`.
- UI label: `ui_bridge_test`, `qml_smoke_test`,
  `ui_workspace_recovery_test`.
- **Manual:**
  - Each action from a fullscreen game and from an elevated window.
  - A chord already taken by another app shows `in_use`.
  - Hotkeys keep working after "Exit UI".
  - Brightness steps on an uploaded wireless loop report that the loop
    is unscaled (R-1).

## Risks and open questions

- **Daniel:** which default chords to propose? Suggestion:
  Ctrl+Alt+Shift + PgUp/PgDn (workspace), Up/Down (brightness),
  End (lights), Pause (hold). None is registered until he enables
  them.
- **Daniel:** should "next profile" cycle all workspaces or a curated
  favourites list? The plan supports both through the optional `cycle`
  list.
- A hotkey firing while the user is typing in the OpenRGB2 window
  still fires. That is acceptable for modifier chords.
- `WM_HOTKEY` delivery to a message-only window is expected to work
  but must be checked on the target machine (guess, not verified).

## Implementation decisions (on the branch)

- Nothing is bound by default. The plan's chords are offered as
  `proposed` in the AUTOMATION body and by a "Use suggested keys"
  button: Ctrl+Alt+Shift + PgDn (next) / PgUp (previous), Up / Down
  (brightness), End (lights), Pause (hold). Unverified: Ctrl+Pause
  usually arrives as VK_CANCEL, so the suggested pause chord may never
  fire — check it on the target machine and pick another key if so.
- "Next profile" cycles every managed workspace in `list` order; the
  optional `cycle` list (set through `set_hotkeys`) is the curated
  favourites list. Ids that no longer exist are skipped.
- Burst debounce: the first press reloads at once (no 1 s latency);
  presses within 1 s of the previous one only move the target, and the
  burst ends with one reload of the latest target — none if that is
  the workspace already loading. A burst of five presses costs at most
  two reloads.
- The VK name table lives in `Automation.cpp` as literal VK codes
  (the same values as `src/core/inputs/VkDefs.h`) so the Qt-free engine
  never includes `<windows.h>`.
- Validation failures (unknown action, unparsable chord, no
  Ctrl/Alt/Win, duplicate chord, more than 16) refuse the whole
  `set_hotkeys` (`bad_request`); an OS refusal is a per-binding state
  (`in_use` on ERROR_HOTKEY_ALREADY_REGISTERED, else `invalid`), never
  a verb failure. A persisted binding that no longer validates is kept
  and reports `invalid`.
- The hotkey thread starts lazily on the first non-empty set (an engine
  with nothing bound never creates a window) and stops inside the
  existing `power-monitor` shutdown step, so the step list is
  unchanged. Presses run on the 17 ms owner fast tick.
- Reply type AUTOMATION is `0x8108`. The body also carries `actions`,
  `proposed`, `registrar` and `max_hotkeys`.
- Step 7: an engine-initiated switch over a dirty draft flushes the
  draft record at once (it sat in the draft store's debounce before),
  releases it as a recovery candidate, raises a notice and sets
  `WorkspaceController.switchedAwayDraft`; a poll reply that raced the
  event and still names the old id is the same switch (no second
  notice).
