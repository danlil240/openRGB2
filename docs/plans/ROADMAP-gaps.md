# ROADMAP-gaps: Plan versus reality, and the open backlog from the docs

Status: proposed · Size: L (sum of S/M items) · Source: audit 2026-11-28 "Plan versus reality" and "Open backlog from the docs"

## Goal

Each gap either gets closed with evidence committed in `docs/benchmarks/` or
`docs/hardware/`, or gets explicitly waived by Daniel in
`IMPLEMENTATION_PLAN.md`. The plan should stop claiming ticked milestones that
the docs say were not measured.

## Current state

Evidence for each row was read in the repo. Raw run data lived in gitignored
`profiles/local/<run>/` and is not in the tree (see the benchmark-results
hygiene plan).

| # | Gap | What the repo says | Hardware? |
| --- | --- | --- | --- |
| G1 | M1: old Studio path never benchmarked | `IMPLEMENTATION_PLAN.md:290` promises it. `docs/benchmarks/baseline.md:35`, `:162`: "NOT RUN — no headless entry point exists (GUI plugin)" | yes, plus GUI automation |
| G2 | M2A: visible FPS and clock-to-phase never measured | Plan `:308`. `docs/benchmarks/playback-result.md:3`, `:48`: "remain unmeasured" | yes (camera or photodiode) |
| G3 | M5: 8 h soak replaced by 65 + 60 min | `docs/benchmarks/mixed-and-soak.md:11-17`. The owner accepted the bounded legs as the release qualification | yes |
| G4 | M5: physical unplug and suspend/resume never run | `docs/benchmarks/lifecycle-matrix.md:40`, `:42`, `:76`: "physical BLOCKED" (simulated or injected only) | yes |
| G5 | M5: DIMMs never qualified | `docs/hardware/support-matrix.md:27`, `:50`; `daniel-pc.md:80` (PawnIO needs elevation) | yes, elevated |
| G6 | `MeasurementProvenance::PhysicallyMeasured` never assigned | Enum at `src/engine/EngineTypes.h:212-218`. Only `TransportMeasured` is assigned (`OpenRgbClient.cpp:55`, `:62`; `DeliveryPipeline.cpp:16`, `:22`, `:56`). `PlaybackController.cpp:1612`: "physical stays provenance Unknown". The UI shows "measured" only for `physically_measured` (`DiagnosticsController.cpp:411-481`) | ingest no; data yes |
| G7 | N01 physical retest of the wireless wedge | Fix and rationale at `tests/backend/lianli_protocol_test.cpp:1037-1050`; wedge visibility through `backend_counters` `wedged:true` (`docs/ipc-protocol.md:558-575`) | yes |
| G8 | N03 re-import policy for an existing workspace id | `importProfiles` / `PumpImportQueue` (`src/ui/WorkspaceController.cpp:1624-1675`) send `import_commit` with slug id and base revision 0. `ImportStagedJson` (`src/engine/WorkspaceService.cpp:2413-2455`) answers `revision_conflict`, asserted at `tests/ui/ui_workspace_import_test.cpp:617-625`. Single import suffixes the slug (`WorkspaceController.cpp:1467-1480`) | no |
| G9 | Bundle inspect warns "unknown preset" for personal looks | `src/core/config/StudioConfig.cpp:972-976` checks only the built-in `FindPreset` (`src/core/effects/Presets.h:51`). The engine already validates against `EffectiveEffectRegistry()` (`src/ipc/EngineService.cpp:500-525`) | no |
| G10 | ASan / Application Verifier pass for F01/F02 never done | F01: `e023cc5` (pipe close race, `NamedPipe.h:120-135`, stress at `tests/engine/ipc_server_test.cpp:1104`) and `6a1ed47`. F02: `a10a573`, `1a714f4` (posted model resets, `LayoutController.cpp:92-100`) | no (optional live-backend leg) |
| G11 | Backend CPU 26–78% of a core; engine memory slope unproven | `mixed-and-soak.md:147-166` (playback, about 26%, engine +0.90 MB/h), `:212-231` (live, about 78%), anomaly A3 `:239` | yes |
| G12 | First-look "stalled window" never reproduced | `docs/ui-first-look-2026-09-28.md:21-23` | yes (live UI, ordinary input) |
| G13 | Unidentified LEDs: GPU fan rings, `LED_C`, `Chipset Accent` | `support-matrix.md:22`, `:25`, `:36-44`; `daniel-pc.md:35` (also ARGB_V2 headers enumerate 0 LEDs, `support-matrix.md:26`) | yes (eyes on the PC) |

## Design

Group the items by who can close them.

**Offline, code changes, one branch each:**
- **G8 (S).** Daniel picks a policy. The recommendation is the same rule as
  single import: suffix the slug until it is free, then report "imported as
  `<id>-2`". Alternatives are replace (with an undo point) or skip. Flip the
  test at `ui_workspace_import_test.cpp:617-625` to the chosen behaviour first.
- **G9 (S).** Give `StudioConfig` the effective registry through an optional
  lookup callback. Inspect paths (`WorkspaceService.cpp:1766-1780`,
  `InspectPackedBundle`) pass the store's registry plus the bundle's own looks.
  Test: `workspace_bundle_test` inspects a bundle that carries a personal look
  and gets no warning, and an unknown name still warns.
- **G10 (M).** Use the `asan` preset from P-13. Run `ipc_server_test` (F01
  stress) and the `ui` QML suites (F02) under ASan. Then run `openrgb2-engine`
  and `openrgb2-ui` under Application Verifier (Basics: Handles, Heaps, Locks)
  for a scripted QA session (`tools/qa/qa.py`) against the fake backend. Record
  the results in `docs/benchmarks/sanitizer-pass.md`.
- **G6 (S or M), Daniel decides:**
  - (a) Add an ingest path: an `engine_ctl set-physical` verb, or a file read at
    startup, that sets `PlaybackStatus.physical` with `PhysicallyMeasured` from a
    G2 measurement, together with its timestamp and method.
  - (b) Delete `PhysicallyMeasured` and the UI "measured" branch, and label
    physical values "not measured".

  The recommendation is (b) until G2 produces data, then (a).

**Hardware sessions, scripted procedures committed under `docs/benchmarks/`:**
- **G2 (M).** Phone slow-motion video (240 fps) of one SL fan plus an on-screen
  frame counter. A small `tools/measure/` script (Python, OpenCV) extracts LED
  brightness per frame to get the visible FPS and cycle time. Clock-to-phase
  comes from two fans on different groups in the same shot. This is the only
  route to G6(a).
- **G3 / G11 (M).**
  - Recreate the soak sampler (`openrgb2-ctl diag` plus `Get-Process`, as
    `mixed-and-soak.md:28` describes) as committed `tools/soak/`.
  - Run one true 8 h live leg overnight. Report backend CPU, engine PrivateBytes
    slope with a least-squares fit, and A3.
  - Backend CPU above 50% is likely tied to P-12 (polling) and P-7 (sleeps
    holding a thread busy). This is a guess, so re-measure after those land.
- **G4 (S).** Physically unplug the RX dongle, then do a real sleep and resume
  (`powercfg /h off`, Start, then Sleep), following the `lifecycle-matrix.md`
  rows. Expect recovery to `Holding`; P-10 is related.
- **G7 (S).** Reproduce the N01 trigger (`Failed`/`ForeignMaster`, then new
  intent), using the steps in the `lianli_protocol_test.cpp:1037` comment.
  Confirm re-arm on hardware and record it in `playback-result.md`.
- **G12 (S).** Two UI instances, ordinary mouse and keyboard only, per the
  first-look note. If it reproduces, file a U-item. If it does not after three
  tries, close it as not reproducible.
- **G13 (S).** Identify each LED by toggling it alone (the calibration wizard
  or `engine_ctl`) and looking. Update `support-matrix.md` and `daniel-pc.md`.
- **G5 (S).** One elevated backend run for DIMM enumeration through PawnIO.
  Record the result or "unsupported".
- **G1.** Recommendation: **waive**. The Studio path is the retired GUI plugin
  in `openrgb-unified`, and benchmarking it needs GUI automation for a path
  users no longer take.

**Rejected alternatives:**
- Unticking milestones wholesale: it loses the information that the code is
  done and only the evidence is missing. Annotate them instead.

## Steps

1. Daniel decides G1 (waive?), G3 (re-run 8 h, or keep the accepted waiver),
   G6 (a or b) and G8 (policy). Annotate `IMPLEMENTATION_PLAN.md` M1, M2A and
   M5 with the decisions and links. Test: none (docs).
2. G9 fix, with a `workspace_bundle_test` case (`engine`).
3. G8 fix, with `ui_workspace_import_test` (`ui`) and `ipc_workspace_test`
   (`engine`).
4. G6 per the decision, with `engine_types_test` and `ui_diagnostics_test`.
5. G10 sanitizer and verifier pass, after P-13 step 5. File new findings.
6. Commit `tools/soak/` and `tools/measure/`. Test: dry-run modes against the
   fake backend (`sdk` or a script self-test).
7. Hardware session A, short (about 2 h): G4, G7, G12, G13, G5.
8. Hardware session B: G2 video. Session C: an overnight 8 h G3/G11 run, after
   P-7 and P-12 land.

## Tests

The offline items are listed in the steps. Hardware items produce committed
reports, not CTest targets: physical benchmarks stay opt-in, per `AGENTS.md`.

## Risks and open questions

- G3 was formally waived by the owner. Re-running it is optional, unless the
  memory slope (+0.90 MB/h) matters for always-on use: about 7 MB/day,
  extrapolated, not measured.
- Video FPS measurement is limited by the phone's real capture rate and rolling
  shutter (guess). A photodiode on an ADC is better but needs parts.
- Several numbers (CPU, slope) predate P-7, P-12 and E-2. Re-measuring before
  those land wastes a session.
