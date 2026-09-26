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

SDK send success is not hardware confirmation — physical presentation
requires the measurement method in `docs/benchmarks/baseline.md` §6.5.
