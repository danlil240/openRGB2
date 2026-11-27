# OpenRGB2 install / startup / recovery guide (Task 6.2)

This guide covers the staged package produced by `stage-release.ps1`
(`build/package/OpenRGB2-<version>-win64/`). The staged folder IS the
product — copy it whole to the install location; all DLLs resolve
app-locally. No installer and no PATH changes.

## Layout

| Path | Role |
| --- | --- |
| `openrgb2-ui.exe` / `openrgb2-engine.exe` / `openrgb2-ctl.exe` | application executables |
| `backend\OpenRGB.exe` + payload | the PINNED vendored backend |
| `presets\`, `profiles\` | bundled preset library + committed topology profiles |
| `profiles\local\` | **user state** (created at runtime; never shipped) — durable engine profile + resolved hardware identities |
| `state\` | **user state** (created at runtime) — backend private config (`state\backend\`), run file, update journal |
| `scripts\*.ps1` | managed startup / shutdown / update / rollback |
| `OpenRGB2.cmd` | double-click entry point (runs `scripts\start-openrgb2.ps1`) |
| `openrgb2-ui.exe` | desktop entry point; starts the managed stack without a terminal and keeps a tray icon |
| `OpenRGB2.vbs` | legacy terminal-free desktop entry point |
| `OpenRGB2.ico` | OpenRGB2 application and desktop shortcut icon |
| `manifest.json` | per-file SHA-256 + provenance for the whole package |
| `.update-in-progress.json` | present only while an update is unfinished (see Update) |

### State written OUTSIDE the install dir

Verified during the Task 6.4 install/remove run — documented, not
hidden:

| Location | Writer | Contents |
| --- | --- | --- |
| `%APPDATA%\OpenRGB\logs\OpenRGB_<ts>.log` | vendored backend | one ~370-byte early-startup log stub **per backend launch**. The vendored `LogManager` opens a file under the *global* config dir during `ResourceManager` construction — *before* `--localconfig` re-points it to `state\backend\logs` (which receives the real log). It also creates `%APPDATA%\OpenRGB\` itself when absent, and briefly reads the global `OpenRGB.json` before the local one (read-only). |
| `%LOCALAPPDATA%\OpenRGB2\OpenRGB2\cache\` | Qt runtime (UI) | `QStandardPaths` pipeline/shader cache for `openrgb2-ui.exe` — Qt writes this for any Qt Quick app; safe to delete. |
| `HKCU\Software\OpenRGB2\OpenRGB2` | UI (`UiSettings`) | `QSettings` for window geometry / last view / playback-param seeds. UI-only preferences — engine state is never in the registry. |

To remove OpenRGB2 completely: disable Windows startup in Settings if
enabled, then delete the install folder (from an elevated shell when
the install is protected -- see "Install protection") and the three
locations above.
The optional sign-in task is the only other system registration.

## Process model and startup

Three processes, strict dependency order (plan §4):

1. **Backend** — `scripts\start-openrgb2.ps1` launches
   `backend\OpenRGB.exe --server --server-host 127.0.0.1
   --server-port 6744 --noautoconnect --localconfig` with
   `state\backend` as the working directory. The backend is the sole
   RGB hardware owner and keeps its private config out of the global
   OpenRGB config dir. The engine NEVER spawns the backend — this
   launcher owns that job (Task 6.2).
2. **Engine** — `openrgb2-engine.exe --sdk --backend-config
   state\backend ...` attaches over the loopback SDK once the backend
   listens on `127.0.0.1:6744`. If the backend is late the engine
   re-probes on a bounded schedule and reports it honestly.
3. **UI** — `openrgb2-ui.exe` attaches to the engine's named pipes.
   Closing it never restarts hardware state; the engine runs without
   it (`-NoUI` starts backend + engine only).

```powershell
powershell -ExecutionPolicy Bypass -File scripts\start-openrgb2.ps1
# or double-click OpenRGB2.cmd
```

Double-click `openrgb2-ui.exe` to start the managed stack without a
terminal. Windows requests administrator approval for a manual launch.
Closing its window hides it in the system tray; click the icon
to reopen it, use **Exit UI** to leave lighting running, or choose
**Quit OpenRGB2** to stop the managed engine and backend before exiting.
The Settings page has the confirmed engine shutdown action. Startup
output goes to `state\startup.log`, and a dialog reports startup errors.
`state\startup.log` and `state\shutdown.log` are rotated to `<name>.1`
once they pass 1 MB.

Settings → Windows startup has **Open OpenRGB2 when I sign in** (off by
default). Enabling it registers a task for the current Windows user with
highest privileges; at sign-in the app starts in the tray without a UAC
prompt. It **refuses to enable** while the install folder,
`openrgb2-ui.exe` or `scripts\` are writable without administrator
rights, and while any folder above the install lets you rename it
aside — which includes the default `%LOCALAPPDATA%\Programs\OpenRGB2`
(see "Install protection" below): install under `%ProgramFiles%\OpenRGB2`
for the sign-in task. Disable the setting before deleting or moving this install. If
the install was moved anyway, the setting reports the task as
belonging to another location (state `stale`, with its path); turning
the setting on registers this install instead, and disabling removes
the user's task whatever it points at. The same actions are available
from `scripts\startup-task.ps1 -Mode status|enable|disable`.

To create a desktop shortcut, run this once from the package directory:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\create-desktop-shortcut.ps1
```

The shortcut points straight to `openrgb2-ui.exe --managed`. It uses
`-TakeOver` while leaving Logitech G Hub and FanControl running. The
shortcut does not change Windows Application Control policy, so an
unsigned binary blocked by that policy still cannot launch.

Options: `-NoUI` headless · `-Scene <file>` startup scene ·
`-ScanOnly` ownership report only · `-TakeOver` explicit takeover ·
`-Elevated` re-launch via UAC (needed when the backend's hardware
access requires admin from a command-line launch; the desktop app
requests elevation automatically).

**First run:** `profiles\local\` and `state\` do not exist yet — the
engine treats a missing profile as a fresh start (never an error),
creates both on first save, and migrates older profile schemas
forward (ProfileStore v1/v2→v3-style forward reads). With no scene
the engine idles at the 60 Hz clock until the UI applies one.

**Already running:** re-running start reuses a listening managed
backend and refuses to spawn a second engine (the engine's own
single-instance guard is the last line anyway).

**Elevated stack, non-elevated shell:** a non-elevated shell cannot
read an elevated process's image path, so it cannot tell our own
elevated backend/engine from a foreign one. Start then **refuses with
exit 4** (even with `-TakeOver`) whenever `OpenRGB.exe`,
`openrgb2-engine.exe` or the `:6744` listener has an unreadable image
path — re-run elevated (`-Elevated`). Stop, update and rollback apply
the same rule.

## Ownership and explicit takeover

Startup scans for competing RGB owners — other OpenRGB instances,
L-Connect, Gigabyte Control Center/RGB Fusion, Razer Synapse,
Logitech G Hub, iCUE, SignalRGB — plus a foreign process holding the
managed port 6744. FanControl is an expected *notice* (cooling, not
RGB) and must keep running.

- **Conflicts found → startup refuses** (exit 3) and prints every
  finding. Takeover is never silent.
- To proceed you must act visibly: **close the competing application
  yourself** (recommended), or re-run with `-TakeOver`. With
  `-TakeOver` the stack starts and competitors are LEFT RUNNING —
  close them for hardware access. Two RGB writers can coexist; the
  managed engine is simply the honest scene writer. **One exception:**
  a foreign `OpenRGB.exe` still blocks startup (see below).
- **Our scripts never kill — and never let the backend's own prompt
  kill either.** The start script contains no process-termination
  call at all; the stop script only signals processes verified by
  image path as this install's own, and `-Force` (its last resort)
  applies to verified own processes only.
- **The pinned backend has its own kill-capable guard.** The vendored
  `backend\OpenRGB.exe` runs a single-instance check at launch
  (`vendor/OpenRGB/startup/main_Windows.cpp`): while a foreign
  `OpenRGB.exe` is running it shows a Yes/No dialog whose **Yes**
  hard-terminates the foreign instance via `TerminateProcess`, and
  its "server already running" probe checks the legacy default port
  **6742**, not our managed **6744** — so the dialog can appear even
  though nothing holds 6744. The vendored cli has **no flag** to skip
  that guard, so `start-openrgb2.ps1` **refuses to spawn the backend
  at all while a foreign `OpenRGB.exe` lives** (exit 3, even under
  `-TakeOver`): the close stays manual and the prompt is never
  triggered. If you run `backend\OpenRGB.exe` by hand in that
  situation the prompt — and its kill — is the backend's own, not
  ours. An `OpenRGB.exe` at OUR pinned path that has not bound yet
  is waited on, never double-spawned.
- The same scan runs inside the engine and is surfaced read-only in
  the UI (Devices & Diagnostics) and in `openrgb2-ctl state`/`diag`
  under `ownership` — report-only there too; there is deliberately
  no kill/takeover verb.
- A foreign listener on port 6744 is a conflict like any other:
  `-TakeOver` proceeds WITHOUT spawning our backend (the port cannot
  be shared) and the engine attaches to whatever listens, reporting
  protocol/ownership honestly.

## Clean shutdown

```powershell
powershell -File scripts\stop-openrgb2.ps1
```

Order: **UI, engine, backend.**

0. The UI (`openrgb2-ui.exe`, verified by image path) gets a close
   request. Closing its window hides it to the tray, so it normally
   keeps running: `-Force` ends it (verified own process only);
   without `-Force` the stop reports it and exits 1 (use the tray's
   **Exit UI**). The UI that launched the stop (tray **Quit OpenRGB2**)
   is never signaled — it exits by itself.
1. `openrgb2-ctl.exe quit` sends the IPC `QUIT` verb — the engine runs
   its ordered, bounded shutdown (power monitor → IPC intake → IPC
   server → scene worker → reactive inputs → delivery lanes → SDK
   transport). The script waits for the process to exit within the
   bound.
2. The backend — still the sole hardware owner — is closed with a
   graceful `CloseMainWindow` so it can persist its own config. The
   engine must be fully exited first (ordered handoff).

`-TimeoutSeconds` bounds each wait. If a verified own process outlives
its bound, `-Force` permits `Stop-Process` on it — reported loudly,
never applied to foreign processes. Closing the UI alone does NOT
stop engine/backend (UI independence); use the stop script.
A stack started elevated is unverifiable from a non-elevated shell —
run the stop elevated (`-Elevated`).

**Profile safety through shutdown:** `profiles\local\engine-state.json`
is written atomically (temp + rename) on every state change with an
`.lkg` last-known-good sidecar, so a clean shutdown — or even a
mid-write crash — never leaves a torn profile. The stop script
reports the profile file and its sidecars at the end.

## Update

```powershell
powershell -File scripts\update-openrgb2.ps1 `
    -PackageDir build\package\OpenRGB2-<new>-win64 -InstallDir <install>
```

1. The new package is SHA-256-verified against its `manifest.json`
   before anything changes (`-SkipVerify` is the documented escape);
   manifest paths that are rooted or contain `..` are refused.
2. A running install is refused — stop it first
   (`scripts\stop-openrgb2.ps1 -Force`, which also closes the UI, or the
   tray's **Quit OpenRGB2**). "Running" includes a same-named process
   whose image path this shell cannot read (an elevated stack: re-run
   elevated) and any install executable that cannot be opened for
   exclusive write.
3. The entire current install is snapshotted to
   `<InstallDir>.previous.tmp`, which replaces `<InstallDir>.previous`
   only once it is complete (one-level rollback — kept on purpose).
   Then `<InstallDir>\.update-in-progress.json` is written.
4. Manifest-diff removes files the update dropped; new package files
   copy over the install; the installed files are re-verified against
   the new manifest; `manifest.json` is copied last.
5. `state\updates.json` journals from→to versions and the marker is
   removed.

**Interrupted update** (crash, power cut, a locked file): the install
may be half-updated, but the snapshot is intact. While the marker
exists the snapshot is **never retaken** — re-running the update
resumes and finishes it; `rollback-openrgb2.ps1` restores the snapshot
instead (it also removes the files the interrupted update added).

**`profiles\local\` and `state\` are never touched** — an update can
never lose the durable profile, resolved identities, backend config,
or journal. Profile schema migration stays honored: the new engine's
ProfileStore reads older state forward and keeps `.lkg`.

## Rollback

```powershell
powershell -File scripts\rollback-openrgb2.ps1 -InstallDir <install>
```

Restores previous binaries from `<InstallDir>.previous`, verified
against the backup's own `manifest.json` BEFORE restoring — a
corrupt/tampered backup is refused, not half-restored. Files the
update added are removed, and the backup's `manifest.json` is restored
last, so the install reports the rolled-back version. **User state is
never rolled back** — it restores binaries only.

## Install protection

The UI runs elevated and starts `scripts\*.ps1`, `openrgb2-engine.exe`
and `backend\OpenRGB.exe` elevated from the install folder; the
optional sign-in task runs `openrgb2-ui.exe` with highest privileges
without a UAC prompt. If that folder is writable by the signed-in
user, any program running as that user can replace a script or binary
and be started elevated at the next launch or sign-in.

- `update-openrgb2.ps1` **run elevated** makes the install admin-only:
  owner `Administrators`, inheritance broken, `Administrators` +
  `SYSTEM` full control, `Users` read/execute. `state\` and
  `profiles\local\` stay writable (Modify) by the installing user
  (`-UserSid`, default: the folder's previous owner);
  `state\backend\plugins\` is pinned admin-only because the backend
  loads plugin DLLs from its config dir. The `.previous` snapshot is
  protected the same way. `-SkipAclHardening` opts out. A non-elevated
  update leaves the ACL unchanged (and, over a protected install, is
  refused: re-run it elevated).
- `scripts\build-all-and-install.ps1` builds as you and runs only the
  stop + deploy step elevated, so the default install
  (`%LOCALAPPDATA%\Programs\OpenRGB2`) ends up protected.
- The sign-in task refuses to enable over a user-writable install.
- The sign-in task **also refuses an install inside a folder you
  control**. A protected folder can still be swapped out from above:
  whoever may rename it (or any folder above it) aside can put a
  look-alike `OpenRGB2` in its place, and the task would start that
  copy's `openrgb2-ui.exe` with highest privileges at the next
  sign-in. So `enable` walks every folder from the install up to the
  volume root and refuses if, for your account (its groups included,
  Administrators excluded), a folder grants delete / change
  permissions / take ownership or is owned by you, or its parent
  grants delete-child / change permissions / take ownership or is
  owned by you. (Add-subfolder rights alone, like the one
  Authenticated Users hold on `C:\`, are harmless: the name is
  taken.) The default `%LOCALAPPDATA%\Programs\OpenRGB2` always fails
  this — your profile is yours. For the sign-in task, install under an
  admin-only location such as `%ProgramFiles%\OpenRGB2`:

  ```powershell
  scripts\build-all-and-install.ps1 -InstallDir "$env:ProgramFiles\OpenRGB2"
  ```

  The elevated deploy step creates the folder; `state\` and
  `profiles\local\` stay writable by you. Then enable the setting from
  that install (move your desktop shortcut to it; the old install's
  user state is not copied — copy `profiles\local\` across first if
  you want it).

**Not covered** (residual risk): `state\backend` (the backend's
working directory) stays writable, so a DLL that is missing from both
`backend\` and the system directories could be picked up from there.
Without the sign-in task, an install under a folder you control (the
default) can still be swapped out from above; the next elevated
launch then shows the UAC prompt for whatever is in its place.

## Recovery notes

- **Package corrupt/incomplete:** delete the install dir and re-stage
  (`stage-release.ps1` is reproducible). User state lives only in
  `profiles\local\` + `state\` — copy them aside first if the install
  was ever run.
- **Corrupt profile:** ProfileStore falls back to `engine-state.json.lkg`
  and reports it; an unrecoverable primary is set aside as
  `.rejected`, never silently destroyed.
- **Missing dependency:** all runtime deps are app-local in the
  package; `manifest.json` lets you verify every file (`-VerifyOnly`
  on the staging script, or the update script's verify pass).
- **Backend config reset:** delete `state\backend` — the backend
  recreates it on next start (managed sole-writer: no legacy
  Studio/Effects/HardwareSync producers on either plugin-load path;
  the engine re-checks at every startup).
- **Elevation:** run `OpenRGB2.cmd`/`start-openrgb2.ps1 -Elevated`
  when the backend needs admin hardware access; children inherit the
  elevated launcher's token.
