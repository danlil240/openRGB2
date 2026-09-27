// SPDX-License-Identifier: GPL-2.0-or-later
//
// REACTIVE_COMMAND service tests (Task 4.6 —
// src/ipc/EngineService.h): a real EngineService over the
// injected-fake ReactiveInputs manager, direct Dispatch
// calls like ipc_engine_service_test. Covers the verb
// surface (set_enabled / set_params bounds), the honest
// Unavailable path (no source, failed start, unwired
// manager), the reactive surface in STATE/DIAGNOSTICS,
// the "reactive" event snapshot + emission, and the
// live-only playback reason a reactive scene reports
// through the shared capability path.

#include "EngineService.h"
#include "IpcProtocol.h"

#include "DeliveryPipeline.h"
#include "DeviceMapper.h"
#include "Engine.h"
#include "FrameSink.h"
#include "PlaybackController.h"
#include "ReactiveInputs.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace openrgb2;
using namespace openrgb2::ipc;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

class FakeSource final : public IReactiveSource
{
public:
    bool Start(studio::InputBus* b) override
    {
        ++starts;
        bus = b;
        if(fail)
        {
            status = name + ": " + fail_reason;
            return false;
        }
        running = true;
        status  = name + ": listening";
        return true;
    }
    void        Stop() override              { ++stops; running = false; }
    bool        Running() const override     { return running; }
    std::string Status() const override      { return status; }
    void        SetSensitivity(float s) override { sens = s; }
    void        SetScreenIndex(int i) override   { index = i; }

    std::string       name    = "fake";
    std::string       status  = "fake: off";
    std::string       fail_reason = "start failed";
    studio::InputBus* bus     = nullptr;
    bool   running = false, fail = false;
    float  sens  = 1.0f;
    int    index = 0;
    int    starts = 0, stops = 0;
};

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    Engine           engine;
    DeliveryPipeline pipeline;
    DeviceMapper     mapper;

    /* Reactive manager with fake sources + two displays. */
    std::vector<FakeSource*> fakes;
    ReactiveInputs::Deps     rdeps;
    rdeps.now = [] { return 0.0; };
    rdeps.source_factory =
        [&fakes](const std::string& id)
            -> std::unique_ptr<IReactiveSource>
        {
            auto s = std::make_unique<FakeSource>();
            s->name = id;
            fakes.push_back(s.get());
            return s;
        };
    rdeps.enumerate_displays = []
    {
        return std::vector<ReactiveDisplay>{
            { 0, "DISPLAY1", 1920, 1080 },
            { 1, "DISPLAY2", 2560, 1440 } };
    };
    ReactiveInputs reactive(std::move(rdeps));

    /* A REAL PlaybackController — the scene classifier is the
       one capability/reason authority; the service provides
       the applied scene itself (SetSceneProvider). Only the
       clock dep is needed: a reactive scene's animation
       request is refused by classification before any
       transport dep is touched. */
    PlaybackController::Deps pdeps;
    pdeps.now = [] { return Clock::now(); };
    PlaybackController playback(std::move(pdeps));

    EngineService::Deps deps;
    deps.pipeline = &pipeline;
    deps.mapper   = &mapper;
    deps.reactive = &reactive;
    deps.playback = &playback;
    EngineService svc(engine, deps);

    std::vector<IpcEvent> events;
    svc.SetEventSink(
        [&events](const IpcEvent& ev) { events.push_back(ev); });

    /*---- verb surface: bounded validation first ------------*/
    {
        IpcReply r = svc.Dispatch(MsgType::ReactiveCommand,
                                  nlohmann::json::object());
        CHECK(r.code == ErrCode::BadRequest);          /* no verb */
        r = svc.Dispatch(MsgType::ReactiveCommand,
            nlohmann::json::object({{"verb", 5}}));
        CHECK(r.code == ErrCode::BadRequest);   /* wrong type */
        r = svc.Dispatch(MsgType::ReactiveCommand,
            nlohmann::json::object({{"verb", "bogus"}}));
        CHECK(r.code == ErrCode::BadRequest);   /* unknown verb */

        r = svc.Dispatch(MsgType::ReactiveCommand,
            nlohmann::json::object({{"verb", "set_enabled"}}));
        CHECK(r.code == ErrCode::BadRequest);   /* no input */
        r = svc.Dispatch(MsgType::ReactiveCommand,
            nlohmann::json::object(
                {{"verb", "set_enabled"}, {"input", "laser"}}));
        CHECK(r.code == ErrCode::BadRequest);   /* bad input */
        r = svc.Dispatch(MsgType::ReactiveCommand,
            nlohmann::json::object(
                {{"verb", "set_enabled"}, {"input", "audio"}}));
        CHECK(r.code == ErrCode::BadRequest);   /* no enabled */
        r = svc.Dispatch(MsgType::ReactiveCommand,
            nlohmann::json::object({{"verb", "set_enabled"},
                {"input", "audio"}, {"enabled", "yes"}}));
        CHECK(r.code == ErrCode::BadRequest);   /* wrong type */

        /* set_params: no parameter at all is bad_request. */
        r = svc.Dispatch(MsgType::ReactiveCommand,
            nlohmann::json::object({{"verb", "set_params"},
                {"input", "audio"}}));
        CHECK(r.code == ErrCode::BadRequest);
        /* sensitivity on the wrong input is bad_request —
           never silently ignored. */
        r = svc.Dispatch(MsgType::ReactiveCommand,
            nlohmann::json::object({{"verb", "set_params"},
                {"input", "key"}, {"sensitivity", 1.2}}));
        CHECK(r.code == ErrCode::BadRequest);
        /* screen_index on a non-screen input is bad_request. */
        r = svc.Dispatch(MsgType::ReactiveCommand,
            nlohmann::json::object({{"verb", "set_params"},
                {"input", "audio"}, {"screen_index", 0}}));
        CHECK(r.code == ErrCode::BadRequest);
        /* out-of-range display index. */
        r = svc.Dispatch(MsgType::ReactiveCommand,
            nlohmann::json::object({{"verb", "set_params"},
                {"input", "screen"}, {"screen_index", 9}}));
        CHECK(r.code == ErrCode::BadRequest);
    }

    /*---- enable/disable round-trip --------------------------*/
    {
        events.clear();
        IpcReply r = svc.Dispatch(MsgType::ReactiveCommand,
            nlohmann::json::object({{"verb", "set_enabled"},
                {"input", "audio"}, {"enabled", true}}));
        CHECK(r.code == ErrCode::Ok);
        /* The reply IS the full reactive body — the client
           lands on real state, not an assumed toggle. */
        CHECK(r.body["available"].get<bool>() == true);
        CHECK(r.body["inputs"].is_array());
        bool saw_audio = false;
        for(const nlohmann::json& in : r.body["inputs"])
        {
            if(in["id"].get<std::string>() == "audio")
            {
                saw_audio = true;
                CHECK(in["enabled"].get<bool>() == true);
                CHECK(in["running"].get<bool>() == true);
                CHECK(in["available"].get<bool>() == true);
            }
        }
        CHECK(saw_audio);
        CHECK(fakes[0]->starts == 1);
        /* the reactive event fired for other subscribers */
        CHECK(!events.empty());
        CHECK(events.back().kind == event_kind::kReactive);

        /* Params reach the running source. */
        r = svc.Dispatch(MsgType::ReactiveCommand,
            nlohmann::json::object({{"verb", "set_params"},
                {"input", "audio"}, {"sensitivity", 1.5}}));
        CHECK(r.code == ErrCode::Ok);
        CHECK(fakes[0]->sens == 1.5f);
        r = svc.Dispatch(MsgType::ReactiveCommand,
            nlohmann::json::object({{"verb", "set_params"},
                {"input", "screen"}, {"screen_index", 1}}));
        CHECK(r.code == ErrCode::Ok);

        /* Display list rides the same body. */
        CHECK(r.body["displays"].size() == 2);
        CHECK(r.body["displays"][1]["name"].get<std::string>()
              == "DISPLAY2");

        r = svc.Dispatch(MsgType::ReactiveCommand,
            nlohmann::json::object({{"verb", "set_enabled"},
                {"input", "audio"}, {"enabled", false}}));
        CHECK(r.code == ErrCode::Ok);
        CHECK(fakes[0]->stops == 1);
        for(const nlohmann::json& in : r.body["inputs"])
        {
            if(in["id"].get<std::string>() == "audio")
            {
                CHECK(in["enabled"].get<bool>() == false);
                CHECK(in["running"].get<bool>() == false);
            }
        }
    }

    /*---- honest Unavailable: failed start --------------------*/
    {
        fakes[2]->fail        = true;             /* key */
        fakes[2]->fail_reason = "hook install failed";
        IpcReply r = svc.Dispatch(MsgType::ReactiveCommand,
            nlohmann::json::object({{"verb", "set_enabled"},
                {"input", "key"}, {"enabled", true}}));
        CHECK(r.code == ErrCode::Unavailable);
        CHECK(r.message.find("hook install failed") !=
              std::string::npos);
        /* the slot stays disabled — Status is the record */
        const std::vector<ReactiveInputStatus> st =
            reactive.Status();
        CHECK(!st[2].enabled);
        CHECK(st[2].status == "key: hook install failed");
    }

    /*---- STATE + snapshot event + pause flag ------------------*/
    {
        IpcReply r = svc.Dispatch(MsgType::GetState,
                                  nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.body.contains("reactive"));
        CHECK(r.body["reactive"]["available"].get<bool>());
        CHECK(r.body["reactive"]["capture_paused"].get<bool>()
              == false);
        CHECK(r.body["reactive"]["inputs"].size() == 3);

        r = svc.Dispatch(MsgType::DiagGet,
                         nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.body.contains("reactive"));
        CHECK(r.body["reactive"]["inputs"].size() == 3);

        /* SnapshotEvent serves the initial push for a fresh
           subscriber — same body as the pull surface. */
        nlohmann::json snap;
        CHECK(svc.SnapshotEvent(event_kind::kReactive, snap));
        CHECK(snap["available"].get<bool>() == true);
        CHECK(snap["inputs"].size() == 3);

        /* capture_paused reaches the surface honestly. */
        reactive.SetPaused(true);
        r = svc.Dispatch(MsgType::GetState,
                         nlohmann::json::object());
        CHECK(r.body["reactive"]["capture_paused"].get<bool>());
        for(const nlohmann::json& in :
                r.body["reactive"]["inputs"])
        {
            CHECK(in["paused"].get<bool>() == true);
        }
        reactive.SetPaused(false);
    }

    /*---- no manager: the whole surface is honest absence ------*/
    {
        EngineService::Deps d2;
        d2.pipeline = &pipeline;
        d2.mapper   = &mapper;               /* reactive: null */
        EngineService svc2(engine, d2);

        IpcReply r = svc2.Dispatch(MsgType::ReactiveCommand,
            nlohmann::json::object({{"verb", "set_enabled"},
                {"input", "audio"}, {"enabled", true}}));
        CHECK(r.code == ErrCode::Unavailable);

        r = svc2.Dispatch(MsgType::GetState,
                          nlohmann::json::object());
        CHECK(r.body["reactive"]["available"].get<bool>()
              == false);
        CHECK(r.body["reactive"]["inputs"].empty());
        CHECK(r.body["reactive"]["reason"].get<std::string>()
                  .find("not wired") != std::string::npos);
    }

    /*---- reactive scene -> honest live-only reason ------------*/
    {
        /* An inline `level` layer is a reactive scene: the
           shared apply path classifies it — animation is
           refused through the same capability/reason channel
           the UI mirrors. */
        const nlohmann::json doc = nlohmann::json::object({
            { "version", 2 },
            { "name",    "reactive-scene" },
            { "effect",  nlohmann::json::object({
                { "layers", nlohmann::json::array({
                    nlohmann::json::object(
                        {{"primitive", "level"}}) }) } }) },
        });
        IpcReply r = svc.Dispatch(MsgType::SetScene,
            nlohmann::json::object({{"document", doc}}));
        CHECK(r.code == ErrCode::Ok);

        r = svc.Dispatch(MsgType::GetPlayback,
                         nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.body["capabilities"]["animation_playback"]
                  .get<bool>() == false);
        CHECK(r.body["unavailable_reason"].is_string());
        CHECK(r.body["unavailable_reason"].get<std::string>()
                  .find("audio") != std::string::npos);
        CHECK(r.body["unavailable_reason"].get<std::string>()
                  .find("live") != std::string::npos);

        /* ...and the mode request itself is refused with the
           honest code + the scene-classified reason, never a
           silent no-op. */
        r = svc.Dispatch(MsgType::SetPlaybackMode,
            nlohmann::json::object(
                {{"mode", "animation_playback"}}));
        CHECK(r.code == ErrCode::Unavailable);
        CHECK(r.message.find("audio") != std::string::npos);
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
