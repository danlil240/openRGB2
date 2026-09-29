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

- **Qt Quick3D is required for Task 8 (3D viewport).** Qt 6.8.3
  Quick3D is **absent** from the pinned kit at
  `C:\Qt\6.8.3\msvc2022_64` (no `Qt6Quick3D` CMake config, DLL, or
  QML directory). It is an additional component of the pinned kit,
  not a toolkit upgrade — install `qtquick3d` for Qt 6.8.3
  msvc2022_64 before Task 8. Its absence does not disable any
  existing UI target and does not block Tasks 2–7.

## Parity status (high level)

| Area | Status |
| --- | --- |
| Shared model core (`openrgb2_studio_core`) | Consolidated; includes `PresetBundle`, `DefaultDesk`, `EditorController`, `TransformCommands` |
| Scene/effect evaluation, schemas, v3 compact workspace model | Imported core; exercised by `core` label suites |
| Reactive input capture (WASAPI, screen, keyboard, power) | Qt-free Windows implementations in `src/platform/windows` |
| Compact workspace durability (engine `WorkspaceStore`) | Not started (Task 2) |
| Workspace IPC (revisions, transfers, jobs, preview) | Not started (Tasks 3, 7) |
| Shared UI draft/edit session, recovery | Not started (Task 4) |
| Scene/object editing + device library panels | Not started (Task 5) |
| Layer/palette/personal-look editing | Not started (Task 6) |
| 3D viewport (`StudioScene.qml` → `StudioViewport`) | Blocked on Quick3D prerequisite (Task 8) |
| Device-type authoring + portable bundles | Not started (Task 9) |
| Workspace file actions + external-change recovery | Not started (Task 10) |
| Single-LED calibration + diagnostics parity | Not started (Task 11) |
| Packaging/notices for the imported workflow | Not started (Task 12) |
