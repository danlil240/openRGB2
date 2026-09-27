# Benchmark producer (Milestone 1)

`lianli_bench` — opt-in physical benchmark producer for the Lian Li
wireless path. Streams Direct-mode color updates over the OpenRGB SDK
(loopback TCP) with deterministic workload patterns, a rate sweep, saved
state capture/restore, and a hard stop. It never touches USB, fans, PWM,
or sensors — the backend owns all hardware.

**Physical runs are opt-in and require the authorized hardware session
procedure in `docs/benchmarks/baseline.md`.** Read-only usage:

```bash
build/tools/bench/Release/lianli_bench.exe --list          # enumerate devices
build/tools/bench/Release/lianli_bench.exe --device "Lian Li" --dry-run
```

Typical session run (writes to real devices):

```bash
build/tools/bench/Release/lianli_bench.exe \
    --device "Lian Li" --pattern gradient \
    --sweep 10,20,30,40,60 --duration-s 30 \
    --out session/sdk-gradient.csv
```

Firmware-mode baseline (uploaded preset, runs autonomously on hardware):

```bash
build/tools/bench/Release/lianli_bench.exe \
    --device "Lian Li" --mode "Spectrum Cycle" --speed 100 --hold-s 15
```

`--mode` applies the named mode (with optional `--speed` override patched
into the raw mode description), holds `--hold-s` seconds so the backend
upload/resend-until-ack traffic is observable in the trace, then restores.

Ctrl+C stops early and still restores the saved mode and colors. Exit code
3 means the run was aborted or restore was incomplete — check devices
manually.

Layout:

| File | Purpose |
| --- | --- |
| `sdk_wire.{h,cpp}` | Pure SDK wire-format parsing/building (protocol capped at 4); offline-tested by `tests/backend/bench_sdk_wire_test.cpp` |
| `sdk_client.{h,cpp}` | Minimal synchronous winsock TCP client with version handshake |
| `bench_producer.cpp` | CLI, workload patterns, rate sweep, state capture/restore, CSV output |
| `skew_bench.cpp` | M5 cross-device skew harness (below) |
| `skew_stats.h` | Pure skew/distribution aggregation; offline-tested by `tests/backend/skew_stats_test.cpp` |

SDK send success is not hardware confirmation — physical presentation
requires the measurement method in `docs/benchmarks/baseline.md` §6.5.

## Skew harness (Milestone 5)

`skew_bench` measures frame-application skew across controllers by
driving the **production delivery path** — a real `DeliveryPipeline`
(per-controller lanes, the engine's wireless 30 ms budget, latest-wins)
sending through a real `OpenRgbClient` — behind an instrumented
transport sink that timestamps every send's call/return. Each producer
tick stamps one logical timestamp (one sequence + one `sample_time`)
across all lanes, mirroring the scene worker's submission shape.

```bash
build/tools/bench/Release/skew_bench.exe --list            # enumerate (read-only)
build/tools/bench/Release/skew_bench.exe --rtt-only        # request/reply RTT probe (read-only)
build/tools/bench/Release/skew_bench.exe --all --dry-run   # show the plan

build/tools/bench/Release/skew_bench.exe \
    --all --pattern gradient --sweep 30,60 --duration-s 20 \
    --rtt-rounds 50 --out session/skew.csv
```

Reports, per lane: send-duration distribution (submit → transport
return), send offset from the tick's logical timestamp, sequence lag
for paced lanes, and per-tick inter-controller call/return spread —
plus the per-controller `DeliveryStats` the engine exposes via
GET_DELIVERY_STATS. UPDATELEDS is fire-and-forget, so "send complete"
means **transport-admitted, never displayed**; the RTT probe
(REQUEST_CONTROLLER_COUNT / CONTROLLER_DATA round trips on a side
connection) is the protocol's only true submit→ack observable.
Physical LED skew requires hardware observation — see
`docs/benchmarks/skew-result.md`.
