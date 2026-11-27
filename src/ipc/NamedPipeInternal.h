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

#include "IpcChannel.h"

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

/* Maps a failed channel I/O call's Win32 error onto the
   channel contract: peer teardown and locally-cancelled I/O
   are clean Closed outcomes, anything else is a transport
   Error carrying "<op>: <win32 text>". */
inline IpcIoResult MapPipeError(DWORD e, const char* op,
                                std::string& err)
{
    switch(e)
    {
    case ERROR_BROKEN_PIPE:
    case ERROR_NO_DATA:
    case ERROR_PIPE_NOT_CONNECTED:
    case ERROR_PIPE_LISTENING:
    case ERROR_INVALID_HANDLE:
        err = "peer closed";
        return IpcIoResult::Closed;
    case ERROR_OPERATION_ABORTED:
        /* CancelIoEx from our own Close() — a local close,
           not a transport fault. */
        err = "channel closed";
        return IpcIoResult::Closed;
    default:
        err = std::string(op) + ": " + LastErrorText(e);
        return IpcIoResult::Error;
    }
}

/* Result of WaitOverlapped: the op completed, the deadline
   elapsed, the channel's close event fired, or the wait
   itself failed (err carries the reason). */
enum class OvWait { Completed, Timeout, Closed, Error };

/* Waits out an ERROR_IO_PENDING overlapped op until it
   completes, the absolute deadline passes, or close_ev (the
   channel's manual-reset close signal, optional) fires.
   Anything but Completed cancels the op and DRAINS it with a
   blocking GetOverlappedResult — the caller's OVERLAPPED and
   I/O buffer are guaranteed dead before this returns, so
   stack storage is safe to unwind. If the drain shows the op
   completed with data anyway (it raced the cancel), the
   result is Completed: transferred bytes are never dropped. */
inline OvWait WaitOverlapped(HANDLE h, OVERLAPPED& ov, HANDLE ev,
                             HANDLE close_ev,
                             uint64_t deadline_abs_ms,
                             std::string& err)
{
    HANDLE       wait_set[2] = { ev, close_ev };
    const DWORD  nwait       = close_ev ? 2 : 1;
    OvWait       result      = OvWait::Completed;
    for(;;)
    {
        const DWORD w = WaitForMultipleObjects(
            nwait, wait_set, FALSE, RemainingMs(deadline_abs_ms));
        if(w == WAIT_OBJECT_0)
        {
            /* The overlapped op completed — nothing pending,
               nothing to drain. */
            return OvWait::Completed;
        }
        if(w == WAIT_OBJECT_0 + 1)
        {
            err    = "channel closed";
            result = OvWait::Closed;
        }
        else if(w == WAIT_TIMEOUT)
        {
            err    = "timeout";
            result = OvWait::Timeout;
        }
        else
        {
            err = "WaitForMultipleObjects: " +
                  LastErrorText(w == WAIT_FAILED ? GetLastError()
                                                : w);
            result = OvWait::Error;
        }
        CancelIoEx(h, &ov);
        DWORD n = 0;
        if(GetOverlappedResult(h, &ov, &n, TRUE) && n > 0)  /* drain */
        {
            /* The op completed in the race with the cancel:
               its bytes were really transferred (read out of
               the pipe / written into it). Reporting a timeout
               here would lose received data or invite a
               duplicate write — the caller re-queries the
               (now final) result and consumes it. */
            err.clear();
            return OvWait::Completed;
        }
        return result;
    }
}

} /* namespace pipe_detail */
} /* namespace ipc */
} /* namespace openrgb2 */

#endif /* _WIN32 */
