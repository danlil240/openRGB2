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

**Final-audit follow-ons (same subsystem):**

- *External-change latch ordering* — `ApplySnapshot()`'s stale-echo
  guard dropped the **whole** snapshot before reading
  `external_change`/`workspace_dir`. The dev QA transcript caught the
  symptom (`reload-settled:false`, `rev` stuck at 6 after a recovered
  reload published a lower revision). The reload-job completion already
  re-baselines `ws_rev_` to the job's reported truth, but the guard
  could still freeze the live external compare on any residual
  drop path (out-of-order push, sha-absent snapshot). The poll's
  path/external fields — live disk truth, computed at request time —
  now land *before* the guard, which only gates doc/revision adoption
  (`src/ui/WorkspaceController.cpp`). Regression: a new section in
  `tests/ui/ui_workspace_recovery_test.cpp` tampers the active
  generation, reloads through recovery, and asserts `externalChange`
  clears and the revision rewinds.
- *Commit warnings preservation* — every `CommitCommon()` caller
  overwrote `res.warnings` with `cr.warnings`, silently dropping
  parse/import/recovery notices accumulated earlier in the same call.
  `CommitCommon` now takes `prior_warnings` and appends the store's
  warnings instead of replacing (`src/engine/WorkspaceService.{cpp,h}`).

## 3. Full suite

```
ctest --test-dir build-studio -C Release --output-on-failure
100% tests passed, 0 tests failed out of 78   (132.13 s)
```

Complete captured output: `task12-evidence/ctest-full.log`.

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

Evidence: `qa/transcript-pkg-round1.json` (packaged run, 401
entries), `qa/transcript.json`, `qa/transcript-dev.json`,
`qa/transcript-extra.json` (Fix Round 2 additions: focused-text save,
conflict-bundle import, external-change reload), `qa/shots/`,
`qa/shots-dev/`, `qa/shots-manifest.json` (per-file SHA-256 +
provenance).

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
with **1,449 files** after verifier checks (restaged in Fix Round 2;
staged binary SHA-256 == `build-studio` outputs, see
`package-run/bin-hashes.json`). Fixes that landed:

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
  Fix Round 2 re-proved the full notice→reload→heal path on the package
  (`workflow_extra.py`, steps `ext-change-visible` /
  `ui-reload-disk` / `reload-settled-2` — all pass).

## Fix Round 1 — commit `db284ec`

Two review findings on the workspace commit/notify path:

1. **Accumulated warnings lost** — `WorkspaceService::CommitCommon()`
   assigned `res.warnings = cr.warnings`, discarding parse/import/
   recovery warnings the callers had already collected. `CommitCommon`
   now takes `prior_warnings` and appends the store's warnings
   (`src/engine/WorkspaceService.{cpp,h}`; all call sites updated).
2. **External-change latch ordering** — `WorkspaceController::
   ApplySnapshot()` returned from the stale-echo guard *before*
   processing `external_change`, `workspace_dir`, and identity fields;
   a lower-revision recovered snapshot could latch the stale state
   forever. Live external/path processing moved above the guard; the
   guard now drops only stale document/revision adoption
   (`src/ui/WorkspaceController.cpp`). Regression coverage added in
   `tests/ui/ui_workspace_recovery_test.cpp` (tamper → recovered
   reload → `externalChange` clears, revision rewinds).

Full suite re-run after the fix: 78/78 (report section 3).

## Fix Round 2 — this commit

Review-round follow-ups: two more real bugs found by the new QA
coverage, screenshot-integrity reconciliation, two new socket-driven
workflows, manifest documentation, and committed test/launch logs.

### Code fixes

- **Post-recovery document fetch served corrupt bytes** —
  `WorkspaceService::DocumentBytes()` always read
  `store_.ReadActiveDocumentBytes()`. After a recovery the published
  in-memory candidate is the valid backup generation while the durable
  `active` marker still points at the corrupt one, so the UI's
  document fetch received malformed JSON
  (`workspace document failed to parse: malformed json`) even though
  the engine had recovered cleanly. `DocumentBytes()` now serves the
  in-memory adopted candidate for the current workspace
  (`src/engine/WorkspaceService.cpp`); the recovery regression in
  `tests/ui/ui_workspace_recovery_test.cpp` asserts the post-recovery
  fetch parses.
- **save-copy asserted source revisions on a new target** —
  `WorkspaceController::saveCopy()` sent the *source* workspace's
  `base_revision`/`resource_revision` for a brand-new id, so the store
  honestly refused (`stale revision … 'qa-copy' is at {0,0}`). The
  verb asserts the target does not exist — the controller now sends
  `{0,0}` (`src/ui/WorkspaceController.cpp`). The focused-text save
  workflow below proves the corrected path end-to-end.

### New socket-driven QA workflows (`qa/workflow_extra.py`, `qa/transcript-extra.json`, shots 21/22/30–33)

- **Focused-text save** — real `type` op (per-char `QKeyEvent`s) into
  `wsCopyName` read back `qa-copy`, `wsSaveCopy` committed, and the
  engine `list` verb returned `qa-copy` as a managed workspace
  (`copyname-type`, `savecopy-settled`, `copy-present` — all pass;
  shots `30-savecopy-typed.png`, `31-savecopy-done.png`).
- **Conflict-bundle import + choice** — `conflict-bundle.or2studio`
  path typed into `importPathField` (the dialog takes a typed path —
  no native file picker is involved, so no substitution was needed),
  `inspectBtn` → report (`importSummary` text captured:
  "1 type(s) · 0 error(s)"), the conflict row rendered as
  `rename_qa-strip` with the engine's suggested id, an explicit rename
  was typed (`clear`+`type`), `confirmImportBtn` committed, and the
  new workspace `conflict-bundle-or2studio` appeared in the engine
  list (shots `32-conflict-report.png`, `33-conflict-committed.png`).
  An earlier run also proved the honest rejection path: typing a
  rename that collides with an already-installed id fails with
  `choices.qa-strip: '<id>' collides with an existing type id`.
- **External change on the package** — engine-side tamper of the
  active generation → UI banner `modified` (`ext-change-visible`),
  `wsReloadDisk` click → recovered last-known-good generation
  (`reload-settled-2`, distinct before/after captures
  `22-external-change.png` / `21-recovered.png`). A successful bundle
  import adopts the new workspace, so the script re-opens `qa-desk`
  before this phase; the tamper also waits for the engine-settled
  check — an open still in flight would recover during the open
  itself and the `modified` window would never reach the UI.

### Screenshot-integrity reconciliation

Per-file hashes recorded in `qa/shots-manifest.json`. Disposition:

- **`shots/01–14*.png` (numbered workflow shots)** — every one was
  captured by `op:shot` in `transcript-pkg-round1.json` /
  `transcript.json` on the packaged stack; the transcript replies
  carry the `shots\` destination paths. Byte-identity to `shots-dev/`
  counterparts is expected, not copied: the seeded workspace renders
  static looks, so the same 1440×958 window produces identical pixels
  and identical PNG bytes across runs.
- **`shots/*-ext.png`, `probe*.png`, `10/11-layout-3d-desk*.png`,
  `12-layout-3d-framed.png`, `13-layout-2d.png`, `14-2d-selected.png`,
  `15-lighting-layers.png`, `16-saved.png`, `10-recovery-offer.png`,
  `20-recovery-banner.png`** — captured on the packaged stack in the
  evidence session (file mtimes predate the `shots-dev/` mirror);
  identical bytes to the dev set for the same static-scene reason.
- **`shots/15-narrow-layout.png`** — fresh packaged capture at a real
  960 px window width (OS-level `SetWindowPos`; the QA socket has no
  resize op — noted as a harness limitation, not a claim).
- **`shots/16-scaled-150.png`** — fresh packaged capture under
  `QT_SCALE_FACTOR=1.5`: 2894×1421 px surface (1929×947 logical ×1.5),
  `connState: connected`.
- **`shots/qual-low/balanced/high.png`, `reduced-motion.png`** —
  fresh packaged captures, all distinct hashes.
- **`shots/11-external-change.png` ≡ `shots/12-recovered.png`** —
  legitimate same-state captures: the external-change banner was still
  visible when "recovered" was snapped (the reload settle raced the
  capture). The claim is now carried by `22-external-change.png` /
  `21-recovered.png`, which are distinct.
- **`shots-dev/16-scaled-150.png` ≡ `shots-dev/17-scaled-narrow.png`**
  — dev-run capture artifact: both names captured the same scaled
  surface; the narrow-at-scale state was not an independent capture.
  The packaged narrow claim uses `shots/15-narrow-layout.png`.
- **`shots-dev/14-devices-empty.png` ≡ `shots-dev/qual-balanced.png`**
  — dev-run timing artifact: `qual-balanced` was captured before the
  Devices→viewport navigation landed. The packaged `qual-balanced.png`
  is a fresh distinct capture on the Lighting/3D surface.

### Packaging / docs / logs

- `docs/packaging/source-manifest.md` — Quick3D runtime + QML modules
  called out as required (debug-DLL exclusion must not match
  `Qt6Quick3D.dll`); `profiles\local\workspaces\` documented as the
  writable in-package workspace root.
- `task12-evidence/ctest-full.log` — complete
  `ctest -C Release --output-on-failure` output: **78/78 pass**
  (132 s).
- `task12-evidence/package-run/launch-env.json` + `launch-check.json`
  — clean-PATH launch record (`PATH=<pkg>;System32;Windows`) plus
  live engine `get` + UI QA-socket `ping`/`state` under that env.
- `task12-evidence/package-run/bin-hashes.json` — staged
  `openrgb2-{engine,ui,ctl}.exe` SHA-256 == `build-studio` binaries,
  i.e. the package carries this fix round's binaries (restaged via
  `stage-release.ps1 -BuildDir build-studio`, manifest verified
  1,449 files).

### Tests re-run after this round's code changes

```
ctest -C Release -L ui --output-on-failure                      29/29
ctest -C Release -R ipc_workspace_test      --output-on-failure  pass
ctest -C Release -R e2e_studio_workspace_test --output-on-failure pass
ctest -C Release --output-on-failure                            78/78
```
