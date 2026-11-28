# R-4: Schedule / lock / idle dimming

Status: proposed · Size: M · Source: audit 2026-11-28 R-4 · Depends on: R-1 (`OutputControl` contributors), R-2 (`AUTOMATION_COMMAND`)

## Goal

The lights dim or turn off on their own in three cases:

- during a nightly time window (for example 23:30–07:00 at 30 %)
- while the session is locked
- after N minutes without keyboard or mouse input

Each case has its own level, is opt-in, and lifts immediately when its
condition ends. Fullscreen games never trigger the idle rule.

## Current state

- **`WinPowerMonitor`** (`src/platform/windows/WinPowerMonitor.h`).
  - It registers `GUID_CONSOLE_DISPLAY_STATE`,
    `GUID_SESSION_DISPLAY_STATUS`, and suspend/resume on a message-only
    window.
  - It feeds `PowerLifecycle::PostEvent`
    (`src/engine/PowerLifecycle.h:62-70`: `DisplayOff/On`,
    `Suspend/Resume`).
  - Lock and user idle are not handled.
- **`PowerLifecycle`** gates output on display-off. The gate freezes
  the transport. It does not dim, so the LEDs keep the last frame.
  That behaviour stays as it is.
- After R-1, `OutputControl` combines named contributor scales into
  one effective scale that `DeviceMapper` applies. After R-2,
  `AUTOMATION_COMMAND` and `EngineState.automation` exist.
- The owner loop runs `PollOnce` every ~500 ms
  (`src/engine/EngineHost.cpp:1801-1831`). That is fast enough for
  minute-scale rules.

## Design

**Three contributors** in `OutputControl`: `"schedule"`, `"lock"`, and
`"idle"`.

- Each contributes a scale in 0..1, where 0 means off. The effective
  scale is `user_master × min(active contributor scales)`, so
  overlapping rules give the strongest dim, not a product of dims.
- Blackout (R-1) still forces 0.
- Each change ramps over ~600 ms, through the existing 60 Hz mapping,
  so dimming is not a hard step. Restore is immediate. Both values are
  configurable.

**Pure logic: `DimRules`** (`src/engine/Automation.*`).

- **Schedule:** windows of `{days:[0..6], start:"HH:MM",
  end:"HH:MM", level}`. A window may cross midnight. Local wall time
  comes from an injected `wall_now()` that returns local date and
  time, so tests never touch the real clock or time zone.
- **Lock:** `{enabled, level}`, driven by lock and unlock events.
- **Idle:** `{enabled, after_s, level, skip_fullscreen:true}`, driven
  by an injected `idle_seconds()` and a `fullscreen()` flag.
- Output: a map from contributor to scale, recomputed on each
  `PollOnce`, plus on lock events right away.

**Platform**

- **Lock.** In `WinPowerMonitor`, call
  `WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION)` and
  handle `WM_WTSSESSION_CHANGE` (`WTS_SESSION_LOCK` and `UNLOCK`). Two
  new `PowerLifecycle::Event` values (`SessionLock`, `SessionUnlock`)
  pass through the same queue. `PowerLifecycle` ignores them. The
  `DimRules` feed reads them.
- **Idle.** `GetLastInputInfo` returns the session-wide last input
  tick. It is polled from the owner loop and needs no thread. It works
  because the engine runs in the interactive session.
- **Fullscreen.** `SHQueryUserNotificationState`, shared with R-3's
  watcher. If R-3 has not landed, call it directly in the poll.

**IPC:** add `set_dimming {schedule:[…], lock:{…}, idle:{…}}` to
`AUTOMATION_COMMAND`. The AUTOMATION body gains
`dimming:{config, active:{schedule, lock, idle}, effective}`. The R-1
OUTPUT body already lists contributors, so the UI can say "dimmed by
schedule". Persisted in `EngineState.automation.dimming`.

**UI:** a "Dimming" card on `ui/pages/SettingsPage.qml` with these
controls:

- schedule rows (days, from, to, level)
- a lock toggle with a level
- an idle toggle with minutes and a level

The tray tooltip (R-1) shows the active reason.

**Rejected alternatives**

- `GUID_SESSION_USER_PRESENCE`: it follows the Windows power idle
  timeout, not a user-chosen delay.
- Using the display-off gate for "off": it freezes the transport
  instead of writing black, so another app could take the device.
- A schedule in the UI: it stops when the UI exits.
- Multiplying contributor scales: two 50 % rules would give 25 %,
  which surprises.

## Steps

1. `DimRules`: windows across midnight, day masks, the `min()`
   arbitration, idle with fullscreen skip, and lock. Test:
   `automation_test` with a fake wall clock (including DST change dates
   as plain local times; see Risks).
2. Contributor ramps in `OutputControl`. Test: `output_control_test`
   checks that the scale is monotonic over ramp ticks and that restore
   is immediate.
3. Lock events in `WinPowerMonitor` and `PowerLifecycle::Event`.
   Test: `power_lifecycle_test` checks that the new events are accepted
   and do not change the gate. The platform part is a manual test.
4. Idle and fullscreen polling in `EngineHost::PollOnce`, behind
   injectable `EngineHostOptions` sources. Test:
   `e2e_fake_backend_test` checks that injected idle seconds dim the
   mapped colors to the configured level and that input restores them.
5. IPC `set_dimming`, the body, and persistence. Update
   `docs/ipc-protocol.md`. Tests: `ipc_engine_service_test` and
   `profile_store_test`.
6. Bridge and Settings card. Tests: `ui_bridge_test` and
   `qml_smoke_test`.

## Tests

- Engine label: `automation_test`, `output_control_test`,
  `power_lifecycle_test`, `e2e_fake_backend_test`,
  `ipc_engine_service_test`, `profile_store_test`.
- UI label: `ui_bridge_test`, `qml_smoke_test`.
- **Manual:**
  - Win+L dims within ~1 s and unlock restores.
  - The idle rule at 1 min dims, and a mouse move restores.
  - The idle rule does not fire during a fullscreen game. It is
    expected to fire during gamepad-only play in a windowed game; see
    Risks.
  - A schedule window that crosses midnight on the real clock.
  - An uploaded wireless loop is reported unscaled (R-1). Only
    blackout-level rules (level 0) stop it.

## Risks and open questions

- **Daniel:** should a level-0 rule (off) stop an uploaded wireless
  loop with stop-black, as R-1 blackout does? Or should it leave the
  loop running, because a re-upload on restore costs time?
- **Daniel:** should lock imply off by default?
- Gamepad input probably does not update `GetLastInputInfo` (guess,
  not verified). The fullscreen skip covers most games, and windowed
  gamepad play may still dim.
- `WM_WTSSESSION_CHANGE` delivery to a message-only window is expected
  but not verified. P-11 notes that this window misses
  `WM_ENDSESSION`, which is a broadcast. If lock events do not arrive,
  fall back to a hidden top-level window. That fallback is shared with
  the E-10 fix.
- DST: windows are local wall times. A window whose start falls in the
  skipped hour on a spring-forward day starts at the next valid
  minute.
