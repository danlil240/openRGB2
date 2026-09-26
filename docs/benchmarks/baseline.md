# Wireless baseline — Milestone 1

> **Status: prepared, not measured.** This report defines the Milestone 1
> baseline procedure and records the exact binaries/configurations once an
> **authorized hardware session** runs them. No presentation FPS is reported
> until physically measured. Source-derived timing analysis below describes
> code, not hardware throughput, and is labeled as such.

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

| Item | Value |
| --- | --- |
| Backend binary + commit | PENDING — path, git describe of `vendor/OpenRGB`, dirty state |
| Backend launch flags | planned: `--server --server-host 127.0.0.1 --server-port 6744 --noautoconnect --localconfig` |
| SDK port | 6744 (private config; installed OpenRGB used 6742) |
| Producer binary | `build/tools/bench/Release/lianli_bench.exe` + git describe |
| Old Studio path | PENDING — exact binary/run steps used from the preserved tree |
| OS / power plan / CPU governor | PENDING |
| Competing owners at run time | PENDING — verified list |
| Backend trace | `OPENRGB_LIANLI_TRACE=<session>/lianli.csv`, default row cap 1,000,000 |
| FanControl state | running throughout; sensor check before/after |

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

## 7. Results (fill in during the authorized session)

### 7.1 Revalidation

PENDING — see checklist outcomes.

### 7.2 Old Studio path

| Metric | Value |
| --- | --- |
| Producer submission rate | PENDING |
| Backend admissions/s | PENDING |
| p50/p95/p99 admission interval | PENDING |
| USB send duration p50/p95/p99 | PENDING |
| Compression bytes + time (per frame) | PENDING |
| Discovery age / deferrals | PENDING |
| Heartbeat gaps | PENDING |
| Lock wait | PENDING |

### 7.3 Minimal SDK producer (wireless group only)

| Rate (fps) | W1 submitted / admitted | W1 deferrals | W2 admitted | W3 admitted | resets/retries | notes |
| --- | --- | --- | --- | --- | --- | --- |
| 10 | PENDING | PENDING | PENDING | PENDING | PENDING | |
| 20 | PENDING | PENDING | PENDING | PENDING | PENDING | |
| 30 | PENDING | PENDING | PENDING | PENDING | PENDING | |
| 40 | PENDING | PENDING | PENDING | PENDING | PENDING | |
| 60 | PENDING | PENDING | PENDING | PENDING | PENDING | |

Interval percentiles (p50/p95/p99, µs) per rate and workload: PENDING.
Compression sensitivity (W3 vs W1): PENDING.

### 7.4 Full setup (all devices active)

PENDING — same table as §7.3, plus per-wired-device lag observations.

### 7.5 Uploaded presets

| Preset | Speed | Frames | Nominal interval (ms) | Measured playback (physical) | Upload traffic | Cycle duration |
| --- | --- | --- | --- | --- | --- | --- |
| Spectrum Cycle | 100 | 48 | 50 | PENDING | PENDING | PENDING |
| Spectrum Cycle | 10 / 200 | 48 | 500 / 25 | PENDING | PENDING | PENDING |
| Rainbow Wave | 100 | 48 | 50 | PENDING | PENDING | PENDING |
| Rainbow Wave | 10 / 200 | 48 | 500 / 25 | PENDING | PENDING | PENDING |
| Breathing | 100 | 48 | 50 | PENDING | PENDING | PENDING |
| Breathing | 10 / 200 | 48 | 500 / 25 | PENDING | PENDING | PENDING |

Note: nominal rates are code-derived (`interval_ms = max(1, 5000/speed)`,
48 frames/loop), **not physical measurements**.

### 7.6 FanControl / sensors verification

PENDING — sensor readings before/after each phase; fan speeds unchanged.

### 7.7 Presentation measurement

PENDING — method, resolution, per-workload visible frame intervals; or
explicitly **unknown** if not physically measured.

## 8. Findings — limiting stages (fill in after measurement)

PENDING — name the measured limiting stage(s) with numbers from §7, tied to
the exact binary/configuration in §2. Only measured evidence goes here;
source-derived analysis stays in §3 and is never presented as a result.
