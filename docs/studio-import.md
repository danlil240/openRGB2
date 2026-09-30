# Desktop Lighting Studio import — living status

This document tracks the in-progress import of the Desktop Lighting
Studio (DLS) feature set into OpenRGB2. The authoritative work
breakdown is `docs/superpowers/plans/2026-09-29-desktop-lighting-studio-import.md`;
later tasks extend this file as they land.

## Approach

Selective adaptation, not an embed and not a rewrite. The Qt-free
Studio model code (scene/config/effects/presets/editor/input-model)
lives in one shared static library, `openrgb2_studio_core`
(`src/core/`), linked by the engine and the offline test suites — no
source is compiled into two targets. The engine keeps owning
evaluation, input capture, playback, and hardware delivery through
the pinned OpenRGB backend; UI code speaks pipe traffic only and
must not link the engine, the SDK client, or controller interfaces.
Studio's QML panels are adapted onto `ui/theme/Theme.qml` tokens and
existing controllers rather than copied verbatim.

Vendor-specific playback (`src/core/playback/PlaybackLoop.cpp`,
which includes `LianLiWirelessProtocol.h`) and the LianLi codec /
tinyuz / divsufsort sources stay compiled in `openrgb2_engine`,
`playback_test`, and the `tests/backend` harness — they are not part
of the shared model.

## Source of record and provenance

- Source (read-only): `C:\Users\daniel\tools\openrgb-unified\plugins\DesktopLightingStudio`
  — the working tree, including uncommitted files. Never modify,
  reset, or clean it; copy from it.
- Per-file ledger: `vendor/provenance/dls-ui-import-hashes.txt`
  records the SHA-256 of every plugin source file that is already
  byte-identical or slated for a later copy, with its destination
  path and adaptation note. Verified on import: all **73** files
  under `scene/`, `effects/`, `presets/`, `schemas/`, `inputs/`,
  `config/`, `editor/` that have a `src/core` counterpart are
  byte-identical.
- `vendor/provenance/MANIFEST.md` documents the original Milestone-0
  import and the remaining deferred components.

## Prerequisites

- **Qt Quick3D provisioned for Task 8 (3D viewport).** `qtquick3d`
  for Qt 6.8.3 msvc2022_64 is installed at
  `C:\Qt\6.8.3\msvc2022_64` (`Qt6Quick3D` CMake config + QML module
  present; verified by the `OPENRGB2_WITH_QUICK3D` configure probe).
  The viewport subtree is capability-gated: a kit without Quick3D
  configures and builds fine, `quick3dAvailable` is false, and the
  Layout page keeps its 2D canvas with the 3D controls hidden.

## Parity status (high level)

| Area | Status |
| --- | --- |
| Shared model core (`openrgb2_studio_core`) | Consolidated; includes `PresetBundle`, `DefaultDesk`, `EditorController`, `TransformCommands` |
| Scene/effect evaluation, schemas, v3 compact workspace model | Imported core; exercised by `core` label suites |
| Reactive input capture (WASAPI, screen, keyboard, power) | Qt-free Windows implementations in `src/platform/windows` |
| Compact workspace durability (engine `WorkspaceStore`) | Done (Task 2) |
| Workspace IPC (revisions, transfers, jobs, preview) | Done for workspace + resource jobs incl. `resource_install` (Tasks 3, 6); preview verbs land with Task 7 |
| Shared UI draft/edit session, recovery | Done (Task 4): `WorkspaceEditSession` owns the draft + bounded undo; every adapter commits through it |
| Scene/object editing + device library panels | Done (Task 5): `src/ui/studio/` adapters (`SceneObjectModel`, `PresetListModel`, `ObjectEditorController` on the shared `WorkspaceEditSession`) + `ui/studio/` panels (`DeviceTree`, `DeviceLibrary`, `TransformInspector`) mounted on the Layout page; `ui_studio_objects_test` covers the seam headless |
| Layer/palette/personal-look editing | Done (Task 6): `src/ui/studio/` `EffectLayerModel` (row-granular diff model) + `EffectEditorController` (stack/palette/path/source/targets edits, gesture commit/cancel, named-look materialization, `saveLookAs` via `resource_install`, persisted draft `inputs.*`) + `ui/studio/` panels (`EffectEditor`, `EffectInspector`, `LayerStack`, `PaletteEditor`, `LookShelf`) mounted on the Lighting page; `ui_studio_effects_test` covers the seam headless |
| Engine prepared scene (`PreparedStudioScene`) | Done (Task 6): `src/engine/StudioScenePreparation` expands matrix emitters + builds `key_positions` against matched hardware and applies `inputs.decay_pct` to runtime ripple density on a copy (authored data untouched); consumed by `SceneWorker` now and preview later; `studio_scene_preparation_test` covers it |
| SDK topology fidelity | Done (Task 6): bounded zone matrix maps + LED names ride `SdkWire`/`OpenRgbClient`/`DeviceSnapshot` end to end |
| 3D viewport (`StudioScene.qml` → `StudioViewport`) | Done (Task 8): `ui/studio/viewport/` (`StudioViewport` + `editor/` camera/selection/gizmo + `devices/` families + `materials/StudioEnvironment`) on explicit `objectCtl`/`workspaceCtl`/`viewport`/`prefs` collaborators — no global `bridge`/`studioHost`; camera pose persists via `WorkspaceController.setCameraState` (meta.camera, never undo), quality/bloom via `renderQuality`/`renderBloom`, reduced-motion via `reducedMotion()`; Layout page keeps the 2D canvas + a persisted `uiPrefs.layoutShow3d` toggle gated on `quick3dAvailable`; `qml_studio_viewport_test` + `tests/ui/studio_qml/` cover the seam |
| Device-type authoring + portable bundles | Done (Task 9): `DevicePresetController` (validate/preview/variant/assembly/delete via bounded resource jobs) + `DevicePresetEditor.qml` — **parity gap, documented:** id/name/category are structured fields; entities/zones/layouts/assets are edited as schema-true JSON in a body pane (structured field editing is NOT carried over). Portable `openrgb2-bundle-files` v1 bundles reuse `InspectBundle`/`ApplyImport`/`ExportBundle` — staging scratch dirs, path/size/file-count caps, no live/input arming, personal-look layers materialized inline (named preset kept as provenance, look files never transported); the committed workspace retains a re-serialized remapped document — local library files are never overwritten, but byte-identical retention applies to resources, not the workspace doc. `WorkspaceImportDialog.qml` inspect→conflict→confirm flow with `has_workspace`/conflict `id` contract honored; `workspace_bundle_test` + `ui_studio_presets_test` + `ui_workspace_import_test` (real engine over an in-process pipe) cover it |
| Workspace file actions + external-change recovery | Done (Task 10): `WorkspaceStore::ExternalScan` fingerprints the active generation (markers + document/manifest/original hashes + resources stamp) — read-only, byte-identical rewrites reproduce the signature (same-content suppression); `CheckExternal` compares it against the adopt/commit baseline and reports `none`/`modified`/`deleted` on the polled `get` — detection never loads, never saves, never touches `apply_scene`. `LoadBackup` + the `restore_backup` job verb commit the marker backup's last-valid generation FORWARD as a new immutable generation (revision-gated, then applied as an explicit user action); `reload` accepts an optional last-seen revision pair and refuses `revision_conflict` when the marker moved. UI: `WorkspaceActionsDialog` (open/save-copy/reload/restore-backup/reset/open-folder via `QDesktopServices`, UI-side only), the strip gains the external-change notice + a replace latch (`replaceOffered`/`replaceText` carry source/destination before a dirty draft is displaced), and the durable draft store survives workspace switches + sessions. Task-9 fix edges landed here: export refusal unsticks the dialog (`error` path), `confirmImport` empty-mode ids can't collide with a managed slug, `preset_import` with no managed workspace refuses honestly, `DevicePresetEditor` guards late `presetDocumentReady` replies by `fetchId`. `workspace_store_test` (ExternalScan/LoadBackup/future-schema/failed-commit-retry) + `ui_workspace_recovery_test` (real engine over the pipe) cover it |
| Single-LED calibration + diagnostics parity | Not started (Task 11) |
| Packaging/notices for the imported workflow | Not started (Task 12) |
