# E-12b: Exit/idle output policy

Status: implemented on `feat/e-12b-exit-idle-policy` (not merged; OUTPUT_COMMAND verbs pending R-1) · Size: M · Source: audit 2026-11-28 E-12 (part b) · Related: E-10, P-11, R-1

## Goal

Daniel chooses what the LEDs show when the engine stops, and while it
has nothing to drive:

- **hold:** the last frame stays on the LEDs. This is today's behaviour
  and stays the default.
- **black:** all lights go off.
- **restore:** each device goes back to the hardware mode it was in
  before OpenRGB2 switched it to Direct.

Today a quit leaves a frozen mid-animation frame on every Direct-mode
device.

## Current state

- **`EngineHost::Stop`** (`src/engine/EngineHost.cpp:1837-1925`) runs
  these steps, in order: `power-monitor`, `ipc-listeners`,
  `ipc-server`, `calibration`, `workspace-jobs`, `scene-worker`,
  `reactive-inputs`, `delivery-pipeline`, `sdk-transport`, then
  optionally `backend-handoff`. Nothing writes a final frame.
- **Direct mode:** the custom-mode upkeep in `PollOnce`
  (`:1620-1672`) calls `sdk_->SetCustomMode(key)` once per topology
  generation per bound controller. It skips groups owned by an
  uploaded loop or by calibration, and pauses while the power gate is
  on. The prior mode is never recorded.
- **SDK:** `OpenRgbClient` has `ActiveMode` (`:205`) and `SelectMode`
  (`:255`, `:261`) (`src/backends/openrgb/OpenRgbClient.h`), so the
  mode can be read and restored.
- **Uploaded loops:** `PlaybackController` already has a stop-black
  write and a pause-hold (the LightingPage transport says "Stop
  replaces playback with black"). An uploaded loop keeps playing on
  the device after the engine exits.
- **No idle policy.** With no scene applied, `SceneWorker::Step`
  produces nothing (`src/engine/SceneWorker.h`, idle-hook comment), so
  the LEDs keep whatever was last written.
- **Shutdown coverage:**
  - An OS shutdown or log-off skips `Stop()` entirely (E-10). The power
    monitor's message-only window never gets `WM_ENDSESSION` (P-11).
  - A policy therefore applies only on an orderly quit, which covers
    tray Quit, `QUIT`, and the stop script, until E-10 lands.

## Design

**Policy model** (`src/engine/OutputPolicy.h/.cpp`, Qt-free):

`EngineState.output_policy = {on_exit, on_idle, per_controller:[{match,
on_exit?, on_idle?}]}`.

- Each value is `hold`, `black`, or `restore`.
- `match` uses the same `{device?, location_prefix?, serial?}` shape as
  E-12a.
- Defaults: `on_exit:"hold"`, `on_idle:"hold"`. That means no
  behaviour change until Daniel opts in.

**Recording the prior mode.** Before the upkeep's first
`SetCustomMode` for a key, call `ActiveMode` and store
`prior_mode_[key] = {name, speed, direction, colors}`. Only the first
observation per key per engine run counts. It is kept in memory: a
crash or restart loses it, so `restore` then falls back to `black` and
says so in the log.

**Exit step.** A new `output-policy` step runs after `scene-worker`
and `reactive-inputs`, and before `delivery-pipeline`. Its bound is
2000 ms. Per bound controller:

- **`black`, live lane:** `pipeline_->SubmitReliable(black frame)`,
  then `WaitIdle(key, 500 ms)`.
- **`black`, group owned by an uploaded loop:**
  `playback_->` stop-black (the existing owner write).
- **`restore`:** `sdk_->SelectMode(key, prior.name, …)`. It goes
  directly to the transport after the lane is idle. A key without a
  recorded mode falls back to `black`.
- **`hold`:** do nothing.
- **Skips:**
  - The power gate is on, because writing would wake or cross a sleep
    boundary. That is the same rule the upkeep follows.
  - Calibration owns the group.
- Results go into the shutdown report: `{key, policy, outcome}`.

**Idle.** On the owner loop, "idle" means no scene has been applied for
5 s: after boot with nothing to adopt, or after a workspace closes.
Then `on_idle` runs once per idle entry, through the same per-key
executor, and the next applied scene clears it. Display-off and
suspend are not idle. `PowerLifecycle` owns those. User idle dimming
is R-4.

**IPC:**

- `get_policy` and `set_policy {…}` on `OUTPUT_COMMAND` (R-1, or
  introduced by E-12a).
- The policy shows in `STATE`.
- The `QUIT` reply gains `exit_policy` so a client can show
  "lights will turn off".

**Rejected alternatives**

- Writing black in a destructor: it is unordered and runs after the
  transport is gone.
- Asking the backend to reset devices on client disconnect: it is a
  vendored-backend change, and other SDK clients would be affected.
- `restore` by re-reading the mode at exit: by then the mode is
  Direct, set by us.

## Steps

1. `OutputPolicy` model, matching, and the `EngineState` round-trip.
   Tests: new `output_policy_test` (engine) and `profile_store_test`.
2. Prior-mode recording in the custom-mode upkeep. Test:
   `e2e_fake_backend_test`. The fake SDK reports mode "Rainbow" before
   `SetCustomMode`, and the recorded entry matches.
3. Exit step: the per-key executor (black via reliable, loop
   stop-black, restore via `SelectMode`, power-gate and calibration
   skips, and the shutdown report). Tests: `shutdown_test` (step order
   and bound) and `e2e_lifecycle_test`. After `Stop()` with `black`,
   the fake transport's last frame per key is all zero. With `restore`,
   the fake saw `SelectMode("Rainbow")`. With the gate on, nothing was
   written.
4. Idle entry and exit on the owner loop. Test: `e2e_fake_backend_test`
   with an injected clock.
5. IPC verbs, the `STATE` field, and the `QUIT` reply field. Update
   `docs/ipc-protocol.md`. Test: `ipc_engine_service_test`.
6. Settings UI: two combos (on exit, on idle). Test: `qml_smoke_test`.

## Tests

- Engine label: `output_policy_test` (new), `profile_store_test`,
  `e2e_fake_backend_test`, `e2e_lifecycle_test`, `shutdown_test`,
  `ipc_engine_service_test`.
- UI label: `qml_smoke_test`.
- **Manual:** for each policy, quit from the tray and check:
  - The G512 and the motherboard ARGB go black, or back to their
    hardware effect.
  - The wireless group's loop either keeps running (hold) or goes
    black (black).
  - Repeat with the stop script.

## Risks and open questions

- **Daniel:** should `restore` be offered for the wireless group, or
  should that group stay on `hold`/`black` only? Re-selecting a hardware
  mode there may re-upload or behave unlike wired devices. Not
  verified.
- **Daniel:** should the default change to `black`? A frozen frame is
  the complaint, but `hold` is the zero-surprise default.
- The exit step adds up to 2 s to quit in the worst case (500 ms per
  `WaitIdle`, with lanes in parallel through the lane workers). Keep
  the measured bound in the shutdown report.
- Until E-10/P-11 land, OS shutdown and log-off still freeze the last
  frame.
