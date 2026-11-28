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
  Correction: the six upstream recipes `scripts/build-appimage.sh`,
  `build-macos.sh`, `build-msi.ps1`, `build-msi.sh`,
  `build-package-files.sh` and `build-supported-devices-md.sh` were
  tracked upstream but silently dropped by the vendored `.gitignore`
  rule `*build-*`; they were re-added byte-identical to the pinned
  commit (`!scripts/build-*` negation added). Only
  `scripts/build-windows.bat` is used by this repository.
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

The origin tree remains the source of record for these until their milestones.
Per-file hashes and planned destinations for the Desktop Lighting Studio
import are in [`dls-ui-import-hashes.txt`](dls-ui-import-hashes.txt).

- `plugins/DesktopLightingStudio/output/` (ControllerAdapter, OutputRouter,
  DisplayPowerMonitor — engine milestone 3; behavioral reference only, the
  delivery pipeline + SDK client replace them)
- `plugins/DesktopLightingStudio/{plugin,ui}/` and Qt-dependent editor models
  (slated for `src/ui/studio/`; `config/ConfigStore` behavior reused by the
  engine `WorkspaceStore` — the QObject/QSaveFile/file-watcher implementation
  is not carried over). **Task 5 landed**: `SceneObjectModel`,
  `PresetListModel`, `ObjectEditorController` (the SceneBridge editor half on
  the shared `WorkspaceEditSession`), and the `DeviceTree` / `DeviceLibrary` /
  `TransformInspector` QML surfaces now live in `src/ui/studio/` +
  `ui/studio/`, covered by `tests/ui/ui_studio_objects_test.cpp`.
  Tasks 6, 8 and 9 landed too: `EffectLayerModel`, `EffectEditorController`
  and the layer/palette/look panels (`LayerStack`, `PaletteEditor`,
  `LookShelf`, `EffectEditor`, `EffectInspector`; Task 6, `dd3d0e8`), the
  Quick3D Layout viewport (`ui/studio/viewport/StudioViewport.qml`; Task 8,
  `a12e420`), and device-type authoring (`DevicePresetController`,
  `DevicePresetEditor`; Task 9, `86c2e9d`).
- ~~Platform reactive inputs (AudioLoopback, KeyHook, ScreenSampler)
  (milestone 4)~~ — **done**: Windows WASAPI loopback, WH_KEYBOARD_LL key
  hook, and GDI screen sampling now exist as Qt-free implementations in
  `src/platform/windows/` (`WinAudioLoopback`, `WinKeyHook`,
  `WinScreenSampler`, `WinReactiveInputs`). The remaining macOS source
  (`AudioLoopback_mac.mm`) is out of scope.
- Qt-dependent test suites (`editor_qml`, `studio_lifecycle_test`,
  `studio_transform_test`, `studio_config_store_test`, `quickwidget_probe`)
- `LED_CALIBRATION_E2E_REPORT_2026-09-18.md` (referenced by the plan's L11)

## Changes to `vendor/OpenRGB/` after the import

The vendored tree is no longer byte-identical to the import above. Changes
made in this repository are recorded here. This record starts with
`9c866ca`; the earlier post-import commits to `vendor/OpenRGB/` are not
itemized here yet (`git log -- vendor/OpenRGB` lists them).

| Commit | Files | Change | Reason |
| --- | --- | --- | --- |
| `9c866ca` (2026-11-28) | `Controllers/LogitechController/LogitechHIDPP20Controller/LogitechHIDPP20Controller.{cpp,h}` | `SetPerKey8080` sleeps `HIDPP20_8080_FRAME_GAP_MS` (2 ms) after every 0x8080 SetKeyColors frame | a G512 RGB (0xC33C, US) dropped the tail keys of a frame when the next one arrived too soon (F5–F12, PrtSc, `0` and `-` kept stale colours). The 2 ms value was found by trial on one G512. The pacing is open-loop: ACKs are not read, and every update repaints all keys (audit P-7) |
| `be1bb03` (2026-11-28, P-12 step 1, branch `perf/p-12-lianli-poll-cost`) | `Controllers/LianLiWirelessController/LianLiWirelessRuntime.{cpp,h}`, `LianLiWirelessDiagnostics.{cpp,h}` | `WirelessRuntime::SetDesired` floors the next discovery poll at `last_poll_ms + min_poll_gap_ms` (new config field, 0 = `poll_ms`); `last_poll_ms` is set when `PollAndMerge` calls `PollDiscovery`; new `discovery_clamped` counter and `poll_clamp` trace row | a group unseen for longer than `max_discovery_defer_ms` (1 s) but not yet Lost (8 s) made every live frame a blocking discovery poll (63 polls/s in the offline test, bound 5). Plan `docs/plans/P-12-lianli-discovery-poll-cost.md` |
| `d8c79bf` (2026-11-28, P-12 step 2, branch `perf/p-12-lianli-poll-cost`) | `Controllers/LianLiWirelessController/LianLiWirelessService.{cpp,h}`, `LianLiWirelessRuntime.h`, `LianLiWirelessDiagnostics.{cpp,h}` | the service's `ObservingLink` answers `PollDiscovery` from the last successful reply when it is younger than `share_window_ms` (new config field, 50 ms; 0 disables) and on the same `ConnectionGeneration`; a failed read or any failed forwarded operation drops it; shared replies are not re-added to the `SightingAccumulator`; new `discovery_shared` counter and `poll_shared` trace row | every fan group ran its own discovery read on the one shared RX dongle, each costing 30-130 ms of link time; the query carries no group or channel field (`query[1] = 1`), so one reply answers every group |

| `6a11027` (2026-11-28, P-7 step 2, branch `perf/p-7-logitech-8080-delta`) | `Controllers/LogitechController/LogitechHIDPP20Controller/LogitechPerKey8080Plan.{cpp,h}` (new), `LogitechHIDPP20Controller.{cpp,h}`, `RGBController_LogitechHIDPP20.cpp` | the 0x8080 grouping, (0,0) skip, dim scaling, G410 keyType-0x40 filter, 14-key chunking and fn3 payload encoding move into `LogitechPerKey8080Plan` (Qt-free, hidapi-free); `SetPerKey8080` takes planned frames; `HIDPP20_8080_KEYS_PER_FRAME` moves to the planner header. No wire change (byte-identical, test-pinned) | testability: `tests/backend/logitech_8080_plan_test.cpp`. Plan `docs/plans/P-7-logitech-8080-closed-loop-pacing.md` |
| `477a2f4` (2026-11-28, P-7 step 3, same branch) | `LogitechPerKey8080Plan.{cpp,h}`, `LogitechHIDPP20Controller.{cpp,h}`, `RGBController_LogitechHIDPP20.{cpp,h}` | the 0x8080 path keeps `sent_colors` (dimmed wire colors) and sends only changed keys; full repaint on the first frame, after every `sent_colors.clear()` site, after a re-init, for `HIDPP20_UNCOMMITTED` keys and every `HIDPP20_8080_FULL_REFRESH_MS` (2000 ms placeholder); an unchanged frame sends nothing. `SetPerKey8080` returns per-frame write success, `PerKeyCommit8080` the FlushLEDS ACK; a failed write or unacked commit leaves its keys `HIDPP20_UNCOMMITTED`. The 2 ms frame gap is unchanged | every frame repainted every key behind a 2 ms sleep per 14-key frame, capping the G512 near 40-50 Hz (audit P-7). fn3 ACK pacing (plan step 4) waits for a G512 measurement |
| `323c0a6` (2026-11-28, P-7 step 5, same branch) | `LogitechPerKey8080Plan.{cpp,h}`, `LogitechHIDPP20Controller.{cpp,h}`, `RGBController_LogitechHIDPP20.cpp` | one `LOG_DEBUG` line per `HIDPP20_8080_SUMMARY_S` (10 s): updates and rate, full / idle, frames and keys per frame, write and FlushLEDS failures, mean / max wall time per update | shows the achieved update rate in a hardware run |

Deploying a change here needs a backend rebuild
(`vendor\OpenRGB\scripts\build-windows.bat`). Since P-7 step 6 (`39201f1`)
the recipe step in `packaging\windows\build-and-deploy.ps1` and
`scripts\build-all-and-install.ps1` writes `openrgb2-backend.stamp` next to
`OpenRGB.exe` (a hash of `vendor/OpenRGB` + `src/core/playback`), and
`build-and-deploy.ps1` refuses a backend whose stamp is missing or does not
match ("backend is stale") unless `-BuildBackend` (rebuild) or
`-AllowStaleBackend` (stage anyway, with a warning) is given. A backend built
before the stamp existed has none, so the first run after this change needs
`-BuildBackend`.

## Licenses

| Component | License | Notes |
| --- | --- | --- |
| `vendor/OpenRGB/` | GPL-2.0-or-later (OpenRGB upstream) | see `vendor/OpenRGB/LICENSE` |
| OpenRGB `dependencies/tinyuz`, `dependencies/HDiffPatch` | MIT (housisong) | compression stack used by uploaded-frame payloads |
| `src/core/`, `tests/` (Desktop Lighting Studio code) | GPL-2.0-or-later | per file headers |
| New files in this repository (CMake, manifest) | GPL-2.0-or-later | project default per implementation plan |

The smoke test's installed L-Connect `yuz.dll` was never copied; compression
builds from the MIT-licensed sources above.
