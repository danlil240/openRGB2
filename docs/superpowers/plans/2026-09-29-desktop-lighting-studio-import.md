# Desktop Lighting Studio Import Implementation Plan

> **For agentic workers:** Use `superpowers:executing-plans` to implement this plan task by task, matching the workflow in `IMPLEMENTATION_PLAN.md`. Checkboxes track future implementation. This request creates the plan only; implementation has not started.

**Goal:** Bring the full Desktop Lighting Studio feature set into OpenRGB2's existing interface, including spatial editing, device and effect authoring, recovery, and portable workspaces.

**Architecture:** Reuse the already imported Qt-free Studio core. Preserve a compact authoring workspace alongside the engine's resolved runtime scene, expose revision-checked workspace operations over IPC, and adapt Studio's QML panels to OpenRGB2's theme and controllers. OpenRGB2's engine continues to own evaluation, input capture, playback, and hardware delivery through the pinned OpenRGB backend.

**Tech stack:** Windows x64, C++17, MSVC 2022, CMake, Qt 6.8.3 / Qt Quick, Qt Quick 3D for the spatial editor, nlohmann JSON, existing restricted named pipes and OpenRGB SDK v4.

**Spec:** Sections 1–5 below define this migration. `IMPLEMENTATION_PLAN.md` sections 1, 3.4, 4–6 remain the product and hardware constraints; this plan expands its UI milestone rather than restarting completed milestones.

**Source:** `C:\Users\daniel\tools\openrgb-unified\plugins\DesktopLightingStudio` — read-only source of record.

**Destination:** `C:\Users\daniel\tools\openRGB2`.

**User decision:** Full Studio features, adapted to OpenRGB2's UI; confirmed during planning on 2026-09-29.

## Global constraints

- Never modify, reset, clean, build in, or deploy over `C:\Users\daniel\tools\openrgb-unified`.
- Copy selected working-tree source files, including relevant untracked files; a Git revision alone is not the source snapshot. Record SHA-256 provenance and adaptations.
- Preserve current uncommitted OpenRGB2 work. Re-read the working tree before execution and integrate with its current behavior.
- Keep `src/core/`, `src/engine/`, `src/ipc/`, and Windows engine platform code Qt-free. UI code must not link the engine, SDK client, controller interfaces, or USB transport.
- Use Qt 6.8.3 at `C:\Qt\6.8.3\msvc2022_64` and MSVC 2022 BuildTools. Quick3D is an additional component of that pinned kit, not a toolkit upgrade.
- Keep ordinary lighting controls usable without entering the 3D editor.
- Keep one hardware producer per wireless group. Arbitrary authored layers, palettes, audio, screen, and key reactions use Live effects; retain existing uploaded-playback capabilities and restrictions.
- Preserve 120 logical wireless LEDs and motherboard header counts 8 / 16 / 8; mirrored visual copies do not add output writers or addresses.
- Preserve missing devices, identity-based bindings, calibration, mode selection, display-power handling, and UI-independent playback.
- No cooling, LCD, Linux/macOS port, firmware work, automatic binding, driver replacement, or ownership takeover is part of this migration.
- Physical identification/calibration/benchmarks remain explicit user actions. Automated tests use fake hardware and ordinary CTest executables; no Smart App Control workarounds.
- Preserve GPL-2.0-or-later source notices and existing third-party notices. Include new Quick3D runtime dependencies in package inventory and notices.

## Review focus

1. Compact workspaces must survive editing and restart without losing device types, hierarchy, metadata, extensions, colors, or effect layers. Tasks 2–4 and 12 exercise this.
2. Two clients, out-of-order replies, or reconnection must not overwrite newer work or clear an unsaved draft. Tasks 3–4 exercise this.
3. Custom type/look collisions, missing assets, and interrupted imports must not corrupt the active workspace or overwrite local content. Tasks 2 and 9 exercise this.
4. Calibration and preview during uploaded playback, display-off, or topology changes must not create another hardware writer or restore stale output. Tasks 7 and 11 exercise this.
5. Large scenes, effect gestures, and 3D rendering must remain bounded and must not stall engine output or basic controls. Tasks 3, 6–8, and 12 exercise this.

## 1. What is already here

Planning inspection used source reads, a focused Graphify dependency extraction, and SHA-256 comparisons. The graph parser reported partial extraction for several Qt macro-heavy headers; direct source inspection is the authority for the findings below.

- OpenRGB2 HEAD inspected: `385f1dd3b6896a3324ede8a73d9e28bdecb21aa9`, with substantial uncommitted work. This is a working-tree plan, not a clean-HEAD inventory.
- Source parent HEAD: `386ad374d44770365bf90c4f1fb08c2522e3a4b6`; the source also contains working-tree additions/changes.
- **73 files are byte-identical** between the plugin's `scene`, `effects`, `presets`, `schemas`, `inputs`, `config`, `editor` directories and corresponding existing `src/core` files. No differing file was found among those matching paths. This is not a claim that the complete plugin is already imported.
- The existing core includes compact workspace schema v3, device/effect registries, migration, bundle handling, transform commands, and the Qt-free `EditorController`, including layer operations.
- Windows WASAPI, screen sampling, keyboard input, and power monitoring already have Qt-free engine implementations in `src/platform/windows`. Do not port those a second time.
- OpenRGB2 already has basic effect selection, profiles, layout, orientation, linked copies, bindings, identification, calibration addresses, and undo/redo. Extend those flows.
- `vendor/provenance/MANIFEST.md` describes the original import. Its “deferred” list predates the current Windows input implementations and must be updated during implementation.

### Feature and source mapping

Paths in the source column are relative to the plugin unless prefixed with `tests/`, which means the source parent repository's tests.

| Capability | Source/reference | Existing OpenRGB2 state | Migration work |
| --- | --- | --- | --- |
| Scene/effect evaluation and schemas | `scene/`, `effects/`, `schemas/` | Imported core | Reuse and test; no bulk recopy |
| Device/look library data | `presets/devices/`, `presets/effects/` | 13 device presets and 9 look files already imported | Expose complete authoring/library workflows |
| Compact authoring and migration | `config/StudioConfig`, `ConfigMigration` | Imported; CLI loader resolves compact documents | Retain compact source and dependencies through IPC/save/restart |
| Scene tree and object editing | `editor/EditorController`, `SceneObjectModel`; `ui/components/DeviceTree.qml` | Core commands imported; UI has a simpler applied-scene canvas | Shared editable workspace, hierarchy, multiselect, grouping, locks, add/delete/rename, align/distribute |
| 3D viewport | `ui/StudioScene.qml`, `ui/editor/`, `ui/devices/`, `ui/materials/` | 2D layout canvas; no Quick3D target | Adapt into Layout's 3D view with existing theme and controllers |
| Device library/type authoring | `editor/PresetListModel`; `DeviceLibrary.qml`, `DevicePresetEditor.qml` | Registry/parser present; no equivalent full editor | Search, favorites, drag/drop, variants, composed types, entities/zones/geometry/assets |
| Layer/palette authoring | `editor/EffectLayerModel`; `EffectEditor.qml`, `LayerStack.qml`, `PaletteEditor.qml`, `EffectInspector.qml`, `LookShelf.qml` | Core layer editing exists; Lighting UI mostly exposes preset/global controls | Ordered layers, blend/opacity, targets, space/origin/direction/path/source, gradient stops, personal looks |
| Workspace file behavior | `config/ConfigStore`, `plugin/StudioTab` file actions | Engine state store, UI profile library, and UI settings are separate | Workspace save/copy/reload/reset, last-valid backup, draft recovery, external changes |
| Portable bundles | `presets/PresetBundle` and bridge bundle methods | Core bundle code already imported, not wired into app | Inspect/import/export UI and managed installation with conflict choices |
| LED calibration | `LedCalibrator.qml`, `StudioTab::calibBegin/Show/End` | Per-address calibration and bounded identify overlay exist | Point layout authoring plus exclusive, cancellable single-LED session |
| Reactive settings | `inputs/`, `LookShelf.qml`, bridge settings | Engine captures and IPC controls already exist | Complete UI parity, per-look requirements, screen selection, sensitivity and ripple decay |
| Diagnostics | `StudioTab::diag*` | OpenRGB2 diagnostics, delivery stats, `tools/bench` exist | Map refresh/identify/measurement controls to existing engine tools and honest rate labels |
| Plugin lifecycle/output | `plugin/DesktopLightingStudio`, `StudioTab`, bridge timers, `output/` | Replaced by standalone UI/engine/SDK | Use as behavioral reference only; do not load plugin DLL or copy direct hardware routes |

### Verified architectural gaps

1. **Authoring versus runtime document:** Studio edits `StudioDocument` v3 (`devices`, `device_settings`, `effects`, external type files). OpenRGB2's current UI edits expanded `SceneDocument` (`objects`, `effect`). `LoadSceneDocument` can resolve v3/legacy files for `--scene`, but retains only the resolved scene. `EngineService::SetScene` directly parses an expanded `SceneDocument`; the documentation's “same path as --scene” wording is broader than the implementation. Correct both paths and the contract before adding authoring controls.
2. **Competing edit models:** Lighting stages a working document until Save; Layout currently mutates the applied document through commands and uses full-document undo. Porting another independent bridge would multiply conflicting document owners.
3. **Preview data:** `frame_status` contains last transport-accepted controller colors. It is not a complete authored-scene preview for unbound objects or unsaved layers, and does not prove physical presentation.
4. **Dependencies:** Quick3D's CMake config, DLL, and QML directory were absent from the pinned kit at inspection. Existing GUI/Widgets/Quick support alone cannot run `StudioScene.qml`.
5. **Bounds:** Existing IPC bodies are capped at 1 MiB. Bundle JSON files allow 4 MiB, individual assets 16 MiB, and bundles 64 MiB. Full workspaces/assets cannot be sent as one unbounded JSON request.
6. **Reactive spatial parity:** Capture exists, but the old bridge also constructs keyboard emitters from zone matrix maps and translates key events to world-space emitter positions. The current SDK parser skips matrix-map bytes and does not retain LED names, so those behaviors need an SDK metadata and scene-preparation port. The old bridge also scales ripple-layer density by `inputs.decay_pct / 100`; merely exposing that existing workspace field would not implement its effect.

## 2. Selected approach

| Approach | Trade-off | Decision |
| --- | --- | --- |
| Embed the old DLL/QQuickWidget/SceneBridge | Quickly exposes old controls, but brings direct hardware access, UI-owned timers and persistence, and plugin-host dependencies | Reject |
| Reimplement every Studio feature from scratch | Fits the new app, but duplicates a large tested model/editor implementation | Reject |
| Selectively adapt Studio models and QML around a shared workspace and existing engine | Requires document/IPC work first; preserves tested behavior and OpenRGB2's process boundaries | **Use this** |

Full feature parity means the capabilities in the matrix above. It does not require the old window chrome, exact pixel layout, plugin API, or OS implementations outside Windows.

## 3. Document, ownership, and IPC design

### 3.1 One authoring session, one applied engine state

- Introduce a shared Qt-free `openrgb2_studio_core` CMake library containing the scene/config/preset/editor/effect/input-model code needed by both processes. Keep playback codec/vendor transport code in its existing engine/backend targets.
- The engine owns an accepted workspace snapshot: compact v3 document, a versioned snapshot of referenced device types/personal looks, resolved immutable scene, workspace revision, and resource revision. `SceneWorker` consumes only the resolved scene.
- A UI `WorkspaceEditSession` owns one draft used by Lighting, Layout, device/type panels, and layer panels. It uses imported `studio::EditorController` and returns typed undo records. The draft and engine's accepted snapshot are separate by design.
- Keep OpenRGB2's ordinary **Save** meaning: validate, persist the workspace, then publish the accepted scene for the next engine tick. Engine acceptance and actual delivery remain separate statuses. Save Copy writes another workspace without activating it.
- Gestures modify the local draft; each committed gesture produces one undo record. History is bounded by both 32 entries and 32 MiB. Cancel restores the gesture snapshot. Changing pages preserves the draft and selection.
- Explicit preview evaluates the draft without changing the saved scene or sending hardware frames. Applying an edit to the lights uses Save. Existing playback, identify, and reactive-input controls remain deliberate engine commands.
- Use `UiSettings` for app/window preferences. Preserve Studio's camera/render/favorites metadata in the workspace and restore it when that workspace is opened; preserve unknown `extensions` verbatim.

### 3.2 Persistence and migration

- Add a Qt-free engine `WorkspaceStore` under an explicit OpenRGB2-owned workspace root. Default development root: `profiles/local/workspaces`; packaged launchers pass a writable app-data root. No installed source directory is writable state.
- Reuse `ConfigStore`'s behavior and tests, not its QObject/QSaveFile/file-watcher implementation inside the engine. Use the existing ProfileStore atomic-write conventions and Windows atomic replacement behavior.
- Store immutable workspace generations (document plus a resource manifest), then atomically replace an active-generation marker. Prepare/validate all state before that marker changes; publish the prepared in-memory snapshot under the same serialized commit boundary with no fallible I/O after marker replacement. Recover the last valid generation after interruption.
- A failed validation/write/commit leaves the previous generation active. Orphan staging files are ignored and cleaned only within the verified managed root. Do not expose partial imports through a live registry.
- Import v3 workspaces and their custom type/look files, older v1/v2 workspaces, legacy host settings, and expanded OpenRGB2 scenes. Use `MigrateLegacySettings` / `MigrateExpandedScene`, resolve against a scratch registry, preserve warnings, and retain original imported bytes locally for recovery.
- Migrated scenes default to Live effects. Import never automatically enables output, input capture, or verified hardware ownership. Local identity hints may be retained for review; portable export strips serial/location and verification through `SanitizeForExport`.
- Keep UI draft recovery separate from applied state: per-workspace, per-client draft records contain the baseline revision and content hash, debounce at 2 seconds, flush on normal close, and offer explicit recovery after a crash. A recovery file never automatically applies to the engine.
- Import existing `scene-library.json` entries into managed workspaces without deleting the original library. Leave `engine-state.json` responsible for current playback/input/identity state; it is not a replacement for the editable workspace.
- Files changed externally trigger a compare/reload notice. Do not overwrite a dirty draft, silently reload hardware output, or use a file watcher as an apply command.

### 3.3 IPC extension

Retain protocol version 1 and existing framing; advertise additive capability `studio_workspace_v1` in WELCOME. Add `WORKSPACE_COMMAND` (`0x010e`) with existing OK/ERROR and EVENT envelopes; do not reinterpret old message numbers.

| Operation family | Contract |
| --- | --- |
| `get`, `list`, `catalog` | Return metadata/revisions and bounded pages; fetch large document/resource content with the transfer mechanism |
| `validate`, `commit`, `save_copy` | Consume a staged candidate; include `base_revision` and `resource_revision`; stale candidates fail with stable detail `revision_conflict` |
| `import_inspect`, `import_commit`, `export`, `reload` | Asynchronous jobs; inspect reports dependency conflicts before a commit; export produces a managed result for an explicit UI-selected destination |
| `transfer_begin`, `transfer_chunk`, `transfer_finish`, `transfer_cancel` | Session-bound, opaque IDs; declared kind/length/SHA-256; ordered offsets; no client-selected server write paths |
| `job_status`, `job_cancel` | Poll by ID after a lost event in the current session; after reconnect reconcile accepted workspace revision/content hash; accepting a job is not a successful save |
| `preview_begin`, `preview_update`, `preview_end` | Operate on validated draft candidates; latest pending candidate replaces an older unsent one; no delivery sink |

Starting limits: 192 KiB decoded bytes per transfer chunk (base64 JSON fits below 1 MiB), 4 MiB per document/type/look JSON, 16 MiB per asset, 64 MiB per imported bundle, at most two transfers per session and 128 MiB staged across the server. Expire idle transfers after 60 seconds. Use disk staging and bounded buffers, not a full bundle in each request. Content caps already present in the core remain in force.

Run file work on one bounded workspace job worker (at most eight pending jobs across clients). IPC handlers only validate/enqueue/read snapshots; do not perform file I/O while holding the service mutex. Recheck revisions when a job commits, not just when enqueued. Completion events contain job ID, revision, and success/error; clients can query status when an event is missed. Cancel queued jobs and staging on disconnect; a commit past the atomic marker boundary is reconciled against the accepted revision/content hash after reconnect, never falsely rolled back or blindly resubmitted. An intervening different commit remains a visible conflict.

Legacy behavior: keep GET_SCENE and telemetry available. Continue SET_SCENE and existing layout mutations for legacy scenes before they become managed workspaces. Once a managed workspace is active, reject lossy legacy document mutations with `bad_request` and detail `workspace_required`; the updated UI routes those edits through the shared draft. Identify/cancel-identify remain available. This explicitly avoids reverse-converting an expanded edit and silently losing compact authoring data. Update `engine_ctl` and compatibility tests accordingly.

### 3.4 Preview and calibration

- Add an engine `PreviewSession` with no `IFrameSink`/SDK reference. It uses the same evaluator, brightness, timeline and captured-input snapshots as normal evaluation. Paused/disabled inputs remain empty; requesting a preview never starts capture.
- One active preview per UI session, a global maximum of two; replace pending updates, cap rendering/publishing at 30 Hz, pause hidden views/display-off and release on disconnect. Publish by draft revision/frame sequence, chunking visible-object color data below the IPC cap. Clients drop incomplete/stale sequences. Above the configured display budget, show geometry plus a clearly labelled reduced preview, never an apparently complete truncated frame.
- Applied-output preview continues to use `frame_status`. Label draft preview, accepted output colors, preview FPS, and physical rates distinctly. Unbound objects can appear in draft preview.
- Extend engine-owned identify behavior into a calibration session that temporarily owns the selected output group/zone, coalesces single-LED steps, times out, and restores through the current playback controller. Pin controller identity and topology generation. UI close, cancel during start, disconnect, display-off, or topology change ends the session; never restore cached bytes to a different device.
- Calibration of a wireless zone must respect ownership of the complete wireless group while uploaded playback is suspended. Resume the current validated mode/scene, not a saved stale frame.

## 4. UI placement and parity requirements

- **Lighting:** retain simple preset/color/speed/group controls. Add an expandable layer editor, look shelf, palettes, targeting, paths, source settings, play/pause/stop, and deterministic remix/seed controls. Saving a personal look must survive restart.
- **Layout:** retain the 2D overview and add a 3D toggle. Add searchable hierarchy, multiselect, device library/favorites, add/rename/delete, lock/hide, parent/group/ungroup, linked copies, alignment/distribution, transforms and snapping. Include whole-object and individual-emitter painting with undo. Camera presets include desk/top/front/case/free and orthographic/perspective.
- **Device/type editor:** open from Layout's library or selected device. Support entity transforms/appearance, child-type references, zones, point/ring/grid layouts, assets, validate/preview, save variant, create type from selection, delete/revert with dependency checks, and single-LED calibration. No duplicate per-entity hardware writers.
- **Workspace actions:** Save, Save Copy, Open/Import, Export Bundle, Reload, Restore Backup, Recover/Discard Draft, Reset, and Open Workspace Folder. Dirty/recovery/conflict notices use existing visual conventions. Flush the currently focused text editor before validating/saving.
- **Devices/Diagnostics:** expose calibration sessions and existing diagnostics with stable identity labels. Adapt old flash/refresh/measurement actions to engine commands/opt-in benchmark tools. Do not copy old direct-controller diagnostic workers.
- **Settings:** render quality (low/balanced/high), bloom, reduced motion, input selection/sensitivity/screen/ripple decay, and workspace location. Preserve actual input unavailable/paused/running errors.
- Adapt components to `ui/theme/Theme.qml`, existing controls, keyboard focus, scaling, and the QA harness. Preserve useful source test selectors or document their new mapping.

## 5. File and module boundaries

New filenames below are planned deliverables, not existing APIs.

| Area | Planned files/responsibility |
| --- | --- |
| Shared model | `src/core/CMakeLists.txt`; one `openrgb2_studio_core` library, independent of Qt and hardware |
| Canonical document | `src/core/config/WorkspaceCandidate.{h,cpp}`; parse/migrate/validate/resolve candidate with supplied registries |
| Engine persistence | `src/engine/WorkspaceStore.{h,cpp}`, `WorkspaceService.{h,cpp}`; generations, resource snapshots, revision-checked jobs |
| Bounded IPC work | `src/ipc/WorkspaceTransfer.{h,cpp}`, `WorkspaceCommands.{h,cpp}`; session-bound staging and command dispatch |
| Draft/edit bridge | `src/ui/WorkspaceEditSession.{h,cpp}`, `WorkspaceController.{h,cpp}`, `WorkspaceDraftStore.{h,cpp}`; shared draft, history, recovery, jobs |
| Adapted Qt models | `src/ui/studio/{SceneObjectModel,PresetListModel,EffectLayerModel}.{h,cpp}`; row models only |
| Feature controllers | `src/ui/studio/{ObjectEditorController,EffectEditorController,DevicePresetController,StudioViewportModel}.{h,cpp}`; focused QML adapters |
| Isolated preview | `src/engine/PreviewSession.{h,cpp}`; deterministic draft evaluation and bounded color snapshots |
| Spatial reactive preparation | `src/engine/StudioScenePreparation.{h,cpp}`; keyboard matrix expansion, key-to-world lookup, and runtime-only ripple parameter scaling |
| Calibration | `src/engine/CalibrationSession.{h,cpp}`, `src/ui/studio/CalibrationController.{h,cpp}`; exclusive identify lifecycle and presentation |
| UI panels | `ui/studio/`; adapted panels and `viewport/{editor,devices,materials}/`, composed into existing pages |
| Documentation | `docs/studio-import.md`, `docs/ipc-protocol.md`, `vendor/provenance/dls-ui-import-hashes.txt`; parity, protocol, exact copy/adaptation records |

## 6. Implementation tasks

Execute in dependency order. Each feature task ends with its targeted tests and a focused commit containing only its files. Preserve unrelated working-tree edits; never stage the whole repository indiscriminately. Use the verification commands in section 7 with each task's regex. New behavioral tests should fail for the missing behavior before implementation and pass afterwards.

### Task 1 — Establish the shared core and import ledger

**Files:** Create `src/core/CMakeLists.txt`, `docs/studio-import.md`, `vendor/provenance/dls-ui-import-hashes.txt`; modify root/engine/core-test CMake and `vendor/provenance/MANIFEST.md`.

**Interfaces:** Produce `openrgb2_studio_core`; preserve existing `studio::*` signatures. Include `PresetBundle`, `DefaultDesk`, `EditorController`, and `TransformCommands`, which must be linked where newly used, even though their source is already present.

- [ ] Recheck repository status and inventory source files; record hashes for every selected later copy, original relative path, destination, and adaptation. Exclude `build`, `out`, generated MOC/RCC/Makefiles, binaries, and runtime settings.
- [ ] Consolidate only shared model sources into the core target; keep codec and wireless-specific playback sources out. Update dependent source lists to avoid duplicate symbols.
- [ ] Run `core|engine|sdk` label suites and inspect target dependencies: no Qt, SDK, or vendor-controller dependency in the shared model.
- [ ] Record Quick3D as an execution prerequisite for Task 8. Its absence must not disable existing UI builds or block Tasks 2–7.
- [ ] Commit the shared-core/provenance change after existing regression suites pass.

### Task 2 — Preserve compact workspaces and resources durably

**Depends on:** 1.

**Files:** Create `WorkspaceCandidate.{h,cpp}`, `WorkspaceStore.{h,cpp}`, `WorkspaceService.{h,cpp}`, `tests/core/workspace_candidate_test.cpp`, `tests/engine/workspace_store_test.cpp`; modify `SceneLoader.{h,cpp}`, `EngineHost.{h,cpp}`, relevant CMake.

**Interfaces:** Define `WorkspaceCandidate` with `studio::StudioDocument document`, device/effect registry snapshots, and `std::shared_ptr<const studio::SceneDocument> resolved`. `ParseWorkspaceCandidate(const nlohmann::json&, const studio::PresetRegistry&, const studio::EffectRegistry&, WorkspaceCandidate&, std::vector<std::string>& errors, std::vector<std::string>& warnings) -> bool`. `WorkspaceStore::Load(id)` returns a candidate/revisions or structured failure; `WorkspaceService` alone commits accepted generations.

- [ ] Add `v3_roundtrip_keeps_authoring_data`, `legacy_extracts_types`, `expanded_scene_migrates`, `missing_type_rejected`, `future_version_not_rewritten`, and `resource_snapshot_survives_restart` cases. Compare compact fields and resolved transforms/colors/layers, not only parser success.
- [ ] Run the new tests to confirm the missing behavior; implement candidate loading using existing migration/resolver functions and isolated registry copies. Unify CLI loading with this candidate path while preserving expanded-scene compatibility.
- [ ] Add crash/failure cases before marker replacement, after marker replacement, invalid primary with valid backup, denied writes, resource collision, and interrupted resource installation. Assert prior state remains usable and incomplete generations are never selected.
- [ ] Implement bounded atomic generations and original-byte backups; retain portable sanitation and Live effects migration behavior. Do not call a source `ConfigStore::Load`, which may migrate its files in place.
- [ ] Run `workspace_candidate_test|workspace_store_test|profile_store_test|studio_config_test|device_preset_test`; commit.

### Task 3 — Expose workspace jobs, revisions, and transfers over IPC

**Depends on:** 2.

**Files:** Create `src/ipc/WorkspaceTransfer.{h,cpp}`, `WorkspaceCommands.{h,cpp}`, `tests/engine/ipc_workspace_test.cpp`; modify `IpcProtocol.{h,cpp}`, `IpcServer.{h,cpp}`, `EngineService.{h,cpp}`, `EngineHost.{h,cpp}`, `EngineBridge.{h,cpp}`, `EngineWireClient.{h,cpp}`, `docs/ipc-protocol.md`, `tools/engine_ctl/engine_ctl.cpp`, relevant CMake.

**Interfaces:** `WorkspaceService::Enqueue(session_id, verb, body) -> JobResult` (accepted job ID or rejection), `Status(session_id, job_id) -> JobStatus`, `CancelSession(session_id)`. Define serializable result/status types in `WorkspaceService.h`. Use section 3.3 operation names and bounds; revision values are uint64 counters. `WorkspaceController` will correlate request/job/revision independently.

- [ ] Add tests for capability negotiation, missing capability fallback, stale revision, two-client conflict, stale resource revision, diagnostics-channel rejection, and managed-workspace legacy-mutation refusal.
- [ ] Add 1 MiB envelope boundary, 4 MiB document transfer, invalid chunk order/hash, aggregate cap, expired transfer, path traversal/absolute path rejection, lost completion event, disconnect during commit, and a slow workspace job alongside responsive PING/scene ticks.
- [ ] Implement bounded staging and one job worker. Revalidate authoring/resource revisions at commit; publish immutable accepted snapshots through the existing scene-apply boundary. Keep loaded/accepted/applied/delivered states distinct.
- [ ] Add bounded catalog/document downloads using the same transfer mechanism. Do not expand framing limits to fit arbitrary scenes or binary assets.
- [ ] Run `ipc_workspace_test|ipc_protocol_test|ipc_server_test|ipc_engine_service_test|ui_wire_client_test`; commit the code and protocol together.

### Task 4 — Give every editing page the same draft and recovery behavior

**Depends on:** 3.

**Files:** Create `WorkspaceEditSession.{h,cpp}`, `WorkspaceController.{h,cpp}`, `WorkspaceDraftStore.{h,cpp}`, `tests/ui/ui_workspace_test.cpp`; modify `SceneEditController`, `SceneEditModel`, `LayoutController`, `LayoutEditModel`, `ProfileLibrary`, `main.cpp`, `ui/Main.qml`, relevant CMake.

**Interfaces:** `WorkspaceEditSession::LoadApplied(candidate, revision, resource_revision)`, `Working() const -> const studio::StudioDocument&`, `CommitEdit(studio::EditorEdit)`, `Undo()`, `Redo()`, `Discard()`. `WorkspaceController` exposes `dirty`, `conflict`, `saving`, `error`, `canUndo`, `canRedo`, `save()`, `saveCopy(name)`, `reload()`, `recoverDraft()`, `discardDraft()`.

- [ ] Test page switching with dirty edits, one gesture/one undo, Cancel/no history, save with focused text, save rejection preserving draft, edits made during save, stale echo after reconnect, and history entry/byte bounds.
- [ ] Replace independent managed-workspace draft ownership with adapters to the shared session. Keep existing simple Lighting/Layout APIs where possible and explicit fallback behavior for old engines.
- [ ] Add save/copy/open/reload/reset/backup actions, notices, and 2-second draft recovery. Test crash recovery against both matching and changed engine revisions; neither recovery nor profile import applies automatically.
- [ ] Migrate existing UI library entries on explicit import, retaining originals and names. A missing engine still permits local draft editing/recovery, with Save to engine unavailable.
- [ ] Run `ui_workspace_test|ui_scene_edit_test|ui_scene_bridge_test|ui_layout_test|qml_focused_save_test`; commit.

### Task 5 — Complete scene/object editing and the device library

**Depends on:** 4.

**Files:** Adapt source `SceneObjectModel` / `PresetListModel` into `src/ui/studio/`; create `ObjectEditorController.{h,cpp}`, `ui/studio/DeviceTree.qml`, `DeviceLibrary.qml`, `TransformInspector.qml`, `tests/ui/ui_studio_objects_test.cpp`; modify `ui/pages/LayoutPage.qml`, module CMake.

**Interfaces:** `ObjectEditorController` wraps the existing `studio::EditorController` selection, transform, add, rename, visibility/lock, delete cascade, group/ungroup, duplicate-mirrored, align/distribute methods against Task 4's draft. Models receive immutable resolved snapshots; they never own another document.

- [ ] Adapt source `tests/editor_qml/tst_library.qml` and relevant workspace/tree tests; add assertions for hierarchy, world-transform preservation, locked descendants, rename reference updates, and deletion previews.
- [ ] Implement searchable/filterable/favorite types, add/drop instances, multiselect, numeric transforms/snapping, hierarchy operations, object/emitter painting, and one-record gestures in the 2D view first. Test painted colors survive type resolution, linked-copy display, save and undo.
- [ ] Reuse `DuplicateMirrored` and test that linked instances add visual geometry without bindings/extra addressed LEDs. New instances start unbound.
- [ ] Verify edits persist through Save/restart and undo returns to the prior compact document. Run `ui_studio_objects_test|studio_editor_test|ui_layout_test`; commit.

### Task 6 — Complete layer, palette, and personal-look editing

**Depends on:** 4; use Task 5 object selection when present.

**Files:** Adapt `EffectLayerModel` into `src/ui/studio/`; create `EffectEditorController.{h,cpp}`, `ui/studio/{EffectEditor,EffectInspector,LayerStack,PaletteEditor,LookShelf}.qml`, `tests/ui/ui_studio_effects_test.cpp`, `src/engine/StudioScenePreparation.{h,cpp}`, `tests/engine/studio_scene_preparation_test.cpp`; modify Lighting page, `src/backends/openrgb/{SdkWire,OpenRgbClient}.{h,cpp}`, `EngineTypes`, `EngineService`, `SceneWorker`, existing SDK tests and module/test CMake.

**Interfaces:** Wrap existing `EditorController` layer/gesture methods. `EffectEditorController::saveLookAs(id, name)` submits a validated effect resource job; apply/reset/remix update the shared workspace draft and stable seed. Personal-look resources use Task 3's resource revision checks.

Define `PreparedStudioScene` with a resolved `studio::SceneDocument`, runtime `std::vector<studio::EffectLayer>`, and `std::map<int, studio::Vec3> key_positions`. `PrepareStudioScene(const WorkspaceCandidate&, const std::vector<DeviceSnapshot>&, PreparedStudioScene&, std::string& error) -> bool` expands hardware-dependent geometry against matched identities and builds the runtime parameter copy; it never mutates authored data. `PositionStudioInput(const studio::InputState&, const PreparedStudioScene&) -> studio::InputState` enriches transient key events using that scene's lookup. Both the scene worker and Task 7 preview use these functions.

- [ ] Adapt `tst_effects.qml`: ordered add/remove/reorder/enable, blend/opacity, layer scalars, local/world space, origin/direction/path, targets, source controls, gradient stop insert/move/color/delete.
- [ ] Test first-edit materialization of a named preset and undo back to preset resolution; slider/stop drag is one history entry even across model refresh. Reject invalid/over-limit layers, stops, targets and paths at parser and UI boundaries.
- [ ] Implement the panels in Lighting's advanced section, personal-look save/reload, deterministic remix, and explicit notice before replacing authored layers with a preset.
- [ ] Preserve bounded zone matrix maps and LED names in SDK/controller snapshots; parse them against the pinned backend's protocol layout. Use existing `layout::KeyboardMatrix` and `VkForKeyName` to prepare emitters and key world positions, rebuilding after keyboard/parent transforms and topology changes. Keep source first-match behavior for duplicate key names; unmatched key events retain the layer-origin fallback. Test malformed/truncated matrices, empty cells, out-of-range indices, missing keyboard, and a moved/rotated keyboard; do not retain typed-text history.
- [ ] Complete reactive UI settings against existing engine inputs; cover screen enumeration, unavailable source, sensitivity and decay persistence. Apply `inputs.decay_pct` (50–300, default 100) by multiplying only runtime ripple-layer density by `decay_pct / 100.0f`, after global effect parameters. Persist authored layers unscaled and test repeated rebuilds do not compound the multiplier.
- [ ] Test that custom layers cannot select uploaded playback, edits do not silently switch modes, and preset-specific uploaded controls retain current behavior. Run `ui_studio_effects_test|studio_scene_preparation_test|sdk_wire_test|sdk_client_test|effect_json_test|ui_reactive_test|ipc_reactive_test|ui_playback_test|playback_mode_test`; commit.

### Task 7 — Provide bounded preview of unsaved workspaces

**Depends on:** 3–4 and effect editing from 6.

**Files:** Create `PreviewSession.{h,cpp}`, `StudioViewportModel.{h,cpp}`, `tests/engine/preview_session_test.cpp`, `tests/ui/ui_studio_preview_test.cpp`; modify workspace command dispatch, bridge subscription, relevant CMake.

**Interfaces:** `PreviewSession::SetCandidate(candidate, draft_revision)`, `SetVisible(bool)`, `Sample(scene_time, input_snapshot) -> PreviewFrame`, `Stop()`. Use Task 6's scene/input preparation against a read-only topology snapshot, with each draft's own key positions. `PreviewFrame` contains draft revision, sequence, object IDs/color arrays and completeness metadata. It has no hardware sink.

- [ ] Test deterministic color/brightness equivalence with engine evaluation, unbound object preview, unchanged physical frame counters, stale candidate replacement, two-session bound, disabled inputs, hidden/display-off pause and disconnect cleanup.
- [ ] Implement section 3.4 scheduling and chunked snapshots. UI displays only complete matching sequences and clearly labels any reduced-detail preview.
- [ ] Preserve the applied-output preview from `frame_status`; verify transport-accepted colors and draft colors are never presented as interchangeable physical measurements.
- [ ] Run `preview_session_test|ui_studio_preview_test|scene_worker_test|e2e_responsiveness_test`; commit.

### Task 8 — Adapt the full 3D viewport into Layout

**Depends on:** 5 and 7. Requires Qt 6.8.3 Quick3D component.

**Files:** Adapt `StudioScene.qml` to `ui/studio/viewport/StudioViewport.qml`; copy/adapt source `ui/editor/{CameraController,SelectionController,TransformGizmo}.qml`, `ui/devices/*.qml`, `ui/materials/StudioEnvironment.qml` into the viewport subtree. Modify root/UI CMake, `UiSettings`, Layout page; add `tests/ui/qml_studio_viewport_test.cpp` and adapted `tests/ui/studio_qml/` tests.

**Interfaces:** The viewport receives explicit `WorkspaceController`, `ObjectEditorController`, `StudioViewportModel`, and camera/render preferences. Remove global `bridge`/`studioHost` assumptions and all plugin references.

- [ ] Provision the matching Quick3D component at implementation time; verify its CMake config and runtime imports. Add a capability-gated 3D module; a core/basic-UI development build can run without it, but the final full-Studio package must include it.
- [ ] Adapt `tst_devices.qml`, `tst_controllers.qml`, and source transform test assertions: device family dispatch, camera presets, projection, selection/marquee, move/rotate, snapping, paint versus orbit gestures, Escape cancellation, and persisted pose.
- [ ] Wire device geometry, emitter colors, linked copies, case ghosting, quality levels/bloom and reduced motion to the shared models. Keep ordinary controls responsive and the 2D view available.
- [ ] Run headless compile/model/input tests plus a real graphics-rendered QA pass for lighting/materials/picking and scaling. A software/headless smoke load alone does not establish 3D correctness.
- [ ] Run `qml_studio_viewport_test|ui_studio_preview_test|qml_smoke_test|qml_hardening`; commit after recording render evidence.

### Task 9 — Complete device-type authoring and portable workspace import/export

**Depends on:** 2–5; 8 for full 3D previews.

**Files:** Create `DevicePresetController.{h,cpp}`, `ui/studio/DevicePresetEditor.qml`, `WorkspaceImportDialog.qml`, `tests/ui/ui_studio_presets_test.cpp`, `tests/engine/workspace_bundle_test.cpp`; modify `WorkspaceService`, `WorkspaceStore`, `PresetBundle` as required, relevant CMake.

**Interfaces:** Controller actions `presetDocument(id)`, `validatePreset(candidate)`, `previewPreset(candidate)`, `savePresetType(candidate, replace)`, `deletePresetType(id)`, `saveInstanceAsVariant(...)`, `createTypeFromSelection(...)` use workspace resource jobs. Bundle flow uses existing `InspectBundle`, `ApplyImport`, and `ExportBundle` within a staging generation, never directly in the active resource directory.

- [ ] Adapt `tst_preset_editor.qml` and library tests for entity/child-type editing, transforms, appearance/asset references, zones, ring/grid/point layouts, validation/preview, variants, favorites, and type creation from selected root instances.
- [ ] Test deletion of an in-use/referenced type, reverting a local override to a packaged default, dependent reference remapping, invalid/missing/cyclic types, and bad LED layouts. A rejected edit preserves active content.
- [ ] Implement import inspection/conflict UI, original-byte preservation, rename/remap for differing same-ID types, bundle limits and path containment, atomic installation, export sanitation and missing-asset warnings.
- [ ] Ensure exports include personal look dependencies as well as device types/assets. Current `PresetBundle` is device-oriented; extend it to carry validated look files, or explicitly materialize the referenced look into inline layers for portable export and report that conversion. Choose inline materialization for this implementation, retaining the named preset as provenance.
- [ ] Test source-tree hashes unchanged, no live/input arming on import, interrupted export/import, identity sanitation, and reimport with nested dependency collisions. Run `ui_studio_presets_test|workspace_bundle_test|workspace_store_test|device_preset_test`; commit.

### Task 10 — Finish workspace file actions and external-change recovery

**Depends on:** 4 and 9.

**Files:** Complete `WorkspaceDraftStore`, `WorkspaceController`, `WorkspaceStore`, `ProfileLibrary`; create `tests/ui/ui_workspace_recovery_test.cpp`; update workspace action panels and `docs/studio-import.md`.

**Interfaces:** Implement Task 4's exposed file actions against Task 3 job outcomes. Use source ConfigStore test behavior as the specification; do not import its write-in-place source loading path.

- [ ] Port/adapt behavioral cases from source `tests/studio_config_store_test/main.cpp`: last-valid backup, future schema refusal, legacy migration retry after failed save, dirty autosave, recovery/discard, external replacement/deletion, and same-content watcher suppression.
- [ ] Complete Save Copy/Open/Reload/Restore Backup/Reset/Open Folder and UI library migration workflows. Display source/destination and conflict information before replacing a user's current draft.
- [ ] Test a failed save never marks the draft clean, focused text is included, cancelled dialogs preserve content, and a source file change is reported without auto-applying it to hardware.
- [ ] Run `ui_workspace_recovery_test|ui_workspace_test|workspace_store_test|qml_focused_save_test`; commit.

### Task 11 — Complete single-LED calibration and diagnostics parity

**Depends on:** 3–5 and 9; integrates with existing playback/lifecycle code.

**Files:** Create `CalibrationSession.{h,cpp}`, `CalibrationController.{h,cpp}`, `ui/studio/LedCalibrator.qml`, `tests/engine/calibration_session_test.cpp`, `tests/ui/ui_studio_calibration_test.cpp`; modify `EngineService`, `PlaybackController`, `DeliveryPipeline` only at their ownership/overlay seams, Devices/Layout panels, relevant CMake.

**Interfaces:** Workspace commands add `calibration_begin`, `calibration_show`, `calibration_end`, `calibration_status`; opaque session token bound to client, stable DeviceKey, topology generation and zone. `CalibrationSession` owns begin/step/end state and timeout; controller only presents state and forwards actions.

- [ ] Adapt `tst_calibrator.qml`: assign/verify without assignment, point placement, generated shapes, zone selection, typed indices, remove, apply, no-session end, and bounds rejection at UI/core/persistence.
- [ ] Test cancel while starting, rapid steps retaining only latest, client crash, timeout, scene/mode change, reconnect with reordered devices, display-off, and calibration during uploaded playback. Assert single producer and no stale restore.
- [ ] Implement temporary engine-owned output override and restoration through current validated playback state. Keep calibration edits in the draft; verifying an LED must not automatically persist a mapping.
- [ ] Map old diagnostic refresh/flash/measure controls to existing engine diagnostics and opt-in tools. Keep preview/transport/physical rates distinct; absent physical data remains unknown.
- [ ] Run `calibration_session_test|ui_studio_calibration_test|ipc_engine_service_test|e2e_lifecycle_test|e2e_mode_qual_test|e2e_noninterference_test`; commit. Physical verification is a separately authorized session.

### Task 12 — Package and qualify the complete imported workflow

**Depends on:** all preceding tasks.

**Files:** Modify `packaging/windows/stage-release.ps1`, launchers as needed for workspace root, packaging tests, `tests/ui/ui_qa_harness_test.cpp`, `docs/studio-import.md`, `docs/packaging/source-manifest.md`, provenance manifest; create `tests/engine/e2e_studio_workspace_test.cpp`.

**Interfaces:** Existing release layout and launcher contract; include Quick3D/QML dependencies and built-in presets/schemas/assets. Exclude the old Studio plugin DLL and all source runtime state.

- [ ] Add end-to-end fake-backend test: import representative legacy and v3 workspaces with custom type/look dependencies, edit objects/layers, save, close UI, restart UI/engine, verify authored data and resolved scene, export/reimport.
- [ ] Adapt source lifecycle assertions for repeated UI attachment, preview disposal, input ownership, and engine continuity. Use new process boundaries rather than the old QQuickWidget lifecycle.
- [ ] Run all CTest suites. Exercise full workflows through `tools/qa/qa.py` against a fake/offline engine: focused text Save, tree/library, layer/palette edits, 2D/3D selection, recovery, conflict, and calibration UI state.
- [ ] Verify scaled/narrow layouts, keyboard access, reduced motion, and quality tiers; inspect real 3D screenshots. Measure the existing p95 <100 ms local-control target under output plus Studio interaction and record conditions/results.
- [ ] Stage a package and launch it from a clean path without development Qt/source directories on PATH. Verify Quick3D imports/assets, writable workspace location, restart recovery, and package-manifest completeness.
- [ ] Reconcile every parity row with evidence and source hashes. Update outdated “deferred” provenance entries without rewriting historical import facts. Report unrun physical acceptance checks separately.
- [ ] Commit final qualification/package changes. Do not publish, install over the preserved project, or claim measured physical FPS from offline tests.

## 7. Verification commands and completion criteria

Run from `C:\Users\daniel\tools\openRGB2` in PowerShell. Use an isolated build tree when current build artifacts belong to other work.

```powershell
$cmakeExe = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctestExe = Join-Path (Split-Path $cmakeExe) 'ctest.exe'
& $cmakeExe -S . -B build-studio -G 'Visual Studio 17 2022' -A x64 -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
& $cmakeExe --build build-studio --config Release
# Replace the regex with the task's named suites; require all selected tests to pass.
& $ctestExe --test-dir build-studio -C Release --output-on-failure -R 'workspace_candidate_test|workspace_store_test'
# Existing label suites, e.g. Task 1:
& $ctestExe --test-dir build-studio -C Release --output-on-failure -L 'core|engine|sdk'
# Full final regression gate:
& $ctestExe --test-dir build-studio -C Release --output-on-failure
```

For a new target, register it in the appropriate CMake test directory with `core`, `engine`, or `ui` label before invoking its test name. Imported QML cases use the installed Qt test facilities through CTest; do not use source batch scripts that contain obsolete verdict workarounds. An empty test selection is not a pass.

Completion requires all feature-matrix rows implemented or an explicit user-approved scope change; compact data survives save/restart/export; migrated hardware authority remains explicit; existing uploaded/live behavior stays intact; all automated suites pass; and the staged full-feature package runs without the source checkout. Physical acceptance evidence remains a separate opt-in qualification and must be stated accurately.

## 8. Planning status and recommended execution sequence

This planning task performed read-only source inspection and comparisons, plus a focused dependency graph in ignored `.superpowers/dls-import-review/`. It did not modify the source project, build either application, run CTest, capture inputs, or touch RGB hardware.

Recommended sequence: **1 → 2 → 3 → 4**, then **5 → 6 → 7 → 8 → 9 → 10 → 11 → 12**. The first four tasks establish the data path and prevent loss of authoring information. The next tasks make progressively usable editor features available; the final task validates the complete workflow and release package.

This is a substantial editor migration, not a directory copy. Keep changes in the task-sized commits above, validate each deliverable before advancing, and retain the existing simple lighting workflow throughout.
