# OpenRGB2 security / boundary verification (Task 6.3)

Verified on the staged package `build/package/OpenRGB2-0.1.0-win64/`,
live on the target machine, 2026-09-28. Each item states a verdict,
the captured evidence, and residual risk. The packaged binaries are
**unsigned** — no boundary is weakened to make a check pass, and no
Smart App Control / Device Guard workaround or exclusion exists in
this repository.

## 1. Loopback port 6744 — PASS (loopback only)

The launcher always binds the backend to loopback:
`scripts\start-openrgb2.ps1` spawns `backend\OpenRGB.exe --server
--server-host 127.0.0.1 --server-port 6744 --noautoconnect
--localconfig` (the only backend-spawn site; `OpenRGB2.cmd` forwards
to the same script).

Live evidence — packaged backend (pid 24648/49444) running with the
managed arguments:

```
Get-NetTCPConnection -LocalPort 6744 -State Listen
  LocalAddress  LocalPort  State   OwningProcess
  127.0.0.1     6744       Listen  49444
netstat -ano -p tcp | findstr 6744
  TCP    127.0.0.1:6744    0.0.0.0:0   LISTENING   49444
```

No `0.0.0.0:6744` (or `[::]`) row exists. Connection probes:

- `connect 172.30.160.1:6744` (this host's LAN IP) → **actively refused**
- `connect 127.0.0.1:6744` → succeeds (expected)

The engine holds **zero** network listeners — `Get-NetTCPConnection
-State Listen` / `Get-NetUDPEndpoint` filtered to the engine and
backend PIDs returns exactly the one row above. The engine's only
endpoints are the two ACL'd named pipes (item 2).

Residual risk: the vendored backend honours a `default_host` settings
key, but the CLI `--server-host` value takes precedence over settings
(`ResourceManager::StartSDKServer` — CLI fills `default_server_host`
non-empty so the settings lookup is skipped). A manual launch of
`backend\OpenRGB.exe` with a hand-edited `state\backend\OpenRGB.json`
could bind `0.0.0.0`; that is outside the managed flow, and the
vendored compile-time default is itself loopback
(`OPENRGB_SDK_HOST "127.0.0.1"`, NetworkProtocol.h:33). Nothing in
the package makes a non-loopback bind possible through the scripts.

## 2. Private configuration — PASS (owner+SYSTEM only)

Two boundaries hold state private: the backend's `--localconfig` and
the restricted named-pipe DACL.

**Backend private config.** The backend ran with `--localconfig` and
`state\backend` as its working directory; it wrote
`state\backend\OpenRGB.json` (98 KB) and `state\backend\logs\` inside
the package — nothing touches the global `%APPDATA%\OpenRGB` tree.
Engine state lives under `profiles\local\` (gitignored user state).

**Named-pipe ACL.** With the packaged engine running, both pipes
exist and carry the Task-3.5 restricted DACL
(`GetNamedSecurityInfoW`, SE_KERNEL_OBJECT, SDDL dump):

```
\\.\pipe\openrgb2-engine       D:P(A;;FA;;;S-1-5-21-985583470-140289594-31760812-1001)(A;;FA;;;SY)
\\.\pipe\openrgb2-engine-diag  D:P(A;;FA;;;S-1-5-21-985583470-140289594-31760812-1001)(A;;FA;;;SY)
```

The SID is the creating user (`LILINTAL-HOME-P\daniel`). `D:P` is a
protected DACL; the only ACEs are GENERIC_ALL to the owner and
LocalSystem — no Everyone/Users/Anonymous/Network access, matching
`docs/ipc-protocol.md` and `src/ipc/NamedPipe.cpp` (`BuildRestrictedSddl`;
the listener refuses to start if it cannot build the descriptor —
no default-open fallback). `tests/engine/ipc_server_test.cpp` asserts
the SDDL shape offline.

Residual risk: a second user on the same machine cannot open the
pipes (denied by the DACL) but the pipe *name* is fixed — a same-user
process can always connect; that is the intended trust boundary
(the wire protocol is additionally bounded/versioned). The
`\\.\pipe\` namespace is local by construction — there is no remote
named-pipe surface.

## 3. Single-instance — PASS

Two independent guards:

- **Engine** (`src/engine/SingleInstance.cpp`, mutex
  `Local\openrgb2-engine`): a second `openrgb2-engine.exe` launched
  in the packaged context while the first ran returned **exit 1**:

  ```
  openrgb2-engine: another engine instance already owns
  'Local\openrgb2-engine' — refusing to double-own the managed
  configuration (section-4 single-owner rule)
  ```

- **Backend**: the vendored `startup\main_Windows.cpp` guard is
  *kill-capable* — launched while a foreign `OpenRGB.exe` runs, its
  Yes/No prompt can `TerminateProcess` that instance, and its "server
  already running" probe checks the legacy default port 6742, not
  managed 6744. The vendored cli has no flag to skip it, so the
  launcher **refuses to spawn the backend at all** while a foreign
  `OpenRGB.exe` lives (exit 3 — packaging suite T9 verifies the
  refusal; the prompt is never reachable from the managed flow and
  was deliberately not triggered in this live test). An `OpenRGB.exe`
  at our pinned path mid-start is waited on, never double-spawned.

Live bonus evidence — the real ownership gate: with Logitech G Hub
running on this machine, `start-openrgb2.ps1 -ScanOnly` printed 11
G-Hub `CONFLICT` findings + FanControl as a notice and exited **3**;
the plain `-NoUI` run refused with the "will not take over silently"
banner and spawned nothing (no `state\openrgb2.run.json` written).
The engine-side scan produced the same findings through
`openrgb2-ctl state` (`ownership.scanned:true`, `conflicts`,
`notices` — report-only, no kill/takeover verb exists).

## 4. Elevation — PASS (nothing silently requests it)

- All four packaged executables embed `requestedExecutionLevel
  level='asInvoker' uiAccess='false'` (extracted from the binaries).
- The whole live run above executed from an unelevated session
  (`IsAdministrator: False`), backend included.
- The only elevation path is the **opt-in** `-Elevated` switch on
  `start-openrgb2.ps1`, which re-launches via
  `Start-Process -Verb RunAs` — the UAC prompt itself is the
  user-visible consent step; nothing else calls `runas`.

Where elevation *is* required, the vendored backend degrades
honestly. Its own log from the unelevated run
(`state\backend\logs\OpenRGB_20260928_003844.log`):

```
[Info  ]Start PawnIO: SmbusI801.bin
[Error ]Permission Denied, PawnIO initialization aborted
   (same for SmbusPIIX4, SmbusNCT6793, SmbusIntelSkylakeIMC, LpcIO)
[Dialog]<h2>Warning:</h2><p>One or more I2C/SMBus interfaces failed
   to initialize…you must install PawnIO… and run OpenRGB as
   administrator or as a system service.</p>
[Info  ][DetectionManager] Registering RGB controller
   UNI FAN SL V3 Wireless …
```

PawnIO-gated paths (SMBus DIMM RGB, SuperIO/motherboard sensors) are
refused by the driver unelevated — logged as errors plus a visible
warning dialog, never silently. USB/HID paths (the SL Wireless group)
work unelevated, so the app still starts and enumerates. Per
Milestone 5 the PawnIO/SMBus/DIMM path is currently absent from the
supported flow; `-Elevated` is documented in `INSTALL.md` for when it
is needed.

## 5. Signed-binary handling — PASS (documented, no workarounds)

- `Get-AuthenticodeSignature` over the package: all four application
  executables (`openrgb2-ui/engine/ctl`, `backend\OpenRGB.exe`) are
  **NotSigned**; bundled third-party DLLs are mixed (Microsoft/Qt
  payloads carry their own signatures; e.g. `hidapi-hotplug.dll` and
  `PawnIOLib.dll` are unsigned).
- Expected platform behavior on this host: Smart App Control /
  Device Guard applies **per-hash** verdicts to unsigned binaries.
  Observed across milestones (task reports 3.1–4.7): freshly linked
  executables are transiently denied (`BAD_COMMAND`/exit 126,
  CodeIntegrity events 3077/3118) until the ISG verdict lands; a
  relink (new hash) re-rolls the verdict. The packaged binaries here
  launched normally — SAC verdicts for these hashes already exist.
- Repository audit: **no** `Add-MpPreference`/`Set-MpPreference`,
  exclusion path/process, SmartScreen/Device-Guard bypass, or
  per-path workaround exists in any shipped or test script
  (grep over `*.ps1/*.cmd/*.bat/CMakeLists.txt` outside `build/`).
  The historical "trampoline" SAC workaround in the origin tree was
  deliberately *not* imported (`vendor/provenance/MANIFEST.md`,
  "Removed historical workarounds"). Per AGENTS.md, none will be
  added.

Residual risk: a release built/installed on a *new* machine may see
first-run launch delays or transient SAC denials until verdicts
land — expected platform behavior for unsigned binaries, not a
defect. Signing would remove this class entirely; it remains a
publish-time decision (plan §1: publication is separate).

## 6. Missing-dependency recovery — PASS (honest failure + recovery)

- **Missing backend while engine runs:** after the managed backend
  was stopped, the packaged engine stayed up and reported
  `sdk.state:"reconnecting"`,
  `last_error:"cannot connect to 127.0.0.1:6744"`,
  `reconnect_attempt` counting bounded retries — no crash, no false
  "connected". Backend restart → engine re-attached by itself
  (`state:"connected", protocol_version:4, reconnect_attempt:0`).
- **Missing DLL (scratch copy, real package untouched):** copying the
  package to `%TEMP%` and removing `Qt6Core.dll` then launching the
  UI → immediate exit `-1073741515` (`0xC0000135`,
  `STATUS_DLL_NOT_FOUND`) — an explicit loader failure naming the
  missing module, not a silent crash. Removing `msvcp140.dll` and
  launching the engine on *this* host still ran (the MSVC redist is
  also in `System32`, so the app-local copy is not the only source
  here); on a machine without the redist the same removal hits the
  `0xC0000135` path — which is why the CRT ships app-local.
- **Manifest-verified integrity:** deleting the files was caught by
  `manifest.json` verification (the same SHA-256/presence pass
  `update-openrgb2.ps1` runs before touching an install): missing
  files flagged by name. Recovery path is in `INSTALL.md` — re-stage
  the package or roll back; user state lives only in
  `profiles\local\` + `state\` and survives.

## Summary

| Boundary | Verdict |
| --- | --- |
| Loopback :6744 | PASS — listener is `127.0.0.1` only; LAN-IP probe refused; engine holds no sockets |
| Private configuration | PASS — pipe DACL owner+SYSTEM; backend state confined to `state\backend` |
| Single-instance | PASS — engine mutex refuses; backend's kill-capable prompt unreachable from launcher |
| Elevation | PASS — `asInvoker` everywhere; only opt-in `-Elevated`/UAC; unelevated degradation is logged+visible |
| Signed binaries | PASS — unsigned + SAC transients documented; zero workarounds/exclusions in repo |
| Missing-dependency recovery | PASS — honest `0xC0000135`/`reconnecting` states, auto-recovery, manifest-verified repair path |

### Known residuals (all documented, none new in this task)

- An operator can hand-edit the private backend config or launch the
  backend manually outside the scripts — the scripts cannot police a
  manual run; inside the managed flow every bound value is fixed by
  arguments.
- The vendored backend's close-time GUI dialog (shown during
  detection warnings) can absorb `CloseMainWindow`; `stop-openrgb2.ps1`
  bounds the wait and `-Force` applies to image-path-verified own
  processes only.
- Same-user processes can connect to the engine pipes by design; the
  boundary is per-user, and the wire protocol is bounded/versioned.
