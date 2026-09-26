/*---------------------------------------------------------*\
|||| EffectJson.h                                              |
||||                                                           |
||||   JSON form of the effect layer model — Qt-free        |
||||   (nlohmann + stdlib). Two surfaces share one           |
||||   validated field grammar:                              |
||||                                                         |
||||   - resolved layers (workspace effects.layers,         |
||||     fixtures): literals only — a remix spec is an      |
||||     error.                                              |
||||   - look documents (presets/effects/<id>.effect.json): |
||||     numeric fields may carry bounded deterministic     |
||||     remix specs; draws consume the seed's RemixRng     |
||||     stream in document order (layer array order, then  |
||||     field order inside each layer object — which is    |
||||     why documents parse as ordered_json).              |
||||                                                         |
||||   Remix specs:                                          |
||||     {"remix":[lo,hi]}            -> Range(lo,hi)       |
||||     {"remix01":true}             -> Next01()           |
||||     {"remix_angle":true}         -> Angle()            |
||||     {"remix_neg":[lo,hi]}        -> -Range(lo,hi)      |
||||     {"remix_pick":[a,b]}         -> Next01()<0.5 ? a:b |
||||     {"remix_u32":[lo,hi]}        -> int draw (seed;    |
||||        [0,4294967295] maps to Next() verbatim)         |
||||     {"remix_yaw":[x,y,z,lo,hi]}  -> RotateYaw(         |
||||        Normalize({x,y,z}), Range(lo,hi)) — direction   |
||||        fields only.                                    |
||||   Every spec consumes exactly one stream draw.          |
||||                                                         |
||||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include "EffectTypes.h"   /* EffectLayer / Vec3 / Palette */

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace studio
{

constexpr int EFFECT_SCHEMA_VERSION = 1;

/* Content caps — preset files are user/community content. */
constexpr unsigned int EFFECT_MAX_LAYERS  = 64;
constexpr unsigned int EFFECT_MAX_STOPS   = 64;
constexpr unsigned int EFFECT_MAX_PATH    = 256;
constexpr unsigned int EFFECT_MAX_TARGETS = 64;

/*---------------------------------------------------------*\
|||| EffectDocument — one presets/effects/<id>.effect.json. |
|||| `layers` keeps the raw (ordered) layer objects —      |
|||| remix specs stay unresolved until a seed is bound.    |
\*---------------------------------------------------------*/
struct EffectDocument
{
    int                  schema_version = EFFECT_SCHEMA_VERSION;
    std::string          id;            /* == file basename           */
    std::string          name;
    std::string          description;
    /* Required input source: "audio" | "key" | "screen" | "" none. */
    std::string          needs;
    nlohmann::ordered_json layers = nlohmann::ordered_json::array();
};

/*---------------------------------------------------------*\
|||| Resolved layers <-> JSON (literals only).            ||
|||| Field-path errors ("layers[2].speed: ..."), never     ||
|||| throws. Unknown keys, unknown primitives, non-finite/ ||
|||| out-of-range values, unsorted/duplicate palette stop  ||
|||| positions, a present-but-empty palette, empty target  ||
|||| strings and remix specs are all validation errors.    |
\*---------------------------------------------------------*/
/* The primitive whitelist — the same names the layer parser and
   the engine's dispatch agree on. The editor's add-layer op uses
   this so a bad name is refused before it reaches the document. */
bool IsPrimitive(const std::string& s);

nlohmann::json EffectLayerToJson(const EffectLayer& l);
nlohmann::json EffectLayersToJson(const std::vector<EffectLayer>& layers);

bool EffectLayerFromJson(const nlohmann::json& j, EffectLayer& l,
                         const std::string& path,
                         std::vector<std::string>* errors = nullptr);
/* `base` prefixes error paths (default "layers"; the workspace
   parser passes "effects.layers"). */
bool EffectLayersFromJson(const nlohmann::json& j,
                          std::vector<EffectLayer>& out,
                          std::vector<std::string>* errors = nullptr,
                          const std::string& base = "layers");

/*---------------------------------------------------------*\
|||| Effect document <-> JSON. Documents parse as         ||
|||| ordered_json so remix draws land in file order.      ||
|||| Unknown ROOT keys are tolerated (forward compat);    ||
|||| unknown LAYER keys are errors.                       |
\*---------------------------------------------------------*/
nlohmann::ordered_json EffectDocumentToJson(const EffectDocument& d);
bool EffectDocumentFromJson(const nlohmann::ordered_json& j,
                            EffectDocument& d,
                            std::vector<std::string>* errors = nullptr);
bool EffectDocumentFromJsonFile(const std::string& path,
                                EffectDocument& d,
                                std::vector<std::string>* errors = nullptr);

/* Draw every remix spec in document order and return the
   resolved literal stack. Never throws. */
bool ResolveEffectLayers(const EffectDocument& d, unsigned int seed,
                         std::vector<EffectLayer>& out,
                         std::vector<std::string>* errors = nullptr);
bool ResolveEffectLayers(const nlohmann::ordered_json& layers,
                         unsigned int seed,
                         std::vector<EffectLayer>& out,
                         std::vector<std::string>* errors = nullptr);

} /* namespace studio */
