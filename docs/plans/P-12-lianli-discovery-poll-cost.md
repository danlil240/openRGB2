# P-12: Lian Li discovery polling costs 30–130 ms of link time per poll

Status: proposed · Size: M · Source: audit 2026-11-28 P-12

## Goal

Each discovery poll should cost close to its actual transfer time instead of a
fixed silence window. With several fan groups, all groups should share one poll.
And a group that has not been seen for a while should no longer make every
frame trigger a poll. Live-effect uploads then get the link time back.

## Current state

- **Where the 30–130 ms goes.** `LianLiWirelessUsbLink::PollDiscovery`
  (`LianLiWirelessTransport.cpp:400-444`) does three things in turn:
  - `FlushInput(rx)` (`:322-339`): at least one 10 ms read, and up to 8 of them.
  - Writes the query.
  - Reads `ReadSome(rx, buf, 512 - size, timeout)` in a loop: 100 ms for the
    first read, then 20 ms each, until a read returns 0 bytes.

  One poll therefore costs at least 10 ms of flush plus 20 ms of trailing
  silence. An empty table, or a slow first reply, costs about 130 ms.
- **Why the bulk read may wait out the whole timeout (derived, not measured).**
  The read asks for up to 512 bytes. A bulk transfer ends early only on a short
  packet or a zero-length packet, so a reply made of full 64-byte packets could
  stall until the timeout.
- **The reply states its own length.**
  `ParseDiscoveryResponse` (`LianLiWirelessProtocol.cpp:115-138`) reads the
  record count from `data[1]` (clamped to `MAX_DISCOVERY_RECORDS` = 10). The
  payload is `4 + 42 * records` bytes (`RECORD_SIZE`, `LianLiWirelessProtocol.h:50-51`).
- **One poll per group.** Each `WirelessRuntime` polls separately through the
  shared `ObservingLink` (`LianLiWirelessService.h:152-181`), and every response
  is already fed into one `SightingAccumulator`.
- **The poll storm.** `WirelessRuntime::SetDesired` (`LianLiWirelessRuntime.cpp:76-85`)
  sets `next_poll_ms = min(now + poll_ms, last_seen_ms + max_discovery_defer_ms)`.
  - Once a group has been unseen for more than 1 s (`max_discovery_defer_ms`,
    `LianLiWirelessRuntime.h:84`), that value is in the past.
  - Every frame then makes a poll due, and `Tick` (`:358-361`) runs it.
  - The `Tick` reschedule is overwritten by the next `SetDesired`.
- **Counters already exist:** `usb_reads`, `usb_read_us`, `usb_read_us_max` and
  `discovery_deferred` (`LianLiWirelessDiagnostics.h:86-94`).

## Design

1. **Length-bounded read.**
   - Read in `USB_CHUNK_SIZE` (64-byte) transfers.
   - After the first chunk, if `chunk[0] == USB_CMD_SEND_RF`, compute
     `expected = 4 + 42 * min(chunk[1], 10)` and stop as soon as `out.size() >= expected`.
   - Otherwise, fall back to today's silence-terminated loop. Keep the 512-byte
     cap.
   - Bytes beyond a clamped count are drained by the next flush.
2. **Cheaper flush.** Measure first. If the data shows the RX pipe is idle after
   a length-bounded read, shorten `FLUSH_TIMEOUT_MS` (10 to 2 ms is a guess), or
   skip the flush when the previous poll consumed exactly `expected` bytes.
   Keep the full flush after any error.
3. **Shared reply.**
   - `ObservingLink::PollDiscovery` caches the last successful response with its
     time and `ConnectionGeneration`.
   - A call within `share_window_ms` (new config field, 50 ms as a guess,
     well under `poll_ms` = 300) on the same generation returns the cached
     bytes without USB I/O.
   - Each runtime still parses the response and merges it at its own `now`.
4. **Clamp in `SetDesired`.**
   - Track `last_poll_ms` (set in `PollAndMerge`).
   - After computing `postponed`, floor it at `last_poll_ms + cfg.min_poll_gap_ms`
     (new field, defaulting to `poll_ms`).
   - The freshness pull-in for a recently seen group still works, and an unseen
     group polls at the `poll_ms` rate, not on every frame.
   - Count clamps in a new `discovery_clamped` counter.

**Rejected alternatives:**
- Raising `max_discovery_defer_ms`: this hides a lost group longer, the opposite
  of the bound's intent (comment at `:73-75`).
- A service-level single poll scheduler that replaces per-runtime polling: it is
  cleaner, but it is a larger rewrite of tested state-machine code. The cache
  gets most of the gain.
- `libusb` async transfers: more code and more failure modes, for a gain the
  length-bounded read already captures.

## Steps

1. Add the `SetDesired` clamp and `last_poll_ms`. Test (`lianli_protocol_test`):
   a fake clock and link, a group unseen for 2 s, `SetDesired` every 16 ms for
   1 s. Assert at most `ceil(1000 / poll_ms) + 1` polls. Keep the existing
   freshness test green.
2. Add the shared reply in `ObservingLink`. Test (`service_pending_test` style):
   three groups and one `StepAll` with all of them due. The inner link sees one
   `PollDiscovery`, and a generation bump invalidates the cache.
3. Add the length-bounded read. Test: needs P-13's libusb stub. Script chunks for
   0, 1 and 10 records and a missing header, and assert the bytes returned and
   the number of reads.
4. Flush tuning, only after step 5's numbers. Test: the same stub, checking that
   no flush is skipped after an error.
5. Hardware measurement (below), before and after each of steps 1–3.

## Tests

- `lianli_protocol_test`, `service_pending_test`, and the new
  `lianli_transport_test` from P-13 (label `backend`).
- **Manual (hardware).** With the 2026-11 topology (several SL Wireless groups),
  run live effects for 10 minutes. Record `usb_read_us / usb_reads`,
  `usb_read_us_max` and the upload rate per group from backend counters (packet
  4000) or `openrgb2-ctl diag`. Expect the mean poll time to fall from tens of
  ms to a few ms (target, not measured). Then power one group off and confirm
  that the poll rate stays at the `poll_ms` cadence: with
  `OPENRGB_LIANLI_TRACE=1`, count the poll rows per second.

## Risks and open questions

- That the first reply chunk always starts with the header is assumed from the
  parser. Verify it with the trace before trusting `expected`.
- Is the shared window safe for groups whose discovery differs by RF channel? The
  query has no channel field (`query[1] = 1`, `:410-411`), so it looks global.
  Confirm this.
- Should the result go into `docs/benchmarks/`? Probably yes; see the
  benchmark-results hygiene plan.
