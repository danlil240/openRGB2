// SPDX-License-Identifier: GPL-2.0-or-later
//
// StudioViewportModel tests (studio-import Task 7): the
// bounded draft-preview surface over the in-process pipe
// against a scripted engine.
//
//   - draft edits drive preview_begin/preview_update with the
//     compact authoring document + a monotonically increasing
//     draft_revision (latest-wins, never a queue)
//   - chunked frame events reassemble; stale/incomplete
//     sequences drop honestly
//   - reduced-detail frames are labeled, never mistaken for
//     hardware-delivered colors (frame_status stays a
//     separate store)
//   - status events (hidden/display_off) pause the surface
//   - wantPreview/visible toggles send the honest verbs
//   - engine refusal surfaces lastError; engine loss clears
//     the preview and reconnects re-begin it
//
// CHECK-macro main like the other UI suites — no framework.

#include "EngineBridge.h"
#include "StudioViewportModel.h"
#include "WorkspaceController.h"

#include "config/StudioConfig.h"

#include "test_pipe.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QVariantList>
#include <QVariantMap>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace openrgb2;
using namespace openrgb2::ipc;
using namespace openrgb2::ui;
using namespace ui_test;
using nlohmann::json;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

/*---------------------------------------------------------*\
|| A minimal valid compact v3 workspace document.           ||
\*---------------------------------------------------------*/
static json CompactDoc(const std::string& name = "desk")
{
    return json::object({
        { "schema_version", 3 },
        { "name",           name },
        { "devices", json::object({
            { "fan0", json::object({
                { "type", "slw_fan" }, { "x", 0.1 },
                { "y", 0.0 }, { "z", 0.2 },
                { "rx", 0.0 }, { "ry", 0.0 }, { "rz", 0.0 } }) },
        }) },
        { "bindings", json::object() },
        { "device_settings", json::object() },
        { "colors", json::object({
            { "objects", json::object({{ "fan0", "#00B4A0" }}) },
            { "emitters", json::object() },
        }) },
        { "effects", json::object({
            { "preset", "" }, { "seed", 0 },
            { "speed", 1.0 }, { "intensity", 1.0 },
            { "playing", true },
            { "layers", json::array() } }) },
    });
}

static studio::StudioDocument ParseCompact(const json& j)
{
    studio::StudioDocument d;
    std::vector<std::string> errs;
    const bool ok = studio::FromJson(j, d, &errs);
    CHECK(ok);
    if(!ok)
    {
        std::printf("  FromJson: %s\n",
                    errs.empty() ? "?" : errs.front().c_str());
    }
    return d;
}

/*---------------------------------------------------------*\
|| Scripted engine                                          ||
\*---------------------------------------------------------*/
struct PvEngine
{
    explicit PvEngine(ServerPeer p) : srv(std::move(p)) {}

    ServerPeer srv;

    /* captures */
    int    begins  = 0;
    int    updates = 0;
    int    ends    = 0;
    json   last_begin;
    json   last_update;
    bool   refuse_begin = false;

    void SendPreviewEvent(const json& data)
    {
        srv.SendMsg(MsgType::Event, 0, json::object({
            { "kind", "preview" }, { "data", data } }));
    }
    void SendEvent(const std::string& kind, const json& data)
    {
        srv.SendMsg(MsgType::Event, 0, json::object({
            { "kind", kind }, { "data", data } }));
    }
};

static json FrameChunk(uint64_t seq, uint64_t rev, int chunk,
                       int chunks, const json& objects,
                       bool reduced = false, int emitter_total = 8,
                       int emitter_sent = 8, int object_total = 1)
{
    return json::object({
        { "type",           "frame" },
        { "sequence",       seq     },
        { "draft_revision", rev     },
        { "scene_time_s",   0.5 * (double)seq },
        { "chunk",          chunk   },
        { "chunks",         chunks  },
        { "objects",        objects },
        { "reduced",        reduced },
        { "emitter_total",  emitter_total },
        { "emitter_sent",   emitter_sent },
        { "object_total",   object_total },
    });
}

static json Obj(const std::string& id, const json& colors,
                int emitter_total = 2)
{
    return json::object({
        { "id", id },
        { "x", 0.1 }, { "y", 0.0 }, { "z", 0.2 },
        { "rx", 0.0 }, { "ry", 0.0 }, { "rz", 0.0 },
        { "w", 0.12 }, { "h", 0.12 },
        { "emitter_total", emitter_total },
        { "colors", colors },
    });
}

static bool ServePv(PvEngine& e, uint32_t wait_ms = 15)
{
    Message m;
    if(!e.srv.RecvMsg(m, wait_ms))
    {
        return false;
    }
    const json body = m.Body();
    switch(m.type)
    {
    case MsgType::Hello:
        e.srv.SendMsg(MsgType::Welcome, 0, json::object({
            { "server_version",     1 },
            { "negotiated_version", 1 },
            { "channel",            "command" },
            { "engine",             "openrgb2-engine" },
            { "running",            true },
            { "capabilities", json::array({
                  "studio_workspace_v1" }) },
        }));
        break;
    case MsgType::Subscribe:
        e.srv.SendMsg(MsgType::Ok, m.request_id, json::object(
            {{ "subscribed", json::array({ "all" }) }}));
        break;
    case MsgType::WorkspaceCommand:
    {
        const std::string verb = body.value("verb", "");
        if(verb == "preview_begin")
        {
            e.last_begin = body;
            if(e.refuse_begin)
            {
                e.srv.SendMsg(MsgType::Error, m.request_id,
                    json::object({
                        { "code",    (uint32_t)ErrCode::BadRequest },
                        { "error",   "bad_request" },
                        { "message", "document failed to parse" },
                        { "detail",  "invalid_document" } }));
            }
            else
            {
                ++e.begins;
                e.srv.SendMsg(MsgType::Ok, m.request_id,
                    json::object({
                        { "active",         true },
                        { "draft_revision",
                          body.value("draft_revision", 0ull) },
                        { "rate_hz",        30 } }));
            }
        }
        else if(verb == "preview_update")
        {
            ++e.updates;
            e.last_update = body;
            e.srv.SendMsg(MsgType::Ok, m.request_id,
                json::object({
                    { "active",         true },
                    { "draft_revision",
                      body.value("draft_revision", 0ull) } }));
        }
        else if(verb == "preview_end")
        {
            ++e.ends;
            e.srv.SendMsg(MsgType::Ok, m.request_id,
                json::object({{ "active", false }}));
        }
        else if(verb == "get")
        {
            /* Unmanaged engine: the draft lives locally. */
            e.srv.SendMsg(MsgType::Ok, m.request_id,
                json::object({{ "managed", false }}));
        }
        else
        {
            e.srv.SendMsg(MsgType::Ok, m.request_id,
                          json::object());
        }
        break;
    }
    default:
        e.srv.SendMsg(MsgType::Error, m.request_id, json::object({
            { "code", (uint32_t)ErrCode::BadRequest },
            { "error", "bad_request" },
            { "message", "unexpected request" } }));
        break;
    }
    return true;
}

template <typename Pred>
static bool PumpBoth(PvEngine& e, Pred pred, int ms = 12000)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(ms);
    while(std::chrono::steady_clock::now() < deadline)
    {
        if(pred())
        {
            return true;
        }
        ServePv(e, 10);
        QCoreApplication::processEvents(QEventLoop::AllEvents,
                                        10);
    }
    return pred();
}

struct PvRig
{
    std::vector<std::shared_ptr<TestPipe>> pipes;
    PvEngine   engine{ TestPipe().Peer() };
    EngineBridge bridge;
    WorkspaceController ctl{ &bridge };
    std::unique_ptr<StudioViewportModel>  vm;

    PvRig()
    {
        auto p = std::make_shared<TestPipe>();
        pipes.push_back(p);
        engine.srv = p->Peer();
        bridge.setChannelFactoryForTesting(
            [this](std::string& err)
                -> std::unique_ptr<IIpcChannel>
            {
                (void)err;
                auto np = std::make_shared<TestPipe>();
                pipes.push_back(np);
                engine.srv = np->Peer();
                return np->ClientChannel();
            });
        EngineWireClient::Options wopts;
        wopts.keepalive_ms = 0;
        bridge.setWireOptionsForTesting(wopts);
        bridge.setBackoffForTesting({ 25 });
        vm = std::make_unique<StudioViewportModel>(&bridge,
                                                 &ctl);
        vm->setDebounceMsForTesting(0);
    }

    template <typename F> bool PumpUntil(F f, int ms = 12000)
    {
        return PumpBoth(engine, f, ms);
    }

    void LoadDraft(const json& doc)
    {
        ctl.Session().LoadDraft(ParseCompact(doc));
        ctl.NoteDraftEdited();        /* the adapter call */
    }
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    /*========================================================*\
    || Draft -> preview_begin -> frame chunks                 ||
    \*========================================================*/
    std::printf("-- begin + chunked frames + honest labels\n");
    {
        PvRig rig;
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] {
            return rig.bridge.connected();
        }));
        /* No draft -> no preview. */
        CHECK(!rig.vm->previewActive());
        rig.PumpUntil([&] { return false; }, 120);

        rig.LoadDraft(CompactDoc("desk"));
        CHECK(rig.PumpUntil([&] { return rig.engine.begins == 1; }));
        /* The wire verb carried the COMPACT authoring document
           and a positive monotonic draft revision. */
        CHECK(rig.engine.last_begin.value("verb", "")
              == "preview_begin");
        CHECK(rig.engine.last_begin.contains("document"));
        CHECK(rig.engine.last_begin["document"]
                  .value("schema_version", 0) == 3);
        CHECK(rig.engine.last_begin["document"]
                  .value("name", "") == "desk");
        CHECK(rig.engine.last_begin.value("draft_revision", 0ull) >= 1);
        CHECK(rig.PumpUntil([&] { return rig.vm->previewActive(); }));
        CHECK(rig.vm->label().contains("draft",
                                       Qt::CaseInsensitive));
        CHECK(rig.vm->label().contains("not applied",
                                       Qt::CaseInsensitive)
              || rig.vm->label().contains("unsaved",
                                          Qt::CaseInsensitive));

        /* Two-chunk frame, delivered out of order — the model
           reassembles and applies only the complete set. */
        const json a = Obj("fan0", json::array({ "#112233",
                                                 "#445566" }));
        const json b = Obj("free0", json::array({ "#778899" }),
                           1);
        rig.engine.SendPreviewEvent(
            FrameChunk(1, 5, 1, 2, json::array({ b }), false,
                       3, 3, 2));
        CHECK(rig.PumpUntil([&] { return true; }, 80));
        CHECK(rig.vm->sequence() == 0);    /* incomplete held */
        rig.engine.SendPreviewEvent(
            FrameChunk(1, 5, 0, 2, json::array({ a }), false,
                       3, 3, 2));
        CHECK(rig.PumpUntil([&] {
            return rig.vm->sequence() == 1;
        }));
        CHECK(rig.vm->draftRevision() == 5);
        CHECK(rig.vm->objectTotal() == 2);
        const QVariantList objs = rig.vm->objects();
        CHECK(objs.size() == 2);
        CHECK(objs[0].toMap().value("id").toString() == "fan0");
        CHECK(objs[0].toMap().value("colors")
                  .toList()[0].toString() == "#112233");
        CHECK(objs[1].toMap().value("id").toString() == "free0");
        CHECK(!rig.vm->reduced());
        CHECK(rig.vm->emitterTotal() == 3);
    }

    /*========================================================*\
    || Stale/incomplete sequences drop; revisions update      ||
    \*========================================================*/
    std::printf("-- stale drop + latest-wins updates\n");
    {
        PvRig rig;
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] { return rig.bridge.connected(); }));
        rig.LoadDraft(CompactDoc());
        CHECK(rig.PumpUntil([&] { return rig.engine.begins == 1; }));
        CHECK(rig.PumpUntil([&] { return rig.vm->previewActive(); }));

        rig.engine.SendPreviewEvent(FrameChunk(
            9, 2, 0, 1, json::array({ Obj("fan0",
                json::array({ "#FF0000" })) })));
        CHECK(rig.PumpUntil([&] { return rig.vm->sequence() == 9; }));

        /* Older sequence — dropped, counted, no repaint. */
        rig.engine.SendPreviewEvent(FrameChunk(
            7, 2, 0, 1, json::array({ Obj("fan0",
                json::array({ "#00FF00" })) })));
        CHECK(rig.PumpUntil([&] {
            return rig.vm->droppedFrames() >= 1;
        }));
        CHECK(rig.vm->sequence() == 9);

        /* A stranded partial dies when a newer sequence lands. */
        rig.engine.SendPreviewEvent(FrameChunk(
            10, 3, 0, 2, json::array({ Obj("fan0",
                json::array({ "#0000FF" })) })));
        rig.engine.SendPreviewEvent(FrameChunk(
            11, 3, 0, 1, json::array({ Obj("fan0",
                json::array({ "#00FFFF" })) })));
        CHECK(rig.PumpUntil([&] {
            return rig.vm->sequence() == 11;
        }));
        CHECK(rig.vm->droppedFrames() >= 2);

        /* Edits push preview_update with a NEW revision —
           the engine never sees a queue of stale drafts. */
        rig.ctl.Session().CommitDocEdit("move",
            [](studio::StudioDocument& d)
            {
                d.meta.name = "moved";
                return true;
            });
        rig.ctl.NoteDraftEdited();
        CHECK(rig.PumpUntil([&] {
            return rig.engine.updates >= 1;
        }));
        CHECK(rig.engine.last_update.value("verb", "")
              == "preview_update");
        CHECK(rig.engine.last_update["document"]
                  .value("name", "") == "moved");
        CHECK(rig.engine.last_update.value("draft_revision", 0ull)
              > rig.engine.last_begin.value("draft_revision",
                                            0ull));
    }

    /*========================================================*\
    || Reduced labeling + distinctness from frame_status      ||
    \*========================================================*/
    std::printf("-- reduced label + frame_status separation\n");
    {
        PvRig rig;
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] { return rig.bridge.connected(); }));
        rig.LoadDraft(CompactDoc());
        CHECK(rig.PumpUntil([&] { return rig.vm->previewActive(); }));

        json o = Obj("fan0", json::array({ "#123456" }));
        o["indices"] = json::array({ 0 });
        rig.engine.SendPreviewEvent(FrameChunk(
            1, 1, 0, 1, json::array({ o }),
            /*reduced*/true, /*total*/64, /*sent*/1, 1));
        CHECK(rig.PumpUntil([&] { return rig.vm->sequence() == 1; }));
        CHECK(rig.vm->reduced());
        CHECK(rig.vm->label().contains("reduced",
                                       Qt::CaseInsensitive));
        CHECK(rig.vm->emitterTotal() == 64);
        CHECK(rig.vm->emitterSent() == 1);
        const QVariantMap first =
            rig.vm->objects()[0].toMap();
        CHECK(first.value("indices").toList().size() == 1);

        /* Transport-accepted output is a DIFFERENT store —
           draft colors never merge into frame_status and vice
           versa. */
        rig.engine.SendEvent("frame_status", json::object({
            { "colors", json::array({ "#ABCDEF" }) },
        }));
        CHECK(rig.PumpUntil([&] {
            return rig.bridge.frameStatus().contains("colors");
        }));
        CHECK(rig.vm->objects()[0].toMap()
                  .value("colors").toList()[0].toString()
              == "#123456");
        CHECK(rig.bridge.frameStatus()["colors"]
                  .toList()[0].toString() == "#ABCDEF");
    }

    /*========================================================*\
    || Status events + visibility toggle                      ||
    \*========================================================*/
    std::printf("-- status pause/resume + visible verb\n");
    {
        PvRig rig;
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] { return rig.bridge.connected(); }));
        rig.LoadDraft(CompactDoc());
        CHECK(rig.PumpUntil([&] { return rig.vm->previewActive(); }));

        rig.engine.SendPreviewEvent(json::object({
            { "type", "status" }, { "active", true },
            { "paused", true }, { "reason", "hidden" } }));
        CHECK(rig.PumpUntil([&] { return rig.vm->paused(); }));
        CHECK(rig.vm->pausedReason() == "hidden");

        rig.engine.SendPreviewEvent(json::object({
            { "type", "status" }, { "active", true },
            { "paused", true },
            { "reason", "display_off" } }));
        CHECK(rig.PumpUntil([&] {
            return rig.vm->pausedReason() == "display_off";
        }));

        rig.engine.SendPreviewEvent(json::object({
            { "type", "status" }, { "active", true },
            { "paused", false } }));
        CHECK(rig.PumpUntil([&] { return !rig.vm->paused(); }));

        /* Hiding the surface tells the engine once. */
        const int before = rig.engine.updates;
        rig.vm->setVisible(false);
        CHECK(rig.PumpUntil([&] {
            return rig.engine.updates > before;
        }));
        CHECK(rig.engine.last_update.value("visible", true)
              == false);
        rig.vm->setVisible(true);
        CHECK(rig.PumpUntil([&] {
            return rig.engine.last_update.value("visible",
                                                false) == true;
        }));
    }

    /*========================================================*\
    || Visibility change while the begin is in flight         ||
    \*========================================================*/
    std::printf("-- hide before the begin reply is reconciled\n");
    {
        PvRig rig;
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] { return rig.bridge.connected(); }));
        /* LoadDraft sends preview_begin synchronously (visible);
           the surface hides BEFORE the reply is processed. */
        rig.LoadDraft(CompactDoc());
        rig.vm->setVisible(false);
        CHECK(rig.PumpUntil([&] { return rig.vm->previewActive(); }));
        CHECK(rig.engine.last_begin.value("visible", false) == true);
        /* The begin reply re-sends the CURRENT visibility. */
        CHECK(rig.PumpUntil([&] {
            return rig.engine.updates >= 1
                   && rig.engine.last_update.value("visible", true)
                          == false;
        }));
        /* Unchanged visibility is not re-sent. */
        const int n = rig.engine.updates;
        rig.vm->setVisible(false);
        rig.PumpUntil([&] { return false; }, 150);
        CHECK(rig.engine.updates == n);
    }

    /*========================================================*\
    || A burst of edits coalesces into ONE preview push       ||
    \*========================================================*/
    std::printf("-- debounce coalesces a burst\n");
    {
        PvRig rig;
        rig.vm->setDebounceMsForTesting(150);
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] { return rig.bridge.connected(); }));
        rig.LoadDraft(CompactDoc());
        CHECK(rig.PumpUntil([&] { return rig.vm->previewActive(); }));
        /* Let the load's own debounced push land first. */
        rig.PumpUntil([&] { return false; }, 400);
        const int before = rig.engine.updates;
        for(int i = 0; i < 8; ++i)
        {
            rig.ctl.Session().CommitDocEdit("rename",
                [i](studio::StudioDocument& d)
                {
                    d.meta.name = "burst" + std::to_string(i);
                    return true;
                });
            rig.ctl.NoteDraftEdited();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        }
        /* undo emits draftEdited + historyChanged — still one */
        rig.ctl.undo();
        rig.PumpUntil([&] { return false; }, 600);
        CHECK(rig.engine.updates == before + 1);
        CHECK(rig.engine.last_update["document"].value("name", "")
              == "burst6");
    }

    /*========================================================*\
    || wantPreview off -> end; refusal -> lastError            ||
    \*========================================================*/
    std::printf("-- end + refusal honesty\n");
    {
        PvRig rig;
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] { return rig.bridge.connected(); }));
        rig.LoadDraft(CompactDoc());
        CHECK(rig.PumpUntil([&] { return rig.vm->previewActive(); }));
        rig.vm->setWantPreview(false);
        CHECK(rig.PumpUntil([&] { return rig.engine.ends == 1; }));
        CHECK(!rig.vm->previewActive());
        /* Re-enable — a fresh begin. */
        rig.vm->setWantPreview(true);
        CHECK(rig.PumpUntil([&] { return rig.engine.begins == 2; }));

        /* Engine refusal surfaces honestly, preview inactive. */
        rig.engine.refuse_begin = true;
        rig.vm->setWantPreview(false);
        CHECK(rig.PumpUntil([&] { return rig.engine.ends >= 2; }));
        rig.vm->setWantPreview(true);
        CHECK(rig.PumpUntil([&] {
            return !rig.vm->lastError().isEmpty();
        }));
        CHECK(!rig.vm->previewActive());

        /* The refusal LATCHES: re-evaluating the want state
           does not retry against a rejecting engine. */
        const int refused_begins = rig.engine.begins;
        rig.vm->setWantPreview(false);
        rig.vm->setWantPreview(true);
        rig.PumpUntil([&] { return false; }, 400);   /* drain */
        CHECK(rig.engine.begins == refused_begins);
        CHECK(!rig.vm->previewActive());
        /* A NEW draft clears the latch — the next edit sends a
           fresh candidate, not a retry of the refused one. */
        rig.engine.refuse_begin = false;
        rig.ctl.Session().CommitDocEdit("rename",
            [](studio::StudioDocument& d)
            {
                d.meta.name = "retry";
                return true;
            });
        rig.ctl.NoteDraftEdited();
        CHECK(rig.PumpUntil([&] {
            return rig.engine.begins > refused_begins;
        }));
        CHECK(rig.PumpUntil([&] {
            return rig.vm->previewActive();
        }));
    }

    /*========================================================*\
    || Engine loss clears; reconnect re-begins                 ||
    \*========================================================*/
    std::printf("-- engine loss + reconnect\n");
    {
        PvRig rig;
        rig.bridge.start();
        CHECK(rig.PumpUntil([&] { return rig.bridge.connected(); }));
        rig.LoadDraft(CompactDoc());
        CHECK(rig.PumpUntil([&] { return rig.vm->previewActive(); }));
        rig.engine.SendPreviewEvent(FrameChunk(
            1, 1, 0, 1, json::array({ Obj("fan0",
                json::array({ "#102030" })) })));
        CHECK(rig.PumpUntil([&] { return rig.vm->sequence() == 1; }));

        rig.engine.srv.Close();
        CHECK(rig.PumpUntil([&] {
            return !rig.bridge.connected();
        }));
        CHECK(rig.PumpUntil([&] {
            return !rig.vm->previewActive();
        }));
        CHECK(rig.vm->sequence() == 0);
        CHECK(rig.vm->objects().isEmpty());

        /* Reconnect -> the model re-begins with the CURRENT
           draft, not a replayed one. */
        CHECK(rig.PumpUntil([&] {
            return rig.bridge.connected();
        }, 20000));
        CHECK(rig.PumpUntil([&] {
            return rig.engine.begins >= 1;
        }));
    }

    std::printf("ui studio preview: %d checks, %d failures\n",
                checks, failures);
    return failures ? 1 : 0;
}
