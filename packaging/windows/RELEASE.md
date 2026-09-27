# OpenRGB2 0.1.0 — local release (Windows x64)

**Status: local release only.** This package has not been published,
tagged, pushed, or signed. It is the Milestone-6 staged build verified
on the development machine. Do not redistribute binaries built from
this tree without the publish-time decisions called out below
(unsigned binaries, SAC first-run transients, license review).

## What is installed

This folder IS the install — no installer and no PATH changes. Copy
it anywhere and run it.

| Entry | What it is |
| --- | --- |
| `openrgb2-ui.exe` | Qt Quick UI (effects, devices, diagnostics) |
| `openrgb2-engine.exe` | Qt-free lighting engine; owns scene clock, delivery lanes, SDK client, IPC pipes |
| `openrgb2-ctl.exe` | CLI for the engine's command/diagnostics pipes |
| `backend\OpenRGB.exe` | pinned `lianli-wireless` OpenRGB backend — the sole RGB hardware owner, SDK server on `127.0.0.1:6744` only |
| `presets\`, `profiles\` | bundled effect/device library + committed topology profiles (no unique IDs) |
| `scripts\*.ps1` + `OpenRGB2.cmd` | managed start / stop / update / rollback |
| `INSTALL.md` | install / startup / ownership / update / rollback / recovery guide |
| `manifest.json` | per-file SHA-256 + provenance for the whole tree |
| `NOTICES.txt`, `licenses\` | license texts and provenance for every payload |

User state is created at runtime inside this folder under `state\`
(backend private config, run file, update journal) and
`profiles\local\` (durable engine profile, resolved device
identities). Three small outside-install writes also exist —
documented in `INSTALL.md` ("State written OUTSIDE the install dir"):
a ~370-byte backend startup log stub per launch in
`%APPDATA%\OpenRGB\logs\` (vendored `LogManager` runs before
`--localconfig`), the UI's Qt cache under `%LOCALAPPDATA%\OpenRGB2\`,
and UI preferences in `HKCU\Software\OpenRGB2\OpenRGB2`. Removal =
delete this folder, optionally those three locations; nothing else
touches the system.

## How to run

Double-click **`OpenRGB2.cmd`** (or
`powershell -File scripts\start-openrgb2.ps1`). The launcher then:

1. **Ownership scan** — reports competing RGB owners (L-Connect, G Hub,
   Synapse, iCUE, SignalRGB, GCC, other OpenRGB instances). FanControl
   is an expected notice and is left alone.
2. **Takeover gate** — if conflicts exist, startup **refuses** (exit 3)
   until you close the competitor yourself or re-run with `-TakeOver`
   (`OpenRGB2.cmd -TakeOver`). Takeover is never silent, and no script
   ever kills a competitor. A foreign `OpenRGB.exe` still refuses
   outright — the pinned backend's own kill-capable single-instance
   prompt is never triggered.
3. **Backend** → **engine** → **UI**, in that order. The engine never
   spawns the backend; the UI is optional (`-NoUI`) and closing it
   leaves engine + backend running.
4. **Stop:** `powershell -File scripts\stop-openrgb2.ps1` — engine gets
   the IPC `QUIT` (ordered bounded shutdown), then the backend is
   closed gracefully. `-Force` exists as the last resort on verified
   own processes only.

Elevation is opt-in (`-Elevated` → UAC prompt) and only needed for
PawnIO-gated paths (SMBus DIMMs, SuperIO) — USB/HID devices work
unelevated.

## Measured support matrix

Full per-device detail: `docs/hardware/support-matrix.md` in the source
tree. Summary — "verified" tiers are explicit there; nothing below
rests on photons:

| Device | Status |
| --- | --- |
| UNI FAN SL V3 Wireless group (3×40 LEDs) | **VERIFIED — both modes**: live ~32 Hz self-paced + uploaded playback held 65 min, device clock + keepalives observed |
| AORUS RTX 5080 MASTER ICE (GPU, 26) | **VERIFIED (transport)** ~58 Hz — side logo physically seen; fan lighting rings never confirmed |
| Logitech G512 keyboard (108) | **VERIFIED (transport)** ~58 Hz — G Hub also owns it on this rig (ownership caveat) |
| Razer Basilisk V3 Pro wired (13) | **VERIFIED (transport)** ~59 Hz |
| X870E `LED_C` + `Chipset Accent` (2) | outside the matrix — zone identity unknown |
| X870E ARGB headers 8/16/8 | **not supported** — enumerate 0 LEDs (backend gap) |
| Corsair Vengeance RGB DDR5 ×2 | **not supported** — needs elevated backend (PawnIO) |
| AIO pump LCD / cooling channels | out of scope for this release |

## Measured performance (honest labels)

All engine-side rates are **transport-admitted** (backend-acknowledged
writes), not measured LED output. Physical FPS is **unmeasured** — no
camera/photodiode exists on this rig.

- **Live effects:** wired lanes 59.1 sends/s at the 60 Hz scene clock,
  wireless lane 32.1 sends/s (its self-paced ~33/s RF ceiling is a
  transport property, not an engine stall; live 60 FPS over this
  transport is unreachable and is an open release criterion —
  see `docs/benchmarks/wireless-result.md`).
- **Uploaded playback:** three presets (Spectrum Cycle, Rainbow Wave,
  Breathing) run as device-side loops off the MCU clock; ~60 fps
  nominal loop rate after frame-count quantization. Device-side
  evidence: `rgb_clock` ticks ≈1600/s nominal, `upload: active` held
  the whole leg.
- **Playback clock drift:** −27.5 ppm over 64.6 min (1599.956 ticks/s)
  — ≈1.6 ms/min visual drift on a 60 fps loop; imperceptible. Not
  reconcilable with the earlier −0.6 ppm/9.7-min fit; treated as
  non-stationary clock behaviour, documented as anomaly A5.
- **Soak (bounded):** 65.0 min playback + 60.0 min live, zero failures,
  zero transport rejections on the live leg, keepalive gaps <1 s,
  backend CPU ~0.8 core under live load, engine memory slope
  decelerating toward ~5 MB. The plan's **8-hour criterion was not
  run** — the owner accepted the bounded 65 + 60 min legs as the
  release qualification (`docs/benchmarks/mixed-and-soak.md`).
- **UI responsiveness (plan §6):** p95 local control response
  **31.1 ms < 100 ms** — 300 IPC request→reply samples over real named
  pipes (subscribed session, six-verb UI surface incl. the SET_SCENE
  save path) while the engine held 60 Hz output to the in-process
  fake backend. Scope: this is the IPC/engine half of "control
  response"; the QML/GUI half is bounded separately by the
  `ui_hardening_test` flood probe. See
  `docs/benchmarks/ui-responsiveness.md` for method + coverage.

## Known limitations / residuals

- **DIMMs unverified** — no SMBus DRAM controller enumerates without
  elevation (PawnIO `Permission Denied` unelevated; logged + visible,
  never silent).
- **Physical FPS unmeasured** — every rate above is transport- or
  device-trace-level; no light measurement instrument exists.
- **8 h soak waived** — the literal 8 h-per-mode criterion was waived by
  the owner in favor of the 65 + 60 min bounded legs; long-run resource
  conclusions beyond that horizon are still not claimed.
- **Motherboard coverage gaps** — `LED_C`/`Chipset Accent` accept
  frames but their physical location is unknown; the 8/16/8 ARGB
  headers enumerate 0 LEDs (largest known coverage gap, predates the
  engine).
- **GPU fan lighting rings** — the three 8-LED fan zones accept frames;
  no physical observation has confirmed they light anything.
- **Anomaly A1 open** — a duplicated/older backend process once wedged
  the wireless runtime silently at the SDK boundary (zero RF traffic
  while `upload: active` reported). Mitigated by the single managed
  instance + trace instrumentation; the backend observability gap
  itself is not closed.
- **Unsigned binaries** — first run on a new machine may see transient
  Smart App Control denials until ISG verdicts land; documented
  platform behaviour, not a defect. No SAC workaround or exclusion
  exists anywhere in this repo (`docs/packaging/security.md`).
- **Backend modal dialog absorbs graceful close** — a backend showing
  a modal (observed: the PawnIO prompt) never receives
  `CloseMainWindow`, so `stop-openrgb2.ps1` needs `-Force` to stop our
  own backend in that state. `-Force` still only applies to processes
  verified by image path as this install's own; foreign processes are
  never touched.

## Verification trail

`docs/packaging/security.md` (boundary checks), `docs/benchmarks/*.md`
(M1–M5 measured reports), `docs/packaging/source-manifest.md`
(build-from-source + component manifest), `vendor/provenance/`
(backend import provenance), `INSTALL.md` (lifecycle + recovery).
