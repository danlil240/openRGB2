# Milestone 2 uploaded playback result

Status: offline implementation verified; physical qualification **partially complete** — all three presets at all three speeds applied, confirmed, and restored cleanly on hardware, with the device RGB clock sampled at its nominal rate. Firmware presentation FPS, cycle duration, and clock-to-phase alignment remain **unmeasured** (no camera/photodiode available). Date: 2026-09-26. Backend: `release/OpenRGB.exe` at `2d3a66b`.

The three supported OpenRGB modes now share the Qt-free `src/core/playback/PlaybackLoop` builder and the imported pure renderers. It preserves the legacy full-cycle duration (`240000 / speed` ms), requests about 60 frames/s, quantizes the interval to hundredths of a 0.625 ms protocol tick, then checks the **actual** tinyuz payload against 12,288 bytes and frame count against 4,096. An oversized loop is rendered again with fewer frames and the same requested cycle duration. `GroupStatus` exposes frame count, effective duration, nominal rate, payload size, effect ID, and a timestamped clock snapshot.

## Offline observations

`playback_test` and `lianli_playback_test` with Release/MSVC and tinyuz:

| Layout and preset | Speed | Frames | Compressed bytes | Nominal playback rate | Requested cycle |
| --- | ---: | ---: | ---: | ---: | ---: |
| 120 LED Rainbow Wave | 10 | 1,440 | 9,489 | 59.99/s | 24 s |
| 120 LED Rainbow Wave | 100 | 144 | 2,695 | 59.99/s | 2.4 s |
| 120 LED Rainbow Wave | 200 | 72 | 1,559 | 59.99/s | 1.2 s |
| 208 LED Rainbow Wave | 10 | 313 | 10,669 | 13.04/s | 24 s |

These are generated/uploaded frame rates. Firmware presentation is **unknown**. The 208 LED case demonstrates that preserving duration can require a lower nominal rate. The 120 LED values above use the generic per-fan order; the hardware-specific LED order and direction are separately covered by the renderer regression suite.

## Clock and transition checks

Offline tests validate the V1 transmitter reply opcode, master identity, response length, big-endian ticks, wraparound, a backward jump, timeout, and sample age. The worker issues a query at most once per 3 seconds while a multi-frame loop is desired, after keep-alive and discovery work. A current sample is **not** proof that the device's displayed frame has a particular phase; that relation requires physical calibration.

Revisioned mode submissions reject queued live frames after entering playback, reject old parameter compiles, and reject stale playback submissions after returning to live. [The pinned upstream clock query](https://github.com/sgtaziz/lian-li-linux/blob/ebcf68ef02a39463e8295bb40ce3c5751622c496/crates/lianli-devices/src/wireless/rgb_clock.rs) supplies the validated V1 reply layout.

## Authorized hardware sessions

Two sessions ran on 2026-09-26 through a private backend on port 6744 with `lianli_bench` driving SDK mode changes. State was captured before each of nine 15-second preset holds (Spectrum Cycle, Rainbow Wave, Breathing at speeds 100, 10, 200); every producer process exited 0 and reported restore complete. FanControl remained running throughout both sessions. Artifacts (traces, per-phase counter snapshots, logs) are in gitignored `profiles/local/m2-2026-09-26/` (`verify/` = pre-fix run, `fixed/` = post-fix run).

### First session (pre-fix backend): real defect found

The trace shows the runtime entering terminal `Failed` at ~245.6 s: three consecutive discovery replies reported the target MAC with `fan_count = 0`, which the layout check counted as an incompatible layout. Only ~28 uploads were admitted before the stall; the remaining presets and all five live sweeps exited 0 producer-side while the backend dropped every frame — SDK success does **not** establish hardware delivery.

### Root cause and fix

`ParseDeviceRecord` reads `fan_count` from byte 19. A bound group always reports ≥1 fan; a zero-fan record carrying the correct MAC is a partially updated row in the receiver's discovery table under RF load. `PollAndMerge` now treats `fan_count == 0` as an incomplete observation — a miss: the last good sighting stays usable until `lost_ms`, sustained incomplete rows degrade to recoverable `Lost`, and valid rows resume playback automatically. Genuine mismatches (fan count 1–255 mismatch, `mb_rgb_sync`) keep the stop-writes → debounce → `Failed` path. Regression: `TestIncompleteDiscoveryRecord` in `lianli_protocol_test`.

### Second session (fixed backend, `2d3a66b`): clean pass

- All nine preset holds applied, confirmed via effect-ID echo, and restored; `upload_failures = 0`, `foreign_master_events = 0`, `transport_revalidations = 0` for the whole session.
- The anomaly **recurred**: 12 consecutive `layout_incomplete` rows (~3.4 s of zero-fan records, again at ~243–247 s, during the Breathing-10 hold — the same session phase that produced the original failure). The runtime rode through with no `Failed` and no write interruption, then resumed on the next valid reply. Repeatability at the same phase suggests a deterministic receiver-table refresh artifact rather than random RF noise.
- Upload traffic per phase (apply + confirm + restore): Spectrum Cycle 2–3 uploads / 5–10 RF packets; Rainbow Wave 2–4 uploads / 25–104 packets (largest compressed payload, one retry in two of three speeds); Breathing 2–6 uploads / 6–44 packets (three retries at speed 10, all during the incomplete-row window). Once an upload confirms, the hold is silent: zero RGB traffic between apply and restore — only keep-alive and discovery. No repeated full uploads in steady state.
- RGB clock: 5 queries per 15 s hold (~3 s cadence, all after keep-alive/discovery work), 45/45 valid replies across the holds. Tick progression Δticks/Δt ≈ **1600 ticks/s** — the device clock runs at the nominal 0.625 ms/tick, matching the V1 reply layout ported from pinned upstream.
- Lock wait after the pending-frame decoupling: ~8–18 µs **total** per phase (vs 9–24 ms per submission at baseline).

## Physical qualification still open

Not measurable with available equipment (no camera/photodiode): visible frame rate, actual cycle duration, and the clock-sample → displayed-phase relationship. Also not exercised this session: rapid parameter edits during a hold, pause/stop, disconnect/reconnect mid-playback, and motherboard-sync transitions. SDK apply/restore plus clock sampling prove the loop reaches and persists on the device; they do not prove what the LEDs show.
