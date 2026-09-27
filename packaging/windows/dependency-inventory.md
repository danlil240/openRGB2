# Dependency Inventory — OpenRGB2 Windows package

Task 6.1 deliverable. Every binary shipped by
`packaging/windows/stage-release.ps1`, its DLL dependencies, and its
provenance. Per-file SHA-256 hashes for a concrete staged tree are in
`<package>/manifest.json` (generated at staging time); this document is
the human-readable inventory and dependency rationale.

Toolchain: Qt 6.8.3 (`C:\Qt\6.8.3\msvc2022_64`), MSVC 2022 BuildTools
v143. `dumpbin /dependents` output below is from the Release build.

## Package layout

```
<OutDir>/                              (build/package/OpenRGB2-<ver>-win64)
  openrgb2-ui.exe                      Qt Quick UI process
  openrgb2-engine.exe                  Qt-free lighting engine
  openrgb2-ctl.exe                     Qt-free engine IPC driver
  Qt6*.dll, qml/, platforms/,          windeployqt output for the UI
    imageformats/, iconengines/
  vcruntime140*.dll, msvcp140*.dll     MSVC CRT (app-local)
  presets/devices, presets/effects     bundled device/effect JSON library
  profiles/                            committed topology profiles
  backend/
    OpenRGB.exe                        pinned vendored backend
    Qt6*.dll, platforms/, styles/,     windeployqt output (recipe parity)
      imageformats/, iconengines/
    PawnIOLib.dll, *.bin               PawnIO library + modules
    hidapi-hotplug.dll, libusb-1.0.dll hardware access
    dxcompiler.dll, dxil.dll           DirectX shader compiler (PawnIO dep)
    vcruntime140*.dll, msvcp140*.dll   MSVC CRT (app-local)
  licenses/                            full license texts
  NOTICES.txt                          license/provenance manifest
  manifest.json                        per-file SHA-256 + origin + build manifest
```

## 1. Application binaries (this repository — GPL-2.0-or-later)

| Binary | Built by | Direct DLL deps (non-OS) |
| --- | --- | --- |
| `openrgb2-ui.exe` | `cmake --build build --config Release`, output `build/src/ui/Release/` | Qt6Core, Qt6Gui, Qt6Qml, Qt6QuickControls2 + MSVC CRT. QML imports (QtQuick, QtQuick.Controls, QtQuick.Dialogs, QtQuick.Layouts, QtQuick.Window) resolved by `windeployqt --qmldir ui/` — pulls the QtQuick/Controls/Dialogs/Layouts QML module tree under `qml/` plus Qt6Quick*, Qt6QuickControls2*, Qt6QuickDialogs2*, Qt6QuickLayouts, Qt6QuickTemplates2, Qt6Qml*, Qt6Svg, Qt6OpenGL, Qt6LabsFolderListModel, Qt6QuickEffects/Shapes |
| `openrgb2-engine.exe` | same build, `build/src/engine/Release/` | Qt-free. OS DLLs only (WS2_32, IPHLPAPI, USER32, GDI32, ole32, ADVAPI32) + MSVC CRT (MSVCP140, VCRUNTIME140, VCRUNTIME140_1). tinyuz (`vendor/OpenRGB/dependencies/tinyuz`) + libdivsufsort (`vendor/OpenRGB/dependencies/HDiffPatch/libHDiffPatch/HDiff/private_diff/libdivsufsort`) statically compiled in (both MIT) |
| `openrgb2-ctl.exe` | same build, `build/tools/engine_ctl/Release/` | Qt-free. WS2_32 + MSVC CRT |

Stale duplicates of the two exes exist at `build/Release/` from an
older layout — the script deliberately sources only the
`build/src/*/Release/` and `build/tools/*/Release/` outputs.

Statically-linked third-party code in the engine/core: nlohmann-json
single header (`vendor/OpenRGB/dependencies/json`, MIT, Niels Lohmann —
text `licenses/MIT-nlohmann-json.txt`), tinyuz (MIT, housisong —
`licenses/MIT-tinyuz.txt`), libdivsufsort (MIT, Yuta Mori —
`licenses/MIT-divsufsort.txt`; vendored inside HDiffPatch). All three
are also statically compiled into `backend/OpenRGB.exe` per
`vendor/OpenRGB/OpenRGB.pro`. No DLLs — listed for provenance
completeness.

## 2. Pinned backend (vendor/OpenRGB — GPL-2.0-or-later)

`backend/OpenRGB.exe` comes **only** from the vendored fork's own
qmake recipe output (`vendor/OpenRGB/OpenRGB Windows 64-bit/`,
falling back to `vendor/OpenRGB/release/` mid-recipe). Never an
installed system OpenRGB. Fork: `danlil240/OpenRGB`, branch
`lianli-wireless`, imported at `53a477eab8d14b0bc49aeb40e3d569cbcb16a377`
(`vendor/provenance/MANIFEST.md`).

Direct deps (`dumpbin /dependents`):

| Dep | Source in vendor tree | License |
| --- | --- | --- |
| Qt6Core, Qt6Gui, Qt6Widgets | Qt 6.8.3 via `windeployqt` (recipe flags, `--no-network --no-quick-import` etc.) | LGPL-3.0 |
| Qt6Core5Compat, Qt6OpenGL, Qt6Svg | Qt 6.8.3, recipe's manual extras + qsvg plugin dep | LGPL-3.0 |
| libusb-1.0.dll | `dependencies/libusb-1.0.27` (VS2019 MS64 dll) | LGPL-2.1+ |
| hidapi-hotplug.dll | `dependencies/hidapi-hotplug-win/x64` | GPLv3 / BSD / orig. (choice — GPLv3 here) |
| PawnIOLib.dll | `dependencies/PawnIO` ([namazso/PawnIO](https://github.com/namazso/PawnIO)) | LGPL-2.1-or-later |
| *.bin (SmbusPIIX4, SmbusI801, SmbusIntelSkylakeIMC, SmbusNCT6793, LpcIO, LedsValve) | `dependencies/PawnIO/modules` — PawnIO hardware-access modules (replaced WinRing0), upstream [namazso/PawnIO.Modules](https://github.com/namazso/PawnIO.Modules) | LGPL-2.1-or-later |
| dxcompiler.dll, dxil.dll | DirectX Shader Compiler, pinned in the vendor deploy tree | LLVM Release License (UIUC/NCSA) — text in `licenses/LLVM-Release-DXShaderCompiler.txt` |
| MSVC CRT | VS2022 BuildTools redist | MS redist license |

The PawnIO **kernel driver** (`PawnIO.sys`, namazso/PawnIO) is
GPL-2.0-or-later with a linking exception and is **not shipped** —
the user installs it separately (`vendor/OpenRGB/Documentation/
SMBusAccess.md`); only the LGPL-2.1 user-mode library and module
blobs are bundled.

Qt plugin subdirs under `backend/` (`platforms/qwindows.dll`,
`styles/qmodernwindowsstyle.dll`, `imageformats/q*.dll`,
`iconengines/qsvgicon.dll`) are regenerated by windeployqt from the
pinned Qt — same flags as `scripts/build-windows.bat`.

## 3. Qt runtime (LGPL-3.0)

All `Qt6*.dll` and the `qml/`, `platforms/`, `styles/`,
`imageformats/`, `iconengines/` trees are produced by
`windeployqt.exe` from the pinned Qt 6.8.3 — not hand-copied.
The UI set is what `--qmldir ui/` scanning resolves for the app's
imports; the backend set matches the vendored build recipe.
LGPL-3.0/GPL-3.0 texts in `licenses/`; relinking note in NOTICES.txt.

## 4. MSVC CRT

`vcruntime140.dll`, `vcruntime140_1.dll`, `msvcp140*.dll`,
`concrt140.dll`, `vccorlib140.dll` copied app-local from
`VC\Redist\MSVC\<newest>\x64\Microsoft.VC143.CRT` so the package runs
without a preinstalled redist (missing-dependency recovery [plan §6]).

## 5. Data files

| Path | Source | Notes |
| --- | --- | --- |
| `presets/devices/*.device.json` | `src/core/presets/devices` | engine probes `presets/devices` relative to CWD |
| `presets/effects/*.effect.json` | `src/core/presets/effects` | same probe chain (DLS_EFFECT_DIR -> presets/effects) |
| `profiles/*.json` | committed `profiles/` (minus gitignored `profiles/local/`) | topology profiles, no unique IDs |
| `licenses/*.txt`, `NOTICES.txt` | `packaging/windows/` | license texts + manifest |
| `manifest.json` | generated | per-file SHA-256, bytes, origin tag |

## 6. Verified exclusions (script asserts; fails the package)

| Excluded | Check |
| --- | --- |
| Old effect plugins | any `plugins/`/`OpenRGBPlugins` dir; any `*plugin*.dll` outside Qt plugin subdirs (`qml/`, `platforms/`, `imageformats/`, `iconengines/`, `styles/`, `qmltooling/`, `generic/`, `platforminputcontexts/`, `tls/`, `networkinformation/`, `scenegraph/`, `multimedia/`, `sqldrivers/`); any `*LightingStudio*`/`*OpenRGBPlugin*` file |
| WinRing0-based HardwareSync | `WinRing0*` names; `HardwareSync` in file names |
| Installed L-Connect `yuz.dll` | exact `yuz.dll` name (tinyuz is compiled in statically — no DLL must ship) |
| Backend user state | `OpenRGB.json`, `*.log` under `backend/` |
| User-local profiles | `profiles/local/` absent (gitignored user state) |
| Debug Qt runtime | `Qt6*d.dll` names (release Qt modules ending in `d` would false-positive — allowlist in script if ever shipped) |

Also asserted present: the three app exes, `backend/OpenRGB.exe`,
Qt6Core+Qt6Widgets (backend) and Qt6Core+Qt6QuickControls2+`qml/`
(UI), `platforms/qwindows.dll` both sides, backend hw-access DLLs,
non-empty preset dirs, NOTICES/licenses/manifest.

## Reproduce

```powershell
powershell -ExecutionPolicy Bypass -File packaging\windows\stage-release.ps1
# env overrides: OPENRGB2_QT_DIR, OPENRGB2_BACKEND_DIR, OPENRGB2_VC_REDIST
# re-verify only:  ... -VerifyOnly -OutDir <dir>
```
