# P-7: Logitech 0x8080 per-key pacing is open-loop

Status: steps 1–3, 5, 6 implemented on `perf/p-7-logitech-8080-delta` (not merged); step 4 waits for the G512 ACK-timing measurement · Size: M · Source: audit 2026-11-28 P-7

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

## Implementation record (2026-11-28)

Branch `perf/p-7-logitech-8080-delta`, not merged. The qmake backend was
not rebuilt and nothing ran on hardware.

| Step | Commit | What |
| --- | --- | --- |
| 1 | `cffedce` (earlier) | `9c866ca` was already in `vendor/provenance/MANIFEST.md`; nothing to add |
| 2 | `6a11027` | `LogitechPerKey8080Plan` extracted; `logitech_8080_plan_test` (label `backend`) pins the full repaint byte for byte against a copy of the pre-extraction loops |
| 3 | `477a2f4` | delta against `sent_colors` with forced full repaints; 2 ms gap kept |
| 5 | `323c0a6` | 10 s `LOG_DEBUG` summary for 0x8080 |
| 6 | `39201f1` | `openrgb2-backend.stamp` + staleness check (`package_backend_stamp_test`, label `packaging`) |

Decisions taken here (all reversible):

- **Refresh period:** the 2 s placeholder, as the compile-time
  `HIDPP20_8080_FULL_REFRESH_MS` (0 disables). It is "configurable" only by
  rebuilding; there is no runtime setting yet.
- **Patch series vs in-tree:** in-tree, with one MANIFEST row per commit
  (the existing practice). No patch series under `vendor/provenance/`.
- **What counts as committed:** without fn3 ACKs (step 4), a key advances in
  `sent_colors` when its frame's `hid_write` succeeded and the update's
  FlushLEDS acked. Otherwise it is marked `HIDPP20_UNCOMMITTED` and re-sent.
  A frame the keyboard drops silently leaves its keys stale until they
  change or the next 2 s refresh.
- **`sent_colors` holds dimmed wire colors,** so a dim change re-sends every
  lit key without a `clear()`.
- **Re-init:** a changed `GetInitGeneration()` forces a full repaint, as on
  the 0x8081 path.
- **Idle updates:** a delta with no changed key sends nothing, FlushLEDS
  included. A full repaint keeps the old rule: FlushLEDS whenever any LED is
  addressable, even if the G410 filter left no frame.
- **Summary period:** 10 s (`HIDPP20_8080_SUMMARY_S`, the figure in this
  plan), not the 60 s `HIDPP20_PERKEY_SUMMARY_S`. It logs every window, not
  only on errors, because it is the achieved-Hz readout. It has no
  matched/lost ACK count until step 4.
- **Stamp scope:** `vendor/OpenRGB` and `src/core/playback`, because
  `OpenRGB.pro` compiles `../../src/core/playback/PlaybackLoop.cpp`.
- **Missing stamp = stale.** Every backend built before this change fails
  `build-and-deploy.ps1` once, until rebuilt with `-BuildBackend`. The new
  `-AllowStaleBackend` stages it with a warning. An `OPENRGB2_BACKEND_DIR`
  backend is not checked; a tree without git only warns.
- **`build-all-and-install.ps1 -SkipBackend`** does not bypass the check.

### Expected effect (derived from the code, not measured)

Update time is about F x (write + 2 ms gap) + one ACKed FlushLEDS, where F
is the number of 14-key frames. A full repaint is F of about 8 (the 20–25 ms
in "Current state"). A delta has F = sum over keyTypes of
ceil(changed keys / 14):

- An unchanged frame costs nothing: no writes, no FlushLEDS.
- A few changed keys of one keyType cost one frame plus the flush.
- An effect that changes every key every frame (Wave, a fast hue sweep) gains
  nothing. It still needs about 8 frames per update, and the 2 s refresh adds
  nothing because every update is already full. Step 4 is what would help
  those effects.

### Step 4 measurement (needs the G512)

Close G Hub, stop the LampArray service and turn Dynamic Lighting off first.
Then decide whether the fn3 ACK arrives only after the frame has been
parsed:

1. Build a debug variant of `SetPerKey8080` with the 2 ms gap set to 0.
   After each frame, block on the response queue for the fn3 ACK (feature
   index `idx_perkey_8080`, function 0x30, software ID), with a 50 ms bound.
   Log the write-to-ACK latency per frame.
2. Run a full-white static scene, then a fast hue sweep. Watch the known tail
   keys (F5–F12, PrtSc, `0`, `-`) with the timed refresh disabled
   (`full_refresh_ms_8080 = 0`).
3. If every frame ACKs and the tail keys never go stale, ACK pacing can
   replace the gap. If keys go stale even though every frame ACKed, the ACK
   comes before the frame is parsed: keep the gap and do not do step 4.
   Record the latency distribution here either way.
