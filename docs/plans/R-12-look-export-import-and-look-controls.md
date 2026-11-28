# R-12: Single-look export/import, and simple controls for every look

Status: proposed · Size: M · Source: audit 2026-11-28 R-12

## Goal

- A user can save one look to a `.effect.json` file, share it, and
  import someone else's look into their library, without exporting a
  whole workspace bundle.
- The simple control card that Wave has today (direction, colours,
  speed, length) extends to the other primitive families (comet, pulse,
  gradient/spin, noise), so most looks can be tuned without opening the
  layer editor.

## Current state

- **Personal looks.**
  - Personal looks are installed with `saveLookAs`, which sends a
    `resource_install` job carrying an `EffectDocument`
    (`src/ui/studio/EffectEditorController.cpp:533-640`).
  - The engine side is collision-safe ("never overwrites") and gated on
    both revisions (`src/engine/WorkspaceService.cpp:1941-1975`).
  - `resource_get {kind:"effect", id}` returns the stored document
    (`src/ipc/WorkspaceCommands.cpp:627-644`).
- **Bundles carry looks only inline.** The workspace bundle path
  "materializes personal-look layers inline … look files never
  transported" (`docs/studio-import.md:71`). There is **no** way to move
  one look as a file.
- **Local file helpers already exist.**
  `WorkspaceController::readLocalFile` and `writeLocalFile`
  (base64, bounded, never a silent truncation;
  `src/ui/WorkspaceController.h:236-240`).
- **The look shelf.** `ui/studio/LookShelf.qml` lists looks with Apply,
  Save-as, Remix and Reset. It has no Export or Import.
- **The WaveControls pattern.**
  - `ui/studio/WaveControls.qml` is visible when `flowState().available`
    is true. It calls `effectEdit.setFlowDirection`, `setFlowColors`,
    `setFlowSpeed` and `setFlowWavelength`
    (`EffectEditorController.h:195-204`).
  - Those call core `EditorController::SetFlow(FlowEdit)`
    (`src/core/editor/EditorController.h:340-362`). It is one undo
    record. It drives every enabled `wave` layer together, rewrites the
    first layer's palette, and rescales the speed and length of every
    wave layer by the same ratio.
  - No other primitive has such a card.
- **The document format.** `EffectDocument` carries only id, name,
  description, needs and layers (`src/core/effects/EffectJson.h:59-68`).
  Unknown top-level keys are accepted on parse but dropped on
  re-serialization (`EffectJson.cpp:1105-1125`, `:1017-1019`).

## Design

**Part A — export/import (S).**
- **Export a library look.** A LookShelf row action, "Export…", sends
  `resource_get {kind:"effect", id}`, opens a `FileDialog` (save) with
  the default `<id>.effect.json`, and calls
  `WorkspaceController::writeLocalFile`. The bytes are the canonical
  `EffectDocumentToJson`.
- **Export the current look.** An "Export current look…" action builds
  the same `EffectDocument` that `saveLookAs` sends (refactor that
  builder into `BuildLookDocument(id, name)`) and writes it, with no
  install.
- **Import.** "Import look…" does:
  1. `readLocalFile`, capped at 256 KB.
  2. A UI-side `EffectDocumentFromJson`, so errors appear before any
     job.
  3. `resource_install {effect}`.
  4. The landing id comes back (collision-safe, so it may differ from
     the file's id). The status says "Imported as '<landing id>'" and
     offers **Apply**.

  A look with `needs` set shows "needs audio/key/screen input" before
  it is applied.
- **No new file format:** it is the packaged `*.effect.json` schema
  (`src/core/schemas/effect.schema.json`).

**Part B — look controls (M).**
- **Core.** Generalise `SetFlow` into `SetFamily(primitive,
  FamilyEdit)`:
  - `FamilyEdit` holds optional `direction`, `origin`, `colors`,
    `speed`, `scale` and `density`.
  - It applies to every enabled layer of that primitive, with the same
    rules as today: the colours go on the first layer, and speed and
    scale use a ratio-rescale on all layers.
  - It refuses fields the primitive does not use: direction for spin,
    for example.
  - `SetFlow` becomes `SetFamily("wave", …)`, and its behaviour does not
    change.
- **Adapter.** `EffectEditorController::familyState(primitive)` returns
  `{available, fields:[…], values}`. `setFamily(primitive, field,
  value)` uses one undo record per call, like Flow.
- **QML.** `LookControls.qml` renders one section per family present in
  the effective stack, in stack order:
  - wave: direction, colours, speed, length (today's WaveControls,
    moved in unchanged)
  - comet: speed, tail length, colours
  - pulse: origin, ring spacing, speed, colours
  - gradient and spin: speed, scale, colours
  - noise: scale, drift speed, density

  Reactive primitives get no section: their controls are the input
  sensitivity, which already exists on the Lighting page.
- **Rejected:** a declarative `controls` block in the look JSON. It is
  lost as soon as the user edits the stack inline: the workspace stores
  only `effect.layers`, and `EffectDocument` drops unknown keys. It is
  also more schema surface for the same effect.

## Steps

1. Factor out `BuildLookDocument` from `saveLookAs`, with no behaviour
   change. Test: `ui_studio_effects_test`, where `saveLookAs` still
   sends an identical `args["effect"]`.
2. Add "Export…" on the LookShelf rows and "Export current look…".
   Test: a `ui_studio_effects_test` case that writes to a temp path and
   re-parses it with `EffectDocumentFromJson`.
3. Add "Import look…": read, validate, install, landing id, Apply.
   Tests:
   - `ui_studio_effects_test`: a bad JSON fails with no job sent; a good
     file sends `resource_install`.
   - `ipc_workspace_test`: importing an existing id lands under a new
     id; check this case exists, and add it if not.
4. Core `SetFamily`, with `SetFlow` delegating to it. Test:
   `studio_editor_test` (label `core`):
   - The existing Flow cases are unchanged.
   - comet speed rescale across two comet layers.
   - A refused field per primitive.
   - One undo record per call.
5. Adapter `familyState` and `setFamily`. Test: `ui_studio_effects_test`
   on the field lists per primitive.
6. `LookControls.qml` replaces the Wave card mount on the Lighting
   page. WaveControls becomes its wave section. Test: a new
   `tests/ui/studio_qml/tst_lookcontrols.qml` (label `ui`): a stack with
   wave and comet layers renders two sections; a reactive-only stack
   renders none.
7. Update `docs/studio-import.md` (the parity row) and the LookShelf
   help text.

## Tests

- Extended: `ui_studio_effects_test`, `ipc_workspace_test` and
  `studio_editor_test`.
- New QML test: `tst_lookcontrols.qml`.
- Manual: export a personal look, delete it, import the file, and check
  that it renders identically (also with `reactor`, which targets
  `mouse/strip`; see C-7).

## Risks and open questions

- **Q-3 overlap.** Q-3 (the Wave direction sliders fight
  normalisation) should land first, or together with this. The shared
  direction control in step 6 should use its azimuth/elevation fix
  rather than copy the bug to the pulse and comet sections.
- **Imported targets.** A look that targets instance ids (`mouse/strip`)
  will not match another user's layout. C-7's type/category targets are
  the portable form, so a note on import is enough for now.
- **Daniel to decide:**
  - Whether import should *replace* an existing personal look with the
    same id when the user confirms. The engine currently has a
    `replace` flag only for device types (`WorkspaceService.cpp:1891`).
  - Whether imported looks should carry a provenance field (for
    example an "imported from <file>" description suffix).
