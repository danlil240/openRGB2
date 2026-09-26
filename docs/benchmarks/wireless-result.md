# Milestone 2 live delivery result

Status: offline transport/service changes verified; physical before/after sweep **not run**. Date: 2026-09-26. A short authorized preset session exposed a runtime stall, so the live sweep was withheld. The rebuilt backend was subsequently blocked by Windows Application Control. No live presentation FPS is claimed.

The baseline in [baseline.md](baseline.md) measured about 38.3–38.5 backend uploads/s for compressible 120 LED frames and about 28.7/s for entropy frames at the saturated producer rates. Physical presentation FPS was unknown. Its `stream_ms = 25` admission floor, repeated RGB header, and USB chunk sleeps remain in this worktree; no RF pacing or header reliability change has been qualified on hardware. The baseline is the *before* dataset, not an after result.

## Offline changes

- Producers replace one pending upload under a short separate lock; the worker alone owns runtime and USB I/O. A regression blocks discovery and checks that `SetDesired` remains responsive.
- Continuous submissions can defer discovery only up to 1 second from the last sighting. A deterministic runtime test catches indefinite deferral.
- Mode revisions and upload type checks reject obsolete submissions at Live ↔ playback boundaries, including an older parameter compile.
- The V1 clock query is serialized in the worker and never substitutes for live-frame acknowledgement.
- The 32-bit upload variant is mixed into the effect ID so identical content after 256 revisions does not automatically reuse an ID.
- A transient incompatible discovery layout stops writes immediately and may recover after a valid reply. Three consecutive incompatible replies remain terminal; this prevents one anomalous reply from leaving the worker permanently idle.

## Required before/after qualification

Repeat the baseline's 10/20/30/40/60 producer sweep on the wireless group and full setup, with gradient, band, entropy, and static patterns. Record submissions, replacement count, lock wait, compression time/bytes, admitted uploads, USB duration, discovery age, keep-alive gaps, retries/resets, and coded visible p50/p95/p99 frame intervals. Confirm FanControl and sensors before and after. Qualify any change to chunk pacing and the repeated header **individually** with recovery and convergence tests, then rerun the sweep. The current code-derived 25 ms floor still precludes a claim of 60 *admitted* uploads/s; hardware presentation is unmeasured. The numerical release gate in `IMPLEMENTATION_PLAN.md` remains open and a lower ceiling needs an explicit accepted criterion.
