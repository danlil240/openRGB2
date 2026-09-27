# Non-Interference — Task 5.4 (Milestone 5)

Verification that RGB activity never wakes a sleeping display, never
misroutes a binding to a same-named different device, and never
interferes with FanControl / fan / pump / sensor operation — while
running live and uploaded playback at full rate.

## Builds under test

| Component | Version |
| --- | --- |
| `openrgb2-engine` + `openrgb2-ctl` | branch `milestone-5`, working tree with Task 5.4 changes (this commit) |
| Managed backend | `vendor/OpenRGB` lianli-wireless fork, `OpenRGB v1.0+ (git)` |
| Backend launch | `OpenRGB.exe --server --server-port 6744 --localconfig`, cwd `profiles/local/nonint/run` |
| Engine launch | `openrgb2-engine --sdk --scene …/scene-playback.json --state …/engine-state.json --backend-config …/run` |

## Topology observed

5 controllers via SDK on `127.0.0.1:6744`: Gigabyte AORUS GeForce RTX
5080 MASTER ICE (GPU lane — the display-wake hazard), X870E AORUS ELITE
WIFI7 ICE, UNI FAN SL V3 Wireless 66:E1 (120 LEDs, uploaded-playback
target), G512 keyboard, Razer Basilisk V3 Pro. DIMMs absent (PawnIO
permission failures, unelevated — same as Tasks 5.2/5.3).

## Item 1 — display-sleep GPU/controller writes: **PASS**

### What had to change (defect found by this task)

The Task 5.3 residual was real: `PowerLifecycle` gated the delivery
lanes on *any* output pause but asserted `PlaybackController`'s
`power_gate_` only on **suspend**. During display-off an IPC
`SET_PLAYBACK_MODE` request ran the full transition — mode select +
transport control writes crossed the wire to hardware while the
display was dark (confirmed: the first e2e run observed one
`RGBCONTROLLER_UPDATEMODE` event inside the gated window).

Fixes (this commit):

- `PowerLifecycle::ApplyGateAndCapture` now asserts the playback gate
  on the SAME output gate as the delivery pipeline (display-off,
  suspend, and pending wake-restore), not just suspend.
- `PlaybackController::RequestImpl` — a request arriving while gated
  is validated, recorded (`requested_mode_`/`requested_control_`/
  `params_` + persisted per-scene selection), and **parked**:
  `accepted + pending`, zero writes. The first ungated `Poll()` tick
  replays it through the normal validated path; the latest parked
  intent wins; a post-wake rejection is reported honestly in
  `status_detail_`. `RevalidateAndRestore`'s `force_restore` path is
  exempt (it IS the designed wake restore).
- `hold_overridden_` distinguishes user intent from the lifecycle's
  own control writes: the wake un-hold/revert in `AttemptRestore` only
  reverts a hold that is still the power-imposed `Paused` — a user
  control request (or parked intent) from inside the off window is
  never clobbered. `SetIntendedControl`/`RequestPowerControl`
  (lifecycle calls) clear it.
- `AckPending()` includes the parked request, so IPC/UI reports
  pending work honestly across the gate.

### Structural proof

- `DeliveryPipeline::DrainDue()` — early return when `output_paused_`
  (`src/engine/DeliveryPipeline.cpp`); `LaneRun()` sleeps while
  paused; `SetOutputPaused` only asserts the flag/reason and wakes
  the lanes — the pending-frame discard is
  `PowerLifecycle::DropAllLanes` → `DeliveryPipeline::DropPending`,
  run inside the same `ApplyEvent` step.
- `EngineHost::PollOnce` — lifecycle poll runs before playback poll,
  so wake restore revalidates before output resumes; custom-mode
  upkeep is skipped while gated (Task 5.3).
- `PlaybackController::Poll` — parked replay precedes all maintenance;
  parked when `power_gate_` holds.

### Automated evidence

- `e2e_noninterference_test` (new, real `EngineHost` + in-process fake
  SDK): display-off → **zero write packets per lane** including a
  distinct GPU device index; a playback request inside the gate is
  parked, writes nothing, and replays to an applied upload after the
  display returns; live frames resume on the GPU lane post-wake.
- `power_lifecycle_test` T7/T8 (new): suspend-window requests are
  parked with zero backend ops; a second parked request overwrites the
  first; post-wake ordering proves revalidation strictly before any
  write; parked selection requests land post-wake.

### Physical evidence (real `SC_MONITORPOWER` on this rig)

| Window | Result |
| --- | --- |
| 15 s display off, live scene streaming all 5 lanes | `transport_completed` delta **+0 on every lane incl. the GPU** — the authoritative no-write claim; the worker kept producing (stats deltas over the window: `submitted` +31/lane — admitted lane frames, counted at the ungated edges; the gate-discarded work lands in `rejected`, +7165, never in `submitted`) |
| `set-playback animation_playback` issued while off | reply `{applied:false, pending:true, requested_mode:animation_playback}`; 4 s later `transport_completed` delta **+0 aggregate** |
| Display on (`SC_MONITORPOWER -1` + input wake) | parked request replays: `mode:animation_playback, upload:active, control:playing`, `pending:false`, `detail:null` — every upload byte strictly post-wake |

Artifacts: `profiles/local/nonint/stats-off-t0/t1/t2/t3.json`,
`engine.log` ("power: display off — RGB output paused, pending frames
discarded" ×3 clean transitions).

## Item 2 — missing-binding integrity: **PASS**

### Structural proof (existing + extended tests)

- `BindingResolver` — a non-empty binding serial requires a candidate
  with a **matching** serial; a same-named different-serial device is
  never a candidate → `Unresolved`, never reassigned. Name-only with
  multiple candidates → `Ambiguous`, never a silent pick.
- `ProfileStore::AugmentSceneBindings` (the `prepare_scene` hook) —
  completes name-only committed bindings from the persisted
  resolved-identity record; requires exact controller-name agreement
  so a re-purposed binding id is never rewritten.
- `EngineHost::RecordResolvedBindings` — only resolved bindings write
  identity to the profile; a failed resolution keeps the previous
  record.

### Automated evidence

- `device_mapping_test` `TestAugmentedIdentityImposterRejected` (new):
  full offline-binding chain — name-only scene binding → augment fills
  serial → imposter (same name, different serial/location) gets
  `Unresolved` + **zero frames** to its key → record unchanged →
  original device returns → re-resolves; name-only first-bind resolves
  to a single candidate (documented exception) and goes `Ambiguous`,
  never a silent pick, with two.
- `e2e_noninterference_test` Case 3: identity recorded in a real
  engine profile across a **restart** — imposter gets zero
  `UPDATELEDS` and `b-dimm` reports `unresolved`; re-adding the real
  device re-resolves it with frames only to its key.
- Existing coverage: `TestMissingDeviceBindingPreserved`,
  `profile_store_test` `TestOfflineBindings`.

Documented exception (unchanged): a **first-bind** name-only binding
with no recorded identity resolves to a single same-named candidate —
there is nothing stronger to pin to; the resolution is then recorded
and pinned from the next apply on. This is intended first-use
behavior, not a silent rebind of a pinned binding.

## Item 3 — cooling/sensor non-interference: **PASS**

Physical run (~5 min engine session, 17,432 scene frames):

| Phase | GPU fan | GPU temp | GPU power | FanControl |
| --- | --- | --- | --- | --- |
| baseline (before) | 0 % | 32 °C | 29.60 W | pid 15100 alive |
| uploaded playback ~50 s | 0 % | 32 °C | 28.3→29.8 W | alive |
| max-rate live ~50 s | 0 % | 32 °C | 30.0→30.2 W | alive |
| after | — | — | — | pid 15100, 176,396 K — never touched |

- Engine ownership scan classified FanControl "cooling owner —
  expected; must keep running untouched" (notice only, no conflict
  action). FanControl was never started, stopped, signaled, or probed
  by the RGB path — reads were `tasklist`/its own process liveness and
  `nvidia-smi` (GPU driver reads only).
- RGB stress changes nothing the cooling owner does: fan duty stayed
  at FanControl's curve value (0 % GPU fan at 32 °C), power draw
  flat within ~1 W noise, no pump/fan writes exist anywhere in the
  RGB delivery path (SDK verbs: UPDATELEDS/UPDATEMODE/SETCUSTOMMODE/
  SAVEMODE only — no fan-duty protocol).

Artifacts: `profiles/local/nonint/smi-{before,upload,upload-end,
live-start,live-end}.txt`, `engine.log` (290.5 s session, clean
1 ms ordered shutdown), `stats-*.json`.

## Verdict

| Item | Verdict | Evidence |
| --- | --- | --- |
| No recurring GPU (or any) RGB writes while display sleeps | **PASS** | physical +0 `transport_completed`/lane over 15 s off; e2e per-lane zero-writes; `DrainDue`/`LaneRun`/`SetPowerGate` gates |
| IPC playback request during gate — parked, replayed post-wake | **PASS (defect found & fixed)** | physical `pending:true`→post-wake `upload:active` with zero interim writes; e2e + unit tests |
| Missing binding never reassigned to same-named different device | **PASS** | serial wins → Unresolved; restart-level e2e imposter; mapper chain test |
| Cooling/sensors unchanged during RGB stress | **PASS** | FanControl alive/untouched; GPU fan/temp/power flat across live + uploaded stress |

### Environment note

Windows Smart App Control (ISG, enforce mode) issued transient
per-hash verdicts blocking several freshly-linked test and tool
binaries during this session (CI events 3033/3077). Relinking
produced passing verdicts; no per-path workarounds, exclusions, or
policy changes were used, per `vendor/provenance/MANIFEST.md`.
`ctest -C Release` final result: **51/51 pass**.
