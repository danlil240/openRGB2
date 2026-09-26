# Milestone 2 uploaded playback result

Status: offline implementation verified; physical qualification **incomplete**. Date: 2026-09-26. Source: the `milestone-2` worktree based on `82d854f` plus the changes in this worktree. No presentation FPS is claimed.

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

## Authorized hardware session

The selected 120 LED group was exercised through a private backend on port 6744 and `lianli_bench` on 2026-09-26. State was captured before each of nine 15-second preset holds (three presets at speeds 10, 100, 200); every producer process exited successfully and reported restore complete. FanControl remained running. The trace from this **pre-fix** backend recorded only three admitted full uploads, two retries, eight keep-alives, and 17 discovery polls. Those counters stopped increasing early in the session despite continued SDK submissions and worker ticks. Thus SDK apply/restore success does not establish playback for the later holds. Trace and private device identifiers are in gitignored `profiles/local/m2-2026-09-26/final/`.

The terminal path consistent with this trace is a discovery layout/ownership mismatch: other terminal paths would have incremented failure or foreign-master counters, which stayed at zero. A regression now covers one inconsistent fan-count reply followed by recovery and a persistent motherboard RGB ownership mismatch. Writes stop on the first incompatible reply; three consecutive incompatible replies cause a terminal error. The rebuilt backend could not be started for validation because Windows Application Control blocked the executable. No workaround was applied. The exact mismatch field, visible playback, cycle duration, clock-to-phase relationship, and steady-state upload rate therefore remain unverified.

## Physical qualification still required

Once the rebuilt backend can run under the normal application policy, repeat each preset at speeds 10, 100, and 200 with saved state and a hard stop. Record the exact binary, firmware, payload metadata, total RF traffic after convergence, visible frame intervals, full-cycle duration, and clock-to-loop phase with a high-speed camera or photodiode. Verify Live → playback → Live, rapid edits, pause/stop, disconnect/reconnect, and FanControl/sensors. The Milestone 2 playback pass and physical 60 FPS target remain unresolved.
