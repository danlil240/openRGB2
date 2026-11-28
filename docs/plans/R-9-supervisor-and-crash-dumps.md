# R-9: Supervisor — restart a dead engine or backend, crash dumps, wireless wedge escalation

Status: implemented on `feat/r-9-supervisor` (not merged) · Size: M · Source: audit 2026-11-28 R-9

## Goal

- If the engine or the backend crashes, the lights come back within
  seconds without user action, and a crash dump is left behind to
  debug.
- If a Lian Li wireless group stays terminally wedged after the
  backend's own re-arm has given up, the backend is restarted once,
  with a bounded budget.
- Every restart is visible: a tray message, a Diagnostics line and the
  log.

## Current state

- **The UI launches, but nothing supervises.**
  - The packaged UI runs `scripts/start-openrgb2.ps1 -HideWindows -NoUI`
    once, through a `QProcess` (`src/ui/main.cpp:610-776`). The exit code
    is read once for 0, 3 (ownership conflict) and 4 (unverifiable).
  - After `bridge.start()` (`:820`), an engine death only shows as the
    bridge reconnect banner (`ui/Main.qml:387-420`). No one restarts it.
- **The script is idempotent.**
  - It reuses a running managed backend and does not start a second
    engine (`packaging/windows/start-openrgb2.ps1:539-616`).
  - It rewrites `state\openrgb2.run.json`.
  - `stop-openrgb2.ps1` resolves pids from the run file, with an
    image-path fallback (`Resolve-OwnPid`, `:305-356`).
- **The engine already knows its backend.**
  - `--backend-exe` and `ResolveManagedBackend` verify the listener pid
    by image path (`EngineHost.cpp:240`, `:1207-1230`).
  - A verified-own backend can be closed with `WM_CLOSE`
    (`pending_backend_pid_`, `:1919-1945`).
  - The engine re-probes a missing backend on a bounded schedule, but
    never starts one.
- **Wireless wedge.**
  - The backend has its own recovery: `TransportFailed`, then
    `RetryTransport` every `reconnect_ms` = 3 s; terminal wedges
    (`Failed && !retry_transport`, or `ForeignMaster`) are re-armed on
    a timed backoff capped at `rearm_max_ms` = 240 s
    (`vendor/OpenRGB/Controllers/LianLiWirelessController/LianLiWirelessRuntime.cpp:33-40`,
    `:153-196`, `:310-323`; `LianLiWirelessRuntime.h:95-97`).
  - The engine sees per-group `state` and `wedged` through
    `sdk->WirelessStatus` (`EngineService::BackendCountersJson`,
    `src/ipc/EngineService.cpp:1192-1240`).
  - The UI drops `frame_status` while it is hidden
    (`EngineBridge.h:106-110`), so a UI-side wedge watchdog would be
    blind exactly when it matters.
- **Crash dumps.** None. No `MiniDumpWriteDump`, no
  `SetUnhandledExceptionFilter`, no WER `LocalDumps` configuration
  anywhere in `src/` or `packaging/`. The audit backlog notes that the
  F01/F02 crashes never got an ASan or App Verifier pass.

## Design

**Each parent supervises its child.**
- **UI supervises the engine** (managed mode only). A new
  `EngineSupervisor` QObject in `src/ui/`:
  - It reads `engine_pid` from the run file after a successful start,
    opens the process with `SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION`,
    and watches the handle with a `QWinEventNotifier`.
  - On exit it restarts **only if** all of these hold:
    1. The exit code is not 0. The ordered `Quit` path is expected to
       exit 0; this is to be confirmed in `shutdown_test`.
    2. The UI did not itself start the stop script or send `Quit`.
    3. No stop is in progress. `stop-openrgb2.ps1` removes the run file
       only at its end (`:436-437`), after the engine has gone, and its
       `-Force` path kills the engine with a non-zero code. So the stop
       script must first write a `state\openrgb2.stopping` marker,
       which the supervisor checks and the script deletes on exit.
  - A restart is `run_start(false)`, the existing lambda (`main.cpp:660`),
    so the ownership gate and logging are reused.
  - Budget: three restarts per 10 minutes with backoff (2 s, 10 s,
    30 s). After that it stops and shows a persistent notice.
- **Engine supervises the backend.** A new `BackendSupervisor` in
  `src/engine/`, ticked from the maintenance poll beside the owner
  re-scan (`EngineHost.cpp:1584-1592`).
  - **Dead backend:** the SDK link has been down for more than 5 s, the
    `ResolveManagedBackend` pid is 0, and nothing (or our own image)
    holds the port. It then spawns the backend with the script's exact
    arguments (`--server --server-host 127.0.0.1 --server-port <port>
    --noautoconnect --localconfig`, cwd `<backend-config>`, hidden).
    It never spawns while a foreign process holds the port.
  - **Wireless escalation:** a group has reported `wedged` with
    `state == "failed"` continuously for more than
    `2 × rearm_max_ms` (8 min), so the backend's own re-arm has had at
    least two tries. It then closes the verified-own backend with the
    existing `WM_CLOSE` path, and the dead-backend rule respawns it.
    `foreign_master` never escalates, because another master owns the
    group and a restart cannot fix that.
  - Budget: two wedge restarts per hour and five spawns per hour, then
    it stops and reports.
  - The script parity is guarded by a test, in the same spirit as the
    T1 owner-rule parity.
- **Visibility.**
  - `STATE.supervisor` reports `{backend_restarts, last_reason,
    last_at, budget_exhausted}`.
  - A new event kind, `supervisor`, carries each action. The UI shows a
    tray `showMessage` and a Diagnostics row.
  - The UI keeps its own engine-restart history in the same card.
- **Crash dumps.**
  - Engine and UI: an in-process `SetUnhandledExceptionFilter` calls
    `MiniDumpWriteDump(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory)`
    into `state\crash\<exe>-<utc>.dmp`, keeps the newest 5, and calls
    `SetThreadStackGuarantee` so stack overflows still dump.
  - The backend is vendored: no in-process change. See the decision
    below.

**Rejected:**
- A Windows service supervisor. It runs in session 0, which breaks the
  per-user pipe ACL, Dynamic Lighting HKCU reads and screen capture.
- The UI re-running the start script for backend death. The UI may be
  hidden (no frame feed), and the engine already owns
  backend-verification logic.
- Restarting on `ForeignMaster`. A restart cannot win against another
  master.

## Steps

1. Add `CrashDump.{h,cpp}` (shared, Qt-free; `dbghelp`) and install it
   first thing in `src/engine/main.cpp` and `src/ui/main.cpp`. Test: a
   `crash_dump_test` (label `engine`) that runs a helper exe with
   `--crash-for-test`, then asserts that a `.dmp` exists and the oldest
   is pruned beyond 5.
2. Add the pure `SupervisorPolicy` (budgets, backoff, wedge-duration
   rule) with no I/O. Test: a `supervisor_policy_test` (label `engine`)
   with a table-driven clock: dead link, wedge for 8 min, a
   `foreign_master` that never escalates, and budget exhaustion.
3. Add the `BackendSupervisor` spawn path and the argument builder,
   plus a parity test that parses `start-openrgb2.ps1` for the backend
   `ArgumentList`. Test: an `e2e_fake_backend_test` case where the fake
   backend dies, the supervisor "spawns" through an injected launcher,
   and the link recovers.
4. Add wireless escalation through the `WM_CLOSE` path, the `STATE` and
   `supervisor` event, and the docs (`docs/ipc-protocol.md`). Test: an
   `ipc_engine_service_test` event-shape case; a supervisor test with an
   injected `WirelessStatus` sequence.
5. Add the `state\openrgb2.stopping` marker to `stop-openrgb2.ps1`:
   write it before step 1, remove it in `finally`. Test: a
   `package_lifecycle_tests.ps1` assertion that the marker exists
   during the stop and is gone after it.
6. Add the UI `EngineSupervisor` (process watch, the exit-code,
   stopping-marker and Quit guards, budget) wired to `run_start`.
   Test: a `ui_supervisor_test` (label `ui`) with an injected
   process-exit source and a fake state directory: an intended stop
   gives no restart; a crash restarts; the fourth crash in 10 minutes
   gives a notice.
7. Add the Diagnostics card rows and the tray messages. Test: a
   `qml_diagnostics_test` case that `supervisor` data renders.
8. Packaging, if Daniel picks WER: `update-openrgb2.ps1` (elevated)
   writes the `LocalDumps\OpenRGB.exe` keys, and the
   rollback/uninstall path removes them. Test: a `launcher_tests.ps1`
   dry-run assertion.

## Tests

- New: `crash_dump_test`, `supervisor_policy_test` (`engine`) and
  `ui_supervisor_test` (`ui`).
- Extended: `e2e_fake_backend_test`, `ipc_engine_service_test` and
  `qml_diagnostics_test`.
- Manual, on the packaged install:
  - `taskkill /F` the engine: it is back within about 2 s.
  - `taskkill /F` the backend: it is respawned, and the wired and
    wireless output resumes.
  - `stop-openrgb2.ps1`: nothing restarts.
  - A wireless wedge escalation needs a real wedge. There is no
    repeatable trigger today: N01 is "physical retest of the wireless
    wedge" (see the audit's open backlog).

## Risks and open questions

- **Backend crash dumps (Daniel decides).**
  - WER `LocalDumps` per-exe keys are HKLM: a system setting written by
    the elevated installer.
  - Or no backend dumps at all.
  - Plan default: WER for `OpenRGB.exe` only, set and removed by the
    install scripts, with a Settings note.
- **A respawned backend has a new pid that the run file does not know.**
  `Resolve-OwnPid` falls back to image path plus name, so stop still
  works. Confirm this in a `package_lifecycle_tests.ps1` case.
- **A crash loop on bad state** (a corrupt workspace). The budget stops
  it. The notice should point at the backup restore. I-11 (crash on a
  wrong-typed manifest) would hit exactly this path, so fix I-11 first.
- **Restarting the backend drops the uploaded loop.** The engine
  re-applies on reconnect, but confirm the playback re-upload on
  hardware.

## Implementation record (feat/r-9-supervisor)

Decisions taken while implementing (each is the plan's
recommendation or the conservative option; Daniel may overrule):

- **Backend crash dumps: WER `LocalDumps\OpenRGB.exe`** (the plan
  default). Set by an *elevated* `update-openrgb2.ps1` (DumpFolder
  `<install>\state\crash`, DumpCount 5, custom flags 0x1040 like the
  engine, plus an `OpenRGB2InstallDir` owner value); a key without
  that owner value is never overwritten; an elevated rollback removes
  only the key that names its own install. The note lives on the
  Devices & Diagnostics "Restarts & crash dumps" card, not in Settings.
- **Shared code in `src/supervise/`** (`CrashDump`, header-only
  `RestartLedger`): Qt-free and engine-free, so the UI links it
  without carrying the engine/SDK boundary.
- **Default dump dir `<exe dir>\state\crash`** (`OPENRGB2_CRASH_DIR`
  overrides) — no new launcher argument. The filter writes from a
  helper thread and then ends the process with the exception code
  (no WER dialog or delay for the engine/UI); `__fastfail` crashes
  bypass it. `openrgb2-engine --crash-for-test av|stack|abort` exists
  for the manual check.
- **Restart = `run_start(false)`**: a restart never carries a
  takeover; a conflict that appeared since goes through the existing
  ask/notify path.
- **Wedge close does not wait** for the backend to exit (the owner
  loop must not block 8 s) and never kills; a backend that ignores
  the signal keeps its pid, so it is never doubled.
- **Stop marker**: both supervisors ignore a marker older than 10
  minutes (a stop that died mid-way), so a stale file cannot disable
  supervision forever.
- **Log**: every action (engine and UI side) is one line in
  `state\supervisor.log` — the engine's console is hidden in the
  managed layout.
- **WER test** lives in `package_lifecycle_tests.ps1` (T17, which
  already dot-sources update/rollback) rather than
  `launcher_tests.ps1` (which covers `OpenRGB2.cmd` only).
