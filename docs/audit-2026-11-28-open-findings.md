# Open findings from the 2026-11-28 audit

A handoff for the next agent. On 2026-11-28 seven read-only audit agents
reviewed the repository: engine runtime, IPC and persistence, core effects
and editor, UI C++, QML, backend/devices/packaging, and roadmap gaps.
They reported about 80 findings. Six were fixed and merged that day. This
file lists everything that is still open.

## Before you start

- **Baseline.** `main` at `24cf1a6`, not pushed to GitHub at the time of
  writing. A Release build is clean, and CTest passes 81 of 82.
- **Known failing test.** `package_lifecycle_test` fails ("clean stop
  returns 0", "run file cleared") whenever the installed OpenRGB2 stack is
  running elevated on the machine. The stop script correctly refuses
  processes it cannot verify. The test is not isolated from the host; see
  [P-6](#p-6).
- **What was fixed** (do not redo):
  - `3830929`: `WorkspaceStore::Load` deleting generations; confirmed
    Reload/Restore deleting the draft record; a draft recovered before the
    engine attaches being replaced on attach; `ProfileLibrary` wiping a
    library it could not load.
  - `cd92979`: layer opacity applied twice; negative-speed comets.
  - Branch `fix/scene-apply-resolve`: [E-1](#e-1). `DeviceMapper::SetScene`
    resolves against the cached enumeration and stages the document under
    the worker revision (`EvaluatedFrame::scene_revision`) until the first
    frame of that revision. Regression: `TestSceneApplyResolvesWithoutResync`.
  - Branch `fix/competing-writer-scan`: [B-1](#b-1). A `lamparray` process
    rule (C++ and `start-openrgb2.ps1`), a Dynamic Lighting registry check
    (`ReadDynamicLightingEnabled`, reported as `ownership.dynamic_lighting`
    plus a conflict when on), and a re-scan every 5 s on the maintenance
    tick that logs only changes. No new IPC event kind: the Diagnostics
    page re-pulls the report while it is open.
- **Line numbers.** They come from the audit, taken before those fixes and
  the Wave commit. Files changed since then (`WorkspaceController.cpp`,
  `WorkspaceStore.cpp`, `EffectTypes.cpp`, `EffectEngine.cpp`,
  `ProfileLibrary.cpp`, `EditorController.cpp`) may have shifted. Find the
  named function first, then the line.
- **Confidence labels.**
  - **checked**: the coordinating agent re-read the code and the claim
    holds.
  - **read**: the auditing agent traced the code path; not re-checked.
  - **likely** or **speculative**: as the auditor stated.
  - Nothing here was reproduced at runtime or on hardware.
- **How to work.** Follow `AGENTS.md`. Branch per fix, write a failing test
  first, and run the full suite. Never modify
  `C:\Users\daniel\tools\openrgb-unified`. In CTest the UI tests need
  `QT_QPA_PLATFORM=offscreen` and the Qt `bin` directory on `PATH`.

## Suggested next ten

Ordered by user impact per unit of effort.

1. ~~[E-1](#e-1)~~ **fixed on `fix/scene-apply-resolve`**: the engine stopped sending output on every scene apply until
   the next 500 ms poll. **checked**
2. ~~[B-1](#b-1)~~ **fixed on `fix/competing-writer-scan`**: the competing-writer scan missed the LampArray service and
   Windows Dynamic Lighting (the G512 flicker cause), and runs only once.
   **checked**
3. [E-5](#e-5): static scenes are never re-sent after another app
   overwrites the device. Pairs with B-1.
4. [Q-1](#q-1): dragging a gradient stop destroys its own handle and leaves
   the gesture open. **checked**
5. [Q-2](#q-2): Ctrl+Z/Ctrl+Y are declared twice window-wide, and the
   viewport's single-letter keys are live on every page. **checked**
6. [I-2](#i-2): staged JSON is parsed with no depth cap; a crafted file can
   crash the engine.
7. [I-11](#i-11): a wrong-typed manifest field throws at startup, so the
   engine crashes on every launch.
8. [P-2](#p-2): re-staging a package wipes `state\` and `profiles\local\`.
9. [U-3](#u-3): the UI treats its own save as a foreign change (redundant
   re-download, false conflict).
10. [E-2](#e-2) / [P-8](#p-8): no timer-resolution management, so the 60 Hz
    tick runs in 15.6 ms steps.

---

## Engine runtime (`src/engine/`)

### E-1
**Every scene apply freezes hardware output until the next maintenance
poll.** Severity: high. **checked**

- **Where.** `DeviceMapper::SetScene` (`src/engine/DeviceMapper.cpp:59-72`)
  clears `resolved_`. `MapFrame` (`:240-251`) skips unresolved bindings.
  The only re-resolve is `mapper_.SyncDevices` in `EngineHost::PollOnce`
  (`src/engine/EngineHost.cpp:1548`, 500 ms tick). The caller is
  `src/ipc/EngineService.cpp:625-633`.
- **Trigger.** SET_SCENE, a workspace commit, and the layout verbs
  (set_position, set_orientation, set_mirror, calibrate_led, set_binding).
  A Layout drag that re-applies faster than 2 Hz keeps output frozen almost
  continuously.
- **Why tests miss it.** The e2e suites call `host.PollOnce()` by hand
  after applying.
- **Fix.** Re-resolve inside `SetScene` against the cached `snapshots_`.
  Swap the mapper document on the worker's applied revision, otherwise one
  tick flashes painted colours.

### E-2
**No timer-resolution management.** Severity: med. **likely** (effect size
not measured). Duplicate of P-8.

- **Where.** `src/engine/SceneWorker.cpp:329-357` (`cv_.wait_until`, 60 Hz),
  `DeliveryPipeline.cpp:852`, the owner loop's `sleep_for(20ms)` at
  `EngineHost.cpp:1769`, and `WorkspaceCommands.cpp:1517-1529`.
- **Effect.** Frame intervals alternate between 15.6 and 31 ms instead of
  16.7 ms. The 20 ms owner sleep rounds up to about 31 ms, so previews and
  `frame_status` publish at about 16 Hz, not 30. The repo's own benchmarks
  already record "bimodal 15.5/31 ms" (`docs/benchmarks/baseline.md:204`).
- **Fix.** Use a high-resolution waitable timer
  (`CreateWaitableTimerExW(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)`) or
  `timeBeginPeriod(1)` for the engine's lifetime. Replace the 20 ms sleep
  with a wait on the next due time.

### E-3
**The 2 s topology refresh stalls every lane.** Severity: med. **read**

- **Where.** `EngineHost.cpp:1511-1519` calls `sdk_->RefreshStatus()`
  (`src/backends/openrgb/OpenRgbClient.cpp:511-689`), which holds
  `io_mutex_` across one full round trip per controller. Every lane's
  `SubmitLatest` (`:1103-1107`) takes the same mutex. Reconnect attempts
  also run on lane threads.
- **Test gap.** `TestBlockedWirelessKeepsWiredRate` uses a fake transport
  that blocks per controller, so it cannot show this.
- **Fix.** Put status traffic on a second SDK connection, or probe only the
  controller count every 2 s and refresh one controller per poll. Move
  reconnect out of the send path.

### E-4
**Identify on an unbound controller, then binding it, rejects frames as
out-of-order.** Severity: med. **read**

- **Where.** `DeviceMapper.cpp:545` increments `emitted_[key].seq` for
  overlay-only frames. The reseed at `:315-324` resets `st.seq = 0` while
  the lane's `max_seq` stays at K (`DeliveryPipeline.cpp:260-272`).
- **Effect.** After identify then set_binding, output to that controller is
  missing for as long as the identify had run (3–30 s).
- **Fix.** Do not reset `seq` when only the baseline is reseeded within the
  same generation. Add a mapper test for identify followed by a bind.

### E-5
**A static scene is never re-asserted.** Severity: med. **read**. Three
auditors raised this independently.

- **Where.** `DeliveryPipeline.cpp:549-566` suppresses frames identical to
  `last_colors` with no periodic resend. The only recovery is wake
  (`PowerLifecycle.cpp:379-416`).
- **Effect.** G Hub, Dynamic Lighting or a firmware reset leaves the device
  wrong until the scene itself changes.
- **Fix.** Add a per-lane keyframe that bypasses suppression after 2–5 s
  (configurable, 0 = off) and count it separately in the stats.

### E-6
**Uploaded-playback mode still evaluates and maps at 60 Hz and throws the
result away.** Severity: med-low. **read**

- **Where.** `EngineHost.cpp:1026-1040` and
  `PlaybackController.cpp:969-1046`. `Tick` also deep-copies every snapshot
  about 30 times a second.
- **Fix.** Add an "evaluate gate" to `SceneWorker` while
  `PlaybackOwnsWired()`, and cache the wired-target list per topology
  generation.

### E-7
**The suspend gate is applied late.** Severity: low-med. **read**.
Duplicate of P-11.

- **Where.** `WinPowerMonitor.cpp:58-61` only queues `PBT_APMSUSPEND`. The
  gate runs on the next `PollOnce`, up to 500 ms later or after a blocking
  SDK call.
- **Fix.** Pause the pipeline output synchronously from the monitor thread,
  or block the window procedure on an acknowledgement for up to about 1.5 s.

### E-8
**`PreviewSession::Sample` skips optimisations the output path already
has.** Severity: low. **read**

- **Where.** `PreviewSession.cpp:194-316`.
- **Detail.** `ResolveWorldMatrices` runs on every sample, `EmitterColor`
  is called for every emitter, `key_positions` is copied under a lock, and
  `OutputOwner` does a linear scan three times per linked object.
- **Fix.** Cache world matrices and an id-to-object index per applied draft.

### E-9
**Data race on `engine_state_.active_workspace`.** Severity: low. **read**

- **Where.** Read without a lock at `EngineHost.cpp:1266-1269`; written
  under `state_mutex_` at `:1085-1103`.
- **Cause.** IPC starts before `ApplyStartupScene`.
- **Fix.** Copy it under the lock, or start the listeners after re-adopt.

### E-10
**Shutdown bounds are measured, not enforced; OS shutdown skips teardown.**
Severity: low. **read**

- **Where.** `OrderlyShutdown.cpp:54-72` has no try/catch and no watchdog.
  `main.cpp:224-228` installs only signal handlers.
- **Related.** The power monitor's `HWND_MESSAGE` window never receives
  `WM_ENDSESSION` (P-11).
- **Fix.** Catch per step, add a watchdog that exits after `TotalBound()`,
  and add `SetConsoleCtrlHandler` or a hidden top-level window that handles
  `WM_ENDSESSION`.

### E-11
**`QUIT {stop_backend}` race.** Severity: med. **checked**

- **Where.** `EngineHost.cpp:1151` sets `stop_` before
  `ResolveManagedBackend` stores `pending_backend_pid_` (`:1165-1176`).
- **Effect.** If the main loop wakes in that window, `Stop()` sees pid 0
  and skips the backend handoff while the reply says "closing".
- **Fix.** Resolve and store the pid first, or read the pid inside the
  step.
- **Test gap.** `e2e_lifecycle_test.cpp:1113-1129` calls `Stop()` by hand.

### E-12
**Engine-layer features.** Severity: feature.

- Per-controller send-budget overrides: `BudgetFor` is hard-coded at
  `EngineHost.cpp:50-91`.
- An exit/idle output policy: `Stop()` leaves the last Direct frame frozen
  on the LEDs.
- Tick-cadence and per-lane latency histograms in `DeliveryStats`.

## IPC and persistence (`src/ipc/`, `src/engine/Workspace*`)

### I-2
**Staged documents are parsed with no nesting cap, then deep-copied
recursively.** Severity: high (crash from an untrusted file). **read**

- **Where.** `WorkspaceService.cpp:1734` and `:1076` use plain
  `nlohmann::json::parse`. `StudioConfig.cpp:948-951` copies `extensions`
  recursively.
- **Contrast.** The wire parser caps depth at 64 (`IpcProtocol.h:60-64`).
- **Effect.** A stack overflow is an SEH fault that nothing catches.
- **Fix.** Use `ParseJsonBounded` for every staged and bundle parse, and
  add a deep-nesting test.

### I-3
See [E-11](#e-11).

### I-4
**WELCOME/STATE/SCENE/DIAGNOSTICS inline the whole scene.** Severity: med.
**read**

- **Where.** `EngineService.cpp:693-718` and `:1252-1257`;
  `IpcServer.cpp:698-735`.
- **Effect.** Past roughly 5,400 emitters (about 1 MiB) no client can
  connect.
- **Doc mismatch.** `docs/ipc-protocol.md` §5 says scene content moves only
  through transfers.
- **Fix.** Send revision, name and hash; serve the document through a
  transfer; count dropped oversize events.

### I-5
**Finished upload tokens expire 60 s after `transfer_finish`.** Severity:
med. **read**

- **Where.** `WorkspaceTransfer.cpp:699-778`; `idle_expiry_ms = 60000`.
- **Effect.** An import dialog left open for more than 60 s fails on
  commit. `transfer_expired` is documented but never emitted.
- **Fix.** Refresh on use, pin tokens held by queued jobs, and emit the
  error.

### I-6
**Handlers touch files and wait on the worker.** Severity: med. **read**

- **Where.** `WorkspaceCommands.cpp:256-297` and `:341`;
  `WorkspaceService.cpp:793-847`, `:1803` and `:3208-3223`;
  `EngineService.cpp:670-673` with `ProfileStore.cpp:1193-1260`.
- **Effect.** The 2 s `get` poll re-reads and hashes the document and
  manifest under the commit mutex. Every SET_SCENE does a durable write on
  the reader thread. Contradicts `docs/ipc-protocol.md` §3 and §5.
- **Fix.** Publish the bytes and hash from the worker into the snapshot,
  and coalesce `persist_scene` on a background writer.

### I-7
**Event bodies are copied two or three times and serialised twice; preview
frames are encoded under the service mutex.** Severity: med. **read**

- **Where.** `EngineService.cpp:373-382` and `:453-465`;
  `IpcServer.cpp:264-269` and `:351-356`; `WorkspaceCommands.cpp:1578-1612`.
- **Fix.** Move the JSON into the event, dump once outside the locks, and
  pack colours as one string per object.

### I-8
**Preview frame "latest-wins" never coalesces.** Severity: low-med. **read**

- **Where.** The key `frame/<sequence>/<chunk>` at
  `WorkspaceCommands.cpp:1608-1611` changes every frame.
- **Effect.** Overflow evicts the queue head regardless of kind, so it can
  drop job-terminal events.
- **Fix.** Key on `frame/<chunk>` and evict high-rate kinds first.

### I-9
**An elevated engine trusts any medium-integrity same-user client, and the
client accepts a same-user squatter.** Severity: low-med. **likely**.
Duplicate of U-6.

- **Where.** `NamedPipe.cpp:148` (no mandatory label) and `:215-227`;
  `NamedPipeClient.cpp:298-306` and `:405-416`.
- **Fix.** When elevated, set a High mandatory label or check the client's
  elevation; in the client, require a server integrity level at least equal
  to its own.

### I-10
**`openrgb2-ctl` sends one request per connection.** Severity: low-med.
**read**

- **Where.** `tools/engine_ctl/engine_ctl.cpp:707-725`.
- **Effect.** Session-bound verbs (transfers, jobs, export, preview) cannot
  work; `diag` ignores `--pipe`.
- **Fix.** Add composite commands that stay in one session, and derive the
  `-diag` pipe name from `--pipe`.

### I-11
**A wrong-typed manifest field throws out of `Load`.** Severity: low-med
(crash loop). **read**

- **Where.** `WorkspaceStore.cpp:467-468`, `:215-216` and `:525-530`.
  Startup calls it at `EngineHost.cpp:1270` with no try/catch.
- **Fix.** Check types the way `ReadMarker` does, and wrap the startup
  `Open` in a catch that continues unmanaged.

### I-12
**Smaller protocol and doc mismatches.** Severity: low. **read**

- A download transfer's `kind` is never checked against its content.
- `resource_get` inlines documents larger than 1 MiB.
- Undocumented fields: `marker_error`, `backend_pid`, `sessions_total`,
  `scene.workspace.managed`.

### I-13
**Seen by the auditor but not ranked.**

- Lost updates when two sessions send layout verbs concurrently
  (`EngineService.cpp:2090-2097`).
- A SUBSCRIBE initial snapshot can queue after a newer event
  (`IpcServer.cpp:1006-1038`).
- `Commit` installs library files before later steps that can fail
  (`WorkspaceStore.cpp:1714-1731`).
- Integer narrowing in `calibration_show` (`WorkspaceCommands.cpp:1880`).
- Unbounded `controllers` list in `add_object`.
- Workspace ids accept Windows reserved names and are case-sensitive.

## Core effects and editor (`src/core/`)

### C-2
**Changing speed or wavelength makes the pattern jump in proportion to
uptime.** Severity: med-high. **read**

- **Where.** Phase is `Wrap1(t*speed/scale)` with `t` = seconds since the
  worker started (`EffectTypes.cpp:232-267`, `:334`; `Presets.cpp:192-201`;
  `SceneWorker.cpp:209`).
- **Effect.** At t = 600 s, one 0.01 step of the Wave speed slider shifts
  the phase by 4.8 cycles.
- **Fix.** Keep phase continuous when layers are replaced: store a
  per-layer phase offset, or integrate the phase per layer.

### C-4
**`SetFlow` writes a world-space direction onto local-space and targeted
wave layers.** Severity: med. **read**. Was WIP; now committed in
`c5e2f38`.

- **Where.** `EditorController.cpp`, `SetFlow` (about `:2526-2550`) and
  `flowState`.
- **Effect.** On Reactor, "Front → Back" turns the local-space RAM wave
  into a uniform flash.
- **Fix.** Restrict flow edits to untargeted world-space waves, and use the
  same predicate for `flowState().available`.

### C-5
**`SetFlow` colour and stop handling.** Severity: low-med. **read**

- The palette target depends on which wave is *enabled*, and disabled
  waves are skipped.
- Any colour edit re-spaces every stop evenly (Aurora 0/.35/.65/.85 →
  even).
- If `speed0 <= 0`, every wave is flattened to the same speed.
- Related ([U-12](#u-12)): `flowState` returns all stops, but `setFlowColors`
  accepts only 2–5, so 6+ swatches are refused and a 1-stop palette can
  only use "+".

### C-6
**The layer gesture is not cancelled on undo or document swap.** Severity:
med. **read**

- **Where.** `EditorController.cpp:1803-1840`; callers in
  `WorkspaceEditSession.cpp` and `ObjectEditorController.cpp:1324,1339`.
- **Effect.** Hold an inspector slider, press Ctrl+Z, release: the next
  undo re-applies the edit you just undid.
- **Fix.** Add `CancelGestures()` covering both gesture kinds and call it
  at the session entry points; refuse undo/redo while
  `LayerGestureActive()`.

### C-7
**Layer targets do not work with instance ids.** Severity: med. **read**

- **Where.** `EffectTypes.cpp:203-218` (`LayerMatches`, exact compare
  only). `Rename` (`EditorController.cpp:609-724`) does not re-key targets.
  `reactor.effect.json:26` hard-codes `mouse/strip`.
- **Fix.** Match on a `/`-boundary prefix, re-key targets on rename, and
  add a type/category target form.

### C-8
**`InputBus::Idle()` is never true again after the first event.**
Severity: med-low (perf). **checked**

- **Where.** `src/core/inputs/InputBus.cpp:60-86`. `Snapshot` filters but
  never prunes; `ClearEvents` has no callers.
- **Effect.** `ReactiveInputs::Snapshot` copies `InputState` 60 times a
  second forever, defeating commit `215c225`.
- **Fix.** Prune expired events in `PushEvent` and `Snapshot`.

### C-9
**Per-LED hot path recomputes invariants; comet allocates per LED.**
Severity: med at high LED counts. **read**

- **Where.** `EffectTypes.cpp:283` (`PathArc` allocates `seg_len` per LED),
  `:487-503` (string-compare dispatch), and the per-LED `Normalize`,
  `1/scale` and phase calculations.
- **Fix.** Build a `PreparedLayer` in `SetLayers` (primitive enum,
  normalised direction, segment lengths) and compute per-frame terms once
  per layer.

### C-10
**`Evaluate` does redundant per-object work.** Severity: low-med. **read**

- **Where.** `EffectEngine.cpp`: `OutputOwner` is a linear scan per object
  (it always returns the object itself), and the frame map is cleared and
  reallocated every tick.
- **Fix.** Use `obj.id` directly and reuse the frame entries.

### C-11
**Wave is missing from the golden-fixture tests.** Severity: med
(test gap). **read**

- **Where.** `tests/core/effect_json_test.cpp`: both id lists still have
  nine entries (`cases_total == 144`), and there is no
  `wave.fixture.json`.
- **Also add.** Tests for `InputBus::Idle` after events expire, and for
  target matching by instance id.

### C-12
**Float-narrowing paths can still produce zero, inf or NaN.** Severity: low.
**read**

- `SetLayerField(Scale, 1e-50)` narrows to 0.
- `effects.speed: 1e39` narrows to inf (`StudioConfig.cpp:563-571`).
- `SetFlow` rescales with no finite check.
- `Clamp01(NaN)` returns NaN, and `(unsigned)(NaN*255)` is undefined
  behaviour (`EffectTypes.cpp:18,43-49`).
- **Fix.** Re-check after narrowing, and make `Clamp01` NaN-safe.

### C-13
**Cheap feature extensions.** Severity: feature.

- `multiply` and `lighten` blends (one switch case plus an enum string).
- A non-wrapping palette mode: `Sample(1.0)` currently returns stop 0.
- `sparkle` and `breathe` primitives.
- Saw/triangle wave band shapes.
- Gamma-aware blending.

### C-14
**`ZoneLayoutToPoints` bakes addresses that override per-instance
`addr_base`.** Severity: low (latent). **read**

- **Where.** `src/core/presets/DevicePreset.cpp:958-991`.

### C-15
**Unranked.**

- `OnsetDetect.cpp:52` divides by `3 - factor`, which is zero at
  sensitivity 0.
- `PresetList()` returns a static vector rebuilt on every call, so
  `FindPreset` pointers dangle.
- `gradient` and `wave` use `direction` and `scale` with opposite meanings.

## UI process C++ (`src/ui/`)

### U-3
**The UI treats its own commit as a foreign change.** Severity: med.
**read**

- **Where.** `WorkspaceController.cpp`, `OnJobEvent` commit path (about
  `:1930-1934`), `ApplySnapshot`, `AdoptFetchedDocument`.
- **Effect.** Every save is re-downloaded; editing during that window
  raises a false "changed on the engine"; `workspaceRevision` stays stale.
- **Fix.** Record the committed hash and skip the fetch when it matches.
  Emit `workspaceChanged` when revisions move.

### U-5
**The diagnostics journal is flooded by preview events.** Severity: med.
**read**

- **Where.** `DiagnosticsController.cpp:714-739` filters only
  `frame_status`.
- **Effect.** A few seconds on Lighting or Layout evict every real error.
- **Fix.** Skip `preview` frames in the journal, and gate
  `RecomputeRates` on the page being active.

### U-6
**An elevated UI trusts a same-user pipe server, and the engine-supplied
path reaches the shell.** Severity: med (security). **read**. See also
[I-9](#i-9).

- **Where.** `openWorkspaceFolder` (`WorkspaceController.cpp:1256-1268`)
  passes `workspace_dir` to `QDesktopServices::openUrl`.
- **Fix.** Require `QFileInfo::isDir()`, launch `explorer.exe` explicitly,
  and require an Administrators/SYSTEM pipe owner when elevated.

### U-7
**A lost or early job-terminal event latches `saving`, import or export
forever.** Severity: med. **read**

- **Where.** `WorkspaceController.cpp:1862-1948`. Only the test hook polls
  `job_status`.
- **Fix.** Buffer terminal events for unknown job ids until the accept
  reply arrives, and add a watchdog that polls `job_status`.

### U-8
**Draft autosave never fires during continuous editing.** Severity:
med-low. **read**

- **Where.** `WorkspaceDraftStore.cpp:105-106`: the 2 s debounce is
  restarted on every edit with no maximum wait.
- **Fix.** Add a maximum wait so the draft is written at least every few
  seconds while editing.

### U-9
**A fetched document is labelled with whatever revision is current when the
download finishes.** Severity: low-med (lost update). **likely**

- **Where.** `FinishDownload` / `AdoptFetchedDocument`.
- **Fix.** Compare the transfer's sha256 with `ws_sha256_` and refetch on
  mismatch.

### U-10
**Per-frame work on the GUI thread.** Severity: med-low. **read**

- **Where.** `EngineBridge.cpp:188`, `:340-344`, `:377-397`, `:436-441`
  and `:503-505`; `LayoutController.cpp:656-665` and `:788-804`.
- **Detail.** Each `frame_status` is deep-copied and converted from JSON
  to QVariant per LED with no latest-wins. Polled stores re-emit even when
  nothing changed.
- **Fix.** Coalesce into one pending frame slot, emit only on change, and
  coalesce `QueueViewNotify`.

### U-11
**The `EngineWireClient` writer can exit before the session goes live.**
Severity: low. **read**

- **Where.** `EngineWireClient.cpp:388-391` vs `:105-111`.
- **Fix.** Set `live_` before starting the writer thread.

### U-12
**Wave card adapter.** Severity: low-med. **read**

- The stop-count mismatch (see C-5).
- `ApplyFlow` refuses silently during a gesture, with no `statusMessage`.
- There are no `tests/ui` cases for `flowState` / `setFlow*`.

### U-13
**Unranked.**

- `save()` sends the document inline, so a document over the 1 MiB IPC
  bound fails with a misleading "connection" error.
- `StudioViewportModel` never resets `sequence_` on re-begin (latent).

### U-14
**Not reviewed.** `ObjectEditorController`, `DevicePresetController`,
`CalibrationController`, the remaining models, and `UiSettings`.

## QML (`ui/`)

### Q-1
**Dragging a gradient stop destroys its own handle.** Severity: high.
**checked**

- **Where.** `ui/studio/PaletteEditor.qml:36-41`, `:71-74` and `:116-157`.
- **Cause.** The handles are a `Repeater` over a JS array that `reload()`
  reassigns on every `stackChanged` during the drag. `onReleased` never
  fires, so the layer gesture stays open, and the next slider press reverts
  edits. There is also no `preventStealing` or `onCanceled`, so the page
  scroll can steal the drag (the same class as `21a7b63` / `8d41aa2`).
- **Fix.** Use an integer or C++ model so handles survive reloads; add
  `preventStealing: true` and `onCanceled: cancelLayerGesture()`.

### Q-2
**Duplicate window-wide shortcuts.** Severity: high. **checked**
(declarations); Qt's ambiguous-shortcut behaviour is not executed.

- **Where.** Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y appear in both
  `ui/Main.qml:223-264` and `ui/studio/viewport/StudioViewport.qml:884-889`.
  `W/E/P/F/Home/Escape` (`:872-891`) are live on every page because the
  `LayoutPage` loader stays alive.
- **Fix.** Drop the viewport's undo shortcuts, and gate the rest on
  `visible && activeFocus` or move them to `Keys.onPressed`.

### Q-3
**Wave direction sliders fight the normalisation.** Severity: med. **read**

- **Where.** `ui/studio/WaveControls.qml:152-175`.
- **Effect.** On an axis-aligned direction a slider can only flip sign;
  dragging X moves Z.
- **Fix.** Use azimuth/elevation controls.
- **Also.** "+" plus a cancelled colour dialog leaves a duplicate stop
  (`:223-236`).

### Q-4
**The Layout page has no Save and Ctrl+S is Lighting-only in workspace
mode.** Severity: med. **read**

- **Where.** `ui/Main.qml:203-209`; `LayoutPage.qml:257-358` and `:1019`.
- **Also.** The hint "Edits apply immediately" is false for a managed
  workspace.

### Q-5
**The draft preview copies the whole `viewport.objects` list every
frame.** Severity: med (perf). **read**

- **Where.** `LightingPage.qml:1768-1771` (the Repeater is rebuilt per
  frame); `LayoutPage.qml:79-86` and `:789-801` (`draftColorsFor` is
  O(objects²)).
- **Fix.** Use `viewport.colorsFor(id, n)` and stable models.

### Q-6
**A full delegate rebuild on every layout edit drops focus and scroll
position.** Severity: med. **likely**

- **Where.** `LayoutPage.qml:96-99`, `:204-207`, `:656-706` and
  `:1488-1605`.
- **Effect.** The arrow-key nudge works only one step at a time; the
  calibration list jumps to the top.

### Q-7
**The `TransformInspector` scrub strip has no `preventStealing`; fields
keep refused text.** Severity: med. **read**

- **Where.** `ui/studio/TransformInspector.qml:183-244`.

### Q-8
**Workspace dialog replace confirmation is hidden behind the modal.**
Severity: med. **read**

- **Where.** `WorkspaceActionsDialog.qml:42`, `:145-151` and `:218-231`
  versus `Main.qml:546-561`.
- **Effect.** Open, Reload and Restore over a dirty draft appear to do
  nothing until the dialog is closed.

### Q-9
**Three modal popups never take keyboard focus.** Severity: med (a11y).
**likely**

- **Where.** `WorkspaceActionsDialog.qml`, `WorkspaceImportDialog.qml`,
  `DevicePresetEditor.qml`.
- **Fix.** Add `focus: true` and focus the first field in `onOpened`.

### Q-10
**Inspector fields commit only on Enter; the source badge never
refreshes.** Severity: low-med. **read**

- **Where.** `EffectInspector.qml:142-168` and `:244-282`;
  `PaletteEditor.qml:177-189`.

### Q-11
**Lighting controls drift from engine state.** Severity: low-med. **read**

- The reactive checkboxes do not re-assert (`LightingPage.qml:1478-1503`,
  `:1246-1263`).
- The uploaded-preset colour well is hard-coded red (`upColor` is unused;
  `:53`, `:1284-1332`).

### Q-12
**No scroll bars on any page; focus does not scroll into view.** Severity:
med (a11y). **read**

- **Also.** The 2D/3D toggle never shows its checked state
  (`LayoutPage.qml:424-437`).

### Q-13
**Import/Export dialog problems.** Severity: low-med. **read**

- **Where.** `WorkspaceImportDialog.qml`.
- Escape mid-operation does not cancel it.
- The "done" phase dead-ends.
- The "replace layout" option is sticky across imports.
- Paths are free text, although `FileDialog` is used elsewhere.

### Q-14
**UI features.** Severity: feature.

- A shared themed colour picker for stops, the Lighting palette and Wave.
- Delete and duplicate keys for layout objects and layers.
- A confirmation for profile and type "Delete".
- Instanced spheres in the 3D viewport (`StudioViewport.qml:584-626` draws
  two Models per LED).

## Backend, SDK, platform, packaging

### B-1
**The competing-writer scan misses the G512 flicker causes and runs once.**
Severity: high for this PC. **checked**

- **Where.** `src/engine/OwnershipScan.cpp:257-259` matches `lghub`,
  `lcore.exe` and `logioptions` only. The service image here is
  `logi_lamparray_service.AMD64.exe`.
- **Missing.** Windows Dynamic Lighting is a registry setting
  (`HKCU\Software\Microsoft\Lighting\AmbientLightingEnabled`), not a
  process.
- **Runs once.** `ScanOwners` is called only once (`EngineHost.cpp:775`).
- **Parity rule.** `start-openrgb2.ps1` must match the engine (test T1
  enforces parity).
- **Fix.** Add a `lamparray` token and a Dynamic Lighting registry check,
  re-scan on the maintenance tick, and push a diagnostics event. Pair with
  E-5.

### B-2
**`stop-openrgb2.ps1` taskkill fallback.** Severity: claimed high.
**checked** that the line exists; **doubtful** that it aborts.

- **Where.** `packaging/windows/stop-openrgb2.ps1:405` runs
  `$null = & taskkill.exe ... 2>&1` under `$ErrorActionPreference = "Stop"`.
  The auditor says this terminates on Windows PowerShell 5.1.
- **Counter-evidence.** The CTest run took that path ("posting WM_CLOSE")
  and continued.
- **Before fixing.** Reproduce under `powershell.exe` 5.1 with a
  windowless backend.
- **Also.** Step 2 signals the backend even when the engine did not exit
  (`$engineGone` is never consulted).

### P-2
**Re-staging wipes user state of a package that was run in place.**
Severity: high (data loss). **read** plus on-disk evidence.

- **Where.** `packaging/windows/stage-release.ps1:285`:
  `Remove-Item -Recurse -Force $OutDir`.
- **Evidence.** `build\package\OpenRGB2-0.1.0-desktop-win64` contains
  `state\` and `profiles\local\`.
- **Fix.** Preserve those two folders, and refuse to wipe a directory that
  has no `manifest.json`.

### P-4
**The sign-in task runs the stack at Below Normal priority.** Severity:
med-high. **checked** (the call has no `-Priority`).

- **Where.** `packaging/windows/startup-task.ps1:232-235`. The scheduler
  default is 7 = below normal, plus low I/O priority, and children inherit
  it. The installed task here reports Priority 7.
- **Fix.** Add `-Priority 4` and assert it in `Test-OpenRGB2StartupTask`
  and the test stub.

### P-5
**The SDK client ignores `DEVICE_LIST_UPDATED`.** Severity: med. **read**

- **Where.** Packet 100 is defined in `SdkWire.h:25,53` but dropped
  (`OpenRgbClient.cpp:204-252`). The vendored server sends it to every
  client (`NetworkServer.cpp:910-920`).
- **Effect.** Frames can hit a stale controller index for up to 2 s after a
  rescan.
- **Fix.** Read the packet in the per-frame `PollIo` select, and bump the
  topology generation when it arrives.

### P-6
**`package_lifecycle_test` depends on host state.** Severity: med
(test infra). **checked**

- **Detail.** It fails whenever an elevated install is running.
- **Fix.** Use unique image names or paths in the test install, or skip
  with a clear message when foreign same-named processes are unverifiable.

### P-7
**Logitech 0x8080 pacing is open-loop.** Severity: med. **read**

- **Where.** `vendor/OpenRGB/Controllers/LogitechController/LogitechHIDPP20Controller/LogitechHIDPP20Controller.cpp:5545-5590`
  (commit `9c866ca`).
- **Cost.** Every update repaints all keys with a fixed 2 ms sleep per
  frame, which caps the keyboard at about 40–50 Hz. ACKs are never read.
- **Missing records.** The change is not recorded in
  `vendor/provenance/MANIFEST.md`.
- **Not deployed.** It needs a qmake backend rebuild:
  `build-and-deploy.ps1:151` does not rebuild the backend.
- **Fix.** Send only changed key groups (a `sent_colors` delta exists on
  the other path), or pace on the ACK.

### P-8
Duplicate of [E-2](#e-2) (timer resolution). The fix also applies to
`tools/bench/bench_producer.cpp:41`.

### P-9
**A trailing backslash on `-InstallDir` makes update copy the install into
itself.** Severity: med. **read** (reproduced by a sub-audit in scratch).

- **Where.** `update-openrgb2.ps1:407,475,506`;
  `rollback-openrgb2.ps1:215-216`.
- **Fix.** `.TrimEnd('\','/')` after `Resolve-Path`, as `stage-release.ps1`
  does.

### P-10
**The Lian Li transport never reopens on persistent timeouts.** Severity:
low-med. **speculative** trigger.

- **Where.** `vendor/OpenRGB/Controllers/LianLiWirelessController/LianLiWirelessTransport.cpp:26`,
  `:45-50` and `:159-169`.
- **Fix.** Close after about 3 consecutive failures, and shorten the write
  timeout.

### P-11
**Power events.** See [E-7](#e-7) and [E-10](#e-10).

### P-12
**Lian Li discovery polling costs 30–130 ms of link time per poll.**
Severity: med (multi-group). **read**

- **Where.** `LianLiWirelessTransport.cpp:322-462`;
  `LianLiWirelessRuntime.cpp:76-85`.
- **Poll storm.** After a group has been unseen for more than 1 s, every
  frame triggers a poll.
- **Fix.** Read 64-byte chunks and stop at the record count, share one
  reply across groups, and clamp the next poll time.

### P-13
**Test and build gaps.**

- There is no `TcpSocket` test (every suite injects a fake transport).
- There is no unsolicited-packet client test.
- `LianLiWirelessTransport.cpp` is in no test target.
- No test runs `stage-release.ps1`.
- The build uses `/W3` only, with no `/W4` or ASan preset; the standard is
  C++17.

### P-14
**Unranked.**

- `-VerifyOnly` never hashes anything (`stage-release.ps1:572-726`).
- `start-openrgb2.ps1 -Elevated` drops `-ScanOnly` and always returns 0
  (`:389-401`).
- An engine that dies at startup leaves the backend orphaned (`:618-623`).
- The manifest's backend pin is a hard-coded string
  (`stage-release.ps1:552`).
- One unparsable controller fails the whole enumeration.

## Roadmap and plan gaps

Source: the gap-analysis agent's read of `IMPLEMENTATION_PLAN.md`, `docs/`
and git history.

### Plan versus reality

Every milestone is ticked, but:

- **M1.** The old Studio path was never benchmarked.
- **M2A.** Visible FPS and clock-to-phase were never measured.
- **M5.** The 8 h soak was replaced by 65 + 60 min; physical unplug and
  suspend/resume were never run; the DIMMs were never qualified.
- **M6.** Unchanged in substance (signing is recorded separately in the
  vault note).
- **Physical measurement.** `MeasurementProvenance::PhysicallyMeasured`
  (`src/engine/EngineTypes.h:217`) is never assigned anywhere.

### Open backlog from the docs

- N01 physical retest of the wireless wedge.
- N03 policy for re-importing an existing workspace id.
- Bundle inspect warns "unknown preset" for personal looks.
- The ASan / Application Verifier pass suggested for the F01/F02 crashes
  was never done.
- Backend CPU is 26–78% of a core; engine memory slope is unproven.
- The first-look "stalled window" anomaly was never reproduced.
- Unidentified LEDs: GPU fan rings, `LED_C`, `Chipset Accent`.

### Stale TODO

`src/engine/EngineHost.h:278` says "stub: true until HasSubscriber" — it is
implemented.

### Feature proposals

Each was checked as absent first.

| # | Feature | Builds on | Size |
| --- | --- | --- | --- |
| R-1 | Tray quick controls (profile, brightness, pause) | `src/ui/main.cpp:496-509`, `ProfileLibrary`, `EngineBridge` | S |
| R-2 | Global hotkeys (next profile, brightness, lights off) | `src/platform/windows/WinKeyHook.cpp` | S/M |
| R-3 | Per-app/game profiles (foreground watcher, engine-side) | `src/platform/windows/`, `ProfileStore.h:82` | M |
| R-4 | Schedule / lock / idle dimming | `WinPowerMonitor.cpp`, `PowerLifecycle` | S/M |
| R-5 | Temperature-reactive scalar input (read-only FanControl/HWiNFO export) | `InputBus.h:36-39`, `level` primitive | M |
| R-6 | Audio spectrum bands (8–16 band FFT + primitive) | `WinAudioLoopback.cpp` | M |
| R-7 | DXGI Desktop Duplication screen capture | `WinScreenSampler.cpp:96-131` (GDI today) | M |
| R-8 | Automation: `engine_ctl` profile/brightness/input verbs, then a loopback HTTP/WebSocket bridge | `tools/engine_ctl` | S / M |
| R-9 | Supervisor: restart a dead engine or backend, minidumps, auto-restart on wireless `Failed` | `src/ui/main.cpp:678` | M |
| R-10 | Competing-writer guard + re-assert | B-1 + E-5 | S |
| R-11 | Compile deterministic looks into uploaded loops (60 fps on wireless) | `src/core/playback/PlaybackLoop.cpp` | L |
| R-12 | Single-look export/import; generalise the `WaveControls` pattern to other looks | `docs/studio-import.md:71` | S/M |

### Hygiene

- **No CI.** There is no `.github/`.
- **No static analysis.** There is no sanitiser or clang-tidy setup.
- **Benchmarks.** Results live only in gitignored `profiles/local/`.
- **Stale docs:**
  - `AGENTS.md` says "four views"; there are five.
  - `docs/ownership.md:10,76` calls managed startup "future M6".
  - `docs/hardware/support-matrix.md:26` calls the ARGB headers
    unsupported.
  - `packaging/windows/RELEASE.md:104` says three uploaded presets; there
    are nine.
  - `vendor/provenance/MANIFEST.md:90` still lists landed panels as
    deferred.
- **Large files worth splitting.** `WorkspaceService.cpp` (3.3k lines),
  `EngineService.cpp`, `WorkspaceController.cpp`, `EditorController.cpp`,
  `WorkspaceStore.cpp`, `LightingPage.qml`, `LayoutController.cpp`,
  `LayoutPage.qml` (each about 2–3k lines).
- **Backlog location.** `docs/qa/` (where the backlog lives) is gitignored,
  while 159 evidence files under `task12-evidence/` are committed.
