# R-6: Audio spectrum bands (16-band FFT + `spectrum` primitive)

Status: proposed · Size: M · Source: audit 2026-11-28 R-6

## Goal

Music-reactive scenes that tell bass from treble. The engine turns the
system audio loopback into 16 smoothed frequency-band levels, and a new
`spectrum` primitive spreads them across the LEDs. For example, a fan
ring or keyboard row becomes an EQ: low bands on one side, high on the
other. Only band energies leave the capture thread, as `level` does
today.

## Current state

- **`WinAudioLoopback`** (`src/platform/windows/WinAudioLoopback.cpp`).
  - It runs WASAPI shared-mode loopback on the default render endpoint.
  - The pump sleeps 5 ms and drains packets (`:357-412`).
  - Each packet becomes one mean-square energy (`ChunkEnergy`, `:99`),
    which feeds `OnsetDetect`. The result goes to
    `InputBus::SetAudioLevel` and to `PushEvent("audio")` onsets.
  - The samples themselves are discarded. Nothing computes frequency
    content.
- **`OnsetDetect`** (`src/core/inputs/OnsetDetect.h`) works on energy
  only. It is not beat tracking.
- **`InputState`** (`src/core/effects/EffectTypes.h:51-58`) has a single
  `audio_level`. **`level`** (`EffectTypes.cpp:485-497`) is the only
  audio-continuous primitive.
- **Primitive registration points:**
  - `EvalPrimitive` (`EffectTypes.cpp:500-516`)
  - `IsPrimitive` (`src/core/effects/EffectJson.cpp:877-890`)
  - the schema enum (`src/core/schemas/effect.schema.json:107`)
  - editor defaults (`src/core/editor/EditorController.cpp:1898-1910`)
  - bake classification (`src/engine/PlaybackPresets.cpp:185-210`)
  - `needs` derivation (`src/ui/studio/EffectEditorController.cpp:583-590`)
  - the Lighting page needs-gate (`ui/pages/LightingPage.qml:130-140`)
- No FFT exists in `src/` or the vendored tree.

## Design

**Core DSP: `src/core/inputs/Spectrum.h/.cpp`** (Qt-free, deterministic,
and testable offline).

- `SpectrumAnalyzer::Feed(const float* mono, size_t n, double rate)`
  writes into a 2048-sample ring. Every hop of 512 samples (≈10.7 ms at
  48 kHz) it runs a Hann window, then an in-place radix-2 FFT (about 60
  lines, so no new dependency), then sums power into 16 log-spaced bands
  from 40 Hz to 16 kHz. Band edges are computed once per sample rate.
- Per band it applies, in order:
  1. Conversion to dB.
  2. An adaptive ceiling: a slow-decaying peak with τ ≈ 3 s and a floor
     of -70 dBFS, so quiet and loud mixes both use the range.
  3. Normalization to 0..1.
  4. Attack/release smoothing, ≈ 15 ms and ≈ 120 ms.
- The existing `sensitivity` (0.25–2.0) scales the ceiling.
- `Bands()` returns `std::array<float, 16>`. `Reset()` runs on device
  change and on pause.
- Cost: a 2048-point FFT about 94 times a second, on the audio thread
  that already exists. It is expected to be well under 1 % of a core
  (a guess, to be measured in step 6).

**Capture.** `WinAudioLoopback` mixes each packet down to mono float,
for the same formats `ChunkEnergy` supports. It feeds the analyzer
next to `OnsetDetect`, then calls `bus_->SetAudioBands(bands)`.
Pausing drains the packets and feeds nothing, as today. Stop and
reinit clear the bands.

**Bus and state.** `InputBus::SetAudioBands`, and an `audio_bands`
member (`std::array<float,16>` plus a `has_bands` flag) on
`InputState`. `Idle()` also checks the bands.

**`spectrum` primitive.**

- The emitter position is projected onto `direction` around `origin`,
  over a width of `scale` meters, using `SamplePos`. Gradient uses the
  same projection (`EffectTypes.cpp:253-261`).
- The projection gives `u` in 0..1, which picks a band position
  `u × 15`, linearly interpolated between neighbours. The layer's
  `phase` mirrors the position: `phase` ≥ 0.5 puts high frequencies on
  the left.
- Color: `palette.Sample(u)`, so the frequency position sets the hue.
- Coverage: `band × opacity`.
- With no input, or with no bands, coverage is zero, as for `level`.
- There are no new layer fields. `source` is implicitly `audio`, and
  the editor sets `source:"audio"` on add, as it does for
  `screenfield → "screen"`.

**Plumbing.**

- Add `spectrum` to `IsPrimitive`, the schema enum, the editor
  primitive list (`EditorController.h:293`), and the `SceneTypes.h:183`
  comment.
- Bake classification: "scene reacts to audio input — reactive scenes
  stay live effects".
- `needs` is `"audio"`, and the Lighting page gate gets the same rule as
  `level`.
- Ship one packaged look, `spectrum-bars.effect.json`, under
  `src/core/presets/effects/` so the feature is visible.

**Rejected alternatives**

- A configurable band count of 8 or 16 on the wire: interpolation
  already gives fewer LEDs a coarser view, and a fixed 16 keeps
  `InputState` fixed-size.
- Overloading `level` with a band index: band position along the
  geometry is what makes it a spectrum.
- Shipping raw FFT bins to the scene: it is larger, and that is a
  privacy regression from "energies only".
- KissFFT or another library: it is a new vendored dependency for
  about 60 lines of code.

## Steps

1. `Spectrum` DSP: FFT, band edges, dB/AGC, and smoothing. Test: new
   `spectrum_test` (core).
   - A pure 100 Hz sine peaks in band 1 or 2, and a 5 kHz sine peaks
     in the expected band.
   - Silence decays to 0.
   - Band edges are monotonic for 44.1, 48 and 96 kHz.
   - The output is deterministic for fixed input.
2. `InputBus::SetAudioBands` and `InputState.audio_bands`. Test:
   `reactive_inputs_test` (a fake source pushes bands, the snapshot
   carries them, and a paused snapshot is empty).
3. `spectrum` primitive, plus `IsPrimitive` and the schema. Tests:
   `studio_scene_test` (synthetic bands map left to right, `phase`
   mirrors, no input is dark) and `effect_json_test`.
4. Capture integration in `WinAudioLoopback`: mono downmix, Feed, and
   Reset on reinit and pause. Manual test (platform code is not in the
   offline suites).
5. Plumbing: editor defaults and list, bake classification, `needs`,
   the Lighting gate, and the packaged look. Tests:
   `studio_editor_test`, `playback_mode_test`,
   `ui_studio_effects_test`, `qml_smoke_test`, and
   `effect_json_test`. The packaged look loads through
   `DLS_EFFECT_DIR`. If the suite pairs every packaged look with a
   `tests/core/fixtures/effects/*.fixture.json`, add that fixture too.
6. Measure the CPU cost of the audio thread before and after
   (Task Manager or ETW, 5 min of music). Record the result in
   `docs/benchmarks/`.

## Tests

- Core label: `spectrum_test` (new), `studio_scene_test`,
  `effect_json_test`, `studio_editor_test`.
- Engine label: `reactive_inputs_test`, `playback_mode_test`.
- UI label: `ui_studio_effects_test`, `qml_smoke_test`.
- **Manual:**
  - A sine sweep from 40 Hz to 16 kHz through the default output: the
    lit band walks from one end to the other on a fan ring and the G512.
  - Changing the default device mid-play recovers.
  - Display-off pauses the bands, and resume does not burst.

## Risks and open questions

- **Daniel:** should the palette follow frequency position (this plan)
  or amplitude (bass-heavy hits turn red)? Either way the palette
  sampling stays in the primitive. A second mode would need a layer
  field.
- The AGC constants and band range are starting guesses. Tune them by
  ear on real music.
- At 60 Hz the wireless group is paced to 30 ms
  (`src/engine/EngineHost.cpp:50-55`), so fast transients look softer
  there than on wired devices. The 120 ms release hides most of it.
- Exclusive-mode audio apps bypass the loopback mix, so the spectrum
  goes silent. That is the same limitation `level` has today.
