# U-14: Review plan for the unreviewed UI parts

Status: proposed · Size: M · Source: audit 2026-11-28 U-14

## Goal

The 2026-11-28 audit skipped these parts:
- `ObjectEditorController`
- `DevicePresetController`
- `CalibrationController`
- the remaining models
- `UiSettings`

This plan sets the order, the questions to ask of each, and the evidence each
answer needs. Findings go into the same severity and confidence format as the
audit, and every confirmed bug gets a failing test first.

## Current state

- **Threading is simple.**
  - None of these classes starts a thread or a timer thread.
  - `EngineBridge` runs its worker `std::thread` (`EngineBridge.cpp:61`) and
    posts results back with a queued `invokeMethod` (`:105`), so every
    reply, event or connection slot runs on the GUI thread.
  - All of them are stack objects in `src/ui/main.cpp:332-401`, and the QML
    engine is declared after them (`:406`), so QML dies first.
- **One ordering gap is already visible.** `ui_prefs` (`main.cpp:401`) is
  declared after `scene_edit` (`:347`) but handed to it as a raw pointer
  (`:404`, `SceneEditController.h:305`), so it is destroyed first.
- **The classes:**

| Class | Lines (.h/.cpp) | IPC verbs | Existing tests |
| --- | --- | --- | --- |
| `ObjectEditorController` (`src/ui/studio/`) | 332 / 1472 | `workspaceCommand("catalog")` (`.cpp:1072`) | `ui_studio_objects_test`, `ui_studio_presets_test`, `qml_studio_viewport_test`, `qml_studio_qml` |
| `DevicePresetController` | 214 / 1138 | `resource_get` (`:222`), `resource_install` (`:408`), `resource_remove` (`:562-568`) | `ui_studio_presets_test` (offline feeds only) |
| `CalibrationController` | 153 / 353 | `calibration_begin`/`_show`/`_end` (`:183`, `:211`, `:226`) | `ui_studio_calibration_test` (offline) |
| `UiSettings` (`src/ui/`) | 177 / 358 | none (`QSettings`, registry) | `ui_hardening_test` (around `:725-800`) |
| `SceneObjectModel` | 161 / 673 | none | `ui_studio_objects_test` |
| `PresetListModel` | 118 / 264 | none | `ui_studio_objects_test` |
| `EffectLayerModel` | 91 / 225 | none | `ui_studio_effects_test` |
| `FrameFeedModel` | 77 / 132 | none | `ui_bridge_test` |
| `StudioViewportModel` | 196 / 520 | none | `ui_studio_preview_test` |
| `LayoutEditModel` (Qt-free) | 216 / 824 | none | `ui_layout_test` |
| `SceneEditModel` (Qt-free) | 193 / 539 | none | `ui_scene_edit_test` |

- **Test harnesses.** The reviewed controllers (`WorkspaceController`,
  `LayoutController`) are tested over the in-process pipe
  (`tests/ui/test_pipe.h`, scripted `ServerPeer` or the real `IpcServer`). The
  three studio controllers are tested only through `Feed*ForTesting` hooks, so
  nothing covers reply/event ordering or disconnects.

## Design

Review in risk order. A part comes first when a mistake there mutates user data
or hardware state, then by how much engine traffic the part handles.

1. **`DevicePresetController`** (it installs or removes resources and commits a
   `Retype`). Questions:
   - Can a terminal job event arrive before the reply that carries its job ID?
     Events are dropped when `job != pending_*_job_` (`.cpp:1081`, `:1089`), and
     the controller then stays busy. Check whether the engine guarantees reply
     before event (`src/engine/WorkspaceService.cpp` job queue at about
     `:1320-1667`).
   - `Retype` commits against whichever draft is current when the job finishes
     (`:868-876`). Is that the workspace the install started from?
   - `pending_install_iid_` is never cleared (`:1085`).
   - The header says `post_op` is `"retype:<iid>"`, but the code compares it
     with `"retype"` (`:868`).
   - `EffectiveRegistry()` copies the whole registry on every call (`:126-141`).
2. **`CalibrationController`** (it drives LEDs on the real device). Questions:
   - Every reply that has `active` and `owner` keys is applied, whatever request
     it answers (`:331`).
   - `starting_` is stuck if an OK begin reply has no status body (`:323-334`).
   - Show and end replies are not correlated with their requests.
   - Is the session ended when the UI disconnects or closes? Check the engine
     side (`calibration_session_test`) for an owner timeout.
   - Unchecked `toLongLong` to `quint64` (`:279-282`).
3. **`ObjectEditorController`** (largest, and has F02 history). Questions:
   - It loads presets from paths relative to the working directory, in
     production (`.cpp:64-74`).
   - It holds raw pointers into `Session().Working()` (`:104-107`, `:463`)
     across document replacement.
   - `undo()` and `redo()` reset the model twice (`:1329-1331`).
   - `RefreshAll()` runs synchronously in `RegisterPresetForSession` and
     `LoadPresetDirectory` (`:1444-1468`). Is either reachable from a QML input
     handler? That is the F02 crash shape.
   - Catalog fields are read without type checks (`:1093`).
   - Re-check C-6 (gesture versus undo, `:1324`, `:1339`) after the Wave
     changes.
4. **The models.** Check every `begin*/end*` pairing and its index arithmetic:
   - `EffectLayerModel.cpp:170-211`, the prefix/suffix diff
   - `FrameFeedModel.cpp:98-113`
   - `PresetListModel.cpp:255`: `extra_.at()` without the check that `:100` has
   - `StudioViewportModel.cpp:469-480`: chunk count and `partials_` are
     unbounded. This joins U-13 (`sequence_`).
   - `SceneEditModel.cpp:428`: `working_["effect"]["layers"]` mutates the
     document and throws on a non-object

   Run each model under `QAbstractItemModelTester` (QtTest ships it) in its
   existing suite.
5. **`UiSettings`** (low impact):
   - a base64 key containing `/` becomes a group separator in QSettings
     (`:308-315`)
   - palette setters accept NaN and emit when nothing changed (`:161-190`)
   - `saveGeometry` does no validation (`:144`)
   - `const_cast` in getters (`:62`)
   - never calls `sync()`
   - the `ui_prefs` destruction order in `main.cpp`

**Method for each part.** Read the class in full. Trace each IPC verb to its
engine handler. Then write findings as `U-14.n` entries in a new
`docs/reviews/U-14-ui-review.md`: the brief forbids editing the audit doc, and
the coordinator can fold it in. Every finding marked **checked** gets a failing
test before any fix.

**Rejected alternatives:**
- Reviewing in file order: it spends the first day on `UiSettings`.
- Fuzzing the QML first: the controller contracts are cheaper to read than to
  fuzz.

## Steps

1. Move `DevicePresetController` and `CalibrationController` tests onto the
   `test_pipe.h` harness: a scripted engine with reply/event reordering and a
   disconnect. Test: event-before-reply, ERROR reply, and disconnect mid-job.
2. Review `DevicePresetController` and record findings.
3. Review `CalibrationController` and record findings.
4. Review `ObjectEditorController` and record findings. Test: an F02-shaped
   test that calls `RegisterPresetForSession` from inside a delegate's
   `onClicked` in `qml_studio_qml` (offscreen).
5. Add `QAbstractItemModelTester` to the model suites and review the index math.
6. Review `UiSettings` and add a round-trip test for a key containing `/`.
7. Hand the findings file to the coordinator, with the suggested ranking.

## Tests

Label `ui` throughout, with `QT_QPA_PLATFORM=offscreen` and Qt `bin` on `PATH`.
No hardware is needed, except a manual calibration run: begin, show, kill the
UI, then confirm the LEDs return to the scene within the engine's owner timeout.

## Risks and open questions

- If the engine does not order reply before event, the fix belongs in the
  engine. That would make it an I-series item, not a UI one.
- Should `ObjectEditorController` load presets from the working directory at all
  in a packaged build? Daniel's call. The recommendation is to resolve paths
  from the executable directory only.
- `NDEBUG` is set on every UI suite, so `assert`s never fire under test. Keep
  that in mind when reading the code's `assert` guards.
