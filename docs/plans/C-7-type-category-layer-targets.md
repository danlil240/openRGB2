# C-7: Type and category layer targets

Status: proposed · Size: S · Source: audit 2026-11-28 C-7 (the type/category part only)

## Goal

A layer can target "every keyboard" or "every SL wireless fan"
(`category:keyboard`, `type:<device-type-id>`) instead of a hard-coded
instance id. Looks stay correct when devices are added, renamed or
shared with another user's layout. This is the portable target form
that packaged and imported looks (R-12) should use.

## Scope

**In scope:** the `type:` and `category:` target forms only.

**Out of scope:** another agent is fixing these, so this plan must
compose with their change and not repeat it:
- instance-id prefix matching on a `/` boundary
- re-keying targets on `Rename` (`EditorController.cpp:609-724` per the
  audit)

## Current state

- **`LayerMatches`** (`src/core/effects/EffectTypes.cpp:203-218`)
  matches a target exactly against three things, and nothing else:
  - `obj.id`: the expanded path `<instance>/<entity>`
  - `obj.geometry`: the mesh tag, for example `keyboard_body`
  - `emitter.group`
- **`SceneObject` has no device-type or category field**
  (`src/core/scene/SceneTypes.h:78-104`).
  - The resolver knows both while expanding:
    `ExpandType(c, inst_path, p, …)` holds the `DevicePreset& p`
    (`src/core/scene/SceneResolver.cpp:71-140`), and the top level reads
    `DeviceInstance::type` (`:290-330`).
  - `DevicePreset::category` exists (`src/core/presets/DevicePreset.h:136`).
    Packaged values: case, cooler, fan, furniture, gpu, group, keyboard,
    mouse and ram.
- **Packaged looks** hard-code instance or geometry targets:
  `reactor.effect.json:26` (`"fan_body", "mouse/strip"`) and
  `wave.effect.json` (`"keyboard_body"`).
- **The target grammar** is any non-empty string, with at most 64
  targets (`EffectJson.cpp:565-593`; schema
  `effect.schema.json:164-168`).
  - Instance ids and type ids are `[A-Za-z0-9_-]`
    (`studio.schema.json:26`, `DevicePreset.cpp:584`), so a `type:` or
    `category:` prefix cannot collide with a real id.
- **The inspector** edits targets as free comma-separated text
  (`ui/studio/EffectInspector.qml:155-166`). There is no picker.

## Design

- **Runtime-only fields.** Add `std::string type_id;` and
  `std::string category;` to `SceneObject`.
  - `ExpandType` stamps both from the `DevicePreset` that produced the
    entity: the innermost type. A fan inside a case assembly therefore
    gets `category:fan`.
  - They are **not** serialized. `SceneJson.cpp` writes an explicit
    field list (`:145-154`), so nothing changes there. They stay empty
    for legacy v2 scene documents, which have no type information.
- **Matching.** In `LayerMatches`, before the existing comparisons, a
  target starting with `type:` compares the rest with `obj.type_id`,
  and one starting with `category:` compares the rest with
  `obj.category`. Both comparisons are exact and case-sensitive, like
  ids.
  - The other agent's prefix rule then applies unchanged to bare
    targets.
  - Rename re-keying must **skip** prefixed targets. Note this for that
    agent: a `type:` target never names an instance.
- **Validation.** `TargetsField` refuses `type:` or `category:` with an
  empty or non-`[A-Za-z0-9_-]` remainder, with the error path
  `layers[i].targets[j]`. An unknown but well-formed type is allowed: it
  matches nothing, just as an unknown id does today.
- **The schema** gets a description update and an optional `pattern`
  (`^(type:|category:)?[A-Za-z0-9_/-]+$`).
- **UI.** The inspector gains an "Add target ▾" menu next to the text
  field. It lists `category:<c>` and `type:<id>` for the device types
  present in the current workspace, from the `objectCtl` scene and the
  preset catalog, plus the selected object's id. Choosing an entry
  appends to the list through the existing `setLayerTargets`.
- **Packaged looks.** Add `category:mouse` beside `mouse/strip` in
  `reactor`, leaving the existing target in place. Then the reactor
  works for any mouse instance name. Daniel decides whether to change
  more looks.

**Rejected:**
- Matching a type id or category as a *bare* string. It is ambiguous
  with geometry tags (`fan_body`) and with future instance ids.
- Resolving categories in the engine's `StudioScenePreparation` instead
  of the core resolver. The preview and the UI evaluate through the
  same core, so the core resolver must carry it.

## Steps

1. Add `SceneObject::type_id` and `category`, stamped in `ExpandType`.
   Test: a `studio_scene_test` (label `core`) case where a resolved
   default desk has a keyboard object with `category == "keyboard"` and
   a nested fan with the child type's category; `SceneJson` output is
   unchanged (a fixture comparison).
2. Add the `LayerMatches` prefix forms. Test: `studio_scene_test`: a
   `category:keyboard` layer colours only keyboard emitters; a
   `type:<sl-fan-id>` layer colours all three fans; an unknown
   `type:nope` colours nothing.
3. Add `TargetsField` validation and the schema. Test:
   `effect_json_test`: `type:` and `category:bad space` are refused,
   `category:fan` is accepted, and it round-trips.
4. Add the inspector "Add target" menu. Test: a
   `ui_studio_effects_test` case where a menu entry appends
   `category:fan` through `setLayerTargets`.
5. Add `category:mouse` to `reactor.effect.json`. Test: the
   registry/bundle load tests; `e2e_studio_workspace_test` still
   passes.
6. Docs: the `EffectLayer` comment (`SceneTypes.h:197-198`) and the
   schema description.

## Tests

- **`core`:** `studio_scene_test` and `effect_json_test`.
- **`ui`:** `ui_studio_effects_test`.
- **Manual:** apply Reactor on Daniel's layout, rename the mouse
  instance, and check that the mouse strip still lights.

## Risks and open questions

- **Merge order.** Coordinate with the agent doing prefix matching and
  re-keying: both edit `LayerMatches`. Whichever lands second rebases.
  The prefixed forms must come first in the function.
- **Category values are free text** in device presets. A user-authored
  type with `category: "Keyboard"` would not match `category:keyboard`.
  Daniel decides whether to lowercase categories at match time. The
  plan says yes, for categories only.
- **Ancestors.** Should a `type:` target also match objects nested under
  an assembly of that type (for example `type:case-assembly` lighting
  its child fans)? The plan default is innermost only. Ancestor matching
  can come later as a separate `under:<type>` form.
