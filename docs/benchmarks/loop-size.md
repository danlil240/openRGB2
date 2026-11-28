# R-11 loop size: can packaged looks compile into 60 fps uploaded loops?

Status: **offline measurement, no hardware.** Date: 2026-11-28.
Branch `feat/r-11-look-period-measurement` (not merged). Produced by
`tools/bench/loop_size` for plan
[`docs/plans/R-11-compile-looks-to-uploaded-loops.md`](../plans/R-11-compile-looks-to-uploaded-loops.md)
step 3. This page is the data for Daniel's decisions on R-11 steps 4–8.
It does not decide them.

## Answer in brief

- **3 of the 10 packaged looks compile: `reactor`, `comet` and `portal`.**
  The other 7 are refused, and snapping cannot rescue any of them.
  - 6 are refused for a structural reason: 3 use `noise` and 3 read
    live input.
  - The 7th is `wave`. It is refused because its two layer periods,
    10.42 s and 6.0 s, share no cycle under 20 s.
- **All 3 compilable looks reach 60 fps at their default speed (seed 0).**
  Across 100 remix seeds, `comet` reaches 60 fps every time, `reactor`
  95 times and `portal` 88 times. The remaining seeds fall back to
  45–58 fps.
- **Snapping at the plan's 5% / 20 s changes nothing for the packaged
  looks.**
  - The three compilable looks already loop exactly.
  - `wave` needs a **7.06%** speed change to loop under 20 s (T =
    11.2 s). Even then, the 12 KiB limit cuts it to **22 fps**.
- **The binding limit is size, not period.** Each look compresses to a
  roughly constant number of bytes per frame: 21 for `comet`, 83 for
  `reactor`, 201 for `portal` and about 46 for snapped `wave`. 12,288 B
  at 60 fps therefore caps the cycle at about 10 s, 2.5 s, 1 s and
  4.5 s respectively. Longer cycles keep their duration and lose fps,
  roughly as fps ≈ 12288 / (B/frame × T) (derived).
- **Compile cost is small.** It took 3–60 ms for a loop that fits on
  the first pass. The worst case was 343 ms for a 42 s Wave that took
  several shrink passes. (Release build on this PC, single thread,
  EffectEngine plus tinyuz.)

## Method

- **Looks.** Each packaged look is resolved with `BuildPreset(id, seed)`,
  with seed 0 by default: the scene default `EffectState.seed`. Global
  speed and intensity are 1.0.
- **Scene.**
  - The scene is `BuildDefaultDesk()` reduced to the wireless group's
    three Device objects (`slw_fan_0..2`, 40 emitters each). Parents
    and decor stay, so the world transforms are correct.
  - Only layers that reach these emitters count. For example, the
    `reactor` layers aimed at the pump, RAM and keyboard are ignored.
- **LED order.**
  - Fan *k*'s emitter with address *a* is LED `40k + a`.
  - That is the scene's zone order. The engine's `DeviceMapper`
    mapping was not used.
  - Scene brightness is 1.0 and is not applied.
- **Period.**
  - `AnalyzeLookPeriod` from `src/core/playback/LookPeriod.h` (step 1)
    computes the period.
  - Its rules are in the next section.
- **Loop.**
  - `T` comes from the period analysis.
  - The frames are `EffectEngine::Evaluate` at `t_k = k·T/N` on the
    runtime stack. Each periodic layer's speed is scaled so its period
    is exactly `T/k`. That change is ≤ 0.5% without snapping.
  - The frames go through `BuildLoopFromRenderer` (step 2). It is the
    same builder the nine backend presets now use. It starts at
    `N = round(60·T)`, quantizes the interval to 0.00625 ms units, and
    lowers `N` until the tinyuz payload is ≤ 12,288 B. `T` stays fixed.
- **Seam.** The seam delta is the largest channel difference between
  the frames at `t = T` and `t = 0`. It is 0 for every compiled look.
- **Limits.** 12,288 B compressed (`MAX_COMPRESSED_BYTES`) and 4,096
  frames (`MAX_FRAME_COUNT`). No look comes near the frame limit: 60 fps
  for 20 s is 1,200 frames.
- **fps.** All fps figures are the *nominal* upload rate
  (`160000 / interval_units`). The firmware's actual presentation rate
  is **unknown** until it is measured with a camera or photodiode
  (plan §3.2).

### Period rules (step 1)

- **Per-layer period**, with global speed folded in:
  - wave and pulse: `scale/|speed|`
  - gradient and spin: `1/|speed|`
  - comet: `path length/|speed|`
- **Layers that do not move:** static, speed 0, opacity 0, a disabled
  layer, or a layer that reaches no emitter.
- **Refused layers:**
  - `noise` with speed ≠ 0, because it never repeats.
  - `ripple`, `screenfield` and `level`, because they read live input.
- **Cycle.** The cycle is the shortest `T ≤ 20 s` that every period
  divides within 0.5%. The search is exact: it scans every interval
  between the points where one layer's best repeat count `k` changes.
- **Snapping (opt-in, `SnapOptions()`).**
  - It is the plan's recommended rule. Each period becomes `T/k`, using
    the `T ≤ 20 s` that needs the smallest worst-case relative speed
    change, with a cap of 5%.
  - Ties: cycles whose change is within 0.5 points of the minimum count
    as equal, and the shortest of them wins.
  - Snapping is reported per layer. Nothing in the UI uses it.

## Packaged looks, seed 0, 120-LED group

| Look | Layers reaching the group | No snap: T s | N | fps | bytes | Snap (5% / 20 s): T s | N | fps | bytes | B/frame | Seam Δ | Build ms |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| reactor | static, spin 2.020 s | 2.020 | 121 | 59.9 | 10,052 | 2.020 | 121 | 59.9 | 10,052 | 83 | 0 | 12 |
| comet | static, comet 4.021 s | 4.021 | 241 | 59.9 | 5,039 | 4.021 | 241 | 59.9 | 5,039 | 21 | 0 | 33 |
| portal | static, pulse 1.005 s | 1.005 | 60 | 59.7 | 12,033 | 1.005 | 60 | 59.7 | 12,033 | 201 | 0 | 7 |
| wave | wave 10.417 s, wave 6.000 s | refused | – | – | – | refused (needs 7.06%) | – | – | – | – | – | – |
| aurora | static, wave 2.468 s, **noise** | refused | – | – | – | refused | – | – | – | – | – | – |
| chrome | gradient 43.153 s, **noise** | refused | – | – | – | refused | – | – | – | – | – | – |
| embers | **noise**, **noise** | refused | – | – | – | refused | – | – | – | – | – | – |
| shockwave | static, **ripple**, **level** | refused | – | – | – | refused | – | – | – | – | – | – |
| keyripple | static, **ripple** | refused | – | – | – | refused | – | – | – | – | – | – |
| ambient | static, **screenfield** | refused | – | – | – | refused | – | – | – | – | – | – |

Bold marks the layer that causes the refusal.

- **Reach 60 fps:** `reactor`, `comet` and `portal`. `portal` is at
  12,033 of 12,288 B with only 1 s of content.
- **Fall back to lower fps:** none at seed 0. With other remix seeds,
  `reactor` and `portal` sometimes drop below 60 fps; see the seed
  sweep. `wave` would run at 22 fps if snapping up to 10% were allowed.
- **Refused:**
  - `wave`: no common cycle.
  - `aurora`, `chrome` and `embers`: `noise`. Without its noise layer,
    `aurora` would be a single 2.47 s wave. `chrome`'s gradient alone
    is 43 s, which is over the limit anyway.
  - `shockwave`, `keyripple` and `ambient`: live input.
- **What snapping changes:** nothing at 5% / 20 s.

## Sensitivity: what Wave would need

`wave` is the only packaged look whose layers are all periodic and that
is still refused. This table shows the effect of the max cycle (the
first decision) and the snapping cap (the second).

| Max cycle s | Max change | T s | Change | N | fps | bytes | Build ms |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 20 | 5% | refused (best 7.06% at 11.21 s) | – | – | – | – | – |
| 20 | 10% | 11.208 | 7.06% | 250 | 22.3 | 11,465 | 124 |
| 30 | 5% | 30.000 | 4.17% | 301 | 10.0 | 12,273 | 233 |
| 30 | 10% | 30.000 | 4.17% | 301 | 10.0 | 12,273 | 234 |
| 45 | 5% or 10% | 41.833 | 0.40% (no snap needed) | 244 | 5.8 | 11,923 | 337 |
| 60 | 5% or 10% | 41.833 | 0.40% (no snap needed) | 244 | 5.8 | 11,923 | 338 |

- At 30 s the cycle is clamped to 30 s, and the 3 : 5 repeat ratio
  needs 4.17%.
- At 45 s or more, Wave loops at 41.8 s (4 × 10.42 ≈ 7 × 6.0) with no
  snapping, but only at 5.8 fps.
- At no setting does Wave come near 60 fps. Its content compresses to
  about 46 B/frame, so 60 fps needs a cycle ≤ about 4.5 s (derived).

## Cycle length vs fps (the compilable looks, slowed down)

`EffectState.speed` scales every layer, so `T` scales by `1/speed` and
the content per cycle stays the same.

| Look | Global speed | T s | N | fps | bytes | B/frame |
|---|---:|---:|---:|---:|---:|---:|
| reactor | 2 | 1.010 | 61 | 60.4 | 5,393 | 88 |
| reactor | 1 | 2.020 | 121 | 59.9 | 10,052 | 83 |
| reactor | 0.5 | 4.040 | 131 | 32.4 | 10,841 | 83 |
| reactor | 0.25 | 8.080 | 128 | 15.8 | 11,049 | 86 |
| reactor | 0.125 | 16.160 | 128 | 7.9 | 11,049 | 86 |
| comet | 2 | 2.010 | 121 | 60.2 | 2,793 | 23 |
| comet | 1 | 4.021 | 241 | 59.9 | 5,039 | 21 |
| comet | 0.5 | 8.041 | 482 | 59.9 | 9,675 | 20 |
| comet | 0.25 | 16.083 | 608 | 37.8 | 12,233 | 20 |
| comet | 0.125 | refused: 32.17 s cycle > 20 s | – | – | – | – |
| portal | 2 | 0.503 | 30 | 59.7 | 6,169 | 206 |
| portal | 1 | 1.005 | 60 | 59.7 | 12,033 | 201 |
| portal | 0.5 | 2.010 | 60 | 29.8 | 12,033 | 201 |
| portal | 0.25 | 4.021 | 61 | 15.2 | 12,218 | 200 |
| portal | 0.125 | 8.041 | 57 | 7.1 | 11,381 | 200 |

- Bytes per frame barely change with `T`, so tinyuz finds little
  redundancy across frames for these looks.
- Once the size limit binds, `N` settles at a fixed count (about 128
  for `reactor` and about 60 for `portal`), and the fps falls as 1/T.
- For `comet`, a compilable look at a slower user speed is the first to
  hit the 20 s cap.

## Baseline: today's nine backend presets on the same group

For comparison, these are the existing uploaded presets
(`BuildPlaybackLoop`, SL V3 tables, 40 LEDs/fan, white) through the same
builder. These loops are uploaded today.

| Preset | Speed 100 (2.4 s): N | fps | bytes | Speed 25 (9.6 s): N | fps | bytes | Speed 10 (24 s): N | fps | bytes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| SpectrumCycle | 144 | 60.0 | 495 | 576 | 60.0 | 1,278 | 1,440 | 60.0 | 1,408 |
| RainbowWave | 144 | 60.0 | 6,214 | 347 | 36.1 | 12,152 | 351 | 14.6 | 12,284 |
| Breathing | 144 | 60.0 | 492 | 576 | 60.0 | 1,747 | 1,440 | 60.0 | 1,967 |
| PerimeterWave | 144 | 60.0 | 7,917 | 184 | 19.2 | 12,182 | 171 | 7.1 | 10,567 |
| PerimeterComet | 144 | 60.0 | 3,466 | 234 | 24.4 | 11,755 | 247 | 10.3 | 11,837 |
| PerimeterChase | 144 | 60.0 | 899 | 576 | 60.0 | 949 | 1,440 | 60.0 | 970 |
| PerimeterPingPong | 144 | 60.0 | 2,478 | 576 | 60.0 | 11,764 | 313 | 13.0 | 11,492 |
| StackWave | 144 | 60.0 | 8,901 | 189 | 19.7 | 11,647 | 191 | 8.0 | 11,767 |
| StackRainbow | 144 | 60.0 | 7,589 | 224 | 23.3 | 10,810 | 224 | 9.3 | 10,810 |

- The fps fallback is not new: 6 of the 9 shipped presets already drop
  to 7–15 fps at speed 10.
- The packaged looks that compile fall in the same range as the busy
  presets. Per frame, `comet` (21 B) is cheaper than RainbowWave
  (43 B). `portal` (201 B) is the most expensive content measured.

## Remix seed sweep (seeds 0–99)

| Look | Compilable, no snap | Compilable, snap | T s min / median / max | fps min / median / max | bytes min / median / max | At 60 fps |
|---|---:|---:|---|---|---|---:|
| reactor | 100/100 | 100/100 | 1.25 / 1.64 / 2.47 | 53.0 / 59.9 / 60.3 | 3,968 / 7,345 / 12,217 | 95/100 |
| comet | 100/100 | 100/100 | 3.25 / 4.03 / 5.79 | 59.9 / 60.0 / 60.1 | 3,908 / 5,880 / 9,219 | 100/100 |
| portal | 100/100 | 100/100 | 0.49 / 0.80 / 1.47 | 45.0 / 59.9 / 60.8 | 5,611 / 9,236 / 12,210 | 88/100 |
| aurora, chrome, embers, shockwave, keyripple, ambient, wave | 0/100 | 0/100 | – | – | – | – |

"At 60 fps" means a nominal rate of at least 59 fps. A rate a little
above 60 comes from rounding `N = round(60·T)` for short cycles.

## What this means for the open decisions

These recommendations are derived from the tables above. They are not
measured.

1. **Max cycle length.** Keep 20 s, or lower it.
   - Going longer buys only low frame rates: Wave reaches 5.8 fps at
     41.8 s. Any loop under about 38.5 fps runs slower than live
     delivery's measured ceiling (baseline §8.1). It still saves the RF
     traffic, but it no longer buys smoothness.
   - What decides the fps is bytes per frame times `T`, not the cap.
     The UI should state the expected fps before upload.
2. **Snapping.** It earns little on the packaged looks.
   - At 5% / 20 s it rescues none of them.
   - The only candidate, Wave, needs 7.06%, and the result is still a
     22 fps loop.
   - Snapping can still matter for hand-made multi-layer looks: two
     layers with nearby speeds is the case it handles. It could stay
     an explicit option, but it does not make the feature cover more of
     what ships.
   - Retuning Wave's two speeds so their periods share a cycle under
     20 s would make it loop exactly. Examples are 12.0 s and 6.0 s, or
     10.42 s and 5.21 s. That is a content change to one file. The size
     limit would still hold it to about 20–26 fps (derived from about
     46 B/frame at a 10–12 s cycle).
3. **Amending `IMPLEMENTATION_PLAN.md:398`.** That line puts compiled
   uploads out of scope.
   - On this data the feature covers 3 of 10 shipped looks at 60 fps:
     `reactor`, `comet` and `portal`.
   - With a retuned `wave` it covers 4. Wave would run at about
     20–26 fps unless its content is also simplified.
   - Whether that is worth steps 4–8 is Daniel's call. That work is
     vendor packet 4001, the SDK, the compiler, the UI and a hardware
     session.

## Caveats

- **Placement.** The desk placement is the default guess
  (`DefaultDesk.cpp`). Other placements change the content and so the
  bytes per frame, but not the periods.
- **LED order.** The order is the scene's zone order. The firmware's
  physical order (`SLV3_LED_ORDER`) is not applied. That changes which
  LED gets which color, which may change the compression ratio slightly.
- **Fan firmware.** Its behaviour with non-preset frame content and
  large frame counts is untested on hardware. That is plan step 8.
- **Speed nudge.** Without snapping, each compiled layer's speed was
  nudged by at most 0.5% to close the seam exactly. For the three
  compiled looks the nudge was 0: one periodic layer each.

## Reproduce

```bash
CM="/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
"$CM" --build build --config Release --target loop_size -- -m:4
build/tools/bench/Release/loop_size.exe            # Markdown to stdout, ~20 s
build/tools/bench/Release/loop_size.exe --seeds 0  # skip the seed sweep
```

The output is deterministic except for the build-time columns.
`DLS_EFFECT_DIR` overrides the looks directory. By default the tool uses
this tree's `src/core/presets/effects`.
