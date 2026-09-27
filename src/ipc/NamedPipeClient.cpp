/*---------------------------------------------------------*\
||| NamedPipeClient.cpp                                       ||
|||                                                           ||
|||   CLIENT side of the named-pipe transport:             ||
|||   NamedPipeChannel (IIpcChannel over one connected,     ||
|||   duplex, byte-mode pipe HANDLE — usable by either     ||
|||   end) and ConnectNamedPipeChannel (bounded client      ||
|||   connect).                                              ||
|||                                                           ||
|||   Deliberately split from the server half              ||
|||   (NamedPipe.cpp: restricted DACL + NamedPipeListener   ||
|||   -> IpcServer::Serve) so IPC CLIENTS — e.g. the Qt      ||
|||   UI process — can link the codec+transport-only        ||
|||   `openrgb2_ipc_wire` target without dragging the       ||
|||   server machinery and its engine/backend dependency    ||
|||   chain into their process (plan section 4: the UI      ||
|||   speaks pipe traffic only).                            ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#include "NamedPipe.h"

#include "NamedPipeInternal.h"

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

/*---------------------------------------------------------*\
| NamedPipeChannel — one connected end                      |
\*---------------------------------------------------------*/

NamedPipeChannel::NamedPipeChannel(void* handle)
    : handle_(handle)
{
}

NamedPipeChannel::~NamedPipeChannel()
{
    Close();
}

bool NamedPipeChannel::IsOpen() const
{
    return handle_.load() != nullptr;
}

void NamedPipeChannel::Close()
{
    void* h = handle_.exchange(nullptr);
    if(h)
    {
        /* Cancel any in-flight blocking call before releasing
           the handle (deadline polls exit on !handle_, but a
           racing ReadFile must see invalid-handle, not a
           recycled one — CancelIoEx + close is the safe order
           for our synchronous-handle use). */
        CancelIoEx((HANDLE)h, nullptr);
        CloseHandle((HANDLE)h);
    }
}

IpcIoResult NamedPipeChannel::RecvSome(void* buf, size_t max_len,
                                       size_t& got,
                                       uint32_t deadline_ms,
                                       std::string& err)
{
    got = 0;
    const uint64_t deadline_abs =
        pipe_detail::NowMs() + deadline_ms;
    for(;;)
    {
        void* h = handle_.load();
        if(!h)
        {
            return IpcIoResult::Closed;
        }
        DWORD avail = 0;
        if(!PeekNamedPipe((HANDLE)h, nullptr, 0, nullptr,
                          &avail, nullptr))
        {
            const DWORD e = GetLastError();
            if(e == ERROR_BROKEN_PIPE || e == ERROR_PIPE_NOT_CONNECTED
               || e == ERROR_NO_DATA || e == ERROR_PIPE_LISTENING
               || e == ERROR_INVALID_HANDLE)
            {
                return IpcIoResult::Closed;
            }
            err = "PeekNamedPipe: " + pipe_detail::LastErrorText(e);
            return IpcIoResult::Error;
        }
        if(avail > 0)
        {
            const DWORD want =
                (DWORD)((avail < max_len) ? avail : max_len);
            DWORD n = 0;
            if(!ReadFile((HANDLE)h, buf, want, &n, nullptr))
            {
                const DWORD e = GetLastError();
                if(e == ERROR_BROKEN_PIPE || e == ERROR_NO_DATA
                   || e == ERROR_PIPE_NOT_CONNECTED)
                {
                    return IpcIoResult::Closed;
                }
                err = "ReadFile: " + pipe_detail::LastErrorText(e);
                return IpcIoResult::Error;
            }
            got = n;
            return IpcIoResult::Ok;
        }
        if(pipe_detail::RemainingMs(deadline_abs) == 0)
        {
            return IpcIoResult::Timeout;
        }
        Sleep(1);
    }
}

IpcIoResult NamedPipeChannel::RecvExact(void* buf, size_t len,
                                        uint32_t deadline_ms,
                                        std::string& err)
{
    uint8_t* out = (uint8_t*)buf;
    size_t   got = 0;
    const uint64_t deadline_abs =
        pipe_detail::NowMs() + deadline_ms;
    while(got < len)
    {
        const uint32_t left =
            pipe_detail::RemainingMs(deadline_abs);
        if(left == 0)
        {
            return IpcIoResult::Timeout;
        }
        size_t n = 0;
        const IpcIoResult r =
            RecvSome(out + got, len - got, n, left, err);
        if(r != IpcIoResult::Ok)
        {
            return r;
        }
        got += n;
    }
    return IpcIoResult::Ok;
}

IpcIoResult NamedPipeChannel::SendAll(const void* data, size_t len,
                                      uint32_t deadline_ms,
                                      std::string& err)
{
    const uint8_t* p = (const uint8_t*)data;
    size_t   sent = 0;
    const uint64_t deadline_abs =
        pipe_detail::NowMs() + deadline_ms;
    while(sent < len)
    {
        void* h = handle_.load();
        if(!h)
        {
            return IpcIoResult::Closed;
        }
        DWORD n = 0;
        const DWORD want =
            (DWORD)((len - sent) > 0x7FFFFFFF ? 0x7FFFFFFF
                                            : (len - sent));
        if(!WriteFile((HANDLE)h, p + sent, want, &n, nullptr))
        {
            const DWORD e = GetLastError();
            if(e == ERROR_PIPE_BUSY || e == ERROR_NO_DATA)
            {
                /* Nonblocking-wait mode: no room right now —
                   poll inside the deadline. */
            }
            else if(e == ERROR_BROKEN_PIPE
                    || e == ERROR_PIPE_NOT_CONNECTED
                    || e == ERROR_INVALID_HANDLE)
            {
                return IpcIoResult::Closed;
            }
            else
            {
                err = "WriteFile: " + pipe_detail::LastErrorText(e);
                return IpcIoResult::Error;
            }
        }
        else
        {
            sent += n;
            if(sent >= len)
            {
                break;
            }
            if(n == 0)
            {
                /* wrote nothing — wait for the peer to drain */
            }
        }
        if(pipe_detail::RemainingMs(deadline_abs) == 0)
        {
            return IpcIoResult::Timeout;
        }
        Sleep(1);
    }
    return IpcIoResult::Ok;
}

/*---------------------------------------------------------*\
| Client connect                                            |
\*---------------------------------------------------------*/

std::unique_ptr<IIpcChannel> ConnectNamedPipeChannel(
    const std::string& pipe_name, uint32_t timeout_ms,
    std::string& err)
{
    const uint64_t deadline_abs =
        pipe_detail::NowMs() + timeout_ms;
    for(;;)
    {
        HANDLE h = CreateFileA(pipe_name.c_str(),
                               GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_EXISTING,
                               0, nullptr);
        if(h != INVALID_HANDLE_VALUE)
        {
            if(!pipe_detail::SetNonblockingWait(h))
            {
                err = "SetNamedPipeHandleState: " +
                      pipe_detail::LastErrorText(GetLastError());
                CloseHandle(h);
                return nullptr;
            }
            return std::unique_ptr<IIpcChannel>(
                new NamedPipeChannel((void*)h));
        }
        const DWORD e = GetLastError();
        if(e == ERROR_ACCESS_DENIED)
        {
            err = "access denied by pipe ACL (engine IPC is "
                  "restricted to its creating user)";
            return nullptr;
        }
        if(e != ERROR_FILE_NOT_FOUND && e != ERROR_PIPE_BUSY)
        {
            err = "CreateFile: " +
                  pipe_detail::LastErrorText(e);
            return nullptr;
        }
        if(pipe_detail::RemainingMs(deadline_abs) == 0)
        {
            err = "pipe not available within " +
                  std::to_string(timeout_ms) + " ms";
            return nullptr;
        }
        Sleep(5);
    }
}

} /* namespace ipc */
} /* namespace openrgb2 */

#else /* !_WIN32 — portable-friendly stubs */

namespace openrgb2
{
namespace ipc
{

NamedPipeChannel::NamedPipeChannel(void* handle) : handle_(handle) {}
NamedPipeChannel::~NamedPipeChannel() {}
bool NamedPipeChannel::IsOpen() const { return false; }
void NamedPipeChannel::Close() {}
IpcIoResult NamedPipeChannel::RecvExact(void*, size_t, uint32_t,
                                        std::string& err)
{ err = "unsupported platform"; return IpcIoResult::Error; }
IpcIoResult NamedPipeChannel::RecvSome(void*, size_t, size_t&,
                                     uint32_t, std::string& err)
{ err = "unsupported platform"; return IpcIoResult::Error; }
IpcIoResult NamedPipeChannel::SendAll(const void*, size_t, uint32_t,
                                      std::string& err)
{ err = "unsupported platform"; return IpcIoResult::Error; }

std::unique_ptr<IIpcChannel> ConnectNamedPipeChannel(
    const std::string&, uint32_t, std::string& err)
{ err = "unsupported platform"; return nullptr; }

} /* namespace ipc */
} /* namespace openrgb2 */

#endif /* _WIN32 */
