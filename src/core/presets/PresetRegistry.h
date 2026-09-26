/*---------------------------------------------------------*\
||| PresetRegistry.h                                          |
|||                                                           |
|||   Device-type library: loads presets/devices/*.device. ||
|||   json files, enforces id == filename, validates the   ||
|||   type-reference dependency graph (missing deps and    ||
|||   cycles are rejected), and layers packaged defaults   ||
|||   underneath file-loaded types so a missing file       ||
|||   falls back to the shipped definition.                |
|||                                                           |
|||   Qt-free (nlohmann json + std::filesystem) so the     |
|||   fast test suite covers it.                           |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include "DevicePreset.h"

#include <map>
#include <string>
#include <vector>

namespace studio
{

class PresetRegistry
{
public:
    /* Packaged defaults — always available underneath the file
       layer. A file type with the same id overrides the default;
       a missing file falls back to it (recoverable default desk). */
    void SetDefaults(std::vector<DevicePreset> defaults);

    /* Drop file-loaded types (defaults stay). */
    void ClearFiles();

    /* Scan a directory for *.device.json. Each file is validated;
       a bad file records an error and is excluded — the rest still
       load. After parsing, the dependency graph is validated:
       types with missing deps or inside a reference cycle are
       removed and reported. Returns true when every file loaded. */
    bool LoadDirectory(const std::string& dir,
                       std::vector<std::string>* errors = nullptr);

    /* Load one preset file: validate, require id == basename, and
       re-run dependency validation across the registry. On failure
       the registry is left unchanged. */
    bool LoadFile(const std::string& path,
                  std::vector<std::string>* errors = nullptr);

    /* Register an already-validated preset programmatically
       (migration output). Re-validates dependencies. */
    bool Add(const DevicePreset& p,
             std::vector<std::string>* errors = nullptr);

    /* File-layer type first, then the packaged default. */
    const DevicePreset* Find(const std::string& id) const;
    bool                Contains(const std::string& id) const { return Find(id) != nullptr; }

    std::vector<std::string> Ids() const;      /* all resolvable ids */
    size_t FileCount() const { return types.size(); }

    /* Library listing for the device-library UI (task 4.2): the
       merged file-over-default view, sorted by id.
       - from_file: the file layer supplied this definition
         (a user edit shadowing a packaged default reports true).
       - zone_count / led_total: zone count and summed static LED
         count; led_total is 0 when any zone is dynamic (a matrix
         resolved from the bound hardware zone at runtime).
       - bounds_m: union of the type's local entity footprints
         (position +- rotated size_m/2); child type refs don't
         contribute — their bounds belong to their own listing.
       - floor_y: lowest y of that local union — placement code
         drops instances so this rests on the desk surface. */
    struct PresetInfo
    {
        std::string  id;
        std::string  name;
        std::string  category;
        bool         from_file  = false;
        unsigned int zone_count = 0;
        unsigned int led_total  = 0;
        Vec3         bounds_m;
        float        floor_y    = 0.0f;
    };
    std::vector<PresetInfo> List() const;

    /* path -> last load error (diagnostics). */
    const std::map<std::string, std::string>& FileErrors() const
    {
        return file_errors;
    }

    /* Dependency validation over the file layer + defaults:
       every entity `type` ref must resolve and the graph must be
       acyclic. Unresolvable file types are removed and reported.
       Returns true when the remaining registry is clean. */
    bool ValidateDependencies(std::vector<std::string>* errors);

private:
    std::map<std::string, DevicePreset> types;      /* file/user   */
    std::map<std::string, DevicePreset> defaults;   /* packaged    */
    std::map<std::string, std::string>  file_errors;
};

} /* namespace studio */
