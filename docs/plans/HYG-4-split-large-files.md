# HYG-4: Splitting the 2–3k-line files (proposed seams)

Status: proposed · Size: L (8 independent S/M moves) · Source: audit 2026-11-28 Hygiene ("Large files worth splitting")

## Goal

The eight largest first-party files get split along seams they already have, as
pure moves with no behaviour change. Reviews, merges and agent context then get
cheaper. This plan proposes the seams only. Nothing is to be split until Daniel
picks which ones are worth it.

## Current state

| File | Lines | Built by | Main tests |
| --- | --- | --- | --- |
| `src/engine/WorkspaceService.cpp` | 3277 | `openrgb2_engine` (`src/engine/CMakeLists.txt:44-45`) | `workspace_store_test`, `workspace_bundle_test`, `ipc_workspace_test`, `ui_workspace_import_test` |
| `src/ipc/EngineService.cpp` | 2931 | `openrgb2_ipc` (`src/ipc/CMakeLists.txt:30`) | `ipc_engine_service_test`, `ipc_reactive_test`, `e2e_*`, `device_mapping_test` |
| `src/ui/WorkspaceController.cpp` | 2821 | `qt_add_qml_module(openrgb2_ui …)` SOURCES (`ui/CMakeLists.txt:115`) | `ui_workspace*_test`, `qml_workspace_save_test` |
| `src/core/editor/EditorController.cpp` | 2693 | `openrgb2_studio_core` (`src/core/CMakeLists.txt:34`) | `studio_editor_test` |
| `src/engine/WorkspaceStore.cpp` | 2703 | `openrgb2_engine` | `workspace_store_test`, `workspace_bundle_test` |
| `src/ui/LayoutController.cpp` | 2071 | `openrgb2_ui` (`ui/CMakeLists.txt:131`) | `ui_layout_test`, `qml_layout_drag_test` |
| `ui/pages/LightingPage.qml` | 2189 | `QML_FILES` (`ui/CMakeLists.txt:84`) | `qml_smoke_test`, `qml_focused_save_test` |
| `ui/pages/LayoutPage.qml` | 1962 | `QML_FILES` (`ui/CMakeLists.txt:83`) | `qml_smoke_test`, `qml_layout_drag_test` |

There is precedent. `src/ipc/WorkspaceCommands.cpp` (1954 lines) is an earlier
split out of `EngineService`.

The tests link the libraries, so a C++ split needs no test CMake edits. A new
QML file must be added to `QML_FILES`; QTP0001 and QTP0004 are NEW
(`ui/CMakeLists.txt:29-32`).

## Design: proposed seams, best ratio of gain to risk first

1. **`WorkspaceService.cpp`:**
   - (a) `WorkspaceBundleCodec.cpp`: the anonymous-namespace
     `IsBundleRelPath`/`Base64`/`UnpackBundleFiles`/`PackBundleDir`
     (`:200-479`), which have no member access.
   - (b) `WorkspaceJobs.cpp`: the job queue (`Enqueue`, `Status`, `Cancel`,
     `WorkerMain`, `:1320-1667`). It touches only `jobs_*` and `surface_`.
   - (c) `WorkspaceImport.cpp`: `CommitJson`, `ImportStagedJson`, and the
     inspect functions (`:2283-3194`).
   - (d) Optionally, break `RunJob` (`:1668-2282`, about 615 lines) into one
     private method per verb. That is a refactor, not a move, so do it last.
2. **`EngineService.cpp`:**
   - (a) `EngineJson.{h,cpp}`: the serializers and wire names (`:25-288`).
     Dedupe `ColorHex` against `src/engine/ProfileStore.cpp:76` in a separate
     commit.
   - (b) `EngineServiceLayout.cpp`: the layout handlers and their helpers
     (`:1796-2706`), which touch only `deps_`.
   - (c) Leave `Dispatch` in place, but extract `SetPlaybackMode`
     (`:1453-1606`) into a member.
3. **`WorkspaceStore.cpp`:**
   - First add an internal header `WorkspaceStoreInternal.h` for `Marker`,
     `Manifest`, the constants and the atomic-IO helpers (`:34-556`).
   - Then `WorkspaceStoreResources.cpp`: `InstallEffectResource`, the resource
     gates, `InstallDeviceTypeResource` and `RemoveResource` (`:2015-2703`).
4. **`WorkspaceController.cpp`:** the existing section banners are the seams.
   - (a) `WorkspaceViewportPrefs.cpp` (`:2650-2821`, `session_` only).
   - (b) `WorkspaceTransfers.cpp`: chunked transfers (`:2253-2622`, the `dl_*`
     and `ul_` members).
   - (c) `WorkspaceImportExport.cpp` (`:1356-1705`).

   All stay members of one class, so one header and several `.cpp` files. A
   member-per-file split keeps `Q_OBJECT` in one place.
5. **`EditorController.cpp`:** `EditorLayers.cpp` for the effect-layer half
   (`:1618-2668`), with scene edits left in place. Note the coupling:
   `BeginTransform` cancels a live layer gesture (`:289-298`), which stays a
   cross-file member call.
6. **`LayoutController.cpp`:** `LayoutIdentify.cpp` (`:1660-1762`, its own state)
   and `LayoutWorkspaceAdapters.cpp` (`:128-640`).
7. **`LayoutPage.qml`:** extract the inspector sections (SELECTION, ADD DEVICES,
   ORIENTATION, MIRROR/LINK, BINDING, CALIBRATION, `:1044-1639`) into
   `ui/pages/layout/*.qml`. They read only `root.ctlUp`, `root.editable` and
   similar flags, so pass those as properties. Leave the 2D canvas (`:614-1010`)
   alone, because `computeView` depends on it.
8. **`LightingPage.qml`:**
   - Extract REACTIVE INPUTS (`:1380-1757`, self-contained) first, then DRAFT
     PREVIEW (`:1758-1820`, no `root.` references).
   - Defer PROFILES and the save bar. `nameField` is shared across them
     (`:83`, `:123`, `:1899`, `:2063-2180`), and the PALETTE ids are read by
     `root` (`:92-94`). Untangling those is a design change.

**Rules for every split:**
- one file per commit
- a pure move: diff `git show --color-moved` clean
- no renames in the same commit
- full CTest green
- for QML, `qml_smoke_test` plus a QA-harness click-through of the page
- run `graphify update .` after each merge, per `CLAUDE.md`

**Rejected alternatives:**
- Splitting by line count: it cuts through shared state.
- Splitting classes into new classes now: a bigger change with a behaviour risk.
  Moves first; class extraction only where a moved unit then clearly stands
  alone, for example the job queue.

## Steps

1. Daniel picks the scope. The recommendation is 1a, 1b, 2a, 2b, 3, 4a, 7 and
   8 (reactive inputs).
2. Do one move per commit, in the order above. Test: the full CTest suite, plus
   `qml_smoke_test` for the QML moves.
3. After the moves, re-measure line counts and update this plan's table. Test:
   none.

## Tests

No new tests. The existing suites listed in the table are the safety net. For
QML moves, add a `qml_smoke_test` assertion that the extracted component loads.

## Risks and open questions

- Open branches touching these files will conflict, so schedule the moves when
  few branches are open. Several audit fixes (I-, U- and Q-series) touch
  `WorkspaceController.cpp` and `LightingPage.qml`.
- In QML, moving a section to its own file changes id scope. Any hidden `root.`
  or sibling-id reference breaks at runtime, not at compile time. Grep each
  extracted block for ids defined outside it before moving it.
- Is the gain worth the churn for files that rarely change? Check `git log
  --since=2026-09-01 --oneline -- <file> | wc -l` per file and skip the cold
  ones. Daniel's call.
