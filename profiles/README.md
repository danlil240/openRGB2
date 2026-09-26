# Profiles

Committed profiles describe device topology and matching rules with
**no unique hardware identifiers**.

## Layout

- `daniel-pc.json` — Daniel's PC RGB topology (matching hints, LED counts,
  constraints, benchmark settings).
- `local/` — gitignored per-machine state: resolved device identities
  (MACs, serials, locations), session revalidation records, and anything
  with unique IDs. Create files here during the authorized hardware
  session (see `docs/hardware/daniel-pc.md`).

Suggested `profiles/local/wireless-group.json` (created at revalidation
time, never committed):

```json
{
  "group_mac": "<unique - fill during session>",
  "master_mac": "<unique - fill during session>",
  "fan_type_bytes": [21, 21, 21],
  "led_count": 120,
  "revalidated": "<date>"
}
```
