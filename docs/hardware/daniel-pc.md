# Daniel's PC — hardware profile

> Milestone 1 deliverable. Describes the RGB topology this project targets.
> Values below come from the implementation plan and Obsidian notes
> (`PC hardware and operating systems`, `Cooling and RGB`,
> `Lian Li SL Wireless smoke test`); they are **historical documentation,
> not current-session measurements**. Every field in the revalidation
> checklist must be re-checked during the authorized hardware session
> before benchmark results are recorded.
>
> Unique hardware identifiers (MAC addresses, serials) are **not** stored
> here — they live in local user state under `profiles/local/` (gitignored).

## Wireless path (primary benchmark target)

| Item | Value | Source |
| --- | --- | --- |
| Fans | 3 × Lian Li UNI FAN SL Wireless 120 mm (white, reverse) | plan §2.1 |
| Group | One SL V3 group, 3 × 40-LED zones, 120 logical LEDs total | plan §2.1 |
| Dongles | V1 wireless TX `0416:8040`, RX `0416:8041` | smoke-test note |
| Transport | libusb bulk, EP 0x01 out / 0x81 in, 4×60-byte USB chunks per RF packet | driver source |
| Compression | tinyuz (vendor-compatible), 4 KB dictionary | `LianLiWirelessCodec.h` |

## Wired topology

| Device / connection | Mapping | Notes |
| --- | --- | --- |
| X870E AORUS ELITE WiFi7 ICE, `ARGB_V2_1` | 4 mirrored case fans, **8** addressable LEDs | mirrored copies — do not multiply LED count by fan count |
| Same board, `ARGB_V2_2` | GAMING 360 ICE AIO: indices 0–7 radiator fans (mirrored), 8–15 pump — **16** LEDs | separate radiator and pump regions |
| Same board, `ARGB_V2_3` | Top-GPU fan — **8** LEDs | |
| Corsair Vengeance RGB DDR5 × 2 | 10 LEDs each | resolve by stable identity/location, never enumeration index |
| AORUS RTX 5080 MASTER ICE | side logo verified | never change fan speed to enable lighting |
| Razer Basilisk V3 Pro | wheel, logo, strip | resolve Synapse ownership first |
| Logitech G512 | 107 per-key LEDs | exclude non-visible Extras zone |
| Motherboard `LED_C` / `Chipset Accent` | **unmapped** | leave until physically identified |

Motherboard headers are configured as **8 / 16 / 8**.

## Ownership and constraints

- **FanControl owns cooling.** RGB work must not touch PWM, pump speed, or
  sensor access. Verify FanControl and sensor polling survive every
  benchmark session.
- A historical PawnIO failure affected both RAM RGB and FanControl sensors;
  automatic driver resets are **not** a recovery strategy.
- Competing RGB owners (L-Connect, GCC, Synapse, Logitech G Hub, stock
  OpenRGB) must be detected and handled explicitly — never killed silently.
- The old Desktop Lighting Studio path (preserved project
  `C:\Users\daniel\tools\openrgb-unified`) is read-only source of record;
  never modify or reset it.

## Revalidation checklist (authorized hardware session)

Record exact values in `profiles/local/session-<date>.json`; keep unique
identifiers out of committed files.

- [ ] Wireless group MAC matches the local state file (do not print it).
- [ ] Fan group reports 3 fans × 40 LEDs (SL V3), fan-type bytes 20–22.
- [ ] TX/RX dongle pair present exactly once (`0416:8040` / `0416:8041`).
- [ ] Backend driver version/firmware revision recorded (discovery record
      device ticks, fan types, and any firmware identifiers available).
- [ ] Motherboard headers enumerate 8 / 16 / 8 as configured.
- [ ] Each wired device resolves by stable identity (serial/location), not
      enumeration order; two DIMMs disambiguate correctly.
- [ ] Current RGB ownership confirmed: FanControl running, no competing
      lighting processes; the old Studio plugin producers disabled.
- [ ] Visible output sanity check (each region lights) with a coded pattern
      before any timing run.
- [ ] FanControl sensors readable before and after the session.

## Session artifacts

| Artifact | Location |
| --- | --- |
| Local identity state (gitignored) | `profiles/local/` |
| Backend diagnostics trace | `OPENRGB_LIANLI_TRACE=<path>` CSV + `.summary` |
| Producer per-send log | `tools/bench` `--out <csv>` |
| Baseline report | `docs/benchmarks/baseline.md` |
