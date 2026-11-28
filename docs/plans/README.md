# Plans

Implementation plans for the features and other non-bug findings of the
[2026-11-28 audit](../audit-2026-11-28-open-findings.md). Every plan is
`Status: proposed`; each lists its steps, tests, and the decisions that
are Daniel's to make. Bugs from the same audit were fixed on
`fix/*` branches instead (see the audit doc's status section).

Size: S = a day or less, M = a few days, L = a week or more.

## Dependencies worth knowing first

- **R-1 is the base for R-2, R-4, R-8 and E-12.** It adds an
  engine-side output control (master brightness, lights off, pause).
  Today brightness is a scene-document field, and in workspace mode a
  change only reaches the LEDs after Save.
- **R-11 needs a decision before work starts.** Multi-layer looks rarely
  share a short common period (the shipped Wave look: 10.42 s and 6.0 s),
  so seamless loops need layer speeds snapped by up to 5 %, and
  `IMPLEMENTATION_PLAN.md` currently puts compiled uploads out of scope.
- **C-7 type/category targets** build on the instance-id matching on
  `fix/c-7-instance-targets`; R-12 imported looks should use them.
- **Q-14 delete/duplicate keys** depend on the shortcut scoping on
  `fix/q-2-shortcut-scope`.
- **R-10** builds on B-1 (merged) and E-5 (`fix/e-5-static-reassert`).
- **E-12c** (histograms) is best landed before or with E-2
  (`fix/e-2-timer-resolution`) so the timer change is measured.
- **P-7** needs a measurement on the G512 first: whether its SetKeyColors
  ACK comes after the frame is parsed.

## Features

| ID | Plan | Size |
| --- | --- | --- |
| R-1 | [Tray quick controls](R-1-tray-quick-controls.md) | M |
| R-2 | [Global hotkeys](R-2-global-hotkeys.md) | M |
| R-3 | [Per-app/game profiles](R-3-per-app-profiles.md) | M |
| R-4 | [Schedule / lock / idle dimming](R-4-schedule-lock-idle-dimming.md) | M |
| R-5 | [Temperature-reactive scalar input](R-5-temperature-scalar-input.md) | M |
| R-6 | [Audio spectrum bands](R-6-audio-spectrum-bands.md) | M |
| R-7 | [DXGI screen capture](R-7-dxgi-screen-capture.md) | M |
| R-8 | [Automation: engine_ctl verbs, then a loopback bridge](R-8-automation-ctl-and-bridge.md) | M |
| R-9 | [Supervisor and crash dumps](R-9-supervisor-and-crash-dumps.md) | M |
| R-10 | [Competing-writer guard](R-10-competing-writer-guard.md) | S |
| R-11 | [Compile looks into uploaded loops](R-11-compile-looks-to-uploaded-loops.md) | L |
| R-12 | [Single-look export/import and look controls](R-12-look-export-import-and-look-controls.md) | M |
| E-12a | [Per-controller send-budget overrides](E-12a-send-budget-overrides.md) | S |
| E-12b | [Exit/idle output policy](E-12b-exit-idle-output-policy.md) | M |
| E-12c | [Cadence and latency histograms](E-12c-cadence-latency-histograms.md) | S |
| C-13 | [Cheap effect extensions](C-13-effect-extensions.md) | M |
| C-7 | [Type and category layer targets](C-7-type-category-layer-targets.md) | S |
| Q-14 | [UI features](Q-14-ui-features.md) | M |

## Backend, tests and review

| ID | Plan | Size |
| --- | --- | --- |
| P-7 | [Logitech 0x8080 closed-loop pacing](P-7-logitech-8080-closed-loop-pacing.md) | M |
| P-10 | [Lian Li reopen on persistent timeouts](P-10-lianli-reopen-on-persistent-timeouts.md) | S |
| P-12 | [Lian Li discovery poll cost](P-12-lianli-discovery-poll-cost.md) | M |
| P-13 | [Test and build gaps](P-13-test-and-build-gaps.md) | M |
| U-14 | [Review of the unreviewed UI parts](U-14-review-unreviewed-ui-parts.md) | M |
| ROADMAP | [Plan versus reality and the open backlog](ROADMAP-gaps.md) | L |

## Hygiene

| ID | Plan | Size |
| --- | --- | --- |
| HYG-1 | [Continuous integration](HYG-1-ci.md) | M |
| HYG-2 | [Static analysis and sanitizers](HYG-2-static-analysis.md) | M |
| HYG-3 | [Benchmark results](HYG-3-benchmark-results.md) | S |
| HYG-4 | [Splitting the large files](HYG-4-split-large-files.md) | L |
| HYG-5 | [Backlog location and evidence policy](HYG-5-backlog-location.md) | S |
