# R-8: Automation — `engine_ctl` verbs, then a loopback HTTP/WebSocket bridge

Status: proposed · Size: M (part A is S, part B is M) · Source: audit 2026-11-28 R-8

## Goal

Scripts, Stream Deck buttons and home-automation tools can switch the
active look/workspace, set brightness and toggle reactive inputs without
the UI. Part A adds friendly `openrgb2-ctl` verbs. Part B adds an opt-in,
token-protected loopback HTTP + WebSocket bridge that exposes the same
verbs and the event stream.

## Current state

- `tools/engine_ctl/engine_ctl.cpp` (802 lines, target `openrgb2-ctl`)
  speaks the command pipe. Commands:
  - `ping|state|scene|snapshots|playback|stats|presets|quit` (`:560-580`)
  - `diag`
  - `set-scene <file>` (`:586`)
  - `set-playback` (`:605-646`)
  - `workspace <verb> [json]` (`:648-667`, the raw `WORKSPACE_COMMAND`)
  - `subscribe` (`:668-679`, with a 5 s ping keepalive at `:720-790`)
  - `raw`
  - `sdk-*`

  There is no profile, brightness or reactive verb, and the argument
  parsing is inline in `main`, so none of it is unit-testable. No CTest
  covers the tool. `tests/packaging/package_lifecycle_tests.ps1:121` only
  copies it.
- **The "profile" today.**
  - In workspace mode (the packaged default), the active look is a
    managed workspace. The UI switches workspaces with `WORKSPACE_COMMAND
    reload {id}`, a job (`WorkspaceController::DoReload`,
    `src/ui/WorkspaceController.cpp:1168-1202`). `list` returns
    `{workspaces, active}` (`WorkspaceCommands.cpp:436-440`).
  - The legacy UI-side `ProfileLibrary` (`AppData/OpenRGB2/scene-library.json`,
    `src/ui/ProfileLibrary.cpp:37-40`) is UI-private. Its header says the
    wire has no profile-enumeration verb (`ProfileLibrary.h:4-15`).
- **Brightness** is `SceneDocument::brightness` (`SceneTypes.h:260`), or
  `StudioDocument::brightness` in workspace mode. The UI changes it only
  by committing the document (`SceneEditController::setBrightness`,
  `src/ui/SceneEditController.cpp:772-799`). No engine-side output scalar
  exists.
- **Reactive inputs.** `REACTIVE_COMMAND` `set_enabled` and `set_params`
  (`EngineService.cpp:2690-2720`, `docs/ipc-protocol.md:466-467`) already
  exist. They are only reachable through `raw 0x010d`.
- **Pipe ACL.** The pipe ACL is the creating user plus SYSTEM
  (`NamedPipe.h:11-15`, SDDL at `NamedPipe.cpp:148`). The QA server shows
  the in-repo pattern for a loopback socket: a per-launch token in a
  per-user token file, refused when elevated (`AGENTS.md`, QA harness;
  `src/ui/QaServer.h`).

## Design

**Part A — CLI verbs (S).**
- Move the argument-to-request mapping out of `main` into
  `tools/engine_ctl/CtlCommands.{h,cpp}`: `BuildRequest(args) ->
  {MsgType, body, follow_up}`, a pure function. `main` keeps only I/O.
- New verbs:
  - `look list` maps to `workspace list` and prints the ids, with
    `*` on the active one.
  - `look apply <id>` sends `reload {id}`, then polls `job_status` until
    the job is terminal, with a bounded wait.
  - `input <audio|screen|key> on|off` sends `REACTIVE_COMMAND`
    `set_enabled`.
  - `input screen display=<n>` and `input audio sensitivity=<x>` send
    `set_params`.
  - `brightness <0-100>`: see the decision below.
  - `playback <mode> ...` is an alias of `set-playback`.
- `--json` prints the raw reply for scripts. Exit codes: 0 ok,
  1 transport, 2 usage, 3 refused (an engine `Error` reply).
- **Brightness (Daniel decides).**
  - **(a) Persisted.** `workspace get`, set `brightness`, then `commit`
    with `base_revision` and `apply`. It survives a restart, but it bumps
    the workspace revision and makes an open UI with a dirty draft show a
    conflict (see U-3).
  - **(b) Transient.** A new engine verb (for example `OUTPUT_COMMAND
    set_output {brightness}`) sets a runtime multiplier applied in the
    scene worker before delivery and reported in `STATE`. It is not
    persisted.
  - The plan recommends **(b)**, shared with R-1 (tray brightness) and
    R-4 (idle dimming). If the R-1/R-4 plan already defines that verb,
    reuse it here. Do not add a second one.

**Part B — loopback bridge (M).** A new optional executable,
`openrgb2-bridge`, is a separate process so the elevated UI and engine
gain no network listener.
- It binds `127.0.0.1` only (default port 5584) and is a pipe client
  like `openrgb2-ctl`, reusing `CtlCommands` so the CLI and HTTP verbs
  cannot drift.
- **HTTP.** Each route is a thin mapping onto Part A's `BuildRequest`
  and returns the engine reply as JSON:
  - `GET /v1/state`
  - `GET /v1/looks`
  - `POST /v1/looks/{id}/apply`
  - `POST /v1/brightness {value}`
  - `POST /v1/inputs/{id} {enabled|...}`
- **WebSocket.** `GET /v1/events` relays `SUBSCRIBE` events
  (`frame_status` excluded by default, because of its rate).
- **Security.**
  - A per-launch random token goes in
    `%LOCALAPPDATA%\OpenRGB2\bridge\token`, readable by the current user
    only. Every request needs `Authorization: Bearer <token>`, or the
    `Sec-WebSocket-Protocol` token for WebSocket. This is the QA-server
    pattern.
  - The bridge refuses any `Origin` header that is not absent or
    `null`. This blocks browser CSRF and DNS rebinding.
  - It refuses a `Host` other than `127.0.0.1:<port>` or
    `localhost:<port>`.
  - Bodies are capped at 64 KB.
  - There is no write path to arbitrary files and no `raw` passthrough.
- **Library.** The HTTP/WebSocket server needs a small dependency.
  Daniel decides:
  - Qt `QHttpServer` and `QWebSocketServer`: Qt is already pinned, but
    `QHttpServer` is a separate module that must be confirmed in the
    6.8.3 install.
  - A vendored single-header server (for example `cpp-httplib`), which
    has no WebSocket support.

  Plan default: Qt, because the module set is already managed by
  `stage-release.ps1`.

**Rejected:**
- An HTTP listener inside the engine. It would put a network surface in
  the elevated hardware owner.
- An HTTP listener inside the UI. The UI is elevated in managed mode
  (`src/ui/main.cpp:273-320`), and the QA socket is refused there for
  that reason.
- Reading `scene-library.json` from the CLI to apply legacy profiles.
  That would apply them behind the UI's back, and the file is UI-private.

## Steps

1. Extract `CtlCommands` (pure) with today's verbs, with no behaviour
   change. Test: a new `engine_ctl_cmds_test` (label `engine`) that
   asserts `MsgType` and body for every existing verb.
2. Add `input …`, `look list` and the `playback` alias. Test: extend
   `engine_ctl_cmds_test`, plus one e2e case in `e2e_fake_backend_test`
   that runs `CtlCommands` against the real engine over an in-process
   pipe (`input screen off` flips `reactive`).
3. Add `look apply <id>` with job polling. Test: an e2e case in
   `e2e_studio_workspace_test` with two workspaces, where `active`
   changes and the scene event fires.
4. Brightness, depending on the decision:
   - **(b):** add the engine verb, the multiplier in the worker, and
     `STATE.output.brightness`; then the CLI verb. Tests:
     `ipc_engine_service_test` (verb validation, 0..1 clamp, event) and
     `scene_worker_test` (scaled output).
   - **(a):** CLI-only commit. Test: an e2e case.
5. Update the docs: `docs/ipc-protocol.md` (the new verb, if any) and
   the `--help` text.
6. Bridge skeleton: `tools/bridge/` with the token file, the Host and
   Origin checks, and `GET /v1/state`. Test: `bridge_http_test` (label
   `engine`, loopback only) for a bad token (401), an `Origin:
   http://evil` (403), a wrong Host (403), and a good request (200).
7. Add the remaining routes and the WebSocket event relay. Test: extend
   `bridge_http_test`: apply a look and receive the `workspace` event
   over the WebSocket.
8. Packaging. `stage-release.ps1` includes the bridge but does not start
   it. A `-Bridge` switch on `start-openrgb2.ps1` (opt-in) starts it,
   and `stop-openrgb2.ps1` stops it. Test: a `launcher_tests.ps1` case.

## Tests

- New: `engine_ctl_cmds_test`, `bridge_http_test` (both label `engine`,
  offline).
- Extended: `e2e_fake_backend_test`, `e2e_studio_workspace_test`,
  `ipc_engine_service_test`, `scene_worker_test` (for brightness (b)).
- Manual:
  - A Stream Deck or `curl` against the packaged install.
  - Confirm that a non-elevated `openrgb2-ctl` can open the pipe of the
    elevated engine. The DACL names the user SID, but the integrity
    label on a pipe created by an elevated process is not verified here.

## Risks and open questions

- **A look applied from the CLI while the UI holds a dirty draft.** The
  UI latches a replace or conflict prompt (`WorkspaceController::openWorkspace`,
  `:1097-1121`). This is correct but noisy. Decide whether `look apply`
  should refuse when a UI session holds a dirty draft. The engine cannot
  see that today.
- **Daniel to decide:**
  - Brightness (a) or (b).
  - Qt versus a vendored HTTP library.
  - Whether the bridge should be launchable from Settings (a UI toggle)
    or stay script-only. The plan default is script-only.
