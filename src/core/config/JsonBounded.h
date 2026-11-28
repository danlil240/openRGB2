/*---------------------------------------------------------*\
||| JsonBounded.h                                             ||
|||                                                           ||
|||   Depth-capped JSON parse for untrusted documents.      ||
|||   nlohmann's SAX parser is iterative, but copy and      ||
|||   dump recurse once per nesting level — a few hundred   ||
|||   KB of '[' overflows a thread stack the moment the     ||
|||   parsed value is copied (StudioConfig keeps            ||
|||   `extensions` verbatim) or serialized. A stack         ||
|||   overflow is an SEH fault nothing catches, so every    ||
|||   file or staged upload a user can hand the engine      ||
|||   (staged documents, bundles, imported workspace        ||
|||   files, preset folders) parses through here; the IPC   ||
|||   wire codec (ipc::ParseJsonBounded) shares this        ||
|||   implementation and its 64-level cap.                  ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace studio
{

/* Container nesting cap — the same bound the IPC wire codec
   applies to message bodies (ipc::kMaxJsonDepth). */
constexpr int kMaxJsonDepth = 64;

/* is_discarded() on invalid JSON or on container nesting
   deeper than max_depth; the over-deep part is never
   materialized (the callback drops it during the SAX walk).
   Never throws. Works for nlohmann::json and ordered_json. */
template<class Json = nlohmann::json>
Json ParseJsonBounded(const std::string& text,
                      int max_depth = kMaxJsonDepth)
{
    /* The callback sees every container start with its depth
       (the number of enclosing containers). Past the cap the
       container — and, once tripped, everything after it — is
       discarded. */
    bool too_deep = false;
    const typename Json::parser_callback_t cb =
        [&too_deep, max_depth](int depth,
                               typename Json::parse_event_t ev,
                               Json&) -> bool
        {
            if(too_deep)
            {
                return false;
            }
            if((ev == Json::parse_event_t::object_start ||
                ev == Json::parse_event_t::array_start) &&
               depth >= max_depth)
            {
                too_deep = true;
                return false;
            }
            return true;
        };
    Json j = Json::parse(text, cb, false);
    if(too_deep)
    {
        return Json(Json::value_t::discarded);
    }
    return j;
}

} /* namespace studio */
