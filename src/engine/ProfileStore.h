/*---------------------------------------------------------*\
||| ProfileStore.h                                            ||
|||                                                           ||
|||   Durable engine state (plan sections 4, 5, and 6 —    ||
|||   Task 3.8): the per-scene PlaybackMode selections     ||
|||   and the resolved binding-identity table, persisted   ||
|||   atomically, versioned, migration-safe, and           ||
|||   recoverable from a last-known-good copy.             ||
|||                                                           ||
|||   What the file holds (profiles/local/ — gitignored    ||
|||   user state; unique hardware identifiers never land   ||
|||   in committed files):                                 ||
|||   - scenes:   scene key -> {mode, params} — the        ||
|||     PlaybackController's per-scene store made durable  ||
|||   - bindings: binding id -> resolved DeviceBinding     ||
|||     identity (name/vendor/serial/location + zone) so a │
|||     binding resolves to the SAME device after restart  │
|||     without the device present, and is never           ||
|||     reassigned to a same-named different device        ||
|||                                                           ||
|||   Guarantees:                                           ||
|||   - Atomic writes: serialize -> re-validate -> backup   ||
|||     the last-valid file -> write temp -> flush ->       ||
|||     rename. A crash mid-save leaves either the old or   ||
|||     the new file intact, never a torn document.        ||
|||   - Versioned schema + migration: older documents are  ||
|||     read forward; fields still applicable are never    ||
|||     silently dropped (unknown members are retained     ||
|||     verbatim in `extensions`). Newer documents are     ||
|||     rejected — no silent downgrade. Old profiles      ||
|||     carried no playback mode: an absent or            ||
|||     unrecognised selection migrates to LiveEffects,    ||
|||     NEVER to uploaded playback (plan section 3.4).     ||
|||   - Last-known-good recovery: a corrupt primary falls  ||
|||     back to the last-valid backup and the failure is   ||
|||     reported honestly — never a blank or guessed       ||
|||     state. A corrupt file being overwritten is kept    ||
|||     aside as <path>.rejected rather than destroyed.    ||
|||                                                           ||
|||   Persistence-boundary validation (calibration report   ||
|||   regression, plan section 5): ValidateSceneLed-       ||
|||   Addresses rejects out-of-range LED addresses where   ||
|||   an expanded document is stored/applied — the check   ||
|||   workspace resolution (SceneResolver) already runs    ||
|||   for compact documents, so expanded documents can no  ||
|||   longer skip it through SET_SCENE/--scene.            ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "EngineTypes.h"
#include "PlaybackPresets.h"   /* PersistedSceneMode, PlaybackParams */
#include "ReactiveInputs.h"    /* ReactiveInputs::PersistedInput     */

#include "scene/SceneTypes.h"  /* studio::DeviceBinding           */

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <vector>

namespace openrgb2
{

/*---------------------------------------------------------*\
||| EngineState — the serializable engine profile.        ||
|||                                                         ||
|||   scenes    — ordered LRU list (front = least recent)  ||
|||   bindings  — binding id -> resolved device identity   ||
|||   extensions— unrecognised members retained verbatim   ||
|||               (migration never silently drops fields   ||
|||               that still apply)                        ||
\*---------------------------------------------------------*/
constexpr int kEngineStateSchemaVersion = 1;

struct EngineState
{
    int                                      schema_version =
        kEngineStateSchemaVersion;
    std::vector<PersistedSceneMode>          scenes;
    std::map<std::string, studio::DeviceBinding> bindings;
    /* Task 4.6: the reactive-input selection (enabled flags +
       per-input params) — restoring re-arms the recorded
       inputs; a source that fails to start stays disabled
       and reports honestly rather than lying enabled. */
    std::vector<ReactiveInputs::PersistedInput> reactive;
    nlohmann::json                           extensions;
};

/*---------------------------------------------------------*\
||| Serialization                                           ||
||\*--------------------------------------------------------*/

/* Deterministic serialization (nlohmann orders object keys;
   the store writes dump(2)). Colors serialize "#RRGGBB". */
nlohmann::json EngineStateToJson(const EngineState& state);

/* Whole-document validation + migration into a candidate:
   - schema_version absent/0: legacy document — every field
     read leniently; an absent or unrecognised playback mode
     migrates to LiveEffects (old profiles are preserved as
     Live effects, never silently converted to uploaded
     playback). Explicit recognised selections survive.
   - schema_version 1: strict field validation — a malformed
     document fails so the caller can recover LKG.
   - schema_version > current: rejected (no silent downgrade).
   On success `out` receives the candidate (migrated to the
   current schema) and the function returns true; on failure
   `out` is untouched and `errors` holds field messages.
   `warnings`, when given, collects non-fatal notes.
   `migrated`, when given, reports that the source predated
   the current schema. Never throws. */
bool EngineStateFromJson(const nlohmann::json& j,
                         EngineState& out,
                         std::vector<std::string>* errors   = nullptr,
                         std::vector<std::string>* warnings = nullptr,
                         bool*                    migrated  = nullptr);

/*---------------------------------------------------------*\
||| Persistence-boundary validation                         ||
||\*--------------------------------------------------------*/

/* LED-address bounds for an EXPANDED scene document — the
   same contract SceneResolver enforces for compact
   workspaces: every emitter address is >= -1 (the render-
   only sentinel) and, when the object's binding declares
   zone_leds > 0, < zone_leds. Out-of-range addresses are
   rejected HERE — the last boundary before a document is
   applied/stored (calibration-report regression). Returns
   true when clean; errors are "<path>: ..." messages. */
bool ValidateSceneLedAddresses(const studio::SceneDocument& doc,
                               std::vector<std::string>* errors);

/*---------------------------------------------------------*\
||| Offline bindings                                        ||
||\*--------------------------------------------------------*/

/* Merge the persisted resolved-identity table into a scene
   document's bindings (in place). For each binding, the
   recorded identity for the SAME binding id fills only the
   fields the document left empty (serial/location/vendor/
   device_type/zone_name/zone_leds), and only when the
   recorded controller_name still matches the binding's —
   a re-purposed binding id is never rewritten. A name-only
   binding thus inherits its recorded serial, so resolution
   still targets the same physical device after restart and
   a same-named different device can never claim it.
   Returns true when any binding gained fields. */
bool AugmentSceneBindings(studio::SceneDocument& doc,
    const std::map<std::string, studio::DeviceBinding>& resolved);

/* The identity to persist for a binding that just resolved:
   controller identity from the snapshot's DeviceKey +
   name/vendor/device_type, zone from the binding (or the
   resolved zone name) + the resolved zone's LED count. */
studio::DeviceBinding ResolvedBindingIdentity(
    const studio::DeviceBinding& binding,
    const DeviceSnapshot&        snap,
    const std::string&           resolved_zone_name,
    uint32_t                     resolved_zone_leds);

/*---------------------------------------------------------*\
||| ProfileStore — the file layer                           ||
||\*--------------------------------------------------------*/

class ProfileStore
{
public:
    explicit ProfileStore(std::string path);

    const std::string& Path()     const { return path_; }
    std::string TempPath()     const { return path_ + ".tmp"; }
    std::string LkgPath()      const { return path_ + ".lkg"; }
    std::string RejectedPath() const { return path_ + ".rejected"; }

    enum class LoadStatus
    {
        Empty,      /* no profile on disk — a fresh start, not an error */
        Loaded,     /* current-schema document loaded                   */
        Migrated,   /* older document loaded + migrated forward         */
        Recovered,  /* primary corrupt — the last-known-good restored;
                       `error` reports the primary failure honestly     */
        Failed,     /* primary AND backup unreadable — `error` reports
                       both failures; `state` is empty and clearly
                       flagged, never guessed                          */
    };
    struct LoadResult
    {
        LoadStatus               status = LoadStatus::Empty;
        EngineState              state;
        std::string              error;
        std::vector<std::string> warnings;
    };

    /* Parse + validate (+migrate) the profile. A missing file is
       LoadStatus::Empty — callers distinguish "no profile yet"
       from a corrupt one. On corruption the last-known-good copy
       (<path>.lkg) is tried and the outcome reported. */
    LoadResult Load() const;

    /* Atomic save:
       1. serialize + re-validate the candidate (the store never
          writes a document its own validation rejects);
       2. if a current file exists: a VALID one is copied to
          <path>.lkg (last-known-good), an invalid one is set
          aside as <path>.rejected (preserved, never silently
          destroyed);
       3. write <path>.tmp, flush to disk, rename over <path>.
       A crash anywhere leaves either the old or the new file —
       never a torn document. Directories are created as needed.
       On failure returns false with `err` set; the previous
       file is left untouched. */
    bool Save(const EngineState& state, std::string& err) const;

private:
    std::string path_;
};

} /* namespace openrgb2 */
