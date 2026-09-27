# Task 4.4 — Layout / spatial model view — DONE

- Commit: `e96e140` on `milestone-4` (17 files, +4841/-74).
- Engine: six LAYOUT_COMMAND verbs live — `identify` = transient bounded
  blink overlay in DeviceMapper (Unavailable while an uploaded loop owns
  the group); mutations edit the applied SceneDocument through the one
  apply path. `set_position` added; ipc-protocol.md §5 rewritten.
- UI: LayoutEditModel/LayoutController — single-flight ops, whole-
  document SET_SCENE undo/redo (bounded 32-entry LIFO); LayoutPage
  canvas: drag-arrange, yaw, linked copies, bindings, calibration,
  identify.
- Tests: ui_layout_test (152 checks) + engine service; 36/36 ctest pass.
- Concern: unlink doesn't restore emitters — whole-doc undo is inverse.
