# Q-14: UI features — colour picker, delete/duplicate keys, delete confirmations, instanced LEDs

Status: proposed · Size: M (four independent S/M items) · Source: audit 2026-11-28 Q-14

## Goal

Four quality-of-life improvements to the QML UI, each landable on its
own:
- one consistent dark-themed colour picker everywhere a colour is
  chosen
- keyboard Delete and Duplicate for layout objects and effect layers
- a confirmation before deleting a saved profile or a device type
- a 3D viewport that stays smooth on LED-dense layouts

## Shared context

- **Design tokens** live in `ui/theme/Theme.qml`. `Theme.paletteSwatches`
  is at `:47`.
- **Reusable controls** live in `ui/components/`: `ColorWell` is a
  display-only swatch, and `PaletteSwatch` is a clickable swatch.
- **Q-2 is a prerequisite for item 2.** It covers the duplicate
  window-wide undo shortcuts (`ui/Main.qml:223-264`,
  `StudioViewport.qml:884-889`) and the viewport's single-letter keys,
  which are live on every page (`:872-891`). New keys must follow
  whatever scoping Q-2 lands. They must never be window-wide.

## 1. Shared themed colour picker

- **Current.** There are four different colour inputs:
  - `ui/studio/WaveControls.qml:75-83` uses `QtQuick.Dialogs`
    `ColorDialog`. It is the native, unthemed dialog, and Q-3 notes a
    cancel leaves a duplicate stop.
  - `ui/studio/PaletteEditor.qml:167-190` uses a `ColorWell` plus a
    `#RRGGBB` text field.
  - `ui/pages/LightingPage.qml` has two hand-rolled HSV slider triplets:
    the group colour `palH/palS/palV` (`:862-930`) with quick swatches
    (`:958-966`), and the uploaded-preset colour `upH/upS/upV`
    (`:1292-1330`).
  - `ui/studio/TransformInspector.qml:306-330` uses the paint colour
    well and swatches.
- **Design.**
  - A new `ui/components/ColorPicker.qml` is an inline, themed control:
    a hue strip, a saturation/value square, a hex field, the
    `Theme.paletteSwatches` row and a "recent colours" row (the last 8,
    in `uiPrefs`).
  - API:
    - `property color value`
    - `signal edited(color c)`, live while dragging
    - `signal committed(color c)`, on release or Enter
  - A `ColorPickerPopup.qml` wraps it in a themed `Popup` anchored to a
    swatch, with OK and Cancel. Cancel emits nothing, which fixes the
    Q-3 duplicate-stop case for free.
  - Live `edited` maps onto the existing gesture pattern:
    `beginLayerGesture` on open, `commitLayerGesture` on OK and
    `cancelLayerGesture` on Cancel. The result is one undo record.
  - **Rejected:** restyling `QtQuick.Dialogs.ColorDialog`. It is native
    on Windows and cannot follow `Theme`.
- **Steps.**
  1. Add `ColorPicker` and `ColorPickerPopup`. Test: a new
     `tests/ui/studio_qml/tst_colorpicker.qml` (label `ui`): a hex round
     trip, swatch picks, Cancel emitting nothing, and the recent-colours
     cap.
  2. Replace the `ColorDialog` in WaveControls, then the PaletteEditor
     stop editor, then the two LightingPage triplets, then
     TransformInspector, one commit each. Tests: the existing
     `ui_studio_effects_test` and `qml_smoke_test`, plus a QML case per
     site that a commit lands one undo record.

## 2. Delete and Duplicate keys for layout objects and layers

- **Current.**
  - Objects have toolbar buttons only:
    - `DeviceTree.qml:216-219` is "Del", which uses `requestDelete`
      (`:120-139`) with the cascade confirmation.
    - `:213` is `duplicateMirrored`.
  - The core has `Delete`, `DeleteCascade` and `DuplicateMirrored`
    (`src/core/editor/EditorController.h:139-172`).
  - Layers have only a remove button (`ui/studio/LayerStack.qml:146`).
    There is **no** duplicate-layer operation in the core: `AddLayer`
    takes a primitive name only (`EditorController.h:291-296`).
- **Design.**
  - **Delete / Backspace:**
    - With focus in the device tree, the layout canvas or the 3D
      viewport, it calls the existing `requestDelete()`, which keeps
      the cascade prompt.
    - With focus in the LayerStack, it calls `removeLayer(current)`.
  - **Ctrl+D:**
    - For objects it calls `duplicateMirrored()`: the existing
      safe-by-design copy, which never makes a second zone writer.
    - For layers it calls a new core `DuplicateLayer(i)`: a copy is
      inserted at i+1 in one undo record, and it is refused while a
      gesture is open.
  - Keys are attached with `Keys.onPressed` on the focused panel. They
    are not `Shortcut` items, per Q-2. All of them are ignored while a
    text field has focus.
- **Steps.**
  1. Add core `DuplicateLayer`. Test: `studio_editor_test` (label
     `core`): copy equality, index, one record, refused mid-gesture,
     and refused at `EFFECT_MAX_LAYERS`.
  2. Add the `effectEdit.duplicateLayer`, the LayerStack button and the
     keys. Test: `ui_studio_effects_test` and a `tst_layerstack.qml`
     key case.
  3. Add the DeviceTree, canvas and viewport keys. Test:
     `tst_controllers.qml` and `qml_layout_drag_test`: Delete triggers
     `requestDelete`, nothing happens in a text field, and the key is
     inert when the Layout page is not visible.

## 3. Confirmation for profile and device-type Delete

- **Current.**
  - Profile Delete is immediate: `LightingPage.qml:2032-2036` calls
    `sceneEdit.deleteProfile(name)` with no undo, and
    `ProfileLibrary::removeProfile` rewrites the file.
  - The device-type editor's Delete is immediate:
    `ui/studio/DevicePresetEditor.qml:484-496` runs
    `deletePresetType` as a job.
  - `DeviceLibrary.qml` already has a delete-type dialog
    (`delTypeDlg`, `:846-930`). It is the model to copy.
- **Design.** A small shared `ConfirmDialog.qml` in `ui/components`
  (title, body, a danger button, Cancel as the default, Escape
  cancels). Both buttons open it with "Delete profile '<name>'? This
  cannot be undone." or "Delete device type '<id>'? Instances using it
  will fail to resolve." `delTypeDlg` migrates to it in the same change
  for consistency.
- **Steps.**
  1. Add `ConfirmDialog`. Test: `tst_confirmdialog.qml`: Escape is
     Cancel, the danger button emits `accepted`.
  2. Wire the profile and preset-editor Deletes, and migrate
     `delTypeDlg`. Test: the `qml_smoke_test` and `ui_studio_presets_test`
     paths, where a deletion only happens after accepting.

## 4. Instanced emitter spheres in the 3D viewport

- **Current.**
  - Per LED, `StudioViewport.qml:584-626` creates a `Repeater3D`
    delegate with **two** `Model`s: the visible dot with its own
    `PrincipledMaterial`, and a 2.6× invisible pick proxy named
    `emit|<obj>|<i>`.
  - Colours update by reassigning `emitterColors` per tick (`:448-500`).
  - Picking and paint rely on the proxy names:
    `editor/SelectionController.qml:149,244,335,375`,
    `tests/ui/qml_studio_viewport_test.cpp:480-488` and
    `tests/ui/studio_qml/tst_controllers.qml:207-213`.
- **Design.**
  - A C++ `EmitterInstancing : QQuick3DInstancing` in `src/ui/studio/`,
    built only when `OPENRGB2_WITH_QUICK3D` is on (`CMakeLists.txt:50-66`),
    with one instance table per object: position, a uniform scale, and
    the colour as the instance colour.
  - Positions update only on a layout change. Colours update with a
    `markDirty()` on each preview tick from `StudioViewportModel`, with
    no QML array churn.
  - One `Model { source: "#Sphere"; instancing: … }` per object draws
    the dots. One more instanced model at 2.6× scale with
    `pickable: true` serves picking. The pick handler reads
    `PickResult.instanceIndex` and rebuilds the same `emit|<obj>|<i>`
    key string, so `SelectionController` and the tests keep their
    contract through a small `emitKeyFor(pick)` helper.
- **Rejected:** merging all LEDs into one custom geometry. Per-LED
  picking would then need a CPU ray test, which instancing already
  provides.
- **Steps.**
  1. Add `EmitterInstancing`, plus a probe. Test: a
     `qml_studio_viewport_test` case for the instance count and the
     colour buffer.
  2. Replace the dot `Repeater3D` with an instanced model. The pick
     proxies stay for now. Test: the existing viewport tests are
     unchanged.
  3. Add instanced picking plus `emitKeyFor`, and remove the per-LED
     proxies. Test: `qml_studio_viewport_test` (paint through the
     instanced pick) and `tst_controllers.qml` (stub picks keep the
     `emit|` names).
  4. Measure frame time before and after on Daniel's full layout with
     the QA harness (`qa.py`, `tools/qa`). Record it in the PR. No
     numbers are claimed here.

## Tests (summary)

- **`core`:** `studio_editor_test` (`DuplicateLayer`).
- **`ui`:** `ui_studio_effects_test`, `ui_studio_presets_test`,
  `qml_smoke_test`, `qml_layout_drag_test` and
  `qml_studio_viewport_test`.
- **New QML tests:** `tst_colorpicker`, `tst_confirmdialog` and
  `tst_layerstack`.
- **Manual:** the picker at 150% scale and the 960 px narrow layout
  (the Task 12 passes), and the viewport frame time on real hardware.

## Risks and open questions

- **`PickResult.instanceIndex`.** Instanced picking needs it in the
  pinned Qt 6.8.3. It is believed available since 6.5. Verify before
  step 4.3; if it is missing, keep the non-instanced proxies and
  instance the dots only.
- **The instance colour multiplies the material base colour.** Use a
  white unlit base. The darkened colour for unbound LEDs
  (`Qt.darker(c, 2.5)`, `:599-601`) moves into the C++ colour fill.
- **Daniel to decide:**
  - Should Ctrl+D on objects stay *mirrored-only*, or also offer an
    independent copy (a new instance without bindings)? The plan keeps
    mirrored only, as the safety contract in
    `EditorController.h:36-38` requires.
  - Should the "recent colours" list persist per user (`uiPrefs`, as
    planned) or per workspace?
