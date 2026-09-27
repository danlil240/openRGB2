/*---------------------------------------------------------*\
||| NamedPipeInternal.h                                       ||
|||                                                           ||
|||   Shared implementation helpers for the named-pipe     ||
|||   transport — used by BOTH the client half             ||
|||   (NamedPipeClient.cpp: channel + connect) and the     ||
|||   server half (NamedPipe.cpp: restricted DACL +        ||
|||   listener). Not a public surface; src/ipc only.       ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace openrgb2
{
namespace ipc
{
namespace pipe_detail
{

inline uint64_t NowMs()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline uint32_t RemainingMs(uint64_t deadline_abs_ms)
{
    const uint64_t now = NowMs();
    if(now >= deadline_abs_ms)
    {
        return 0;
    }
    return (uint32_t)(deadline_abs_ms - now);
}

inline std::string LastErrorText(DWORD e)
{
    char buf[256];
    const DWORD n = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, e, 0, buf, sizeof(buf), nullptr);
    if(n == 0)
    {
        return "win32 error " + std::to_string(e);
    }
    std::string s(buf, n);
    while(!s.empty() && (s.back() == '\r' || s.back() == '\n' ||
                        s.back() == ' '))
    {
        s.pop_back();
    }
    return s;
}

/* Puts a connected pipe end into nonblocking-wait byte mode
   so every I/O can honor a deadline by polling. */
inline bool SetNonblockingWait(HANDLE h)
{
    DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
    return SetNamedPipeHandleState(h, &mode, nullptr, nullptr) != 0;
}

} /* namespace pipe_detail */
} /* namespace ipc */
} /* namespace openrgb2 */

#endif /* _WIN32 */
