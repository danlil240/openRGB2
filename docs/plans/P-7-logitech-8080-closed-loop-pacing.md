# P-7: Logitech 0x8080 per-key pacing is open-loop

Status: proposed · Size: M · Source: audit 2026-11-28 P-7

## Goal

The G512 (and other 0x8080 per-key keyboards) should follow live effects at
the engine rate instead of topping out at about 40–50 Hz. Keys must still never
go stale. That was the tail-key bug that `9c866ca` fixed with a fixed sleep.
The change must also be deployed: today it ships only through a manual qmake
rebuild.

## Current state

- **Send path.** `RGBController_LogitechHIDPP20::DeviceUpdateLEDs` handles 0x8080
  at `RGBController_LogitechHIDPP20.cpp:2273-2334`:
  - It snapshots `colors` and applies `GetDimBrightness()`.
  - It groups every addressable LED by keyType.
  - It calls `SetPerKey8080(by_type)` and then `PerKeyCommit8080()`.
  - It does not diff against `sent_colors`, so every frame repaints every key.
  - The 0x8080 "Off" path at `:2995-3019` is the same full repaint.
- **Per-frame sleep.** `LogitechHIDPP20Controller::SetPerKey8080`
  (`LogitechHIDPP20Controller.cpp:5509-5592`) splits each group into frames of
  `HIDPP20_8080_KEYS_PER_FRAME` = 14 keys (`.h:118`). It sends each frame with
  `SendVeryLongFrame` (`:1042-1090`, `hid_write` on `dev_perkey_vl`,
  fire-and-forget). After every frame it sleeps
  `HIDPP20_8080_FRAME_GAP_MS` = 2 ms (`.h:119`, `:5588`).
- **ACKs are never consumed.** They arrive on `dev`, the reader thread queues
  them (`ReadFromQueue`, `:341-387`), and they sit there until
  `PerKeyCommit8080` calls `SendAcked(..., HIDPP20_POLICY_RELIABLE)`
  (`:5598-5614`). That call flushes the queue first (`SendAcked`, `:549-552`).
- **Frame budget.** It runs on the per-device `DeviceCallThread`
  (`RGBController.cpp:95`) and holds `TransactionMutex` the whole time (`:2214`).
  With roughly 8 frames of 2 ms sleep, plus the writes, plus one reliable ACKed
  flush, the 20–25 ms frame time is consistent with the audit's 40–50 Hz.
  *Derived, not measured.*
- **Pattern to reuse.** The non-8080 per-key path already does closed-loop
  pacing:
  - a delta against `sent_colors` (`:2567-2580`)
  - ACK accounting that advances `sent_colors` only on ACK and marks misses
    `HIDPP20_UNCOMMITTED` (`:2787-2828`)
  - a learned busy window and a 10 s summary log line (`.cpp:5287-5329`,
    `:5446-5462`)
- **Not recorded.** `vendor/provenance/MANIFEST.md` "Deliberate adaptations"
  (`:54-62`) does not record `9c866ca`.
- **Deploy gap.** `packaging/windows/build-and-deploy.ps1:146-172` builds the
  backend only when `OpenRGB.exe` is missing, and only with `-BuildBackend`. A
  changed vendor source deploys the old binary without any warning.
  `scripts/build-all-and-install.ps1:289-308` does run the incremental qmake
  recipe unless `-SkipBackend` is passed.
- **No tests.** No test target compiles any Logitech code (`tests/backend/CMakeLists.txt`).

## Design

Delta sends combined with ACK pacing, in four parts.

1. **Pure planner.** Move "which keys to send, grouped and chunked" into a
   Qt-free, hidapi-free helper, `LogitechPerKey8080Plan.{h,cpp}`. It takes the
   snapshot, `led_to_keytype_keyid_8080`, `sent_colors`, brightness and a
   force-full flag. It returns the frames to send, each carrying the LED indices
   it covers. The KT40 filter (`:5519-5543`) moves in with it. Part 4 tests it.
2. **Delta.** The 0x8080 branch keeps `sent_colors`, as the other per-key path
   does. It sends only changed LEDs.
   - Force a full repaint in these cases: the first frame; after
     `ClaimSWControlIfNeeded`, wake or dim changes (the existing
     `sent_colors.clear()` sites at `:1517`, `:2963`, `:3271`); and on a refresh
     timer every 2 s (guess, configurable).
   - The refresh is the safety net against a competing writer and against any
     frame that was silently dropped. It pairs with E-5.
3. **ACK pacing.** Replace the fixed sleep with: write the frame, then wait for
   its 0x8080 fn3 ACK from the response queue.
   - Use a short window, 10 ms as a guess, measured on the G512 first. Match on
     feature index and function, like the existing echo compare.
   - On an ACK, advance `sent_colors` for that frame's LEDs.
   - On a timeout or a BUSY error, mark them `HIDPP20_UNCOMMITTED` and fall back
     to the 2 ms gap for the rest of the frame.
   - Commit with `FlushLEDS` once, after all frames are settled. That keeps the
     queue flush in `SendAcked` from eating frame ACKs.
   - Keep `HIDPP20_8080_FRAME_GAP_MS` as the fallback, and add a quirk bit if a
     device turns out not to ACK fn3.
4. **Observability.** Mirror the 10 s summary for 0x8080: frames, keys per frame,
   ACKs matched or lost, and the mean wall time per update. This is what shows
   the achieved Hz on the real keyboard.

**Deploy.** Add a backend staleness check to `build-and-deploy.ps1`: hash the
`vendor/OpenRGB` sources (`git ls-files -s vendor/OpenRGB` plus a working-tree
dirty check), compare the hash with a stamp written next to `OpenRGB.exe` by the
recipe step, and either rebuild (`-BuildBackend`) or fail with "backend is stale".
Record `9c866ca` and this change in `MANIFEST.md`.

**Rejected alternatives:**
- A smaller fixed gap (1 ms): still open-loop, and the safe minimum is unknown
  for other 0x8080 boards.
- Moving 0x8080 onto the existing per-key sender thread wholesale: that thread is
  built around the 0x8081 FrameEnd and staged-zone semantics. Reusing its
  accounting pattern is cheaper than generalising it.
- Delta without ACKs: a dropped frame would leave keys stale until the next full
  refresh, which is the original bug coming back at a lower rate.

## Steps

1. Record `9c866ca` in `vendor/provenance/MANIFEST.md`. Test: none (docs).
2. Extract the planner (`LogitechPerKey8080Plan`) with no behaviour change, and
   add `logitech_8080_plan_test` to `tests/backend` (label `backend`). The test
   covers chunking at 14, KT40 filtering and the brightness scaling.
3. Add the delta and the forced full-repaint triggers using the planner, keeping
   the fixed gap. Test: the planner emits only changed keys, and a full repaint
   after `clear()` or a refresh.
4. Add ACK pacing in `SetPerKey8080`, with per-frame settle into `sent_colors`
   and the fallback gap. Test: planner and settle logic with a scripted ACK
   sequence (matched, lost, BUSY). The I/O layer stays untested offline.
5. Add the 0x8080 summary log line. Test: none offline; check it in a
   hardware run.
6. Add the backend stamp and staleness check to `build-and-deploy.ps1`, and
   write the stamp from the recipe step in both scripts. Test: add a case to
   `tests/packaging` that runs the check against a fake vendor tree and a stamp.

## Tests

- New `logitech_8080_plan_test` (`backend`), built like
  `blackwell_gpu_packet_test` (vendor sources compiled into a plain executable).
- Packaging test for the staleness check (`packaging`).
- **Manual on the G512** (needs hardware). Close G Hub, stop the LampArray
  service and turn Dynamic Lighting off first: the B-1 report now flags all
  three. Then:
  - Run Wave at 60 Hz with a backend log at debug level. Read the new summary:
    achieved updates/s and ACK loss.
  - Watch F5–F12, PrtSc, `0` and `-` for 5 minutes on a full-white static scene,
    then on a fast hue sweep. No key may stay stale for longer than one refresh
    period.
  - Repeat with the refresh disabled to show that the ACK path alone keeps the
    tail keys.

## Risks and open questions

- **Does the G512 ACK fn3 only after it has parsed the frame?** If the ACK
  arrives before parsing, ACK pacing does not fix the tail-key race and the gap
  has to stay. Measure this first; it is a guess either way.
- The full-refresh period trades bus load against how fast a stale key recovers.
  Daniel's call; 2 s is a placeholder.
- Other 0x8080 boards (G410 with the KT40 quirk, G810 family) are not on this
  PC. The fallback gap must remain their behaviour unless they are tested.
- Should a vendor change that is not upstreamed live in a patch series under
  `vendor/provenance/` rather than only in-tree? Daniel's call.
