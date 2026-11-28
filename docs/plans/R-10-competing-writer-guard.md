# R-10: Competing-writer guard — surface conflicts and guide the fix

Status: proposed · Size: S · Source: audit 2026-11-28 R-10 (remaining part after B-1 and E-5)

## Goal

When another program starts fighting OpenRGB2 for the lights (G Hub, the
Logitech LampArray service, Windows Dynamic Lighting, L-Connect, iCUE …),
the user sees it right away, wherever they are in the app or in the tray.
They also get concrete steps to turn that writer off. OpenRGB2 never
kills a process, stops a service or edits a setting itself.

## Current state

- **Done on `main` (B-1, `c4cfa2b`).**
  - `ClassifyOwners` matches `lamparray`, `lghub`, `logioptions`,
    L-Connect, Gigabyte, Razer, iCUE and SignalRGB
    (`src/engine/OwnershipScan.cpp:200-305`).
  - `ReadDynamicLightingEnabled` and `ClassifyDynamicLighting`
    (`OwnershipScan.h:100-122`).
  - `EngineHost::ScanCompetitors` re-runs every `owner_rescan` (5 s) on
    the maintenance tick (`EngineHost.cpp:1584-1592`) and only logs
    changes through `DescribeOwnerChanges` (`:383-390`).
- **In flight elsewhere.** E-5 re-assert of static scenes, on
  `fix/e-5-static-reassert` by another agent. Not planned here.
- **Missing 1: no push.** No IPC event carries ownership changes. The
  B-1 commit says the Diagnostics page re-pulls. `DiagnosticsController`
  polls only while its page is visible (`DiagnosticsController.cpp:347-359`),
  and `RecomputeOwnership` (`:670-707`) reads `STATE` and `DIAGNOSTICS`.
  A writer that appears while the user is on Lighting, or while the
  window is in the tray, goes unseen.
- **Missing 2: generic guidance.**
  - The Ownership card (`ui/pages/DevicesPage.qml:549-640`) lists
    `owner (exe, pid)` plus the `detail`.
  - Its one guidance line is generic (`:610`: "Close the conflicting
    application … then restart the engine").
  - Nothing tells the user how to turn off Dynamic Lighting or the
    LampArray service, or that G Hub auto-starts at login.
  - "Restart the engine" will be wrong once E-5 lands: re-assert makes a
    restart unnecessary.
- **Missing 3: no stable key.** An `OwnerFinding` has no
  machine-readable kind, only the `owner` label and `exe`
  (`OwnershipScan.h:78-89`). The UI would have to string-match labels to
  choose guidance, and the B-1 contract forbids UI-side
  re-classification (`EngineHost.cpp`, `OwnershipJson` comment).
- **Startup gate.** The takeover dialog in `src/ui/main.cpp:688-712`
  lists conflicts as plain text only.

## Design

1. **Stable key, set in the engine.** Add `std::string key` to
   `OwnerFinding`: `dynamic_lighting`, `lamparray`, `logitech`,
   `lianli`, `gigabyte`, `razer`, `corsair`, `signalrgb`, `openrgb`,
   `fancontrol`. Set it beside each `f.owner =` in `ClassifyOwners` and
   in `ClassifyDynamicLighting`. Serialize it in `OwnershipJson`. Keep
   the parity rule: `start-openrgb2.ps1` gains the same key in its
   finding objects, so test T1 can compare keys.
2. **Push an `ownership` event.** Add `event_kind::kOwnership`
   (`src/ipc/IpcProtocol.h:67-83`, `EventKinds()`) with data =
   `OwnershipJson()`. `ScanCompetitors` emits it when
   `DescribeOwnerChanges` is non-empty, and also on the initial scan.
   `EngineService::SnapshotEvent` serves it on `SUBSCRIBE`, so a
   reconnecting UI gets the current state. The 5 s scan already exists:
   no new timer.
3. **A conflict surface in the UI, outside the Diagnostics page.**
   - `EngineBridge` stores the pushed report.
   - `DiagnosticsController::RecomputeOwnership` prefers the pushed
     report and falls back to its poll.
   - `Main.qml` gains a warn banner under the connection banner
     (`ui/Main.qml:387-445`): "Another app is controlling your lights:
     <owners>", with a "How to fix" button that opens the Devices page
     at the Ownership card.
   - When a *new* conflict key appears while the window is hidden, the
     tray `showMessage` fires once per key per session.
4. **Guided steps, keyed in QML.** A small `OwnershipGuide.qml`
   component in `ui/components` maps each key to 2–4 short steps:
   - `dynamic_lighting`: Settings > Personalization > Dynamic Lighting >
     turn off "Use Dynamic Lighting on my devices". An "Open Settings"
     button calls `Qt.openUrlExternally("ms-settings:personalization-lighting")`.
     The URI must be verified on Windows 11.
   - `lamparray`: the `logi_lamparray_service` service, which Windows
     Update installs.
     - An "Open Services" button launches `services.msc`.
     - The text shows `sc stop logi_lamparray_service` and
       `sc config logi_lamparray_service start= demand`, each with a
       Copy button for the user to run elevated.
     - It notes that Windows Update may re-enable the service.
   - `logitech`: quit G Hub from the tray and turn off "Start G HUB on
     system startup", or set the device to onboard memory off.
   - The other keys get generic "close the app or disable its lighting
     module" text.
   - Unknown keys fall back to today's generic line.
   - The `:610` line drops "then restart the engine" once E-5 is merged,
     and becomes "OpenRGB2 re-applies your lights once the other app
     stops".
5. **Startup dialog.** `ask_takeover` (`main.cpp:688`) appends one line
   per key: "Open OpenRGB2 → Devices for steps". No other dialog
   change.

**Rejected:**
- A one-click "Stop service" or "Turn off Dynamic Lighting" button that
  writes HKCU or calls the Service Control Manager. It breaks the
  report-only contract (plan section 4 and the B-1 commit). Daniel can
  revisit this, but the plan keeps actions in the user's hands.
- Polling ownership in the UI on every page. It duplicates the engine
  scan, which already runs every 5 s.

## Steps

1. Add `OwnerFinding::key` in the classifier, `OwnershipJson` and the
   script mirror (`start-openrgb2.ps1:200-215`). Test: an
   `ownership_scan_test` case per key; parity test T1
   (`tests/packaging/package_lifecycle_tests.ps1:170-185`) extended to
   compare keys.
2. Add the `ownership` event kind, the emit on change and the
   `SnapshotEvent` support. Test: `e2e_fake_backend_test` (the B-1
   re-scan add/clear case) asserts one event per change;
   `ipc_protocol_test` checks that the kind is in `EventKinds()`.
3. Add the bridge store and the `DiagnosticsController` preference for
   pushed data. Test: a `ui_diagnostics_test` case where a pushed event
   updates `ownership` while the page is hidden.
4. Add the `Main.qml` banner and the tray notice (once per key). Test:
   `qml_smoke_test` or `qml_diagnostics_test` (offscreen): the banner is
   visible with an injected conflict and hidden when it clears.
5. Add `OwnershipGuide.qml` in the Ownership card and the reworded
   `:610` line. The reword depends on E-5 being merged. Test: a
   `qml_diagnostics_test` case where key `dynamic_lighting` renders its
   steps and the Settings button exists.
6. Extend the startup dialog text. Test: a `ui_launch_hardening_test`
   case on the conflict text builder, if it is extracted as a function.

## Tests

- Extended: `ownership_scan_test`, T1 parity, `e2e_fake_backend_test`,
  `ipc_protocol_test` (all `engine`); `ui_diagnostics_test` and
  `qml_diagnostics_test` (`ui`).
- Manual, on Daniel's PC:
  - Turn Dynamic Lighting on while on the Lighting page: the banner
    appears within about 5 s.
  - Start `logi_lamparray_service` with the window in the tray: one
    tray message.
  - The "Open Settings" link lands on the Dynamic Lighting page.

## Risks and open questions

- **`ms-settings:personalization-lighting`** is from memory. Verify it
  on the current Windows 11 build, or fall back to
  `ms-settings:personalization`.
- **Flapping.** If a writer toggles often (the LampArray service
  restarting), the banner flickers. Add a 10 s hold before clearing.
- **Daniel to decide:** whether a banner should also appear for
  *notices* (FanControl). The plan says no: notices are not conflicts.
