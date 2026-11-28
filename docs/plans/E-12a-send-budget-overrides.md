# E-12a: Per-controller send-budget overrides

Status: proposed · Size: S · Source: audit 2026-11-28 E-12 (part a)

## Goal

Daniel can set a send pacing interval for any controller (for example
"G512: 20 ms", "Razer mouse: 33 ms") without rebuilding. A device that
flickers or drops packets under the default unpaced 60 Hz stream then
gets a fix in the config, not a new `if` in `BudgetFor`. Overrides
persist, show in Diagnostics, and can never pace the wireless group
faster than its measured ceiling.

## Current state

- `BudgetFor(const DeviceSnapshot&)` lives in an anonymous namespace in
  `src/engine/EngineHost.cpp:50-72`. It hard-codes 30 ms for the
  wireless group and 45 ms for SMBus I2C GPUs (`device_type == 2` with
  an `I2C:` location). Everything else gets zero, which means unpaced.
- `SyncLanes` (`:75-91`) applies the budget only when it changed, and
  runs from `Start` and from each `PollOnce` sync (`:900`, `:1574`).
- `DeliveryPipeline::SetBudget` and `Budget` already support any
  per-lane interval (`src/engine/DeliveryPipeline.h`, "Per-controller
  send budget").
- **Known pacing fixes elsewhere:**
  - The G512 tail-key drop was fixed in the vendored driver (a 2 ms
    gap per HID++ 0x8080 frame), not by an engine budget.
  - The 45 ms GPU budget came from a wire trace on the AORUS RTX 5080.
  - Each of these was a code change.
- `DeviceKey` is `{device, serial, location, zone}`
  (`src/engine/EngineTypes.h:63-69`). Bindings already resolve
  controllers by stable identity, never by enumeration index.

## Design

**Rule table.** `EngineState.send_budgets` holds
`[{match:{device?, location_prefix?, serial?}, min_interval_ms}]`.

- Matching runs in order: the first rule whose given fields all match
  wins. `device` is an exact name, `location_prefix` is a `rfind(…, 0)`
  test, and `serial` is exact.
- Persisted through `ProfileStore`. The unknown-key retention
  (`src/engine/ProfileStore.cpp:764-796`) keeps older builds safe.
  Serials are unique identifiers and stay in `profiles/local/`,
  never in committed files (the `docs/hardware/daniel-pc.md` rule).

**Resolution** (a pure function, moved out of the anonymous namespace
to `src/engine/SendBudget.h/.cpp`):

`ResolveBudget(snap, rules) -> {interval, source}`. `source` is one of
`"wireless_floor"`, `"gpu_i2c_default"`, `"override"`, or `"default"`.

- The built-in defaults stay as the base.
- An override replaces the base, with one exception: for the wireless
  group the result is `max(override, 30 ms)`. The ~33.5 uploads/s
  ceiling (`docs/benchmarks/wireless-result.md`) is a floor.
- Bounds: 0–1000 ms. Values out of range are rejected at validation,
  not clamped silently.

**Application.** `SyncLanes` calls `ResolveBudget`. A rule change
from IPC sets a `budgets_dirty_` flag, and the next `PollOnce` re-runs
`SyncLanes`. The existing "only on change" guard keeps lanes quiet.

**IPC**

- Budgets are engine runtime config, not document state, so they do
  not belong in `LAYOUT_COMMAND`. Add the verbs `get_budgets` and
  `set_budgets {rules}` to `OUTPUT_COMMAND` (introduced by R-1). If
  E-12a lands first, it introduces `OUTPUT_COMMAND` with only these two
  verbs, and R-1 extends it.
- `GET_DELIVERY_STATS` per controller gains `budget_ms` and
  `budget_source`, so Diagnostics shows why a lane is paced.

**UI:** a per-row budget and its source on the Diagnostics page's
delivery card (`src/ui/DiagnosticsController.cpp`), plus an edit field
(ms, empty means default).

**Rejected alternatives**

- A CLI flag such as `--budget name=ms`: it is not persisted, and the
  packaged launcher would need editing.
- A global rate: plan section 4 says budgets are qualified per
  controller.
- Matching by enumeration index: it breaks on replug and reordering.

## Steps

1. Extract `ResolveBudget` with the built-in defaults unchanged. Test:
   new `send_budget_test` (engine). It feeds wireless, GPU-I2C, and
   plain snapshots and expects today's values, so the change is a
   no-op.
2. Rule matching, the wireless floor, bounds, and the `EngineState`
   round-trip. Tests: `send_budget_test` and `profile_store_test`.
3. `SyncLanes` uses the rules, and the dirty flag re-applies them.
   Test: `e2e_fake_backend_test`. An override on a fake controller
   shows up in `pipeline.Budget()`, and the measured `min_interval_s`
   is at least the override.
4. IPC verbs and the `budget_ms` / `budget_source` stats fields.
   Update `docs/ipc-protocol.md`. Test: `ipc_engine_service_test`.
5. Diagnostics UI fields. Tests: `ui_diagnostics_test` and
   `qml_diagnostics_test`.

## Tests

- Engine label: `send_budget_test` (new), `profile_store_test`,
  `e2e_fake_backend_test`, `ipc_engine_service_test`.
- UI label: `ui_diagnostics_test`, `qml_diagnostics_test`.
- **Manual:**
  - Set G512 = 33 ms and confirm through the E-12c histogram, or
    `engine_ctl` stats, that the lane interval holds.
  - A wireless override of 10 ms reports 30 ms with source
    `wireless_floor`.

## Risks and open questions

- **Daniel:** should overrides be editable from the UI at all, or only
  through `engine_ctl` and the state file? A UI field makes it easy to
  set a bad value. The floor and bounds limit the damage.
- Should Diagnostics recommend budgets from E-12c latency data? That
  is out of scope here, and a candidate follow-up.
- A too-large override makes animation visibly steppy. Diagnostics
  should show the effective fps (`1/budget`) beside the field.
