# Wireless baseline — Milestone 1

> **Status: measured (SDK-producer path), 2026-09-26 session.** Backend-side
> counters and trace were measured live; the old Studio path is deferred
> (GUI plugin, not headless-runnable) and physical presentation FPS remains
> **unknown** — no camera/photodiode method was available. Source-derived
> timing analysis below describes code, not hardware throughput, and is
> labeled as such.

## 1. Scope

Reproduce the current wireless limitation on Daniel's three-fan SL Wireless
group (see `docs/hardware/daniel-pc.md`), separately measuring:

1. **The old Studio path** — the preserved Desktop Lighting Studio output
   adapter driving the vendored backend (source of record at
   `C:\Users\daniel\tools\openrgb-unified`; run read-only).
2. **A minimal SDK producer** — `tools/bench/lianli_bench`, this
   repository's Milestone 1 producer (Direct-mode color updates over the
   SDK TCP loopback).
3. **The three existing uploaded presets** (Spectrum Cycle, Rainbow Wave,
   Breathing) as autonomous loops — recorded separately from live tests.

## 2. Exact binary/configuration (fill in per session)

Session: `profiles/local/bench-2026-09-26/` (gitignored — binaries, CSVs,
summaries, per-phase logs).

| Item | Value |
| --- | --- |
| Backend binary + commit | `vendor/OpenRGB/release/OpenRGB.exe`; openRGB2 `7332e3b` + session patches (`--device` field matching; `--mode/--speed/--hold-s`; interval-summary fix) on branch `milestone-1` |
| Backend launch flags | `--server --server-host 127.0.0.1 --server-port 6744 --noautoconnect --localconfig` |
| SDK port | 6744 (private config; installed OpenRGB used 6742) |
| Producer binary | `build/tools/bench/Release/lianli_bench.exe`, negotiated protocol 4 |
| Old Studio path | NOT RUN — Desktop Lighting Studio is a GUI plugin in the preserved tree (`openrgb-unified/plugins/DesktopLightingStudio`); no headless driver exists for it |
| OS / power plan / CPU governor | Windows 11 Pro 26200; Balanced power plan |
| Competing owners at run time | none for the wireless group; Logitech G Hub running (owns G512); FanControl running throughout |
| Backend trace | `OPENRGB_LIANLI_TRACE=<session>/lianli.csv` — 192,501 rows, cap not hit (`trace_capped=0`) |
| FanControl state | running throughout; process verified before and after the session |

## 3. Source-derived bottleneck analysis (code reading, not measurement)

Per plan §3.1, from the imported driver source — these are the stages the
instrumentation in `LianLiWirelessDiagnostics` observes:

| Stage | Current behavior (source-derived) | Consequence |
| --- | --- | --- |
| Studio output adapter (old path) | 40 ms minimum wireless pacing | ≤ ~25 submissions/s before other costs |
| Wireless runtime | `stream_ms = 25` minimum upload gap | ideal admission ceiling ~40 uploads/s |
| RGB upload | header packet transmitted twice | extra RF/USB work per frame |
| USB sender | 4 chunks, 1 ms sleep per chunk, 2 ms after the RF packet | ≥ 6 ms configured sleeps per RF packet |
| Service worker | 50 ms default wait, woken by new desired frames | wake behavior to be measured |
| Service locking | runtime ticks hold the mutex needed by submission | lock-wait to be measured |
| Discovery during live input | fresh frames postpone RX polling after a sighting | freshness/recovery to be measured |

For `n = ceil(compressed_bytes / 220)` payload packets, configured sleeps
alone cost `6 ms × (2 + n)`: 18 ms at n = 1, 24 ms at n = 2 — before USB,
scheduler, and heartbeat overhead. **Achieving 60 live FPS needs a faster
transport path; a faster UI timer cannot fix it.**

## 4. Instrumentation

The backend writes bounded diagnostics
(`vendor/OpenRGB/Controllers/LianLiWirelessController/LianLiWirelessDiagnostics.*`):

- Always-on fixed-size counters: submissions, replaced-pending, lock wait,
  compression (calls/time/bytes), admission (admitted/deferred), RF packets,
  header repeats, retries/failures, USB send/read durations, discovery
  (polls/misses/records/deferrals/age), keep-alive gaps, revalidations,
  foreign-master events.
- Optional CSV trace via `OPENRGB_LIANLI_TRACE=<path>` (bounded rows,
  default 1,000,000; override `OPENRGB_LIANLI_TRACE_MAX_ROWS`), with a
  `.summary` counter snapshot rewritten next to it.
- Trace row schema is documented in `LianLiWirelessDiagnostics.h`.

The producer records its own send timestamps and interval percentiles, and
prints the reminder that **SDK send success is not hardware confirmation**.

## 5. Workload matrix (plan §3.2)

Identical before/after workloads, each run against (a) the wireless group
only, and (b) the full setup (motherboard headers, DIMMs, GPU logo,
keyboard, mouse active):

| # | Workload | Producer pattern | Notes |
| --- | --- | --- | --- |
| W1 | Moving gradient, all 120 wireless LEDs | `--pattern gradient` | smooth hue change, low entropy |
| W2 | Rotating band | `--pattern band` | sharp transitions |
| W3 | Repeatable high-entropy per-LED colors | `--pattern entropy` | exposes compression-dependent limits |
| W4 | Static baseline | `--pattern static` | control run |

Rate sweep: `--sweep 10,20,30,40,60` with one pending live frame per group
(newest frame replaces an unsent older one — the runtime already enforces
this). Suggested duration: 30 s per rate, 2 s settle between rates; extend
to 10 minutes for the "stable operating rate" criterion when a candidate
rate matters.

## 6. Session procedure

1. **Pre-flight (no writes):**
   - Revalidate topology/ownership per the checklist in
     `docs/hardware/daniel-pc.md`; save unique IDs to `profiles/local/`.
   - Start the private backend (port 6744) with the trace env var set.
   - `lianli_bench --list` — confirm enumeration and record it.
   - `lianli_bench --device "Lian Li" --dry-run` — confirm the plan.
   - Confirm FanControl + sensors are healthy.
2. **Old Studio path (separate run):**
   - Run the preserved Studio path against its own backend configuration,
     driving the wireless group with the equivalent moving-gradient
     workload; record its trace separately. The preserved tree is
     read-only — copy binaries out if needed, never edit in place.
   - Record: submission rate admitted by the 40 ms adapter pacing,
     backend-side admission, USB timings, discovery deferrals.
3. **Minimal SDK producer:**
   - `lianli_bench --device "Lian Li" --pattern gradient --sweep 10,20,30,40,60 --duration-s 30 --out <session>/sdk-gradient.csv`
   - Repeat for `band`, `entropy` (W2, W3).
   - Repeat with the full setup selected (`--all` or repeated `--device`).
   - Verify state restore succeeded and devices show pre-run lighting.
4. **Uploaded preset baselines (separate from live tests):**
   - Apply each preset (Spectrum Cycle / Rainbow Wave / Breathing) at
     default speed 100 and one low/high speed (e.g. 10 and 200) via the
     backend; record: requested speed, frame count (48), nominal interval
     `max(1, 5000/speed)` ms, upload traffic (one compressed upload +
     resends until ack), and — with the camera/photodiode method — measured
     physical playback intervals and full-cycle duration.
5. **Physical presentation measurement (when equipment available):**
   - Coded pattern + high-speed camera or photodiode; record measurement
     resolution and LED PWM aliasing limits. If unavailable, presentation
     FPS stays **unknown** and is reported as such.
6. **Post-flight:**
   - Restore saved state; confirm FanControl/sensors healthy; collect the
     `.summary` and CSV artifacts; record everything in this file and in
     `profiles/local/session-<date>.json`.

**Hard stop:** the producer stops at its duration limit and restores saved
state; Ctrl+C also stops and restores. If anything behaves unexpectedly
(fan control, sensors, wrong device lighting), stop the session, restore,
and record the anomaly.

## 7. Results (2026-09-26 authorized session)

Window boundaries were recovered from `set_desired` gaps in the trace
(2 s settle gaps separate the five 30 s rate windows; counter snapshots
were diffed per phase with up to ~30 s summary-flush granularity).

### 7.1 Revalidation

- 5 controllers enumerated; wireless group `UNI FAN SL V3 Wireless 66:E1`
  (serial/location `9B:10:76:E5:66:E1`) present with **120 LEDs** and modes
  `Direct / Static / Spectrum Cycle / Rainbow Wave / Breathing`. ✓
- GPU (26 LEDs), Razer mouse (13), G512 keyboard (108) enumerated.
- **Topology gap:** motherboard `X870E AORUS ELITE WIFI7 ICE` reported only
  **2 LEDs** — the expected 8/16/8 header layout did not enumerate in this
  session. Recorded as an open issue; not corrected silently.
- DIMMs absent from enumeration (backend ran non-elevated; PawnIO path).
- Foreign-master events: 0; transport revalidations: 0; discovery misses:
  11 total across ~2 h of streaming. Ownership was stable.

### 7.2 Old Studio path

NOT RUN — no headless entry point exists (GUI plugin). Left for a future
session with GUI access, or until the Studio path is wrapped in a
scriptable producer.

### 7.3 Minimal SDK producer (wireless group only)

`set_desired` = frames reaching the driver; `admitted` = `upload` trace
events (RF uploads actually sent). Producer send count was the nominal
300/600/900/1200/1800 per 30 s window.

| Rate | Producer sent | W1 desired/admitted | W2 band admitted | W3 entropy admitted | W4 static admitted | Retries |
| --- | --- | --- | --- | --- | --- | --- |
| 10 | 300 | 301 / 301 | 301 | 301 | 301 | 0 |
| 20 | 600 | 600 / 600 | 600 | 600 | 600 | 0 |
| 30 | 900 | 900 / 901 | 900 | 861 | 901 | 0 |
| 40 | 1200 | 1187 / 1136 | 1146 | 859 | 1146 | ≤1/phase |
| 60 | 1800 | 1189 / 1150 | 1147 | 864 | 1149 | ≤1/phase |

Backend upload inter-arrival (admission cadence, ms):

| Rate | W1/W2/W4 p50 | W1/W2/W4 p95 | W3 entropy p50 | W3 entropy p95 |
| --- | --- | --- | --- | --- |
| 10 | 94.2–94.8 | ~110 | 94.8 | 110.2 |
| 20 | 46.9–47.1 | ~63.6 | 47.1 | 63.3 |
| 30 | 31.2–31.5 | ~47.1 | **34.6** | 36.9 |
| 40 | 25.6–26.0 | ~27.1 | **34.5** | 36.2 |
| 60 | 25.7–26.0 | ~27.2 | **34.5** | 36.1 |

Compression per submitted frame (360 B in, 120 LEDs × 3 B): gradient/static
≈ 13–15 B out, band ≈ 14.5 B, entropy ≈ 370 B (incompressible → extra RF
payload packets). Per-phase mean `upload_us`: 25.6–25.9 ms compressible,
**34.5 ms** entropy. `usb_send` ≈ 8.5–8.6 ms per 4-chunk batch throughout.

Sustained ceilings measured: **≈ 38.3–38.5 uploads/s** for compressible
frames, **≈ 28.7 uploads/s** for incompressible frames. At ≥40 fps the
excess collapsed via newest-frame-wins (`replaced_pending` ≈ submissions
at saturation; queue depth stayed 1) — no unbounded growth.

Submission `lock_wait` mean per phase: 9.2 ms (W1), 12.7 ms (W2),
**24.3 ms (W3)**, 13.0 ms (W4) — submissions block behind the in-flight
RF upload; head-of-line contention confirmed. Max observed 89 ms.

Producer pacing note: `lianli_bench`'s send timer is quantized to the
Windows 15.6 ms tick — send intervals are bimodal (15.5/31 ms at 60 fps)
while the mean rate stays on target. Use backend-side timing for analysis.

### 7.4 Full setup (all devices active)

`--all` gradient sweep, 4800 frames to each of 5 controllers: wireless
admissions 302/600/900/1147/1151 — statistically identical to the
wireless-only run (§7.3). Wired devices do not measurably degrade the
wireless path; the ceiling is upload serialization, not contention.

### 7.5 Uploaded presets

Each `--mode` apply was held 12 s, then restored; per apply→hold→restore
cycle the backend recorded ~3–4 `upload` admissions (one compressed
effect upload + resends until the reported effect ID converges, plus the
restore upload). No failures, 0–1 resend retries each; all converged
within the hold.

| Preset | Speed | Compressed bytes out (per apply, ÷3 calls) | RF packets | Measured playback |
| --- | --- | --- | --- | --- |
| Spectrum Cycle | 100 / 10 / 200 | ≈ 63 B | 6/cycle | **unknown** |
| Rainbow Wave | 100 / 10 / 200 | ≈ 946 B | ~32/cycle + 1 resend | **unknown** |
| Breathing | 100 / 10 / 200 | ≈ 62 B | 6/cycle | **unknown** |

Nominal intervals (`max(1, 5000/speed)` ms, 48 frames/loop): speed 100 →
50 ms; 10 → 500 ms; 200 → 25 ms. Physical playback was not measured —
no camera/photodiode method available. Upload traffic is identical across
speeds (speed is a mode parameter inside one upload, not extra RF work).

### 7.6 FanControl / sensors verification

FanControl.exe ran before, during (mid-session check), and after the
session; the bench producer never touches PWM/sensors and the backend was
launched with `--noautoconnect`. No cooling anomaly was observed or
reported. Quantitative sensor logs were not captured — flagged for next
session's runbook improvement.

### 7.7 Presentation measurement

**Unknown** — no high-speed camera/photodiode available. The numbers in
§7.3 are backend-side RF admission timing, not visible light intervals.
Do not present them as display FPS.

## 8. Findings — limiting stages (measured)

All numbers below are from the 2026-09-26 session, binary/config per §2.

1. **RF upload serialization is the ceiling.** Each live-frame upload
   takes ≈ 25.6–26.0 ms for compressible frames (the 25 ms minimum stream
   gap overlaps the ~6 ms of configured per-packet sleeps) and ≈ 34.5 ms
   for incompressible frames (extra payload packets). Sustained admission
   saturates at ≈ 38.5/s and ≈ 28.7/s respectively — the source-derived
   §3 prediction (~25–39/s) is confirmed. **60 live FPS is unreachable on
   this transport path.**
2. **Compression ratio directly sets the ceiling.** W3's 370 B compressed
   frames need one more RF packet than W1/W2/W4's ~15 B frames — that one
   packet costs ~8.5 ms of sleeps + USB time per upload, cutting the
   ceiling ~25%.
3. **Head-of-line blocking under the service mutex.** Submissions wait
   9–24 ms behind the in-flight upload; the mutex is held across the RF
   send. The excess collapses safely via newest-frame-wins
   (`replaced_pending` ≈ 100% at saturation), but any future faster
   producer gains nothing without shortening the upload critical section.
4. **SDK-side delivery saturates ≈ 39.6/s.** At 60 fps the driver saw
   1189 of 1800 sent frames — the server's serialized update path
   (blocked on the same mutex during uploads) coalesces the surplus.
5. **Reliability is not the problem.** 0 upload failures, ≤5 retries
   across ~19.9 k uploads, 0 foreign-master events, keep-alive gap max
   885 ms (one 3.3 s outlier during W4), discovery correctly deferred
   1:1 with submissions during streams. The limitation is throughput.
6. **Presets upload once and converge fast** (~1 upload + ≤2 resends per
   apply, converging well inside 12 s); their on-hardware playback rate is
   autonomous and was not physically measured.
