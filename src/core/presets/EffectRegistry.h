/*---------------------------------------------------------*\
|||| EffectRegistry.h                                          |
||||                                                           |
||||   Effect-look library: loads presets/effects/*.effect. ||
||||   json files, enforces id == filename, and layers      ||
||||   packaged defaults underneath file-loaded looks so a  ||
||||   missing file falls back to the shipped definition    ||
||||   and a same-id file overrides it — the same contract  ||
||||   PresetRegistry gives device types.                   ||
||||                                                           |
||||   Qt-free (nlohmann json + std::filesystem) so the     ||
||||   fast test suite covers it.                           |
||||                                                           |
||||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include "../effects/EffectJson.h"

#include <map>
#include <string>
#include <vector>

namespace studio
{

class EffectRegistry
{
public:
    /* Packaged defaults — always available underneath the file
       layer. A file look with the same id overrides the default;
       a missing file falls back to it. */
    void SetDefaults(std::vector<EffectDocument> defaults);

    /* Load a directory of packaged defaults (plugin source tree
       probe for the Qt-free side — the Qt bridge feeds qrc
       contents through SetDefaults instead). */
    bool LoadDefaultsDirectory(const std::string& dir,
                               std::vector<std::string>* errors = nullptr);

    /* Drop file-loaded looks (defaults stay). */
    void ClearFiles();

    /* Scan a directory for *.effect.json. Each file is validated;
       a bad file records an error and is excluded — the rest still
       load. Returns true when every file loaded. */
    bool LoadDirectory(const std::string& dir,
                       std::vector<std::string>* errors = nullptr);

    /* Load one look file: validate and require id == basename.
       On failure the registry is left unchanged. */
    bool LoadFile(const std::string& path,
                  std::vector<std::string>* errors = nullptr);

    /* Register an already-validated document programmatically
       (tests, migration output). */
    bool Add(const EffectDocument& d,
             std::vector<std::string>* errors = nullptr);

    /* File-layer look first, then the packaged default. */
    const EffectDocument* Find(const std::string& id) const;
    bool Contains(const std::string& id) const { return Find(id) != nullptr; }

    std::vector<std::string> Ids() const;      /* all resolvable ids */
    size_t FileCount() const { return effects.size(); }

    /* Draw the look's remix specs in document order against the
       seed's RemixRng stream. false + errors when the document
       fails to resolve (a validated document never should). */
    bool Build(const std::string& id, unsigned int seed,
               std::vector<EffectLayer>& out,
               std::vector<std::string>* errors = nullptr) const;

    /* The merged file-over-default listing, ordered by
       `known_order` first then alphabetical — the strip's fixed
       card order survives file overrides while personal looks
       append sorted. */
    struct EffectInfo
    {
        std::string id;
        std::string name;
        std::string description;
        std::string needs;
        bool        from_file = false;
    };
    std::vector<EffectInfo> List(
        const std::vector<std::string>& known_order = {}) const;

    /* path -> last load error (diagnostics). */
    const std::map<std::string, std::string>& FileErrors() const
    {
        return file_errors;
    }

private:
    std::map<std::string, EffectDocument> effects;   /* file/user   */
    std::map<std::string, EffectDocument> defaults;  /* packaged    */
    std::map<std::string, std::string>    file_errors;
};

} /* namespace studio */
