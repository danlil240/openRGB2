# UI responsiveness — plan §6 acceptance measurement

Acceptance row: *"UI responsiveness — p95 local control response below
100 ms during full output"* (deferred from Milestone 4 task 4.8 to the
M5 measurement pass; measured here on 2026-09-28).

**Verdict: PASS — p95 = 31.1 ms vs the 100 ms bound (~3.2× headroom).**

## What was measured

Client-side wall time of one **IPC request → matching reply** over
**real named pipes**, against the **real `EngineHost` production
composition** (single-instance guard → SDK attach → enumerate → bind →
60 Hz scene worker → per-controller delivery lanes → command +
diagnostics pipe listeners), while the engine streams frames at full
rate.

Harness: `tests/engine/e2e_responsiveness_test.cpp` (CTest label
`engine`, ~12 s). The backend is the in-process fake SDK server from
`tests/sdk/fake_sdk_server.h` — a 120-LED wireless group + an 8-LED
wired header, the daniel-pc shape in miniature. The scene is a moving
world-space wave so every tick produces changing colors on all 128
LEDs (no unchanged-frame suppression collapsing the load).

The client session is shaped like the real UI's:

- HELLO handshake, then `SUBSCRIBE "all"` — request/reply round-trips
  interleave with the push channel: the initial-state burst, the
  bounded-rate `frame_status` feed, and `scene`/`playback` change
  events emitted by the mutation verbs (110 events were skipped while
  awaiting replies — the same drain discipline `openrgb2-ctl` and the
  UI wire client use).
- 300 samples round-robin over the six-verb UI surface, ~5 ms apart:

| Verb | IPC | What it stands for |
| --- | --- | --- |
| `ping` | PING → PONG | transport floor |
| `get_state` | GET_STATE → STATE | the poll every page issues |
| `get_playback` | GET_PLAYBACK → PLAYBACK | playback status read |
| `get_stats` | GET_DELIVERY_STATS → DELIVERY_STATS | diagnostics read |
| `set_playback` | SET_PLAYBACK_MODE → OK | mode-selector control (real dispatch path; idempotent accept) |
| `set_scene` | SET_SCENE → OK | **Save** — heaviest mutation: full parse → resolve → worker submit → re-bind → scene event |

## Results

Window 10.49 s; scene clock held **60.0 Hz** (629 frames produced);
delivery lanes wrote UPDATELEDS to both controllers for the whole
window (258 packets ≈ 24.6/s aggregate — per-lane budget pacing, not a
stall; frames kept producing at 60 Hz throughout).

| Verb | n | min | p50 | p95 | p99 | max |
| --- | --- | --- | --- | --- | --- | --- |
| ping | 50 | 15.16 | 15.55 | 30.71 | 31.10 | 31.10 |
| get_state | 50 | 15.22 | 15.58 | 31.05 | 31.10 | 31.10 |
| get_playback | 50 | 15.28 | 15.57 | 31.04 | 31.10 | 31.10 |
| get_stats | 50 | 15.23 | 15.56 | 31.04 | 31.29 | 31.29 |
| set_playback | 50 | 15.23 | 15.57 | 31.06 | 31.12 | 31.12 |
| set_scene | 50 | 30.91 | 31.10 | 31.61 | 46.22 | 46.22 |
| **ALL** | **300** | **15.16** | **15.57** | **31.13** | **31.61** | **46.22** |

(All values in ms.)

### Reading the numbers

Latencies quantize to ~15.6/31.2/46.7 ms bands — the production
client transport's `Sleep(1)` receive poll under the default Windows
timer resolution (~15.6 ms). One tick ≈ reply arrived within the first
poll gap; heavier verbs (`set_scene` parse + re-bind + event fanout)
cost one extra tick of server work. The quantization is real
production behaviour — `openrgb2_ipc_wire` is the same client
transport the UI links — not a measurement artefact added by the
harness, and it is what dominates the result; even so the worst
per-verb p95 (`set_scene`, 31.6 ms) sits ~3× under the bound.

## What this does **not** cover — honest scope

- **QML/GUI half:** input sampling, GUI-thread marshalling, and paint
  time are not in this number. That half is bounded separately by the
  `ui_hardening_test` flood probe (400 queued `frame_status` pushes +
  one control round-trip must drain inside 2 s on the GUI thread —
  observed ~ms-scale). A full click→paint measurement would need the
  real Qt Quick scene driving real pointer input, which is not
  automatable offline; between the two bounded halves the <100 ms
  criterion is met on every measured component.
- **Backend/USB latency:** the fake SDK server answers in-process.
  Hardware-side latency is irrelevant to *UI* response by
  construction — frame writes run on delivery-lane threads and are
  never inside a request's reply path — so substituting the real
  backend could not move this number. Real-hardware IPC latency is not
  claimed, only unreachable-by-design.
- **Machine load:** measured on a quiet dev machine (the production
  engine + UI were running concurrently on a separate instance guard).
  A pathologically loaded host could inflate scheduling latency; the
  ~3× headroom plus the quantization floor suggests the bound holds
  with margin.

## Reproduce

```powershell
ctest --test-dir build -C Release -R e2e_responsiveness_test --output-on-failure
# optional JSON dump:
build\tests\engine\Release\e2e_responsiveness_test.exe <out.json>
```

The test also *asserts* the criterion (overall p95 < 100 ms, plus the
60 Hz output floor), so it doubles as the regression gate for this
acceptance row.

## Related fix

While wiring this harness, `EngineHostOptions::instance_name` was
found to be a dead option — `EngineHost` always acquired the
production guard name (`Local\openrgb2-engine`), so every
`single_instance` e2e test refused to start whenever a real engine
was running. `EngineHost::Start` now builds the guard from the option;
the four affected suites take their documented test-scoped names.
