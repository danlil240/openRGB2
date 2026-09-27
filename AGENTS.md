# AGENTS.md — OpenRGB2

Windows RGB application with uploaded animation playback and live-effects
modes for Lian Li SL Wireless fans, driven through the wireless-enabled
OpenRGB backend. See `IMPLEMENTATION_PLAN.md` for the full spec and
milestones.

## Layout

| Path | Contents |
| --- | --- |
| `vendor/OpenRGB/` | Wireless-enabled OpenRGB backend (imported working-tree state of the `lianli-wireless` fork; provenance in `vendor/provenance/`) |
| `src/core/` | Qt-free scene/effects/resolver core (imported Desktop Lighting Studio core) |
| `src/engine/` | Qt-free lighting engine: scene worker, delivery pipeline, device mapping, `openrgb2-engine` process |
| `src/backends/openrgb/` | Qt-free OpenRGB SDK v4 client (loopback TCP to the pinned backend) |
| `src/ipc/` | Versioned/bounded local IPC: restricted named pipes (command + read-only diagnostics), JSON framing — contract in `docs/ipc-protocol.md` |
| `tests/core/` | Qt-free offline regression suites (CTest label `core`) |
| `tests/backend/` | Offline Lian Li wireless protocol harness (CTest label `backend`) |
| `tests/engine/` | Qt-free engine + IPC suites (CTest label `engine`) |
| `tests/sdk/` | SDK-client suites over an in-process fake backend (CTest label `sdk`) |
| `tools/bench/` | Milestone 1 benchmark producer (`lianli_bench`); physical runs are opt-in, wire layer is offline-tested |
| `docs/` | Hardware profile (`hardware/daniel-pc.md`) and benchmark reports (`benchmarks/baseline.md`) |
| `profiles/` | Committed topology profiles (no unique IDs); `profiles/local/` is gitignored user state |

## Toolchain (pinned)

- Qt 6.8.3 at `C:\Qt\6.8.3\msvc2022_64`, jom at `C:\Qt\jom`
- MSVC 2022 BuildTools (`C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`)
- CMake via the VS-bundled binary (`.../BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/`); `cmake`/`ctest` are not on PATH in Git Bash

## Commands

```bash
# Configure + build + test (offline, no hardware, no elevation)
CM="/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
"$CM" -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
"$CM" --build build --config Release
"$CM" -E chdir build ctest -C Release --output-on-failure   # or use ctest.exe path

# Backend only (its own qmake recipe, in-tree; does not touch installed OpenRGB)
cd vendor/OpenRGB && cmd //c "scripts\\build-windows.bat 6.8.3 2022 64"
```

## Rules

- Never modify `C:\Users\daniel\tools\openrgb-unified` — it is the preserved
  source of record for deferred components (output adapters, plugin/UI code,
  platform reactive inputs). Copy from it; never reset or clean it.
- Physical hardware benchmarks are opt-in and separate from CTest.
- Tests must run as ordinary executables (CTest); no Smart App Control
  per-path workarounds (see `vendor/provenance/MANIFEST.md`).
- Core tests resolve preset/fixture paths via the `DLS_EFFECT_DIR` /
  `DLS_PRESET_DIR` env vars set in `tests/core/CMakeLists.txt`.
