/*---------------------------------------------------------*\
|| SceneJson.h                                               |
||                                                           |
||   SceneDocument <-> JSON. Versioned; unknown fields are  |
||   ignored so newer documents degrade instead of failing. |
||                                                           |
||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include "SceneTypes.h"
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace studio
{

nlohmann::json ToJson(const SceneDocument& doc);

/* Parse a document. Returns false on malformed input, a version
   newer than the current schema (no silent downgrade), or a failed
   graph validation (dangling/cyclic parent or mirror references);
   `doc` stays untouched on failure and `errors`, when given,
   collects the validation messages. */
bool FromJson(const nlohmann::json& j, SceneDocument& doc,
              std::vector<std::string>* errors = nullptr);

/* Shared color coding: writes "#RRGGBB"; reads that or the legacy
   packed 0x00BBGGRR integer. Used by the scene and workspace
   serializers. */
std::string SceneColorHex(SceneColor c);
bool        ParseSceneColor(const nlohmann::json& v, SceneColor& out);

} /* namespace studio */
