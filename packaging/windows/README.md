# OpenRGB2 Windows packaging (Task 6.1)

Reproducible staging of the local release tree. No hand-assembled
folders: `stage-release.ps1` copies build outputs, runs `windeployqt`
from the pinned Qt, stages the pinned vendored backend, drops the
license notices, writes a SHA-256 `manifest.json`, then verifies the
result — failing if required files are missing or excluded components
appear.

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
- Run `openrgb2-engine.exe` from inside the folder (the engine probes
  `presets/devices` + `presets/effects` relative to its working
  directory and writes durable state to `profiles/local/` there).
- The backend is launched EXTERNALLY — the engine only attaches over
  the loopback SDK. Start it as:
  `backend\OpenRGB.exe --server --server-host 127.0.0.1
   --server-port 6744 --noautoconnect --localconfig`
  (managed startup/elevation is Task 6.2's job).
- Recovery: delete the folder and re-stage — the package is fully
  reproducible. User state lives only under `profiles/local/` +
  `backend/OpenRGB.json` (written at runtime, never shipped).
- Managed startup/elevation/update+rollback is Task 6.2; this script
  only produces the verified release tree.

## Files here

- `stage-release.ps1` — staging + verification script.
- `NOTICES.txt` — shipped license/provenance manifest.
- `licenses/` — license texts shipped in the package.
- `dependency-inventory.md` — every shipped binary + deps + provenance.
