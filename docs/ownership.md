# Process ownership — intended arrangement

Milestone 3 deliverable (Task 3.9); implements IMPLEMENTATION_PLAN.md
section 4 ("Process ownership").

## Processes and what owns what

| Process | Owns | Started by |
| --- | --- | --- |
| `OpenRGB.exe` (vendored, `vendor/OpenRGB/release/`) | **all RGB hardware** — the sole hardware owner. USB/HID/SMBus access lives here and nowhere else | Externally, with `--server --server-host 127.0.0.1 --server-port 6744 --noautoconnect --localconfig`, its private configuration directory as working dir. The engine never spawns it (managed startup is a Milestone-6 packaging item) |
| `openrgb2-engine.exe` | the scene clock, delivery pipeline, playback-mode state machine, IPC surface | User/launcher. **Attaches** to the backend over the SDK; never touches hardware directly |
| UI (Milestone 4) | nothing | attaches/detaches to the engine's named pipes freely — closing it never stops output |
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
- Never spawns the backend (Milestone 6 adds managed startup).
- Never touches FanControl, PWM, pump/fan speed, PawnIO, or sensors.
