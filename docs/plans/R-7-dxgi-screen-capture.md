# R-7: DXGI Desktop Duplication screen capture

Status: proposed · Size: M · Source: audit 2026-11-28 R-7

## Goal

The `screenfield` ambient layer samples the display through DXGI Desktop
Duplication instead of GDI `StretchBlt`. Capture gets cheaper and runs at a
steadier rate, and it can see frames that GDI cannot read. GDI stays as the
fallback, so the feature never gets worse than it is today.

## Current state

- `src/platform/windows/WinScreenSampler.cpp` is the only capture path.
  `GrabGdi` (`:98-147`) runs `GetDC(nullptr)` and `StretchBlt` (HALFTONE)
  into a 12x6 32-bit DIB. The file header claims about 2–4 ms per grab.
  That figure is not measured.
- `ThreadMain` (`:256-273`) grabs every 40 ms (about 25 Hz) and sleeps 50 ms
  while `paused_` is set. `Grab` (`:275-332`) applies a luminance cap
  (`max_lum_` 0.85) and smoothing (0.35). It then calls
  `InputBus::SetScreenGrid(12, 6, cells_)`.
- `Start` runs a synchronous `Probe` grab (`:228-248`), so a display that
  cannot be read is refused honestly. `Status()` strings
  (`"screen: sampling \\.\DISPLAYn"`, `"capture unavailable"`) reach the
  UI through the reactive body (`EngineService.cpp`, `HandleReactiveCommand`).
- Displays are indexed by `EnumDisplayMonitors` order (`DisplayRects`,
  `:87-93`; `EnumerateDisplays`, `:355-372`, used at
  `EngineHost.cpp:997`). That index is persisted as `inputs.screen_index`
  (`StudioConfig.h:113`, `ProfileStore.cpp:736`).
- `src/platform/windows/CMakeLists.txt` links `gdi32`, `user32` and
  others, but not `d3d11` or `dxgi`. The offline suites never load this
  library: tests inject a fake `IReactiveSource`.
- The engine runs elevated in the interactive session
  (`packaging/windows/startup-task.ps1:231`, `-RunLevel Highest`).
  Desktop Duplication needs exactly that: it fails in session 0.

**Missing:** a DXGI path, any timing for either path, and a backend
selection or report in the status.

## Design

- Split the grab behind a small internal interface in
  `WinScreenSampler.cpp`: `IGrabber { bool Grab(const DisplayRect&,
  unsigned* px12x6, std::string& err); const char* Name(); }`. Two
  implementations: `GdiGrabber` (today's `GrabGdi`, unchanged) and
  `DxgiGrabber`.
- **DxgiGrabber.**
  - Find the `IDXGIOutput` whose `DXGI_OUTPUT_DESC.DeviceName` equals
    `MONITORINFOEX.szDevice`. Match by name, never by enumeration index,
    so the persisted `screen_index` keeps its meaning.
  - Create the D3D11 device on the adapter that owns that output. A
    hybrid-GPU mismatch gives `DXGI_ERROR_UNSUPPORTED`.
  - Prefer `IDXGIOutput5::DuplicateOutput1` with
    `[DXGI_FORMAT_B8G8R8A8_UNORM]`, and fall back to `DuplicateOutput`.
  - Per grab: `AcquireNextFrame(0)`, then `CopySubresourceRegion` into
    mip 0 of a texture created with `GENERATE_MIPS`, then `GenerateMips`.
    Copy the first mip that is 64 px wide or less into a staging texture,
    `Map` it, and box-average it on the CPU into 12x6. Only a few KB cross
    to the CPU.
  - `DXGI_ERROR_WAIT_TIMEOUT` means the desktop is unchanged: keep the
    previous grid and do not touch the bus.
  - `DXGI_ERROR_ACCESS_LOST` (mode change, full-screen switch) and
    `E_ACCESSDENIED` (secure desktop, UAC) cause a recreate with backoff
    (1 s, then 5 s). While it fails, use GDI for that grab.
- **Selection.** The sampler tries DXGI first and drops to GDI after
  repeated DXGI failures (for example 3 recreates within 30 s). It
  re-tries DXGI on the next `Start` or display change. `Status()` gains
  the backend: `"screen: sampling \\.\DISPLAY1 (dxgi)"`. No IPC schema
  change: status is already a free-form string. `docs/ipc-protocol.md`
  §reactive notes the suffix.
- **Pure helpers** in a new header (`ScreenGrid.h`, Qt-free, no Win32),
  so they are testable offline:
  - `BoxDownsample(src, w, h, stride, cols, rows, out)` for BGRA8.
  - `MatchOutputByName(names, wanted)`.
- **Privacy is unchanged:** the full frame stays on the GPU, and only the
  12x6 grid reaches scene evaluation. `paused_` still skips all GPU work.
  The duplication is released on `Stop` and on pause, so no handle is
  held while the displays sleep.

**Rejected:**
- Windows.Graphics.Capture. It needs WinRT/C++/WinRT, adds a capture
  border or consent on some builds, and is heavier to integrate into a
  Qt-free engine.
- Raising the rate to 60 Hz. The grid feeds a smoothed ambient layer, so
  25 Hz is enough, and the rate stays a constant.
- A pixel-shader downsample into a 12x6 render target. It needs HLSL
  build plumbing. The mip chain plus a CPU box filter is simpler and
  cheap enough.

## Steps

1. Add `ScreenGrid.h/.cpp` with `BoxDownsample` and `MatchOutputByName`,
   and move the luminance-cap/smoothing loop out of `Grab` into
   `ApplyCapAndSmooth`. Test: a new `screen_grid_test` (label `engine`)
   with a synthetic BGRA buffer giving known cell averages, a name match
   and a miss.
2. Introduce `IGrabber` and wrap the current GDI code as `GdiGrabber`
   with no behaviour change. Test: the build, plus a manual check that
   `reactive` status still reads `sampling`.
3. Add `DxgiGrabber`: device and duplication setup on the matched
   adapter, the mip downsample, and the timeout and access-lost
   handling. Link `d3d11` and `dxgi` in
   `src/platform/windows/CMakeLists.txt`. Test: the manual checks below.
4. Add DXGI-first selection with GDI fallback, the `(dxgi)`/`(gdi)`
   status suffix, and the recreate backoff. Test: a `FakeGrabber`-driven
   unit test of the selection state machine (fail N times, then switch
   to GDI) inside `screen_grid_test`.
5. Add an opt-in probe tool, `tools/bench/screen_probe` (not CTest).
   It grabs 500 frames with each backend and prints p50/p95 grab time.
   Record the results in `docs/benchmarks/`.

## Tests

- `screen_grid_test` (CTest label `engine`): downsample math, output-name
  matching, and the fallback state machine with a fake grabber.
- Manual, on Daniel's PC (RTX 5080):
  - The `screenfield` look follows a coloured full-screen window.
  - A UAC prompt or Ctrl+Alt+Del drops to GDI or holds, and then
    recovers.
  - A resolution change gives `ACCESS_LOST`, then a recreate.
  - An exclusive full-screen game.
  - HDR on versus off.
  - `screen_probe` timings for both backends.
- `ipc_reactive_test` keeps passing (status strings are opaque to it).

## Risks and open questions

- **HDR.** Whether `DuplicateOutput1` with only a BGRA8 format gives
  usable SDR colours on an HDR desktop is a guess. It must be checked on
  hardware. If it does not, accept `R16G16B16A16_FLOAT` and tonemap in
  `BoxDownsample`.
- **Black frames.** DRM-protected video is black on both paths. No fix
  is possible, so document it.
- **Concurrent duplications.** Other apps that duplicate the same output
  (OBS, the Xbox Game Bar) can make `DuplicateOutput` fail. The system
  limit is not verified here. The GDI fallback covers it.
- **Daniel to decide:** keep DXGI-first as the default, or put it behind
  an `inputs.screen_backend` setting (`auto|dxgi|gdi`). Plan default:
  `auto`, no setting, because the status shows which backend is in use.
