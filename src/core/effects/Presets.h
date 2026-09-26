/*---------------------------------------------------------*\
||| Presets.h                                                 |
|||                                                           |
|||   The six nonreactive Stage-2 presets. Each builds a     |
|||   deterministic layer list from (id, seed); global       |
|||   speed/intensity multipliers apply on top. The seed is  |
|||   stored in the scene document, so a Remix is           |
|||   reproducible and persists with the scene.              |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include "EffectTypes.h"
#include "../presets/EffectRegistry.h"

namespace studio
{

/* The process-wide effect-look registry. The Qt bridge fills the
   defaults layer from bundled qrc resources and loads the
   workspace's presets/effects/ over it; the Qt-free side lazily
   probes the source-tree packaged dir so tests resolve the same
   looks. */
EffectRegistry& EffectLooks();

/* Invalidate the lazy packaged-file probe (tests pointing the
   registry at a fixture dir before first use don't need this —
   it only matters after an explicit reset). */
void            ResetEffectLooks();

struct PresetInfo
{
    std::string id;
    std::string name;
    std::string description;
    std::string needs;          /* required input source: "audio" |   */
                                /* "key" | "screen" | "" = none       */
    bool        from_file = false;  /* a presets/effects file supplies  */
                                    /* this look (file-over-default or  */
                                    /* file-only)                       */
};

/* Preset strip listing — the nine shipped looks in fixed card
   order (metadata from the registry: a file override supplies
   the file's name/description/needs), then any file-only looks
   sorted by id. Rebuilt on each call; the pointer a FindPreset
   returns is valid until the next call. */
const std::vector<PresetInfo>& PresetList();
const PresetInfo*              FindPreset(const std::string& id);

/* Resolve a look's layers via the registry (remix draws consume
   the seed's stream in document order). Unknown id -> empty list;
   when NOTHING is readable (no packaged files, no file layer) the
   one minimal built-in fallback look answers any id so the desk
   still lights. */
std::vector<EffectLayer> BuildPreset(const std::string& id, unsigned int seed);

/* Global user controls applied to a built layer list. */
void ApplyGlobalParams(std::vector<EffectLayer>& layers, float speed, float intensity);

/* SplitMix32 — deterministic remix stream. */
struct RemixRng
{
    unsigned int state;
    explicit RemixRng(unsigned int seed) : state(seed) {}
    unsigned int Next();
    float        Next01();                 /* [0,1)  */
    float        Range(float lo, float hi);
    float        Angle();                  /* radians, full circle */
};

/* Rotate a direction in the desk plane (around +Y). */
Vec3 RotateYaw(const Vec3& dir, float radians);

} /* namespace studio */
