# OpenRGB2 Windows packaging (Task 6.1)

Reproducible staging of the local release tree. No hand-assembled
folders: `stage-release.ps1` copies build outputs, runs `windeployqt`
from the pinned Qt, stages the pinned vendored backend, drops the
license notices, verifies the result — failing if required files are
missing or excluded components appear — and only then writes the
SHA-256 `manifest.json` (so a failed package can't carry an
`exclusions_verified` claim).

## Usage

```powershell
powershell -ExecutionPolicy Bypass -File packaging\windows\stage-release.ps1
```

Output: `build\package\OpenRGB2-<version>-win64\` (version is read
from the top-level `CMakeLists.txt`).

Inputs it expects (all built in-tree first):

| Input | Default location | Override |
| --- | --- | --- |
| App Release build | `build/src/*/Release`, `build/tools/engine_ctl/Release` | `-BuildDir` |
| Qt 6.8.3 | `C:\Qt\6.8.3\msvc2022_64` | `-QtDir` / `OPENRGB2_QT_DIR` |
| Pinned backend deploy dir | `vendor\OpenRGB\OpenRGB Windows 64-bit` (else `vendor\OpenRGB\release`) | `-BackendDeployDir` / `OPENRGB2_BACKEND_DIR` |
| MSVC CRT redist | newest `VC\Redist\MSVC\<ver>\x64\Microsoft.VC143.CRT` | `-VcRedistDir` / `OPENRGB2_VC_REDIST` |

Re-verify an already staged tree without re-staging:

```powershell
powershell -File packaging\windows\stage-release.ps1 -VerifyOnly -OutDir <dir>
```

## Install / recovery notes (local release)

- The staged folder is the product; copy it whole to the install
  location. App DLLs are resolved app-locally — no installer or PATH
  changes needed.
- Managed startup/shutdown/update/rollback is Task 6.2: the shipped
  `scripts/*.ps1` + `OpenRGB2.cmd` launch the pinned backend
  (`--server --server-host 127.0.0.1 --server-port 6744 --noautoconnect
  --localconfig`, private config `state\backend`), then the engine
  (attach-only — it never spawns the backend), then the UI. Full
  guide: `install-recovery.md` here (staged as `INSTALL.md`).
- User state lives only under `profiles/local/` + `state/` (created
  at runtime, never shipped) — update and rollback both preserve it.
- Recovery: delete the folder and re-stage — the package is fully
  reproducible; `manifest.json` verifies every file.

## Files here

- `stage-release.ps1` — staging + verification script.
- `start-openrgb2.ps1` / `stop-openrgb2.ps1` — managed startup
  (ownership scan -> explicit `-TakeOver` gate -> backend -> engine ->
  UI) and ordered clean shutdown (engine IPC QUIT -> graceful backend
  handoff; verified-own processes only, never kills competitors).
- `update-openrgb2.ps1` / `rollback-openrgb2.ps1` — manifest-verified
  update over an install (preserving `profiles/local/` + `state/`)
  and one-level rollback from `<install>.previous`.
- `OpenRGB2.cmd` — double-click launcher shipped at the package root.
  The copy in this source directory launches the staged release under
  `build/package/` when one is present.
- `OpenRGB2.vbs` — terminal-free launcher for a desktop shortcut. It
  starts the managed stack with `-TakeOver`, keeps competing applications
  running, and logs startup to `state/startup.log`. The GUI wrapper shows
  an error dialog when startup fails.
- `create-desktop-shortcut.ps1` — creates `OpenRGB2.lnk` on the current
  user's desktop, using the packaged OpenRGB icon.
- `install-recovery.md` — install/startup/ownership/update/rollback
  guide (staged as `INSTALL.md`).
- `NOTICES.txt` — shipped license/provenance manifest.
- `licenses/` — license texts shipped in the package.
- `dependency-inventory.md` — every shipped binary + deps + provenance.
- `RELEASE.md` — release presentation (what's installed, how to run,
  measured support matrix + performance, honest limitations); staged
  at the package root (Task 6.4).
