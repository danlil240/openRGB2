# Milestone 5 cross-device skew result

Status: **measured on the real rig**, 2026-09-27 session. All numbers below
are *transport-admitted* timing — frames accepted by the SDK socket/backend —
**never** "displayed". Physical LED timing requires hardware observation
(camera/photodiode) which is not available; see §6.

## 1. Scope and method

Question (plan Milestone 5): *measure cross-device skew and add latency
compensation only where justified.* "Skew" here is the spread of times at
which frames stamped with the **same logical timestamp** reach each
controller's transport — the engine-side part of any visible misalignment.

**Harness:** `tools/bench/skew_bench` (new opt-in tool, same pattern as
`lianli_bench`). It drives the **production delivery objects**, not a
reimplementation:

- a real `DeliveryPipeline` — per-controller lanes, latest-wins depth-1
  pending, the engine's qualified budgets (`IsWirelessGroupController` →
  30 ms, all others unpaced, mirroring `EngineHost::BudgetFor`);
- a real `sdk::OpenRgbClient` over `TcpSocket` — one mutex-serialized TCP
  stream, exactly what the engine's lanes contend on;
- an instrumented `IFrameSink` (`TimingSink`) between them recording each
  send's call/return `steady_clock` timestamps.

Each producer tick stamps one `sequence` + one `sample_time` across all
lanes — the scene worker's submission shape. Frames use a moving pattern
(gradient/band/entropy) so unchanged-frame suppression cannot eat the
measured sends. Per-send CSV + summary: send duration (submit → transport
return), offset from the tick's logical time, per-tick cross-lane spread,
and per-lane sequence lag. The run also times the SDK's only request/reply
verbs (`REQUEST_CONTROLLER_COUNT`, `REQUEST_CONTROLLER_DATA`) as the
protocol-level submit→ack probe — `UPDATELEDS` itself is fire-and-forget
and has no ack. Per-controller `DeliveryStats` are printed (the same
counters `GET_DELIVERY_STATS` exposes over engine IPC).

**Rig/session:** private backend `vendor/OpenRGB/release/OpenRGB.exe`
`--server --server-host 127.0.0.1 --server-port 6744 --noautoconnect
--localconfig`, private config `profiles/local/skew-2026-09-27/config`,
backend trace `OPENRGB_LIANLI_TRACE` active. 5 controllers enumerated:
GPU 26 LEDs (I2C), Razer mouse 13 (HID), X870E motherboard **2 LEDs**
(the 8/16/8 header gap from `baseline.md` persists), G512 keyboard 108
(HID), wireless group 120 (`Wireless:`). DIMMs still absent
(non-elevated backend). G Hub owns the keyboard; FanControl (PID 15100)
ran throughout; backend counters clean (0 upload failures, 0
foreign-master, 0 revalidations, keepalive max 877 ms).

Runs: gradient sweep 30+60 fps × 20 s/rate, entropy 60 fps × 20 s.
State captured and restored per run (exit 0, "restore complete").

## 2. What was measured

### 2.1 Per-controller submit → transport-return latency

Send duration = time inside the serialized `OpenRgbClient::SubmitLatest`
(mutex queue wait + validate + packet build + TCP write). Gradient run,
µs:

| Lane | Device | 30 fps p50/p95/p99/max | 60 fps p50/p95/p99/max |
| --- | --- | --- | --- |
| 0 | GPU 26 LED | 94/243/302/352 | 77/197/250/594 |
| 1 | Razer mouse | 119/250/292/321 | 105/203/240/538 |
| 2 | Motherboard | 155/255/306/381 | 119/206/237/557 |
| 3 | G512 keyboard | 161/270/314/383 | 135/219/261/469 |
| 4 | Wireless (paced) | 187/290/323/390 | 54/100/212/280 |

Entropy @60 fps was within noise of gradient (p50 53–134 µs). Send
durations are ~50–200 µs — the whole 5-controller burst costs <1 ms of
serialized socket time. The wireless lane's *lower* 60 fps durations are
expected: it sends on budget openings when the wired lanes are mostly
idle, so it rarely queues behind other sends.

### 2.2 Protocol submit→ack probe (request/reply RTT, idle baseline)

50 rounds each, µs — the only true "ack" the SDK offers (UPDATELEDS has
none): `REQUEST_CONTROLLER_COUNT` p50 ≈ 29–33; `CONTROLLER_DATA` per
controller p50 ≈ 31–38, worst p95 94 (Razer), max 137 µs (G512). These
bound the server round-trip; the write path is faster still since it
needs no reply.

### 2.3 Inter-controller skew at the same logical timestamp

Per-tick spread of transport call/return times across lanes (µs):

| Scope | Rate | call-spread p50/p95/p99/max | done-spread p50/p95/p99/max |
| --- | --- | --- | --- |
| Wired lanes (0–3) | 30 fps | 25/48/63/82 | 160/241/290/322 |
| Wired lanes | 60 fps | 22/47/61/519 | 130/193/224/450 |
| Wired lanes (entropy) | 60 fps | 22/46/63/125 | 132/196/225/308 |
| **All 5 lanes** | **30 fps** | **28/50/68/89** | **177/262/304/322** |
| All 5 lanes | 60 fps | 15394/15955/30976/31184 | 15385/15965/31013/31221 |

**The headline:** at 30 fps — the maximum rate the wireless transport can
sustain (~33 admitted/s ceiling) — *all five* controllers receive their
same-tick frame within **~0.3 ms** of each other. At 60 fps the four
wired lanes stay within **~0.2–0.3 ms**; the wireless lane is not part of
the same tick at all — it is on a different effective cadence (see 2.4).

### 2.4 Wireless pacing lag (structural, not jitter)

At 60 fps the wireless lane completed 645–646 of 1200 ticks (≈54% — the
30 ms budget plus backend admission ceiling); its per-send offset from
its own tick was bimodal: ~0 µs when the budget was already open, else a
uniform wait up to ~31 ms (p50 ≈ 15.5 ms, p95 ≈ 31 ms). Sequence lag:
p50 = 0, max = 1 tick — it always sends the *newest* pending frame, never
a backlog. At 30 fps its tick offset collapses to p50 ≈ 245 µs — the
budget reopens inside every 33.3 ms tick, so the whole topology is
aligned to sub-millisecond again.

Backend trace (device-side, the wireless path's closest observable):
`set_desired` → next RF `upload` gap p50 ≈ 25 ms under saturation
(includes time behind an in-flight upload); upload duration p50 ≈ 25.7 ms,
p99 ≈ 35 ms; 2740 uploads, 1 retry, 0 failures; `lock_wait` ≈ 0 (the M2
decoupling holds). So the wireless group's *physical* update latency is
dominated by ~26–35 ms of RF serialization — its LEDs are on a ~30 Hz
device timeline regardless of submit timing.

### 2.5 Real-engine corroboration

`openrgb2-engine --sdk` with the live smoke scene drove the wireless lane
through the full production path (scene worker → mapper → lane → same
client): `GET_DELIVERY_STATS` showed completed sends at mean interval
**31.0 ms** (min 30.2, max 46.9) — statistically identical to the
harness's wireless lane (31.1/30.6/45.2). The harness numbers therefore
represent the real engine path, not just the real classes. (The scene
binds only the wireless group; wired lanes were measured by the harness
runs.) A display-off pause during the run correctly gated output — the
power lifecycle working as designed.

## 3. Verdict: latency compensation **not justified** on this topology

Threshold reasoning: cross-device delay is visually relevant only when
same-tick transitions land visibly apart — a conservative bound is a
large fraction of one 60 Hz frame (~8–16 ms); below ~1–2 ms no observer
can distinguish order. Measured:

- **Wired-vs-wired skew: ~0.2–0.3 ms (p99 ≤ 0.31 ms).** ~50× below the
  relevance bound. Nothing to compensate.
- **Wired-vs-wireless at ≥30 Hz producer rate: sub-millisecond.** The
  full topology is already aligned when the producer rate is within the
  wireless ceiling.
- **Wired-vs-wireless at 60 fps: ~15–31 ms phase offset** — but this is
  *structural*: the RF transport admits ~33 uploads/s and each upload
  serializes ~26–35 ms. The frames already go out as early as physics
  allows; there is no lead time to give the wireless lane, and delaying
  the wired lanes would only degrade the fast path to mask a rate
  mismatch (wired ~60 Hz vs wireless ~30 Hz effective) that compensation
  cannot remove. The engineered answer to this mismatch is what the
  design already does: per-controller budgets + device-side playback
  loops in `animation_playback` mode (see `playback-result.md`).

**No compensation code was added.** Adding per-lane lead/delays on this
evidence would be complexity without a measurable benefit.

Revisit triggers (when compensation would become justified): a transport
whose per-send blocking approaches a frame interval (>~4 ms p95), wired
lanes that share a slow serial path, or a slow lane whose lag is
*schedulable* lead-time rather than a physical admission ceiling.

## 4. What was not measured

- **Physical LED timing.** Camera/photodiode still unavailable — LED
  presentation skew, the wireless clock→displayed-phase relationship, and
  any post-wire (HID/I2C/USB→LED) per-device latency are **unknown**.
  The numbers above end where the backend accepted the bytes; the
  wireless trace extends one step further (RF admission), still not
  light-out.
- Motherboard 8/16/8 headers and DIMMs (enumeration gaps — see §1).
- Playback-mode baked-vs-device-loop alignment (a different sync
  mechanism — `PlaybackClockSync`; out of scope for this measurement).

## 5. Artifacts

`profiles/local/skew-2026-09-27/` (gitignored): `skew-gradient.csv`,
`skew-entropy.csv` (per-send rows), `lianli.csv` + `.summary` (backend
trace/counters), `engine.log`, `engine-state.json`, backend private
`config/`.
