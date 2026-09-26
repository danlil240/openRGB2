/*---------------------------------------------------------*\
||| JsonFields.h                                              |
|||                                                           |
|||   Shared checked JSON field readers — the ONE copy.     |
|||   scene/SceneJson.cpp and config/StudioConfig.cpp both  |
|||   consume them (they used to carry divergent private    |
|||   duplicates, which is how the effect.seed u32 bug      |
|||   slipped in). Every reader appends                     |
|||   "<path>: expected ..." instead of throwing, so a      |
|||   malformed hand-edited studio.json produces field      |
|||   errors rather than aborting the whole load.           |
|||                                                           |
|||   Qt-free; compiled under plain cl by the test suites.  |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace studio
{

inline bool IsInt(const nlohmann::json& v)
{
    return v.is_number_integer() || v.is_number_unsigned();
}

inline void AddErr(std::vector<std::string>& errs, const std::string& path,
                   const std::string& msg)
{
    errs.push_back(path + ": " + msg);
}

/*---------------------------------------------------------*\
||| Checked readers — record "<path>: expected ..."       ||
||| errors instead of throwing on malformed hand-edited   ||
||| files. FieldNum rejects non-finite values too (the    ||
||| old per-file copies diverged on exactly this).        ||
\*---------------------------------------------------------*/
inline std::string FieldStr(const nlohmann::json& j, const char* key,
                            const std::string& def, const std::string& path,
                            std::vector<std::string>& errs)
{
    if(!j.contains(key))
    {
        return def;
    }
    if(!j[key].is_string())
    {
        AddErr(errs, path + "." + key, "expected string");
        return def;
    }
    return j[key].get<std::string>();
}

inline bool FieldBool(const nlohmann::json& j, const char* key,
                      bool def, const std::string& path,
                      std::vector<std::string>& errs)
{
    if(!j.contains(key))
    {
        return def;
    }
    if(!j[key].is_boolean())
    {
        AddErr(errs, path + "." + key, "expected boolean");
        return def;
    }
    return j[key].get<bool>();
}

inline double FieldNum(const nlohmann::json& j, const char* key,
                       double def, const std::string& path,
                       std::vector<std::string>& errs)
{
    if(!j.contains(key))
    {
        return def;
    }
    if(!j[key].is_number())
    {
        AddErr(errs, path + "." + key, "expected number");
        return def;
    }
    const double v = j[key].get<double>();
    if(!std::isfinite(v))
    {
        AddErr(errs, path + "." + key, "expected finite number");
        return def;
    }
    return v;
}

/* Full-width integer read: returns int64 so callers narrow with
   their own bounds (FieldI32/FieldU32 cover the common cases). */
inline long long FieldInt(const nlohmann::json& j, const char* key,
                          long long def, const std::string& path,
                          std::vector<std::string>& errs)
{
    if(!j.contains(key))
    {
        return def;
    }
    if(!IsInt(j[key]))
    {
        AddErr(errs, path + "." + key, "expected integer");
        return def;
    }
    /* An unsigned value >= 2^63 wraps negative on the narrowing
       get<long long>; the caller's range check still rejects it. */
    return j[key].get<long long>();
}

/* int32 read — rejects out-of-range values instead of letting
   get<int> wrap them (a 2^32+k address would alias to k). */
inline int FieldI32(const nlohmann::json& j, const char* key,
                    int def, const std::string& path,
                    std::vector<std::string>& errs)
{
    if(!j.contains(key))
    {
        return def;
    }
    const nlohmann::json& v = j[key];
    if(!IsInt(v))
    {
        AddErr(errs, path + "." + key, "expected integer");
        return def;
    }
    if(v.is_number_unsigned())
    {
        const unsigned long long u = v.get<unsigned long long>();
        if(u > 2147483647ull)
        {
            AddErr(errs, path + "." + key, "expected 32-bit integer");
            return def;
        }
        return (int)u;
    }
    const long long s = v.get<long long>();
    if(s < -2147483648ll || s > 2147483647ll)
    {
        AddErr(errs, path + "." + key, "expected 32-bit integer");
        return def;
    }
    return (int)s;
}

/* uint32 read with an explicit 0..UINT_MAX range check — the
   reader the old int32 path lacked: get<int> wrapped seeds
   >= 2^31 negative and rejected every other remix seed on load. */
inline unsigned int FieldU32(const nlohmann::json& j, const char* key,
                             unsigned int def, const std::string& path,
                             std::vector<std::string>& errs)
{
    if(!j.contains(key))
    {
        return def;
    }
    const nlohmann::json& v = j[key];
    if(!IsInt(v))
    {
        AddErr(errs, path + "." + key, "expected unsigned integer");
        return def;
    }
    if(v.is_number_unsigned())
    {
        const unsigned long long u = v.get<unsigned long long>();
        if(u > 4294967295ull)
        {
            AddErr(errs, path + "." + key,
                   "expected unsigned integer in 0..4294967295");
            return def;
        }
        return (unsigned int)u;
    }
    const long long s = v.get<long long>();
    /* Bound BOTH ends like FieldI32: a positive value can arrive on
       the signed path too (programmatic json(long long), or a binary
       format deserializer) — without the upper check the narrowing
       (unsigned int) cast wraps it into a silently-wrong field. */
    if(s < 0 || s > 4294967295ll)
    {
        AddErr(errs, path + "." + key,
               "expected unsigned integer in 0..4294967295");
        return def;
    }
    return (unsigned int)s;
}

} /* namespace studio */
