# E-12c: Tick-cadence and per-lane latency histograms

Status: proposed · Size: S · Source: audit 2026-11-28 E-12 (part c) · Related: E-2 / P-8

## Goal

Diagnostics shows the real distribution, not min/max/mean, of three
things:

- how evenly the 60 Hz scene clock ticks
- how long each frame waits before it is sent, per controller
- how long each send takes

That turns "the wireless fans look steppy" or "the tick runs in
15.6 ms steps" (E-2) into numbers before and after a fix.

## Current state

- **`DeliveryStats`** (`src/engine/EngineTypes.h:242-267`) keeps
  counters plus `last`/`min`/`max`/mean of transport-completion
  intervals, recorded by `RecordTransportCompletion`
  (`src/engine/EngineTypes.cpp`, `src/engine/DeliveryPipeline.cpp:600-601`).
  There are no percentiles and no latency.
- **Timestamps already available:**
  - `RgbFrame::sample_time` is the scene sample time, copied from
    `EvaluatedFrame::sample_time` by `DeviceMapper`
    (`src/engine/DeviceMapper.cpp:405`).
  - `Lane::last_send` is when the send starts.
  - The completion is stamped in the delivery path.
  - So both "sample → send start" (queue plus budget wait) and
    "send start → completion" (transport) can be measured with no new
    clock reads.
- **Scene clock:** `SceneWorker::Run` waits with `cv_.wait_until`, then
  calls `Step(Clock::now())` (`src/engine/SceneWorker.cpp:330-357`).
  The actual tick-to-tick interval is not recorded. E-2 says it
  alternates between 15.6 and 31 ms without timer-resolution management.
  That is **likely**, but not measured.
- **Wire:** `StatsToJson` (`src/ipc/EngineService.cpp:162-177`) serializes
  `DeliveryStats` for `GET_DELIVERY_STATS` and `frame_status`. The
  Dashboard, Lighting, and Layout pages read `mean_interval_s`.
  `DiagnosticsController` shows engine state.
- `MeasurementProvenance` (`EngineTypes.h:217`) labels what a number
  means. Histograms are `TransportMeasured`, never "displayed".

## Design

**`Histogram`** (Qt-free, `src/engine/Histogram.h`): a fixed set of
log-ish bucket edges in ms:

`0.5, 1, 2, 4, 8, 12, 16, 17, 20, 25, 33, 40, 50, 75, 100, 250, +inf`.

- The edges straddle 16.7 ms and 33.3 ms on purpose, so tick slip and
  wireless pacing are visible.
- Each histogram keeps `uint32_t counts[17]`, `total`, and `sum_ms`.
- `Percentile(p)` gives the bucket upper edge. That is honest about
  bucket resolution: it is not an interpolated fake precision.
- `Reset()`. There is no allocation on the hot path.

**Where it records:**

- **`SceneWorker`:** `tick_interval` is the delta between consecutive
  `Step` calls on the worker thread. It is recorded under the existing
  `mutex_` and read through a new `TickStats()` snapshot. Gated ticks
  count too: the clock keeps ticking.
- **`DeliveryPipeline` lane** (under the stats lock that already
  exists):
  - `queue_ms = last_send - frame.sample_time`
  - `send_ms = completed - last_send`
  - Also in the aggregate.
  - Synthetic frames (playback bakes, stop-black) are excluded from
    `queue_ms`, because their `sample_time` is not a scene sample.
    They count in `send_ms`.

**Windows.** Histograms are cumulative since engine start or a reset.
A `reset_stats` verb clears them so a before/after comparison is easy.
No time-windowed rings: they add complexity for little gain.

**Wire.**

- `GET_DELIVERY_STATS` per controller and the aggregate gain
  `queue_hist` and `send_hist`. Each is
  `{edges_ms:[…], counts:[…], total, p50, p95, p99}`.
- `GET_STATE` gains `scene.tick_hist`.
- `frame_status` events do not carry histograms (a size budget). The
  Diagnostics page pulls them at 1 Hz while it is open, the same
  re-pull pattern B-1 used.
- `reset_stats` goes on `OUTPUT_COMMAND` (R-1/E-12a). If neither has
  landed, `GET_DELIVERY_STATS {reset:true}`.

**UI:** a "Timing" card on the Diagnostics page.

- One compact bar row per histogram: the scene tick, and per lane
  queue and send.
- p50/p95/p99 text, and a Reset button.
- The `DiagnosticsController` holds the pulled maps. QML draws plain
  rectangles, so no chart dependency is needed.

**Rejected alternatives**

- HDR-histogram style dynamic buckets: overkill for one machine.
- ETW tracing: needs elevation and tools. It is a fine one-off, but
  not an always-on view.
- Measuring "displayed" latency: the engine cannot observe photons,
  and the provenance rules forbid claiming it.

## Steps

1. `Histogram` with edges, percentiles, and reset. Test: new
   `histogram_test` (engine). It covers bucket boundaries, percentiles
   on known distributions, and an empty histogram.
2. `SceneWorker` tick recording. Test: `scene_worker_test`. Synthetic
   `Step(now)` at 16.7 ms then 31 ms lands in the 17 and 33 buckets.
3. Lane `queue_ms` and `send_ms`, excluding synthetic frames from
   `queue_ms`. Test: `delivery_pipeline_test` (drain with controlled
   `now` values and a fake transport that sleeps for a fixed time), and
   a wireless-budget lane showing ~30 ms queue values.
4. Wire fields and `reset_stats`. Update `docs/ipc-protocol.md` §5.
   Test: `ipc_engine_service_test`.
5. Diagnostics Timing card. Tests: `ui_diagnostics_test` and
   `qml_diagnostics_test`.
6. Capture a baseline on Daniel's PC before E-2 lands: 10 min, live
   scene. Store it in `docs/benchmarks/` so E-2 has a before/after.

## Tests

- Engine label: `histogram_test` (new), `scene_worker_test`,
  `delivery_pipeline_test`, `ipc_engine_service_test`.
- UI label: `ui_diagnostics_test`, `qml_diagnostics_test`.
- **Manual:** the step 6 baseline, and after E-2 the same capture to
  confirm the tick distribution collapses around 16–17 ms. That
  outcome is expected, not yet measured.

## Risks and open questions

- **Daniel:** E-2 before or after this? This plan recommends landing it
  first so E-2's effect is measured, not assumed.
- `sample_time` and `last_send` come from the same steady clock, so the
  deltas are valid. Across a lane reset (a topology generation change),
  the stats keep accumulating. That is acceptable for a cumulative view.
- Extra work per send: two subtractions and one bucket search over 17
  edges under a lock that is already held. That is negligible (a guess,
  not profiled).
