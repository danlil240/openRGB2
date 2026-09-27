# Lifecycle Matrix — Task 5.3 (Milestone 5)

Qualification of engine lifecycle behavior: cold start, backend restart,
UI crash, device unplug/replug, display sleep, system suspend/resume, and
an existing competing owner. Rows were run on the real rig where safe and
automated through the fake backend / injected power events where a
physical trigger was infeasible or unsafe.

## Builds under test

| Component | Version |
| --- | --- |
| `openrgb2-engine` + `openrgb2-ctl` | branch `milestone-5`, working tree at `c8a68fd` + task changes (committed on top) |
| Managed backend | `vendor/OpenRGB` lianli-wireless fork, `OpenRGB v1.0+ (git)` |
| Backend deployment | `vendor/OpenRGB/OpenRGB Windows 64-bit/OpenRGB.exe --server --server-port 6744 --localconfig`, cwd `profiles/local/lifecycle/run2` |
| Engine launch | `openrgb2-engine.exe --sdk --scene …/scene-live.json --state …/engine-state.json --backend-config …/run2` |

## Topology observed

5 controllers via SDK on `127.0.0.1:6744`: Gigabyte RTX 5080 MASTER ICE
(26 LEDs), X870E AORUS ELITE WIFI7 ICE (2), UNI FAN SL V3 Wireless 66:E1
(120), G512 keyboard (108), Razer Basilisk V3 Pro (13). DIMMs absent
(PawnIO permission failures, unelevated — same as Task 5.2).

## Measurement honesty

- Transport-boundary evidence only: submitted/completed counters,
  connection state, and log lines prove wire behavior, not photons.
- Timings are wall-clock from process launch/SDK attach — the backend's
  own ~40 s device-detection window is excluded where noted.

## Results matrix

| Row | Expected | Observed | Verdict | Evidence |
| --- | --- | --- | --- | --- |
| Cold start | Attach to running backend, enumerate, restore scene, honest timing | Engine from nothing: SDK connected **t+117 ms**, first frames on the wire **t+165 ms**, 5 controllers, protocol 4, 2 per-scene playback selections restored | **PASS (physical)** | `engine5.log`: "backend listener detected on port 6744 (pid 49868) — attaching via SDK; the engine does not own the backend process", "SDK connected … (protocol 4, 5 controllers)", "restored 2 per-scene playback selections"; timed poll loop |
| Cold start, backend late | Bounded retry, honest "not attached", attach without restart when backend appears | Fake: **233 ms** attach after listener opens; start→first `UPDATELEDS` 1466 ms incl. 1213 ms injected outage. Physical: engine survived ~6 min of backend being absent/stuck, re-armed after backoff exhaustion, attached when the listener finally opened | **PASS (sim + physical)** | `e2e_lifecycle_test` output; `engine3.log`: "no backend detected … will retry" → "backend absent — re-arming the bounded SDK reconnect" → `ctl snapshots` showed `connected` |
| Backend restart | Kill backend mid-session → engine re-attaches, re-negotiates, restores state; engine must not own/restart the backend process | `taskkill` of backend pid 41704 during live delivery → `state: reconnecting`, bounded retries → backend relaunched (pid 49868) → `state: connected`, protocol 4, identical 5-controller identity, delivery resumed (submit counter continued 19695→41703→53088, rejected stayed 0) | **PASS (physical + sim)** | `ctl snapshots`/`stats` before/during/after; fake test `SetPeerClosed` → reconnect → revalidation |
| UI crash | IPC session drop leaves engine running; UI restart rejoins honestly | Long-lived client (`ctl subscribe 60`, pid 26128) force-killed mid-subscription after receiving EVENTs → engine unaffected (SDK connected, 5 controllers); a new client rejoined immediately. Fake test drops a real named-pipe session and reconnects a second session | **PASS (physical + sim)** | `sub1.out` (22 lines of events pre-kill), post-kill `ctl snapshots` OK; `e2e_lifecycle_test` |
| Unplug / replug | Topology change detected; removed controller stops receiving frames; re-added controller re-enumerated and re-bound | Fake: `RemoveController` then `AddController` — engine's `RefreshStatus()` now re-fetches controller count so the new device appears without a transport reconnect; topology row passes live. **Physical unplug not performed** — pulling the wireless dongle/USB device mid-run was not safe to do unattended | **PASS (simulated); physical BLOCKED — not exercised** | `e2e_lifecycle_test` unplug/replug rows; `OpenRgbClient.cpp` `RefreshStatus` fix |
| Display sleep | Monitor-off pauses RGB output; no recurring writes that could wake the panel; resume restores | Real `SC_MONITORPOWER` broadcast: log "power: display off — RGB output paused, pending frames discarded"; over 15 s off, submits only +175 (≈12/s residual upkeep vs ~1500/s active aggregate — sampled freeze + status traffic, LED writes gated), rejected 0→4545 (gated backlog discarded), pending 1→0. Wake: "power: display on", submits +2395 in 8 s | **PASS (physical + sim)** | `engine3.log` power lines; `ctl stats` before/during/after; injected `DisplayOff/On` in fake test |
| System suspend/resume | No writes across the suspend boundary; identity revalidation before restore | **Physical suspend not performed** — suspending the host unattended mid-session was not safe. Injected `PowerLifecycle::Suspend`/`Resume` with transport death across the boundary: pending frames discarded, output parked, resume revalidated identity, "wake revalidation -> restored", per-owned-group reconnect revalidation. A custom-mode upkeep write crossing the boundary was found and fixed | **PASS (injected proxy); physical BLOCKED — documented limitation** | `e2e_lifecycle_test` power log lines: "power: system suspend — pending frames discarded, output parked", "power: system resumed — revalidating identity before restore"; `EngineHost.cpp` gate |
| Competing owner | Ownership scan reports the conflict honestly; engine never kills the competitor | Stub `fakeowner/OpenRGB.exe --sleeper` (renamed, 1.4 MB) classified "OpenRGB (other instance) — report only, never killed"; managed backend pid 49868 classified "expected sole hardware owner"; FanControl notice-only ("must keep running untouched"); 11 real Logitech G Hub/Options processes flagged as conflicts; fixture stayed alive through every scan | **PASS (physical + sim)** | `engine4.log`/`engine5.log` scan lines; `tasklist` after scans; fake test fixture |

## Defects found by this task (fixed before commit)

1. **Topology refresh missed replugged devices** —
   `OpenRgbClient::RefreshStatus()` only iterated already-known devices;
   a newly added controller stayed invisible until a full transport
   reconnect. `RefreshStatus` now re-fetches the controller count and
   re-enumerates. Caught by the unplug/replug row.
2. **Custom-mode upkeep wrote across the suspend gate** —
   `EngineHost::PollOnce()` issued per-controller `SETCUSTOMMODE` upkeep
   even while `PowerLifecycle` had output parked. Upkeep is now skipped
   while gated. Caught by the suspend row's packet accounting.
3. **Reconnect exhaustion latched forever** — after `max_attempts` the
   client pinned `Disconnected` and nothing re-armed it. `EngineHost`
   now runs a bounded re-probe (`sdk_reprobe`, configurable) and logs
   "backend absent — re-arming the bounded SDK reconnect".
4. **Test environment-dependence** — the e2e test's default SDK port
   (6744) made the ownership scan see a *real* running backend and take
   the "port held by foreign service" branch; pinned to unused 64998.

## Caveats observed

- **Backend startup is fragile when a process named `OpenRGB.exe`
  already exists.** Two launches (pids 3612, 49060) sat at the main
  window, ~0 CPU, never created a log file or opened the listener while
  the competing-owner fixture was alive. The engine reported "no backend
  detected" honestly and attached once a healthy backend appeared —
  engine behavior correct; backend-side startup interaction noted.
- **Real backend listener churn:** the backend log shows
  `recv_select failed receiving magic, closing listener` → listener
  restart ~20 s later after client disconnects. The engine's bounded
  retries + re-probe rode through it.
- Suspend/resume and unplug/replug physical rows remain **unexercised**;
  the injected/automated coverage is documented above rather than
  claimed as physical.
- Display-off "submitted" count is not exactly zero during the gate:
  ~12/s residual is sampled-freeze/status traffic, not LED streaming —
  LED submits are gated (reject counter shows discards).
