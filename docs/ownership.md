# Process ownership — intended arrangement

Milestone 3 deliverable (Task 3.9); implements IMPLEMENTATION_PLAN.md
section 4 ("Process ownership").

## Processes and what owns what

| Process | Owns | Started by |
| --- | --- | --- |
| `OpenRGB.exe` (vendored, `vendor/OpenRGB/release/`) | **all RGB hardware** — the sole hardware owner. USB/HID/SMBus access lives here and nowhere else | Externally, with `--server --server-host 127.0.0.1 --server-port 6744 --noautoconnect --localconfig`, its private configuration directory as working dir. The managed launcher `packaging/windows/start-openrgb2.ps1` (Milestone 6, shipped as `scripts\start-openrgb2.ps1`) starts it; the engine's backend supervisor (R-9) respawns it with the same arguments after it dies — see "What the engine does NOT do" for the exact gate |
| `openrgb2-engine.exe` | the scene clock, delivery pipeline, playback-mode state machine, IPC surface | User/launcher. **Attaches** to the backend over the SDK; never touches hardware directly |
| UI (Milestone 4) | nothing | attaches/detaches to the engine's named pipes freely — closing it never stops output. In a managed session it **supervises the engine** (R-9): it watches the run file's `engine_pid` (verified engine image, passive handle) and, on a non-zero exit it did not cause, re-runs `start-openrgb2.ps1` — the same ownership gate, and the script never starts a second engine. Never while `state\openrgb2.stopping` exists; 3 restarts per 10 minutes |
| `openrgb2-ctl` | nothing | one-shot IPC/SDK driver used for M3 verification |

## Startup ownership rules (enforced in `EngineHost::Start`)

1. **Single instance.** One engine owns the managed configuration. The
   guard is the named kernel object `Local\openrgb2-engine` (released by
   the OS on process death — no stale lock file). A second engine refuses
   to start and reports the existing owner; it never takes over and never
   kills. The IPC pipe names are additionally created with
   `FILE_FLAG_FIRST_PIPE_INSTANCE`, so a pipe-name collision cannot yield
   two command surfaces.
2. **Backend attach vs conflict.** The engine resolves the PID owning the
   SDK listener on port 6744 before connecting:
   - listener present + SDK handshake completes → **attach** (expected:
     the externally launched backend is already running — that is the
     intended arrangement, the engine borrows it);
   - nothing listening → report "no backend detected" and keep retrying
     on the client's bounded backoff (backend may start later);
   - port held by a process that fails the SDK handshake → report an
     ownership **conflict** (a foreign service is squatting on the
     managed endpoint).
3. **Competing owners — report only.** Startup scans the process list
   for other `OpenRGB.exe` instances (each may own hardware or host the
   legacy Studio/Effects/HardwareSync plugin producers), L-Connect,
   Gigabyte Control Center / RGB Fusion, Razer Synapse, Logitech G
   Hub/Options, iCUE, and SignalRGB. Findings are logged and kept in the
   ownership report — an explicit takeover is the operator's decision;
   the engine never kills arbitrary processes. `FanControl.exe` is a
   **notice, not a conflict**: it owns cooling and must keep running.
4. **Sole scene writer.** Two plugin-load paths are checked (the
   backend loads both):
   - Given `--backend-config <dir>` (the backend's private config dir),
     startup checks `<dir>/OpenRGB.json` for enabled plugins and
     `<dir>/plugins/` for loadable plugin binaries.
   - Independently of `--backend-config`, when the SDK listener's PID
     resolves, its image path gives the backend's exe dir — Windows
     loads `<exe dir>/plugins/` as **system plugins** unconditionally
     and defaults unknown plugins to enabled on first run, so a legacy
     producer dropped beside `OpenRGB.exe` is flagged even before it is
     persisted into `OpenRGB.json`. An unreadable image path (e.g. an
     elevated backend) is reported and that scope is skipped.
   Any enabled/loadable plugin is reported; anything matching the
   legacy Studio/Effects/HardwareSync producers is a `WARNING` — those
   would be a second scene writer in the managed configuration, which
   section 4 forbids. With no plugins enabled/present, the engine is
   the sole scene writer.

## What the engine does NOT do

- Never opens USB/HID/SMBus — all hardware access goes through the
  backend's SDK on loopback TCP 6744.
- Never kills or signals another process (competing owners, the old
  Studio path, a second OpenRGB) — conflicts are reported honestly.
  Besides the R-9 wedge close below (its OWN verified backend,
  gracefully), the one exception is the operator's own explicit
  request: IPC QUIT
  `{"stop_backend":true}` ("Quit all") adds a backend-handoff step to
  the ordered shutdown that signals the managed backend's PID-scoped
  shutdown event (`Local\OpenRGB2-Backend-Shutdown-<pid>` — the
  headless `--server` backend owns no windows, so the event is the
  channel WM_CLOSE cannot be; WM_CLOSE stays as the fallback for a
  windowed build) — but ONLY when the pid's image path equals the declared
  `--backend-exe` exactly (the same verified-own standard
  `stop-openrgb2.ps1` enforces; a foreign or unverifiable process is
  reported and left running, and nothing is ever hard-killed).
- Spawns the backend only as its **supervisor** (R-9), in the managed
  layout (`--sdk` + `--backend-exe` + `--backend-config`), and only
  when ALL hold: the SDK link has been down 5 s, no process with the
  `--backend-exe` image exists (a starting or hung backend is never
  doubled), nothing listens on :6744, no other `OpenRGB.exe` (foreign
  or unreadable image) runs — the pinned backend's own single-instance
  prompt could `TerminateProcess` it, the same reason
  `start-openrgb2.ps1` refuses — no stop is in progress
  (`state\openrgb2.stopping`), and the budget allows it (5 per hour,
  backoff 0/10/30/60/120 s). The spawn uses the launcher's exact
  arguments and working directory (parity is a test). Otherwise the
  launcher starts the backend, as before.
- Closes the backend on its own initiative in ONE case (R-9): a Lian
  Li wireless group reported `wedged` + `state:"failed"` for 8 minutes
  (twice the backend's own re-arm cap), at most twice per hour — the
  same verified-own graceful channel as Quit-all (image re-verified,
  shutdown event / WM_CLOSE, never a kill); the dead-backend rule then
  respawns it. `foreign_master` never escalates.
- Never touches FanControl, PWM, pump/fan speed, PawnIO, or sensors.
