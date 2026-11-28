// SPDX-License-Identifier: GPL-2.0-or-later
//
// ui_studio_effects_test — studio-import Task 6 suite, adapted
// from the source-of-record editor QML tests
// (tests/editor_qml/tst_effects.qml) onto the OpenRGB2
// architecture:
//
//   Part A — the REAL C++ surface offline (no engine, no
//   display): EngineBridge + WorkspaceController with a loaded
//   compact draft, driving EffectEditorController → the shared
//   WorkspaceEditSession. Asserts the effective-stack read
//   (authored inline layers, else resolved named look), first-
//   edit materialization + undo back to preset resolution,
//   ordered add/remove/reorder/enable, blend/opacity, scalar
//   fields, space, origin/direction, source, targets, palette
//   stop insert/move/color/delete, path points, one-record-per-
//   gesture, reset/remix/seed semantics, persisted inputs
//   (decay/sensitivity/enable/screen index), refusal of invalid
//   ids/fields/stop collisions, and saveLookAs's honest
//   offline/validation refusals.
//
//   Part B — the EffectLayerModel row-diff contract (the
//   adapted source-of-record model): same-shape edits emit
//   row-granular dataChanged, contiguous add/remove emit
//   insert/remove, non-contiguous rewrites reset.
//
//   Part C — the adapted QML panels load headless (parse +
//   degrade honesty without a controller).
//
// DLS_EFFECT_DIR (set by CMake) points the look registry probe
// at src/core/presets/effects so the packaged looks resolve.

#include "EngineBridge.h"
#include "JsonVariant.h"          /* JsonToVariant */
#include "WorkspaceController.h"
#include "WorkspaceEditSession.h"
#include "studio/EffectEditorController.h"
#include "studio/EffectLayerModel.h"

#include "config/StudioConfig.h"
#include "effects/EffectJson.h"
#include "effects/EffectTypes.h"
#include "effects/Presets.h"           /* FindPreset / PresetList */
#include "presets/EffectRegistry.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QPointer>
#include <QTest>
#include <QSignalSpy>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

using namespace openrgb2;
using namespace openrgb2::ui;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

/*---------------------------------------------------------*\
|| Fixture — a compact v3 draft naming the packaged look   ||
|| "aurora" with no authored layers (registry resolution), ||
|| plus a couple of device instances for target ids.       ||
\*---------------------------------------------------------*/
static nlohmann::json CompactDoc(const std::string& preset)
{
    return nlohmann::json::object({
        { "schema_version", 3 },
        { "name",           "desk" },
        { "devices", nlohmann::json::object({
            { "fan0", nlohmann::json::object({
                { "type", "fan-slw" }, { "x", 0.1 },
                { "y", 0.0 }, { "z", 0.2 } }) },
        }) },
        { "bindings", nlohmann::json::object() },
        { "device_settings", nlohmann::json::object() },
        { "effects", nlohmann::json::object({
            { "preset", preset }, { "seed", 7 }, { "speed", 1.0 },
            { "intensity", 1.0 }, { "playing", true },
            { "layers", nlohmann::json::array() } }) },
    });
}

static studio::StudioDocument ParseCompact(const nlohmann::json& j)
{
    studio::StudioDocument d;
    std::vector<std::string> errs;
    const bool ok = studio::FromJson(j, d, &errs);
    if(!ok)
    {
        std::printf("  FromJson errors: %s\n",
                    errs.empty() ? "?" : errs.front().c_str());
    }
    CHECK(ok);
    return d;
}

/* A draft-owning rig: real bridge (never connected), real
   WorkspaceController, real controller on the shared session. */
struct Rig
{
    EngineBridge             bridge;
    WorkspaceController      ws{ &bridge };
    EffectEditorController   ctl{ &bridge, &ws };

    const studio::StudioDocument& Working() const
        { return ws.Session().Working(); }
    size_t UndoCount() const { return ws.Session().UndoCount(); }

    void LoadDoc(const nlohmann::json& j)
    {
        ws.Session().LoadApplied(ParseCompact(j), 1, 1);
    }
};

/*---------------------------------------------------------*\
|| Helpers                                                  ||
\*---------------------------------------------------------*/

static QStringList RowPrimitives(EffectEditorController& ctl)
{
    QStringList out;
    auto* m = static_cast<EffectLayerModel*>(ctl.layerModel());
    for(int i = 0; i < m->count(); ++i)
    {
        out << m->rowAt(i).value("primitive").toString();
    }
    return out;
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    const char* effect_dir = std::getenv("DLS_EFFECT_DIR");
    CHECK(effect_dir != nullptr && effect_dir[0] != '\0');
    /* Force the registry probe now — EffectLooks() caches the
       defaults layer on first access. */
    CHECK(studio::FindPreset("aurora").has_value());

    /*======================================================*\
    || A. EffectEditorController over the shared session    ||
    \*======================================================*/
    std::printf("-- effective stack resolves the named look\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc("aurora"));
        CHECK(rig.ctl.ready());
        CHECK(rig.ctl.presetId() == QStringLiteral("aurora"));
        CHECK(!rig.ctl.hasInlineLayers());
        /* The model answers the RESOLVED stack — authored inline
           layers are empty, so the look's layers show. */
        const auto eff = rig.ctl.EffectiveLayersForTesting();
        CHECK(!eff.empty());
        CHECK(rig.ctl.layerCount() == (int)eff.size());
    }

    std::printf("-- first edit materializes; undo returns to "
                "preset resolution\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc("aurora"));
        const size_t resolved =
            rig.ctl.EffectiveLayersForTesting().size();
        CHECK(rig.UndoCount() == 0);

        /* One discrete edit materializes the resolved stack into
           authored inline layers INSIDE the same record. */
        rig.ctl.setLayerOpacity(0, 0.5);
        CHECK(rig.ctl.hasInlineLayers());
        CHECK(rig.Working().effect.layers.size() == resolved);
        CHECK(rig.Working().effect.preset == "aurora");  /* provenance */
        CHECK(rig.UndoCount() == 1);
        CHECK(std::fabs(rig.Working().effect.layers[0].opacity
                        - 0.5f) < 1e-4);

        rig.ctl.undo();
        CHECK(rig.Working().effect.layers.empty());
        CHECK(!rig.ctl.hasInlineLayers());
        CHECK(rig.ctl.layerCount() == (int)resolved);
    }

    std::printf("-- ordered add/remove/reorder/enable\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc("aurora"));
        /* Materialize, then reorder/remove. */
        rig.ctl.addLayer("wave");      /* materialize + append */
        CHECK(rig.ctl.hasInlineLayers());
        const int n = rig.ctl.layerCount();
        rig.ctl.addLayer("ripple");
        CHECK(rig.ctl.layerCount() == n + 1);
        QStringList prims = RowPrimitives(rig.ctl);
        CHECK(prims.last() == QStringLiteral("ripple"));

        rig.ctl.moveLayer(n, 0);       /* ripple to the front */
        prims = RowPrimitives(rig.ctl);
        CHECK(prims.first() == QStringLiteral("ripple"));

        rig.ctl.setLayerEnabled(0, false);
        CHECK(rig.ctl.layerAt(0).value("enabled").toBool() == false);

        rig.ctl.removeLayer(0);
        CHECK(rig.ctl.layerCount() == n);
        prims = RowPrimitives(rig.ctl);
        CHECK(prims.first() != QStringLiteral("ripple"));

        /* Out-of-range ops are refused without history or
           mutation. */
        const size_t recs = rig.UndoCount();
        rig.ctl.removeLayer(999);
        rig.ctl.setLayerEnabled(999, true);
        rig.ctl.moveLayer(999, 0);
        CHECK(rig.UndoCount() == recs);
        CHECK(rig.ctl.layerCount() == n);
    }

    std::printf("-- blend/opacity/fields/space/origin/"
                "direction/source/targets\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc("aurora"));
        rig.ctl.setLayerBlend(0, "add");
        rig.ctl.setLayerOpacity(0, 0.7);
        rig.ctl.setLayerField(0, "speed", 2.5);
        rig.ctl.setLayerField(0, "scale", 0.9);
        rig.ctl.setLayerField(0, "phase", 0.25);
        rig.ctl.setLayerField(0, "density", 3.0);
        rig.ctl.setLayerSpace(0, "local");
        rig.ctl.setLayerOrigin(0, 0.1, 0.2, 0.3);
        rig.ctl.setLayerDirection(0, 0.0, 1.0, 0.0);
        rig.ctl.setLayerSource(0, "key");
        rig.ctl.setLayerTargets(0, QStringList{"fan0"});

        const QVariantMap l = rig.ctl.layerAt(0);
        CHECK(l.value("blend").toString() == "add");
        CHECK(std::fabs(l.value("opacity").toDouble() - 0.7) < 1e-4);
        CHECK(std::fabs(l.value("speed").toDouble() - 2.5) < 1e-4);
        CHECK(std::fabs(l.value("scale").toDouble() - 0.9) < 1e-4);
        CHECK(std::fabs(l.value("phase").toDouble() - 0.25) < 1e-4);
        CHECK(std::fabs(l.value("density").toDouble() - 3.0) < 1e-4);
        CHECK(l.value("space").toString() == "local");
        CHECK(std::fabs(l.value("origin").toMap()
                            .value("x").toDouble() - 0.1) < 1e-4);
        CHECK(std::fabs(l.value("direction").toMap()
                            .value("y").toDouble() - 1.0) < 1e-4);
        CHECK(l.value("source").toString() == "key");
        CHECK(l.value("targets").toStringList()
              == QStringList{"fan0"});

        /* Invalid names refused — no record, no crash. */
        const size_t recs = rig.UndoCount();
        QSignalSpy status(&rig.ctl,
                          &EffectEditorController::statusMessage);
        rig.ctl.setLayerBlend(0, "multiply");
        rig.ctl.setLayerField(0, "bogus", 1.0);
        rig.ctl.setLayerSpace(0, "screen");
        rig.ctl.setLayerSource(0, "midi");
        rig.ctl.addLayer("notaprimitive");
        CHECK(status.count() >= 4);
        CHECK(rig.UndoCount() == recs);
    }

    std::printf("-- palette stops: insert/move/color/delete\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc("aurora"));
        const int nstops =
            rig.ctl.layerAt(0).value("palette").toList().size();
        CHECK(nstops >= 1);

        rig.ctl.addLayerStop(0, 0.5, QColor("#FF0000"));
        QVariantList pal =
            rig.ctl.layerAt(0).value("palette").toList();
        CHECK(pal.size() == nstops + 1);
        /* Sorted — find the inserted stop. */
        int at = -1;
        for(int i = 0; i < pal.size(); ++i)
        {
            if(std::fabs(pal[i].toMap().value("pos").toDouble()
                         - 0.5) < 1e-4)
            {
                at = i;
            }
        }
        CHECK(at >= 0);
        CHECK(pal[at].toMap().value("color").toString()
              == "#FF0000");

        rig.ctl.setLayerStopColor(0, at, QColor("#00FF00"));
        pal = rig.ctl.layerAt(0).value("palette").toList();
        CHECK(pal[at].toMap().value("color").toString()
              == "#00FF00");

        /* Move returns the stop's index after the re-sort. */
        const int moved = rig.ctl.moveLayerStop(0, at, 0.05);
        CHECK(moved >= 0);
        pal = rig.ctl.layerAt(0).value("palette").toList();
        CHECK(std::fabs(pal[moved].toMap()
                            .value("pos").toDouble() - 0.05) < 1e-4);
        /* Sorted invariant holds. */
        for(int i = 1; i < pal.size(); ++i)
        {
            CHECK(pal[i].toMap().value("pos").toDouble() >
                  pal[i - 1].toMap().value("pos").toDouble());
        }

        /* A colliding position is refused. */
        const double other =
            pal[(moved + 1) % pal.size()].toMap()
                .value("pos").toDouble();
        const size_t recs = rig.UndoCount();
        rig.ctl.moveLayerStop(0, moved, other);
        CHECK(rig.UndoCount() == recs);

        rig.ctl.removeLayerStop(0, moved);
        CHECK(rig.ctl.layerAt(0).value("palette").toList().size()
              == nstops);
    }

    std::printf("-- path points\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc("aurora"));
        rig.ctl.addLayerPathPoint(0, 0.1, 0.0, 0.2);
        rig.ctl.addLayerPathPoint(0, 0.3, 0.0, 0.4);
        QVariantList path =
            rig.ctl.layerAt(0).value("path").toList();
        CHECK(path.size() == 2);
        rig.ctl.setLayerPathPoint(0, 1, 0.5, 0.1, 0.6);
        path = rig.ctl.layerAt(0).value("path").toList();
        CHECK(std::fabs(path[1].toMap()
                            .value("x").toDouble() - 0.5) < 1e-4);
        rig.ctl.removeLayerPathPoint(0, 0);
        CHECK(rig.ctl.layerAt(0).value("path").toList().size() == 1);
    }

    std::printf("-- a slider scrub is ONE history record\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc("aurora"));
        rig.ctl.beginLayerGesture();
        CHECK(rig.ctl.gestureActive());
        for(int i = 1; i <= 7; ++i)
        {
            rig.ctl.setLayerField(0, "speed", 0.1 * i);
        }
        rig.ctl.commitLayerGesture("speed scrub");
        CHECK(!rig.ctl.gestureActive());
        CHECK(rig.UndoCount() == 1);
        CHECK(std::fabs(rig.Working().effect.layers[0].speed
                        - 0.7f) < 1e-4);
        rig.ctl.undo();
        /* Undo restores pre-gesture state — which was the
           MATERIALIZED stack already? No: the gesture's begin
           snapshot predates materialization, so one undo returns
           to registry resolution. */
        CHECK(rig.Working().effect.layers.empty());
    }

    std::printf("-- a scrub that returns to start records "
                "nothing\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc("aurora"));
        /* aurora layer 0 resolves speed 1.0 — scrub away and back
           leaves no net change and no history record, and the
           materialize-only begin is rolled back (source parity). */
        rig.ctl.beginLayerGesture();
        rig.ctl.setLayerField(0, "speed", 0.5);
        rig.ctl.setLayerField(0, "speed", 1.0);
        rig.ctl.commitLayerGesture("round trip");
        CHECK(rig.UndoCount() == 0);
        CHECK(!rig.ctl.hasInlineLayers());
    }

    std::printf("-- gesture cancel leaves no record\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc("aurora"));
        rig.ctl.beginLayerGesture();
        rig.ctl.setLayerField(0, "speed", 9.0);
        rig.ctl.cancelLayerGesture();
        CHECK(rig.UndoCount() == 0);
        CHECK(!rig.ctl.hasInlineLayers());
    }

    std::printf("-- Ctrl+Z mid-scrub is refused; a reset cancels "
                "the scrub (audit C-6)\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc("aurora"));
        rig.ctl.setLayerOpacity(0, 0.5);          /* record 1 */
        rig.ctl.beginLayerGesture();
        rig.ctl.setLayerField(0, "speed", 3.0);
        rig.ctl.undo();                           /* held slider */
        CHECK(rig.ctl.gestureActive());
        CHECK(rig.UndoCount() == 1);
        rig.ctl.commitLayerGesture("speed scrub");
        CHECK(rig.UndoCount() == 2);
        rig.ctl.undo();
        /* One undo takes back the scrub only. */
        CHECK(rig.ctl.hasInlineLayers());
        CHECK(std::fabs(rig.Working().effect.layers[0].opacity
                        - 0.5f) < 1e-4);
        CHECK(std::fabs(rig.Working().effect.layers[0].speed
                        - 3.0f) > 1e-3);

        /* Discarding the draft mid-scrub cancels the gesture and
           tells QML. */
        QSignalSpy gest(&rig.ctl,
                        &EffectEditorController::gestureChanged);
        rig.ctl.beginLayerGesture();
        rig.ctl.setLayerField(0, "speed", 4.0);
        gest.clear();
        rig.ws.resetEdits();
        CHECK(!rig.ctl.gestureActive());
        CHECK(gest.count() >= 1);
        CHECK(!rig.ctl.hasInlineLayers());
        rig.ctl.commitLayerGesture("late release");
        CHECK(rig.UndoCount() == 0);
    }

    std::printf("-- selectPreset: stack cleared + fresh seed, "
                "undoable\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc("aurora"));
        rig.ctl.setLayerOpacity(0, 0.5);     /* author a stack */
        CHECK(rig.ctl.hasInlineLayers());
        const size_t before = rig.UndoCount();

        CHECK(rig.ctl.selectPreset("comet"));
        CHECK(rig.ctl.presetId() == QStringLiteral("comet"));
        CHECK(!rig.ctl.hasInlineLayers());
        CHECK(rig.Working().effect.seed == 0);   /* fresh roll */
        CHECK(rig.Working().effect.layers.empty());
        /* Undo restores stack + preset + seed together. */
        rig.ctl.undo();
        CHECK(rig.ctl.presetId() == QStringLiteral("aurora"));
        CHECK(rig.ctl.hasInlineLayers());
        CHECK(rig.Working().effect.seed == 7);
        (void)before;
    }

    std::printf("-- resetToPreset clears the authored stack\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc("aurora"));
        rig.ctl.setLayerOpacity(0, 0.5);
        CHECK(rig.ctl.hasInlineLayers());
        CHECK(rig.ctl.resetToPreset());
        CHECK(!rig.ctl.hasInlineLayers());
        CHECK(rig.ctl.presetId() == QStringLiteral("aurora"));
        /* Already-resolved: a second reset is a no-op. */
        CHECK(!rig.ctl.resetToPreset());
    }

    std::printf("-- remix advances the seed deterministically\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc("aurora"));
        const unsigned int s0 = rig.Working().effect.seed;
        CHECK(rig.ctl.remix());
        const unsigned int s1 = rig.Working().effect.seed;
        CHECK(s1 == studio::HashU32(s0 ^ 0x5D15A5E9u) + 1u);
        /* No preset → refused. */
        Rig rig2;
        rig2.LoadDoc(CompactDoc(""));
        CHECK(!rig2.ctl.remix());
    }

    std::printf("-- flowState: the Wave card only drives untargeted "
                "world-space waves\n");
    {
        /* Reactor's waves are a local-space RAM wave and a
           keyboard-targeted wave: a desk direction written onto
           either turns it into a uniform flash, so the card is
           not offered (audit C-4). */
        Rig rig;
        rig.LoadDoc(CompactDoc("reactor"));
        CHECK(rig.ctl.flowState().value("available").toBool() == false);
        CHECK(!rig.ctl.setFlowDirection(0, 0, -1));
        CHECK(rig.UndoCount() == 0);
        CHECK(!rig.ctl.hasInlineLayers());

        Rig rig2;
        rig2.LoadDoc(CompactDoc("aurora"));
        CHECK(rig2.ctl.flowState().value("available").toBool());
    }

    std::printf("-- flowState follows the first flow layer, enabled "
                "or not; speed is a magnitude\n");
    {
        /* Aurora: [0] static, [1] wave (4 stops), [2] noise. */
        Rig rig;
        rig.LoadDoc(CompactDoc("aurora"));
        rig.ctl.setLayerEnabled(1, false);
        QVariantMap f = rig.ctl.flowState();
        CHECK(f.value("available").toBool());
        CHECK(f.value("colors").toStringList().size() == 4);
        rig.ctl.setLayerField(1, "speed", -0.2);
        f = rig.ctl.flowState();
        CHECK(std::fabs(f.value("speed").toDouble() - 0.2) < 1e-4);
        CHECK(rig.ctl.setFlowSpeed(0.3));
        CHECK(std::fabs(rig.Working().effect.layers[1].speed + 0.3f)
              < 1e-4);
    }

    std::printf("-- Wave card adapter: flowState / setFlow* "
                "(audit U-12)\n");
    {
        /* The packaged Wave look: [0] colour flow (4 stops, z -1,
           1.25 m, 0.12 m/s), [1] screen swell, [2] keyboard
           white. */
        Rig rig;
        rig.LoadDoc(CompactDoc("wave"));
        QVariantMap f = rig.ctl.flowState();
        CHECK(f.value("available").toBool());
        const QVariantMap dir = f.value("direction").toMap();
        CHECK(std::fabs(dir.value("z").toDouble() + 1.0) < 1e-4);
        CHECK(std::fabs(f.value("speed").toDouble() - 0.12) < 1e-4);
        CHECK(std::fabs(f.value("wavelength").toDouble() - 1.25) < 1e-4);
        CHECK(f.value("colors").toStringList().size() == 4);
        CHECK(f.value("keyboardLayer").toInt() == 2);
        CHECK(f.value("keyboardOn").toBool());

        /* Direction: one record over both waves, undoable. */
        CHECK(rig.ctl.setFlowDirection(2, 0, 0));
        CHECK(rig.UndoCount() == 1);
        CHECK(std::fabs(rig.Working().effect.layers[0].direction.x
                        - 1.0f) < 1e-4);
        CHECK(std::fabs(rig.Working().effect.layers[1].direction.x
                        - 1.0f) < 1e-4);
        CHECK(std::fabs(rig.ctl.flowState().value("direction").toMap()
                            .value("x").toDouble() - 1.0) < 1e-4);
        rig.ctl.undo();
        CHECK(!rig.ctl.hasInlineLayers());

        /* Speed / wavelength rescale proportionally. */
        CHECK(rig.ctl.setFlowSpeed(0.24));
        CHECK(std::fabs(rig.Working().effect.layers[1].speed - 0.30f)
              < 1e-4);
        CHECK(rig.ctl.setFlowWavelength(2.5));
        CHECK(std::fabs(rig.Working().effect.layers[1].scale - 1.8f)
              < 1e-4);

        /* Every count flowState can show is accepted back: a
           6th swatch, then recolouring one of six. */
        QStringList cols = rig.ctl.flowState().value("colors")
                               .toStringList();
        cols << "#FF0000" << "#00FF00";
        CHECK(rig.ctl.setFlowColors(cols));
        cols = rig.ctl.flowState().value("colors").toStringList();
        CHECK(cols.size() == 6);
        while(cols.size() < 6)
        {
            cols << "#FFFFFF";
        }
        cols[5] = "#0000FF";
        CHECK(rig.ctl.setFlowColors(cols));
        CHECK(rig.ctl.flowState().value("colors").toStringList()
                  .value(5).compare("#0000FF", Qt::CaseInsensitive)
              == 0);
        /* ...and a one-colour palette can be recoloured. */
        CHECK(rig.ctl.setFlowColors({ "#FFFFFF" }));
        CHECK(rig.ctl.flowState().value("colors").toStringList()
                  .size() == 1);
        CHECK(rig.ctl.setFlowColors({ "#123456" }));

        /* Refusals are honest: a status line, no record. */
        const size_t recs = rig.UndoCount();
        QSignalSpy status(&rig.ctl,
                          &EffectEditorController::statusMessage);
        CHECK(!rig.ctl.setFlowColors({}));
        CHECK(!rig.ctl.setFlowColors({ "#FF0000", "not-a-color" }));
        QStringList many;
        for(unsigned int k = 0; k <= studio::EFFECT_MAX_STOPS; ++k)
        {
            many << "#FF0000";
        }
        CHECK(!rig.ctl.setFlowColors(many));
        CHECK(!rig.ctl.setFlowSpeed(-1.0));
        CHECK(!rig.ctl.setFlowDirection(0, 0, 0));
        CHECK(status.count() == 5);
        CHECK(rig.UndoCount() == recs);

        /* Mid-gesture the card is refused — with a status line,
           not silently. */
        status.clear();
        rig.ctl.beginLayerGesture();
        CHECK(!rig.ctl.setFlowSpeed(0.4));
        CHECK(status.count() == 1);
        rig.ctl.cancelLayerGesture();
        CHECK(rig.UndoCount() == recs);
    }

    std::printf("-- persisted input settings (draft inputs.*)\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc("aurora"));
        rig.ctl.setDecayPct(150);
        CHECK(rig.Working().inputs.decay_pct == 150);
        CHECK(rig.ctl.decayPct() == 150);
        rig.ctl.setDecayPct(999);        /* clamps to 300 */
        CHECK(rig.Working().inputs.decay_pct == 300);
        rig.ctl.setDecayPct(0);          /* clamps to 50 */
        CHECK(rig.Working().inputs.decay_pct == 50);

        rig.ctl.setInputEnabled("audio", true);
        rig.ctl.setInputEnabled("key", true);
        CHECK(rig.Working().inputs.audio);
        CHECK(rig.Working().inputs.keys);
        rig.ctl.setInputSensitivity(175);
        CHECK(rig.Working().inputs.sens_pct == 175);
        rig.ctl.setInputScreenIndex(2);
        CHECK(rig.Working().inputs.screen_index == 2);
        /* Same-value writes leave no record. */
        const size_t recs = rig.UndoCount();
        rig.ctl.setDecayPct(50);            /* already 50 */
        rig.ctl.setDecayPct(50);
        rig.ctl.setInputEnabled("audio", true);
        rig.ctl.setInputScreenIndex(2);     /* already 2 */
        CHECK(rig.UndoCount() == recs);
        /* One genuine change still records once. */
        rig.ctl.setInputScreenIndex(1);
        CHECK(rig.UndoCount() == recs + 1);
    }

    std::printf("-- saveLookAs refusals are honest\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc("aurora"));
        /* Bad id. */
        QVariantMap r = rig.ctl.saveLookAs("bad id!", "x");
        CHECK(r.value("ok").toBool() == false);
        /* No connection — refused before the wire. */
        r = rig.ctl.saveLookAs("my_look", "My Look");
        CHECK(r.value("ok").toBool() == false);
        /* Mid-gesture refused. */
        rig.ctl.beginLayerGesture();
        r = rig.ctl.saveLookAs("my_look", "My Look");
        CHECK(r.value("ok").toBool() == false);
        rig.ctl.cancelLayerGesture();
    }

    std::printf("-- inputSourceState is honest without engine "
                "state\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc("aurora"));
        const QVariantMap st = rig.ctl.inputSourceState("audio");
        CHECK(st.value("enabled").toBool() == false);
        CHECK(st.value("ready").toBool() == false);
        CHECK(rig.ctl.inputSourceState("bogus")
                  .value("ready").toBool() == false);
    }

    std::printf("-- registered personal look resolves\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc("my_look"));
        /* Unresolvable id → empty effective stack, honest. */
        CHECK(rig.ctl.layerCount() == 0);
        studio::EffectDocument d;
        d.id   = "my_look";
        d.name = "My Look";
        nlohmann::ordered_json l;
        l["primitive"] = "wave";
        l["opacity"]   = 0.8;
        d.layers.push_back(l);
        rig.ctl.RegisterLookForTesting(d);
        CHECK(rig.ctl.layerCount() == 1);
        const QVariantMap row = rig.ctl.layerAt(0);
        CHECK(row.value("primitive").toString() == "wave");
    }

    std::printf("-- catalog feed: a saved look resolves in a "
                "fresh session\n");
    {
        /* A fresh session has no same-session save — the look
           only resurfaces because the catalog's file-supplied
           entry carries its document (the restart path). */
        Rig rig;
        studio::EffectDocument d;
        d.id   = "my_look";
        d.name = "My Look";
        nlohmann::ordered_json l;
        l["primitive"] = "ripple";
        l["opacity"]   = 0.6;
        d.layers.push_back(l);

        QVariantMap e;
        e["id"] = "my_look"; e["name"] = "My Look";
        e["needs"] = ""; e["from_file"] = true;
        /* document_text preserves the ordered_json field order
           (a re-keyed object would diverge remix RNG draws). */
        e["document_text"] = QString::fromStdString(
            studio::EffectDocumentToJson(d).dump());
        /* A file-supplied row under a PACKAGED id is not a
           personal look — the compiled-in id set filters it
           (bridge_->presets() merges file looks, so it cannot
           serve as the filter). */
        QVariantMap pk;
        pk["id"] = "aurora"; pk["name"] = "Aurora";
        pk["needs"] = ""; pk["from_file"] = true;
        rig.ctl.FeedEffectsCatalogForTesting({ e, pk });

        const QVariantList looks = rig.ctl.looks();
        CHECK(looks.size() == 1);
        CHECK(looks.first().toMap().value("id").toString()
                  == "my_look");
        CHECK(looks.first().toMap().value("name").toString()
                  == "My Look");

        /* Resolution survives: a draft naming the look gets the
           real stack, and the materialize/edit contract holds. */
        rig.LoadDoc(CompactDoc("my_look"));
        CHECK(rig.ctl.layerCount() == 1);
        CHECK(rig.ctl.layerAt(0).value("primitive").toString()
                  == "ripple");
        rig.ctl.setLayerOpacity(0, 0.5);
        CHECK(rig.ctl.hasInlineLayers());
    }

    std::printf("-- disconnect mid-save unlatches + fails "
                "honestly\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc("aurora"));
        QSignalSpy results(&rig.ctl,
            &EffectEditorController::lookSaveResult);
        rig.ctl.SetSavingForTesting();
        CHECK(rig.ctl.savingLook());
        /* Emitting the signal directly exercises the same
           disconnect branch a real connection drop runs. */
        emit rig.bridge.connectionStateChanged();
        CHECK(!rig.ctl.savingLook());
        CHECK(results.count() == 1);
        if(results.count() == 1)
        {
            const QVariantMap res =
                results.first().first().toMap();
            CHECK(res.value("ok").toBool() == false);
            CHECK(res.value("id").toString() == "test_look");
        }
    }

    std::printf("-- 'succeeded' terminal job resolves the look "
                "save\n");
    {
        /* The engine's terminal state name is "succeeded" — a
           record that spells it must unlatch savingLook,
           report ok, and re-request the catalog (previously
           only the legacy "completed" alias was accepted, so
           the save hung forever — F07). */
        Rig rig;
        rig.LoadDoc(CompactDoc("aurora"));
        QSignalSpy results(&rig.ctl,
            &EffectEditorController::lookSaveResult);
        rig.ctl.SetSavingForTesting();
        CHECK(rig.ctl.savingLook());
        const int reqs_before = rig.ctl.CatalogRequestsForTesting();
        /* Emitting through the bridge signal exercises the real
           dispatch — the same payload the wire delivers. */
        emit rig.bridge.eventReceived(
            QStringLiteral("workspace"),
            QVariantMap({
                { "job_id", 7 },          /* matches the latched job */
                { "verb",   "resource_install" },
                { "state",  "succeeded" },
                { "committed", false },
                { "result", QVariantMap{
                      { "landed_id", "test_look" } } },
            }));
        CHECK(!rig.ctl.savingLook());
        CHECK(results.count() == 1);
        if(results.count() == 1)
        {
            const QVariantMap res =
                results.first().first().toMap();
            CHECK(res.value("ok").toBool() == true);
            CHECK(res.value("id").toString() == "test_look");
        }
        /* The shelf re-requested the effects catalog. */
        CHECK(rig.ctl.CatalogRequestsForTesting() ==
              reqs_before + 1);
        /* The landed look was adopted through the same one
           undoable record the connected path uses: the stack
           is a preset REFERENCE to the locally-registered
           document (the effective aurora stack it serialized),
           not inline layers. */
        CHECK(rig.ctl.presetId() == QStringLiteral("test_look"));
        CHECK(!rig.ctl.hasInlineLayers());
        CHECK(rig.ctl.layerCount() ==
              (int)rig.ctl.EffectiveLayersForTesting().size());
        CHECK(rig.ctl.layerCount() > 0);
        CHECK(rig.UndoCount() == 1);
    }

    std::printf("-- addLayer refused keeps selection\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc("aurora"));
        rig.ctl.setSelectedLayer(0);
        rig.ctl.addLayer("not_a_primitive");
        CHECK(rig.ctl.selectedLayer() == 0);
    }

    /*======================================================*\
    || B. EffectLayerModel row-diff contract                ||
    \*======================================================*/
    std::printf("-- model: same-shape diff / contiguous "
                "insert / reset\n");
    {
        EffectLayerModel m;
        studio::EffectLayer a;
        a.primitive = "wave";
        studio::EffectLayer b;
        b.primitive = "ripple";
        b.enabled   = false;
        studio::EffectLayer c;
        c.primitive = "static";

        QSignalSpy ins(&m, &QAbstractListModel::rowsInserted);
        QSignalSpy rem(&m, &QAbstractListModel::rowsRemoved);
        QSignalSpy dat(&m, &QAbstractListModel::dataChanged);
        QSignalSpy rst(&m, &QAbstractListModel::modelReset);

        CHECK(m.SetStack({ a, b }));
        CHECK(ins.count() == 1);
        CHECK(m.count() == 2);
        CHECK(m.rowAt(1).value("primitive").toString() == "ripple");
        CHECK(m.rowAt(1).value("enabled").toBool() == false);

        /* Same-shape content change → row-granular dataChanged,
           no insert/reset. */
        ins.clear(); rst.clear();
        studio::EffectLayer b2 = b;
        b2.enabled = true;
        CHECK(m.SetStack({ a, b2 }));
        CHECK(dat.count() == 1);
        CHECK(ins.count() == 0 && rst.count() == 0);

        /* Identical stack → no signal at all. */
        dat.clear();
        CHECK(!m.SetStack({ a, b2 }));
        CHECK(dat.count() == 0);

        /* Contiguous append → rowsInserted. */
        ins.clear();
        CHECK(m.SetStack({ a, b2, c }));
        CHECK(ins.count() == 1);
        CHECK(m.count() == 3);

        /* Contiguous remove → rowsRemoved. */
        rem.clear();
        CHECK(m.SetStack({ a, b2 }));
        CHECK(rem.count() == 1);

        /* Same-size content rewrite → per-row dataChanged for the
           differing rows, still no reset (a wholesale rewrite at
           the same shape keeps delegates alive). */
        rst.clear(); dat.clear(); ins.clear(); rem.clear();
        studio::EffectLayer d;
        d.primitive = "comet";
        d.opacity   = 0.3f;
        CHECK(m.SetStack({ d, c }));
        CHECK(rst.count() == 0);
        CHECK(dat.count() == 2);

        /* Non-contiguous shape change → full reset. */
        rst.clear(); dat.clear();
        studio::EffectLayer e2;
        e2.primitive = "wave";
        CHECK(m.SetStack({ e2 }));
        CHECK(rst.count() == 1);
        CHECK(m.count() == 1);
    }

    /*======================================================*\
    || Q-1: a gradient-stop drag keeps its own handle        ||
    \*======================================================*/
    /* The real PaletteEditor in a real (offscreen) window,
       driven by real mouse events. Every preview move emits
       stackChanged; the handles must survive the reload so the
       press that opened the layer gesture also gets the
       release that commits it (ONE undo record, no gesture
       left open). A grab taken away mid-drag must cancel the
       gesture and restore the stops. */
    std::printf("-- qml: palette stop drag survives reloads "
                "(audit Q-1)\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc("aurora"));
        /* Layer 1 of aurora: a wave with stops 0 / .35 / .65 /
           .85 — materialize it so the stops are authored. */
        rig.ctl.setLayerOpacity(1, 0.9);
        const size_t undo0 = rig.UndoCount();
        CHECK(rig.ctl.layerAt(1).value("palette").toList().size()
              == 4);

        QQmlEngine eng;
        eng.rootContext()->setContextProperty(
            QStringLiteral("effectEdit"), &rig.ctl);
        QQmlComponent c(&eng, QUrl(QStringLiteral(
            "qrc:/qt/qml/OpenRGB2/UI/studio/PaletteEditor.qml")));
        CHECK(!c.isError());
        QQuickWindow win;
        win.resize(440, 200);
        std::unique_ptr<QObject> o(c.create(eng.rootContext()));
        auto* pal = qobject_cast<QQuickItem*>(o.get());
        CHECK(pal != nullptr);
        QObject* rep =
            pal ? pal->findChild<QObject*>("stopHandles") : nullptr;
        CHECK(rep != nullptr);
        if(pal != nullptr && rep != nullptr)
        {
            pal->setParentItem(win.contentItem());
            pal->setX(20);
            pal->setY(20);
            pal->setWidth(400);
            pal->setProperty("layerIndex", 1);
            win.show();
            CHECK(QTest::qWaitForWindowExposed(&win));

            auto handleAt = [&](int i) -> QQuickItem* {
                QQuickItem* h = nullptr;
                QMetaObject::invokeMethod(rep, "itemAt",
                    Q_RETURN_ARG(QQuickItem*, h), Q_ARG(int, i));
                return h;
            };
            auto centerOf = [&](QQuickItem* h) -> QPoint {
                return h->mapToScene(QPointF(h->width() / 2,
                                             h->height() / 2))
                    .toPoint();
            };
            auto dragArea = [&](QQuickItem* h) -> QQuickItem* {
                for(QQuickItem* ch : h->childItems())
                {
                    if(QString::fromLatin1(
                           ch->metaObject()->className())
                           .contains(QStringLiteral("MouseArea")))
                    {
                        return ch;
                    }
                }
                return nullptr;
            };

            /* The page Flickable must not steal the drag. */
            QQuickItem* h1 = handleAt(1);
            CHECK(h1 != nullptr);
            QQuickItem* ma1 = h1 ? dragArea(h1) : nullptr;
            CHECK(ma1 != nullptr
                  && ma1->property("preventStealing").toBool());

            /* Drag stop 1 (.35) right past stop 2 (.65). */
            if(h1 != nullptr)
            {
                QPointer<QQuickItem> held(h1);
                QPoint p = centerOf(h1);
                QTest::mousePress(&win, Qt::LeftButton, {}, p);
                CHECK(rig.ctl.gestureActive());
                for(int k = 0; k < 8; ++k)
                {
                    p.rx() += 20;
                    QTest::mouseMove(&win, p);
                }
                /* THE REGRESSION: the handle that took the press
                   is still alive after the preview reloads. */
                CHECK(!held.isNull());
                QTest::mouseRelease(&win, Qt::LeftButton, {}, p);
                CHECK(!rig.ctl.gestureActive());
                CHECK(rig.UndoCount() == undo0 + 1);
                const QVariantList st =
                    rig.ctl.layerAt(1).value("palette").toList();
                CHECK(st.size() == 4);
                bool movedPast = false;
                for(const QVariant& v : st)
                {
                    const double pos =
                        v.toMap().value("pos").toDouble();
                    if(pos > 0.7 && pos < 0.8)
                        movedPast = true;
                    CHECK(std::fabs(pos - 0.35) > 1e-3);
                }
                CHECK(movedPast);
                /* The handles still track the stops. */
                CHECK(handleAt(3) != nullptr);
            }

            /* A grab taken away mid-drag (here: the drag area
               disabled under the pointer) cancels the gesture
               and restores the stops — no record, nothing
               left open. */
            QQuickItem* h0 = handleAt(0);
            CHECK(h0 != nullptr);
            if(h0 != nullptr)
            {
                const size_t undo1 = rig.UndoCount();
                const QVariantList before =
                    rig.ctl.layerAt(1).value("palette").toList();
                QPoint p = centerOf(h0);
                QTest::mousePress(&win, Qt::LeftButton, {}, p);
                for(int k = 0; k < 4; ++k)
                {
                    p.rx() += 20;
                    QTest::mouseMove(&win, p);
                }
                CHECK(rig.ctl.gestureActive());
                QQuickItem* ma0 = dragArea(h0);
                CHECK(ma0 != nullptr);
                if(ma0 != nullptr)
                    ma0->setEnabled(false);
                CHECK(!rig.ctl.gestureActive());
                CHECK(rig.UndoCount() == undo1);
                CHECK(rig.ctl.layerAt(1).value("palette").toList()
                      == before);
                QTest::mouseRelease(&win, Qt::LeftButton, {}, p);
                CHECK(rig.UndoCount() == undo1);
            }
            pal->setParentItem(nullptr);
        }
    }

    /*======================================================*\
    || C. QML panels load headless + degrade honestly        ||
    \*======================================================*/
    std::printf("-- qml: studio panels load headless\n");
    {
        QQmlEngine eng;
        const QString base =
            QStringLiteral("qrc:/qt/qml/OpenRGB2/UI/studio/");
        const char* files[] = {
            "EffectEditor.qml",
            "EffectInspector.qml",
            "LayerStack.qml",
            "LookShelf.qml",
            "PaletteEditor.qml",
        };
        for(const char* f : files)
        {
            QQmlComponent c(&eng,
                QUrl(base + QString::fromLatin1(f)));
            if(c.isError())
            {
                std::printf("  %s: %s\n", f,
                            c.errorString().toUtf8().constData());
            }
            CHECK(!c.isError());
            std::unique_ptr<QObject> o(
                c.create(eng.rootContext()));
            if(!o)
            {
                std::printf("  %s: %s\n", f,
                            c.errorString().toUtf8().constData());
            }
            CHECK(o != nullptr);
        }
    }

    /* Audit Q-10: the inspector text fields commit when the edit
       ends (editingFinished = Enter OR focus loss), an unchanged
       or refused edit leaves the model's value showing, and the
       reactive-source badge follows the engine's input state. */
    std::printf("-- qml: inspector fields commit on edit end; "
                "source badge refreshes (audit Q-10)\n");
    {
        auto findIn = [](QObject* root, const char* name)
            -> QObject* {
            const QString n = QString::fromLatin1(name);
            std::vector<QQuickItem*> stack;
            if(auto* r = qobject_cast<QQuickItem*>(root))
                stack.push_back(r);
            while(!stack.empty())
            {
                QQuickItem* it = stack.back();
                stack.pop_back();
                if(it->objectName() == n)
                    return it;
                for(QQuickItem* ch : it->childItems())
                    stack.push_back(ch);
            }
            return nullptr;
        };
        auto endEdit = [](QObject* field, const QString& text) {
            field->setProperty("text", text);
            QMetaObject::invokeMethod(field, "editingFinished");
        };
        Rig rig;
        rig.LoadDoc(CompactDoc("aurora"));
        rig.ctl.setSelectedLayer(1);   /* the wave layer */

        QQmlEngine eng;
        eng.rootContext()->setContextProperty(
            QStringLiteral("effectEdit"), &rig.ctl);
        QQmlComponent c(&eng, QUrl(QStringLiteral(
            "qrc:/qt/qml/OpenRGB2/UI/studio/EffectInspector.qml")));
        CHECK(!c.isError());
        std::unique_ptr<QObject> o(c.create(eng.rootContext()));
        CHECK(o != nullptr);
        QObject* targets = o ? findIn(o.get(), "inspTargets")
                             : nullptr;
        QObject* originX = o ? findIn(o.get(), "inspOrigin_x")
                             : nullptr;
        QObject* pal = o ? findIn(o.get(), "paletteEditor")
                         : nullptr;
        QObject* stopColor = o ? findIn(o.get(), "stopColorField")
                               : nullptr;
        QObject* badge = o ? findIn(o.get(), "inspSourceState")
                           : nullptr;
        CHECK(targets && originX && pal && stopColor && badge);
        if(targets && originX && pal && stopColor && badge)
        {
            /* Targets: focus loss commits; ending again with the
               same text adds no record. */
            const size_t u0 = rig.UndoCount();
            endEdit(targets, QStringLiteral("fan0"));
            CHECK(rig.ctl.layerAt(1).value("targets").toStringList()
                  == QStringList({ "fan0" }));
            CHECK(rig.UndoCount() == u0 + 1);
            endEdit(targets, QStringLiteral("fan0"));
            CHECK(rig.UndoCount() == u0 + 1);

            /* Origin: a number commits; text that is not a number
               is refused and the field shows the model again. */
            endEdit(originX, QStringLiteral("0.25"));
            CHECK(std::fabs(rig.ctl.layerAt(1).value("origin")
                                .toMap().value("x").toDouble()
                            - 0.25) < 1e-6);
            const size_t u1 = rig.UndoCount();
            endEdit(originX, QStringLiteral("abc"));
            CHECK(rig.UndoCount() == u1);
            CHECK(originX->property("text").toString()
                  == QStringLiteral("0.25"));

            /* Palette stop color: commits on edit end; a refused
               color reverts the field. */
            pal->setProperty("selStop", 0);
            endEdit(stopColor, QStringLiteral("#00FF00"));
            CHECK(QColor(rig.ctl.layerAt(1).value("palette").toList()
                             .value(0).toMap().value("color")
                             .toString()) == QColor("#00FF00"));
            const size_t u2 = rig.UndoCount();
            endEdit(stopColor, QStringLiteral("not-a-color"));
            CHECK(rig.UndoCount() == u2);
            CHECK(QColor(stopColor->property("text").toString())
                  == QColor("#00FF00"));

            /* Source badge follows the engine's reactive state. */
            rig.ctl.setLayerSource(1, QStringLiteral("audio"));
            CHECK(badge->property("text").toString()
                  == QStringLiteral("not running"));
            QVariantMap audio;
            audio["id"] = QStringLiteral("audio");
            audio["enabled"] = true;
            audio["available"] = true;
            audio["running"] = true;
            QVariantMap reactive;
            reactive["inputs"] = QVariantList({ audio });
            rig.bridge.setReactiveForTesting(reactive);
            CHECK(badge->property("text").toString()
                  == QStringLiteral("live"));
        }
    }

    std::printf("-- qml: look apply prompts before replacing "
                "authored layers\n");
    {
        Rig rig;
        rig.LoadDoc(CompactDoc("aurora"));
        rig.ctl.setLayerOpacity(0, 0.5);   /* authored stack */
        CHECK(rig.ctl.hasInlineLayers());

        /* A personal look row for the shelf (same feed the wire
           catalog reply takes). */
        studio::EffectDocument d;
        d.id = "shelf_look"; d.name = "Shelf Look";
        nlohmann::ordered_json l;
        l["primitive"] = "static";
        d.layers.push_back(l);
        QVariantMap e;
        e["id"] = "shelf_look"; e["name"] = "Shelf Look";
        e["needs"] = ""; e["from_file"] = true;
        e["document_text"] = QString::fromStdString(
            studio::EffectDocumentToJson(d).dump());
        rig.ctl.FeedEffectsCatalogForTesting({ e });

        QQmlEngine eng;
        eng.rootContext()->setContextProperty(
            QStringLiteral("effectEdit"), &rig.ctl);
        QQmlComponent c(&eng, QUrl(QStringLiteral(
            "qrc:/qt/qml/OpenRGB2/UI/studio/LookShelf.qml")));
        CHECK(!c.isError());
        std::unique_ptr<QObject> o(c.create(eng.rootContext()));
        CHECK(o != nullptr);
        /* Qt6 Repeater delegates aren't QObject children —
           reach them through itemAt(). */
        QObject* rep =
            o ? o->findChild<QObject*>("lookRows") : nullptr;
        QQuickItem* row = nullptr;
        if(rep != nullptr)
        {
            QMetaObject::invokeMethod(rep, "itemAt",
                Q_RETURN_ARG(QQuickItem*, row), Q_ARG(int, 0));
        }
        QObject* btn =
            row ? row->findChild<QObject*>("lookApply_shelf_look")
                : nullptr;
        CHECK(rep != nullptr);
        CHECK(row != nullptr);
        CHECK(btn != nullptr);
        QObject* dlg =
            o ? o->findChild<QObject*>("lookApplyDialog")
              : nullptr;
        CHECK(dlg != nullptr);
        if(btn != nullptr && dlg != nullptr)
        {
            /* Apply over an authored stack → the mandated notice
               engages instead of silently replacing layers. A
               modal Popup can't show without a window — the
               observable proof is the dialog's armed look id +
               the authored stack remaining untouched. */
            const bool inv =
                QMetaObject::invokeMethod(btn, "clicked");
            CHECK(inv);
            CHECK(dlg->property("lookId").toString()
                  == "shelf_look");
            CHECK(rig.ctl.hasInlineLayers());
            CHECK(rig.ctl.presetId() == "aurora");

            /* Cancel preserves the authored layers. */
            QMetaObject::invokeMethod(dlg, "reject");
            CHECK(rig.ctl.hasInlineLayers());
            CHECK(rig.ctl.presetId() == "aurora");

            /* Accept applies the pick through one undoable
               record. */
            QMetaObject::invokeMethod(btn, "clicked");
            QMetaObject::invokeMethod(dlg, "accept");
            CHECK(!rig.ctl.hasInlineLayers());
            CHECK(rig.ctl.presetId() == "shelf_look");
            rig.ctl.undo();
            CHECK(rig.ctl.hasInlineLayers());
            CHECK(rig.ctl.presetId() == "aurora");
        }
    }

    /*======================================================*\
    || Q-3: Wave card direction + color add                  ||
    \*======================================================*/
    /* The stored direction is a unit vector (SetFlow
       normalises), so per-axis x/y/z sliders fought the
       normalisation. Heading + tilt span the same vector and
       stay independent: heading never changes the tilt, tilt
       never changes the heading (kept through vertical). */
    std::printf("-- qml: wave heading/tilt drive the stored "
                "direction (audit Q-3)\n");
    {
        auto findIn = [](QObject* root, const char* name)
            -> QObject* {
            const QString n = QString::fromLatin1(name);
            if(QObject* c = root->findChild<QObject*>(n))
                return c;
            std::vector<QQuickItem*> stack;
            if(auto* r = qobject_cast<QQuickItem*>(root))
                stack.push_back(r);
            while(!stack.empty())
            {
                QQuickItem* it = stack.back();
                stack.pop_back();
                if(it->objectName() == n)
                    return it;
                for(QQuickItem* ch : it->childItems())
                    stack.push_back(ch);
            }
            return nullptr;
        };
        Rig rig;
        rig.LoadDoc(CompactDoc("wave"));
        CHECK(rig.ctl.flowState().value("available").toBool());
        auto dirNear = [&](double x, double y, double z) {
            const QVariantMap d = rig.ctl.flowState()
                .value("direction").toMap();
            const bool ok =
                std::fabs(d.value("x").toDouble() - x) < 1e-3
                && std::fabs(d.value("y").toDouble() - y) < 1e-3
                && std::fabs(d.value("z").toDouble() - z) < 1e-3;
            if(!ok)
                std::printf("  direction is (%.4f, %.4f, %.4f)\n",
                            d.value("x").toDouble(),
                            d.value("y").toDouble(),
                            d.value("z").toDouble());
            return ok;
        };

        QQmlEngine eng;
        eng.rootContext()->setContextProperty(
            QStringLiteral("effectEdit"), &rig.ctl);
        QQmlComponent c(&eng, QUrl(QStringLiteral(
            "qrc:/qt/qml/OpenRGB2/UI/studio/WaveControls.qml")));
        CHECK(!c.isError());
        std::unique_ptr<QObject> o(c.create(eng.rootContext()));
        CHECK(o != nullptr);
        QObject* heading = o ? findIn(o.get(), "waveHeading")
                             : nullptr;
        QObject* tilt = o ? findIn(o.get(), "waveTilt") : nullptr;
        CHECK(heading != nullptr);
        CHECK(tilt != nullptr);
        auto commit = [](QObject* s, double v) {
            QMetaObject::invokeMethod(s, "gestureCommitted",
                                      Q_ARG(double, v));
        };
        auto model = [](QObject* s) {
            return s->property("modelValue").toDouble();
        };
        if(heading != nullptr && tilt != nullptr)
        {
            const double c30 = std::cos(30.0 * 3.14159265358979 / 180);
            CHECK(dirNear(0, 0, -1));        /* the look's default */
            CHECK(std::fabs(model(heading)) < 1e-6);
            CHECK(std::fabs(model(tilt)) < 1e-6);

            /* Heading alone turns the bands around the vertical. */
            commit(heading, 90);
            CHECK(dirNear(1, 0, 0));
            /* Tilt alone keeps the heading. */
            commit(tilt, 30);
            CHECK(dirNear(c30, 0.5, 0));
            CHECK(std::fabs(model(heading) - 90) < 1e-3);
            /* Heading keeps the tilt. */
            commit(heading, -90);
            CHECK(dirNear(-c30, 0.5, 0));
            CHECK(std::fabs(model(tilt) - 30) < 1e-3);
            /* Straight up: heading is undefined but kept, so
               tilting back restores it. */
            commit(tilt, 90);
            CHECK(dirNear(0, 1, 0));
            CHECK(std::fabs(model(heading) + 90) < 1e-3);
            commit(tilt, 0);
            CHECK(dirNear(-1, 0, 0));

            /* An axis-aligned direction can be tilted (the old
               z slider could only flip its sign). */
            CHECK(rig.ctl.setFlowDirection(0, 0, -1));
            commit(tilt, -45);
            CHECK(dirNear(0, -0.7071, -0.7071));
        }

        /* "+" appends a color only when the dialog is accepted —
           a cancelled pick used to leave a duplicate stop. */
        std::printf("-- qml: wave '+' adds a color only on accept "
                    "(audit Q-3)\n");
        auto colorCount = [&] {
            return (int)rig.ctl.flowState().value("colors")
                .toList().size();
        };
        const int n0 = colorCount();
        CHECK(n0 >= 2 && n0 < 5);
        QObject* add = o ? findIn(o.get(), "waveColorAdd") : nullptr;
        QObject* dlg = o ? findIn(o.get(), "waveColorDialog")
                         : nullptr;
        CHECK(add != nullptr);
        if(add != nullptr)
        {
            QMetaObject::invokeMethod(add, "clicked");
            CHECK(colorCount() == n0);
        }
        CHECK(dlg != nullptr);
        if(add != nullptr && dlg != nullptr)
        {
            /* A headless dialog never opens, so its accept()/
               reject() don't emit — emit the outcome signals the
               card handles directly. */
            QMetaObject::invokeMethod(dlg, "rejected");
            CHECK(colorCount() == n0);
            QMetaObject::invokeMethod(add, "clicked");
            dlg->setProperty("selectedColor", QColor("#123456"));
            QMetaObject::invokeMethod(dlg, "accepted");
            const QVariantList cols =
                rig.ctl.flowState().value("colors").toList();
            CHECK((int)cols.size() == n0 + 1);
            CHECK(!cols.isEmpty()
                  && QColor(cols.last().toString())
                         == QColor("#123456"));
        }
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
