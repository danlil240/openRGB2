# Milestone 2 live delivery result

Status: physical before/after sweep **completed** on the fixed backend (`2d3a66b`), 2026-09-26. No live presentation FPS is claimed — all rates below are *backend-admitted RF uploads/s*, not measured LED output.

## Offline changes

- Producers replace one pending upload under a short separate lock; the worker alone owns runtime and USB I/O. A regression blocks discovery and checks that `SetDesired` remains responsive.
- Continuous submissions can defer discovery only up to 1 second from the last sighting. A deterministic runtime test catches indefinite deferral.
- Mode revisions and upload type checks reject obsolete submissions at Live ↔ playback boundaries, including an older parameter compile.
- The V1 clock query is serialized in the worker and never substitutes for live-frame acknowledgement.
- The 32-bit upload variant is mixed into the effect ID so identical content after 256 revisions does not automatically reuse an ID.
- Incomplete discovery rows (`fan_count == 0` for the target MAC) are treated as misses, not incompatible layouts; genuine mismatches keep stop-writes → debounce → terminal `Failed`. Added after the first session's stall; hardware-verified in the second.

## Measured before/after (same producer, same sweep: 10/20/30/40/60 × 30 s)

Admitted uploads per 30 s window on the 120-LED wireless group:

| Requested | Baseline (`82d854f`) | Fixed M2 (`2d3a66b`) | Pattern |
| --- | ---: | ---: | --- |
| 10 fps | 301/30 s | ~301/30 s | all |
| 20 fps | 600/30 s | ~540/30 s | all |
| 30 fps | ~900/30 s | ~806/30 s | all |
| 40 fps | ~1 150/30 s | ~980/30 s | gradient/band/static |
| 60 fps | ~1 150/30 s | ~1 000/30 s | gradient/band/static |
| 40–60 fps | ~860/30 s | ~730/30 s | entropy |

Effective ceilings: **~33.5 admitted/s compressible** (was ~38.3), **~24.5/s entropy** (was ~28.7). Inter-upload gaps in the saturated window: p50 26 ms / p95 34 ms / p99 159 ms (baseline: 26/28). The per-upload floor is unchanged at ~26 ms; the ~4 ms mean increase is preemption by keep-alive and discovery.

**The baseline ceiling was measured with a defect:** continuous submissions deferred discovery indefinitely, so sweeps ran with no sighting refresh. The M2 bound (≤1 s staleness) lets ~1.4 discovery polls/s interleave during streams — each poll is a ~20–130 ms USB read — which is where the throughput went. The lower number is the *correct* operating point; the higher one required unsafe deferral. This trade is deliberate: live streaming cannot starve identity checks.

Full-setup sweep (all 5 enumerated devices driven): wireless admissions unchanged (~33/s ceiling) — wired devices do not degrade the wireless path. Reliability across the whole session: 0 upload failures, 4 retries total (all during preset holds), 0 foreign-master events, 0 transport revalidations, keep-alive max gap ~1.1 s.

Submission-side improvements held: `lock_wait` collapsed from 9–24 ms per submission to **~1 ms total per 150 s sweep**; `replaced_pending` absorbs excess submissions (≈100 % at saturation) keeping queue depth ≤1.

## Pinned upstream comparison (sgtaziz/lian-li-linux @ebcf68e)

| Behavior | Upstream | This driver | Verdict |
| --- | --- | --- | --- |
| Pending work model | Per-device job queue, same-command dedup, 1 inflight | One desired slot per group, newest-wins | Equivalent |
| Confirmation | `rgb_upload_applied` check before and after send; resend @130 ms until observed | Effect-ID echo via discovery; drift resend | Equivalent |
| RGB header repeats | **4×, 20 ms spacing** (upload worker passes `header_repeats=4`) | 2×, transport-paced | Deferred — 0 missed-header evidence in ~17.5 k uploads; qualify individually before changing |
| Inter-packet sleep | None in send loop | 1 ms/chunk + 2 ms/packet in USB layer | Deferred — inside the 25 ms admission floor anyway |
| Motherboard RGB sync held | Waits: requeue every 250 ms until clear | Terminal `Failed` after debounce | Kept stricter: not observed on this setup; revisitable if real mb-sync transitions appear |
| Upload timeout | 3 s whole-upload deadline | Per-chunk write timeouts + retry budget | Equivalent coverage |

## Performance gate: resolved — 60 FPS live does not pass

Measured ceiling is ~33.5 admitted uploads/s (compressible), ~24.5/s worst-case entropy. Live 60 FPS over this transport is **unreachable**; the M1 prediction is confirmed with the safety-corrected runtime. Proposed release criterion: *live mode targets ≤30 fps effective device update rate; smooth ≥50–60 fps visuals are delivered through uploaded playback* (see `playback-result.md` — device-side loops run off the device clock, not the USB/RF link). This needs Daniel's explicit acceptance at release review. Physical presentation FPS remains unmeasured in both modes.
