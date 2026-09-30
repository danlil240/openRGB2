/*---------------------------------------------------------*\
||| EngineLog.h                                             |
|||                                                           |
|||   Shared low-dependency log seam for the Qt-free        |
|||   engine components. A component exposes a SetLogger-   |
|||   style sink (EngineLogFn); when none is installed the  |
|||   line falls back to stderr with the engine prefix —    |
|||   the same format EngineHost installs by default, so    |
|||   unwired components still surface diagnostics in the   |
|||   standalone process.                                   |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
|\*---------------------------------------------------------*/

#pragma once

#include <cstdio>
#include <functional>
#include <string>

namespace openrgb2
{

using EngineLogFn = std::function<void(const std::string& line)>;

/* Emit `line` to the installed sink, else to stderr with the
   engine prefix (EngineHost's default-sink format). */
inline void EmitEngineLog(const EngineLogFn& sink,
                          const std::string& line)
{
    if(sink)
    {
        sink(line);
    }
    else
    {
        std::fprintf(stderr, "openrgb2-engine: %s\n",
                     line.c_str());
    }
}

} /* namespace openrgb2 */
