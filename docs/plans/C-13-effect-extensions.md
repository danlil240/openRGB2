# C-13: Cheap effect extensions

Status: proposed · Size: M (five S items, each landable alone) · Source: audit 2026-11-28 C-13

## Goal

More looks can be built from the existing layer model without code:
- multiply and lighten blends
- a palette that does not wrap
- `sparkle` and `breathe` primitives
- saw and triangle band shapes
- optional gamma-aware (linear-light) blending

Every item is additive. Existing looks, fixtures and workspaces render
byte-identically.

## Current state (shared by all sections)

- **Evaluation and compositing** live in `src/core/effects/EffectTypes.cpp`:
  - primitives `EvalWave` `:228-239`, `EvalPulse` `:241-251`,
    `EvalLevel` `:486-498`
  - dispatch `EvalPrimitive` `:500-516`
  - compositing `BlendOver` `:521-543`
  - `Palette::Sample` `:77-110`
  - colour conversion `ToColorF` and `ToSceneColor` `:33-49`

  `EffectEngine::Evaluate` composites layers in order over the painted
  base colour (`EffectEngine.cpp:86-99`).
- **Types.** `BlendMode {Replace, Add, Screen}`
  (`src/core/scene/SceneTypes.h:145-150`) and `EffectLayer`
  (`:206-224`).
- **JSON.**
  - Blend parsing is at `EffectJson.cpp:736-738`.
  - `IsPrimitive` holds the whitelist (`:876-891`).
  - `EffectLayerToJson` writes **every** field (`:896-940`). This is the
    canonical shape the fixture oracle also writes, so new fields must
    serialize only when non-default.
  - Unknown layer keys are rejected (`EffectJson.h:70-77`).
- **Schema.** `src/core/schemas/effect.schema.json:106-107` (the
  primitive enum) and `:116` (the blend enum).
- **UI lists.**
  - `ui/studio/LayerStack.qml:171-173` (add-layer primitives)
  - `ui/studio/EffectInspector.qml:84,90` (the blend combo)
  - `src/ui/studio/EffectEditorController.cpp:28-33` (`BlendName`) and
    `:1025-1045` (`setLayerBlend` string map)
  - `EditorController::AddLayer` validates through `IsPrimitive`
    (`src/core/editor/EditorController.cpp:1871-1882`), so it needs no
    change.
- **Tests.** `tests/core/effect_json_test.cpp`, `studio_scene_test.cpp`
  and the `tests/core/fixtures/effects` oracle (label `core`).
- **Rollback hazard.** An older engine rejects unknown layer keys and
  enum values, so a workspace that *uses* a new feature will not load
  after `rollback-openrgb2.ps1`. Unused features keep documents
  unchanged.

## 1. Multiply and lighten blends

- **Design.** Add `BlendMode::Multiply` and `BlendMode::Lighten` in
  `BlendOver`:
  - Multiply: `dst *= lerp(1, src, a)`
  - Lighten: `dst = lerp(dst, max(dst, src), a)`, per channel

  Add the strings `"multiply"` and `"lighten"` in the parse, the
  serialize, the schema enum, `BlendName`, `setLayerBlend` and the
  inspector combo.
- **Steps.**
  1. Core and JSON. Test: `effect_json_test` (round-trip, unknown blend
     still refused) and a `studio_scene_test` blend table (a white
     multiply is the identity; black multiply gives black at a=1; a
     lighten never darkens).
  2. UI strings. Test: a `ui_studio_effects_test` case where
     `setLayerBlend("multiply")` lands.

## 2. Non-wrapping palette mode

- **Today.** `Sample` applies `Frac(u)`, so `Sample(1.0)` returns stop 0
  (`:88`). `EvalLevel` samples `Sample(lvl)` with `lvl` in [0,1]
  (`:494-495`), so a full-scale audio level flips back to the *first*
  colour. That is arguably a bug in that primitive today.
- **Design.** Add `Palette::wrap` (default true) and a layer key
  `"palette_mode": "wrap"|"clamp"`, written only when it is "clamp". A
  clamped `Sample` holds the first and last stop colour outside
  `[first.pos, last.pos]` and never interpolates last to first.
- **Steps.**
  1. Core, JSON and schema. Test: `effect_json_test` and a
     `Palette::Sample` unit table (wrap is unchanged; with clamp,
     `Sample(1.0)` gives the last stop).
  2. Add an inspector toggle, shown for every primitive. Test:
     `ui_studio_effects_test`.
- **Daniel decides:** whether `level` (and `comet`, `ripple`, whose `u`
  is also in [0,1)) should *default* to clamp. That is a visible change
  to packaged looks such as `ambient`. The plan default: no default
  change; fix `level` explicitly in its packaged looks.

## 3. `sparkle` and `breathe` primitives

- **sparkle.** Per emitter: `h = HashU32(seed ^ emitter-key ^
  floor(t·speed + phase_e))`, where `phase_e` is a hashed per-emitter
  offset. The emitter is lit when `(h & 0xFFFF)/65535 < density`.
  - The envelope is `exp(-4·frac(t·speed + phase_e)/scale)`.
  - Colour comes from `palette.Sample(hash01)`.
  - For R-11 compilability, take `floor(...) mod M` with `M` from the
    layer (default 64), so the pattern repeats every `M/speed` seconds.
- **breathe.** Coverage `a = opacity · (0.5 − 0.5·cos(2π(t·speed +
  phase)))^density`. The colour is `palette.Sample(floor(t·speed)/n)`
  for `n` stops: one colour per breath, so a single-stop palette is
  plain breathing. Its period is `n/speed`.
- **Plumbing.** `IsPrimitive`, the schema enum, the LayerStack list, the
  EffectLayer doc comment (`SceneTypes.h:178-205`), and an inspector
  field-hint map (what speed, scale and density mean).
- **Steps.**
  1. `breathe`. Tests: `effect_json_test` and a `studio_scene_test`
     check that coverage is 0 at phase 0 and peaks at half a period.
  2. `sparkle`. Tests: determinism (the same `t` and seed give the same
     frame) and periodicity (`t` and `t + M/speed` match).
  3. Add one packaged look per primitive under
     `src/core/presets/effects/`, for example `twinkle.effect.json` and
     `breathe.effect.json`. Test: the bundle/registry load test.

## 4. Saw and triangle band shapes

- **Design.** `EvalWave` and `EvalPulse` both use `band = 0.5 −
  0.5·cos(2πu)` (`:235`, `:247`). Add the layer key `"shape":
  "sine"|"saw"|"triangle"`, written only when it is not sine:
  - saw: `frac(u)`
  - triangle: `1 − |2·frac(u) − 1|`

  It is an enum field on `EffectLayer`. The density exponent still
  applies.
- **Steps.**
  1. Core, JSON and schema. Test: a `studio_scene_test` table of `band`
     at u = 0, 0.25, 0.5 and 0.75 for each shape; `effect_json_test`
     confirms that sine is absent in the output.
  2. Add an inspector combo, visible for wave and pulse only. Test:
     `ui_studio_effects_test`.

## 5. Gamma-aware blending

- **Today.** Colours are blended as sRGB-encoded floats, straight from
  `ToColorF` (`:33-41`). Opacity fades and gradients therefore dip in
  the middle on LEDs.
- **Design.** Add a look-level switch `EffectState.linear_blend`
  (workspace `effects.linear_blend`, written only when true) and
  `EffectEngine::linear_blend`. When it is set, `Evaluate` converts the
  painted base and every primitive colour with an sRGB→linear lookup
  table before `BlendOver`, then converts back with the inverse before
  `ToSceneColor`. Coverage (alpha) is not converted. When it is off,
  nothing changes.
- **Steps.**
  1. Core: the LUT and the engine flag. Test: `studio_scene_test`: a
     50% replace of black over white gives 188 (0xBC) when linear and
     128 (0x80) today; with the flag off, the existing fixtures are
     byte-identical.
  2. Persistence through `StudioConfig` effects parsing and
     serialization. Test: `studio_config_test` (absent stays absent, and
     it round-trips).
  3. Add a Lighting-page toggle under the look controls. Test:
     `ui_studio_effects_test`.
- **Daniel decides:**
  - Per look (in `.effect.json`), per workspace (as planned), or
    global.
  - Whether the default should flip for *new* looks later.

## Tests (summary)

- **`core`:** `effect_json_test`, `studio_scene_test`,
  `studio_config_test` and the fixture oracle (it must stay
  byte-identical).
- **`ui`:** `ui_studio_effects_test`.
- **Manual:** each new feature on the real G512 and fans, to check that
  the linear-blend midpoint looks even to the eye. That is a perceptual
  judgement, not automatable.

## Risks and open questions

- **Rollback compatibility** (see Current state). Write new fields only
  when they are used, and note the limitation in
  `packaging/windows/RELEASE.md`.
- **R-11 interaction.** Every new primitive needs a period rule in
  `LookPeriod`: breathe has `n/speed`, sparkle has `M/speed`, and the
  shapes do not change the period.
- **Preview cost.** The preview uses the same core, so no separate
  renderer change is needed. The sparkle hash per emitter per frame is
  cheap, but the preview at 60 fps over every emitter is unmeasured
  (a guess: negligible next to `PathArc`).
