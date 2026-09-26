/*---------------------------------------------------------*\
||| PresetBundle.h                                            |
|||                                                           |
|||   Portable workspace bundle: a directory containing    ||
|||     studio.json                — the sanitized v3 doc  ||
|||     presets/devices/*.device.json — every referenced   ||
|||                                     type (transitive)  ||
|||     assets/<rel>               — referenced assets     ||
|||                                                           |
|||   Export strips local hardware identity (serial /      ||
|||   location), forces verified + live flags off, and     ||
|||   copies only what the workspace actually uses.        ||
|||   Import inspects first (no writes), classifies each   ||
|||   bundled type new|identical|conflict against the      ||
|||   local library, then applies with explicit same-id    ||
|||   conflict choices — a different-content local type    ||
|||   is never overwritten. The applied workspace is a     ||
|||   CANDIDATE: the caller runs it through the normal     ||
|||   load/resolve path, so a rejected import leaves the   ||
|||   active scene untouched.                              ||
|||                                                           |
|||   Qt-free (nlohmann json + std::filesystem) so the     ||
|||   fast test suite covers it.                           ||
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include "DevicePreset.h"
#include "PresetRegistry.h"
#include "../config/StudioConfig.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace studio
{

/* Bundle content caps — community content must not be able to
   destabilize the host. Per-JSON and per-asset limits plus a
   whole-bundle ceiling; the PRESET_MAX_* type caps are enforced
   by DevicePresetFromJson during inspect. */
constexpr unsigned long long BUNDLE_MAX_JSON_BYTES  = 4ull  * 1024 * 1024;
constexpr unsigned long long BUNDLE_MAX_ASSET_BYTES = 16ull * 1024 * 1024;
constexpr unsigned long long BUNDLE_MAX_TOTAL_BYTES = 64ull * 1024 * 1024;

/* Every devices.*.type plus the transitive PresetDependencies
   walk (child-entity `type` refs inside types). `ids` receives
   the sorted set. A type the registry cannot resolve is an
   ERROR (missing refs must not silently shrink the bundle);
   `ids` is still filled with everything that did resolve. */
bool CollectUsedTypes(const StudioDocument& ws,
                      const PresetRegistry& registry,
                      std::vector<std::string>& ids,
                      std::vector<std::string>* errors = nullptr);

/* Portable-copy hygiene. Stripped, per the compat rules:
   - bindings.*.serial / .location — local hardware identity;
     name/vendor/zone_name/zone_leds stay as resolution hints.
   - device_settings.*.zones.*.verified — shared content never
     grants write authority; local resolution re-verifies.
   - output.live_on_startup / effects.playing — a shared doc
     must not arm hardware or start an effect by itself.
   Binding ids, zone structure and placements are kept — the
   skeleton is what local resolution binds against. */
StudioDocument SanitizeForExport(const StudioDocument& w);

/* Write <dest>/studio.json + bundled types + resolvable assets.
   `source_preset_dir` is the workspace's presets/devices dir —
   asset refs resolve under <workspace>/assets/ (same convention
   as PresetRegistry::CheckAssetRefs). Each used type is written
   from the registry's RESOLVED definition (file layer first,
   else the packaged default), re-serialized deterministically.
   Missing or oversized assets are warnings, never failure.
   Refuses a non-empty destination unless `overwrite` is set —
   a dir already holding studio.json gets the sharper
   "already contains a bundle" error. studio.json is written LAST so an
   interrupted export leaves an obviously incomplete bundle
   (InspectBundle requires it). Returns type/asset counts. */
bool ExportBundle(const std::string& dest_dir,
                  const StudioDocument& workspace,
                  const PresetRegistry& registry,
                  const std::string& source_preset_dir,
                  std::vector<std::string>* errors   = nullptr,
                  std::vector<std::string>* warnings = nullptr,
                  bool overwrite = false,
                  unsigned int* types_written  = nullptr,
                  unsigned int* assets_copied  = nullptr);

struct BundleType
{
    enum class Status
    {
        New,        /* id not resolvable locally               */
        Identical,  /* content-equal to the local definition   */
        Conflict    /* same id, different content              */
    };
    std::string  id;
    DevicePreset preset;         /* bundled definition          */
    DevicePreset local;          /* local definition (has_local) */
    bool         has_local = false;
    Status       status    = Status::New;
};

struct ImportPlan
{
    /* Sanitized workspace candidate — NOT activated. The caller
       resolves it against the post-import registry. */
    StudioDocument           workspace;
    std::vector<BundleType>  types;         /* sorted by id      */
    std::vector<std::string> assets;        /* bundled rel paths */
    std::vector<std::string> warnings;
    /* Resolvable local type ids at inspect time — ApplyImport
       uses it so its signature needs no live registry. */
    std::set<std::string>    local_ids;
    std::string              source_dir;
};

/* Parse a bundle WITHOUT writing anything:
   - studio.json: size cap, schema_version <= 3 (older expanded
     docs migrate through the same SceneJson + ConfigMigration
     path ConfigStore uses on file load), full v3 validation.
   - every bundled presets/devices/*.device.json: size cap,
     parse + validate, id == basename, dependency closure
     (bundle ∪ local registry).
   - bundled assets: local relative paths only
     (IsPresetAssetPath), per-file and total size caps.
   - bounded resolution: the candidate is run through
     ResolveScene against a scratch registry (local types with
     the bundle overlaid in dependency order) — resolve failures,
     reference cycles and object/emitter cap breaches reject the
     bundle before apply ever runs.
   - classifies each type new|identical|conflict and records
     warnings (e.g. referenced assets missing from the bundle).
   On failure `plan` is untouched and errors are field-specific. */
bool InspectBundle(const std::string& src_dir,
                   const PresetRegistry& local_registry,
                   ImportPlan& plan,
                   std::vector<std::string>* errors = nullptr);

/* Apply an inspected plan. `choices` maps each Conflict id to
   the new id it imports under (the only allowed resolution for
   different-content conflicts); identical types auto-reuse and
   new types auto-write — no choice needed. Every reference is
   remapped: workspace devices.*.type, child-entity `type` refs
   inside imported types, AND refs inside other imported types
   that pointed at a renamed id. If remapping turns an
   "identical" type into different content (its dependency was
   renamed), it is itself imported under a derived free id —
   the cascade is reported in warnings.
   Writes go through the same validate-then-write discipline as
   ConfigStore::InstallTypes (serialize, re-validate, temp+rename,
   re-read the landed file). An existing local type file is NEVER
   overwritten: identical target content is a reuse hit, anything
   else is an error. Bundled assets copy to <workspace>/assets/,
   never overwriting existing files. On failure `out` is
   untouched; type files already landed stay (they are valid new
   library entries) and the error says what happened. */
bool ApplyImport(const ImportPlan& plan,
                 const std::map<std::string, std::string>& choices,
                 const std::string& target_preset_dir,
                 StudioDocument& out,
                 std::vector<std::string>* errors   = nullptr,
                 std::vector<std::string>* warnings = nullptr);

} /* namespace studio */
