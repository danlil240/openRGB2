# R-3: Per-app/game profiles (foreground watcher, engine-side)

Status: proposed · Size: M · Source: audit 2026-11-28 R-3 · Depends on: R-2 (`Automation`, `AUTOMATION_COMMAND`)

## Goal

When a chosen program comes to the foreground (a game, Blender, a
video player), the engine switches to the workspace bound to it. When
the program leaves the foreground or exits, the engine switches back.
It runs in the engine, so it works with the UI closed. Alt-tab churn
never causes a reload storm.

## Current state

- **No foreground watcher exists.** `src/platform/windows/` has the
  power monitor, the audio, screen and key sources, and nothing else.
- **Workspace switching:** the `reload {id}` job does `Open` plus
  `apply_scene` (`src/engine/WorkspaceService.cpp:2190-2252`). Jobs are
  per session (`Enqueue(session_id, …)`, `:1320`). IPC session ids
  start at 1 (`src/ipc/IpcServer.h:135`), so session 0 is free for
  engine-internal jobs.
- **Persistence:** the audit points at `ProfileStore.h:82`
  (`EngineState`). It has `active_workspace`, the workspace adopted
  last, which boot re-opens. An optional new member survives older
  builds through `extensions` (`src/engine/ProfileStore.cpp:764-796`).
- **UI side:** a foreign adopt (a different workspace id in the
  `workspace` event) is already handled
  (`src/ui/WorkspaceController.cpp:573-600`). A dirty draft stays in
  the draft store (Task 10).
- After R-2: `Automation` (Qt-free), `AUTOMATION_COMMAND`, the
  `automation` event, and internal-session reload enqueue exist.

## Design

**Platform: `WinForegroundWatcher`**
(`src/platform/windows/WinForegroundWatcher.h/.cpp`).

- A thread that calls
  `SetWinEventHook(EVENT_SYSTEM_FOREGROUND, …, WINEVENT_OUTOFCONTEXT)`
  and pumps messages.
- On each event it resolves `GetWindowThreadProcessId`, then
  `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)`, then
  `QueryFullProcessImageNameW`, and emits
  `{exe_basename_lower, pid, fullscreen}`. `fullscreen` comes from
  `SHQueryUserNotificationState` (`QUNS_RUNNING_D3D_FULL_SCREEN` or
  `QUNS_BUSY`). R-4 reuses it to skip idle dimming.
- A process exit is noticed when the next foreground event names
  another process, plus a 2 s re-check of the matched pid on the owner
  loop.
- The sink only queues.

**Engine: `AppRules`** (in `src/engine/Automation.*`, pure logic).

- Each rule is `{exe, workspace_id, enabled}`. `exe` is a basename,
  matched case-insensitively. Full paths are not stored. At most 64
  rules.
- The arbiter state machine runs on the owner loop with an injected
  clock:
  1. A foreground change starts a settle timer (default 1.5 s).
  2. When the timer expires on a matched exe whose workspace is not
     already active, the arbiter remembers `return_to` (the active id
     at the time) and enqueues `reload {rule.workspace_id}`.
  3. When the matched app leaves the foreground or exits, the same
     settle runs, then the arbiter reloads `return_to`.
- **Manual override wins.** A user open while a rule is active (a
  workspace adopt the arbiter did not request) clears `return_to`, and
  the arbiter stays quiet until the matched app goes away.
- **Missing workspace:** a rule whose `workspace_id` no longer exists
  reports `state:"missing"` and does nothing.
- **Gates:** nothing switches while the calibration session holds its
  lease, or while the power gate is on. Those are checked through the
  existing `CalibrationSession` and `PowerLifecycle::OutputPaused()`.
- Persisted as `EngineState.automation.app_rules` plus
  `automation.app_rules_enabled` (a master switch, default off).

**IPC:** add `set_app_rules {enabled, rules:[…]}` to
`AUTOMATION_COMMAND`. The AUTOMATION body gains:

- `app_rules:{enabled, rules:[{exe, workspace_id, state}]}`
- `foreground:{exe, matched_rule?}`

`foreground` is reported only while the automation section is enabled,
or while the UI asks for it with the `peek_foreground` verb, so the
"add current app" button works.

**UI:** an "App profiles" card on `ui/pages/SettingsPage.qml`.

- A rule list with an exe field and a workspace combo
  (`workspace.workspaceList`), and a "use current foreground app"
  button.
- Each rule has a status pill (active, waiting, missing).

**Privacy:** the engine never logs or reports window titles or full
paths, only basenames. `foreground` is not part of DIAGNOSTICS.

**Rejected alternatives**

- A UI-side watcher: it stops when the UI exits.
- Polling `GetForegroundWindow` at 2 Hz: it is laggier and costs CPU.
  The WinEvent hook is push-based.
- Matching on window title: titles are private and change at run
  time.
- Looks instead of workspaces: a look switch needs a document commit
  (see R-1 open question).

## Steps

1. `AppRules` arbiter: settle, return-to, manual override, missing
   id, gates, and persistence round-trip. Test: `automation_test`
   (fake clock, fake foreground feed, fake active-id feed).
2. Owner-loop wiring in `EngineHost::PollOnce`
   (`src/engine/EngineHost.cpp:1542`): drain the watcher queue, feed
   the arbiter, enqueue reloads under session 0. Test:
   `e2e_studio_workspace_test` with an injected foreground source. Two
   workspaces, a match, a return, and a manual override.
3. The `WinForegroundWatcher` platform thread, wired behind
   `EngineHostOptions::foreground_watcher` (default true; tests
   inject the source). Manual test.
4. IPC: `set_app_rules`, `peek_foreground`, the body and event
   fields. Update `docs/ipc-protocol.md`. Test:
   `ipc_engine_service_test`.
5. Bridge and Settings card. Tests: `ui_bridge_test` and
   `qml_smoke_test`.

## Tests

- Engine label: `automation_test`, `e2e_studio_workspace_test`,
  `ipc_engine_service_test`, `profile_store_test`.
- UI label: `ui_bridge_test`, `qml_smoke_test`, and
  `ui_workspace_recovery_test` (a rule-driven switch with a dirty
  draft; shared with R-2).
- **Manual:**
  - A real game in exclusive fullscreen and in borderless mode.
  - An elevated foreground app: `PROCESS_QUERY_LIMITED_INFORMATION`
    should work from the elevated engine.
  - Alt-tab rapidly for 10 s: at most one reload settles.
  - Quit the game: the engine returns to the previous workspace.

## Risks and open questions

- **Daniel:** settle time. 1.5 s is a guess. A workspace reload is a
  full Open plus apply. Its cost on the wireless group (a possible
  re-upload of a loop) is not measured.
- **Daniel:** when two rules could match (a launcher and the game),
  should the last foreground app win (this plan) or should rules have
  explicit priority?
- An anti-cheat that blocks `OpenProcess` on the game makes the basename
  unknown. The rule then shows "waiting" and does not match. Not
  verified.
- A switch away from a workspace with an uploaded wireless loop
  re-uploads when the user comes back. Check the transition cost
  before enabling rules for loop-heavy workspaces.
