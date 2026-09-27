// SPDX-License-Identifier: GPL-2.0-or-later
//
// Layout view tests (Task 4.4) — two layers:
//
//   1. LayoutEditModel (Qt-free): the applied-document view,
//      op builders' boundary validation (calibration indices
//      are refused BEFORE a body exists), linked-copy
//      semantics (a mirrored fan is a copy — never a second
//      device, never multiplied LEDs), and the bounded
//      undo/redo history (LIFO revert order, cap eviction).
//
//   2. EngineBridge + LayoutController over the in-process
//      wire pipe (test_pipe.h) against a scripted engine:
//      move/rotate/link/bind/calibrate emit exact
//      LAYOUT_COMMAND bodies, the engine's scene event
//      re-renders the applied document, undo/redo travel as
//      whole-document SET_SCENE reverts, a refused op leaves
//      no ghost undo entry, and out-of-range calibration
//      never reaches the wire.
//
// CHECK-macro main like the other UI suites — no framework.

#include "EngineBridge.h"
#include "LayoutController.h"
#include "LayoutEditModel.h"

#include "test_pipe.h"

#include <QCoreApplication>
#include <QVariantMap>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

using namespace openrgb2;
using namespace openrgb2::ipc;
using namespace openrgb2::ui;
using namespace ui_test;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

static nlohmann::json V3(double x, double y, double z)
{
    return nlohmann::json::object({{"x",x},{"y",y},{"z",z}});
}

/* The applied document: two bound device fans, one linked
   mirror copy of fan0 (emitters cleared — the linked-copy
   contract), one unbound device, and a decor body. */
static nlohmann::json SceneFixture()
{
    return nlohmann::json::object({
        { "version",    2 },
        { "name",       "layout" },
        { "brightness", 1.0 },
        { "bindings", nlohmann::json::array({
            nlohmann::json::object({
                { "id",              "slw0" },
                { "controller_name", "UNI FAN" },
                { "vendor",          "Lian Li" },
                { "serial",          "" },
                { "location",        "" },
                { "device_type",     -1 },
                { "zone_name",       "Fan 1" },
                { "zone_leds",       40 },
            }),
            nlohmann::json::object({
                { "id",              "slw1" },
                { "controller_name", "UNI FAN" },
                { "vendor",          "Lian Li" },
                { "serial",          "" },
                { "location",        "" },
                { "device_type",     -1 },
                { "zone_name",       "Fan 2" },
                { "zone_leds",       40 },
            }),
        }) },
        { "objects", nlohmann::json::array({
            nlohmann::json::object({
                { "id",       "fan0" },
                { "label",    "Fan 0" },
                { "kind",     "device" },
                { "position", V3(0.0, 0.0, 0.0) },
                { "rotation", V3(0.0, 0.0, 0.0) },
                { "size_m",   V3(0.12, 0.025, 0.12) },
                { "binding",  "slw0" },
                { "geometry", "fan" },
                { "verified", true },
                { "visible",  true },
                { "emitters", nlohmann::json::array({
                    nlohmann::json::object(
                        {{ "pos", V3(0,0,0) },
                         { "group", "fan0" }, { "address", 0 }}),
                    nlohmann::json::object(
                        {{ "pos", V3(0.01,0,0) },
                         { "group", "fan0" }, { "address", 1 }}),
                }) },
            }),
            nlohmann::json::object({
                { "id",       "fan1" },
                { "label",    "Fan 1" },
                { "kind",     "device" },
                { "position", V3(0.20, 0.0, 0.0) },
                { "rotation", V3(0.0, 45.0, 0.0) },
                { "size_m",   V3(0.12, 0.025, 0.12) },
                { "binding",  "slw1" },
                { "geometry", "fan" },
                { "verified", true },
                { "visible",  true },
                { "emitters", nlohmann::json::array({
                    nlohmann::json::object(
                        {{ "pos", V3(0,0,0) },
                         { "group", "fan1" }, { "address", 0 }}),
                }) },
            }),
            /* the mirrored fan — a LINKED COPY, not a third
               device: kind linked, no emitters, no binding */
            nlohmann::json::object({
                { "id",        "fan0copy" },
                { "label",     "Fan 0 copy" },
                { "kind",      "linked" },
                { "position",  V3(0.40, 0.0, 0.0) },
                { "rotation",  V3(0.0, 0.0, 0.0) },
                { "size_m",    V3(0.12, 0.025, 0.12) },
                { "binding",   "" },
                { "mirror_of", "fan0" },
                { "geometry",  "fan" },
                { "verified",  false },
                { "visible",   true },
                { "emitters",  nlohmann::json::array() },
            }),
            nlohmann::json::object({
                { "id",       "ghost" },
                { "label",    "Unbound" },
                { "kind",     "device" },
                { "position", V3(0.0, 0.0, 0.30) },
                { "rotation", V3(0.0, 0.0, 0.0) },
                { "size_m",   V3(0.10, 0.02, 0.10) },
                { "binding",  "" },
                { "verified", false },
                { "visible",  true },
                { "emitters", nlohmann::json::array({
                    nlohmann::json::object(
                        {{ "pos", V3(0,0,0) },
                         { "group", "" }, { "address", -1 }}),
                }) },
            }),
            nlohmann::json::object({
                { "id",       "case" },
                { "label",    "Case" },
                { "kind",     "decor" },
                { "position", V3(0.0, 0.0, -0.25) },
                { "rotation", V3(0.0, 0.0, 0.0) },
                { "size_m",   V3(0.50, 0.40, 0.50) },
                { "binding",  "" },
                { "visible",  true },
                { "emitters", nlohmann::json::array() },
            }),
        }) },
        { "object_colors",  nlohmann::json::object() },
        { "emitter_colors", nlohmann::json::object() },
        { "effect", nlohmann::json::object({
            { "preset", "aurora" }, { "seed", 0 },
            { "speed", 1.0 }, { "intensity", 1.0 },
            { "playing", true },
            { "layers", nlohmann::json::array() },
        }) },
    });
}

/* One snapshot the bindings join against — the same name
   matching the controller_name hint resolves to. */
static nlohmann::json SnapFixture()
{
    return nlohmann::json::object({
        { "key", nlohmann::json::object({
            { "device",   "Lian Li: SL Wireless" },
            { "serial",   "AABBCCDDEEFF" },
            { "location", "Wireless: AA:BB:CC:DD:EE:FF" },
            { "zone",     "" } }) },
        { "name",       "Lian Li UNI FAN SL Wireless" },
        { "vendor",     "Lian Li" },
        { "device_type", 2 },
        { "connection", "connected" },
        { "led_count",  120 },
        { "zones", nlohmann::json::array({
            nlohmann::json::object(
                {{ "name", "Fan 1" }, { "led_count", 40 }}),
            nlohmann::json::object(
                {{ "name", "Fan 2" }, { "led_count", 40 }}),
            nlohmann::json::object(
                {{ "name", "Fan 3" }, { "led_count", 40 }}),
        }) },
    });
}

/*---------------------------------------------------------*\
||| Part 1 — Qt-free LayoutEditModel                          ||
\*---------------------------------------------------------*/

static void ModelTests()
{
    LayoutEditModel m;
    m.LoadApplied(SceneFixture());
    m.SetSnapshots({ SnapFixture() });
    CHECK(m.HasScene());

    /*---- the items view ------------------------------------*/
    const std::vector<LayoutItem> items = m.Items();
    CHECK(items.size() == 5);
    const LayoutItem* fan0 = nullptr;
    const LayoutItem* copy = nullptr;
    const LayoutItem* ghost = nullptr;
    const LayoutItem* dcase = nullptr;
    for(const LayoutItem& it : items)
    {
        if(it.id == "fan0")     fan0  = &it;
        if(it.id == "fan0copy") copy  = &it;
        if(it.id == "ghost")    ghost = &it;
        if(it.id == "case")     dcase = &it;
    }
    CHECK(fan0 && copy && ghost && dcase);
    /* the mirrored fan is a linked copy — not a device, not
       another writer: */
    CHECK(copy->kind == "linked");
    CHECK(copy->state == "linked");
    CHECK(copy->mirror_of == "fan0");
    CHECK(copy->emitter_count == 0);
    CHECK(copy->zone_leds == 0);      /* no own zone  */
    /* while its source carries the real bound counts */
    CHECK(fan0->kind == "device");
    CHECK(fan0->state == "resolved");
    CHECK(fan0->connected);
    CHECK(fan0->zone_leds == 40);
    CHECK(fan0->emitter_count == 2);
    CHECK(fan0->addressed_count == 2);
    CHECK(ghost->state == "unbound");
    CHECK(dcase->state.empty());
    CHECK(fan0->w > 0.10 && fan0->w < 0.13);

    /*---- op bodies ------------------------------------------*/
    LayoutOp op;
    std::string err;
    CHECK(m.BuildSetPosition("fan0", 0.5, 0.0, 0.25, op, err));
    CHECK(op.body["verb"] == "set_position");
    CHECK(op.body["object_id"] == "fan0");
    CHECK(op.body["position"]["x"].get<double>() == 0.5);
    CHECK(!m.BuildSetPosition("nobody", 0, 0, 0, op, err));

    CHECK(m.BuildSetOrientation("fan0", 0, 90, 0, op, err));
    CHECK(op.body["verb"] == "set_orientation");
    CHECK(op.body["rotation"]["y"].get<double>() == 90.0);

    /* mirror: linked-copy rules mirrored at the boundary */
    CHECK(m.BuildSetMirror("fan1", "fan0", op, err));
    CHECK(op.body["verb"] == "set_mirror");
    CHECK(op.body["mirror_of"] == "fan0");
    CHECK(!m.BuildSetMirror("fan1", "fan1", op, err));  /* self */
    CHECK(!m.BuildSetMirror("fan1", "case", op, err));  /* decor */
    CHECK(!m.BuildSetMirror("fan1", "fan0copy", op, err)); /* chain */
    /* fan0 is fan0copy's mirror SOURCE — linking it would
       strand fan0copy.mirror_of on a linked object (the chain
       the engine's graph validation forbids). Same refusal as
       the verb boundary; a copy sharing that source is legal. */
    CHECK(!m.BuildSetMirror("fan0", "fan1", op, err));
    CHECK(m.BuildSetMirror("ghost", "fan0", op, err));
    CHECK(m.BuildSetMirror("fan0copy", "", op, err));   /* unlink */
    CHECK(op.body["mirror_of"] == "");
    CHECK(!m.BuildSetMirror("fan0", "", op, err));      /* not linked */

    /* bindings: existing id, unbind, unknown id, and the
       controller-derived create. */
    CHECK(m.BuildSetBinding("fan0", "slw1", op, err));
    CHECK(op.body["binding"] == "slw1");
    CHECK(m.BuildSetBinding("fan0", "", op, err));
    CHECK(!m.BuildSetBinding("fan0", "nope", op, err));
    CHECK(!m.BuildSetBinding("case", "slw0", op, err)); /* decor */
    CHECK(m.BuildSetBindingFromController("ghost", "b_new",
          nlohmann::json::object(
              {{ "device",   "Lian Li: SL Wireless" },
               { "serial",   "AABBCCDDEEFF" },
               { "location", "Wireless: AA:BB:CC:DD:EE:FF" },
               { "zone",     "Fan 3" }}),
          op, err));
    CHECK(op.body["binding"] == "b_new");
    CHECK(op.body["controller"]["zone"] == "Fan 3");
    CHECK(!m.BuildSetBindingFromController("ghost", "slw0",
          nlohmann::json::object({{"device","x"}}), op, err));
    CHECK(!m.BuildSetBindingFromController("ghost", "b2",
          nlohmann::json::object(), op, err));   /* no key */

    /*---- calibration: the UI is the FIRST rejection boundary —
           out-of-range indices refuse BEFORE any body exists */
    CHECK(m.BoundZoneLeds("fan0") == 40);
    CHECK(m.BoundZoneLeds("ghost") == 0);
    CHECK(m.BuildCalibrateLed("fan0", 0, 39, op, err));
    CHECK(op.body["verb"] == "calibrate_led");
    CHECK(op.body["emitter_index"].get<int>() == 0);
    CHECK(op.body["address"].get<int>() == 39);
    CHECK(m.BuildCalibrateLed("fan0", 1, -1, op, err));  /* ro */
    CHECK(!m.BuildCalibrateLed("fan0", 0, 40, op, err)); /* =N  */
    CHECK(!m.BuildCalibrateLed("fan0", 0, 400, op, err));
    CHECK(!m.BuildCalibrateLed("fan0", 0, -2, op, err));
    CHECK(!m.BuildCalibrateLed("fan0", 5, 0, op, err));  /* idx */
    CHECK(!m.BuildCalibrateLed("ghost", 0, 0, op, err)); /* bound */
    CHECK(m.BuildCalibrateLed("ghost", 0, -1, op, err)); /* ro ok */

    /*---- undo/redo: bounded stack, LIFO revert order --------*/
    LayoutEditModel h;
    h.LoadApplied(SceneFixture());
    CHECK(h.UndoCount() == 0 && h.RedoCount() == 0);
    for(int i = 0; i < (int)LayoutEditModel::kHistoryBound + 8;
        ++i)
    {
        h.PushUndo("edit " + std::to_string(i));
    }
    CHECK(h.UndoCount() == LayoutEditModel::kHistoryBound);
    /* the cap evicted the OLDEST entries — the newest is the
       next revert (LIFO): */
    CHECK(h.PeekUndo()->label ==
          "edit " + std::to_string(
              (int)LayoutEditModel::kHistoryBound + 8 - 1));
    /* revert order walk: 39,38,... — strictly newest-first
       (numeric suffix order, not lexical) */
    const auto label_num = [](const std::string& s)
    {
        return std::atoi(s.c_str() + 5);   /* "edit N" */
    };
    int last_num = 0;
    int walked = 0;
    while(const LayoutEditModel::HistoryEntry* e = h.PeekUndo())
    {
        if(walked == 0)
        {
            last_num = label_num(e->label);
        }
        else
        {
            CHECK(label_num(e->label) <
                  last_num);        /* strictly earlier edit */
            last_num = label_num(e->label);
        }
        h.PopUndo();
        ++walked;
    }
    CHECK(walked == (int)LayoutEditModel::kHistoryBound);

    /* undo<->redo migration preserves each step's document —
       pop an undo entry, park the CURRENT doc on redo, and
       replay pops it back to undo. */
    LayoutEditModel m2;
    nlohmann::json base = SceneFixture();
    nlohmann::json moved = base;
    moved["objects"][0]["position"]["x"] = 0.7;
    m2.LoadApplied(base);
    m2.PushUndo("move fan0");          /* restore = base     */
    m2.LoadApplied(moved);             /* applied: the edit  */
    const LayoutEditModel::HistoryEntry* u = m2.PeekUndo();
    CHECK(u && u->doc["objects"][0]["position"]["x"]
                   .get<double>() == 0.0);
    m2.PopUndo();
    m2.PushRedo("move fan0", m2.Applied());   /* park moved */
    m2.LoadApplied(base);                     /* reverted   */
    const LayoutEditModel::HistoryEntry* rd = m2.PeekRedo();
    CHECK(rd && rd->doc["objects"][0]["position"]["x"]
                    .get<double>() == 0.7);
    m2.PopRedo();
    m2.PushUndoEntry("move fan0", m2.Applied());  /* redo path */
    CHECK(m2.UndoCount() == 1);
    CHECK(m2.PeekUndo()->doc["objects"][0]["position"]["x"]
              .get<double>() == 0.0);

    /* a NEW edit clears the redo timeline */
    m2.PushRedo("stale", base);
    m2.PushUndo("new edit");
    CHECK(m2.RedoCount() == 0);
    m2.ClearHistory();
    CHECK(m2.UndoCount() == 0 && m2.RedoCount() == 0);
}

/*---------------------------------------------------------*\
||| Part 2 — bridge + controller over the scripted engine     ||
\*---------------------------------------------------------*/

struct Server
{
    explicit Server(ServerPeer peer) : srv(std::move(peer)) {}
    ServerPeer     srv;
    nlohmann::json doc = SceneFixture();
    int            revision        = 1;
    int            layout_commands = 0;
    int            scene_saves     = 0;
    bool           fail_next       = false;
    nlohmann::json last_layout_body;
    nlohmann::json last_scene_body;
};

static nlohmann::json SceneEnvelope(const Server& s)
{
    return nlohmann::json::object({
        { "loaded",   true },
        { "revision", s.revision },
        { "name",     "layout" },
        { "playing",  true },
        { "document", s.doc },
    });
}

static nlohmann::json WelcomeBody(const nlohmann::json& doc)
{
    return nlohmann::json::object({
        { "server_version",     1 },
        { "negotiated_version", 1 },
        { "channel",            "command" },
        { "engine",             "openrgb2-engine" },
        { "running",            true },
        { "state", nlohmann::json::object({
            { "engine", nlohmann::json::object(
                {{ "running", true }, { "scene_clock_hz", 60 }}) },
            { "scene", nlohmann::json::object(
                {{ "loaded", true }, { "revision", 1 },
                 { "name", "layout" }, { "playing", true },
                 { "document", doc }}) },
            { "sdk", nlohmann::json::object(
                {{ "state", "connected" },
                 { "reconnect_attempt", 0 }}) },
        }) },
    });
}

/* The scripted engine mutates its stored document for the
   scene-mutation verbs — the scene EVENT echo is what the
   controller repaints from. Task 4.5: calibrate_led mirrors
   the REAL engine's dispatch validation (EngineService::
   HandleLayoutMutate + the apply path's
   ValidateSceneLedAddresses) so the fake server is a second
   boundary, not a rubber stamp — a hostile client that skips
   LayoutEditModel's check meets a refusal here, exactly like
   EngineService::Dispatch. Returns "" applied, else the
   bad_request message. */
static std::string ApplyLayoutVerb(Server& s,
                                   const std::string& verb,
                                   const nlohmann::json& body)
{
    const std::string oid = body.value("object_id", "");
    for(nlohmann::json& o : s.doc["objects"])
    {
        if(o.value("id", "") != oid)
        {
            continue;
        }
        if(verb == "set_position")
        {
            o["position"] = body["position"];
        }
        else if(verb == "set_orientation")
        {
            o["rotation"] = body["rotation"];
        }
        else if(verb == "set_mirror")
        {
            const std::string t = body.value("mirror_of", "");
            if(t.empty())
            {
                o["kind"]      = "device";
                o["mirror_of"] = "";
            }
            else
            {
                o["kind"]      = "linked";
                o["mirror_of"] = t;
                o["binding"]   = "";
                o["emitters"]  = nlohmann::json::array();
                o["verified"]  = false;
            }
        }
        else if(verb == "calibrate_led")
        {
            /* Engine parity (ipc_engine_service_test): bad
               object kind, bad emitter index, address < -1,
               unbound/unknown zone bound, and
               address >= zone_leds all refuse. */
            if(o.value("kind", "") != "device")
            {
                return "calibrate_led targets device objects";
            }
            if(!body.value("emitter_index", nlohmann::json())
                    .is_number_integer() ||
               !body.value("address", nlohmann::json())
                    .is_number_integer())
            {
                return "calibrate_led requires integer "
                       "emitter_index and address";
            }
            const int64_t ei =
                body["emitter_index"].get<int64_t>();
            const int64_t ad = body["address"].get<int64_t>();
            const int64_t emitters =
                o.contains("emitters") && o["emitters"].is_array()
                    ? (int64_t)o["emitters"].size() : 0;
            if(ei < 0 || ei >= emitters)
            {
                return "emitter_index out of range";
            }
            if(ad < -1)
            {
                return "address must be >= -1";
            }
            if(ad >= 0)
            {
                int64_t zone_leds = 0;
                const std::string bid = o.value("binding", "");
                if(!bid.empty() && s.doc.contains("bindings") &&
                   s.doc["bindings"].is_array())
                {
                    for(const nlohmann::json& b : s.doc["bindings"])
                    {
                        if(b.value("id", "") == bid)
                        {
                            zone_leds =
                                b.value("zone_leds", (int64_t)0);
                            break;
                        }
                    }
                }
                if(zone_leds <= 0)
                {
                    return "object '" + oid +
                           "' has no bound zone LED count";
                }
                if(ad >= zone_leds)
                {
                    return "address " + std::to_string(ad) +
                           " out of range (zone has " +
                           std::to_string(zone_leds) + " LEDs)";
                }
            }
            o["emitters"][ei]["address"] = ad;
        }
        else if(verb == "set_binding")
        {
            o["binding"] = body.value("binding", "");
        }
        break;
    }
    return std::string();
}

static bool ServeOnce(Server& s, uint32_t wait_ms = 15)
{
    Message m;
    if(!s.srv.RecvMsg(m, wait_ms))
    {
        return false;
    }
    const nlohmann::json body = m.Body();
    switch(m.type)
    {
    case MsgType::Hello:
        s.srv.SendMsg(MsgType::Welcome, 0, WelcomeBody(s.doc));
        break;
    case MsgType::Subscribe:
        s.srv.SendMsg(MsgType::Ok, m.request_id,
            nlohmann::json::object(
                { { "subscribed", nlohmann::json::array({
                    "topology", "scene", "playback",
                    "connection", "frame_status" }) } }));
        break;
    case MsgType::GetScene:
        s.srv.SendMsg(MsgType::Scene, m.request_id,
                      SceneEnvelope(s));
        break;
    case MsgType::GetSnapshots:
        s.srv.SendMsg(MsgType::Snapshots, m.request_id,
                      nlohmann::json::object());
        break;
    case MsgType::GetState:
        s.srv.SendMsg(MsgType::State, m.request_id,
            nlohmann::json::object({
                { "engine", nlohmann::json::object(
                    {{ "running", true }}) },
                { "scene",  SceneEnvelope(s) },
            }));
        break;
    case MsgType::GetPresets:
        s.srv.SendMsg(MsgType::Presets, m.request_id,
                      nlohmann::json::object(
                          { { "presets",
                              nlohmann::json::array() } }));
        break;
    case MsgType::GetDeliveryStats:
        s.srv.SendMsg(MsgType::DeliveryStats, m.request_id,
                      nlohmann::json::object());
        break;
    case MsgType::GetPlayback:
        s.srv.SendMsg(MsgType::Playback, m.request_id,
                      nlohmann::json::object());
        break;
    case MsgType::SetScene:
        s.last_scene_body = body;
        ++s.scene_saves;
        if(s.fail_next)
        {
            s.fail_next = false;
            s.srv.SendMsg(MsgType::Error, m.request_id,
                nlohmann::json::object({
                    { "code",    (uint32_t)ErrCode::BadRequest },
                    { "error",   "bad_request" },
                    { "message", "refused by test" },
                }));
        }
        else
        {
            s.doc = body.value("document",
                               nlohmann::json::object());
            ++s.revision;
            s.srv.SendMsg(MsgType::Ok, m.request_id,
                nlohmann::json::object(
                    { { "revision", s.revision } }));
            s.srv.SendMsg(MsgType::Event, 0,
                nlohmann::json::object({
                    { "kind", "scene" },
                    { "data", SceneEnvelope(s) },
                }));
        }
        break;
    case MsgType::LayoutCommand:
        ++s.layout_commands;
        s.last_layout_body = body;
        {
            const std::string verb = body.value("verb", "");
            if(verb == "identify")
            {
                s.srv.SendMsg(MsgType::Ok, m.request_id,
                    nlohmann::json::object({
                        { "identifying", true },
                        { "controller",  body["controller"] },
                        { "zone", body.value("zone",
                                             std::string()) },
                        { "led_start", 40 },
                        { "led_count", 1 },
                        { "duration_ms",
                          body.value("duration_ms", 3000) },
                    }));
            }
            else if(s.fail_next)
            {
                s.fail_next = false;
                s.srv.SendMsg(MsgType::Error, m.request_id,
                    nlohmann::json::object({
                        { "code",
                          (uint32_t)ErrCode::BadRequest },
                        { "error",   "bad_request" },
                        { "message", "refused by test" },
                    }));
            }
            else
            {
                const std::string verr =
                    ApplyLayoutVerb(s, verb, body);
                if(!verr.empty())
                {
                    /* Engine-parity refusal — the dispatch
                       boundary's BadRequest. */
                    s.srv.SendMsg(MsgType::Error, m.request_id,
                        nlohmann::json::object({
                            { "code",
                              (uint32_t)ErrCode::BadRequest },
                            { "error",   "bad_request" },
                            { "message", verr },
                        }));
                    break;
                }
                ++s.revision;
                s.srv.SendMsg(MsgType::Ok, m.request_id,
                    nlohmann::json::object({
                        { "object_id",
                          body.value("object_id",
                                     std::string()) },
                        { "revision", s.revision },
                    }));
                s.srv.SendMsg(MsgType::Event, 0,
                    nlohmann::json::object({
                        { "kind", "scene" },
                        { "data", SceneEnvelope(s) },
                    }));
            }
        }
        break;
    default:
        s.srv.SendMsg(MsgType::Error, m.request_id,
            nlohmann::json::object({
                { "code",    (uint32_t)ErrCode::BadRequest },
                { "error",   "bad_request" },
                { "message", "unexpected request" },
            }));
        break;
    }
    return true;
}

template <typename Pred>
static bool PumpBoth(Server& s, Pred pred, int ms = 12000)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(ms);
    while(std::chrono::steady_clock::now() < deadline)
    {
        if(pred())
        {
            return true;
        }
        ServeOnce(s, 10);
        QCoreApplication::processEvents(QEventLoop::AllEvents,
                                        10);
    }
    return pred();
}

static double AppliedX(LayoutController& c, const QString& id)
{
    const nlohmann::json* o =
        c.ModelForTesting().FindObject(id.toStdString());
    if(!o)
    {
        return -999;
    }
    return o->value("position", nlohmann::json())
        .value("x", -999.0);
}

static void BridgeTests()
{
    auto pipe = std::make_shared<TestPipe>();
    Server          s(pipe->Peer());
    EngineBridge    bridge;

    bridge.setChannelFactoryForTesting(
        [&](std::string& err)
            -> std::unique_ptr<IIpcChannel>
        {
            (void)err;
            return pipe->ClientChannel();
        });
    EngineWireClient::Options wopts;
    wopts.keepalive_ms = 0;
    bridge.setWireOptionsForTesting(wopts);

    LayoutController ctl(&bridge);
    bridge.start();

    std::printf("-- handshake + applied scene\n");
    CHECK(PumpBoth(s, [&] { return ctl.hasScene(); }));
    CHECK(ctl.items().size() == 5);
    CHECK(ctl.canUndo() == false);

    /* the linked copy renders AS a copy — kind linked, no
       emitters, the source's output; it must not appear as a
       third device nor double LED counts */
    {
        const QVariantList items = ctl.items();
        int linked = 0, devices = 0;
        for(const QVariant& v : items)
        {
            const QVariantMap m = v.toMap();
            const QString kind =
                m.value(QStringLiteral("kind")).toString();
            if(kind == QLatin1String("linked"))
            {
                ++linked;
                CHECK(m.value(QStringLiteral("mirror_of"))
                          .toString()
                      == QStringLiteral("fan0"));
                CHECK(m.value(QStringLiteral("emitter_count"))
                          .toInt() == 0);
            }
            else if(kind == QLatin1String("device"))
            {
                ++devices;
            }
        }
        CHECK(linked == 1);
        CHECK(devices == 3);   /* fan0, fan1, ghost — the copy
                                  is NOT counted as a device */
    }

    /*---- move: exact LAYOUT_COMMAND body on the wire ------*/
    std::printf("-- set_position\n");
    ctl.select(QStringLiteral("fan0"));
    const int before = s.layout_commands;
    CHECK(ctl.moveObject(QStringLiteral("fan0"), 0.5, 0.25));
    CHECK(PumpBoth(s, [&] {
        return s.layout_commands == before + 1;
    }));
    {
        const nlohmann::json& b = s.last_layout_body;
        CHECK(b["verb"] == "set_position");
        CHECK(b["object_id"] == "fan0");
        CHECK(b["position"]["x"].get<double>() == 0.5);
        CHECK(b["position"]["z"].get<double>() == 0.25);
        /* y preserved from the applied document */
        CHECK(b["position"]["y"].get<double>() == 0.0);
    }
    CHECK(PumpBoth(s, [&] {
        return AppliedX(ctl, QStringLiteral("fan0")) == 0.5
               && !ctl.busy();
    }));
    CHECK(ctl.canUndo());
    CHECK(ctl.undoLabel() == QStringLiteral("move Fan 0"));

    /*---- undo: whole-document revert via SET_SCENE ---------*/
    std::printf("-- undo\n");
    ctl.undo();
    CHECK(PumpBoth(s, [&] {
        return s.scene_saves == 1 && !ctl.busy();
    }));
    /* the revert body IS the pre-edit document */
    CHECK(s.last_scene_body["document"]["objects"][0]
              ["position"]["x"].get<double>() == 0.0);
    CHECK(PumpBoth(s, [&] {
        return AppliedX(ctl, QStringLiteral("fan0")) == 0.0;
    }));
    CHECK(ctl.canRedo());
    CHECK(!ctl.canUndo());

    /*---- redo replays the parked post-edit document --------*/
    std::printf("-- redo\n");
    ctl.redo();
    CHECK(PumpBoth(s, [&] {
        return s.scene_saves == 2 && !ctl.busy()
               && AppliedX(ctl, QStringLiteral("fan0")) == 0.5;
    }));
    CHECK(ctl.canUndo());
    CHECK(!ctl.canRedo());

    /*---- orientation ----------------------------------------*/
    std::printf("-- set_orientation\n");
    CHECK(ctl.rotateObject(QStringLiteral("fan0"), 90));
    CHECK(PumpBoth(s, [&] {
        return s.last_layout_body.value("verb", "") ==
                   "set_orientation"
               && !ctl.busy();
    }));
    CHECK(s.last_layout_body["rotation"]["y"].get<double>()
          == 90.0);

    /*---- calibration: refused BEFORE the wire ---------------*/
    std::printf("-- calibrate boundary\n");
    {
        const int lc = s.layout_commands;
        const int sc = s.scene_saves;
        CHECK(!ctl.calibrateEmitter(QStringLiteral("fan0"), 0,
                                    40));      /* = zone N   */
        /* a UI-side refusal is a DIFFERENT state than an
           engine rejection — never conflated (Task 4.5) */
        CHECK(ctl.lastErrorSource() ==
              QStringLiteral("input"));
        CHECK(!ctl.calibrateEmitter(QStringLiteral("fan0"), 0,
                                    400));
        CHECK(!ctl.calibrateEmitter(QStringLiteral("fan0"), 9,
                                    0));       /* bad index  */
        /* nothing was sent — the model refused them */
        CHECK(s.layout_commands == lc);
        CHECK(s.scene_saves == sc);
        CHECK(!ctl.lastError().isEmpty());
        CHECK(ctl.lastErrorSource() ==
              QStringLiteral("input"));
    }
    CHECK(ctl.calibrateEmitter(QStringLiteral("fan0"), 0, 39));
    CHECK(PumpBoth(s, [&] {
        return s.last_layout_body.value("verb", "") ==
                   "calibrate_led" && !ctl.busy();
    }));
    CHECK(s.last_layout_body["emitter_index"].get<int>() == 0);
    CHECK(s.last_layout_body["address"].get<int>() == 39);
    CHECK(ctl.lastError().isEmpty());
    CHECK(ctl.lastErrorSource().isEmpty());

    /*---- the SAME out-of-range address refused at the IPC
           boundary too — the UI check is not the only gate.
           A hostile client bypassing LayoutEditModel entirely
           (raw bridge.layoutCommand) meets the scripted
           server's engine-parity dispatch refusal; the real
           engine's identical coverage lives in
           tests/engine/ipc_engine_service_test.cpp
           (calibrate_led). Task 4.5 boundary regression. --*/
    std::printf("-- hostile calibrate bypass\n");
    {
        QString reply_type;
        QString reply_msg;
        int     reply_id = 0;
        QObject::connect(&bridge, &EngineBridge::replyReceived,
            [&](int id, const QString& type,
                const QVariantMap& b)
            {
                reply_id   = id;
                reply_type = type;
                reply_msg  = b.value(QStringLiteral("message"))
                                 .toString();
            });
        QVariantMap hostile;
        hostile.insert(QStringLiteral("object_id"),
                       QStringLiteral("fan0"));
        hostile.insert(QStringLiteral("emitter_index"), 0);
        hostile.insert(QStringLiteral("address"), 40);   /* =N */
        const int req =
            bridge.layoutCommand(QStringLiteral("calibrate_led"),
                                 hostile);
        CHECK(req > 0);
        CHECK(PumpBoth(s, [&] { return reply_id == req; }));
        CHECK(reply_type == QStringLiteral("ERROR"));
        CHECK(reply_msg.contains(QStringLiteral("out of range")));
        /* the applied document is untouched — no ghost write
           (the address stays at the last APPLIED 39, the
           refused 40 never landed) */
        const nlohmann::json* o =
            ctl.ModelForTesting().FindObject("fan0");
        CHECK(o != nullptr);
        if(o)
        {
            CHECK((*o)["emitters"][0]["address"].get<int>()
                  == 39);
        }
        /* and the controller never counted it as an applied
           edit — no ghost undo entry either */
        CHECK(!ctl.busy());
    }

    /*---- linking a mirrored copy ----------------------------*/
    std::printf("-- set_mirror\n");
    CHECK(ctl.setMirrorTarget(QStringLiteral("fan1"),
                              QStringLiteral("fan0")));
    CHECK(PumpBoth(s, [&] {
        return s.last_layout_body.value("verb", "") ==
                   "set_mirror" && !ctl.busy();
    }));
    CHECK(s.last_layout_body["mirror_of"] == "fan0");
    /* the engine echo repaints fan1 as the linked copy */
    CHECK(PumpBoth(s, [&] {
        for(const QVariant& v : ctl.items())
        {
            const QVariantMap m = v.toMap();
            if(m.value(QStringLiteral("id")).toString()
               == QStringLiteral("fan1"))
            {
                return m.value(QStringLiteral("kind"))
                           .toString()
                       == QStringLiteral("linked");
            }
        }
        return false;
    }));

    /*---- a refused op leaves NO ghost undo entry ------------*/
    std::printf("-- error path\n");
    {
        const int undo_before = ctl.undoCount();
        s.fail_next = true;
        CHECK(ctl.moveObject(QStringLiteral("fan0"), 9.0, 9.0));
        CHECK(PumpBoth(s, [&] { return !ctl.busy(); }));
        CHECK(ctl.undoCount() == undo_before);
        CHECK(ctl.lastError() == QStringLiteral("refused by test"));
        /* an ENGINE rejection is a distinct state from the
           local input refusal above (Task 4.5) */
        CHECK(ctl.lastErrorSource() ==
              QStringLiteral("engine"));
        /* the refused doc never touched the applied model */
        CHECK(AppliedX(ctl, QStringLiteral("fan0")) == 0.5);
    }

    /*---- identify: transient, no history ---------------------*/
    std::printf("-- identify\n");
    {
        const int undo_before = ctl.undoCount();
        QVariantMap key;
        key.insert(QStringLiteral("device"),
                   QStringLiteral("Lian Li: SL Wireless"));
        key.insert(QStringLiteral("serial"),
                   QStringLiteral("AABBCCDDEEFF"));
        key.insert(QStringLiteral("location"),
                   QStringLiteral("Wireless: AA:BB:CC:DD:EE:FF"));
        const int req = ctl.identify(key, QStringLiteral("Fan 2"),
                                     7, 4000);
        CHECK(req > 0);
        CHECK(PumpBoth(s, [&] {
            return s.last_layout_body.value("verb", "")
                       == "identify"
                   && !ctl.identifying().isEmpty();
        }));
        CHECK(s.last_layout_body["zone"] == "Fan 2");
        CHECK(s.last_layout_body["led_index"].get<int>() == 7);
        CHECK(s.last_layout_body["duration_ms"].get<int>()
              == 4000);
        CHECK(ctl.undoCount() == undo_before); /* never history */
    }

    bridge.stop();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    std::printf("-- model\n");
    ModelTests();
    std::printf("-- bridge\n");
    BridgeTests();

    std::printf("ui_layout_test: %d checks, %d failures\n",
                checks, failures);
    return failures ? 1 : 0;
}
