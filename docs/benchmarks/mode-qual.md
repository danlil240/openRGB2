# Mode Qualification — Task 5.2 (Milestone 5)

Physical + automated qualification of **Live effects** and **uploaded
Animation playback** across the complete *available* RGB topology, plus
rapid mode/parameter-change storming, device disconnection during upload,
reconnect recovery, and multi-minute playback clock-drift measurement.

## Builds under test

| Component | Version |
| --- | --- |
| `openrgb2` engine + `openrgb2-ctl` | branch `milestone-5`, working tree at `8b7780e` + task changes (committed on top) |
| Managed backend | `vendor/OpenRGB` — `danlil240/OpenRGB` fork @ `53a477e`, branch `lianli-wireless` (+ working-tree mods per `vendor/provenance/import-hashes.txt`); self-identifies as `OpenRGB v1.0+ (git)` |
| Backend deployment | `vendor/OpenRGB/OpenRGB Windows 64-bit/OpenRGB.exe`, launched externally: `--server --server-port 6744 --localconfig`, cwd `profiles/local/modequal` |
| Trace | `OPENRGB_LIANLI_TRACE=…/lianli.csv`, `OPENRGB_LIANLI_TRACE_MAX_ROWS=300000` |

## Topology observed (real run)

5 controllers enumerated via SDK on `127.0.0.1:6744`:

| # | Device | LEDs | Qualification coverage |
| --- | --- | --- | --- |
| 1 | Gigabyte AORUS GeForce RTX 5080 MASTER ICE (GPU) | 26 | live + baked playback frames |
| 2 | Gigabyte X870E AORUS ELITE WIFI7 ICE (motherboard) | 2 | live + baked playback frames |
| 3 | UNI FAN SL V3 Wireless 66:E1 (Lian Li wireless group, serial on file in `profiles/local/`) | 120 | live + uploaded playback (owned lane) |
| 4 | Logitech G512 RGB Mechanical Gaming Keyboard | 108 | live + baked playback frames |
| 5 | Razer Basilisk V3 Pro (Wired) | 13 | live + baked playback frames |

**Not present / not qualified:** DIMMs — the backend logged PawnIO
permission failures (unelevated); no SMBus DRAM controller enumerated.
Motherboard coverage is limited to its HID RGB controller (2 LEDs);
several motherboard zones report 0 LEDs. This is a hardware/access
limitation of the run, not an engine defect.

**Scene binding caveat:** the Razer controller's enumerated serial is
space-padded; the profile binding needed the serial dropped to resolve.
Recorded for profile hygiene.

## Measurement honesty

- All engine numbers are **transport-boundary** measurements: SDK packets
  accepted/sent by the backend. They do *not* prove photons were emitted.
- `playback` status reports `sync: unknown` — there is no clock channel
  over the SDK wire; synchronization health stays unknown unless measured
  externally.
- The only device-side observable is the backend's own `rgb_clock`
  trace: the Lian Li wireless MCU's playback tick counter, polled by the
  backend every ~3 s while an uploaded loop is desired.
- `upload: active` means the backend acknowledged the uploaded loop is
  armed/selected; physical display correctness is *unverified* (no
  light-meter evidence in this run).

## Results

### 1. Live effects — full topology (transport level)

~60 s steady-state window after display-on (power gate released):

| Controller | completed sends | mean interval | implied rate |
| --- | --- | --- | --- |
| GPU | 1410 | 17.2 ms | ~58 Hz |
| Motherboard | 1410 | 17.2 ms | ~58 Hz |
| Lian Li wireless group | 769 | 31.5 ms | ~32 Hz |
| Logitech G512 | 1410 | 17.2 ms | ~58 Hz |
| Razer Basilisk V3 Pro | 161 | 16.9 ms | ~59 Hz (shorter window — joined after binding fix) |

- Wired/peripheral lanes run at the 60 Hz scene clock (interval ≈ scene
  tick; unchanged-frame suppression and pacing keep means ~17 ms).
- Wireless lane self-paces at ~32 Hz — consistent with the disclosed
  content-dependent wireless ceiling (Task 5.1); the group takes
  complete-controller writes and paces its own upload.
- High early `rejected` counts (≈1874/controller) are the **power-gate
  backlog**: Windows reported display-off for the first minutes and the
  engine correctly discarded pending frames rather than queueing a flood.
  A synthetic F15 keypress released the gate. Power lifecycle behaved as
  designed; rejection counters are cumulative and include that window.

### 2. Animation playback — upload + baked lanes

- `set-playback animation_playback`: applied → `upload: active`,
  `control: playing`, `effective_frame_count: 144`,
  `frame_interval: 16.67 ms` (≈60 fps nominal, 2.4 s cycle),
  `sync: unknown` (honest).
- **Lane ownership verified on the wire:** the wireless lane's transport
  counter froze (924 completed) for the entire playback window — zero
  live sends leaked onto the owned group while the uploaded loop runs.
- Wired lanes (GPU/MB/G512/Razer) kept producing baked timeline frames
  at the wired frame cadence (~22–45 Hz observed window means including
  suppression).
- Backend trace counters for the run: `uploads_admitted 832`,
  `upload_retries 1`, **`upload_failures 0`**, `set_desired_rejected 0`,
  `keepalive_failures 0`, `discovery_misses 0`, `foreign_master_events 0`,
  `transport_revalidations 0`, upload compression 2,482,560 → 315,690
  bytes (~87%).

### 3. Playback clock drift — 9.7 minutes

Method: backend `rgb_clock` trace rows — device playback tick counter vs
backend wall time — sampled every ~3.0 s while the uploaded loop held.
195 samples over 582.9 s, linear fit:

- **Tick rate: 1599.9991 ticks/s vs 1600 nominal → −0.6 ppm drift.**
- Residual RMS 15.7 ticks (≈9.8 ms) — sample-time/scheduling jitter, not
  accumulated phase error; no systematic slope beyond the fit.
- Interpretation: the fan group's playback clock tracks host wall time to
  ~1 ppm over ~10 min — a ~60 fps uploaded loop drifts ≪1 ms/min against
  the baked wired timeline. No observable long-term drift at this window;
  the honest status stays `sync: unknown` on the wire (this measurement
  is backend-trace evidence, not a wire-visible sync state).

### 4. Rapid mode/parameter storm (physical, over real IPC)

50 `openrgb2-ctl set-playback` requests in ~15 s — alternating
`animation_playback speed={40..200}`, `reverse`,
`control=paused|playing`, and `live_effects`:

- 2 applied immediately, 48 raced (accepted `pending`, then superseded).
- **No wedge, no deadlock, no lost requests** — every call returned a
  structured reply; the pipeline serialized superseded applies.
- Final converged state ~12 s later: `pending: false`,
  `mode: animation_playback`, `upload: active`, `control: playing`,
  `parameter_revision: 52` — the last effective request applied.
- Superseded requests never surfaced as failed *or* silently stuck; the
  only states observed were pending → applied (latest) or pending →
  superseded (earlier requests) — matching design: latest-request-wins,
  honest pending.

### 5. Disconnect during upload + recovery (physical)

Sequence: `live_effects` applied → `animation_playback` requested →
backend process killed **mid-apply** → backend relaunched with the
`Lian Li Wireless` detector disabled → relaunched re-enabled.

| Stage | Observation |
| --- | --- |
| Kill during apply | Within ~6 s: `upload: failed`, `detail: "the wireless group disappeared"`, `pending: false`. Requested mode retained as `animation_playback`. **No partial-success claim.** |
| Backend up, device absent | 4 controllers enumerated; engine reconnects, revalidates identity; failure state persists honestly — not cleared, not silently "applied". |
| Backend up, device back | 5 controllers; identity revalidated (serial match); restore transition issued; `upload: active`, `detail: null`. Recovery automatic, no operator action. |

### 6. Automated fake-backend qualification (`e2e_mode_qual_test`)

`tests/engine/e2e_mode_qual_test.cpp` — production `EngineHost` +
`PlaybackController` + `DeliveryPipeline` + `OpenRgbClient` over the
in-process `FakeSdkServer` (injected `IStreamTransport`, no sockets):

- Request storm: 25 accepted / 35 rejected-with-reason — no wedge, every
  rejection carried a reason; converged, no permanent pending.
- `UPDATEMODE`/`SETCUSTOMMODE` reflected in fake-backend observable
  state; refresh/revalidation reads real fake state.
- Controller removal mid-run (positional enumeration shift, matching
  real backend behavior) → honest degradation; reinsertion →
  `RevalidateResult::Restored` (enum index 5) — restore issued.
- **Found and fixed a real production bug**: `Poll()` previously called
  `Changed()` while holding `mutex_` on the missing-wireless path —
  same-thread `resource deadlock` (`PlaybackController.cpp` ~L1131;
  `Changed()` now runs outside the lock at all call sites).

## Verification

- `ctest -C Release`: **49/49 pass** (includes `e2e_mode_qual_test`,
  2.51 s).

## Limitations / unresolved concerns

1. **DIMMs not qualified** — PawnIO permission failure; needs elevated
   backend or signed-driver path. Recorded, not silently skipped.
2. **Transport ≠ display** — all "applied/active" claims are
   backend-acknowledged transport state; LED light output was observed
   qualitatively but not instrumented (no photodiode/camera evidence).
3. **`sync: unknown`** — no wire clock channel; drift was measured via
   backend trace only. Wireless-vs-wired visual phase lock is claimed
   only by the −0.6 ppm drift evidence, not a wire state.
4. **Competing ownership**: engine diagnostics logged LGHUB processes
   (Logitech ownership conflicts) — expected on this machine; G512
   frames still flowed, but physical flicker/contention was not
   instrumented.
5. **Power gating**: display-off discards pending frames by design —
   qualification required the display on; the early reject counts in
   `stats` are the gate backlog, not delivery failures.
6. Backend restart truncates `lianli.csv` — drift segment preserved
   separately as `lianli-drift.csv`.

## Artifacts

`profiles/local/modequal/` (gitignored): `lianli-drift.csv` +
`lianli.csv.summary` (backend counters), `storm.log` (50 request
replies), `hold.log` (16×30 s playback polls, all `active`),
`engine.log`, `baseline.json`, `final-*.json` (stats/playback/
snapshots/diag), `scene-live.json`, `scene-playback.json`,
backend `config/` + `logs/`.
