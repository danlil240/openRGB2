/*---------------------------------------------------------*\
|| EngineService.cpp                                         ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "EngineService.h"

#include "scene/SceneJson.h"

#include <cstdio>
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
        k.device   = j.value("device",   std::string());
        k.serial   = j.value("serial",   std::string());
        k.location = j.value("location", std::string());
        k.zone     = j.value("zone",     std::string());
    }
    return k;
}

std::string ColorHex(RgbColor c)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", c.r, c.g, c.b);
    return buf;
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

nlohmann::json SnapshotToJson(const DeviceSnapshot& snap,
                              bool include_colors)
{
    nlohmann::json zones = nlohmann::json::array();
    for(const ZoneSnapshot& z : snap.zones)
    {
        zones.push_back(nlohmann::json::object({
            { "name",      z.name      },
            { "led_count", z.led_count },
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

/* Layout/calibration verbs reserved for Milestone 4 (plan
   section 5, Layout view). Recognized now, honestly
   Unavailable — unknown verbs are BadRequest. */
bool IsKnownLayoutVerb(const std::string& verb)
{
    static const char* kVerbs[] = {
        "identify",          /* flash a zone for LED identification */
        "set_orientation",   /* rotate an object's emitter mapping  */
        "set_mirror",        /* link/mirror objects (linked fans)   */
        "calibrate_led",     /* LED address calibration             */
        "set_binding",       /* re-bind a scene binding             */
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
}

void EngineService::SetEventSink(
    std::function<void(const IpcEvent&)> sink)
{
    std::lock_guard<std::mutex> lock(mutex_);
    event_sink_ = std::move(sink);
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

    /* Resolve the effect stack the persisted document intends:
       the inline layer stack when present, otherwise the named
       preset through the effect-look registry (same contract
       as the --scene CLI path). */
    std::vector<studio::EffectLayer> layers = doc->effect.layers;
    if(layers.empty() && !doc->effect.preset.empty())
    {
        if(!studio::FindPreset(doc->effect.preset))
        {
            err = "unknown preset '" + doc->effect.preset + "'";
            return false;
        }
        layers = studio::BuildPreset(doc->effect.preset,
                                     doc->effect.seed);
        studio::ApplyGlobalParams(layers, doc->effect.speed,
                                  doc->effect.intensity);
    }

    uint64_t revision;
    {
        /* The whole apply is serialized: revision, worker
           submission, mapper re-bind, and the stored document
           move together — two concurrent SET_SCENEs can't
           interleave a stale document over a newer revision. */
        std::lock_guard<std::mutex> lock(mutex_);
        revision = ++scene_revision_;
        engine_.Worker().SubmitScene(doc, std::move(layers),
                                     revision);
        if(deps_.mapper)
        {
            deps_.mapper->SetScene(doc);
        }
        scene_ = doc;
    }
    Emit(event_kind::kScene, nlohmann::json::object({
        { "revision", revision  },
        { "name",     doc->name },
    }));
    return true;
}

uint64_t EngineService::SceneRevision() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return scene_revision_;
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
    return out;
}

nlohmann::json EngineService::PlaybackJson() const
{
    PlaybackCapabilities caps;
    caps.live_effects       = true;
    caps.animation_playback = false;
    if(deps_.playback_caps)
    {
        caps = deps_.playback_caps();
    }
    PlaybackMode requested;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        requested = requested_mode_;
    }
    return nlohmann::json::object({
        { "requested_mode", PlaybackModeName(requested) },
        { "capabilities", nlohmann::json::object({
            { "live_effects",       caps.live_effects       },
            { "animation_playback", caps.animation_playback },
        })},
        { "status", PlaybackStatusToJson(engine_.Status()) },
    });
}

nlohmann::json EngineService::ConnectionJson() const
{
    nlohmann::json out = nlohmann::json::object({
        { "sdk_present", deps_.sdk != nullptr },
    });
    if(deps_.sdk)
    {
        out["state"]             = ConnStateName(deps_.sdk->State());
        out["protocol_version"]  = deps_.sdk->ProtocolVersion();
        out["reconnect_attempt"] = deps_.sdk->ReconnectAttempt();
        out["last_error"]        = deps_.sdk->LastError();
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

nlohmann::json EngineService::SnapshotsJson() const
{
    nlohmann::json snaps = nlohmann::json::array();
    if(deps_.sdk)
    {
        for(const DeviceSnapshot& snap : deps_.sdk->Snapshots())
        {
            snaps.push_back(SnapshotToJson(snap, /*colors*/ true));
        }
    }
    return nlohmann::json::object({
        { "snapshots",  std::move(snaps) },
        { "connection", ConnectionJson() },
    });
}

nlohmann::json EngineService::DeliveryStatsJson(
    const nlohmann::json& body) const
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
    if(deps_.sdk)
    {
        for(const DeviceSnapshot& snap : deps_.sdk->Snapshots())
        {
            DeliveryStats stats;
            if(deps_.pipeline->Stats(snap.key, stats))
            {
                nlohmann::json c;
                c["key"]                 = KeyToJson(snap.key);
                c["stats"]               = StatsToJson(stats);
                c["pending"]             =
                    deps_.pipeline->PendingCount(snap.key);
                c["colors"]              = nlohmann::json::array();
                RgbFrame delivered;
                Clock::duration budget;
                if(deps_.pipeline->LastDelivered(snap.key,
                                                 delivered, &budget))
                {
                    c["colors"] = ColorsToJson(delivered.colors);
                    c["budget_ms"] =
                        std::chrono::duration_cast<
                            std::chrono::milliseconds>(budget)
                            .count();
                }
                controllers.push_back(std::move(c));
            }
        }
    }
    return nlohmann::json::object({
        { "available",   true                                       },
        { "aggregate",   StatsToJson(deps_.pipeline->Stats())       },
        { "pending",     deps_.pipeline->PendingCount()             },
        { "controllers", std::move(controllers)                     },
    });
}

nlohmann::json EngineService::FrameStatusJson() const
{
    /* The delivered-state feed for the Dashboard preview: what
       the transport last accepted per controller (plan
       section 5 — preview and output share the same sampled
       colors; preview FPS is labelled separately). */
    nlohmann::json controllers = nlohmann::json::array();
    if(deps_.pipeline && deps_.sdk)
    {
        for(const DeviceSnapshot& snap : deps_.sdk->Snapshots())
        {
            RgbFrame delivered;
            if(deps_.pipeline->LastDelivered(snap.key, delivered))
            {
                DeliveryStats stats;
                deps_.pipeline->Stats(snap.key, stats);
                controllers.push_back(nlohmann::json::object({
                    { "key",          KeyToJson(snap.key)          },
                    { "sequence",     delivered.sequence           },
                    { "colors",       ColorsToJson(delivered.colors)},
                    { "stats",        StatsToJson(stats)           },
                }));
            }
        }
    }
    return nlohmann::json::object({
        { "aggregate",   deps_.pipeline
                             ? StatsToJson(deps_.pipeline->Stats())
                             : StatsToJson(DeliveryStats{}) },
        { "controllers", std::move(controllers)                   },
    });
}

nlohmann::json EngineService::StateJson() const
{
    nlohmann::json ipc_stats = nullptr;
    if(deps_.server_stats)
    {
        const IpcServerStats st = deps_.server_stats();
        ipc_stats = nlohmann::json::object({
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
    return nlohmann::json::object({
        { "engine", nlohmann::json::object({
            { "running",         engine_.Running()          },
            { "scene_clock_hz",  SceneWorker::kTickHz       },
            { "scene_sequence",  engine_.Worker().Sequence()},
        })},
        { "scene",      SceneJsonLocked()                    },
        { "sdk",        ConnectionJson()                     },
        { "playback",   PlaybackJson()                       },
        { "ipc",        ipc_stats                            },
    });
}

nlohmann::json EngineService::DiagnosticsJson(TimePoint now) const
{
    const double uptime_s =
        std::chrono::duration<double>(now - started_).count();
    const double steady_ms =
        std::chrono::duration<double, std::milli>(
            now.time_since_epoch()).count();

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
                                  nlohmann::json::object())  },
        { "sdk",              ConnectionJson()               },
        { "ipc",              deps_.server_stats
                                  ? StateJson()["ipc"]
                                  : nlohmann::json(nullptr)  },
        { "clock", nlohmann::json::object({
            { "steady_ms", steady_ms },
        })},
        /* The backend's own read-only counters pipe is a
           separate plan item; honest null until it exists. */
        { "backend_counters", nullptr                        },
    });
}

/*---------------------------------------------------------*\
|| IIpcService                                               ||
\*---------------------------------------------------------*/

nlohmann::json EngineService::OnWelcome()
{
    return nlohmann::json::object({
        { "engine",    "openrgb2-engine" },
        { "running",   engine_.Running() },
        { "state",     StateJson()       },
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
        out = FrameStatusJson();
        return true;
    }
    return false;
}

IpcReply EngineService::Dispatch(MsgType type,
                                 const nlohmann::json& body)
{
    switch(type)
    {
    case MsgType::GetSnapshots:
        return IpcReply::Success(MsgType::Snapshots,
                                 SnapshotsJson());

    case MsgType::GetScene:
        return IpcReply::Success(MsgType::Scene,
                                 SceneJsonLocked());

    case MsgType::SetScene:
    {
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
        /* Honest: the request is recorded; applying it to
           hardware is Task 3.6 — status.mode is the APPLIED
           state and only moves on acknowledgement. */
        return IpcReply::Success(MsgType::Ok,
            nlohmann::json::object({
                { "requested_mode", PlaybackModeName(mode) },
                { "applied",        false                  },
                { "pending",        true                   },
            }));
    }

    case MsgType::GetDeliveryStats:
        return IpcReply::Success(MsgType::DeliveryStats,
                                 DeliveryStatsJson(body));

    case MsgType::GetState:
        return IpcReply::Success(MsgType::State, StateJson());

    case MsgType::GetPresets:
    {
        nlohmann::json presets = nlohmann::json::array();
        for(const studio::PresetInfo& p : studio::PresetList())
        {
            presets.push_back(nlohmann::json::object({
                { "id",          p.id          },
                { "name",        p.name        },
                { "description", p.description },
                { "needs",       p.needs       },
            }));
        }
        return IpcReply::Success(MsgType::Presets,
            nlohmann::json::object({{ "presets", presets }}));
    }

    case MsgType::LayoutCommand:
    {
        const std::string verb =
            body.is_object()
                ? body.value("verb", std::string())
                : std::string();
        if(verb.empty())
        {
            return IpcReply::Fail(ErrCode::BadRequest,
                                  "LAYOUT_COMMAND requires {verb}");
        }
        if(!IsKnownLayoutVerb(verb))
        {
            return IpcReply::Fail(ErrCode::BadRequest,
                                  "unknown layout verb '" + verb + "'");
        }
        return IpcReply::Fail(ErrCode::Unavailable,
            "layout/calibration verb '" + verb +
            "' is reserved for the Milestone-4 layout engine");
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

void EngineService::PollEvents(TimePoint now)
{
    std::vector<IpcEvent> pending;

    const nlohmann::json conn = ConnectionJson();
    if(conn.dump() != last_connection_sig_)
    {
        last_connection_sig_ = conn.dump();
        pending.push_back(IpcEvent{ event_kind::kConnection, conn });
    }

    const nlohmann::json snaps = SnapshotsJson();
    if(snaps.dump() != last_topology_sig_)
    {
        last_topology_sig_ = snaps.dump();
        pending.push_back(IpcEvent{ event_kind::kTopology, snaps });
    }

    const nlohmann::json play = PlaybackJson();
    if(play.dump() != last_playback_sig_)
    {
        last_playback_sig_ = play.dump();
        pending.push_back(IpcEvent{ event_kind::kPlayback, play });
    }

    /* frame_status: bounded rate, only when the aggregate
       counters actually moved. */
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
            pending.push_back(
                IpcEvent{ event_kind::kFrameStatus,
                          FrameStatusJson() });
        }
        else
        {
            /* Not silently lost — the next poll inside the
               window re-checks; the signature stays consumed
               so the burst coalesces into the next push. */
            last_stats_sig_.clear();
        }
    }

    for(const IpcEvent& ev : pending)
    {
        Emit(ev.kind, ev.data);
    }
}

} /* namespace ipc */
} /* namespace openrgb2 */
