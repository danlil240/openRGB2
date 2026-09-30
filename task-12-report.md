# Task 12 — Final qualification + packaging evidence

**Verdict: PASS (offline qualification).** The complete imported Desktop
Lighting Studio workflow is engine-owned, durable, IPC-only, and ships as
a manifest-verified standalone Windows package. Physical-hardware
acceptance remains opt-in and out of scope (see *Non-goals*).

All evidence lives under `task12-evidence/`. The staged package payload
itself is gitignored (binary artifacts; restage via
`packaging/windows/stage-release.ps1 -BuildDir build-studio`).

## 1. Fake-backend full-workflow E2E

`tests/engine/e2e_studio_workspace_test.cpp` (new, 1,469 lines, CTest
`engine` label) drives one engine over real named pipes with the injected
fake SDK backend:

- **A. Legacy import** — a host-settings blob (`{"scene":…,"inputs":…}`)
  uploads, inspects, and commits as workspace `desk-legacy` (rev 1,
  never auto-applied to output).
- **B. v3 workspace + editing** — compact `schema_version:3` document
  with a custom device type (`qa-strip`), armed inputs, palette edits,
  revision-gated commits; stale-revision writer refused.
- **C. Packed bundle import** — `openrgb2-bundle-files` upload resolves
  bundled device types/looks onto a fresh id.
- **D. UI process boundary** — repeated client attach/detach: an
  in-flight preview dies with its session, a new session rejoins and
  takes a preview (dead session's slot released), engine continuity
  asserted across all boundaries.
- **E. Engine restart** — a second `EngineHost` on the same workspace
  root recovers the active generation; authoring data intact.
- **F. Export → reimport** — portable bundle stages the workspace plus
  referenced types, re-imports under a new id.

## 2. Recovery-correctness hardening (root cause found by the E2E)

A recovered generation could wedge: the engine published the
last-known-good revision while the durable marker still pointed at the
corrupt generation, so the next commit compared the client's published
expectation against the marker and failed `revision_conflict` forever.

Fix in `src/engine/WorkspaceService.{cpp,h}`: `StoreExpectedRevs()`
translates a caller expectation matching the *published recovered* state
to the marker revision; genuine stale writers still fail. Applied to
`CommitJson`, save-copy, `RestoreBackup`, reload gating, and
`ImportStagedJson` marker handling. A successful post-recovery commit
heals the marker and mints a new generation.
`src/ui/WorkspaceController.cpp` no longer discards a lower-revision
recovered document on hash mismatch; `recoveryOffered` is strictly the
unsaved-draft path. Regression coverage in
`tests/engine/ipc_workspace_test.cpp` (+87 lines).

## 3. Full suite

```
ctest --test-dir build-studio -C Release --output-on-failure
100% tests passed, 0 tests failed out of 78   (129.84 s)
```

Labels: backend 6, core 8, engine 27, packaging 5, sdk 3, ui 29.

## 4. QA-scripted UI workflows

`task12-evidence/qa/workflow.py` drives the real windowed UI through the
QA socket against the isolated fake-backend engine (`--pipe
\\.\pipe\openrgb2-qa-engine`, port 5584) — and was then re-run against
the **packaged** binaries (port 5587). Passing steps include live Quick3D
viewport (`viewport-active`), screen-space 3D pick (`desk`/`kb`), 2D
canvas selection, device-library interaction, layer add, palette stop
add, Ctrl+S save (`dirty:true→false`, revision advanced), workspace
actions dialog + list, tamper→external-change→recovery, and the
calibration surface refusing honestly without hardware.

Harness fixes along the way: `src/ui/QaServer.cpp` routes key events
through the application notification path so Qt shortcuts (Ctrl+1..5
view switching, Ctrl+S) actually fire; `ui/pages/LightingPage.qml` +
`LayoutPage.qml` expose deterministic scroller/content-Y seams.

Evidence: `qa/transcript.json` (packaged run, 401 entries),
`qa/transcript-dev.json`, `qa/shots/`, `qa/shots-dev/`.

## 5. Accessibility / layout / rendering tiers

- **Narrow layout** 960×820: `15-narrow-layout.png`, nav + pages reflow
  (2222 items rendered, no clipping failures).
- **Scaled layout** `QT_SCALE_FACTOR=1.5`: `16-scaled-150.png`
  (2894×1421 px surface = 1929×947 logical ×1.5, verified non-blank,
  still connected, keyboard nav live).
- **Keyboard access**: Ctrl+1..Ctrl+5 switch all five views; Ctrl+S
  saves from the Lighting page.
- **Reduced motion**: `reduced-motion.png` — viewport honors the
  `reducedMotion()` gate.
- **Quality tiers**: `qual-low.png` / `qual-balanced.png` /
  `qual-high.png` — `renderQuality`/`renderBloom` switch the Quick3D
  pipeline.

## 6. Local-control responsiveness

`tests/engine/e2e_responsiveness_test.cpp` measures IPC request→reply
wall time on real named pipes while the engine runs a 60 Hz scene clock
with output active and a subscribed client. `task12-evidence/responsiveness.json`:

- 300 samples over 8 verbs; engine produced 614 frames at 59.82 Hz
- **overall p95 31.13 ms < 100 ms → PASS** (max 31.60 ms)

## 7. Package staging + clean-PATH runtime

`packaging/windows/stage-release.ps1 -BuildDir build-studio` produced
`task12-evidence/package/OpenRGB2-0.1.0-win64` — `manifest.json` written
with **1,448 files** after verifier checks. Fixes that landed:

- `Qt6Quick3D.dll` allowlisted — the `^Qt6\w*d\.dll$` debug-DLL pattern
  falsely matched the release Quick3D runtime.
- Strict-mode guard on an empty `$locking` array.
- `-BuildDir` honored (this worktree builds `build-studio`, not `build`).

Clean-PATH runtime verification (`PATH = <pkg>;System32;Windows` only,
launched from Python to keep `\\.\pipe\` arguments intact — see
*Caveats*): packaged engine + packaged UI on a custom pipe
`\\.\pipe\openrgb2-pkg-engine` with QA on 5587:

- UI `connState: connected`; full workflow re-run (3D pick, edits, save).
- Engine loaded **13 device-type presets from the packaged
  `presets\devices`** and restored per-scene playback selections from the
  packaged `profiles\local\engine-state.json`.
- Workspace seed created writable state under the package
  (`profiles\local\workspaces\qa-desk\generations\gen-000001..2` with
  active marker + last-known-good + bundled device resources).
- **Restart recovery**: both processes killed, relaunched — `qa-desk`
  persisted in the store and reopened at workspace revision 2
  (generation 2, the healed post-recovery save).
- Ownership scan ran (G Hub/FanControl reported, never killed);
  managed-config sole-scene-writer check verified.

Logs: `task12-evidence/package-run/` (`state-eng*.log`,
`state-ui*.log`, `workspace-files.txt`), manifest copy at
`task12-evidence/package-manifest.json`.

## 8. Parity / provenance reconciliation

`vendor/provenance/dls-ui-import-hashes.txt` — all **162** rows
re-verified against the read-only source tree
(`openrgb-unified\plugins\DesktopLightingStudio`): every recorded source
SHA-256 still matches, every `byte-identical` destination still
byte-identical. Corrections made:

- `../tests/editor_qml/*` → `../../tests/…` (7 rows; the plugin test
  sources live at the repo root, one level higher).
- `EffectRegistry.h`, `PresetRegistry.{cpp,h}` annotated as extended in
  Task 9 (file-layer removal / layer queries for preset delete) —
  destination hashes differ *by design*, documented rather than masked.

`docs/studio-import.md` parity table: Task-12 row written; the
byte-identical claim updated (70/73 identical + 3 documented
extensions).

## Caveats / non-goals

- **Pipe-name arguments must never traverse MSYS/PowerShell quote
  layers** — `\\` collapses to `\` and the engine then tries
  `CreateNamedPipe \.\pipe\…` (invalid). Package verification launches
  used Python `subprocess` with pipe names built from `chr(92)`/`os.sep`.
  The shipped `scripts\start-openrgb2.ps1` uses only the default pipe
  name internally, so end users never hit this.
- The packaged-run verification launched the engine **without `--sdk`**
  (null sink): hardware delivery was qualified by the fake-backend E2E
  and the engine/SDK suites, not by touching real devices.
  Physical-hardware acceptance (real controller output, calibration on
  hardware) is opt-in and tracked separately.
- `ext-change-surfaced` soft-check timed out on the packaged run's 15 s
  bound (the external-scan cadence is longer); the recovery path itself
  was still exercised via reload + post-recovery commit (rev 1→2).
