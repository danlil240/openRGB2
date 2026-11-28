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
from the top-level `CMakeLists.txt`). A relative `-OutDir` is resolved
against the current PowerShell location. The manifest records the git
revision and `revision_dirty` (tracked files differed from HEAD).
Staging refuses to wipe an existing package dir while one of its
executables is in use (also catches an elevated stack whose path a
non-elevated shell cannot read); processes running from elsewhere do
not block it.

The UI needs Qt: `OPENRGB2_BUILD_UI` (CMake option, default `ON`) makes
a missing Qt component a configure error. `-DOPENRGB2_BUILD_UI=OFF`
builds only the Qt-free engine, tools and non-UI tests.

Inputs it expects (all built in-tree first):

| Input | Default location | Override |
| --- | --- | --- |
| App Release build | `build/src/*/Release`, `build/tools/engine_ctl/Release` | `-BuildDir` |
| Qt 6.8.3 | `C:\Qt\6.8.3\msvc2022_64` | `-QtDir` / `OPENRGB2_QT_DIR` |
| Pinned backend deploy dir | `vendor\OpenRGB\OpenRGB Windows 64-bit` (else `vendor\OpenRGB\release`) | `-BackendDeployDir` / `OPENRGB2_BACKEND_DIR` |
| MSVC CRT redist | newest `VC\Redist\MSVC\<ver>\x64\Microsoft.VC143.CRT` | `-VcRedistDir` / `OPENRGB2_VC_REDIST` |
| Trusted Signing metadata.json | — (only with `-Sign`) | `-SignMetadata` / `OPENRGB2_TS_METADATA` |
| Signing account fields | — (synthesized when no metadata file) | `-TsEndpoint` `-TsAccount` `-TsProfile` / `OPENRGB2_TS_ENDPOINT` `_ACCOUNT` `_PROFILE` |
| `Azure.CodeSigning.Dlib.dll` | newest in `~/.nuget/packages/microsoft.trusted.signing.client` | `-SignDlib` / `OPENRGB2_TS_DLIB` |
| `signtool.exe` | newest Windows SDK under `Windows Kits\10\bin` | `-SignTool` / `OPENRGB2_SIGNTOOL` |

Re-verify an already staged tree without re-staging:

```powershell
powershell -File packaging\windows\stage-release.ps1 -VerifyOnly -OutDir <dir>
```

## Signing (Azure Trusted Signing)

Unsigned local builds are exactly what Smart App Control blocks on
sight — it has no exclusion list, and a valid Authenticode signature
is the only per-file pass it honors. `-Sign` runs a signing pass over
every staged `*.exe` (the three app binaries plus `backend\OpenRGB.exe`)
after staging and before manifest hashing, using `signtool /dlib` with
the Trusted Signing endpoint `timestamp.acs.microsoft.com`:

```powershell
powershell -File packaging\windows\stage-release.ps1 -Sign `
    -TsEndpoint https://wus2.codesigning.azure.net/ `
    -TsAccount <account> -TsProfile <cert-profile>
```

Prerequisites on the signing machine:

- Windows SDK signtool **10.0.22621+**.
- The `Microsoft.Trusted.Signing.Client` NuGet package (provides
  `bin\x64\Azure.CodeSigning.Dlib.dll`).
- An Azure credential the dlib's DefaultAzureCredential chain accepts —
  `az login` for interactive runs, or the `AZURE_TENANT_ID` /
  `AZURE_CLIENT_ID` / `AZURE_CLIENT_SECRET` service-principal env vars
  in CI.

With `-VerifyOnly -Sign`, no signing happens — the verify pass instead
asserts every staged `.exe` reports `Valid` from
`Get-AuthenticodeSignature`. The shipped `manifest.json` records a
per-file `signed` flag and a `signing` block when a package was signed.

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
- `build-and-deploy.ps1` — one-command wrapper: CMake build (fails
  early on a `Qt6_DIR-NOTFOUND` cache) → pinned backend check
  (`-BuildBackend` runs the qmake recipe from `vendor\OpenRGB`) →
  backend staleness check (the `openrgb2-backend.stamp` next to
  `OpenRGB.exe` must match a hash of `vendor/OpenRGB` +
  `src/core/playback`; a stale or unstamped backend is rebuilt with
  `-BuildBackend`, staged with a warning with `-AllowStaleBackend`,
  and otherwise fails with "backend is stale") →
  with `-InstallDir`, stop the running install → `stage-release.ps1`
  → `update-openrgb2.ps1` into `-InstallDir`.
- `backend-stamp.ps1` — the stamp writer and staleness check
  (dot-sourced by `build-and-deploy.ps1` and
  `build-all-and-install.ps1`; functions only, not staged).
- `..\..\scripts\build-all-and-install.ps1` — repo-to-desktop-icon
  wrapper: builds and stages as you, and asks UAC only to stop and
  deploy the elevated install (plus a sweep when an elevated dev-run
  process holds a repo binary).
- `start-openrgb2.ps1` / `stop-openrgb2.ps1` — managed startup
  (ownership scan -> explicit `-TakeOver` gate -> backend -> engine ->
  UI) and ordered clean shutdown (engine IPC QUIT -> graceful backend
  handoff; verified-own processes only, never kills competitors).
- `update-openrgb2.ps1` / `rollback-openrgb2.ps1` — manifest-verified
  update over an install (preserving `profiles/local/` + `state/`)
  and one-level rollback from `<install>.previous`. An interrupted
  update keeps its snapshot (`.update-in-progress.json` marker): re-run
  to resume, or roll back. Run elevated, the update makes the install
  admin-only.
- `OpenRGB2.cmd` — double-click launcher shipped at the package root.
  The copy in this source directory launches the staged release under
  `build/package/` when one is present.
- `openrgb2-ui.exe` — terminal-free desktop entry. It starts the managed
  stack as administrator, keeps a tray icon after the window closes,
  and logs startup to `state/startup.log`.
- `startup-task.ps1` — manages the optional current-user, highest-privilege
  sign-in task controlled by Settings → Windows startup. Refuses to
  enable over a user-writable install, or one inside a folder the user
  could rename it out of (e.g. `%LOCALAPPDATA%\Programs` — use
  `%ProgramFiles%\OpenRGB2`); reports a `stale` task left by a moved
  install, which disable removes.
- `OpenRGB2.vbs` — retained compatibility launcher for older shortcuts.
- `create-desktop-shortcut.ps1` — creates `OpenRGB2.lnk` pointing directly
  at the UI executable, using the packaged OpenRGB2 icon.
- `install-recovery.md` — install/startup/ownership/update/rollback
  guide (staged as `INSTALL.md`).
- `NOTICES.txt` — shipped license/provenance manifest.
- `licenses/` — license texts shipped in the package.
- `dependency-inventory.md` — every shipped binary + deps + provenance.
- `RELEASE.md` — release presentation (what's installed, how to run,
  measured support matrix + performance, honest limitations); staged
  at the package root (Task 6.4).
