# R-11: Compile deterministic looks into uploaded loops (60 fps on wireless)

Status: proposed · Size: L · Source: audit 2026-11-28 R-11

## Goal

Any look that depends only on time can be selected in **Animation
playback**. That covers Wave, Comet, gradients, spins, pulses and static
layers, but nothing reactive. The engine renders one seamless cycle of
the look for each Lian Li wireless group, compresses it, and uploads it
once. The fans then play it device-side at up to 60 frames per second
with no live RF traffic, while wired devices keep evaluating the same
look locked to the fans' clock.

## Current state

- **Uploads are backend-built presets only.**
  - The wireless controller maps an SDK mode (`UPDATEMODE` by name) to a
    `LianLiWireless::PlaybackPreset` and calls
    `controller->SetPlaybackLoop(BuildPlaybackLoop(request), revision)`
    (`vendor/OpenRGB/Controllers/LianLiWirelessController/RGBController_LianLiWireless.cpp:330-391`).
  - The nine presets are hard-coded renderers
    (`src/core/playback/PlaybackLoop.cpp:41-86`).
- **`BuildPlaybackLoop` already targets 60 fps.**
  - It uses `count = cycle_ms * 0.06` frames, with the interval in
    0.00625 ms units.
  - It shrinks the frame count until the tinyuz payload fits
    `MAX_COMPRESSED_BYTES` = 12288 (`PlaybackLoop.cpp:20-103`;
    `LianLiWirelessProtocol.h:43-45`: 255 LEDs and 4096 frames max).
- **A raw-frames entry point exists but is unused.**
  - `LianLiWirelessController::SetFrames(rgb_frames, frame_count,
    interval_ms)` (`LianLiWirelessController.cpp:92-127`) exists, but no
    SDK packet reaches it.
  - The only vendor SDK extension is packet 4000, wireless status
    (`vendor/OpenRGB/NetworkProtocol.h:217`, `src/backends/openrgb/SdkWire.h:66`).
- **Engine side.**
  - `ClassifyScenePlayback` allows Animation playback **only** when the
    scene names one of the nine presets (`src/engine/PlaybackPresets.h:123-150`,
    `.cpp:169`).
  - Wired devices follow the fans through `PlaybackFrameSampler`, which
    re-renders the preset, and `PlaybackClockSync`, which gives the phase
    from the wireless RGB clock (`PlaybackClockSync.h:1-40`).
- **Scope note.** The plan of record excludes this:
  `IMPLEMENTATION_PLAN.md:398` says "arbitrary compiled scene uploads
  are outside this release". It is a deliberate next step, not a gap.
- **Live delivery on wireless saturates at about 38.5/s**
  (`docs/benchmarks/baseline.md` §8.1). That is why uploaded playback
  matters.
- **Determinism.**
  - Layer evaluation is a pure function of `(doc, t)`
    (`EffectTypes.h:1-10`).
  - Phase terms are 1-periodic in `t·speed/scale` (`EffectTypes.cpp:20-28`).
  - `noise` is explicitly non-periodic (`:359-377`).
  - `ripple`, `screenfield` and `level` read live input (`:394-498`).

## Design

1. **Period analysis (core, pure).** Add
   `src/core/playback/LookPeriod.{h,cpp}` with `std::optional<double>
   LookCycleSeconds(const std::vector<EffectLayer>&, const
   SceneDocument&)`:
   - Per layer it computes the period:
     - wave and pulse: `scale/|speed|`
     - gradient and spin: `1/|speed|`
     - comet: `path_length/|speed|`
     - static, or `speed == 0`: none
     - noise with speed ≠ 0, ripple, screenfield, level: **not
       compilable**, with a reason
   - The cycle is the smallest `T ≤ 20 s` that every period divides
     within 0.5% (a rational LCM search).
   - If none exists, the result is `nullopt` with the reason "layer
     speeds do not share a short common cycle".
   - The global `EffectState.speed` and `intensity` are folded in.
2. **Compiler (engine).** Add `LookLoopCompiler` in `src/engine/`.
   - For each wireless group it evaluates `EffectEngine` at
     `t_k = k·T/N` over the prepared scene (`StudioScenePreparation`).
     It maps the group's emitters to LED order through the existing
     `DeviceMapper` path that live output uses, applies the scene
     brightness, and fills an RGB buffer.
   - `N` follows `BuildPlaybackLoop`'s rule (60 fps nominal, shrink
     until it fits 12 KiB).
   - Refactor `BuildPlaybackLoop` so the frame-count search and the
     compression are shared: a new `BuildLoopFromRenderer(cycle_ms,
     led_count, render(count))`.
   - The compiler runs off the scene-worker thread. Cost is a guess: up
     to about 1200 frames × 120 LEDs × the layer count. Measure it.
3. **Upload path.** Add a new vendor SDK packet
   `NET_PACKET_ID_LIANLI_WIRELESS_UPLOAD_LOOP = 4001`, with body
   `{controller_idx, revision, frame_count, interval_ticks,
   interval_fraction, compressed bytes}`.
   - The server handler validates the sizes and calls
     `SetPlaybackLoop` with a `PlaybackLoop` built from the payload.
     The backend re-validates by decompressing and checks
     `rgb.size == leds·3·frames` before upload.
   - A new mode `LLW_MODE_CUSTOM_LOOP` ("Compiled loop") appears in the
     mode table, so the active mode is honest in enumeration.
   - The engine `OpenRgbClient` gains `UploadLoop(...)`, probed like
     packet 4000. A stock backend drops it, and that latches
     "unsupported".
   - Acknowledgement reuses the existing revision and effect-ID
     convergence (`service.SetDesired(mac, upload, revision)`).
4. **Engine integration.**
   - `ClassifyScenePlayback` returns `animation_playback: true` with
     source `compiled` when `LookCycleSeconds` succeeds and the wireless
     backend supports packet 4001. Otherwise it gives the period
     analyser's reason.
   - `PlaybackController` handles a third kind beside presets:
     `PlaybackSource::CompiledLook`, with the cache key `(look hash,
     layout hash, brightness)`.
   - Wired devices evaluate the look live at `t = phase·T`, where the
     phase comes from `PlaybackClockSync`. This replaces
     `PlaybackFrameSampler` for compiled looks, which is exact because
     it uses the same evaluator.
   - Any edit to the look, the layout or the brightness recompiles and
     re-uploads, debounced by 500 ms.
5. **UI.**
   - The playback selector stops greying out compilable looks and shows
     "Compiled loop · T s · N frames".
   - Diagnostics shows the compiled frame count, the effective fps and
     the compressed size.
   - No new controls: the look's own controls (and R-12's) drive it.

**Rejected:**
- Uploading uncompressed frames and compressing in the backend. That
  costs bigger SDK packets and duplicates the work the engine already
  does with the shared codec.
- Quantising layer speeds silently to force a common cycle. It changes
  the look without asking. This could become an explicit option; see
  the open questions.
- Supporting `noise` by baking a long random loop. The seam would be
  visible, and it would not fit in 12 KiB at 60 fps.

## Steps

1. Add `LookPeriod` (pure). Test: a `look_period_test` (label `core`)
   with:
   - Packaged Wave (`wave.effect.json`): the layer periods are
     1.25/0.12 = 10.42 s and 0.9/0.15 = 6.0 s, with no common cycle
     under 20 s, so it is refused without snapping.
   - Noise refused and ripple refused.
   - A commensurate pair (2 s and 3 s) giving a 6 s cycle.
   - Static giving a 1-frame loop.
2. Add the `BuildLoopFromRenderer` refactor with the existing presets
   routed through it. Test: `playback_test` (`core` and `backend`)
   stays byte-identical for all nine presets.
3. Add an offline measurement tool, `tools/bench/loop_size`. It
   compiles all ten packaged looks for the 120-LED group and prints `T`,
   `N`, the effective fps and the compressed bytes. Record the results
   in `docs/benchmarks/` **before** committing to the UX. This decides
   whether 60 fps is reachable for real looks.
4. Vendor: packet 4001, the handler, `LLW_MODE_CUSTOM_LOOP`, and the
   payload validation. Test: `lianli_protocol_test` and
   `mode_table_test` (label `backend`) for bad sizes refused and a good
   payload reaching `SetDesired`.
5. SDK client: `UploadLoop` plus the supported-probe. Test:
   `sdk_client_test` against `fake_sdk_server.h` (label `sdk`) for the
   frame encoding and the unsupported latch.
6. Engine: `LookLoopCompiler`, the `PlaybackSource::CompiledLook`
   handling, the classifier change, and wired phase-locked evaluation.
   Tests: `playback_mode_test` (classification and revision flow) and
   `e2e_mode_qual_test` (a compiled-look upload hits the fake backend;
   editing the look re-uploads once after the debounce).
7. UI selector and Diagnostics fields. Test: `ui_playback_test` (a
   compiled look is selectable; the reason is shown when refused).
8. Hardware qualification:
   - Upload Wave to the SL group.
   - Check for a seamless loop.
   - Check wired and wireless sync against `PlaybackClockSync` health.
   - The physical fps stays **unknown** unless measured with a camera
     or photodiode (plan §3.2).

## Tests

- New: `look_period_test` (`core`).
- Extended: `playback_test`, `lianli_protocol_test`, `mode_table_test`,
  `sdk_client_test`, `playback_mode_test`, `e2e_mode_qual_test` and
  `ui_playback_test`.
- Manual: the step 8 checks. The fan firmware's tolerance for arbitrary
  (non-preset) frame content is untested. The existing static-frame and
  `SetFrames` paths suggest it works. That is a guess.

## Risks and open questions

- **Size versus fps.** A 10 s cycle at 60 fps is 600 frames × 360 B =
  216 KB raw and must compress about 18× to fit 12 KiB. The ratio for
  real looks is unknown until step 3. Long or busy looks will drop below
  60 fps automatically, and the UI must say so.
- **Firmware behaviour** with large frame counts (up to 4096) is not
  measured on hardware.
- **Exact periods rarely line up.** The packaged Wave look itself has
  no common cycle under 20 s (see step 1). Without speed snapping,
  most hand-made multi-layer looks will be refused.
- **Daniel to decide:**
  - The maximum cycle length (plan: 20 s).
  - Whether to offer "snap speeds to a loopable cycle" as an explicit,
    visible option. The plan recommends yes: snap each layer's period
    to `T/k` for the `T` that needs the smallest relative speed change
    (≤ 5%), and show the change. Without it the feature covers little.
  - Whether to amend `IMPLEMENTATION_PLAN.md:398`, which puts this out
    of scope, before starting.
- **Dependencies.** C-13's new primitives (`sparkle`, `breathe`, the
  band shapes) need period rules here. Design `sparkle` with a periodic
  seed (hash of `floor(t·rate) mod M`) so it stays compilable.
