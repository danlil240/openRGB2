# HYG-2: Static analysis and sanitizers

Status: proposed · Size: M · Source: audit 2026-11-28 Hygiene ("No static analysis")

## Goal

Add a repeatable way to find the bug classes this codebase has already shipped:
handle use-after-close (F01), a delegate destroyed mid-event (F02), unlocked
shared reads (`6a1ed47`) and unchecked JSON access. The tools to add are
clang-tidy on first-party code, ASan runs of the test suites, and an Application
Verifier session. Results get a baseline, so that only new findings block.

## Current state

- **No project clang-tidy config.** The only `.clang-tidy` is under
  `vendor/OpenRGB/dependencies/hueplusplus-1.2.0`. `.clangd` suppresses one
  clang diagnostic and refers to a "build-clangd Ninja configure" that writes
  `compile_commands.json`. That configure is not scripted in the repo.
- **Tools on this PC.**
  - VS BuildTools ships `clang-tidy.exe` and `clang-format.exe` under
    `VC/Tools/Llvm/x64/bin`, but no `clang-cl`.
  - MSVC 14.44 ships the ASan runtime (`clang_rt.asan_*` under `bin/Hostx64/x64`).
- **Compiler flags.** `/W3` only, no `/analyze` and no sanitizer
  (`CMakeLists.txt:20-22`). P-13 plans the `w4` and `asan` presets. This plan
  uses them and does not redefine them.
- **Threads.** The engine has real ones: the scene worker, the delivery pipeline,
  pipe servers and the Lian Li service worker. MSVC has no ThreadSanitizer.

## Design

1. **clang-tidy, baseline-gated.**
   - Add a root `.clang-tidy` with `HeaderFilterRegex: '^(src|tools|tests)/'`,
     so vendor and Qt headers are excluded.
   - Checks: `bugprone-*`, `concurrency-*`, `performance-*`,
     `clang-analyzer-core.*`, `clang-analyzer-cplusplus.*`, and
     `cppcoreguidelines-pro-type-member-init`.
   - Disabled checks: `bugprone-easily-swappable-parameters` and
     `bugprone-narrowing-conversions`, both noisy (guess; tune on the first run).
   - Add `scripts/clang-tidy.ps1`. It configures a Ninja tree into
     `build-tidy/` with `CMAKE_EXPORT_COMPILE_COMMANDS=ON`, using `cl` from a VS
     dev shell. It then runs `run-clang-tidy` or a parallel loop over first-party
     translation units, and writes `build-tidy/tidy.txt` plus a
     per-check count.
   - Commit `docs/static-analysis/baseline.txt`: counts per check, per
     directory.
   - A PR is gated with `clang-tidy-diff` on changed lines only. Do this
     locally first; CI gets it after HYG-1.
2. **Sanitizer runs** (the procedure, using P-13's `asan` preset):
   - The Qt-free labels first.
   - Then `ipc_server_test` in a loop (F01 class).
   - Then the `ui` QML suites offscreen (F02 class).
   - Record suppressions only for findings in third-party code, with a reason.
3. **Application Verifier session** (manual, about 1 h):
   - Enable Basics (Handles, Heaps, Locks, Leak) for `openrgb2-engine.exe` and
     `openrgb2-ui.exe`.
   - Run the engine against the fake backend, or against the real one if Daniel
     chooses, while `tools/qa/qa.py` drives a QA script.
   - Attach WinDbg or the VS debugger to catch verifier stops.
   - Disable the verifier afterwards: its settings persist in the registry per
     image.
4. **MSVC `/analyze` as a cheap second opinion.** A one-off report on the engine
   library. Not gated.

**Rejected alternatives:**
- Installing the full LLVM toolchain for `clang-cl` and TSan: TSan does not
  support Windows targets, and the engine is Win32-bound.
- Gating every clang-tidy warning from day one: the backlog is unknown, and a
  gate nobody can pass gets disabled.
- cppcheck or PVS-Studio: an extra install or a licence, for overlap with
  clang-analyzer.

## Steps

1. Add `.clang-tidy` and `scripts/clang-tidy.ps1`, then run them once. Test: the
   script exits 0 and writes a report. Commit the baseline counts.
2. Triage the top three checks by count. File real defects as new audit items
   with the audit's confidence labels. Test: none (triage).
3. Add `clang-tidy-diff` gating to the local pre-PR checklist in `AGENTS.md`, and
   to CI after HYG-1. Test: a deliberate bugprone violation in a scratch branch
   fails the gate.
4. ASan pass, which needs P-13 step 5. Test: a clean run, or filed findings.
   Write it up in `docs/benchmarks/sanitizer-pass.md`; this closes ROADMAP-gaps
   G10 together with step 5.
5. Application Verifier session. Test: a verifier log with no stops, or filed
   findings.

## Tests

No new CTest targets. The deliverables are the script, the baseline, and the two
pass reports. The Application Verifier leg can use the real backend, but it does
not need it.

## Risks and open questions

- clang-tidy parsing MSVC-flavoured `compile_commands.json` can be flaky with Qt
  moc output. `.clangd` already works around one parser issue. Exclude
  `*_autogen/`.
- Which checks to gate on is Daniel's call. The list above is a starting point,
  not a measured choice.
- ASan slows tests by about 2× (guess). Timing-threshold tests such as
  `e2e_responsiveness_test` may need to be skipped under ASan.
