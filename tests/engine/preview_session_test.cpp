// SPDX-License-Identifier: GPL-2.0-or-later
//
// PreviewSession + IPC preview surface tests (studio-import
// Task 7, plan section 3.4): a bounded draft preview that
// evaluates the SAME prepared scene/evaluator/brightness/
// timeline/captured-input path as output — with NO IFrameSink,
// no SDK reference, and no physical frame production.
//
// Covers the brief's acceptance list:
//   - deterministic color/brightness equivalence with engine
//     evaluation (SceneWorker + the DeviceMapper scale)
//   - unbound objects appear in the draft preview
//   - physical frame counters (worker sequence) untouched
//   - stale candidate replacement (latest draft wins)
//   - two-session global bound + one preview per session
//   - disabled inputs stay empty; preview never starts capture
//   - hidden view + display-off pause; disconnect releases
//   - chunked visible-object color data below the IPC cap
//   - reduced-detail labeling above the display budget

#include "PreviewSession.h"

#include "Engine.h"
#include "EngineService.h"
#include "IpcServer.h"
#include "PowerLifecycle.h"
#include "ReactiveInputs.h"
#include "SceneWorker.h"
#include "StudioScenePreparation.h"
#include "WorkspaceService.h"

#include "config/StudioConfig.h"
#include "config/WorkspaceCandidate.h"
#include "presets/PresetRegistry.h"
#include "presets/EffectRegistry.h"

#include "fake_ipc_channel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace openrgb2;
using namespace openrgb2::ipc;
using namespace ipc_test;
using nlohmann::json;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

namespace fs = std::filesystem;

static fs::path g_tmp;

/*---------------------------------------------------------*\
|| Fixtures                                                 ||
\*---------------------------------------------------------*/

/* One device type on disk so compact documents resolve. */
static void WriteTestType(const fs::path& dir, int leds)
{
    fs::create_directories(dir);
    const json ent = json::object({
        { "geometry", "fan_body" },
        { "zone",     "ring"     },
    });
    const json doc = json::object({
        { "schema_version", 1          },
        { "id",             "test-fan" },
        { "name",           "Test Fan" },
        { "category",       "fan"      },
        { "entities", json::object({{ "body", ent }}) },
        { "zones", json::array({ json::object({
              { "id",        "ring" },
              { "entity",    "body" },
              { "led_count", leds   },
              { "layout", json::object({
                    { "type",     "ring" },
                    { "radius_m", 0.05  } }) } }) }) },
    });
    std::ofstream(dir / "test-fan.device.json") << doc.dump(2);
}

/* Compact v3 document: `fans` devices bound to "Test
   Controller" zones; a static red inline layer paints every
   device object; `unbound_extra` adds a device object with NO
   binding — it must still preview. */
static json CompactDoc(int fans, int unbound_extra,
                       double brightness, size_t pad = 0)
{
    json devices = json::object();
    json settings = json::object();
    json colors = json::object();
    for(int i = 0; i < fans; ++i)
    {
        const std::string id = "fan" + std::to_string(i);
        devices[id] = json::object({
            { "type", "test-fan" },
            { "x", 0.1 * i }, { "y", 0.0 }, { "z", 0.0 },
            { "rx", 0.0 }, { "ry", 0.0 }, { "rz", 0.0 } });
        settings[id] = json::object({
            { "zones", json::object({
                  { "ring", json::object({
                        { "binding",   "b1"  },
                        { "addr_base", 0     },
                        { "verified",  true  } }) } }) } });
        colors[id] = "#804020";
    }
    for(int i = 0; i < unbound_extra; ++i)
    {
        const std::string id = "free" + std::to_string(i);
        devices[id] = json::object({
            { "type", "test-fan" },
            { "x", 0.5 + 0.1 * i }, { "y", 0.0 }, { "z", 0.4 } });
        /* No device_settings entry: never bound, never
           verified — draft preview must still show it. */
        colors[id] = "#0040C0";
    }
    json doc = json::object({
        { "schema_version", 3 },
        { "name",           "preview-doc" },
        { "devices",        devices },
        { "bindings", json::object({
              { "b1", json::object({
                    { "controller_name", "Test Controller" },
                    { "zone_name",       "ring"            },
                    { "zone_leds",       8                 } }) } }) },
        { "device_settings", settings },
        { "colors", json::object({
              { "objects",  colors },
              { "emitters", json::object() } }) },
        { "output", json::object({{ "brightness", brightness }}) },
        { "effects", json::object({
              { "preset", "" }, { "seed", 0 },
              { "speed", 1.0 }, { "intensity", 1.0 },
              { "playing", true },
              { "layers", json::array({
                    json::object({
                        { "primitive", "static" },
                        { "space",     "world"  },
                        { "blend",     "replace"},
                        { "opacity",   1.0      },
                        { "palette", json::array({
                              json::object({
                                  { "pos", 0.0 },
                                  { "color", "#FF2000" } }) }) } }) }) } }) },
    });
    if(pad)
    {
        doc["extensions"] = json::object(
            {{ "pad", std::string(pad, 'p') }});
    }
    return doc;
}

static std::shared_ptr<const WorkspaceCandidate> ParseCand(
    const json& doc, const studio::PresetRegistry& devices,
    const studio::EffectRegistry& effects)
{
    auto cand = std::make_shared<WorkspaceCandidate>();
    std::vector<std::string> errors, warnings;
    CHECK(ParseWorkspaceCandidate(doc, devices, effects,
                                  *cand, errors, warnings));
    if(!errors.empty())
    {
        std::printf("  parse errors: %s\n", errors.front().c_str());
    }
    return cand;
}

static DeviceSnapshot TestSnapshot(int leds = 8)
{
    DeviceSnapshot snap;
    snap.name       = "Test Controller";
    snap.vendor     = "Test";
    snap.key.device = "Test:Test Controller";
    snap.key.serial = "C1";
    snap.key.location = "usb:c1";
    snap.connection = ConnectionState::Connected;
    ZoneSnapshot z;
    z.name = "ring";
    z.led_count = (uint32_t)leds;
    z.leds_min  = (uint32_t)leds;
    z.leds_max  = (uint32_t)leds;
    snap.zones = { z };
    return snap;
}

/* The DeviceMapper brightness scale — replicated so the test
   asserts preview output against the DOCUMENTED output math,
   not against preview's own implementation. */
static RgbColor RefScale(RgbColor c, float brightness)
{
    RgbColor out;
    out.r = uint8_t((unsigned)(c.r * brightness) & 0xFFu);
    out.g = uint8_t((unsigned)(c.g * brightness) & 0xFFu);
    out.b = uint8_t((unsigned)(c.b * brightness) & 0xFFu);
    return out;
}

/* Reference colors for one object at scene time t through the
   production path: EffectEngine over the prepared document,
   painted-color fallback, brightness scale — exactly what
   DeviceMapper::MapFrame writes to the lane. */
static std::vector<RgbColor> RefObjectColors(
    const PreparedStudioScene& p, const std::string& object_id,
    double t, const studio::InputState* input)
{
    studio::EffectEngine eng;
    eng.SetLayers(p.layers);
    studio::FrameColors eval;
    eng.Evaluate(*p.doc, t, eval, input);
    const studio::SceneObject* obj =
        studio::FindObject(*p.doc, object_id);
    if(!obj)
    {
        return {};
    }
    /* Linked objects preview their owner's frame. */
    const studio::SceneObject* owner =
        studio::OutputOwner(*p.doc, object_id);
    const auto fit = eval.find(owner ? owner->id : object_id);
    std::vector<RgbColor> out;
    for(size_t e = 0; e < obj->emitters.size(); ++e)
    {
        studio::SceneColor c =
            studio::EmitterColor(*p.doc, object_id, (int)e);
        if(fit != eval.end() && e < fit->second.size())
        {
            c = fit->second[e];
        }
        out.push_back(RefScale(FromSceneColor(c), p.doc->brightness));
    }
    return out;
}

/*---------------------------------------------------------*\
|| Unit: PreviewSession                                     ||
\*---------------------------------------------------------*/

static void TestDeterministicEquivalence()
{
    std::printf("-- unit: deterministic color/brightness equivalence\n");
    studio::PresetRegistry dev_reg;
    std::vector<std::string> errs;
    dev_reg.LoadDirectory((g_tmp / "types").string(), &errs);
    studio::EffectRegistry eff_reg;

    auto cand = ParseCand(CompactDoc(2, 1, 0.5), dev_reg, eff_reg);
    const std::vector<DeviceSnapshot> snaps = { TestSnapshot() };

    /* Drive the production worker on the SAME prepared scene. */
    PreparedStudioScene prepared;
    std::string err;
    CHECK(PrepareStudioScene(*cand, snaps, prepared, err));
    SceneWorker worker;
    const TimePoint epoch = Clock::now();
    worker.SetEpoch(epoch);
    EvaluatedFrame eval;
    worker.SetFrameCallback(
        [&eval](const EvaluatedFrame& f) { eval = f; });
    worker.SubmitPreparedScene(prepared, 1);
    const TimePoint tnow = epoch + std::chrono::milliseconds(740);
    CHECK(worker.Step(tnow));
    const double t = std::chrono::duration<double>(tnow - epoch).count();

    /* The preview session evaluates the same instant. */
    PreviewSession::Deps pdeps;
    pdeps.snapshots = [snaps] { return snaps; };
    PreviewSession ps(pdeps);
    CHECK(ps.SetCandidate(cand, 7, err));
    ps.SetVisible(true);
    PreviewFrame frame;
    studio::InputState empty_input;
    CHECK(ps.Sample(t, &empty_input, frame));
    CHECK(frame.valid);
    CHECK(frame.draft_revision == 7);
    CHECK(frame.sequence == 1);
    CHECK(std::fabs(frame.scene_time_s - t) < 1e-9);
    CHECK(!frame.objects.empty());

    /* Every preview object matches the worker's evaluated
       colors after the mapper's brightness scale — the exact
       bytes output would have carried. */
    std::map<std::string, const PreviewObjectColors*> by_id;
    for(const PreviewObjectColors& o : frame.objects)
    {
        by_id[o.object_id] = &o;
    }
    for(const studio::SceneObject& obj : prepared.doc->objects)
    {
        if(obj.kind != studio::ObjectKind::Device ||
           obj.emitters.empty())
        {
            continue;
        }
        auto it = by_id.find(obj.id);
        CHECK(it != by_id.end());
        if(it == by_id.end()) continue;
        const std::vector<RgbColor> want =
            RefObjectColors(prepared, obj.id, t, &empty_input);
        CHECK(it->second->colors == want);
        CHECK(it->second->emitter_total == obj.emitters.size());
        CHECK(it->second->indices.empty());   /* dense frame */
    }
    CHECK(!frame.reduced);
    CHECK(frame.emitter_total == frame.emitter_sent);

    /* Bit-identical across sessions: same inputs -> same
       frame (deterministic, no wall-clock peeking). */
    PreviewSession ps2(pdeps);
    CHECK(ps2.SetCandidate(cand, 7, err));
    ps2.SetVisible(true);
    PreviewFrame frame2;
    CHECK(ps2.Sample(t, &empty_input, frame2));
    CHECK(frame2.objects.size() == frame.objects.size());
    for(size_t i = 0; i < frame.objects.size(); ++i)
    {
        CHECK(frame2.objects[i].colors == frame.objects[i].colors);
    }
}

/* Mirrored devices: the resolver turns a mirror's parts into
   Linked objects with NO emitters of their own (output goes
   through the owner). The draft preview must still paint them
   with the owner's evaluated frame — otherwise only one fan of
   each mirrored set animates in the Layout views. */
static void TestMirroredCopiesPreviewOwner()
{
    std::printf("-- unit: mirrored copies preview the owner's frame\n");
    studio::PresetRegistry dev_reg;
    std::vector<std::string> errs;
    dev_reg.LoadDirectory((g_tmp / "types").string(), &errs);
    studio::EffectRegistry eff_reg;

    json doc = CompactDoc(2, 0, 0.5);
    doc["device_settings"]["fan1"] =
        json::object({{ "mirror_of", "fan0" }});
    auto cand = ParseCand(doc, dev_reg, eff_reg);
    const std::vector<DeviceSnapshot> snaps = { TestSnapshot() };

    PreparedStudioScene prepared;
    std::string err;
    CHECK(PrepareStudioScene(*cand, snaps, prepared, err));

    PreviewSession::Deps pdeps;
    pdeps.snapshots = [snaps] { return snaps; };
    PreviewSession ps(pdeps);
    CHECK(ps.SetCandidate(cand, 1, err));
    PreviewFrame frame;
    studio::InputState empty_input;
    const double t = 0.37;
    CHECK(ps.Sample(t, &empty_input, frame));

    std::map<std::string, const PreviewObjectColors*> by_id;
    for(const PreviewObjectColors& o : frame.objects)
    {
        by_id[o.object_id] = &o;
    }
    int linked = 0;
    for(const studio::SceneObject& obj : prepared.doc->objects)
    {
        if(obj.kind != studio::ObjectKind::Linked)
        {
            continue;
        }
        ++linked;
        const studio::SceneObject* owner =
            studio::OutputOwner(*prepared.doc, obj.id);
        CHECK(owner != nullptr && !owner->emitters.empty());
        if(owner == nullptr) continue;
        auto it = by_id.find(obj.id);
        CHECK(it != by_id.end());
        if(it == by_id.end()) continue;
        CHECK(it->second->kind == "linked");
        CHECK(it->second->emitter_total == owner->emitters.size());
        CHECK(it->second->colors ==
              RefObjectColors(prepared, owner->id, t, &empty_input));
    }
    CHECK(linked > 0);
    CHECK(frame.emitter_total == frame.emitter_sent);
}

static void TestUnboundObjectAndWorkerUntouched()
{
    std::printf("-- unit: unbound object + physical counters untouched\n");
    studio::PresetRegistry dev_reg;
    std::vector<std::string> errs;
    dev_reg.LoadDirectory((g_tmp / "types").string(), &errs);
    studio::EffectRegistry eff_reg;
    auto cand = ParseCand(CompactDoc(1, 1, 1.0), dev_reg, eff_reg);

    Engine engine;             /* the real output path — idle */
    PreviewSession::Deps pdeps;
    const std::vector<DeviceSnapshot> snaps = { TestSnapshot() };
    pdeps.snapshots = [snaps] { return snaps; };
    PreviewSession ps(pdeps);
    std::string err;
    CHECK(ps.SetCandidate(cand, 3, err));
    ps.SetVisible(true);

    PreviewFrame frame;
    CHECK(ps.Sample(1.25, nullptr, frame));
    /* "free0" is never bound and never verified — it still
       shows in the draft preview (resolved ids are
       <instance>/<entity>, e.g. "free0/body"). */
    bool found = false;
    for(const PreviewObjectColors& o : frame.objects)
    {
        if(o.object_id.rfind("free0", 0) == 0)
        {
            found = true;
            CHECK(!o.colors.empty());
        }
    }
    CHECK(found);

    /* No output machinery moved: the worker produced nothing
       and no frames were submitted anywhere. */
    CHECK(engine.Worker().Sequence() == 0);
    CHECK(engine.Worker().AppliedRevision() == 0);
    CHECK(!engine.Worker().HasScene());
}

static void TestDisabledInputsStayEmpty()
{
    std::printf("-- unit: disabled/empty inputs contribute nothing\n");
    studio::PresetRegistry dev_reg;
    std::vector<std::string> errs;
    dev_reg.LoadDirectory((g_tmp / "types").string(), &errs);
    studio::EffectRegistry eff_reg;

    /* A ripple layer responds only to input events; disabled
       inputs -> empty state -> the painted base stands. */
    json doc = CompactDoc(1, 0, 1.0);
    doc["effects"]["layers"] = json::array({
        json::object({
            { "primitive", "ripple" },
            { "space",     "world"  },
            { "opacity",   1.0      },
            { "source",    "key"    },
            { "palette", json::array({
                  json::object({
                      { "pos", 0.0 },
                      { "color", "#00FF00" } }) }) } }) });
    auto cand = ParseCand(doc, dev_reg, eff_reg);
    PreviewSession::Deps pdeps;
    const std::vector<DeviceSnapshot> snaps = { TestSnapshot() };
    pdeps.snapshots = [snaps] { return snaps; };
    PreviewSession ps(pdeps);
    std::string err;
    CHECK(ps.SetCandidate(cand, 1, err));
    ps.SetVisible(true);

    PreviewFrame a, b;
    studio::InputState empty;
    CHECK(ps.Sample(2.0, nullptr, a));
    CHECK(ps.Sample(2.0, &empty, b));
    /* null input and an empty snapshot evaluate identically —
       no source ever starts capturing because a preview asked. */
    CHECK(a.objects[0].colors == b.objects[0].colors);
}

static void TestHiddenPauseAndStaleCandidate()
{
    std::printf("-- unit: hidden pause + stale candidate replacement\n");
    studio::PresetRegistry dev_reg;
    std::vector<std::string> errs;
    dev_reg.LoadDirectory((g_tmp / "types").string(), &errs);
    studio::EffectRegistry eff_reg;
    PreviewSession::Deps pdeps;
    const std::vector<DeviceSnapshot> snaps = { TestSnapshot() };
    pdeps.snapshots = [snaps] { return snaps; };
    PreviewSession ps(pdeps);
    std::string err;

    auto cand1 = ParseCand(CompactDoc(1, 0, 1.0), dev_reg, eff_reg);
    CHECK(ps.SetCandidate(cand1, 5, err));
    ps.SetVisible(true);
    PreviewFrame f;
    CHECK(ps.Sample(0.0, nullptr, f));
    CHECK(f.draft_revision == 5);
    CHECK(f.sequence == 1);

    /* Latest-wins replacement: a newer draft replaces the
       held candidate outright — no queue of unsent drafts. */
    json doc2 = CompactDoc(1, 0, 1.0);
    doc2["name"] = "replaced";
    doc2["effects"]["layers"][0]["palette"][0]["color"] =
        "#00FF00";
    auto cand2 = ParseCand(doc2, dev_reg, eff_reg);
    CHECK(ps.SetCandidate(cand2, 6, err));
    CHECK(ps.Sample(0.0, nullptr, f));
    CHECK(f.draft_revision == 6);
    CHECK(f.sequence == 2);
    /* The new palette reaches the preview immediately. */
    CHECK(f.objects[0].colors[0].g == 0xFF);

    /* Hidden view -> no frames, sequence does not advance. */
    ps.SetVisible(false);
    CHECK(!ps.Sample(0.5, nullptr, f));
    CHECK(!ps.Sample(1.0, nullptr, f));
    ps.SetVisible(true);
    CHECK(ps.Sample(1.5, nullptr, f));
    CHECK(f.sequence == 3);

    /* Stop releases the candidate. */
    ps.Stop();
    CHECK(!ps.Active());
    CHECK(!ps.Sample(2.0, nullptr, f));
}

static void TestReducedLabeling()
{
    std::printf("-- unit: reduced-detail labeling above budget\n");
    studio::PresetRegistry dev_reg;
    std::vector<std::string> errs;
    dev_reg.LoadDirectory((g_tmp / "types").string(), &errs);
    studio::EffectRegistry eff_reg;
    auto cand = ParseCand(CompactDoc(4, 0, 1.0), dev_reg, eff_reg);
    PreviewSession::Deps pdeps;
    const std::vector<DeviceSnapshot> snaps = { TestSnapshot() };
    pdeps.snapshots = [snaps] { return snaps; };
    PreviewSession::Options popts;
    popts.display_budget_leds = 8;   /* 4 objects x 8 = 32 > 8 */
    PreviewSession ps(pdeps, popts);
    std::string err;
    CHECK(ps.SetCandidate(cand, 9, err));
    ps.SetVisible(true);
    PreviewFrame f;
    CHECK(ps.Sample(0.0, nullptr, f));
    CHECK(f.reduced);
    CHECK(f.emitter_total == 32);
    CHECK(f.emitter_sent <= 8 + 4);  /* quota + stride rounding */
    CHECK(f.emitter_sent < f.emitter_total);
    for(const PreviewObjectColors& o : f.objects)
    {
        /* A reduced object honestly advertises which emitters
           its colors belong to — never a truncated array
           pretending to be complete. */
        CHECK(o.indices.size() == o.colors.size());
        CHECK(!o.indices.empty());
        CHECK(o.emitter_total == 8);
    }
    /* Geometry still arrives: every device object is listed. */
    CHECK(f.object_total == 4);
}

/*---------------------------------------------------------*\
|| IPC: preview verbs over the real session machinery       ||
\*---------------------------------------------------------*/

struct Rig
{
    openrgb2::Engine                 engine;
    studio::PresetRegistry           devices;
    WorkspaceService::Deps           wdeps;
    EngineService::Deps              edeps;
    EngineServiceOptions             eopts;
    IpcServerOptions                 sopts;
    PowerLifecycle                   power{ PowerLifecycle::Deps{} };
    std::vector<DeviceSnapshot>      snaps;
    std::unique_ptr<ReactiveInputs>  reactive;
    std::atomic<int>                 sources_started{ 0 };
    std::unique_ptr<WorkspaceService> ws;
    std::unique_ptr<EngineService>    svc;
    std::unique_ptr<IpcServer>        server;

    Rig(const fs::path& root)
    {
        std::vector<std::string> errs;
        devices.LoadDirectory((g_tmp / "types").string(), &errs);
        CHECK(errs.empty());
        wdeps.devices = devices;
        ws = std::make_unique<WorkspaceService>(root.string(),
                                              wdeps);
        edeps.workspace = ws.get();
        edeps.power     = &power;
        snaps = { TestSnapshot() };
        edeps.snapshots = [this] { return snaps; };
        /* A real ReactiveInputs whose source factory counts —
           preview consumes snapshots but must NEVER start a
           capture source. */
        ReactiveInputs::Deps rdeps;
        rdeps.source_factory =
            [this](const std::string&)
                -> std::unique_ptr<IReactiveSource>
            {
                struct CountingSource : IReactiveSource
                {
                    std::atomic<int>* n;
                    bool run = false;
                    explicit CountingSource(std::atomic<int>* n)
                        : n(n) {}
                    bool Start(studio::InputBus*) override
                    { ++*n; run = true; return true; }
                    void Stop() override { run = false; }
                    bool Running() const override { return run; }
                    std::string Status() const override
                    { return "fake"; }
                };
                return std::make_unique<CountingSource>(
                    &sources_started);
            };
        reactive = std::make_unique<ReactiveInputs>(
            std::move(rdeps));
        edeps.reactive = reactive.get();
        sopts.max_connections = 16;
    }

    void Start()
    {
        svc    = std::make_unique<EngineService>(engine, edeps,
                                                 eopts);
        server = std::make_unique<IpcServer>(*svc, sopts);
        svc->SetEventSink(
            [this](const IpcEvent& ev) { server->Emit(ev); });
        svc->SetSessionEventSink(
            [this](uint64_t sid, const IpcEvent& ev,
                   const std::string& key)
            { server->EmitTo(sid, ev, key); });
    }

    /* Drive one preview publish tick (the owner loop calls
       PollPreviews every ~20 ms, independent of whether
       anyone subscribed to frame_status). */
    void Tick(TimePoint now)
    {
        svc->PollPreviews(now);
    }
};

static json Welcome(TestClient& c, IpcServer& server)
{
    CHECK(server.Serve(c.MakeChannel(), ChannelKind::Command));
    c.Hello();
    Message m;
    CHECK(c.RecvMsg(m, 5000));
    CHECK(m.type == MsgType::Welcome);
    return m.Body();
}

static json WsCall(TestClient& c, uint32_t& seq, const json& body,
                   std::vector<json>* events = nullptr,
                   uint32_t timeout_ms = 8000)
{
    c.SendMsg(MsgType::WorkspaceCommand, ++seq, body);
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeout_ms);
    while(std::chrono::steady_clock::now() < deadline)
    {
        Message m;
        if(!c.RecvMsg(m, 1000)) continue;
        if(m.type == MsgType::Event)
        {
            if(events) events->push_back(m.Body());
            continue;
        }
        if(m.request_id == seq)
        {
            return m.Body();
        }
    }
    CHECK(false);
    return json();
}

static json WsCallErr(TestClient& c, uint32_t& seq,
                      const json& body, ErrCode code,
                      const char* detail = nullptr)
{
    c.SendMsg(MsgType::WorkspaceCommand, ++seq, body);
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(8000);
    while(std::chrono::steady_clock::now() < deadline)
    {
        Message m;
        if(!c.RecvMsg(m, 1000)) continue;
        if(m.type == MsgType::Event) continue;
        if(m.request_id != seq) continue;
        CHECK(m.type == MsgType::Error);
        const json b = m.Body();
        CHECK(b.value("code", 0xFFFFFFFFu) == (uint32_t)code);
        if(detail)
        {
            CHECK(b.value("detail", "") == detail);
        }
        return b;
    }
    CHECK(false);
    return json();
}

/* Collect preview-frame EVENT data until one complete
   sequence assembles (chunks 0..chunks-1 of one sequence all
   present and contiguous), returning the merged objects
   array + envelope fields. */
static bool CollectFrame(TestClient& c, uint32_t timeout_ms,
                         json& frame_out)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeout_ms);
    uint64_t want_seq = UINT64_MAX, want_rev = 0;
    int chunks = -1;
    json objects = json::array();
    std::vector<bool> seen;
    json env;
    while(std::chrono::steady_clock::now() < deadline)
    {
        Message m;
        if(!c.RecvMsg(m, 500)) continue;
        if(m.type != MsgType::Event) continue;
        const json b = m.Body();
        if(b.value("kind", "") != "preview") continue;
        const json d = b.value("data", json::object());
        /* Every preview EVENT body is under the wire cap —
           RecvMsg already enforced kMaxBodyBytes. */
        if(d.value("type", "") == "frame")
        {
            const uint64_t s = d.value("sequence", 0ull);
            if(want_seq == UINT64_MAX)
            {
                want_seq = s;
                want_rev = d.value("draft_revision", 0ull);
                chunks   = d.value("chunks", 0);
                seen.assign((size_t)chunks, false);
                env = d;
            }
            if(s != want_seq) continue;      /* stale/other seq */
            const int ci = d.value("chunk", -1);
            if(ci >= 0 && ci < chunks && !seen[(size_t)ci])
            {
                seen[(size_t)ci] = true;
                for(const json& o : d["objects"])
                {
                    objects.push_back(o);
                }
            }
            bool all = true;
            for(bool v : seen) all &= v;
            if(all && chunks > 0)
            {
                env["objects"] = objects;
                env["draft_revision"] = want_rev;
                frame_out = env;
                return true;
            }
        }
    }
    return false;
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    g_tmp = fs::temp_directory_path() /
            ("o2-preview-" + std::to_string((uint64_t)
                 std::chrono::steady_clock::now()
                     .time_since_epoch().count()));
    fs::create_directories(g_tmp);
    WriteTestType(g_tmp / "types", 8);

    TestDeterministicEquivalence();
    TestMirroredCopiesPreviewOwner();
    TestUnboundObjectAndWorkerUntouched();
    TestDisabledInputsStayEmpty();
    TestHiddenPauseAndStaleCandidate();
    TestReducedLabeling();

    uint32_t seq = 0;

    /*========================================================*\
    || IPC surface                                            ||
    \*========================================================*/

    /*---- begin -> bounded frame events, capture untouched ---*/
    {
        std::printf("-- ipc: begin + frames, no capture started\n");
        Rig rig(g_tmp / "r1");
        rig.Start();
        TestClient c;
        Welcome(c, *rig.server);
        c.SendMsg(MsgType::Subscribe, ++seq,
                  json::object({{ "events", json::array({ "preview" }) }}));
        Message m;
        CHECK(c.RecvMsg(m, 5000));
        CHECK(m.type == MsgType::Ok);
        /* the snapshot event is an honest status */
        bool snap_seen = false;
        for(int i = 0; i < 4 && !snap_seen; ++i)
        {
            Message ev;
            if(!c.RecvMsg(ev, 2000)) continue;
            if(ev.type == MsgType::Event &&
               ev.Body().value("kind", "") == "preview")
            {
                snap_seen = true;
                CHECK(ev.Body()["data"].value("type", "") == "status");
                CHECK(ev.Body()["data"].value("active", true) == false);
            }
        }
        CHECK(snap_seen);

        const json r = WsCall(c, seq, json::object({
            { "verb",           "preview_begin" },
            { "document",       CompactDoc(2, 1, 0.75) },
            { "draft_revision", 11 },
        }));
        CHECK(r.value("active", false));
        CHECK(r.value("draft_revision", 0ull) == 11);

        /* A full frame assembles within a few ticks. */
        json frame;
        TimePoint now = Clock::now();
        for(int i = 0; i < 40 && !frame.is_object(); ++i)
        {
            now += std::chrono::milliseconds(20);
            rig.Tick(now);
            CollectFrame(c, 30, frame);
        }
        CHECK(frame.is_object());
        CHECK(frame.is_object() &&
              frame.value("draft_revision", 0ull) == 11);
        CHECK(frame.is_object() &&
              frame.value("sequence", 0ull) >= 1);
        bool free0 = false;
        size_t nobj = 0;
        if(frame.is_object() && frame.contains("objects"))
        {
            for(const json& o : frame["objects"])
            {
                ++nobj;
                free0 |= o.value("id", "").rfind("free0", 0)
                         == 0;
            }
        }
        CHECK(nobj >= 2);
        CHECK(free0);

        /* Preview never touched output counters or capture. */
        CHECK(rig.engine.Worker().Sequence() == 0);
        CHECK(rig.sources_started.load() == 0);
    }

    /*---- stale candidate replacement over the wire ---------*/
    {
        std::printf("-- ipc: preview_update replaces candidate\n");
        Rig rig(g_tmp / "r2");
        rig.Start();
        TestClient c;
        Welcome(c, *rig.server);
        c.SendMsg(MsgType::Subscribe, ++seq,
                  json::object({{ "events", "all" }}));
        Message m;
        CHECK(c.RecvMsg(m, 5000));

        CHECK(WsCall(c, seq, json::object({
            { "verb", "preview_begin" },
            { "document", CompactDoc(1, 0, 1.0) },
            { "draft_revision", 1 } })).value("active", false));

        json doc2 = CompactDoc(1, 0, 1.0);
        doc2["name"] = "newer";
        CHECK(WsCall(c, seq, json::object({
            { "verb", "preview_update" },
            { "document", doc2 },
            { "draft_revision", 2 } })).value("draft_revision", 0ull) == 2);

        json frame;
        TimePoint now = Clock::now();
        for(int i = 0; i < 40
                && !(frame.is_object() &&
                     frame.value("draft_revision", 0ull) == 2);
            ++i)
        {
            now += std::chrono::milliseconds(20);
            rig.Tick(now);
            CollectFrame(c, 30, frame);
        }
        CHECK(frame.is_object() &&
              frame.value("draft_revision", 0ull) == 2);

        /* update with no session -> no_preview */
        TestClient c2;
        Welcome(c2, *rig.server);
        WsCallErr(c2, seq, json::object({
            { "verb", "preview_update" },
            { "draft_revision", 9 } }),
            ErrCode::BadRequest, "no_preview");
    }

    /*---- hidden view pauses; resume publishes again --------*/
    {
        std::printf("-- ipc: visible pause/resume\n");
        Rig rig(g_tmp / "r3");
        rig.Start();
        TestClient c;
        Welcome(c, *rig.server);
        c.SendMsg(MsgType::Subscribe, ++seq,
                  json::object({{ "events", json::array({ "preview" }) }}));
        Message m;
        CHECK(c.RecvMsg(m, 5000));
        Message drain;
        while(c.RecvMsg(drain, 60)) {}         /* snapshot event */
        CHECK(WsCall(c, seq, json::object({
            { "verb", "preview_begin" },
            { "document", CompactDoc(1, 0, 1.0) },
            { "draft_revision", 1 } })).value("active", false));
        /* confirm frames flow */
        json frame;
        TimePoint now = Clock::now();
        for(int i = 0; i < 40 && !frame.is_object(); ++i)
        {
            now += std::chrono::milliseconds(20);
            rig.Tick(now);
            CollectFrame(c, 30, frame);
        }
        CHECK(frame.is_object());

        /* Hide the view — the engine pauses and SAYS so once.
           Flush first: frames emitted while the view was still
           visible may sit in the writer queue past the update
           reply — they are legitimate pre-pause traffic, not
           new work. */
        WsCall(c, seq, json::object({
            { "verb", "preview_update" },
            { "visible", false } }));
        while(c.RecvMsg(drain, 200)) {}
        bool paused_evt = false;
        TimePoint pnow = Clock::now();
        for(int i = 0; i < 10; ++i)
        {
            pnow += std::chrono::milliseconds(20);
            rig.Tick(pnow);
            Message ev;
            while(c.RecvMsg(ev, 40))
            {
                const json b = ev.Body();
                if(ev.type == MsgType::Event &&
                   b.value("kind", "") == "preview" &&
                   b["data"].value("type", "") == "status" &&
                   b["data"].value("paused", false))
                {
                    paused_evt = true;
                    CHECK(b["data"].value("reason", "")
                          == "hidden");
                }
                /* no frame events while hidden */
                CHECK(!(ev.type == MsgType::Event &&
                        b.value("kind", "") == "preview" &&
                        b["data"].value("type", "") == "frame"));
            }
        }
        CHECK(paused_evt);

        /* Resume. */
        WsCall(c, seq, json::object({
            { "verb", "preview_update" },
            { "visible", true } }));
        frame = json();
        for(int i = 0; i < 40 && !frame.is_object(); ++i)
        {
            pnow += std::chrono::milliseconds(20);
            rig.Tick(pnow);
            CollectFrame(c, 30, frame);
        }
        CHECK(frame.is_object());
    }

    /*---- display-off pauses the preview --------------------*/
    {
        std::printf("-- ipc: display-off pause\n");
        Rig rig(g_tmp / "r4");
        rig.Start();
        TestClient c;
        Welcome(c, *rig.server);
        c.SendMsg(MsgType::Subscribe, ++seq,
                  json::object({{ "events", json::array({ "preview" }) }}));
        Message m;
        CHECK(c.RecvMsg(m, 5000));
        Message drain;
        while(c.RecvMsg(drain, 60)) {}
        CHECK(WsCall(c, seq, json::object({
            { "verb", "preview_begin" },
            { "document", CompactDoc(1, 0, 1.0) },
            { "draft_revision", 1 } })).value("active", false));
        json frame;
        TimePoint now = Clock::now();
        for(int i = 0; i < 40 && !frame.is_object(); ++i)
        {
            now += std::chrono::milliseconds(20);
            rig.Tick(now);
            CollectFrame(c, 30, frame);
        }
        CHECK(frame.is_object());

        rig.power.PostEvent(PowerLifecycle::Event::DisplayOff);
        rig.power.Poll(now);
        CHECK(!rig.power.DisplayOn());
        /* Flush pre-pause frames still flushing on the wire. */
        while(c.RecvMsg(drain, 200)) {}

        bool paused_evt = false;
        for(int i = 0; i < 10; ++i)
        {
            now += std::chrono::milliseconds(20);
            rig.Tick(now);
            Message ev;
            while(c.RecvMsg(ev, 40))
            {
                const json b = ev.Body();
                if(ev.type == MsgType::Event &&
                   b.value("kind", "") == "preview" &&
                   b["data"].value("type", "") == "status")
                {
                    if(b["data"].value("paused", false))
                    {
                        paused_evt = true;
                        CHECK(b["data"].value("reason", "")
                              == "display_off");
                    }
                }
                CHECK(!(b.value("kind", "") == "preview" &&
                        b["data"].value("type", "") == "frame"));
            }
        }
        CHECK(paused_evt);

        rig.power.PostEvent(PowerLifecycle::Event::DisplayOn);
        rig.power.Poll(now);
        CHECK(rig.power.DisplayOn());
        frame = json();
        for(int i = 0; i < 40 && !frame.is_object(); ++i)
        {
            now += std::chrono::milliseconds(20);
            rig.Tick(now);
            CollectFrame(c, 30, frame);
        }
        CHECK(frame.is_object());
    }

    /*---- global bound of two; per-session replace ----------*/
    {
        std::printf("-- ipc: two-session bound + replace\n");
        Rig rig(g_tmp / "r5");
        rig.eopts.preview.max_sessions = 2;
        rig.Start();
        TestClient a, b, c3;
        Welcome(a, *rig.server);
        Welcome(b, *rig.server);
        Welcome(c3, *rig.server);
        const json doc = CompactDoc(1, 0, 1.0);
        CHECK(WsCall(a, seq, json::object({
            { "verb", "preview_begin" }, { "document", doc },
            { "draft_revision", 1 } })).value("active", false));
        CHECK(WsCall(b, seq, json::object({
            { "verb", "preview_begin" }, { "document", doc },
            { "draft_revision", 1 } })).value("active", false));
        /* A re-begin on session A replaces its own session —
           it does NOT consume a second slot. */
        CHECK(WsCall(a, seq, json::object({
            { "verb", "preview_begin" }, { "document", doc },
            { "draft_revision", 2 } })).value("active", false));
        /* The third distinct session is refused honestly. */
        WsCallErr(c3, seq, json::object({
            { "verb", "preview_begin" }, { "document", doc } }),
            ErrCode::Busy, "preview_limit");
        /* End frees the slot. */
        WsCall(a, seq, json::object({{ "verb", "preview_end" }}));
        CHECK(WsCall(c3, seq, json::object({
            { "verb", "preview_begin" }, { "document", doc },
            { "draft_revision", 1 } })).value("active", false));
    }

    /*---- malformed bodies never mutate live state ------------*/
    {
        std::printf("-- ipc: validation before mutation\n");
        Rig rig(g_tmp / "r10");
        rig.eopts.preview.max_sessions = 1;
        rig.Start();
        TestClient c;
        Welcome(c, *rig.server);
        /* A begin whose visible flag is the wrong type is
           refused BEFORE a slot is allocated — the single
           global slot stays free for a healthy client. */
        WsCallErr(c, seq, json::object({
            { "verb", "preview_begin" },
            { "document", CompactDoc(1, 0, 1.0) },
            { "visible", "yes" } }),
            ErrCode::BadRequest);
        TestClient c2;
        Welcome(c2, *rig.server);
        CHECK(WsCall(c2, seq, json::object({
            { "verb", "preview_begin" },
            { "document", CompactDoc(1, 0, 1.0) },
            { "draft_revision", 7 } })).value("active", false));
        /* c2 holds the one slot — nothing leaked from the
           refused begin. */
        WsCallErr(c, seq, json::object({
            { "verb", "preview_begin" },
            { "document", CompactDoc(1, 0, 1.0) } }),
            ErrCode::Busy, "preview_limit");

        /* A document-bearing update with a malformed visible
           is refused WITHOUT touching the live candidate —
           revision 7 keeps previewing. */
        WsCallErr(c2, seq, json::object({
            { "verb", "preview_update" },
            { "document", CompactDoc(1, 0, 1.0) },
            { "draft_revision", 99 },
            { "visible", 42 } }),
            ErrCode::BadRequest);
        const json up = WsCall(c2, seq, json::object({
            { "verb", "preview_update" },
            { "visible", true } }));
        CHECK(up.value("active", false));
        CHECK(up.value("draft_revision", 0ull) == 7);
        /* A document update omitting draft_revision keeps the
           session's current one — never a silent reset to 0. */
        const json u2 = WsCall(c2, seq, json::object({
            { "verb", "preview_update" },
            { "document", CompactDoc(1, 0, 1.0) } }));
        CHECK(u2.value("draft_revision", 0ull) == 7);
    }

    /*---- disconnect releases the preview --------------------*/
    {
        std::printf("-- ipc: disconnect cleanup\n");
        Rig rig(g_tmp / "r6");
        rig.eopts.preview.max_sessions = 1;
        rig.Start();
        TestClient a;
        Welcome(a, *rig.server);
        const json doc = CompactDoc(1, 0, 1.0);
        CHECK(WsCall(a, seq, json::object({
            { "verb", "preview_begin" }, { "document", doc },
            { "draft_revision", 1 } })).value("active", false));
        /* Second session refused while A holds the slot. */
        TestClient b;
        Welcome(b, *rig.server);
        WsCallErr(b, seq, json::object({
            { "verb", "preview_begin" }, { "document", doc } }),
            ErrCode::Busy, "preview_limit");
        /* A dies — the slot frees without any preview_end. */
        a.CloseInbound();
        CHECK(WaitFor([&] {
            return rig.server->Stats().sessions_active == 1;
        }));
        CHECK(WsCall(b, seq, json::object({
            { "verb", "preview_begin" }, { "document", doc },
            { "draft_revision", 4 } })).value("active", false));
    }

    /*---- chunking below the IPC cap -------------------------*/
    {
        std::printf("-- ipc: chunked frame under cap\n");
        Rig rig(g_tmp / "r7");
        /* Force several chunks: 24 objects x 8 emitters. */
        rig.eopts.preview.chunk_bytes = 4 * 1024;
        rig.Start();
        TestClient c;
        Welcome(c, *rig.server);
        c.SendMsg(MsgType::Subscribe, ++seq,
                  json::object({{ "events", json::array({ "preview" }) }}));
        Message m;
        CHECK(c.RecvMsg(m, 5000));
        Message drain;
        while(c.RecvMsg(drain, 60)) {}
        CHECK(WsCall(c, seq, json::object({
            { "verb", "preview_begin" },
            { "document", CompactDoc(24, 0, 1.0) },
            { "draft_revision", 1 } })).value("active", false));
        json frame;
        TimePoint now = Clock::now();
        int chunk_obs = 0;
        /* Reassemble the first complete sequence in-line —
           consumed chunks must not starve the assembler. */
        uint64_t want_seq = UINT64_MAX;
        int      chunks   = -1;
        std::vector<bool> seen;
        json     objects  = json::array();
        json     env;
        for(int i = 0; i < 60 && !frame.is_object(); ++i)
        {
            now += std::chrono::milliseconds(20);
            rig.Tick(now);
            Message ev;
            while(c.RecvMsg(ev, 15))
            {
                if(ev.type != MsgType::Event) continue;
                const json b = ev.Body();
                if(b.value("kind", "") != "preview") continue;
                const json d = b.value("data", json::object());
                if(d.value("type", "") != "frame") continue;
                ++chunk_obs;
                /* every chunk is individually well under
                   the 1 MiB body cap — enforced on the
                   wire; re-check the serialized size. */
                CHECK(b.dump().size() < kMaxBodyBytes);
                const uint64_t s = d.value("sequence", 0ull);
                if(want_seq == UINT64_MAX)
                {
                    want_seq = s;
                    chunks   = d.value("chunks", 0);
                    seen.assign((size_t)chunks, false);
                    env = d;
                }
                if(s != want_seq) continue;
                const int ci = d.value("chunk", -1);
                if(ci >= 0 && ci < chunks && !seen[(size_t)ci])
                {
                    seen[(size_t)ci] = true;
                    for(const json& o : d["objects"])
                    {
                        objects.push_back(o);
                    }
                }
                bool all = chunks > 0;
                for(bool v : seen) all &= v;
                if(all)
                {
                    env["objects"] = objects;
                    frame = env;
                }
            }
        }
        CHECK(frame.is_object());
        CHECK((frame.is_object() &&
               frame.value("chunks", 1) > 1) || chunk_obs > 1);
        /* all 24 objects reassembled in document order */
        CHECK(frame.is_object() && frame.contains("objects") &&
              frame["objects"].size() == 24);
    }

    /*---- invalid document refused honestly ------------------*/
    {
        std::printf("-- ipc: invalid document\n");
        Rig rig(g_tmp / "r8");
        rig.Start();
        TestClient c;
        Welcome(c, *rig.server);
        WsCallErr(c, seq, json::object({
            { "verb", "preview_begin" },
            { "document", json::object({{ "bogus", true }}) },
        }), ErrCode::BadRequest, "invalid_document");
        WsCallErr(c, seq, json::object({
            { "verb", "preview_begin" },
            { "staged", "nosuchtoken" },
        }), ErrCode::BadRequest);
    }

    /*---- preview_end stops frames ---------------------------*/
    {
        std::printf("-- ipc: preview_end\n");
        Rig rig(g_tmp / "r9");
        rig.Start();
        TestClient c;
        Welcome(c, *rig.server);
        c.SendMsg(MsgType::Subscribe, ++seq,
                  json::object({{ "events", json::array({ "preview" }) }}));
        Message m;
        CHECK(c.RecvMsg(m, 5000));
        Message drain;
        while(c.RecvMsg(drain, 60)) {}
        CHECK(WsCall(c, seq, json::object({
            { "verb", "preview_begin" },
            { "document", CompactDoc(1, 0, 1.0) },
            { "draft_revision", 1 } })).value("active", false));
        const json end = WsCall(c, seq, json::object({
            { "verb", "preview_end" } }));
        CHECK(end.value("active", true) == false);
        /* Frames emitted just before the end may still flush —
           drain them so only POST-end work is observed. */
        {
            Message d;
            while(c.RecvMsg(d, 200)) {}
        }
        /* several ticks produce no frame events */
        TimePoint now = Clock::now();
        bool frame_seen = false;
        for(int i = 0; i < 8; ++i)
        {
            now += std::chrono::milliseconds(20);
            rig.Tick(now);
            Message ev;
            while(c.RecvMsg(ev, 40))
            {
                const json b = ev.Body();
                if(ev.type == MsgType::Event &&
                   b.value("kind", "") == "preview" &&
                   b["data"].value("type", "") == "frame")
                {
                    frame_seen = true;
                }
            }
        }
        CHECK(!frame_seen);
    }

    /*---- I-7: preview frames leave the service OFF its mutex
       (encoding a chunk never stalls SceneJson/apply), and
       the pre-serialized body is exactly the event's JSON. */
    {
        std::printf("-- ipc: preview emit runs outside the service mutex\n");
        Rig rig(g_tmp / "r-i7");
        rig.Start();
        std::mutex                         probe_m;
        std::vector<std::future<uint64_t>> probes;   /* probe_m */
        std::atomic<int> frames{ 0 }, blocked{ 0 }, raw_mismatch{ 0 };
        rig.svc->SetSessionEventSink(
            [&](uint64_t sid, const IpcEvent& ev,
                const std::string& key)
            {
                if(ev.kind == "preview" &&
                   key.rfind("frame/", 0) == 0)
                {
                    ++frames;
                    /* Another thread reading service state must
                       not wait for this emit to finish. */
                    std::future<uint64_t> f = std::async(
                        std::launch::async,
                        [&rig] { return rig.svc->SceneRevision(); });
                    if(f.wait_for(std::chrono::seconds(1)) !=
                       std::future_status::ready)
                    {
                        ++blocked;
                    }
                    /* Serialized once by the producer, and
                       exactly the event's JSON; the framed
                       EVENT is byte-identical to the old
                       envelope encode. */
                    if(!ev.data_json ||
                       *ev.data_json != ev.data.dump() ||
                       EncodeEventMessage(ev) !=
                           EncodeMessage(MsgType::Event, 0,
                               json::object({
                                   { "kind", ev.kind },
                                   { "data", ev.data } })))
                    {
                        ++raw_mismatch;
                    }
                    std::lock_guard<std::mutex> g(probe_m);
                    probes.push_back(std::move(f));
                }
                rig.server->EmitTo(sid, ev, key);
            });
        TestClient c;
        Welcome(c, *rig.server);
        c.SendMsg(MsgType::Subscribe, ++seq,
                  json::object({{ "events", json::array({ "preview" }) }}));
        const json r = WsCall(c, seq, json::object({
            { "verb",           "preview_begin" },
            { "document",       CompactDoc(3, 1, 0.5) },
            { "draft_revision", 4 },
        }));
        CHECK(r.value("active", false));
        json frame;
        TimePoint now = Clock::now();
        for(int i = 0; i < 40 && !frame.is_object(); ++i)
        {
            now += std::chrono::milliseconds(20);
            rig.Tick(now);
            CollectFrame(c, 30, frame);
        }
        CHECK(frame.is_object());
        CHECK(frames.load() > 0);
        CHECK(blocked.load() == 0);
        CHECK(raw_mismatch.load() == 0);
        std::lock_guard<std::mutex> g(probe_m);
        for(std::future<uint64_t>& f : probes)
        {
            f.wait();
        }
    }

    std::error_code ec;
    fs::remove_all(g_tmp, ec);
    std::printf("preview session: %d checks, %d failures\n",
                checks, failures);
    return failures ? 1 : 0;
}
