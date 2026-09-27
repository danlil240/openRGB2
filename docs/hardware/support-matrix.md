# Support Matrix — daniel-pc (Milestone 5)

Per-device verified status for the OpenRGB2 engine + managed
`lianli-wireless` backend, as qualified across Tasks 5.1–5.5
(`docs/benchmarks/`). Verified here means something specific per column —
read the tier definitions before the table.

## Verification tiers

| Tier | Meaning | Evidence |
| --- | --- | --- |
| **Enumerated** | Backend reports the controller over the SDK (`sdk-list`) with a resolvable identity (serial/location, not enumeration index). | `sdk-list` output, engine `diag` controller keys |
| **Transport-delivered** | Engine frames crossed the SDK boundary and the backend acknowledged the write path (`transport_completed` counters advance; `set_desired`/`UPDATELEDS` trace events). Wire-level proof only — **not** proof photons were emitted. | `GET_DELIVERY_STATS`, backend `lianli.csv` trace |
| **Device-side observed** | A device-side observable confirms the controller is doing what was asked (wireless `rgb_clock` playback ticks advancing, `upload: active`, keepalives answered). Only available for the wireless group — wired controllers expose no equivalent. | `OPENRGB_LIANLI_TRACE` rows |
| **Physically observed** | A human saw light. This rig has no photodiode/camera instrumentation; all physical claims are **qualitative**, made only where a task explicitly noted it. | Task run notes |

## Matrix

| Device | LEDs | Enumerated | Transport-delivered | Device-side observed | Physically observed | Status |
| --- | --- | --- | --- | --- | --- | --- |
| **UNI FAN SL V3 Wireless group** (3 × SL V3, serial on file in `profiles/local/`) | 120 (3 × 40) | ✓ stable serial | ✓ live ~32 Hz self-paced + baked playback frames suppressed correctly while owned | ✓ `rgb_clock` ticks at 1599.956/s (−27.5 ppm over 65 min, Task 5.5 — supersedes the −0.6 ppm/9.7-min Task 5.2 fit), keepalives answered p99 804 ms, `upload: active` held 65 min | ✓ qualitative — animation and live effects seen on the fans in 5.2/5.3 sessions; not instrumented | **VERIFIED** — both modes |
| **Gigabyte AORUS RTX 5080 MASTER ICE** (GPU) | 26 (Right/Left/Middle Fan 8 ea, Side/Top Logo 1 ea) | ✓ NvAPI I2C location | ✓ ~58 Hz live lane | — (no device-side channel) | partial — side logo verified in the hardware profile; **fan lighting rings never physically confirmed** | **VERIFIED (transport)** — rings unverified, see below |
| **Logitech G512 keyboard** | 108 (Keyboard 107 + Extras 1) | ✓ serial `3135470D` | ✓ ~58 Hz live lane | — | ✓ qualitative | **VERIFIED (transport)** — see ownership caveat |
| **Razer Basilisk V3 Pro (Wired)** | 13 (wheel, logo, strip) | ✓ (serial space-padded — binding drops serial to resolve; profile hygiene note) | ✓ ~59 Hz live lane | — | ✓ qualitative | **VERIFIED (transport)** |
| **X870E AORUS ELITE WIFI7 ICE — `LED_C` + `Chipset Accent`** | 2 | ✓ HID controller | ✓ frames accepted by backend | — | ✗ **zone identity unknown** — the two enumerated LEDs were never mapped to a physical location on the board | **OUTSIDE the verified matrix** |
| **X870E ARGB headers `ARGB_V2_1/2/3`** (wired 8/16/8: case fans, AIO radiator+pump, top-GPU fan) | 0 enumerated | ✗ — zones enumerate with **0 LEDs** despite confirmed physical wiring | ✗ | — | ✗ | **NOT SUPPORTED in this build** — backend enumeration gap |
| **Corsair Vengeance RGB DDR5 × 2** | — | ✗ — no SMBus DRAM controller enumerated; backend ran unelevated, PawnIO permission failure (all 5.x sessions) | ✗ | — | ✗ | **NOT SUPPORTED in this build** — needs elevated backend or signed-driver path |
| AIO pump LCD / other cooling channels | — | — | — | — | — | **OUT OF SCOPE** — cooling/LCD excluded from this delivery |

## Notes and caveats

1. **Wireless group identity** is resolved by the dongle serial (kept in
   gitignored `profiles/local/`), never by enumeration index — positional
   re-enumeration was tested in Task 5.3 (controller removal/reinsertion
   re-bound by identity).
2. **GPU fan rings**: the three 8-LED "fan" zones accepted frames in every
   session, but no physical observation ever confirmed which light (if any)
   is wired to them on this card — the hardware profile records only the
   AORUS MASTER ICE **side logo** as verified (`docs/hardware/daniel-pc.md`);
   the **top logo** enumerates but has no physical verification on file.
   Rings stay flagged unverified until seen.
3. **Motherboard unidentified LEDs**: `LED_C` and `Chipset Accent` accept
   frames at transport level and are bound in the qualified scene, but
   per the plan's pass criterion they remain **outside the verified
   support matrix** — nobody has confirmed where on the board they light.
   The 8/16/8 ARGB headers (the real case/AIO/top-fan lighting) do not
   enumerate LEDs at all in the vendored backend — this is the largest
   known coverage gap on this rig and predates the engine (recorded in
   the M1 hardware profile).
4. **DIMMs** were absent in every session (PawnIO needs elevation). They
   are a known absentee, not a regression; an elevated backend run would
   be needed to bring them into the matrix.
5. **Ownership**: Logitech G Hub runs on this machine and was flagged as a
   competing owner for the G512 in every scan (11 processes, Task 5.3/5.4).
   G512 frames still flowed and no contention artifact was instrumented,
   but physical flicker was not measured. FanControl owns cooling and was
   verified untouched before/during/after (Task 5.4).
6. **What "verified" does not mean**: no device claim rests on photons.
   Every "transport-delivered" mark is a backend-acknowledged write; the
   only device-side behavioral evidence in the system is the wireless
   group's playback clock/keepalive trace.
