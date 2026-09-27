# OpenRGB2 Engine IPC Protocol — v1

The engine (`openrgb2-engine`) exposes a **versioned, bounded, local-only**
interface to the UI process over Windows named pipes (IMPLEMENTATION_PLAN.md
section 4). This document is the complete wire contract — a Milestone-4 UI
implementer needs nothing else.

- Transport: Windows named pipes, byte mode.
- Two pipes per engine process:

  | Pipe | Purpose |
  | --- | --- |
  | `\\.\pipe\openrgb2-engine` | command channel (full verb catalogue) |
  | `\\.\pipe\openrgb2-engine-diag` | **read-only** diagnostics channel |

- ACL: every pipe instance carries an explicit restricted DACL —
  `D:P(A;;GA;;;<creating-user-SID>)(A;;GA;;;SY)` — GENERIC_ALL to the user
  that started the engine and to LocalSystem only. No Everyone/Users/
  Anonymous/Network access. A client running as another local user gets
  `ERROR_ACCESS_DENIED` from `CreateFile`; there is no privilege negotiation.
  The engine never creates the pipes without this DACL — if it cannot build
  the descriptor it does not listen at all (never default-open).

## 1. Framing

Every message on the stream, both directions:

```
offset  size  field
0       4     magic       = 0x5049324F  (wire bytes "O2IP")
4       2     version     uint16 — wire version of THIS message
6       2     type        uint16 — message type code (§4)
8       4     request_id  uint32 — correlator; server echoes it in replies.
                          0 on HELLO/WELCOME and on EVENT pushes.
12      4     body_len    uint32 — byte count of the JSON body that follows
16      N     body        UTF-8 JSON (parse as {} when body_len == 0)
```

All integers little-endian. `body` is always a JSON object in v1.

Bounds (plan section 6 — everything is bounded):

| Bound | Value |
| --- | --- |
| Max body (`body_len`) | 1 MiB (1 048 576 bytes) |
| Message read deadline | 5 s from first byte of the message |
| HELLO deadline | 5 s from connect |
| Write deadline | 5 s per outbound message |
| Requests per second per connection | 128 (1 s windows) |
| Concurrent sessions per pipe | 8 (independent caps — a full command pipe cannot starve diagnostics) |
| Queued events per session | 64 (per-kind latest-wins, oldest evicted) |
| Resync scan | ≤ 4 MiB skipped per message |
| Per-command dispatch | by contract — see note below |

## 2. Handshake

The first message a client sends MUST be `HELLO` (type `0x0001`,
`request_id = 0`, header `version = 1`):

```json
{ "client_name": "openrgb2-ui", "min_version": 1, "max_version": 1 }
```

- `min_version`/`max_version` are the client's supported range (optional,
  default 1). The server negotiates `negotiated = min(max_version,
  server_version)`; if `negotiated < min_version` it replies
  `ERROR(version_mismatch)` and closes.
- Any other first message → `ERROR(expected_hello)` and close. Malformed
  HELLO → `ERROR(malformed)` and close.
- On success the server replies `WELCOME` (type `0x8001`):

```json
{
  "server_version": 1, "negotiated_version": 1,
  "channel": "command" | "diagnostics",
  "max_body_bytes": 1048576,
  "engine": "openrgb2-engine", "running": true,
  "state": { "... GET_STATE body ..." }
}
```

After the handshake every message's header `version` MUST equal
`negotiated_version`. A mismatched message gets `ERROR(version_mismatch)`;
the connection survives.

## 3. Failure handling

The server rejects garbage without dying and, where it can keep stream sync,
without closing:

| Condition | Result |
| --- | --- |
| Bad/missing magic | resync: server scans forward for the next magic (bounded); the next valid message is still answered. Unrecoverable garbage past the scan bound → connection closed. |
| `body_len` > max | `ERROR(too_large)`; the body is skipped by resync; connection survives. |
| Truncated message / peer close mid-message | session ends (deadline or close); server unaffected. |
| Version ≠ negotiated | `ERROR(version_mismatch)`; connection survives. |
| Unknown type code | `ERROR(unknown_command)`; connection survives. |
| Invalid JSON body / missing fields | `ERROR(malformed)` / `ERROR(bad_request)`; connection survives. |
| Flood (>128 req/s) | `ERROR(rate_limited)` then close. |
| Connection cap | `ERROR(busy)` then close. |
| Any command on the diagnostics pipe | `ERROR(read_only)`; connection survives. |

`ERROR` (type `0x8003`) body:

```json
{ "code": 6, "error": "bad_request", "message": "human detail",
  "...": "optional structured detail (e.g. errors[])" }
```

Stable `code` values: `1` malformed, `2` too_large, `3` version_mismatch,
`4` expected_hello, `5` unknown_command, `6` bad_request, `7` unavailable,
`8` read_only, `9` rate_limited, `10` busy, `11` internal.

Two more robustness guarantees worth knowing:

- **Exceptions can never take the engine down.** Request dispatch,
  the handshake parse, and the initial-event snapshot are each
  wrapped — a handler throwing on malformed client input becomes
  `ERROR(bad_request)` (JSON-shape failures) or `ERROR(internal)`
  (anything else) and the connection survives.
- **Per-command bound is contractual, not enforced.** `Dispatch`
  runs inline on the session's reader thread and there is no
  handler deadline — a thread cannot be safely cancelled
  mid-call. Handlers are required to be non-blocking
  (in-memory engine state only; `src/ipc/IpcService.h`). If a
  future handler ever blocks, the blast radius is its own
  session (one of ≤8 slots): other sessions, the event writer,
  and the engine are unaffected.

## 4. Message catalogue (command channel)

Request → reply. `request_id` echoes.

| Code | Name | Request body | Reply |
| --- | --- | --- | --- |
| 0x0001 | HELLO | `{client_name?, min_version?, max_version?}` | WELCOME / ERROR+close |
| 0x0100 | GET_SNAPSHOTS | `{}` | SNAPSHOTS |
| 0x0101 | GET_SCENE | `{}` | SCENE |
| 0x0102 | SET_SCENE | `{document:<SceneDocument>}` | OK `{revision,name}` |
| 0x0103 | GET_PLAYBACK | `{}` | PLAYBACK |
| 0x0104 | SET_PLAYBACK_MODE | `{mode, control?, params?}` | OK `{requested_mode,applied,pending,revision?}` / ERROR |
| 0x0105 | GET_DELIVERY_STATS | `{}` or `{controller:{device,serial,location}}` | DELIVERY_STATS |
| 0x0106 | GET_STATE | `{}` | STATE |
| 0x0107 | LAYOUT_COMMAND | `{verb, ...args}` | OK / ERROR |
| 0x0108 | SUBSCRIBE | `{events:"all"}` or `{events:[kind,...]}` | OK `{subscribed:[...]}` + initial EVENT per kind |
| 0x0109 | UNSUBSCRIBE | `{}` | OK `{subscribed:[]}` |
| 0x010A | PING | `{}` | PONG `{}` |
| 0x010B | GET_PRESETS | `{}` | PRESETS |
| 0x010C | QUIT | `{}` | OK `{shutting_down:true, ordered:true}` / ERROR |
| 0x0300 | DIAG_GET | `{}` | DIAGNOSTICS (valid on both pipes) |

Reply types: WELCOME 0x8001, OK 0x8002, ERROR 0x8003, SNAPSHOTS 0x8100,
SCENE 0x8101, PLAYBACK 0x8102, DELIVERY_STATS 0x8103, STATE 0x8104,
PONG 0x8105, PRESETS 0x8106, EVENT 0x8200, DIAGNOSTICS 0x8300.

## 5. Body schemas

`DeviceKey` objects: `{device, serial, location, zone}` (zone is `""` for a
whole controller). Colors are `"#RRGGBB"` strings.

**SNAPSHOTS** — one entry per enumerated controller; `connection` mirrors
the SDK link:

```json
{ "snapshots": [ {
    "key": {...}, "topology_generation": 3, "name": "...", "vendor": "...",
    "device_type": 2, "zones": [{"name":"...","led_count":40}],
    "modes": ["Direct","Spectrum Cycle",...], "active_mode": 0,
    "connection": "connected", "led_count": 120,
    "colors": ["#RRGGBB", ...] } ],
  "connection": {"sdk_present":true,"state":"connected",
                 "protocol_version":4,"reconnect_attempt":0,"last_error":""} }
```

**SCENE** — `{loaded, revision, applied_revision, playing, name, document}`
where `document` is the core SceneDocument JSON (same schema `--scene`
accepts) or `null`. SET_SCENE applies the same path as `--scene` (inline
`effect.layers`, else the named `effect.preset` through the registry —
unknown presets → `bad_request`), takes effect on the next 60 Hz tick, and
emits a `scene` event.

**PLAYBACK** —

```json
{ "requested_mode": "live_effects"|"animation_playback",
  "requested_revision": 0,
  "pending": false,
  "unavailable_reason": null,
  "detail": null,
  "capabilities": {"live_effects": true, "animation_playback": false},
  "status": { "mode": ..., "preset_revision": 0, "parameter_revision": 0,
    "upload": "none|uploading|active|failed",
    "control": "playing|paused|stopped",
    "effective_frame_count": 0, "effective_frame_interval_s": 0,
    "clock_sample_s": 0, "clock_uncertainty_s": 0,
    "sync": "unknown|healthy|degraded|stale",
    "physical": {"frame_interval_s":0,"cycle_s":0,"provenance":"..."} } }
```

`requested_mode` is the recorded UI selection (persisted per scene);
`status.mode` is the APPLIED mode and only moves on backend
acknowledgement — UI shows pending until they agree (plan section 4).
`pending` is true while a transition or backend acknowledgement is in
flight; `requested_revision` is the request generation the client can
poll against `status.parameter_revision`. `unavailable_reason` carries
the scene-capability reason animation is unavailable for the loaded
scene; `detail` carries the last transition/ack/control failure.
Unsupported modes → `unavailable` with reason; unknown names →
`bad_request`.

SET_PLAYBACK_MODE accepts `mode` plus optional `control`
(`"playing"` default | `"paused"` sampled-hold | `"stopped"` black) and
optional `params` `{speed:int, reverse:bool, color:"#RRGGBB"}` applied
to the uploaded preset (speed 10..200, direction for Rainbow Wave,
color for Breathing). Each params field overlays the engine's recorded
per-scene selection — an omitted field (or an omitted `params` object)
PRESERVES the recorded value rather than resetting it to defaults: the
wire reports parameter revisions, not values, so clients send only the
fields they are changing. Animation playback is limited to the three
uploaded presets — `Spectrum Cycle`, `Rainbow Wave`, `Breathing` —
which a scene selects by naming one of them in `effect.preset` (SDK
names and `snake_case` ids both accepted). While an uploaded loop owns
the Lian Li wireless group, live frames to that group are rejected by
the delivery lane (counted as suppressed, never silently dropped);
wired controllers keep receiving matching baked frames sampled from
the same preset/timeline. Reply `applied:false/pending:true` means the
mode change was accepted and serialized; `status.mode` follows when the
backend reports it.

**DELIVERY_STATS** — `{available, aggregate, pending, controllers:[{key,
stats, pending, colors, budget_ms}]}`; per-controller form adds
`{controller, found, stats}`. `stats` =
`{submitted,replaced_pending,rejected,transport_completed,suppressed,
transport_rejected,last_interval_s,min_interval_s,max_interval_s,
mean_interval_s,interval_count,provenance}`.
`transport_completed` means *accepted by the transport* — never
"displayed" (plan section 4).

**STATE** — `{engine:{running,scene_clock_hz,scene_sequence}, scene:{...},
sdk:{...}, playback:{...}, power:{...}, ipc:{sessions_active, messages_in,
responses_out, events_sent, events_dropped, queued_events, resync_bytes,
rejected_hellos, rate_limited, refused_busy}}`.

**QUIT** — the "Quit All" hook (plan section 4): requests the engine's
ordered, bounded shutdown (the power monitor, IPC intake, scene worker,
delivery lanes, and SDK transport stop in order; the reply is prepared
before the hook fires). `unavailable` when the process has no shutdown
hook wired. Owning the backend's process is the caller's concern — the
engine never spawns it.

**POWER** (STATE/DIAGNOSTICS field, `null` when unwired) —
`{display_on, suspended, output_paused, power_hold, restore_pending,
detail}`: the Task 3.7 lifecycle state — display-off/suspend gating,
the one-time loop freeze (`power_hold`), and wake-restore progress.

**PRESETS** — `{presets:[{id,name,description,needs}]}` — Lighting-view
preset cards.

**LAYOUT_COMMAND** — the Layout view's verb space (plan section 5).
Every request carries `{verb:"<name>", ...args}`; unknown verbs →
`bad_request`. Six verbs exist (Task 4.4):

| verb | body | reply (OK) |
| --- | --- | --- |
| `identify` | `{controller:<DeviceKey>, zone?:"name", led_index?:int, duration_ms?:int}` | `{identifying:true, controller, zone, led_start, led_count, duration_ms}` |
| `set_position` | `{object_id, position:{x,y,z}}` | `{object_id, position, revision}` |
| `set_orientation` | `{object_id, rotation:{x,y,z}}` | `{object_id, rotation, revision}` |
| `set_mirror` | `{object_id, mirror_of:"id"|""}` | `{object_id, mirror_of, revision}` |
| `calibrate_led` | `{object_id, emitter_index:int, address:int}` | `{object_id, emitter_index, address, revision}` |
| `set_binding` | `{object_id, binding:"id"|""[, controller:<DeviceKey>]}` | `{object_id, binding, revision}` |

`identify` is a **transient** operation — it never touches the scene
document. The engine runs a bounded blink overlay in the frame path
(device mapper): the identified LED range alternates lit/dark on a
250 ms half-period over a dark field, through the normal delivery
lane (budgets and suppression still apply). `zone` names a zone (or
`controller.zone` carries it); `led_index` selects one LED inside the
scope; `duration_ms` defaults to 3000 and is clamped to 500–30000;
`duration_ms:0` cancels → `{identifying:false, cancelled, controller}`.
Errors: absent/disconnected controller → `unavailable`; bad zone or
out-of-range index → `bad_request`; a wireless group owned by an
uploaded animation loop → `unavailable` (one producer per group).

The other five verbs are **scene mutations**: they edit the applied
SceneDocument through the same apply path as SET_SCENE (LED-address
validation, effect resolution, revision bump, `scene` event). No
loaded scene → `unavailable`. Fields:

- `set_position`/`set_orientation` require the full `{x,y,z}` vector
  (meters / euler XYZ degrees — the document's only rotation
  convention); partial or non-finite vectors → `bad_request`.
- `set_mirror` follows the resolver's own semantics: linking makes the
  object a render-only `linked` copy sharing the target's output —
  its binding/emitters/verified are cleared (unlink does NOT restore
  them; UI undo reverts the whole document). Targets must be device
  objects; self-mirror and chains → `bad_request`. Mirrored fans
  share the source zone — addressable LED counts never multiply.
- `calibrate_led` sets an emitter's `address` — an index inside the
  object's bound zone; `-1` marks it render-only. The bound is the
  binding's `zone_leds`, else the resolved zone's enumerated count;
  no bound → `bad_request`. Out-of-range → `bad_request` (the UI
  range-checks first; the engine is the last boundary).
- `set_binding` repoints `object.binding` (`""` unbinds). A NEW id
  requires `controller:<DeviceKey>` and derives the binding's
  identity (name/vendor/serial/location/type/zone/count) from the
  live snapshot — absent topology → `unavailable`; clients cannot
  fabricate controller identities.

`set_position` was added in Task 4.4 alongside the reserved five —
the Layout view's arrangement editing needed an explicit move op.

**DIAGNOSTICS** — the read-only aggregate view:

```json
{ "engine":   {"running":true,"scene_clock_hz":60,"scene_sequence":N,"uptime_s":F},
  "scene":    {"loaded":..., "revision":N, "applied_revision":N, "playing":B, ...},
  "playback": {"requested_mode":..., "capabilities":..., "status":{...}},
  "delivery": {"available":..., "aggregate":{...}, "pending":N, "controllers":[...]},
  "sdk":      {"state":..., "protocol_version":N, "reconnect_attempt":N, ...},
  "power":    {"display_on":B, "suspended":B, "output_paused":B, ...},
  "ipc":      {"sessions_active":N, "queued_events":N, "events_dropped":N, ...},
  "clock":    {"steady_ms":F},
  "backend_counters": null }
```

`backend_counters` is `null` until the backend's own read-only counters
pipe lands (a separate plan item — engine-side aggregate view only).

## 6. Diagnostics channel (read-only by construction)

`\\.\pipe\openrgb2-engine-diag` speaks the SAME framing and HELLO/WELCOME
handshake (WELCOME reports `"channel":"diagnostics"`). After the handshake
the only accepted verb is `DIAG_GET` — the dispatch table for this channel
literally has one entry. Every other message type — including PING,
SUBSCRIBE, SET_SCENE, and any code a future command channel adds — gets
`ERROR(read_only)`. There is no code path on this pipe that can mutate
engine or hardware state: it is not a second command surface, not even an
undocumented one (plan section 4).

## 7. Events (SUBSCRIBE push channel)

`SUBSCRIBE {events:"all"}` or `{events:[kind,...]}` registers per-session
subscriptions (bounded queue, see §1). Replies `OK {subscribed:[...]}` then
immediately pushes one EVENT per subscribed kind carrying current state —
a subscriber never needs a second request to paint.

`EVENT` (type `0x8200`, `request_id = 0`): `{kind, data}` where `data`
matches the corresponding pull body:

| kind | data | emitted |
| --- | --- | --- |
| `topology` | SNAPSHOTS body | snapshot list / generation / connection change |
| `scene` | SCENE body | scene applied (SET_SCENE or --scene) |
| `playback` | PLAYBACK body | requested-mode or applied-status change |
| `connection` | `sdk` object | SDK link state change |
| `frame_status` | `{aggregate, controllers:[{key,sequence,colors,stats}]}` | ≤ 1 Hz, only when delivery counters moved; `colors` are the last transport-accepted frame per controller — the same sampled colors output uses |

An `events` field that is neither `"all"` nor an array of strings, or any
unknown kind name → `bad_request` (subscription unchanged). Queue overflow
drops the oldest queued event (counted in `ipc.events_dropped`); a queued
event of a kind is replaced by a newer one rather than appended.

## 8. Notes for implementers

- Replies always pair the request's `request_id`; EVENTs use `0`.
- `unavailable` is honest absence (no SDK configured, no pipeline, feature
  not yet implemented) — never a fabricated success.
- Unknown JSON fields in requests are ignored; unknown fields in replies
  must be tolerated by clients (forward compatibility).
- Closing the UI does not affect the engine; reconnecting to the same
  pipes resumes state via WELCOME + SUBSCRIBE initial pushes (plan section
  6, "UI independence").
