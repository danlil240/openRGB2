# HYG-1: Continuous integration

Status: proposed · Size: M · Source: audit 2026-11-28 Hygiene ("No CI")

## Goal

Every push and pull request to `danlil240/openRGB2` builds the Release tree and
runs the offline CTest suite on a clean Windows machine. "Green on Daniel's PC"
then stops being the only check, and host-state failures such as P-6 show up as
infrastructure problems, not code problems.

## Current state

- **Nothing runs on GitHub.** There is no `.github/`. The remote exists
  (`origin` = `github.com/danlil240/openRGB2`), but nothing runs there.
- **Toolchain (`AGENTS.md:29-46`).**
  - MSVC 2022 and the Visual Studio 17 2022 generator.
  - Qt 6.8.3 `msvc2022_64`, with optional Quick3D: `OPENRGB2_WITH_QUICK3D` is ON
    and falls back quietly (`CMakeLists.txt:50-66`).
  - CMake 3.24 or newer.
  - A Qt-free configure is possible with `-DOPENRGB2_BUILD_UI=OFF`
    (`CMakeLists.txt:24-45`).
- **Test labels:** `core`, `engine`, `sdk`, `backend`, `ui`, `packaging`.
  - The UI tests need `QT_QPA_PLATFORM=offscreen` and Qt `bin` on `PATH`
    (audit "Before you start").
  - The backend tests compile vendored controller sources straight into CMake
    targets (`tests/backend/CMakeLists.txt`), so CTest does not need the
    qmake-built `OpenRGB.exe`.
- **Known host dependence.** `package_lifecycle_test` fails whenever an elevated
  OpenRGB2 install is running (P-6). On a clean runner it should pass, but that
  is not verified.
- **The backend build.** The qmake/jom recipe
  (`vendor/OpenRGB/scripts/build-windows.bat`) is long. Only packaging needs it.

## Design

Use a GitHub-hosted `windows-2022` runner with two jobs.

1. **`build-test`** (every push and PR):
   - `actions/checkout`
   - Qt via `jurplel/install-qt-action`: 6.8.3, `win64_msvc2022_64`, with the
     modules `qtquick3d` and `qtshadertools`, cached.
   - Configure with the same command as `AGENTS.md`, then build Release.
   - `ctest -C Release --output-on-failure -LE packaging -j 4`, with
     `QT_QPA_PLATFORM=offscreen` and Qt `bin` prepended to `PATH`.
   - Upload `Testing/Temporary/LastTest.log` on failure.
2. **`packaging`** (nightly and manual `workflow_dispatch`):
   - Cache the qmake backend build, keyed on the hash of `vendor/OpenRGB/**`.
   - Run the backend recipe on a cache miss.
   - Run `ctest -L packaging` and `stage-release.ps1`.
   - Upload the staged manifest. Signing stays local: no secrets in CI.

Later, once P-13 lands, add an `asan` matrix leg for the Qt-free labels and a
`w4` warnings-count job (report only).

**Rejected alternatives:**
- **A self-hosted runner on Daniel's PC.** It has the hardware, but CI code would
  run as his user on his desktop. The elevated install also breaks P-6, and the
  results would depend on host state, which is the problem CI should remove.
- **Building the backend on every push.** It costs a long qmake build for a
  binary CTest does not use.
- **Linux runners.** The engine is Qt-free but uses Win32 (named pipes, power
  monitor), so it does not build there.

## Steps

1. Add `.github/workflows/ci.yml` with the `build-test` job, running only the
   `core`, `engine`, `sdk` and `backend` labels. Test: the workflow run itself.
   Record its wall time.
2. Add the `ui` label with offscreen and the Qt `PATH`. Test: the workflow run.
   Any test that fails only on the runner becomes a new P-item, not a skip.
3. Add Quick3D to the Qt install, so `qml_studio_viewport_test` runs. Test: the
   workflow run shows it executed, not skipped.
4. Add the nightly `packaging` job with the backend cache. Test: a manual
   dispatch run.
5. Add a status badge and a one-line "CI" section to `AGENTS.md`.

## Tests

The workflow is the test. Two checks for timing-sensitive suites:
- `e2e_responsiveness_test` (p95 thresholds)
- the `ipc_server_test` stress

Run each three times in step 2. If one flakes on shared runners, give it a CI
tolerance knob. Do not drop it.

## Risks and open questions

- **Actions minutes.** The repo is private, and Windows minutes bill at a
  multiplier. Daniel should check the plan quota. A build-test run on a warm Qt
  cache is probably 15–25 min (guess).
- **Qt licensing.** The open-source Qt through aqtinstall is what the action
  downloads, which is fine for this project's GPL backend. Daniel should confirm
  that this matches his local install.
- **Should a red CI block merges** (branch protection), or only report? It is
  Daniel's call. Recommendation: report-only until step 2 has been green for a
  week.
- **The audit notes `main` was not pushed when it was written.** CI only helps
  once the branches are pushed.
