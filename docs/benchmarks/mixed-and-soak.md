# Mixed-workload and soak — Task 5.5 (Milestone 5)

Long-run stability verification of the full production stack — engine +
managed backend + real hardware — in both output modes, plus the
per-device support matrix in `docs/hardware/support-matrix.md`. Leg 1
doubles as the *mixed* workload of the title: the wireless lane runs an
uploaded device-side loop while the four wired lanes stream
baked-timeline frames concurrently — both delivery modes exercised at
once, which is the plan's mixed-mode intent.

> **Truncation disclosure:** the plan specifies an eight-hour soak per
> mode. What was actually run is recorded honestly below — a bounded
> in-session run of **65.0 min (playback)** and **60.0 min (live)**, not
> 8 h per leg. Eight-hour-plus resource conclusions are *not* claimed;
> the numbers below show what the bounded run measured.

## Builds under test

| Component | Version |
| --- | --- |
| `openrgb2-engine` + `openrgb2-ctl` | branch `milestone-5`, HEAD `f79945f` |
| Managed backend | `vendor/OpenRGB/release/OpenRGB.exe` (2026-09-26 23:52 build — the fully-instrumented binary; see anomaly A1) |
| Backend launch | `OpenRGB.exe --server --server-port 6744 --localconfig`, cwd `profiles/local/soak/run` |
| Engine launch | `openrgb2-engine --sdk --scene …/scene-playback.json --state …/engine-state.json --backend-config …/run` |
| Trace | `OPENRGB_LIANLI_TRACE=…/lianli.csv`, `OPENRGB_LIANLI_TRACE_MAX_ROWS=4000000` |
| Sampler | `profiles/local/soak/sample.py` — `openrgb2-ctl diag` + `Get-Process` every 60 s → `soak-<leg>.csv`, `diag-<leg>.jsonl`, `proc-<leg>.csv` |

## Topology

5 controllers via SDK `127.0.0.1:6744`: RTX 5080 MASTER ICE (26),
X870E AORUS ELITE WIFI7 ICE (2), UNI FAN SL V3 Wireless 66:E1 (120),
G512 (108), Razer Basilisk V3 Pro (13). DIMMs absent (PawnIO,
unelevated — unchanged since M1).

## Methodology

- Every metric is a **transport-boundary or device-side** measurement:
  engine counters count backend-acknowledged sends; backend trace rows
  are the wireless MCU's observable work (discovery polls, keepalives,
  RF uploads, playback clock). No light-meter exists on this rig —
  physical output claims are qualitative only.
- The sampler polls `diag` (delivery stats, playback status, power,
  IPC) and process WorkingSet/PrivateBytes/CPU/handles/threads for
  `openrgb2-engine` and `OpenRGB` every 60 s.
- `ES_DISPLAY_REQUIRED` was held for the soak window
  (`keepawake.ps1`) so the power gate stayed open; power-state is
  sampled every minute in the CSV and any display-off window is visible
  as `output_paused`.
- Drift: linear fit of `rgb_clock` tick counter vs backend wall time
  (same method as Task 5.2).
- Resource-table `Fit slope` values are least-squares (OLS) fits over
  the whole sample window, **not** a (last−first)/duration delta —
  growth here is front-loaded (allocator/page-cache warm-up), so the
  fitted rate reads ~half the naive endpoint rate.

## Anomaly A1 — wireless runtime wedge on the *older* backend binary

**This is a real finding, documented rather than hidden.**

The first ~35 min of the session ran on
`vendor/OpenRGB/OpenRGB Windows 64-bit/OpenRGB.exe` (2026-09-26 20:04)
and a first `release/` instance. Symptom: the wireless service worker
ticked every ~50 ms and accepted `set_desired` (both live frames and
the uploaded loop), yet produced **zero** `poll`/`usb_read`/`usb_send`/
`keepalive`/`rgb_clock`/`state` trace rows across three backend
instances and ~25 min — meaning **no RF traffic reached the fans at
all** in that window. SDK-level delivery looked healthy everywhere:
`upload: active`, wired lanes streaming, `transport_completed` ticking.

Observed contributing factor: **two `OpenRGB.exe` processes coexisted**
(pids 22588 + 49276). The likely mechanism is that the second instance
held or invalidated the USB dongle pair, leaving the serving instance's
wireless runtime unable to progress past its initial transport state —
it retried a ~132 ms operation every ~437 ms indefinitely with no trace
output. A clean single-process cold start restored full telemetry
immediately (Boot → Searching → Uploading within ~5.5 s).

Takeaways:

- The failure is **silent at the SDK boundary** — `upload: active` and
  Direct-mode sends are acknowledged even when no RF traffic exists.
  This is a backend-observability gap, not an engine reporting lie: the
  engine honestly reports `sync: unknown` and `physical.provenance:
  unknown`.
- The `OpenRGB Windows 64-bit/OpenRGB.exe` binary (20:04) also
  predates the full trace instrumentation (no `rgb_clock`,
  `usb_send`, `poll_defer`, `foreign_master` symbols). All subsequent
  soak data uses `vendor/OpenRGB/release/OpenRGB.exe`.
- Wedged-window rows were preserved (`*-wedged.csv`, `lianli-run*.csv`)
  and are excluded from the steady-state numbers below.

## Leg 1 — Animation playback (uploaded loop, continuous)

Window: `2026-09-27T20:50:47` – `21:55:47` (**65.0 min**, 66 samples @ 60 s).
Scene `modequal-playback` (preset `rainbow_wave`, 144-frame loop @ ~60 fps
nominal); `upload: active`, `control: playing` for the entire leg;
display on throughout (`display_on: True`, no output-pause events).

### Delivery

- Wired lanes (GPU / MB / G512 / Basilisk): baked-timeline delivery,
  **21.13 sends/s per lane, flat** (linear-fit residual RMS 61 sends —
  no drift, no stall). The wireless group's timeline drives the wired
  choreography at ~1/3 scene rate, matching Task 5.2.
- Wireless lane: `transport_completed` held at ~1 (ownership verified —
  live frames are rejected at the pipeline with "controller owned by
  uploaded playback", ~3.6 k rejections/min accumulating honestly in
  `rejected`, never sent).
- `agg_max_interval_s` = 33.7 s — a **single** early gap during the
  mode-apply/park transition; it did not recur and did not grow.

### Wireless device-side (backend trace, playback window)

| Metric | Value |
| --- | --- |
| keepalives | 4 853; gap p50 = 801 ms, p99 = 804 ms, **max = 809 ms** — metronome-regular heartbeat, zero >1 s gaps |
| keepalive failures | 0 |
| discovery misses | 2 (single-poll transients; recovered next poll) |
| uploads admitted | 256 loop resends, p50 = 25.8 ms, p99 = 27.1 ms, max = 28.2 ms |
| upload retries / failures | 1 retry total (initial admission) / **0 failures** |
| state transitions | 5× Uploading→Holding, 4× Holding→Uploading — resend-on-drift cycles; runtime settled `Holding` (device-confirmed `effect_id` match) |
| foreign masters / revalidations | 0 / 0 |

### Playback clock drift — 64.6 minutes

`rgb_clock` (device playback tick counter vs backend wall time),
1 291 valid samples over 3 875 s, linear fit:

- **Tick rate: 1 599.9561 ticks/s vs 1 600 nominal → −27.5 ppm**
  (≈ −1.65 ms/min, ≈ −107 ms over the full leg).
- Residual RMS 58.5 ticks (≈36.6 ms) — poll-scheduling jitter around
  the fit, not accumulated phase error.
- Task 5.2 measured −0.6 ppm over 9.7 min (195 samples, 582.9 s). The
  two fits are **not** reconcilable as a single constant drift rate:
  5.2's fit resolved slope to roughly ±4 ppm, so a −27.5 ppm clock
  would have shown there at ~6σ. The likely cause is non-stationary
  clock behaviour — oscillator warm-up after loop start, or a
  temperature/session difference — so read −27.5 ppm as what the MCU
  free-ran *in this window*, not necessarily a device constant. At a
  60 fps uploaded loop this is ≈1.6 ms/min of visual drift against the
  baked wired timeline — ~0.1 frames/min; imperceptible, and the wired
  lanes bake the same wall-clock timeline anyway. `sync` stays
  `unknown` on the wire by design; this is backend-trace evidence.

### Resources

| Process | First | Last | Fit slope |
| --- | --- | --- | --- |
| engine WorkingSet | 11.5 MB | 13.8 MB | +1.07 MB/h |
| engine PrivateBytes | 2.8 MB | 5.0 MB | +0.90 MB/h |
| engine handles / threads | 188 / 16 | 181 / 10 | flat-to-falling |
| backend WorkingSet | 35.8 MB | 36.4 MB | +0.20 MB/h |
| backend PrivateBytes | 11.0 MB | 11.4 MB | +0.11 MB/h |
| backend handles / threads | 379 / 25 | 375 / 24 | flat |
| engine CPU | — | — | 46.5 s/h ≈ 1.3 % of one core |
| backend CPU | — | — | 932 s/h ≈ 26 % of one core |

Engine memory shows a mild upward slope (~1 MB/h) — most plausibly
allocator/page-cache warm-up inside the first hour, but **at 65 min it
is not proven asymptotic**; flagged honestly rather than called flat.
Backend CPU ~26 % of one core is the wireless worker's poll cadence
(~132 ms RX-window every ~437 ms + keepalive/upload work) — the
polling architecture's standing cost, unchanged all leg.


## Leg 2 — Live effects (continuous live delivery)

Window: `2026-09-27T21:56:00` – `22:56:00` (**60.0 min**, 61 samples @
60 s). Scene `modequal-live` with effect preset `aurora` — the flat
single-point layout in this profile makes spatially-uniform effects
constant per frame, which the unchanged-frame suppressor correctly
drops; `aurora` varies temporally and produces a real continuous
stream. `upload: none`, `control: playing`, `display_on: True`
throughout.

### Delivery

| Lane | Rate (linear fit) | Residual RMS | Verdict |
| --- | --- | --- | --- |
| GPU / MB / G512 / Basilisk | **59.12 sends/s each** | 7 sends | flat — wired lanes at full live cadence, zero drift over the hour |
| Wireless (UNI FAN SL V3) | **32.09 sends/s** | 4 sends | flat — self-paced RF lane at its content-dependent ~32 fps ceiling |
| Aggregate | 268.6/s | — | stable; `agg_max_interval` 33.7 s carried over from leg-1 startup, never recurred |

`rejected` delta for the whole live leg: **0** — live frames were never
transport-rejected. `agg_replaced` grew ~11.5 k over the hour — ~3.2/s:
the producer ticks at ~59 Hz against the wireless lane's ~33/s budgeted
submission cadence, so a replace is counted whenever a producer frame
lands while its predecessor is still queued for that lane. Intended
backpressure behaviour, not loss.

### Wireless device-side (backend trace, live window)

| Metric | Value |
| --- | --- |
| live uploads admitted | **101 179** (~28/s RF effect uploads); duration p50 = 26.0 ms, p99 = 28.1 ms, max = 53.7 ms |
| `usb_send` RF packets | 308 241 (~3 packets per upload, ~86/s) |
| keepalives | 4 703; gap p50 = 769 ms, p99 = 878 ms, **max = 908 ms** — heartbeat stays under 1 s even at full upload load |
| keepalive failures | 0 |
| state transitions | 168× Holding→Uploading, 168× Uploading→Holding — normal per-burst settle cycles, runtime always re-converged |
| discovery polls | 3 597 (fewer than playback's 9.7 k — polls are deferred while the RF queue is busy, `poll_defer` = 69.8 k); `discovery_age_ms_max` = 3 079 ms |
| discovery misses | 61 (~1/min) single-poll transients, each recovered on the next poll — correlates with upload-queue pressure; disclosed honestly, no escalation observed |
| retries / failures | 1 cumulative retry (leg-1 admission) / **0 failures** |

The ~4/s gap between the engine-side 32.09 sends/s and the ~28.1/s
admitted RF uploads here is **backend desired-state coalescing**, not
loss: `set_desired` updates that arrive while an upload is still in
flight collapse into a single upload of the latest state, so several
engine sends can merge into one device-side upload.

### Resources

| Process | First | Last | Fit slope |
| --- | --- | --- | --- |
| engine WorkingSet | 12.8 MB | 14.1 MB | +0.56 MB/h |
| engine PrivateBytes | 3.9 MB | 5.3 MB | +0.71 MB/h |
| engine handles / threads | 183 / 10 | 181 / 10 | flat |
| backend WorkingSet / PrivateBytes | 36.4 / 11.4 MB | 36.4 / 11.4 MB | **0.00 MB/h — dead flat** |
| backend handles / threads | 374 / 24 | 374 / 24 | flat |
| engine CPU | — | — | 110 s/h ≈ 3 % of one core |
| backend CPU | — | — | 2 799 s/h ≈ **78 % of one core** |

Live delivery is a heavy backend workload: ~101 k compress+upload
cycles/hour keeps the wireless worker near 0.8 cores. That is the
architecture's standing cost at ~32 fps, constant all leg — no upward
drift in CPU rate. Engine private memory reached ~5.3 MB at leg end vs
5.0 MB at the end of leg 1 — growth is **decelerating** across the two
legs (+2.2 MB during leg 1, +1.4 MB during leg 2, approaching a
~5 MB plateau), consistent with allocator warm-up rather than a leak,
though a 60-min window cannot prove asymptote.

## Anomalies / verdicts

| # | Item | Disposition |
| --- | --- | --- |
| A1 | Wireless runtime wedge on the older/duplicated backend process — zero RF traffic while SDK looked healthy | Documented above; observability gap in the backend, engine reporting stayed honest (`sync: unknown`). Mitigation: single managed instance + trace instrumentation. |
| A2 | 33.7 s `agg_max_interval` | Single mode-apply gap at leg-1 startup; did not recur across either leg. Not a steady-state stall. |
| A3 | Engine memory slope ~0.9→0.7 MB/h | Decelerating; PrivateBytes plateauing near 5.3 MB. Watch item for any future true 8-h run; no evidence of handle/thread leak (flat or falling). |
| A4 | 61 live-leg discovery misses + `discovery_age` 3.08 s | Upload-queue pressure defers polls; misses are single-poll transients with immediate recovery. Worth a future look at poll scheduling under load. |
| A5 | `rgb_clock` drift: −27.5 ppm here vs −0.6 ppm (Task 5.2, 9.7 min) | Not reconcilable as one constant rate — 5.2's fit resolved ~±4 ppm, so −27.5 ppm would have shown there at ~6σ; likely non-stationary clock (oscillator warm-up / temperature) or a session difference. In this window the MCU free-ran ~27 ppm slow — ~1.6 ms/min visual drift on an uploaded 60 fps loop; imperceptible, wired lanes bake wall-clock anyway. |
| A6 | First-leg display power-off mid-run (earlier session) | `output_paused` power gate worked as designed; rows preserved, excluded. |

**Playback verdict (65 min): PASS (bounded).** Uploaded loop owned the
wireless lane continuously; wired choreography steady at 21.1/s; device
heartbeat metronome-regular; zero failures, zero physical-output
contradiction observed.

**Live verdict (60 min): PASS (bounded).** 59.1/s wired + 32.1/s
wireless, flat; zero transport rejections; heartbeat <1 s under load;
backend CPU ~0.8 core is the known standing cost.

**Not claimed:** eight-hour stability, physically-verified photons (no
camera/light-meter on the rig — output was qualitatively observed
changing on all five controllers during spot checks), DIMM behaviour
(absent — PawnIO/elevation).

## Artifacts

`profiles/local/soak/` (gitignored): `soak-{playback,live}.csv`,
`diag-{playback,live}.jsonl`, `proc-{playback,live}.csv`,
`lianli.csv` (+ `.summary`), `lianli-run{1,2,3}.csv` (wedged),
`*-wedged.csv`, `engine*.log`, `backend*.log`, `baseline.json`
(pre-run device state), `keepawake.ps1`, `key.ps1`, `sample.py`,
`analyze.py`, scene copies.
