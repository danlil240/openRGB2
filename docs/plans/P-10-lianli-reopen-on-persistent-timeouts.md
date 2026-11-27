# P-10: Lian Li transport never reopens on persistent timeouts

Status: proposed · Size: S · Source: audit 2026-11-28 P-10

## Goal

If the TX/RX dongle handles get stuck returning timeouts, the wireless runtime
should drop them and reopen within a few seconds, not retry the same dead
handles forever. Each failing write should also stop holding up the service
worker, and with it every other fan group, for a full second.

## Current state

- **Close happens only on hard errors.**
  - `LianLiWirelessUsbLink::Fail` (`LianLiWirelessTransport.cpp:159-169`) calls
    `Close()` only when `NeedsReopen(rc)` holds (`:45-50`): `NO_DEVICE`, `IO` or
    `PIPE`.
  - A `LIBUSB_ERROR_TIMEOUT` on write fails the call but keeps the handles.
  - Read timeouts return 0 bytes, by design (`ReadSome`, `:285-301`).
- **Timeouts.** `WRITE_TIMEOUT_MS` = 1000 and `QUERY_TIMEOUT_MS` = 1000 (`:26-27`).
- **The runtime recovers without touching the link.**
  - It counts consecutive failures per kind (`poll_failures`, `send_failures`,
    `keepalive_failures`). At `cfg.max_send_failures` = 5
    (`LianLiWirelessRuntime.h:93`) it calls `TransportFailed`
    (`LianLiWirelessRuntime.cpp:154-160`, call sites `:340-342`, `:523-525`,
    `:681-683`).
  - After `reconnect_ms` = 3000 it calls `RetryTransport` (`:162-189`,
    `:316-325`), which resets the runtime state.
  - The link is never told to close. `EnsureOpen` (`Transport.cpp:171-184`)
    returns true at once on the same handles, and `ConnectionGeneration` never
    changes. A stuck handle is therefore never reopened.
- **Shared link.** All groups share one link through
  `LianLiWirelessService::ObservingLink` (`LianLiWirelessService.h:152-181`).
- **What triggers it is speculative.** Nothing in `docs/benchmarks/` records a
  timeout storm.

## Design

1. **Interface hook.** Add `virtual void DropConnection() {}` to `IWirelessLink`
   (`LianLiWirelessRuntime.h:49-59`). Its default is a no-op, so the four test
   `FakeLink`s compile unchanged. `LianLiWirelessUsbLink` overrides it with
   `Close()`, which bumps `connection_generation`. `ObservingLink` forwards it.
2. **Runtime.** `TransportFailed` calls `link.DropConnection()`. The next
   `RetryTransport` then goes through `EnsureOpen`: it re-enumerates, keeps the
   existing 1 s `REOPEN_BACKOFF_MS`, and makes fresh handles. Other groups see
   the generation change and revalidate (`:326-330`). That is the same path a
   hard USB error already takes today.
3. **Shorter timeouts.** Lower `WRITE_TIMEOUT_MS` to 200 ms. A 64-byte bulk
   write on a healthy dongle should take about 1 ms (a guess: confirm it from
   the `usb_send` duration counters in `LianLiWirelessDiagnostics`). The worst
   case then drops from 5 s of blocked worker to about 1 s before the reopen.
4. **Fault injection for verification.** Add an env-gated fault hook in the
   transport, `OPENRGB_LIANLI_FAULT=write_timeout:<n>`, read once beside
   `OPENRGB_LIANLI_TRACE` (`LianLiWirelessDiagnostics.cpp:69-72`). It makes the
   next *n* bulk writes return `LIBUSB_ERROR_TIMEOUT` without touching the
   device. That reproduces the speculative trigger with no hardware risk: the
   fans keep running their last upload.

**Rejected alternatives:**
- A consecutive-timeout counter inside the transport: it duplicates the
  runtime's counters, and it cannot be unit-tested until P-13's libusb stub
  exists.
- `libusb_reset_device`: it re-enumerates the dongle, and its effect on RF
  pairing is unknown (guess). A close and reopen is the smallest step.
- Treating a write timeout as `NeedsReopen` at once: one slow write would bounce
  the shared link for every group.

## Steps

1. Add `DropConnection` to the interface, `ObservingLink` and the USB link. Make
   `TransportFailed` call it. Test: in `lianli_protocol_test`, a `FakeLink` that
   counts `DropConnection` calls and fails `SendChunks` 5 times. Assert exactly
   one drop, then `Boot` after `reconnect_ms`, then `Holding` once sends succeed.
2. Multi-group behaviour. Test: in `service_pending_test`, two groups on one
   fake link whose generation bumps on drop. When one group fails, the other
   revalidates and does not fail itself.
3. Lower `WRITE_TIMEOUT_MS`. Test: none offline (a constant). It is checked by
   the hardware step.
4. Add the fault hook. Test: once P-13's libusb stub exists, a transport test
   that sets the env var and asserts the timeout, `Close`, and the reopen.

## Tests

- `lianli_protocol_test`, `service_pending_test` (label `backend`).
- **Manual (hardware, safe).** Start the backend with
  `OPENRGB_LIANLI_FAULT=write_timeout:8` and `OPENRGB_LIANLI_TRACE=1` while
  live effects run. Expect in the trace:
  - five failures
  - `Failed`
  - a link close (generation +1)
  - `Boot`, then `Searching`, then `Holding` within about 5 s
  - the other group revalidating and recovering

  Repeat with `:1000` to check that the worker never blocks other groups for
  more than about 1 s per tick.

## Risks and open questions

- The real-world trigger is still speculative. Should the fault hook ship in
  release builds (env-gated, like the trace) or only in debug builds? It is
  Daniel's call; the recommendation is env-gated, because verifying needs the
  release backend.
- Closing the shared link on one group's failure resets keep-alive timing for
  all groups. That is acceptable, because hard errors already do it, but it is
  worth one look in the trace.
- 200 ms is a guess. Take the measured p99 `usb_send` time first and use about
  10 times that value.
