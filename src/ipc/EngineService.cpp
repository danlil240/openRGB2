/*---------------------------------------------------------*\
|| EngineService.cpp                                         ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "EngineService.h"

#include "ProfileStore.h"      /* ValidateSceneLedAddresses */

#include "scene/SceneGraph.h"  /* ValidateSceneGraph         */
#include "scene/SceneJson.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <sstream>

namespace openrgb2
{
namespace ipc
{

namespace
{

/*---------------------------------------------------------*\
|| Stable wire names (docs/ipc-protocol.md)                 ||
\*---------------------------------------------------------*/

const char* ConnStateName(ConnectionState s)
{
    switch(s)
    {
    case ConnectionState::Disconnected: return "disconnected";
    case ConnectionState::Reconnecting: return "reconnecting";
    case ConnectionState::Connected:    return "connected";
    }
    return "disconnected";
}

const char* UploadStateName(UploadState s)
{
    switch(s)
    {
    case UploadState::None:      return "none";
    case UploadState::Uploading: return "uploading";
    case UploadState::Active:    return "active";
    case UploadState::Failed:    return "failed";
    }
    return "none";
}

const char* SyncHealthName(SyncHealth s)
{
    switch(s)
    {
    case SyncHealth::Unknown:  return "unknown";
    case SyncHealth::Healthy:  return "healthy";
    case SyncHealth::Degraded: return "degraded";
    case SyncHealth::Stale:    return "stale";
    }
    return "unknown";
}

const char* ProvenanceName(MeasurementProvenance p)
{
    switch(p)
    {
    case MeasurementProvenance::Unknown:           return "unknown";
    case MeasurementProvenance::Configured:        return "configured";
    case MeasurementProvenance::TransportMeasured: return "transport_measured";
    case MeasurementProvenance::PhysicallyMeasured:return "physically_measured";
    }
    return "unknown";
}

/*---------------------------------------------------------*\
|| Serializers                                               ||
\*---------------------------------------------------------*/

nlohmann::json KeyToJson(const DeviceKey& k)
{
    return nlohmann::json::object({
        { "device",   k.device   },
        { "serial",   k.serial   },
        { "location", k.location },
        { "zone",     k.zone     },
    });
}

DeviceKey KeyFromJson(const nlohmann::json& j)
{
    DeviceKey k;
    if(j.is_object())
    {
        /* Client input: wrong-typed fields become empty — the
           caller validates the result. nlohmann::value() would
           throw type_error straight through the session
           thread (std::terminate = the whole engine). */
        const auto field = [&j](const char* key) -> std::string
        {
            const auto it = j.find(key);
            return (it != j.end() && it->is_string())
                       ? it->get<std::string>()
                       : std::string();
        };
        k.device   = field("device");
        k.serial   = field("serial");
        k.location = field("location");
        k.zone     = field("zone");
    }
    return k;
}

std::string ColorHex(RgbColor c)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", c.r, c.g, c.b);
    return buf;
}

bool ParseHexColor(const std::string& s, RgbColor& out)
{
    if(s.size() != 7 || s[0] != '#')
    {
        return false;
    }
    const auto nib = [](char ch) -> int
    {
        if(ch >= '0' && ch <= '9') return ch - '0';
        if(ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if(ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    int v[6];
    for(int i = 0; i < 6; ++i)
    {
        v[i] = nib(s[i + 1]);
        if(v[i] < 0)
        {
            return false;
        }
    }
    out = RgbColor{ (uint8_t)(v[0] * 16 + v[1]),
                    (uint8_t)(v[2] * 16 + v[3]),
                    (uint8_t)(v[4] * 16 + v[5]) };
    return true;
}

nlohmann::json ColorsToJson(const std::vector<RgbColor>& colors)
{
    nlohmann::json out = nlohmann::json::array();
    for(const RgbColor& c : colors)
    {
        out.push_back(ColorHex(c));
    }
    return out;
}

nlohmann::json StatsToJson(const DeliveryStats& s)
{
    return nlohmann::json::object({
        { "submitted",           s.submitted           },
        { "replaced_pending",    s.replaced_pending    },
        { "rejected",            s.rejected            },
        { "transport_completed", s.transport_completed },
        { "suppressed",          s.suppressed          },
        { "transport_rejected",  s.transport_rejected  },
        { "last_interval_s",     s.last_interval_s     },
        { "min_interval_s",      s.min_interval_s      },
        { "max_interval_s",      s.max_interval_s      },
        { "mean_interval_s",     s.MeanIntervalS()     },
        { "interval_count",      s.IntervalCount()     },
        { "provenance",          ProvenanceName(s.provenance) },
    });
}

/* QA F04: the mapper's per-pass counters — object
   bound/writable/emitted plus frames produced/dropped — the
   surface that makes "bound but never delivered" visible. */
nlohmann::json MapStatsToJson(const MapStats& m)
{
    return nlohmann::json::object({
        { "objects_total",      m.objects_total    },
        { "objects_bound",      m.objects_bound    },
        { "objects_writable",   m.objects_writable },
        { "objects_emitted",    m.objects_emitted  },
        { "skipped_unverified_or_unresolved",
                                m.skipped_unverified_or_unresolved },
        { "frames_emitted",     m.frames_emitted   },
        { "frames_dropped",     m.frames_dropped   },
    });
}

nlohmann::json SnapshotToJson(const DeviceSnapshot& snap,
                              bool include_colors)
{
    nlohmann::json zones = nlohmann::json::array();
    for(const ZoneSnapshot& z : snap.zones)
    {
        zones.push_back(nlohmann::json::object({
            { "name",      z.name      },
            { "led_count", z.led_count },
            { "leds_min",  z.leds_min  },
            { "leds_max",  z.leds_max  },
        }));
    }
    nlohmann::json out = nlohmann::json::object({
        { "key",                 KeyToJson(snap.key)          },
        { "topology_generation", snap.topology_generation      },
        { "name",                snap.name                    },
        { "vendor",              snap.vendor                  },
        { "device_type",         snap.device_type             },
        { "zones",               std::move(zones)             },
        { "modes",               snap.modes                   },
        { "active_mode",         snap.active_mode             },
        { "connection",          ConnStateName(snap.connection)},
        { "led_count",           snap.LedCount()              },
    });
    if(include_colors)
    {
        out["colors"] = ColorsToJson(snap.colors);
    }
    return out;
}

nlohmann::json PlaybackStatusToJson(const PlaybackStatus& st)
{
    return nlohmann::json::object({
        { "mode",              PlaybackModeName(st.mode)     },
        { "preset_revision",   st.preset_revision            },
        { "parameter_revision",st.parameter_revision         },
        { "upload",            UploadStateName(st.upload)    },
        { "control",           PlaybackControlName(st.control)},
        { "effective_frame_count",
                               st.effective_frame_count      },
        { "effective_frame_interval_s",
                               st.effective_frame_interval_s },
        { "clock_sample_s",    st.clock_sample_s             },
        { "clock_uncertainty_s", st.clock_uncertainty_s      },
        { "sync",              SyncHealthName(st.sync)       },
        { "physical", nlohmann::json::object({
            { "frame_interval_s", st.physical.frame_interval_s },
            { "cycle_s",          st.physical.cycle_s          },
            { "provenance",       ProvenanceName(st.physical.provenance) },
        })},
    });
}

/* Reactive-input verbs (Task 4.6 — plan section 5, the
   Lighting view's reactive-controls card). Unknown verbs
   are BadRequest. */
bool IsKnownReactiveVerb(const std::string& verb)
{
    return verb == "set_enabled" || verb == "set_params";
}

/* Layout/calibration verbs (Task 4.4 — plan section 5,
   Layout view; bodies in docs/ipc-protocol.md §5). Unknown
   verbs are BadRequest. */
bool IsKnownLayoutVerb(const std::string& verb)
{
    static const char* kVerbs[] = {
        "identify",          /* blink a zone for LED identification */
        "set_position",      /* move an object on the arrangement   */
        "set_orientation",   /* rotate an object's emitter mapping  */
        "set_mirror",        /* link/mirror objects (linked fans)   */
        "calibrate_led",     /* LED address calibration             */
        "set_binding",       /* re-bind a scene binding             */
        "add_object",        /* create device objects from topology */
    };
    for(const char* v : kVerbs)
    {
        if(verb == v)
        {
            return true;
        }
    }
    return false;
}

} /* anonymous namespace */

/*---------------------------------------------------------*\
|| EngineService                                             ||
\*---------------------------------------------------------*/

EngineService::EngineService(Engine& engine)
    : EngineService(engine, Deps{}, EngineServiceOptions{})
{
}

EngineService::EngineService(Engine& engine, Deps deps,
                             EngineServiceOptions options)
    : engine_(engine), deps_(std::move(deps)), options_(options),
      started_(Clock::now())
{
    /* The controller classifies against the APPLIED scene —
       the service owns the document, so it provides it. */
    if(deps_.playback)
    {
        deps_.playback->SetSceneProvider(
            [this]() { return Scene(); });
    }

    /* Task 4.4 review fix: `identify` exists to find hardware
       BEFORE a scene is bound, so the blink must reach the
       transports with NO scene loaded — but the worker's
       no-scene ticks sleep unless an idle hook asks for the
       frame path. The mapper's overlay ledger is that hook:
       while any overlay is booked (expired-but-unpruned
       included — the prune+restore frame needs its tick) the
       worker keeps producing frames and MapFrame's overlay
       pass does the rest. The overlay's own expiry bounds the
       ticking; with no mapper wired the hook stays unset. */
    if(deps_.mapper)
    {
        DeviceMapper* m = deps_.mapper;
        engine_.Worker().SetIdleFrameHook(
            [m] { return m->IdentifyActive(); });
    }

    /* studio-import Task 3: the managed-workspace verb space.
       The command layer exists only while the dep is wired —
       WELCOME advertises "studio_workspace_v1" exactly when
       this is non-null. */
    if(deps_.workspace)
    {
        ws_commands_ = std::make_unique<WorkspaceCommands>(
            deps_.workspace, options_.transfers);
        /* Completion events are emitted OFF the workspace job
           mutex (the worker calls this after the record is
           stored) — a slow subscriber never stalls workspace
           work, and the bounded event queue still applies. */
        deps_.workspace->SetJobEventHook(
            [this](const WorkspaceService::JobStatus& st)
            {
                Emit(event_kind::kWorkspace,
                     JobStatusToJson(st));
            });
        /* The published snapshot rebroadcasts on every change —
           without it a session whose subscribe-time snapshot
           raced the boot re-adoption latches managed:false, and
           every save drives into the workspace_required refusal
           on the legacy scene path. */
        deps_.workspace->SetSnapshotEventHook(
            [this]
            {
                if(ws_commands_)
                {
                    Emit(event_kind::kWorkspace,
                         ws_commands_->SnapshotJson());
                }
            });

        /* studio-import Task 7: the session-bound draft
           preview. Topology reads share the layout seam's
           provider; input is a Snapshot() consume — never a
           source start; the scene epoch is the worker's own
           clock so drafts animate on the output timeline.
           Delivery is targeted through SetSessionEventSink —
           wire it BEFORE preview_begin or frames go nowhere
           (the verbs then answer Unavailable honestly via the
           empty deps.emit check). */
        WorkspaceCommands::PreviewDeps pd;
        pd.snapshots  = deps_.snapshots;
        pd.power      = deps_.power;
        pd.reactive   = deps_.reactive;
        pd.scene_epoch =
            [this]() { return engine_.Worker().Epoch(); };
        pd.emit =
            [this](uint64_t sid, const IpcEvent& ev,
                   const std::string& key)
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if(session_sink_)
                {
                    session_sink_(sid, ev, key);
                }
            };
        pd.max_sessions        = options_.preview.max_sessions;
        pd.min_interval_ms     = options_.preview.min_interval_ms;
        pd.chunk_bytes         = options_.preview.chunk_bytes;
        pd.display_budget_leds =
            options_.preview.display_budget_leds;
        ws_commands_->SetPreviewDeps(std::move(pd));

        /* studio-import Task 11: the physical calibration
           session — the calibration_* verbs bind client
           session + opaque token to the engine-owned
           CalibrationSession. Its status pushes share the
           session-targeted emitter the preview feed uses —
           the owning client sees state flips; nobody else
           sees the token. */
        WorkspaceCommands::CalibrationDeps cd;
        cd.session = deps_.calibration;
        cd.emit =
            [this](uint64_t sid, const IpcEvent& ev,
                   const std::string& key)
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if(session_sink_)
                {
                    session_sink_(sid, ev, key);
                }
            };
        ws_commands_->SetCalibrationDeps(std::move(cd));
    }
}

EngineService::~EngineService()
{
    /* The event hook captures `this` — clear it before the
       workspace service outlives us into its own teardown. */
    if(deps_.workspace)
    {
        deps_.workspace->SetJobEventHook(nullptr);
        deps_.workspace->SetSnapshotEventHook(nullptr);
    }
}

void EngineService::SetEventSink(
    std::function<void(const IpcEvent&)> sink)
{
    std::lock_guard<std::mutex> lock(mutex_);
    event_sink_ = std::move(sink);
}

void EngineService::SetSessionEventSink(
    std::function<void(uint64_t, const IpcEvent&,
                       const std::string&)> sink)
{
    std::lock_guard<std::mutex> lock(mutex_);
    session_sink_ = std::move(sink);
}

void EngineService::SetServerStatsProvider(
    std::function<IpcServerStats()> fn)
{
    std::lock_guard<std::mutex> lock(mutex_);
    deps_.server_stats = std::move(fn);
}

void EngineService::Emit(const std::string& kind,
                         const nlohmann::json& data)
{
    std::function<void(const IpcEvent&)> sink;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sink = event_sink_;
    }
    if(sink)
    {
        sink(IpcEvent{ kind, data });
    }
}

/*---------------------------------------------------------*\
|| Scene ownership                                           ||
\*---------------------------------------------------------*/

bool EngineService::ApplySceneDocument(
    std::shared_ptr<const studio::SceneDocument> doc,
    std::string& err)
{
    if(!doc)
    {
        err = "null scene document";
        return false;
    }

    /* Task 3.8 offline bindings: the persisted identity table
       completes name-only bindings BEFORE anything else reads
       the document — the hook may hand back an augmented copy. */
    if(deps_.prepare_scene)
    {
        deps_.prepare_scene(doc);
        if(!doc)
        {
            err = "scene preparation returned no document";
            return false;
        }
    }

    /* Graph + LED-address validation run inside
       ApplyPreparedScene — on the PREPARED document, so
       matrix-expanded emitter addresses are checked against
       the persisted zone size as well (Task 6). */

    /* Effect ids resolve against the EFFECTIVE registry — the
       packaged look library plus the personal looks landed
       under the managed root's library/effects by
       resource_install. That library is root-level file state,
       not open-workspace state: it is readable here even before
       the startup workspace re-adoption runs, so a last-scene
       snapshot referencing a user-saved look restores instead
       of degrading to "unknown preset". */
    const studio::EffectRegistry effreg =
        deps_.workspace
            ? deps_.workspace->Store().EffectiveEffectRegistry()
            : studio::EffectLooks();

    /* The persisted preset name is validated HERE (the IPC/CLI
       boundary): a playback-preset name (Spectrum Cycle /
       Rainbow Wave / Breathing) is also valid — it has no
       registry layer stack; live fallback renders the
       document's base colors while Animation playback drives
       the real preset (Task 3.6). The actual layer build runs
       inside PrepareStudioScene below. */
    if(doc->effect.layers.empty() && !doc->effect.preset.empty() &&
       !effreg.Contains(doc->effect.preset) &&
       !studio::FindPreset(doc->effect.preset) &&
       !IsPlaybackPresetName(doc->effect.preset))
    {
        err = "unknown preset '" + doc->effect.preset + "'";
        return false;
    }

    /* SET_SCENE / --scene expanded documents carry no workspace
       inputs — synthesize the minimal candidate preparation
       needs: the effective effect registry resolves named
       presets AND personal looks (same contract as before, plus
       the managed library layer) and decay defaults to 100 (a
       1.0 multiplier). Authored fields stay untouched. */
    WorkspaceCandidate cand;
    cand.resolved         = doc;
    cand.effect_registry  = effreg;
    cand.document.inputs.decay_pct = 100;
    return ApplyWorkspaceCandidate(cand, err);
}

bool EngineService::ApplyWorkspaceCandidate(const WorkspaceCandidate& cand,
                                            std::string& err)
{
    /* Task 6 shared preparation: matrix emitters + key
       positions expand against the LIVE snapshots; runtime
       layers resolve through the candidate's own registry
       snapshot and take the persisted decay scaling on a
       copy — authored workspace data is never mutated. */
    PreparedStudioScene prepared;
    if(!PrepareStudioScene(cand, LayoutSnapshots(), prepared, err))
    {
        return false;
    }
    return ApplyPreparedScene(prepared, err);
}

bool EngineService::ApplyPreparedScene(const PreparedStudioScene& prepared,
                                       std::string& err)
{
    const std::shared_ptr<const studio::SceneDocument>& doc =
        prepared.doc;
    if(!doc)
    {
        err = "null scene document";
        return false;
    }

    /* Structural rules the SceneJson parser enforces are
       re-checked HERE because the layout mutation verbs edit
       the applied document directly — they never re-enter
       FromJson. A mutation that strands mirror_of on a
       Linked object (a mirror chain), a parent cycle, or a
       duplicate id can never reach the live document. SET_
       SCENE documents already passed this exact check inside
       FromJson — re-running it is free, not a policy change.
       (Task 4.4 review: set_mirror refuses the chain at the
       verb too; this is the boundary that also covers
       undo-reverts and any future mutating verb.) */
    {
        std::vector<std::string> errs;
        if(!studio::ValidateSceneGraph(*doc, &errs))
        {
            err = errs.empty()
                      ? "scene document failed graph validation"
                      : errs.front();
            if(errs.size() > 1)
            {
                err += " (+" + std::to_string(errs.size() - 1) +
                       " more)";
            }
            return false;
        }
    }

    /* Task 3.8 persistence-boundary regression (calibration
       report, plan section 5): expanded documents bypass
       workspace resolution, so out-of-range LED addresses are
       rejected HERE — the last boundary before the scene is
       applied and stored. Task 6: the check runs on the
       PREPARED document, so matrix-expanded emitter addresses
       are validated against the persisted zone size too. */
    {
        std::vector<std::string> errs;
        if(!ValidateSceneLedAddresses(*doc, &errs))
        {
            err = errs.front();
            if(errs.size() > 1)
            {
                err += " (+" + std::to_string(errs.size() - 1) +
                       " more)";
            }
            return false;
        }
    }

    uint64_t revision;
    {
        /* The whole apply is serialized: revision, worker
           submission, mapper re-bind, and the stored document
           move together — two concurrent SET_SCENEs can't
           interleave a stale document over a newer revision.
           The worker receives the PREPARED scene atomically:
           document, runtime layers, and the key-position
           lookup never tear across revisions. */
        std::lock_guard<std::mutex> lock(mutex_);
        revision = ++scene_revision_;
        engine_.Worker().SubmitPreparedScene(prepared, revision);
        if(deps_.mapper)
        {
            deps_.mapper->SetScene(doc);
        }
        scene_ = doc;
    }
    /* The persisted per-scene playback selection is restored
       against the NEW document's capabilities (Task 3.6). */
    if(deps_.playback)
    {
        /* QA F03: the Result is never dropped. A rejected
           re-apply — the persisted preset lost its capability
           under the new document, the backend refused the
           mode, a transition is in flight — leaves the OLD
           effect playing on hardware while the scene commit
           itself legitimately succeeded (the apply is NOT
           failed; the retained effect is the honest state).
           The controller already records the reason in
           Detail() — PLAYBACK's "detail" field, persistent
           until a later transition succeeds — so emit the
           playback event now, the same emission
           SET_PLAYBACK_MODE performs after every transition,
           instead of leaving subscribers to notice on the
           next diff poll. */
        const PlaybackController::Result pr =
            deps_.playback->OnSceneApplied(doc);
        if(!pr.accepted)
        {
            Emit(event_kind::kPlayback, PlaybackJson());
        }
    }
    /* The event carries the full SCENE body — the documented
       contract is that event data matches the corresponding
       pull body (docs/ipc-protocol.md §7). */
    Emit(event_kind::kScene, SceneJsonLocked());
    /* The last-scene snapshot: every successful apply that
       carries objects persists the document — SET_SCENE,
       layout mutations, and the startup --scene/restore path
       all funnel through here. An object-less document is a
       deliberate "new scene" staging step, not the scene to
       restore on next launch — it never overwrites the
       snapshot. */
    if(deps_.persist_scene && !doc->objects.empty())
    {
        deps_.persist_scene(*doc);
    }
    return true;
}

uint64_t EngineService::SceneRevision() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return scene_revision_;
}

std::shared_ptr<const studio::SceneDocument> EngineService::Scene() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return scene_;
}

/*---------------------------------------------------------*\
|| Payload builders                                          ||
\*---------------------------------------------------------*/

nlohmann::json EngineService::SceneJsonLocked() const
{
    nlohmann::json out;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        out["loaded"]   = scene_ != nullptr;
        out["revision"] = scene_revision_;
        if(scene_)
        {
            out["name"]     = scene_->name;
            out["document"] = studio::ToJson(*scene_);
        }
        else
        {
            out["document"] = nullptr;
        }
    }
    out["applied_revision"] = engine_.Worker().AppliedRevision();
    out["playing"]          = engine_.Worker().HasScene();
    /* studio-import Task 3: SCENE replies tell old clients the
       managed workspace state — the gate rule lives here, not
       in guesswork. */
    out["workspace"] = nlohmann::json::object(
        {{ "managed", WorkspaceManaged() }});
    return out;
}

nlohmann::json EngineService::PlaybackJson() const
{
    PlaybackCapabilities caps;
    PlaybackMode         requested;
    PlaybackStatus       status;
    uint64_t             requested_rev = 0;
    bool                 pending       = false;
    std::string          unavailable_reason;
    std::string          status_detail;
    if(deps_.playback)
    {
        /* The controller is authoritative: applied status,
           pending ack, and the scene-classified capability. */
        caps      = deps_.playback->Caps();
        requested = deps_.playback->RequestedMode();
        status    = deps_.playback->Status();
        requested_rev = deps_.playback->RequestedRevision();
        pending       = deps_.playback->AckPending();
        unavailable_reason = deps_.playback->SceneCaps().reason;
        status_detail = deps_.playback->Detail();
    }
    else
    {
        caps.live_effects       = true;
        caps.animation_playback = false;
        if(deps_.playback_caps)
        {
            caps = deps_.playback_caps();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        requested = requested_mode_;
        status    = engine_.Status();
    }
    return nlohmann::json::object({
        { "requested_mode",     PlaybackModeName(requested) },
        { "requested_revision", requested_rev             },
        { "pending",            pending                   },
        { "unavailable_reason",
          unavailable_reason.empty()
              ? nlohmann::json(nullptr)
              : nlohmann::json(unavailable_reason)        },
        { "detail",
          status_detail.empty()
              ? nlohmann::json(nullptr)
              : nlohmann::json(status_detail)             },
        { "capabilities", nlohmann::json::object({
            { "live_effects",       caps.live_effects       },
            { "animation_playback", caps.animation_playback },
        })},
        { "status", PlaybackStatusToJson(status) },
    });
}

nlohmann::json EngineService::ConnectionJson() const
{
    nlohmann::json out = nlohmann::json::object({
        { "sdk_present", deps_.sdk != nullptr },
    });
    if(deps_.sdk)
    {
        /* QA F12: ONE state-lock acquisition for all four
           fields — four separate getters could each queue
           behind wire I/O. */
        const sdk::ConnectionSnapshotInfo cs =
            deps_.sdk->ConnectionSnapshot();
        out["state"]             = ConnStateName(cs.state);
        out["protocol_version"]  = cs.protocol_version;
        out["reconnect_attempt"] = cs.reconnect_attempt;
        out["last_error"]        = cs.last_error;
    }
    else
    {
        out["state"]             = "disconnected";
        out["protocol_version"]  = 0;
        out["reconnect_attempt"] = 0;
        out["last_error"]        = "no SDK transport configured";
    }
    return out;
}

nlohmann::json EngineService::SnapshotsJson(bool include_colors) const
{
    nlohmann::json snaps = nlohmann::json::array();
    if(deps_.sdk)
    {
        for(const DeviceSnapshot& snap : deps_.sdk->Snapshots())
        {
            snaps.push_back(SnapshotToJson(snap, include_colors));
        }
    }
    return nlohmann::json::object({
        { "snapshots",  std::move(snaps) },
        { "connection", ConnectionJson() },
    });
}

nlohmann::json EngineService::DeliveryStatsJson(
    const nlohmann::json& body,
    const std::vector<DeviceSnapshot>& snaps) const
{
    if(!deps_.pipeline)
    {
        return nlohmann::json::object({
            { "available", false },
            { "reason",    "no delivery pipeline configured" },
        });
    }

    /* Per-controller query: {controller:{device,serial,location}}. */
    if(body.is_object() && body.contains("controller"))
    {
        const DeviceKey key = KeyFromJson(body["controller"]);
        DeliveryStats   stats;
        nlohmann::json  out;
        out["available"]  = true;
        out["controller"] = KeyToJson(key);
        out["found"]      = deps_.pipeline->Stats(key, stats);
        if(out["found"].get<bool>())
        {
            out["stats"] = StatsToJson(stats);
        }
        return out;
    }

    nlohmann::json controllers = nlohmann::json::array();
    for(const DeviceSnapshot& snap : snaps)
    {
        DeliveryStats stats;
        if(deps_.pipeline->Stats(snap.key, stats))
        {
            nlohmann::json c;
            c["key"]                 = KeyToJson(snap.key);
            c["stats"]               = StatsToJson(stats);
            c["pending"]             =
                deps_.pipeline->PendingCount(snap.key);
            /* QA F04/F11 qualifiers: a lane with zero
               deliveries is still reported (it always was —
               Stats() covers registered lanes) but now says
               so explicitly instead of implying it through
               absent colors/budget. */
            c["delivered"]           = false;
            c["wireless"]            =
                IsWirelessGroupController(snap);
            c["suppressed"]          =
                deps_.pipeline->IsSuppressed(snap.key);
            c["owned"]               =
                deps_.playback &&
                deps_.playback->OwnsGroup(snap.key);
            c["colors"]              = nlohmann::json::array();
            RgbFrame delivered;
            Clock::duration budget;
            if(deps_.pipeline->LastDelivered(snap.key,
                                             delivered, &budget))
            {
                c["delivered"] = true;
                c["colors"] = ColorsToJson(delivered.colors);
                c["budget_ms"] =
                    std::chrono::duration_cast<
                        std::chrono::milliseconds>(budget)
                        .count();
            }
            controllers.push_back(std::move(c));
        }
    }
    return nlohmann::json::object({
        { "available",   true                                       },
        { "aggregate",   StatsToJson(deps_.pipeline->Stats())       },
        { "pending",     deps_.pipeline->PendingCount()             },
        { "controllers", std::move(controllers)                     },
        /* QA F11: the wireless lane's own counters — the
           aggregate mixes wired lanes and must never be read
           as the wireless rate. */
        { "wireless",    WirelessLaneJson(snaps)                    },
        /* QA F04 observability: the last MapFrame pass —
           "bound but unwritable/unresolved" objects emit no
           frames and are invisible in delivery counters; the
           mapper's counters are the only surface that counts
           them. Null when no mapper is wired. */
        { "map_stats",   deps_.mapper
                             ? MapStatsToJson(
                                   deps_.mapper->LastMapStats())
                             : nlohmann::json(nullptr)              },
    });
}

nlohmann::json EngineService::WirelessLaneJson(
    const std::vector<DeviceSnapshot>& snaps) const
{
    /* The Lian Li wireless service owns ONE full-group LED
       buffer — its controllers carry the "Wireless:" location
       (IsWirelessGroupController). The first enumerated group
       with a registered delivery lane reports its OWN lane
       counters; null when no wireless lane exists. */
    if(!deps_.pipeline)
    {
        return nullptr;
    }
    for(const DeviceSnapshot& snap : snaps)
    {
        if(!IsWirelessGroupController(snap))
        {
            continue;
        }
        DeliveryStats ws;
        if(!deps_.pipeline->Stats(snap.key, ws))
        {
            /* Enumerated but no lane yet — keep looking; a
               laneless group reports "not measured" through
               the null, never through wired numbers. */
            continue;
        }
        return nlohmann::json::object({
            { "key",        KeyToJson(snap.key)                  },
            { "stats",      StatsToJson(ws)                      },
            { "suppressed", deps_.pipeline->IsSuppressed(
                                snap.key)                        },
            { "pending",    (uint64_t)deps_.pipeline->PendingCount(
                                snap.key)                        },
            { "owned",      deps_.playback &&
                            deps_.playback->OwnsGroup(snap.key)   },
        });
    }
    return nullptr;
}

nlohmann::json EngineService::FrameStatusJson(
    const std::vector<DeviceSnapshot>& snaps) const
{
    /* The delivered-state feed for the Dashboard preview: what
       the transport last accepted per controller (plan
       section 5 — preview and output share the same sampled
       colors; preview FPS is labelled separately).
       QA F04: a lane that has never delivered still produces a
       row — delivered:false, zeroed counters, empty colors —
       so "bound but never delivered" is a visible state, not
       an absent row. */
    nlohmann::json controllers = nlohmann::json::array();
    if(deps_.pipeline)
    {
        for(const DeviceSnapshot& snap : snaps)
        {
            RgbFrame delivered;
            const bool has_delivered =
                deps_.pipeline->LastDelivered(snap.key, delivered);
            DeliveryStats stats;
            const bool has_lane =
                deps_.pipeline->Stats(snap.key, stats);
            controllers.push_back(nlohmann::json::object({
                { "key",        KeyToJson(snap.key)                },
                { "delivered",  has_delivered                      },
                { "lane",       has_lane                           },
                { "wireless",   IsWirelessGroupController(snap)    },
                { "suppressed", deps_.pipeline->IsSuppressed(
                                    snap.key)                      },
                { "owned",      deps_.playback &&
                                deps_.playback->OwnsGroup(
                                    snap.key)                      },
                { "pending",    (uint64_t)deps_.pipeline->
                                    PendingCount(snap.key)         },
                { "sequence",   has_delivered
                                    ? delivered.sequence : 0       },
                { "colors",     has_delivered
                                    ? ColorsToJson(delivered.colors)
                                    : nlohmann::json::array()      },
                { "stats",      StatsToJson(stats)                 },
            }));
        }
    }
    return nlohmann::json::object({
        { "aggregate",   deps_.pipeline
                             ? StatsToJson(deps_.pipeline->Stats())
                             : StatsToJson(DeliveryStats{}) },
        /* QA F11: the wireless lane's own counters — never the
           wired-mixing aggregate under a wireless label. */
        { "wireless",    WirelessLaneJson(snaps)              },
        { "controllers", std::move(controllers)               },
    });
}

nlohmann::json EngineService::PowerJson() const
{
    if(!deps_.power)
    {
        return nullptr;
    }
    nlohmann::json out;
    out["display_on"]      = deps_.power->DisplayOn();
    out["suspended"]       = deps_.power->Suspended();
    out["output_paused"]   = deps_.power->OutputPaused();
    out["power_hold"]      = deps_.power->PowerHold();
    out["restore_pending"] = deps_.power->RestorePending();
    const std::string detail = deps_.power->Detail();
    out["detail"] = detail.empty() ? nlohmann::json(nullptr)
                                   : nlohmann::json(detail);
    return out;
}

nlohmann::json EngineService::ReactiveJson() const
{
    /* No manager wired -> honest absence, never a fabricated
       set of toggles. */
    if(!deps_.reactive)
    {
        return nlohmann::json::object({
            { "available", false },
            { "reason",    "reactive inputs are not wired on "
                           "this engine" },
            { "inputs",    nlohmann::json::array() },
            { "displays",  nlohmann::json::array() },
        });
    }

    nlohmann::json inputs = nlohmann::json::array();
    for(const ReactiveInputStatus& s : deps_.reactive->Status())
    {
        inputs.push_back(nlohmann::json::object({
            { "id",           s.id           },
            { "available",    s.available    },
            { "enabled",      s.enabled      },
            { "running",      s.running      },
            { "paused",       s.paused       },
            { "sensitivity",  s.sensitivity  },
            { "screen_index", s.screen_index },
            { "status",       s.status       },
        }));
    }
    nlohmann::json displays = nlohmann::json::array();
    for(const ReactiveDisplay& d : deps_.reactive->Displays())
    {
        displays.push_back(nlohmann::json::object({
            { "index",  d.index  },
            { "name",   d.name   },
            { "width",  d.width  },
            { "height", d.height },
        }));
    }
    return nlohmann::json::object({
        { "available",      true                        },
        { "capture_paused", deps_.reactive->Paused()    },
        { "inputs",         std::move(inputs)           },
        { "displays",       std::move(displays)         },
    });
}

void EngineService::EmitReactive()
{
    Emit(event_kind::kReactive, ReactiveJson());
}

void EngineService::EmitCalibrationStatus()
{
    /* Task 11: the calibration session's changed hook —
       session-targeted status through the commands layer
       (the owner gets the token-carrying body; the push
       replaces queued copies on the "status" slot). */
    if(ws_commands_)
    {
        ws_commands_->EmitCalibrationStatus();
    }
}

nlohmann::json EngineService::OwnershipJson() const
{
    /* Task 4.7: no provider wired -> honest null (the IPC
       schema marks the field "null when no ownership report
       is wired"), never an empty conflict list that would
       read as "verified clean". A non-object provider result
       is a bug in the owner — null is still the honest
       surface. */
    if(!deps_.ownership)
    {
        return nullptr;
    }
    const nlohmann::json j = deps_.ownership();
    return j.is_object() ? j : nullptr;
}

/* QA F12: the IPC counters body on its own — STATE and
   DIAGNOSTICS both embed it; building it here keeps DIAG_GET
   from re-serializing an entire StateJson for one field. */
nlohmann::json EngineService::IpcStatsJson() const
{
    if(!deps_.server_stats)
    {
        return nullptr;
    }
    const IpcServerStats st = deps_.server_stats();
    return nlohmann::json::object({
        { "sessions_active",  st.sessions_active  },
        { "sessions_total",   st.sessions_total   },
        { "messages_in",      st.messages_in      },
        { "responses_out",    st.responses_out    },
        { "events_sent",      st.events_sent      },
        { "events_dropped",   st.events_dropped   },
        { "queued_events",    st.queued_events    },
        { "resync_bytes",     st.resync_bytes     },
        { "rejected_hellos",  st.rejected_hellos  },
        { "rate_limited",     st.rate_limited     },
        { "refused_busy",     st.refused_busy     },
    });
}

nlohmann::json EngineService::StateJson() const
{
    return nlohmann::json::object({
        { "engine", nlohmann::json::object({
            { "running",         engine_.Running()          },
            { "scene_clock_hz",  SceneWorker::kTickHz       },
            { "scene_sequence",  engine_.Worker().Sequence()},
        })},
        { "scene",      SceneJsonLocked()                    },
        { "sdk",        ConnectionJson()                     },
        { "playback",   PlaybackJson()                       },
        { "power",      PowerJson()                          },
        { "reactive",   ReactiveJson()                       },
        { "ownership",  OwnershipJson()                      },
        { "ipc",        IpcStatsJson()                       },
    });
}

nlohmann::json EngineService::DiagnosticsJson(TimePoint now) const
{
    const double uptime_s =
        std::chrono::duration<double>(now - started_).count();
    const double steady_ms =
        std::chrono::duration<double, std::milli>(
            now.time_since_epoch()).count();

    /* QA F12: ONE topology capture shared by every section
       that needs it — the old body re-ran the whole
       StateJson() for "ipc" alone and let each section take
       its own SDK lock acquisition. */
    const std::vector<DeviceSnapshot> snaps = LayoutSnapshots();

    return nlohmann::json::object({
        { "engine", nlohmann::json::object({
            { "running",        engine_.Running()           },
            { "scene_clock_hz", SceneWorker::kTickHz        },
            { "scene_sequence", engine_.Worker().Sequence() },
            { "uptime_s",       uptime_s                    },
        })},
        { "scene",            SceneJsonLocked()              },
        { "playback",         PlaybackJson()                 },
        { "delivery",         DeliveryStatsJson(
                                  nlohmann::json::object(),
                                  snaps)                     },
        { "sdk",              ConnectionJson()               },
        { "power",            PowerJson()                    },
        { "reactive",         ReactiveJson()                 },
        { "ownership",        OwnershipJson()                },
        { "ipc",              IpcStatsJson()                 },
        { "clock", nlohmann::json::object({
            { "steady_ms", steady_ms },
        })},
        { "backend_counters", BackendCountersJson()           },
    });
}

/*---------------------------------------------------------*\
||| backend_counters — the vendor SDK extension's cached   |||
||| snapshot. WirelessStatus() is a state-lock read; this   |||
||| never waits behind wire I/O (same discipline as the     |||
||| F12 fix). The raw backend document is passed through    |||
||| verbatim so server-side fields surface without a client |||
||| rev.                                                    |||
\*---------------------------------------------------------*/

nlohmann::json EngineService::BackendCountersJson() const
{
    if(!deps_.sdk)
    {
        return nullptr;              /* honest: no backend   */
    }
    sdk::WirelessStatusSnapshot snap;
    deps_.sdk->WirelessStatus(snap);
    if(!snap.probed)
    {
        return nlohmann::json::object({
            { "available", false },
            { "probed",    false },
        });
    }
    if(!snap.supported)
    {
        /* Stock backend: it dropped packet 4000 — "unsupported"
           is itself diagnostic. */
        return nlohmann::json::object({
            { "available", false },
            { "probed",    true  },
            { "supported", false },
        });
    }
    if(!snap.have_data)
    {
        /* Answered but never parsed (e.g. malformed body). */
        return nlohmann::json::object({
            { "available", false },
            { "probed",    true  },
            { "supported", true  },
        });
    }
    nlohmann::json out = snap.status.raw;
    out["probed"]    = true;
    out["supported"] = true;
    out["fetched_age_ms"] =
        std::chrono::duration<double, std::milli>(
            Clock::now() - snap.fetched_at).count();
    return out;
}

/*---------------------------------------------------------*\
|| IIpcService                                               ||
\*---------------------------------------------------------*/

nlohmann::json EngineService::OnWelcome()
{
    nlohmann::json caps = nlohmann::json::array();
    if(ws_commands_)
    {
        /* studio-import Task 3: managed-workspace jobs,
           revisions, and bounded transfers (plan 3.3). A client
           that doesn't see it falls back to the legacy surface
           unchanged. */
        caps.push_back("studio_workspace_v1");
    }
    return nlohmann::json::object({
        { "engine",       "openrgb2-engine" },
        { "running",      engine_.Running() },
        { "capabilities", caps              },
        { "state",        StateJson()       },
    });
}

bool EngineService::SnapshotEvent(const std::string& kind,
                                  nlohmann::json& out)
{
    if(kind == event_kind::kTopology)
    {
        out = SnapshotsJson();
        return true;
    }
    if(kind == event_kind::kScene)
    {
        out = SceneJsonLocked();
        return true;
    }
    if(kind == event_kind::kPlayback)
    {
        out = PlaybackJson();
        return true;
    }
    if(kind == event_kind::kConnection)
    {
        out = ConnectionJson();
        return true;
    }
    if(kind == event_kind::kFrameStatus)
    {
        out = FrameStatusJson(LayoutSnapshots());
        return true;
    }
    if(kind == event_kind::kReactive)
    {
        out = ReactiveJson();
        return true;
    }
    if(kind == event_kind::kWorkspace)
    {
        /* The initial snapshot is the `get` reply — metadata
           only; document/type/look content rides transfers. */
        out = ws_commands_ ? ws_commands_->SnapshotJson()
                           : nlohmann::json::object(
                                 {{ "managed", false }});
        return true;
    }
    return false;
}

bool EngineService::SnapshotEventFor(uint64_t session_id,
                                     const std::string& kind,
                                     nlohmann::json& out)
{
    if(kind == event_kind::kPreview)
    {
        /* Session-bound: "preview" reports THIS session's
           preview state — a status record, never another
           session's frames and never a global broadcast. */
        out = ws_commands_
                  ? ws_commands_->PreviewStatusJson(session_id)
                  : nlohmann::json::object({
                        { "type",   "status" },
                        { "active", false    },
                        { "paused", false    },
                    });
        return true;
    }
    if(kind == event_kind::kCalibration)
    {
        /* Task 11: session-bound — this session's own
           calibration view; the token is only reported to
           the session that owns the session. */
        out = ws_commands_
                  ? ws_commands_->CalibrationStatusJson(session_id)
                  : nlohmann::json::object({
                        { "active",   false },
                        { "starting", false },
                        { "ending",   false },
                    });
        return true;
    }
    return SnapshotEvent(kind, out);
}

bool EngineService::WorkspaceManaged() const
{
    return deps_.workspace != nullptr &&
           deps_.workspace->ManagedActive();
}

IpcReply EngineService::DispatchSession(uint64_t session_id,
                                        MsgType type,
                                        const nlohmann::json& body)
{
    if(type == MsgType::WorkspaceCommand)
    {
        if(!ws_commands_)
        {
            return IpcReply::Fail(ErrCode::Unavailable,
                "the studio_workspace_v1 capability is not "
                "present on this server — use the legacy "
                "surface");
        }
        return ws_commands_->Handle(session_id, body);
    }
    return Dispatch(type, body);
}

void EngineService::OnSessionEnd(uint64_t session_id)
{
    /* Session-bound workspace state dies with the session:
       queued jobs and staged transfers are cancelled. A commit
       already past the durable marker is never rolled back —
       the reconnecting client reconciles by revision/content
       hash. */
    if(ws_commands_)
    {
        ws_commands_->OnSessionEnd(session_id);
    }
}

IpcReply EngineService::Dispatch(MsgType type,
                                 const nlohmann::json& body)
{
    switch(type)
    {
    /* A WorkspaceCommand arriving through the non-session
       dispatch path (direct service calls in harnesses) binds
       to session 0 — the jobs/transfers still work, scoped to
       a virtual session that never disconnects. */
    case MsgType::WorkspaceCommand:
        return DispatchSession(0, type, body);

    case MsgType::GetSnapshots:
        return IpcReply::Success(MsgType::Snapshots,
                                 SnapshotsJson());

    case MsgType::GetScene:
        return IpcReply::Success(MsgType::Scene,
                                 SceneJsonLocked());

    case MsgType::SetScene:
    {
        /* studio-import Task 3: once a managed workspace is
           active, the lossy legacy scene path is refused — the
           workspace is the authoring surface (plan 3.3). The
           gate is honest: reads (GET_SCENE), identify, and
           telemetry stay available; only document mutations
           are refused. */
        if(WorkspaceManaged())
        {
            IpcReply r = IpcReply::Fail(ErrCode::BadRequest,
                "a managed workspace is active — edit it through "
                "WORKSPACE_COMMAND (base_revision + "
                "resource_revision), not the lossy legacy "
                "scene path");
            r.body["detail"] = "workspace_required";
            return r;
        }
        if(!body.is_object() || !body.contains("document")
           || !body["document"].is_object())
        {
            return IpcReply::Fail(ErrCode::BadRequest,
                "SET_SCENE requires {document:<SceneDocument>}");
        }
        auto doc = std::make_shared<studio::SceneDocument>();
        std::vector<std::string> errors;
        if(!studio::FromJson(body["document"], *doc, &errors))
        {
            nlohmann::json b = nlohmann::json::object(
                { { "errors", errors } });
            IpcReply r = IpcReply::Fail(ErrCode::BadRequest,
                                        "scene document failed to load");
            r.body     = std::move(b);
            return r;
        }
        std::string err;
        if(!ApplySceneDocument(doc, err))
        {
            return IpcReply::Fail(ErrCode::BadRequest, err);
        }
        return IpcReply::Success(MsgType::Ok,
            nlohmann::json::object({
                { "revision", SceneRevision() },
                { "name",     doc->name       },
            }));
    }

    case MsgType::GetPlayback:
        return IpcReply::Success(MsgType::Playback,
                                 PlaybackJson());

    case MsgType::SetPlaybackMode:
    {
        if(!body.is_object() || !body.value("mode", nlohmann::json())
                                    .is_string())
        {
            return IpcReply::Fail(ErrCode::BadRequest,
                "SET_PLAYBACK_MODE requires {mode:\"live_effects\""
                "|\"animation_playback\"}");
        }
        const std::string name = body["mode"].get<std::string>();
        PlaybackMode mode;
        if(!PlaybackModeFromName(name, mode))
        {
            return IpcReply::Fail(ErrCode::BadRequest,
                                  "unknown playback mode '" + name + "'");
        }

        /* Optional transport control: "playing" (default) |
           "paused" (sampled hold) | "stopped" (black). */
        PlaybackControl control = PlaybackControl::Playing;
        {
            const auto it = body.find("control");
            if(it != body.end())
            {
                if(!it->is_string())
                {
                    return IpcReply::Fail(ErrCode::BadRequest,
                        "control must be \"playing\"|\"paused\""
                        "|\"stopped\"");
                }
                const std::string c = it->get<std::string>();
                if(c == "paused")      control = PlaybackControl::Paused;
                else if(c == "stopped") control = PlaybackControl::Stopped;
                else if(c != "playing")
                {
                    return IpcReply::Fail(ErrCode::BadRequest,
                        "unknown playback control '" + c + "'");
                }
            }
        }

        /* Optional preset parameters (animation playback):
           {speed:int, reverse:bool, color:"#RRGGBB"}. Fields
           present in the request overlay the controller's
           RECORDED selection — an omitted field preserves the
           persisted per-scene value rather than resetting to
           defaults (the wire exposes parameter revisions, not
           values, so a UI has nothing honest to seed fields
           it isn't changing; omission must never clobber the
           recorded selection). */
        PlaybackParams params = deps_.playback
            ? deps_.playback->CurrentParams()
            : PlaybackParams{};
        {
            const auto it = body.find("params");
            if(it != body.end())
            {
                if(!it->is_object())
                {
                    return IpcReply::Fail(ErrCode::BadRequest,
                        "params must be an object");
                }
                const nlohmann::json& p = *it;
                const auto pj = p.find("speed");
                if(pj != p.end())
                {
                    if(!pj->is_number_integer())
                    {
                        return IpcReply::Fail(ErrCode::BadRequest,
                            "params.speed must be an integer");
                    }
                    params.speed = pj->get<int32_t>();
                }
                const auto rj = p.find("reverse");
                if(rj != p.end())
                {
                    if(!rj->is_boolean())
                    {
                        return IpcReply::Fail(ErrCode::BadRequest,
                            "params.reverse must be a boolean");
                    }
                    params.reverse = rj->get<bool>();
                }
                const auto cj = p.find("color");
                if(cj != p.end())
                {
                    if(!cj->is_string() ||
                       !ParseHexColor(cj->get<std::string>(),
                                      params.color))
                    {
                        return IpcReply::Fail(ErrCode::BadRequest,
                            "params.color must be \"#RRGGBB\"");
                    }
                }
            }
        }

        if(deps_.playback)
        {
            /* The real transition: suppression, SDK select,
               revision acknowledgement (Task 3.6). */
            const PlaybackController::Result r =
                deps_.playback->Request(mode, control, params);
            if(!r.accepted)
            {
                return IpcReply::Fail(ErrCode::Unavailable,
                                      r.reason);
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                requested_mode_ = mode;
            }
            Emit(event_kind::kPlayback, PlaybackJson());
            /* status.mode only moves when the backend reports
               the applied revision — "pending" is the honest
               state until then. */
            return IpcReply::Success(MsgType::Ok,
                nlohmann::json::object({
                    { "requested_mode", PlaybackModeName(mode) },
                    { "applied",        !r.pending             },
                    { "pending",        r.pending              },
                    { "revision",       r.revision             },
                }));
        }

        PlaybackCapabilities caps;
        caps.live_effects       = true;
        caps.animation_playback = false;
        if(deps_.playback_caps)
        {
            caps = deps_.playback_caps();
        }
        if(!PlaybackModeSupported(mode, caps))
        {
            return IpcReply::Fail(ErrCode::Unavailable,
                std::string("playback mode '") + name +
                "' is not supported by the selected scene "
                "(capabilities report animation_playback=false)");
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requested_mode_ = mode;
        }
        Emit(event_kind::kPlayback, PlaybackJson());
        /* No controller wired: the request is recorded;
           status.mode is the APPLIED state. */
        return IpcReply::Success(MsgType::Ok,
            nlohmann::json::object({
                { "requested_mode", PlaybackModeName(mode) },
                { "applied",        false                  },
                { "pending",        true                   },
            }));
    }

    case MsgType::GetDeliveryStats:
    {
        /* A controller filter must be a DeviceKey object with
           string fields — wrong-typed input is a bounded
           bad_request, not a thrown type_error. */
        if(body.is_object() && body.contains("controller"))
        {
            const nlohmann::json& cj = body["controller"];
            bool key_ok = cj.is_object();
            if(key_ok)
            {
                static const char* kFields[] = {
                    "device", "serial", "location", "zone"
                };
                for(const char* f : kFields)
                {
                    const auto it = cj.find(f);
                    if(it != cj.end() && !it->is_string())
                    {
                        key_ok = false;
                        break;
                    }
                }
            }
            if(!key_ok)
            {
                return IpcReply::Fail(ErrCode::BadRequest,
                    "controller must be a DeviceKey object "
                    "with string fields");
            }
        }
        return IpcReply::Success(
            MsgType::DeliveryStats,
            DeliveryStatsJson(body, LayoutSnapshots()));
    }

    case MsgType::GetState:
        return IpcReply::Success(MsgType::State, StateJson());

    case MsgType::GetPresets:
    {
        nlohmann::json presets = nlohmann::json::array();
        std::set<std::string> seen;
        for(const studio::PresetInfo& p : studio::PresetList())
        {
            seen.insert(p.id);
            presets.push_back(nlohmann::json::object({
                { "id",          p.id          },
                { "name",        p.name        },
                { "description", p.description },
                { "needs",       p.needs       },
            }));
        }
        /* Personal looks landed by resource_install live in the
           managed workspace library — the global look registry
           never re-scans it, so merge the effective registry's
           file-supplied entries (sorted tail, like the catalog).
           Resolution goes through the same registry, so an id
           listed here always resolves. */
        if(deps_.workspace)
        {
            const auto looks =
                deps_.workspace->Store().EffectiveEffectRegistry()
                    .List({});
            for(const studio::EffectRegistry::EffectInfo& i : looks)
            {
                if(!i.from_file || seen.count(i.id))
                {
                    continue;
                }
                presets.push_back(nlohmann::json::object({
                    { "id",          i.id          },
                    { "name",        i.name        },
                    { "description", i.description },
                    { "needs",       i.needs       },
                }));
            }
        }
        return IpcReply::Success(MsgType::Presets,
            nlohmann::json::object({{ "presets", presets }}));
    }

    case MsgType::LayoutCommand:
    {
        /* value() with a json default can't throw; the
           is_string() guard keeps get<>() from ever seeing a
           wrong-typed verb. */
        const std::string verb =
            (body.is_object()
             && body.value("verb", nlohmann::json()).is_string())
                ? body["verb"].get<std::string>()
                : std::string();
        if(verb.empty())
        {
            return IpcReply::Fail(ErrCode::BadRequest,
                "LAYOUT_COMMAND requires {verb:<string>}");
        }
        if(!IsKnownLayoutVerb(verb))
        {
            return IpcReply::Fail(ErrCode::BadRequest,
                                  "unknown layout verb '" + verb + "'");
        }
        return HandleLayoutCommand(verb, body);
    }

    case MsgType::ReactiveCommand:
    {
        const std::string verb =
            (body.is_object()
             && body.value("verb", nlohmann::json()).is_string())
                ? body["verb"].get<std::string>()
                : std::string();
        if(verb.empty())
        {
            return IpcReply::Fail(ErrCode::BadRequest,
                "REACTIVE_COMMAND requires {verb:<string>}");
        }
        if(!IsKnownReactiveVerb(verb))
        {
            return IpcReply::Fail(ErrCode::BadRequest,
                "unknown reactive verb '" + verb + "'");
        }
        return HandleReactiveCommand(verb, body);
    }

    case MsgType::Quit:
    {
        /* The "Quit All" hook (plan section 4): the UI's quit
           command lands here; the engine then runs its ordered,
           bounded shutdown (OrderlyShutdown in main.cpp — owned
           components stop in order). The reply body is assembled
           BEFORE the hook fires; the hook's own fields are merged
           in so the backend-handoff decision rides the same OK. */
        if(!deps_.request_shutdown)
        {
            return IpcReply::Fail(ErrCode::Unavailable,
                "engine shutdown is not wired on this process");
        }
        const bool stop_backend =
            body.is_object() &&
            body.value("stop_backend", nlohmann::json()).is_boolean() &&
            body["stop_backend"].get<bool>();
        nlohmann::json reply_body = nlohmann::json::object({
            { "shutting_down", true          },
            { "ordered",       true          },
            { "stop_backend",  stop_backend  },
        });
        const nlohmann::json extra =
            deps_.request_shutdown(stop_backend);
        if(extra.is_object())
        {
            for(auto it = extra.begin(); it != extra.end(); ++it)
            {
                reply_body[it.key()] = it.value();
            }
        }
        return IpcReply::Success(MsgType::Ok, std::move(reply_body));
    }

    case MsgType::DiagGet:
        return IpcReply::Success(MsgType::Diagnostics,
                                 DiagnosticsJson());

    default:
        return IpcReply::Fail(ErrCode::UnknownCommand,
                              "no handler for message type");
    }
}

/*---------------------------------------------------------*\
|| Diff-driven events                                        ||
\*---------------------------------------------------------*/

/*---------------------------------------------------------*\
||| LAYOUT_COMMAND handlers (Task 4.4)                        ||
|||                                                            ||
|||   identify        — transient: the mapper's bounded       ||
|||                     blink overlay, resolved against the   ||
|||                     LIVE topology (never persisted, never ||
|||                     a scene mutation).                    ||
|||   set_position / set_orientation / set_mirror /           ||
|||   calibrate_led / set_binding — scene-document edits:     ||
|||                     copy -> mutate -> ApplySceneDocument, ||
|||                     the ONE apply path SET_SCENE uses, so ||
|||                     validation, revision, the scene       ||
|||                     event, and the mapper re-bind are     ||
|||                     identical.                            ||
\*---------------------------------------------------------*/

namespace
{

bool Vec3FromJson(const nlohmann::json& j, studio::Vec3& out)
{
    if(!j.is_object())
    {
        return false;
    }
    const auto num = [&j](const char* k, float& v)
    {
        const auto it = j.find(k);
        if(it == j.end() || !it->is_number())
        {
            return false;
        }
        const double d = it->get<double>();
        if(!std::isfinite(d))
        {
            return false;
        }
        v = (float)d;
        return true;
    };
    return num("x", out.x) && num("y", out.y) && num("z", out.z);
}

/* A DeviceKey field-set check — same bounded bad_request the
   DELIVERY_STATS controller filter applies. */
bool KeyFieldsOk(const nlohmann::json& cj)
{
    if(!cj.is_object())
    {
        return false;
    }
    static const char* kFields[] = {
        "device", "serial", "location", "zone"
    };
    for(const char* f : kFields)
    {
        const auto it = cj.find(f);
        if(it != cj.end() && !it->is_string())
        {
            return false;
        }
    }
    return true;
}

const DeviceSnapshot* FindSnapshot(
    const std::vector<DeviceSnapshot>& snaps,
    const DeviceKey& key)
{
    for(const DeviceSnapshot& s : snaps)
    {
        if(s.key.device   == key.device &&
           s.key.serial   == key.serial &&
           s.key.location == key.location)
        {
            return &s;
        }
    }
    return nullptr;
}

/* Zone name -> absolute offset inside the complete-controller
   buffer (zone order = enumeration order). */
bool ZoneOffset(const DeviceSnapshot& snap, const std::string& zone,
                uint32_t& start, uint32_t& count)
{
    uint32_t off = 0;
    for(const ZoneSnapshot& z : snap.zones)
    {
        if(z.name == zone)
        {
            start = off;
            count = z.led_count;
            return true;
        }
        off += z.led_count;
    }
    return false;
}

} /* anonymous namespace */

std::vector<DeviceSnapshot> EngineService::LayoutSnapshots() const
{
    if(deps_.snapshots)
    {
        return deps_.snapshots();
    }
    if(deps_.sdk)
    {
        return deps_.sdk->Snapshots();
    }
    return {};
}

IpcReply EngineService::HandleLayoutCommand(
    const std::string& verb, const nlohmann::json& body)
{
    if(verb == "identify")
    {
        return HandleIdentify(body);
    }
    if(verb == "add_object")
    {
        return HandleAddObjects(body);
    }
    return HandleLayoutMutate(verb, body);
}

IpcReply EngineService::HandleIdentify(const nlohmann::json& body)
{
    /* {controller:<DeviceKey>, zone?:"<zone name>",
       led_index?:int, duration_ms?:int} */
    if(!body.is_object() || !KeyFieldsOk(body.value("controller",
                                                   nlohmann::json())))
    {
        return IpcReply::Fail(ErrCode::BadRequest,
            "identify requires {controller:<DeviceKey>} with "
            "string fields");
    }
    if(!deps_.mapper)
    {
        return IpcReply::Fail(ErrCode::Unavailable,
            "no device mapper is wired on this engine");
    }
    DeviceKey key = KeyFromJson(body["controller"]);

    /* Zone selection: the top-level "zone" field, else the
       controller key's own zone field carries the name. */
    std::string zone_name = key.zone;
    {
        const auto zit = body.find("zone");
        if(zit != body.end())
        {
            if(!zit->is_string())
            {
                return IpcReply::Fail(ErrCode::BadRequest,
                    "zone must be a string");
            }
            zone_name = zit->get<std::string>();
        }
    }
    key.zone.clear();   /* the overlay key is controller-level */

    /* duration_ms: 0 = cancel; default 3000; bounded
       500..30000 so a stale request can't pin an override. */
    int64_t duration_ms = 3000;
    {
        const auto dit = body.find("duration_ms");
        if(dit != body.end())
        {
            if(!dit->is_number_integer())
            {
                return IpcReply::Fail(ErrCode::BadRequest,
                    "duration_ms must be an integer");
            }
            duration_ms = dit->get<int64_t>();
        }
    }
    if(duration_ms == 0)
    {
        const bool was =
            deps_.mapper->CancelIdentify(key, Clock::now());
        return IpcReply::Success(MsgType::Ok,
            nlohmann::json::object({
                { "identifying", false                 },
                { "cancelled",   was                   },
                { "controller",  KeyToJson(key)        },
            }));
    }
    if(duration_ms < 0)
    {
        return IpcReply::Fail(ErrCode::BadRequest,
            "duration_ms must be >= 0");
    }
    duration_ms = std::max<int64_t>(500,
                    std::min<int64_t>(duration_ms, 30000));

    /* Resolve against the LIVE topology — identify exists to
       find hardware, so a stale/absent key is honest
       Unavailable, not a silent no-op. (The snapshot vector
       must outlive the pointer — bind it, never dangle into
       a temporary.) */
    const std::vector<DeviceSnapshot> snaps = LayoutSnapshots();
    const DeviceSnapshot* snap = FindSnapshot(snaps, key);
    if(snap == nullptr)
    {
        return IpcReply::Fail(ErrCode::Unavailable,
            "controller '" + key.device +
            "' is not in the current topology");
    }
    if(snap->connection != ConnectionState::Connected)
    {
        return IpcReply::Fail(ErrCode::Unavailable,
            "controller '" + snap->name + "' is not connected");
    }
    /* An uploaded loop owns the wireless group — identification
       frames would only be suppressed at the lane, so the
       honest answer is Unavailable (Task 3.6: one producer
       per group). A physical calibration session owns the
       controller's lane the same way (Task 11). */
    if(deps_.playback && deps_.playback->OwnsGroup(key))
    {
        return IpcReply::Fail(ErrCode::Unavailable,
            "an uploaded animation loop owns '" + snap->name +
            "' — identification frames are suppressed while "
            "it is active");
    }
    if(deps_.calibration && deps_.calibration->OwnsGroup(key))
    {
        return IpcReply::Fail(ErrCode::Unavailable,
            "a calibration session owns '" + snap->name +
            "' — identification frames are suppressed while "
            "it is active");
    }

    uint32_t start = 0;
    uint32_t count = snap->LedCount();
    if(!zone_name.empty() &&
       !ZoneOffset(*snap, zone_name, start, count))
    {
        return IpcReply::Fail(ErrCode::BadRequest,
            "controller '" + snap->name +
            "' has no zone '" + zone_name + "'");
    }

    const auto lit = body.find("led_index");
    if(lit != body.end())
    {
        if(!lit->is_number_integer() || lit->get<int64_t>() < 0)
        {
            return IpcReply::Fail(ErrCode::BadRequest,
                "led_index must be a non-negative integer");
        }
        const int64_t idx = lit->get<int64_t>();
        if((uint64_t)idx >= count)
        {
            return IpcReply::Fail(ErrCode::BadRequest,
                "led_index " + std::to_string(idx) +
                " is out of range (" + std::to_string(count) +
                " LEDs in scope)");
        }
        start += (uint32_t)idx;
        count  = 1;
    }

    std::string err;
    if(!deps_.mapper->StartIdentify(key, start, count,
            std::chrono::milliseconds(duration_ms), Clock::now(),
            err))
    {
        return IpcReply::Fail(ErrCode::BadRequest, err);
    }
    return IpcReply::Success(MsgType::Ok,
        nlohmann::json::object({
            { "identifying", true                   },
            { "controller",  KeyToJson(key)         },
            { "zone",        zone_name              },
            { "led_start",   start                  },
            { "led_count",   count                  },
            { "duration_ms", duration_ms            },
        }));
}

IpcReply EngineService::HandleLayoutMutate(
    const std::string& verb, const nlohmann::json& body)
{
    /* studio-import Task 3: layout mutation verbs are lossy
       legacy edits — with a managed workspace active they are
       refused so a raw edit can never bypass the generation
       history. `identify` is NOT a mutation and stays live
       (it is handled before this gate). */
    if(WorkspaceManaged())
    {
        IpcReply r = IpcReply::Fail(ErrCode::BadRequest,
            "a managed workspace is active — layout changes go "
            "through WORKSPACE_COMMAND commit, not the legacy "
            "mutation path");
        r.body["detail"] = "workspace_required";
        return r;
    }
    return HandleLayoutMutateImpl(verb, body);
}

IpcReply EngineService::HandleLayoutMutateImpl(
    const std::string& verb, const nlohmann::json& body)
{
    if(!body.is_object() ||
       !body.value("object_id", nlohmann::json()).is_string() ||
       body["object_id"].get<std::string>().empty())
    {
        return IpcReply::Fail(ErrCode::BadRequest,
            verb + " requires {object_id:<string>}");
    }
    const std::string oid = body["object_id"].get<std::string>();

    std::shared_ptr<const studio::SceneDocument> cur = Scene();
    if(!cur)
    {
        return IpcReply::Fail(ErrCode::Unavailable,
            "no scene is loaded — apply a scene before "
            "layout edits");
    }
    auto doc = std::make_shared<studio::SceneDocument>(*cur);
    studio::SceneObject* obj = studio::FindObject(*doc, oid);
    if(obj == nullptr)
    {
        return IpcReply::Fail(ErrCode::BadRequest,
            "unknown scene object '" + oid + "'");
    }

    nlohmann::json detail = nlohmann::json::object();
    detail["object_id"] = oid;

    if(verb == "set_position")
    {
        /* {object_id, position:{x,y,z}} — meters, all three
           required (a partial vector silently dropping an
           axis is how arrangements corrupt). */
        studio::Vec3 p;
        const auto it = body.find("position");
        if(it == body.end() || !Vec3FromJson(*it, p))
        {
            return IpcReply::Fail(ErrCode::BadRequest,
                "set_position requires position:{x,y,z} "
                "finite numbers");
        }
        obj->transform.position = p;
        detail["position"] = *it;
    }
    else if(verb == "set_orientation")
    {
        /* {object_id, rotation:{x,y,z}} — euler XYZ degrees,
           the document's only rotation convention. */
        studio::Vec3 r;
        const auto it = body.find("rotation");
        if(it == body.end() || !Vec3FromJson(*it, r))
        {
            return IpcReply::Fail(ErrCode::BadRequest,
                "set_orientation requires rotation:{x,y,z} "
                "finite degrees");
        }
        obj->transform.rotation_deg = r;
        detail["rotation"] = *it;
    }
    else if(verb == "set_mirror")
    {
        /* {object_id, mirror_of} — "" unlinks. Linking follows
           the resolver's own semantics (SceneResolver): the
           copy becomes Linked, loses its binding/emitters/
           verified flag, and shares the source's output —
           never a second writer, never multiplied LEDs. */
        const auto it = body.find("mirror_of");
        if(it == body.end() || !it->is_string())
        {
            return IpcReply::Fail(ErrCode::BadRequest,
                "set_mirror requires {mirror_of:<object id>} "
                "(\"\" unlinks)");
        }
        const std::string target = it->get<std::string>();
        if(target.empty())
        {
            if(obj->kind != studio::ObjectKind::Linked)
            {
                return IpcReply::Fail(ErrCode::BadRequest,
                    "object '" + oid + "' is not linked");
            }
            /* Unlink restores Device kind; the emitters and
               binding the link consumed are NOT restored —
               reverting the whole document (the Layout
               view's undo) is the honest inverse. */
            obj->kind      = studio::ObjectKind::Device;
            obj->mirror_of.clear();
        }
        else
        {
            if(obj->kind != studio::ObjectKind::Device &&
               obj->kind != studio::ObjectKind::Linked)
            {
                return IpcReply::Fail(ErrCode::BadRequest,
                    "object '" + oid + "' (" +
                    std::string(
                        obj->kind == studio::ObjectKind::Group
                            ? "group" : "decor") +
                    ") cannot be mirrored — only device "
                    "objects link");
            }
            const studio::SceneObject* src =
                studio::FindObject(*doc, target);
            if(src == nullptr)
            {
                return IpcReply::Fail(ErrCode::BadRequest,
                    "mirror target '" + target +
                    "' does not exist");
            }
            if(target == oid)
            {
                return IpcReply::Fail(ErrCode::BadRequest,
                    "an object cannot mirror itself");
            }
            /* SceneGraph forbids mirror chains — the target
               must own output, i.e. be a Device object. */
            if(src->kind != studio::ObjectKind::Device)
            {
                return IpcReply::Fail(ErrCode::BadRequest,
                    "mirror target '" + target +
                    "' is not a device object (mirror chains "
                    "are not allowed)");
            }
            /* The reverse chain is equally forbidden: if
               `oid` is already someone's mirror SOURCE,
               linking it strands that mirror_of on a Linked
               object — the same chain ValidateSceneGraph
               rejects, and the source's bound zone would
               freeze (MapFrame skips Linked objects). */
            for(const studio::SceneObject& other : doc->objects)
            {
                if(other.id != oid && other.mirror_of == oid)
                {
                    return IpcReply::Fail(ErrCode::BadRequest,
                        "object '" + oid + "' is '" +
                        other.id +
                        "'s mirror source — unlink '" +
                        other.id +
                        "' first (mirror chains are not "
                        "allowed)");
                }
            }
            obj->kind      = studio::ObjectKind::Linked;
            obj->mirror_of = target;
            obj->binding.clear();
            obj->layout.clear();
            obj->emitters.clear();
            obj->verified  = false;
        }
        detail["mirror_of"] = target;
    }
    else if(verb == "calibrate_led")
    {
        /* {object_id, emitter_index:int, address:int} —
           address is an index inside the object's BOUND zone;
           -1 marks the emitter render-only. The bound comes
           from the binding's declared zone_leds, else the
           resolved zone's enumerated count. */
        if(obj->kind != studio::ObjectKind::Device)
        {
            return IpcReply::Fail(ErrCode::BadRequest,
                "calibrate_led targets device objects — '" +
                oid + "' is not one");
        }
        const auto eit = body.find("emitter_index");
        const auto ait = body.find("address");
        if(eit == body.end() || !eit->is_number_integer() ||
           ait == body.end() || !ait->is_number_integer())
        {
            return IpcReply::Fail(ErrCode::BadRequest,
                "calibrate_led requires integer "
                "emitter_index and address");
        }
        const int64_t ei = eit->get<int64_t>();
        const int64_t ad = ait->get<int64_t>();
        if(ei < 0 || (size_t)ei >= obj->emitters.size())
        {
            return IpcReply::Fail(ErrCode::BadRequest,
                "emitter_index " + std::to_string(ei) +
                " out of range (object '" + oid + "' has " +
                std::to_string(obj->emitters.size()) +
                " emitters)");
        }
        uint32_t zone_leds = 0;
        if(!obj->binding.empty())
        {
            for(const studio::DeviceBinding& b : doc->bindings)
            {
                if(b.id == obj->binding)
                {
                    zone_leds = b.zone_leds;
                    break;
                }
            }
        }
        if(zone_leds == 0 && deps_.mapper && !obj->binding.empty())
        {
            /* The persisted binding didn't declare a count —
               the resolved zone's enumerated count is the
               next honest bound. */
            ZonePlacement zp;
            if(deps_.mapper->BoundZone(obj->binding, zp))
            {
                zone_leds = zp.led_count;
            }
        }
        if(ad < -1)
        {
            return IpcReply::Fail(ErrCode::BadRequest,
                "address must be >= -1 (-1 = render-only)");
        }
        if(ad >= 0)
        {
            if(zone_leds == 0)
            {
                return IpcReply::Fail(ErrCode::BadRequest,
                    "object '" + oid +
                    "' has no bound zone LED count — assign "
                    "a binding before calibrating addresses");
            }
            if((uint64_t)ad >= zone_leds)
            {
                return IpcReply::Fail(ErrCode::BadRequest,
                    "address " + std::to_string(ad) +
                    " out of range (zone has " +
                    std::to_string(zone_leds) + " LEDs)");
            }
        }
        obj->emitters[(size_t)ei].address = (int)ad;
        detail["emitter_index"] = ei;
        detail["address"]       = ad;
    }
    else if(verb == "set_binding")
    {
        /* {object_id, binding:"<id>"|""[, controller:
           <DeviceKey>]} — repoint at an existing binding,
           clear with "", or CREATE a binding whose identity
           is derived from a live snapshot (the engine writes
           the identity fields — a client can never fabricate
           a controller). */
        const auto bit = body.find("binding");
        if(bit == body.end() || !bit->is_string())
        {
            return IpcReply::Fail(ErrCode::BadRequest,
                "set_binding requires {binding:<id>} "
                "(\"\" unbinds)");
        }
        if(obj->kind != studio::ObjectKind::Device)
        {
            return IpcReply::Fail(ErrCode::BadRequest,
                "set_binding targets device objects — '" +
                oid + "' is not one");
        }
        const std::string bid = bit->get<std::string>();
        if(bid.empty())
        {
            obj->binding.clear();
        }
        else
        {
            bool exists = false;
            for(const studio::DeviceBinding& b : doc->bindings)
            {
                if(b.id == bid)
                {
                    exists = true;
                    break;
                }
            }
            if(!exists)
            {
                /* New binding: identity must come from a live
                   snapshot — missing topology is honest
                   Unavailable, never a guessed identity. */
                if(!KeyFieldsOk(body.value("controller",
                                           nlohmann::json())))
                {
                    return IpcReply::Fail(ErrCode::BadRequest,
                        "unknown binding '" + bid +
                        "' — set_binding needs "
                        "{controller:<DeviceKey>} to create "
                        "it from a live snapshot");
                }
                const DeviceKey ck =
                    KeyFromJson(body["controller"]);
                const std::vector<DeviceSnapshot> snaps =
                    LayoutSnapshots();
                const DeviceSnapshot* snap =
                    FindSnapshot(snaps, ck);
                if(snap == nullptr)
                {
                    return IpcReply::Fail(ErrCode::Unavailable,
                        "controller '" + ck.device +
                        "' is not in the current topology — "
                        "a binding cannot be created for it");
                }
                uint32_t zstart = 0, zleds = snap->LedCount();
                std::string zone = ck.zone;
                if(!zone.empty() &&
                   !ZoneOffset(*snap, zone, zstart, zleds))
                {
                    return IpcReply::Fail(ErrCode::BadRequest,
                        "controller '" + snap->name +
                        "' has no zone '" + zone + "'");
                }
                if(zone.empty() && !snap->zones.empty())
                {
                    zone  = snap->zones[0].name;
                    zleds = snap->zones[0].led_count;
                }
                studio::DeviceBinding b;
                b.id              = bid;
                b.controller_name = snap->name;
                b.vendor          = snap->vendor;
                b.serial          = ck.serial;
                b.location        = ck.location;
                b.device_type     = snap->device_type;
                b.zone_name       = zone;
                b.zone_leds       = zleds;
                doc->bindings.push_back(std::move(b));
            }
            obj->binding = bid;
        }
        detail["binding"] = bid;
    }
    else
    {
        return IpcReply::Fail(ErrCode::BadRequest,
                              "unknown layout verb '" + verb + "'");
    }

    std::string err;
    if(!ApplySceneDocument(doc, err))
    {
        return IpcReply::Fail(ErrCode::BadRequest, err);
    }
    detail["revision"] = SceneRevision();
    return IpcReply::Success(MsgType::Ok, std::move(detail));
}

/*---------------------------------------------------------*\
||| add_object — scene authoring from live topology        |||
|||                                                          |||
|||   {controllers:[<DeviceKey>...], position?:{x,y,z}}      |||
|||                                                          |||
|||   Each DeviceKey resolves against LayoutSnapshots() —    |||
|||   the engine writes the binding identity from the        |||
|||   enumerated snapshot (name/vendor/type + key            |||
|||   serial/location + resolved zone), exactly like         |||
|||   set_binding's create path; a client can never          |||
|||   fabricate a controller. An identical existing          |||
|||   binding (same snapshot identity + zone) is reused —    |||
|||   adding the same zone twice yields two objects on ONE   |||
|||   binding, never a duplicate binding row.                |||
|||                                                          |||
|||   Emitters default by zone LED count: 1 -> a point,      |||
|||   2..12 -> a row along x, >12 -> a ring in the x/z       |||
|||   plane; addresses run 0..n-1 inside the zone. New       |||
|||   objects are VERIFIED: the binding identity came from   |||
|||   live topology, and DeviceMapper only writes verified   |||
|||   objects — an unverified add would be a device that     |||
|||   never lights, with no verb to flip it.                 |||
|||                                                          |||
|||   Batch semantics: per-entry failures report in          |||
|||   `skipped` ([{controller, error}]) while successes      |||
|||   apply as ONE document mutation — one revision, one     |||
|||   scene event, one undo step on the client. Every entry  |||
|||   failing is an error reply, scene untouched.            |||
\*---------------------------------------------------------*/
IpcReply EngineService::HandleAddObjects(const nlohmann::json& body)
{
    if(!body.is_object() ||
       !body.value("controllers", nlohmann::json()).is_array() ||
       body["controllers"].empty())
    {
        return IpcReply::Fail(ErrCode::BadRequest,
            "add_object requires "
            "{controllers:[<DeviceKey>...]}");
    }

    std::shared_ptr<const studio::SceneDocument> cur = Scene();
    if(!cur)
    {
        return IpcReply::Fail(ErrCode::Unavailable,
            "no scene is loaded — apply a scene before "
            "layout edits");
    }
    auto doc = std::make_shared<studio::SceneDocument>(*cur);

    /* Optional shared placement hint; without it each new
       object cascades right of the last so repeated adds
       don't stack on the origin. */
    bool         have_pos = false;
    studio::Vec3 pos;
    if(const auto pit = body.find("position");
       pit != body.end())
    {
        if(!Vec3FromJson(*pit, pos))
        {
            return IpcReply::Fail(ErrCode::BadRequest,
                "add_object position must be {x,y,z} finite "
                "numbers");
        }
        have_pos = true;
    }

    const std::vector<DeviceSnapshot> snaps =
        LayoutSnapshots();

    /* Smallest "p<N>" id not already taken — deterministic,
       bounded by the document size. */
    const auto next_id = [](const char* prefix, auto&& taken)
    {
        for(unsigned int i = 1;; ++i)
        {
            const std::string cand =
                prefix + std::to_string(i);
            if(!taken(cand))
            {
                return cand;
            }
        }
    };
    const auto object_id_taken = [&doc](const std::string& id)
    {
        return studio::FindObject(*doc, id) != nullptr;
    };
    const auto binding_id_taken = [&doc](const std::string& id)
    {
        for(const studio::DeviceBinding& b : doc->bindings)
        {
            if(b.id == id)
            {
                return true;
            }
        }
        return false;
    };

    nlohmann::json added   = nlohmann::json::array();
    nlohmann::json skipped = nlohmann::json::array();
    const auto skip = [&skipped](const nlohmann::json& cj,
                                 const std::string& e)
    {
        skipped.push_back(nlohmann::json::object({
            { "controller", cj.is_object()
                                ? cj : nlohmann::json() },
            { "error",      e },
        }));
    };

    for(const nlohmann::json& cj : body["controllers"])
    {
        if(!KeyFieldsOk(cj))
        {
            skip(cj, "controller entries are DeviceKey objects "
                     "with string fields");
            continue;
        }
        const DeviceKey ck = KeyFromJson(cj);
        const DeviceSnapshot* snap = FindSnapshot(snaps, ck);
        if(snap == nullptr)
        {
            skip(cj, "controller '" + ck.device +
                     "' is not in the current topology");
            continue;
        }
        uint32_t    zstart = 0, zleds = snap->LedCount();
        std::string zone = ck.zone;
        if(!zone.empty() &&
           !ZoneOffset(*snap, zone, zstart, zleds))
        {
            skip(cj, "controller '" + snap->name +
                     "' has no zone '" + zone + "'");
            continue;
        }
        if(zone.empty() && !snap->zones.empty())
        {
            zone  = snap->zones[0].name;
            zleds = snap->zones[0].led_count;
        }
        if(zleds == 0)
        {
            skip(cj, "zone '" + zone + "' on '" + snap->name +
                     "' has no LEDs");
            continue;
        }

        /* Binding reuse: an identical snapshot-derived binding
           (same controller identity + zone) is reused —
           repeated adds never duplicate the row. */
        std::string bid;
        for(const studio::DeviceBinding& b : doc->bindings)
        {
            if(b.controller_name == snap->name &&
               b.serial          == ck.serial &&
               b.location        == ck.location &&
               b.zone_name       == zone)
            {
                bid = b.id;
                break;
            }
        }
        if(bid.empty())
        {
            bid = next_id("b", binding_id_taken);
            studio::DeviceBinding b;
            b.id              = bid;
            b.controller_name = snap->name;
            b.vendor          = snap->vendor;
            b.serial          = ck.serial;
            b.location        = ck.location;
            b.device_type     = snap->device_type;
            b.zone_name       = zone;
            b.zone_leds       = zleds;
            doc->bindings.push_back(std::move(b));
        }

        studio::SceneObject o;
        o.id      = next_id("o", object_id_taken);
        o.label   = snap->name +
                    (zone.empty() ? std::string()
                                  : " / " + zone);
        o.kind     = studio::ObjectKind::Device;
        o.binding  = bid;
        o.verified = true;
        if(have_pos)
        {
            o.transform.position = pos;
        }
        else
        {
            o.transform.position.x =
                0.08f * (float)doc->objects.size();
        }
        for(uint32_t i = 0; i < zleds; ++i)
        {
            studio::Emitter e;
            e.address = (int)i;
            e.group   = o.id;
            if(zleds == 1)
            {
                /* a single point at the object origin */
            }
            else if(zleds <= 12)
            {
                /* a row across the body, centred on origin */
                e.local_pos.x =
                    ((float)i - (float)(zleds - 1) * 0.5f) *
                    0.008f;
            }
            else
            {
                /* a ring in the footprint plane (~8 mm pitch) */
                const float r =
                    std::max(0.03f,
                             (float)zleds * 0.008f /
                                 (2.0f * 3.14159265f));
                const float a =
                    2.0f * 3.14159265f * (float)i /
                    (float)zleds;
                e.local_pos.x = r * std::cos(a);
                e.local_pos.z = r * std::sin(a);
            }
            o.emitters.push_back(e);
        }

        added.push_back(nlohmann::json::object({
            { "object_id", o.id   },
            { "binding",   bid    },
            { "zone",      zone   },
            { "zone_leds", zleds  },
        }));
        doc->objects.push_back(std::move(o));
    }

    if(added.empty())
    {
        /* Nothing landed — the per-entry reasons ride in
           `skipped`; the scene stays untouched. */
        std::string msg = "no requested controller could be "
                          "added";
        if(!skipped.empty() &&
           !skipped[0].value("error", std::string()).empty())
        {
            msg = skipped[0]["error"].get<std::string>();
        }
        IpcReply r = IpcReply::Fail(ErrCode::Unavailable, msg);
        r.body = nlohmann::json::object(
            { { "skipped", std::move(skipped) } });
        return r;
    }

    std::string err;
    if(!ApplySceneDocument(doc, err))
    {
        return IpcReply::Fail(ErrCode::BadRequest, err);
    }
    return IpcReply::Success(MsgType::Ok,
        nlohmann::json::object({
            { "added",    std::move(added)   },
            { "skipped",  std::move(skipped) },
            { "revision", SceneRevision()    },
        }));
}

/*---------------------------------------------------------*\
||| REACTIVE_COMMAND handlers (Task 4.6)                      ||
|||                                                            ||
|||   set_enabled {input:"audio"|"screen"|"key",             ||
|||                enabled:bool} — starts/stops the capture   ||
|||                source. A disabled input captures          ||
|||                nothing (its thread is joined).            ||
|||   set_params  {input, sensitivity?|screen_index?} —       ||
|||                per-input tuning; wrong-input params       ||
|||                are bad_request, never silently ignored.   ||
|||   Every reply carries the full reactive body — the        ||
|||   client lands on real state, never an assumed toggle.    ||
|||   The "reactive" event for other subscribers is emitted   ||
|||   by the manager's changed hook (ReactiveInputs::Changed  ||
|||   -> EngineHost::OnReactiveChanged), NOT here — emitting  ||
|||   in both places would double-push every command.         ||
\*---------------------------------------------------------*/
IpcReply EngineService::HandleReactiveCommand(
    const std::string& verb, const nlohmann::json& body)
{
    if(!deps_.reactive)
    {
        return IpcReply::Fail(ErrCode::Unavailable,
            "reactive inputs are not wired on this engine");
    }
    if(!body.is_object() ||
       !body.value("input", nlohmann::json()).is_string())
    {
        return IpcReply::Fail(ErrCode::BadRequest,
            verb + " requires {input:\"audio\"|\"screen\"|\"key\"}");
    }
    const std::string input = body["input"].get<std::string>();
    bool known = false;
    for(const std::string& id : ReactiveInputs::InputIds())
    {
        if(id == input)
        {
            known = true;
            break;
        }
    }
    if(!known)
    {
        return IpcReply::Fail(ErrCode::BadRequest,
            "unknown reactive input '" + input + "'");
    }

    std::string err;
    if(verb == "set_enabled")
    {
        const auto it = body.find("enabled");
        if(it == body.end() || !it->is_boolean())
        {
            return IpcReply::Fail(ErrCode::BadRequest,
                "set_enabled requires {enabled:bool}");
        }
        const bool enabled = it->get<bool>();
        if(!deps_.reactive->SetEnabled(input, enabled, err))
        {
            /* Source missing/failed — honest Unavailable with
               the source's own reason; the slot stays
               disabled. */
            return IpcReply::Fail(ErrCode::Unavailable, err);
        }
        return IpcReply::Success(MsgType::Ok, ReactiveJson());
    }

    /* set_params: {input, sensitivity?} (audio) |
       {input:"screen", screen_index} */
    bool any = false;
    {
        const auto it = body.find("sensitivity");
        if(it != body.end())
        {
            if(!it->is_number())
            {
                return IpcReply::Fail(ErrCode::BadRequest,
                    "sensitivity must be a number");
            }
            any = true;
            if(!deps_.reactive->SetSensitivity(
                   input, (float)it->get<double>(), err))
            {
                return IpcReply::Fail(ErrCode::BadRequest, err);
            }
        }
    }
    {
        const auto it = body.find("screen_index");
        if(it != body.end())
        {
            if(input != "screen")
            {
                return IpcReply::Fail(ErrCode::BadRequest,
                    "screen_index only applies to the "
                    "\"screen\" input");
            }
            if(!it->is_number_integer())
            {
                return IpcReply::Fail(ErrCode::BadRequest,
                    "screen_index must be an integer");
            }
            any = true;
            if(!deps_.reactive->SetScreenIndex(
                   (int)it->get<int64_t>(), err))
            {
                return IpcReply::Fail(ErrCode::BadRequest, err);
            }
        }
    }
    if(!any)
    {
        return IpcReply::Fail(ErrCode::BadRequest,
            "set_params carries no parameter (sensitivity | "
            "screen_index)");
    }
    return IpcReply::Success(MsgType::Ok, ReactiveJson());
}

void EngineService::PollEvents(TimePoint now)
{
    std::vector<IpcEvent> pending;

    const nlohmann::json conn = ConnectionJson();
    if(conn.dump() != last_connection_sig_)
    {
        last_connection_sig_ = conn.dump();
        pending.push_back(IpcEvent{ event_kind::kConnection, conn });
    }

    /* Topology is structure, not pixels: the diff signature
       excludes per-frame `colors` — with them, any animated
       frame flipped the signature and a full color-carrying
       snapshot pushed every poll tick (BUG-5). The emitted
       payload still carries colors so consumers get a fresh
       frame on real changes; per-frame colors stream via
       frame_status regardless. */
    const nlohmann::json sig =
        SnapshotsJson(/*include_colors*/ false);
    if(sig.dump() != last_topology_sig_)
    {
        last_topology_sig_ = sig.dump();
        pending.push_back(IpcEvent{ event_kind::kTopology,
                                    SnapshotsJson() });
    }

    const nlohmann::json play = PlaybackJson();
    if(play.dump() != last_playback_sig_)
    {
        last_playback_sig_ = play.dump();
        pending.push_back(IpcEvent{ event_kind::kPlayback, play });
    }

    /* reactive: status strings shift on their own (a source
       reinitializing, a display disappearing) — diff-driven
       like the others so subscribers see them without a
       command round-trip. */
    const nlohmann::json react = ReactiveJson();
    if(react.dump() != last_reactive_sig_)
    {
        last_reactive_sig_ = react.dump();
        pending.push_back(IpcEvent{ event_kind::kReactive, react });
    }

    for(const IpcEvent& ev : pending)
    {
        Emit(ev.kind, ev.data);
    }

    /* Idle workspace transfers expire on the owner tick too —
       the lazy sweep inside each WORKSPACE_COMMAND is the
       primary expiry path; this keeps a truly abandoned
       upload from waiting for the next command. */
    if(ws_commands_)
    {
        ws_commands_->SweepExpired();
    }

    /* frame_status emits last — the same ordering it had as
       the final entry of the pending batch. */
    PollFrameStatus(now);
}

void EngineService::PollFrameStatus(TimePoint now)
{
    /* Bounded rate, only when the aggregate counters
       actually moved. */
    const DeliveryStats agg =
        deps_.pipeline ? deps_.pipeline->Stats() : DeliveryStats{};
    std::ostringstream sig;
    sig << agg.submitted << '/' << agg.replaced_pending << '/'
        << agg.transport_completed << '/' << agg.suppressed << '/'
        << agg.transport_rejected;
    if(sig.str() != last_stats_sig_)
    {
        last_stats_sig_ = sig.str();
        const uint64_t since_last = (uint64_t)
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_frame_status_).count();
        if(last_frame_status_ == TimePoint{} ||
           since_last >= options_.frame_status_min_interval_ms)
        {
            last_frame_status_ = now;
            Emit(event_kind::kFrameStatus,
                 FrameStatusJson(LayoutSnapshots()));
        }
        else
        {
            /* Not silently lost — the next poll inside the
               window re-checks; the signature stays consumed
               so the burst coalesces into the next push. */
            last_stats_sig_.clear();
        }
    }

    /* Task 7 draft previews ride the same fast tick; the
       per-session 30 Hz cap lives inside PollPreviews. */
    if(ws_commands_)
    {
        ws_commands_->PollPreviews(now);
    }
}

} /* namespace ipc */
} /* namespace openrgb2 */
