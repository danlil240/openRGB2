# R-5: Temperature-reactive scalar input (read-only sensor export)

Status: proposed · Size: M · Source: audit 2026-11-28 R-5

## Goal

A scene can follow a temperature: for example, the case fans shift
from blue to red as the GPU warms from 40 to 80 °C. The engine reads
values that another tool already exports. It never touches sensors,
PWM, pumps, or FanControl itself. FanControl stays the cooling owner,
unstarted, unsignalled, and unprobed.

## Current state

- **`InputState`** (`src/core/effects/EffectTypes.h:51-58`) carries
  events, the screen grid, and one `audio_level`. **`InputBus`**
  (`src/core/inputs/InputBus.h:36-39`) has `SetScreenGrid`, `SetAudioLevel`,
  and `PushEvent`. It has no named scalar.
- **The `level` primitive** (`src/core/effects/EffectTypes.cpp:485-497`)
  samples the palette at `audio_level`, with alpha equal to the level.
  `EffectLayer::source` is validated to `"" | audio | key | screen`
  (`src/core/effects/EffectJson.cpp:826-845`,
  `src/core/schemas/effect.schema.json:170-174`).
- **`ReactiveInputs`** (`src/engine/ReactiveInputs.h`) has three fixed
  slots: audio, screen, and key. Each slot has an `IReactiveSource`,
  the pause gate, the honest pre-flight, and persistence in
  `EngineState.reactive`. The Windows sources live in
  `src/platform/windows/`.
- **Constraints already in force:**
  - `docs/hardware/daniel-pc.md:41-43`: "FanControl owns cooling. RGB
    work must not touch PWM, pump speed, or sensor access."
  - `OwnershipScan` classifies `fancontrol.exe` as "cooling owner —
    expected; must keep running untouched"
    (`src/engine/OwnershipScan.cpp:294-302`).
  - A PawnIO failure once took out both RAM RGB and FanControl sensors.
- **Reactive classification.** `PlaybackPresets.cpp:185-210` marks
  scenes with reactive layers as live-only.
  `EffectEditorController.cpp:583-590` derives a look's `needs`.

## Design

**Input: a fourth reactive slot, `"sensor"`.**

- One source holds up to 8 named channels. Each channel is
  `{name, adapter_key, lo, hi, smoothing}`.
- The channel publishes a normalized value: `clamp01((value - lo) /
  (hi - lo))`, smoothed by an EMA (default τ = 2 s).
- The raw value is reported only to the UI for configuration, never to
  effects.
- **Polling:** 1 Hz on the source's own thread.
- **Pause:** the gate stops polling, as the screen sampler does.
- **Stale data:** a value older than 10 s is "stale". The channel then
  holds its last value and reports it. It never fabricates a number.

**Adapters.** Each adapter reads data that someone else already
produces. Neither opens a driver, a device, an SMBus, or a WMI sensor
class.

1. **JSON file** (generic, ships first). A user-chosen file, either a
   flat map `{"<key>": number}` or a `key=value` text file. Bounds: 64
   KiB, 64 keys, and read-only open with `FILE_SHARE_READ |
   FILE_SHARE_WRITE`, so the writer is never blocked. Any exporter can
   feed it.
2. **HWiNFO shared memory** (second). Open `Global\HWiNFO_SENS_SM2`
   with `OpenFileMappingW(FILE_MAP_READ)` and map it read-only. Walk
   the documented sensor and reading arrays, bounds-checking every
   offset against the mapping size, and match the reading by
   sensor label plus reading label. If the mapping is absent, the
   pre-flight fails with "HWiNFO shared memory not enabled".

FanControl: no external export is known to us (not verified). If one
exists, or a user tool writes its values to a file, adapter 1 covers
it. Nothing here loads code into FanControl.

**Core.**

- `InputState` gains `std::vector<std::pair<std::string, float>>
  scalars` (bounded at 8), and `InputBus` gains `SetScalar(name,
  value)` and `ClearScalars()`.
- `level` reads `source`. `""` and `"audio"` keep `audio_level`
  (unchanged behaviour). `"sensor.<name>"` reads that scalar. A missing
  scalar gives zero coverage, as a null input does today.
- The source validator and schema accept `sensor.[a-z0-9_]{1,24}`.
  The palette does the color mapping. There are no new layer fields.

**Plumbing.**

- `PlaybackPresets` reports "scene reacts to sensor input — reactive
  scenes stay live effects".
- The editor's `needs` becomes `"sensor"`.
- The workspace `InputSettings` (`src/core/config/StudioConfig.h:108`)
  gains `sensor` (bool). The import sanitizer forces it false, the same
  rule as the other inputs.

**IPC:** `REACTIVE_COMMAND` with `input:"sensor"`.

- `set_enabled` works as for the other inputs.
- `set_params {input:"sensor", adapter:"file"|"hwinfo",
  path?, channels:[…]}`. Paths go through the same no-client-path
  rule as elsewhere: an absolute path chosen in the UI, read-only, and
  never created.
- The REACTIVE body gains `sensor:{adapter, channels:[{name, raw,
  norm, stale, detail}]}`.
- `docs/ipc-protocol.md` §5.

**UI:** the Lighting page reactive card gets a Sensor row with the
adapter and file pickers, and per channel: name, key, lo/hi, and a
live raw value. The effect editor offers `sensor.<name>` in the
`level` source combo.

**Rejected alternatives**

- In-process LibreHardwareMonitor, WinRing0, PawnIO, or WMI
  `MSAcpi_ThermalZoneTemperature`: each is sensor access, which the
  rule forbids, and the PawnIO history shows the risk.
- Querying FanControl's process or UI: that is probing the cooling
  owner.
- A new `temperature` primitive: `level` with a source covers it, and
  each new primitive costs schema, editor, and bake-classification work.

## Steps

1. Core: `InputState.scalars`, `InputBus::SetScalar`, `level` source
   routing, and source validation and schema. Tests:
   `studio_scene_test` (scalar drives the palette; a missing scalar
   is dark; audio behaviour unchanged) and `effect_json_test` (source
   pattern accepted, junk rejected).
2. Channel normalization, EMA, and staleness as a pure helper
   (`src/core/inputs/ScalarChannel.*`). Test: new
   `scalar_channel_test` (core).
3. `ReactiveInputs` fourth slot with `set_params` channel config and
   persistence. Tests: `reactive_inputs_test` with a fake source, and
   `ipc_reactive_test`.
4. The file adapter (Qt-free parse in the engine; only the open runs on
   the platform side). Test: `reactive_inputs_test` covering parse
   bounds, malformed files, and a writer replacing the file mid-read.
5. The HWiNFO shared-memory adapter, with the layout parse kept separate
   from `MapViewOfFile`. Test: a parser fed a synthetic buffer,
   including truncated and lying offsets. Manual test against a real
   HWiNFO.
6. Plumbing: bake classification, editor `needs`, and the workspace
   `inputs.sensor` flag with sanitize. Tests: `playback_mode_test`,
   `ui_studio_effects_test`, and `workspace_bundle_test`.
7. UI rows and editor source combo. Tests: `ui_reactive_test` and
   `qml_smoke_test`.

## Tests

- Core label: `studio_scene_test`, `effect_json_test`,
  `scalar_channel_test` (new).
- Engine label: `reactive_inputs_test`, `ipc_reactive_test`,
  `playback_mode_test`, `workspace_bundle_test`.
- UI label: `ui_reactive_test`, `ui_studio_effects_test`,
  `qml_smoke_test`.
- **Manual:**
  - FanControl keeps running through enabling, disabling, and an
    adapter failure. Its PID and sensor values are unchanged before and
    after (the `docs/benchmarks/non-interference.md` method).
  - A file written by a script drives a fan ring.
  - HWiNFO with shared memory on gives a live GPU temperature.

## Risks and open questions

- **Daniel:** which exporter does he actually run? HWiNFO alongside
  FanControl means two sensor pollers on the same buses. That is his
  call, and this plan never starts HWiNFO.
- HWiNFO's free edition may time-limit shared memory (reported,
  not verified). The adapter must report "stale", not freeze silently.
- Smoothing defaults (τ = 2 s) and stale threshold (10 s) are guesses.
- A file path stored in engine state is user data. It stays in
  `profiles/local/`, never in committed workspaces. Workspaces store
  only the `sensor.<name>` channel names.
