# Import Provenance Manifest — Milestone 0

Import date: 2026-09-26. Source: the `openrgb-unified` working tree at
`C:\Users\<user>\tools\openrgb-unified` (left untouched by the import).

The origin commit IDs are **not** complete snapshots of the imported
functionality: both origin trees carried uncommitted tracked modifications and
untracked source files. This manifest records the exact state imported into
this repository. File-level SHA-256 hashes for everything that differs from an
origin HEAD are in [`import-hashes.txt`](import-hashes.txt).

## Origin repositories

| Component | Repository | Revision at import |
| --- | --- | --- |
| Parent project (plugin, tests) | `https://github.com/danlil240/openrgb-unified.git` | `386ad374d44770365bf90c4f1fb08c2522e3a4b6` |
| OpenRGB submodule (backend) | `https://github.com/danlil240/OpenRGB` (fork of `CalcProgrammer1/OpenRGB`) | `53a477eab8d14b0bc49aeb40e3d569cbcb16a377`, branch `lianli-wireless` |

Reference (not imported): Lian Li Linux `ebcf68ef02a39463e8295bb40ce3c5751622c496`,
stock OpenRGB `0f8f2dccc46576f0a9dac6b70adcbf49901c6b73`.

## Import method

- `vendor/OpenRGB/` — every file tracked by the OpenRGB submodule
  (`git ls-files`, 2,393 files) copied **from the working tree**, which carries
  the 8 uncommitted tracked modifications listed in `import-hashes.txt`, plus
  the 2 untracked effects files (`LianLiWirelessEffects.cpp/.h`). Build
  artifacts (`OpenRGB Windows 64-bit/`, `_intermediate_*`, `release/`,
  `debug/`, `OpenRGB-baseline-backup/`, generated `Makefile*`) were excluded by
  construction because only tracked files plus the listed untracked sources
  were copied.
- `src/core/` — the Qt-free scene/effects/resolver core copied from the
  parent working tree (uncommitted modifications included):
  `scene/` (complete), `effects/` (complete), `presets/` (complete, including
  the bundled device/effect JSON library), `schemas/`, `inputs/`
  (InputBus/OnsetDetect/KeyMap/KeyTranslate/VkDefs — the Qt-free input core),
  `config/` (StudioConfig/ConfigMigration), `editor/`
  (EditorController/TransformCommands).
- `tests/core/` — the Qt-free offline suites from the parent `tests/`
  (`studio_scene_test`, `studio_config_test`, `device_preset_test`,
  `studio_editor_test`, `effect_json_test`, `key_translate_test`) plus
  `tests/fixtures/`. Adaptations for this repository are listed below.
- `tests/backend/` — the offline Lian Li wireless protocol harness
  (`lianli_protocol_test.cpp`) from the parent `tests/`, adapted to CMake
  (source list mirrors `tests/build-and-run.ps1`).

## Deliberate adaptations (imported code changes)

Recorded here so provenance stays auditable; everything else is byte-identical
to the origin working tree.

| File | Change | Reason |
| --- | --- | --- |
| `tests/core/device_preset_test.cpp` | `PackagedPresetDir()` honors a `DLS_PRESET_DIR` env override and gained the candidate `../../src/core/presets/devices` | packaged presets moved from `plugins/DesktopLightingStudio/presets/devices` to `src/core/presets/devices` |
| `tests/core/key_translate_test.cpp` | includes rewritten from `../plugins/DesktopLightingStudio/inputs/…` to `inputs/…` | include layout changed under `src/core/` |

## Removed historical workarounds

The origin `tests/build-studio-tests.bat` contained a Smart App Control
per-path verdict workaround: binaries were copied onto a previously evaluated
"trampoline" path (`out\studio_scene_test2.exe`) and the copy was executed when
SAC denied the fresh binary. That workaround is **not** carried over. Tests in
this repository run through CTest as ordinary locally built executables; no
path-based verdict bypass, exclusion, or security-setting change is used.

## Deferred to later milestones (still only in the origin tree)

The origin tree remains the source of record for these until their milestones:

- `plugins/DesktopLightingStudio/output/` (ControllerAdapter, OutputRouter,
  DisplayPowerMonitor — engine milestone 3)
- `plugins/DesktopLightingStudio/{plugin,ui}/` and Qt-dependent editor models
  (SceneObjectModel, EffectLayerModel, PresetListModel) and `config/ConfigStore`
  (UI milestone 4)
- Platform reactive inputs (AudioLoopback, KeyHook, ScreenSampler) (milestone 4)
- Qt-dependent test suites (`editor_qml`, `studio_lifecycle_test`,
  `studio_transform_test`, `studio_config_store_test`, `quickwidget_probe`)
- `LED_CALIBRATION_E2E_REPORT_2026-09-18.md` (referenced by the plan's L11)

## Licenses

| Component | License | Notes |
| --- | --- | --- |
| `vendor/OpenRGB/` | GPL-2.0-or-later (OpenRGB upstream) | see `vendor/OpenRGB/LICENSE` |
| OpenRGB `dependencies/tinyuz`, `dependencies/HDiffPatch` | MIT (housisong) | compression stack used by uploaded-frame payloads |
| `src/core/`, `tests/` (Desktop Lighting Studio code) | GPL-2.0-or-later | per file headers |
| New files in this repository (CMake, manifest) | GPL-2.0-or-later | project default per implementation plan |

The smoke test's installed L-Connect `yuz.dll` was never copied; compression
builds from the MIT-licensed sources above.
