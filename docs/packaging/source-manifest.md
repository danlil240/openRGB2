# OpenRGB2 source / build manifest (Task 6.4)

How the staged package `build/package/OpenRGB2-<version>-win64/` is
produced from source. Everything below is reproducible: the same pins
in the same order produce the same tree, and `manifest.json` inside the
package carries a per-file SHA-256 record of a specific staging run.

## Pinned toolchain (from `AGENTS.md`)

| Tool | Pin |
| --- | --- |
| OS / arch | Windows 10/11, x64 only |
| Qt | **6.8.3** at `C:\Qt\6.8.3\msvc2022_64` (provides `bin\windeployqt.exe`) |
| Build tool | `jom` at `C:\Qt\jom` (backend qmake recipe) |
| Compiler | MSVC 2022 BuildTools — `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools` |
| CMake / CTest | the VS-bundled binaries at `…\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\` (not on PATH in Git Bash) |
| CRT payload | newest `VC\Redist\MSVC\<ver>\x64\Microsoft.VC143.CRT` under BuildTools — copied app-local, no redist installer |

## Build from source

```bash
CM="/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"

# 1. App (engine, UI, ctl) — configure + build + test
"$CM" -S . -B build -G "Visual Studio 17 2022" -A x64 \
      -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
"$CM" --build build --config Release
"$CM" -E chdir build ctest -C Release --output-on-failure

# 2. Pinned vendored backend (its own qmake recipe, in-tree; does not
#    touch any installed OpenRGB)
cd vendor/OpenRGB && cmd //c "scripts\\build-windows.bat 6.8.3 2022 64"
#    output: vendor/OpenRGB/"OpenRGB Windows 64-bit"/

# 3. Stage the package (copies binaries, runs windeployqt, verifies
#    required files present + forbidden components absent, then writes
#    the SHA-256 manifest.json)
powershell -ExecutionPolicy Bypass -File packaging\windows\stage-release.ps1
```

Re-verify an existing tree without re-staging:
`stage-release.ps1 -VerifyOnly -OutDir <package dir>`.

## Component manifest

| Component | Source | License | Package destination |
| --- | --- | --- | --- |
| `openrgb2-engine.exe` | `src/engine/` + `src/core/` + `src/ipc/` + `src/backends/openrgb/` + `src/platform/windows/` (this repo) | GPL-2.0-or-later | package root |
| `openrgb2-ui.exe` + QML | `src/ui/` + `ui/` QML module (this repo) | GPL-2.0-or-later | package root + `qml/` |
| `openrgb2-ctl.exe` | `tools/engine_ctl/` (this repo) | GPL-2.0-or-later | package root |
| `backend\OpenRGB.exe` + payload | `vendor/OpenRGB/` — the pinned `lianli-wireless` fork | GPL-2.0-or-later (OpenRGB upstream) | `backend/` |
| Qt runtime + QML modules | Qt 6.8.3 via `windeployqt` | LGPL-3.0 / Qt GPL exceptions | root + `backend/` + `qml/`, `platforms/`, … |
| MSVC CRT | `Microsoft.VC143.CRT` redist folder | MSVC redist terms | root + `backend/` |
| Backend non-Qt payloads | `hidapi-hotplug.dll`, `libusb-1.0.dll`, `PawnIOLib.dll`, `*.bin` PawnIO modules, `dxcompiler.dll`/`dxil.dll` | per-file — see `NOTICES.txt` / `licenses/` | `backend/` |
| Preset library | `src/core/presets/{devices,effects}` | GPL-2.0-or-later | `presets/` |
| Topology profiles | `profiles/*.json` (committed; no unique IDs) | GPL-2.0-or-later | `profiles/` |
| Lifecycle scripts + docs | `packaging/windows/` | GPL-2.0-or-later | `scripts/`, `OpenRGB2.cmd`, `OpenRGB2.vbs`, `INSTALL.md`, `RELEASE.md`, `NOTICES.txt`, `licenses/` |
| Desktop shortcut icon | `vendor/OpenRGB/qt/OpenRGB.ico` | GPL-2.0-or-later | `OpenRGB2.ico` |

**Compression stack:** tinyuz + HDiffPatch are compiled from the MIT-licensed
sources vendored inside `vendor/OpenRGB/dependencies/` — the installed
L-Connect `yuz.dll` is never bundled (asserted absent by the staging
verification).

## Vendored provenance

The backend is not stock OpenRGB. `vendor/OpenRGB/` is the imported
working-tree state of the `lianli-wireless` fork at
`53a477eab8d14b0bc49aeb40e3d569cbcb16a377`, including its uncommitted
modifications at import time. The complete provenance record — origin
repos/revisions, file-level SHA-256 hashes, deliberate adaptations,
removed historical workarounds, and per-component licenses — is in
**[`vendor/provenance/MANIFEST.md`](../../vendor/provenance/MANIFEST.md)**
(+ `import-hashes.txt` beside it).

## Per-file manifest

`manifest.json` (inside the staged package) lists every shipped file
with `path` / `sha256` / `bytes` / `origin` (`app-build`, `qt-runtime`,
`msvc-crt`, `vendored-backend`, `src/core/presets`, `profiles`,
`packaging/windows`, …), plus a `source` block recording the repo,
git revision, configuration, and toolchain inputs. Paths in that block
are privacy-scrubbed at staging time (`<repo>` / `<profile>`
placeholders). Verify the tree the same way `update-openrgb2.ps1`
does — every listed file must exist and hash-match.

## Privacy note

Committed files carry **no unique hardware identifiers** — device
serials/MACs live only in gitignored `profiles/local/` user state.
Public docs use placeholders (`<user>`, `<host>`, `serial on file`);
the diagnostics export path additionally runs the
`ScrubPrivatePaths` redactor (`src/ui/DiagnosticsController.cpp`) on
produced exports.
