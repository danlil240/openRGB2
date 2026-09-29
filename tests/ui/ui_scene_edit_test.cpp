// SPDX-License-Identifier: GPL-2.0-or-later
//
// SceneEditModel + ProfileLibrary tests (Task 4.2) — the
// Qt-free edit model that builds the SET_SCENE body, the
// uploaded-preset capability table from the wire contract,
// and the UI's named-scene library file:
//
//   - edits produce a document matching the SET_SCENE body
//     schema ({document:<SceneDocument>} — docs §5)
//   - preset pick replaces the inline layer stack honestly
//   - brightness/speed/intensity clamp to their domains
//   - object_colors emit "#RRGGBB" strings only
//   - layer `targets` only ever exist on inline layer stacks
//   - playback-param support matches the contract table —
//     controls emit ONLY supported fields
//   - applied-scene changes under pending edits flag a
//     conflict, never merge silently
//   - the profile library round-trips documents, bounds the
//     store, and sets corrupt files aside honestly
//
// CHECK-macro main like the other UI suites — no framework.

#include "SceneEditModel.h"
#include "ProfileLibrary.h"
#include "JsonVariant.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QVariantMap>

#include <cstdio>
#include <string>
#include <vector>

using namespace openrgb2::ui;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

/* A minimal valid expanded SceneDocument (the shape GET_SCENE
   returns under `document`). */
static nlohmann::json FixtureDoc()
{
    nlohmann::json pos = nlohmann::json::object(
        {{"x",0},{"y",0},{"z",0}});
    return nlohmann::json::object({
        { "version",    2 },
        { "name",       "test-scene" },
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
        }) },
        { "objects", nlohmann::json::array({
            nlohmann::json::object({
                { "id",        "fan0" },
                { "label",     "SLW Fan 0" },
                { "kind",      "device" },
                { "parent_id", "" },
                { "position",  pos },
                { "rotation",  pos },
                { "scale",     nlohmann::json::object(
                                   {{"x",1},{"y",1},{"z",1}}) },
                { "size_m",    pos },
                { "binding",   "slw0" },
                { "mirror_of", "" },
                { "geometry",  "fan-slw" },
                { "layout",    "" },
                { "verified",  true },
                { "visible",   true },
                { "emitters",  nlohmann::json::array({
                    nlohmann::json::object({
                        { "pos", pos }, { "group", "fan0" },
                        { "address", 0 } }) }) },
            }),
            nlohmann::json::object({
                { "id",    "desk" },
                { "kind",  "decor" },
                { "emitters", nlohmann::json::array() },
            }),
        }) },
        { "object_colors", nlohmann::json::object(
            { { "fan0", "#00B4A0" } }) },
        { "emitter_colors", nlohmann::json::object() },
        { "effect", nlohmann::json::object({
            { "preset",    "aurora" },
            { "seed",      0   },
            { "speed",     1.0 },
            { "intensity", 1.0 },
            { "playing",   true },
            { "layers",    nlohmann::json::array() },
        }) },
    });
}

static nlohmann::json InlineLayerDoc()
{
    nlohmann::json doc = FixtureDoc();
    doc["effect"]["layers"] = nlohmann::json::array({
        nlohmann::json::object({
            { "primitive", "wave" },
            { "targets",   nlohmann::json::array() },
        }),
        nlohmann::json::object({
            { "primitive", "pulse" },
            { "targets",   nlohmann::json::array({ "desk" }) },
        }),
    });
    return doc;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    /*---- baseline / dirty / conflict ----------------------*/
    std::printf("-- baseline, dirty, conflict\n");
    {
        SceneEditModel m;
        CHECK(!m.HasBaseline());
        CHECK(!m.HasWorking());
        CHECK(!m.Dirty());

        m.LoadApplied(FixtureDoc());
        CHECK(m.HasBaseline());
        CHECK(!m.Dirty());
        CHECK(m.PresetId() == "aurora");

        std::string err;
        CHECK(m.SetPreset("rainbow_wave", err));
        CHECK(m.Dirty());
        CHECK(m.PresetId() == "rainbow_wave");
        CHECK(m.Working()["effect"]["playing"].get<bool>());
        CHECK(!m.PresetReplacedLayers());   /* layers were empty */

        /* Remote apply while dirty -> conflict, edits kept. */
        nlohmann::json remote = FixtureDoc();
        remote["name"] = "other-scene";
        m.LoadApplied(remote);
        CHECK(m.Conflict());
        CHECK(m.Dirty());
        CHECK(m.PresetId() == "rainbow_wave");   /* edit kept */

        m.DiscardEdits();
        CHECK(!m.Dirty());
        CHECK(!m.Conflict());
        CHECK(m.PresetId() == "aurora");

        /* Commit promotes working to baseline. */
        CHECK(m.SetPreset("comet", err));
        m.Commit();
        CHECK(!m.Dirty());
        CHECK(m.Baseline()["effect"]["preset"] == "comet");
    }

    /*---- own-save echo: normalized doc adopted, no conflict -*/
    std::printf("-- save echo adoption\n");
    {
        SceneEditModel m;
        m.LoadApplied(FixtureDoc());
        std::string err;
        CHECK(m.SetPreset("breathing", err));
        CHECK(m.SetBrightness(0.33));
        m.Commit();

        /* The engine reserializes what it applied
           (ToJson(FromJson(doc))): floats narrow (0.33 ->
           0.33000001311302185) and defaulted fields appear —
           the echo is a DIFFERENT json document yet the same
           apply. The caller correlates it by scene revision;
           the model adopts it without flagging a remote
           change. */
        nlohmann::json echo = m.Working();
        echo["brightness"] = (double)(float)0.33;
        echo["objects"][0]["parent_id"] = "";
        echo["objects"][0]["mirror_of"] = "";
        echo["objects"][0]["layout"]    = "";
        echo["objects"][0]["label"]     = "fan0";   /* <- id */
        echo["objects"][0]["size_m"] =
            nlohmann::json::object({{"x",0},{"y",0},{"z",0}});
        echo["bindings"][0]["vendor"]      = "Lian Li";
        echo["bindings"][0]["device_type"] = -1;
        CHECK(echo != m.Working());   /* not the submitted doc */

        m.AdoptApplied(echo);
        CHECK(!m.Dirty());
        CHECK(!m.Conflict());
        CHECK(m.Baseline() == echo);
        CHECK(m.Working() == echo);

        /* Edits made after the save stay pending through the
           echo — dirty recomputes, never a conflict. */
        SceneEditModel m2;
        m2.LoadApplied(FixtureDoc());
        CHECK(m2.SetPreset("comet", err));
        m2.Commit();
        CHECK(m2.SetBrightness(0.9));
        nlohmann::json echo2 = m2.Baseline();
        echo2["name"] = "normalized-name";
        m2.AdoptApplied(echo2);
        CHECK(m2.Dirty());
        CHECK(!m2.Conflict());
        CHECK(m2.Baseline()["name"] == "normalized-name");
        CHECK(m2.Working()["brightness"].get<double>() == 0.9);
    }

    /*---- preset pick replaces an inline layer stack -------*/
    std::printf("-- preset replaces inline layers\n");
    {
        SceneEditModel m;
        m.LoadApplied(InlineLayerDoc());
        CHECK(m.GroupsTargetable());
        std::string err;
        CHECK(m.SetPreset("portal", err));
        CHECK(m.PresetReplacedLayers());
        CHECK(m.Working()["effect"]["layers"].empty());
        CHECK(!m.GroupsTargetable());
    }

    /*---- scalar edits + clamps -----------------------------*/
    std::printf("-- scalar edits\n");
    {
        SceneEditModel m;
        m.LoadApplied(FixtureDoc());
        CHECK(m.SetBrightness(1.4));
        CHECK(m.Working()["brightness"].get<double>() == 1.0);
        CHECK(m.SetBrightness(-0.5));
        CHECK(m.Working()["brightness"].get<double>() == 0.0);
        CHECK(m.SetEffectSpeed(100.0));
        CHECK(m.Working()["effect"]["speed"].get<double>() == 8.0);
        CHECK(m.SetEffectIntensity(2.0));
        CHECK(m.Working()["effect"]["intensity"].get<double>() == 1.0);
        std::string err;
        CHECK(m.SetName("renamed", err));
        CHECK(m.Working()["name"] == "renamed");
        CHECK(!m.SetName(std::string(200, 'x'), err));
    }

    /*---- palette writes emit wire colors --------------------*/
    std::printf("-- object colors\n");
    {
        SceneEditModel m;
        m.LoadApplied(FixtureDoc());
        std::string err;
        CHECK(m.SetObjectColor("fan0", "#00ff10", err));
        CHECK(m.Working()["object_colors"]["fan0"] == "#00FF10");
        CHECK(!m.SetObjectColor("fan0", "notacolor", err));
        CHECK(!m.SetObjectColor("fan0", "#12345", err));
        CHECK(SceneEditModel::IsHexColor("#ABCDEF"));
        CHECK(!SceneEditModel::IsHexColor("#ABCDEG"));
    }

    /*---- targets only on inline layer stacks ---------------*/
    std::printf("-- group targets\n");
    {
        SceneEditModel m;
        m.LoadApplied(InlineLayerDoc());
        m.SetSelectedGroups({ "fan0" });
        for(const nlohmann::json& l :
            m.Working()["effect"]["layers"])
        {
            CHECK(l["targets"].size() == 1);
            CHECK(l["targets"][0] == "fan0");
        }
        /* Named-preset doc: selection can't reach layers. */
        SceneEditModel m2;
        m2.LoadApplied(FixtureDoc());
        m2.SetSelectedGroups({ "fan0" });
        CHECK(!m2.GroupsTargetable());
    }

    /*---- group derivation -----------------------------------*/
    std::printf("-- device groups\n");
    {
        SceneEditModel m;
        m.LoadApplied(FixtureDoc());
        const std::vector<DeviceGroup> groups = m.DeviceGroups();
        CHECK(groups.size() == 1);            /* decor excluded */
        CHECK(groups[0].id == "fan0");
        CHECK(groups[0].label == "SLW Fan 0");
        CHECK(groups[0].controller_name == "UNI FAN");
        CHECK(groups[0].zone_name == "Fan 1");
        CHECK(groups[0].zone_leds == 40);
        CHECK(groups[0].verified);
        CHECK(groups[0].emitter_count == 1);
    }

    /*---- SET_SCENE body shape -------------------------------*/
    std::printf("-- SET_SCENE body\n");
    {
        SceneEditModel m;
        m.LoadApplied(FixtureDoc());
        std::string err;
        m.SetPreset("breathing", err);
        m.SetBrightness(0.5);
        const nlohmann::json body = m.BuildSetSceneBody();
        CHECK(body.contains("document"));
        CHECK(body["document"].is_object());
        const nlohmann::json& d = body["document"];
        /* The contract fields the engine's loader needs —
           version as INTEGER (the loader rejects floats). */
        CHECK(d["version"].is_number_integer());
        CHECK(d["version"].get<int>() == 2);
        CHECK(d["name"] == "test-scene");
        CHECK(d["effect"]["preset"] == "breathing");
        CHECK(d["brightness"].get<double>() == 0.5);
        CHECK(d["object_colors"]["fan0"] == "#00B4A0");
        CHECK(d["bindings"].is_array());
        CHECK(d["objects"].is_array());
    }

    /*---- playback preset contract table ---------------------*/
    std::printf("-- playback preset support\n");
    {
        CHECK(PlaybackPresetCatalog().size() == 9);
        PlaybackPresetInfo p;
        CHECK(PlaybackPresetFromName("spectrum_cycle", p));
        CHECK(p.speed && !p.direction && !p.color);
        CHECK(PlaybackPresetFromName("Rainbow Wave", p));
        CHECK(p.id == "rainbow_wave");
        CHECK(p.speed && p.direction && !p.color);
        CHECK(PlaybackPresetFromName("BREATHING", p));
        CHECK(p.speed && !p.direction && p.color);
        CHECK(PlaybackPresetFromName("perimeter_wave", p));
        CHECK(p.speed && p.direction && !p.color);
        CHECK(PlaybackPresetFromName("Perimeter Comet", p));
        CHECK(p.speed && p.direction && p.color);
        CHECK(PlaybackPresetFromName("stack_rainbow", p));
        CHECK(p.id == "stack_rainbow");
        CHECK(!PlaybackPresetFromName("aurora", p));
        CHECK(!PlaybackPresetFromName("", p));
        CHECK(NormalizePresetId("Rainbow-Wave") == "rainbowwave");
        CHECK(ClampPlaybackSpeed(5) == 10);
        CHECK(ClampPlaybackSpeed(300) == 200);
        CHECK(ClampPlaybackSpeed(100) == 100);
    }

    /*---- profile library -------------------------------------*/
    std::printf("-- profile library\n");
    {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        const QString path =
            dir.filePath(QStringLiteral("lib.json"));

        {
            ProfileLibrary lib;
            lib.setPathForTesting(path);
            CHECK(lib.entries().isEmpty());
            const QVariantMap doc =
                JsonToVariant(FixtureDoc()).toMap();
            CHECK(lib.saveProfile(QStringLiteral("Evening"), doc));
            CHECK(lib.saveProfile(QStringLiteral("Desk"), doc));
            CHECK(lib.entries().size() == 2);
            /* Round-trip: the stored document comes back. */
            const QVariantMap back =
                lib.profileDocument(QStringLiteral("Evening"));
            CHECK(back.value(QStringLiteral("name")).toString()
                  == QStringLiteral("test-scene"));
            CHECK(back.value(QStringLiteral("effect")).toMap()
                      .value(QStringLiteral("preset")).toString()
                  == QStringLiteral("aurora"));
            /* Same-name save replaces, never duplicates. */
            CHECK(lib.saveProfile(QStringLiteral("Evening"), doc));
            CHECK(lib.entries().size() == 2);
            CHECK(lib.removeProfile(QStringLiteral("Desk")));
            CHECK(lib.entries().size() == 1);
            CHECK(lib.lastError().isEmpty());
        }

        /* Persistence across instances. */
        {
            ProfileLibrary lib;
            lib.setPathForTesting(path);
            CHECK(lib.entries().size() == 1);
            CHECK(lib.entries().first().toMap()
                      .value(QStringLiteral("name")).toString()
                  == QStringLiteral("Evening"));
        }

        /* Bound: name cap refused honestly. */
        {
            ProfileLibrary lib;
            lib.setPathForTesting(path);
            CHECK(!lib.saveProfile(QString(200, 'x'),
                                   QVariantMap()));
            CHECK(!lib.lastError().isEmpty());
            CHECK(!lib.saveProfile(QStringLiteral(""),
                                   QVariantMap()));
        }

        /* Corrupt file -> set aside + honest error, no crash. */
        {
            const QString bad =
                dir.filePath(QStringLiteral("bad.json"));
            QFile f(bad);
            CHECK(f.open(QIODevice::WriteOnly));
            f.write("{ not json ");
            f.close();
            ProfileLibrary lib;
            lib.setPathForTesting(bad);
            CHECK(lib.entries().isEmpty());
            CHECK(!lib.lastError().isEmpty());
            CHECK(QFile::exists(bad + QStringLiteral(".rejected")));
        }
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
