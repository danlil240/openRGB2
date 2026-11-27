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
#include <vector>

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
    /* Per-channel manual-reset event signaled once by Close():
       overlapped waits include it so teardown wakes a parked
       read/write instantly instead of riding out the caller's
       deadline (Close's own CancelIoEx covers ops already
       pending; the event also catches the narrow window where
       a call loaded the handle but hasn't issued its op yet).
       A creation failure degrades to deadline-only waits. */
    close_event_ =
        (void*)CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

NamedPipeChannel::~NamedPipeChannel()
{
    Close();
    if(close_event_)
    {
        CloseHandle((HANDLE)close_event_);
        close_event_ = nullptr;
    }
}

bool NamedPipeChannel::IsOpen() const
{
    return handle_.load() != nullptr;
}

void NamedPipeChannel::Close()
{
    /* Mark shutdown first — new I/O calls short-circuit on the
       null handle. Then signal + cancel BEFORE taking the
       direction mutexes: an in-flight RecvSome/SendAll parked
       in its overlapped wait wakes on close_event_ (or the
       cancel) and unwinds promptly, so the locks below are
       bounded by the cancel — never by a caller's deadline.
       CloseHandle runs only once every Win32 call on the
       handle has fully returned: the mutexes pin it across the
       whole overlapped op, so the kernel can never recycle the
       handle out from under a live ReadFile/WriteFile. */
    void* h = handle_.exchange(nullptr);
    if(!h)
    {
        return;
    }
    if(close_event_)
    {
        SetEvent((HANDLE)close_event_);
    }
    CancelIoEx((HANDLE)h, nullptr);
    std::lock_guard<std::mutex> rl(read_mutex_);
    std::lock_guard<std::mutex> wl(write_mutex_);
    CloseHandle((HANDLE)h);
}

IpcIoResult NamedPipeChannel::RecvSome(void* buf, size_t max_len,
                                       size_t& got,
                                       uint32_t deadline_ms,
                                       std::string& err)
{
    got = 0;
    /* read_mutex_ pins the handle across the WHOLE overlapped
       op — ReadFile, the deadline wait, GetOverlappedResult —
       and serializes readers (channels are single-reader by
       contract). It does NOT cover writes: the pipe is full
       duplex and a pending read must never block the writer.
       No PeekNamedPipe poll: the read itself pends until bytes
       arrive or the deadline expires. */
    std::lock_guard<std::mutex> lock(read_mutex_);
    void* hv = handle_.load();
    if(!hv)
    {
        err = "channel closed";
        return IpcIoResult::Closed;
    }
    const HANDLE h = (HANDLE)hv;

    /* Per-call event: direction mutexes already serialize the
       waiters, but a private event means a late-completing
       cancelled op can never leave a stale signal for the
       NEXT call to consume. */
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if(!ev)
    {
        err = "CreateEvent: " +
              pipe_detail::LastErrorText(GetLastError());
        return IpcIoResult::Error;
    }
    OVERLAPPED ov{};
    ov.hEvent  = ev;
    const DWORD want =
        (DWORD)(max_len > 0x7FFFFFFF ? 0x7FFFFFFF : max_len);
    DWORD n = 0;
    if(!ReadFile(h, buf, want, &n, &ov))
    {
        const DWORD e = GetLastError();
        if(e != ERROR_IO_PENDING)
        {
            CloseHandle(ev);
            return pipe_detail::MapPipeError(e, "ReadFile", err);
        }
        const pipe_detail::OvWait wr = pipe_detail::WaitOverlapped(
            h, ov, ev, (HANDLE)close_event_,
            pipe_detail::NowMs() + deadline_ms, err);
        if(wr != pipe_detail::OvWait::Completed)
        {
            CloseHandle(ev);
            return wr == pipe_detail::OvWait::Timeout
                       ? IpcIoResult::Timeout
                       : (wr == pipe_detail::OvWait::Closed
                              ? IpcIoResult::Closed
                              : IpcIoResult::Error);
        }
        if(!GetOverlappedResult(h, &ov, &n, FALSE))
        {
            const DWORD ge = GetLastError();
            CloseHandle(ev);
            return pipe_detail::MapPipeError(ge, "ReadFile", err);
        }
    }
    CloseHandle(ev);
    if(n == 0)
    {
        /* A zero-byte read completion is the pipe's EOF —
           the peer's write end is gone. */
        err = "peer closed";
        return IpcIoResult::Closed;
    }
    got = n;
    return IpcIoResult::Ok;
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
            err = "timeout";
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
    /* write_mutex_ pins the handle across the whole send and
       serializes writers (the session's reply path and event
       writer can both reach here). A full pipe no longer busy-
       polls: the WriteFile pends and the wait carries the
       deadline. */
    std::lock_guard<std::mutex> lock(write_mutex_);
    while(sent < len)
    {
        void* hv = handle_.load();
        if(!hv)
        {
            err = "channel closed";
            return IpcIoResult::Closed;
        }
        const HANDLE h = (HANDLE)hv;
        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if(!ev)
        {
            err = "CreateEvent: " +
                  pipe_detail::LastErrorText(GetLastError());
            return IpcIoResult::Error;
        }
        OVERLAPPED ov{};
        ov.hEvent  = ev;
        const DWORD want =
            (DWORD)((len - sent) > 0x7FFFFFFF ? 0x7FFFFFFF
                                            : (len - sent));
        DWORD n = 0;
        if(!WriteFile(h, p + sent, want, &n, &ov))
        {
            const DWORD e = GetLastError();
            if(e != ERROR_IO_PENDING)
            {
                CloseHandle(ev);
                return pipe_detail::MapPipeError(e, "WriteFile",
                                                 err);
            }
            const pipe_detail::OvWait wr =
                pipe_detail::WaitOverlapped(
                    h, ov, ev, (HANDLE)close_event_,
                    deadline_abs, err);
            if(wr != pipe_detail::OvWait::Completed)
            {
                CloseHandle(ev);
                return wr == pipe_detail::OvWait::Timeout
                           ? IpcIoResult::Timeout
                           : (wr == pipe_detail::OvWait::Closed
                                  ? IpcIoResult::Closed
                                  : IpcIoResult::Error);
            }
            if(!GetOverlappedResult(h, &ov, &n, FALSE))
            {
                const DWORD ge = GetLastError();
                CloseHandle(ev);
                return pipe_detail::MapPipeError(ge, "WriteFile",
                                                 err);
            }
        }
        CloseHandle(ev);
        if(n == 0)
        {
            /* A zero-progress write on a wait-mode pipe should
               never happen; don't let it spin hot. */
            if(pipe_detail::RemainingMs(deadline_abs) == 0)
            {
                err = "timeout";
                return IpcIoResult::Timeout;
            }
            Sleep(1);
            continue;
        }
        sent += n;
    }
    return IpcIoResult::Ok;
}

/*---------------------------------------------------------*\
| Server verification                                       |
|                                                           |
| A client must not talk to whoever happens to own the pipe |
| NAME: another local account could create it first (squat) |
| and harvest requests. Accepted servers:                   |
|  - the pipe object's OWNER is this user, LocalSystem, or  |
|    BUILTIN\Administrators. The engine's descriptor sets no |
|    explicit owner, so the owner is its token's default    |
|    owner: the user SID when unelevated, Administrators    |
|    when the (managed) engine runs ELEVATED as the same    |
|    user — that case must keep working for an unelevated   |
|    UI. Only an elevated/admin process can create an       |
|    Administrators-owned object, and such a process is     |
|    already trusted with the whole machine.                |
|  - otherwise, best effort: the server PROCESS's token     |
|    user equals ours (GetNamedPipeServerProcessId).        |
\*---------------------------------------------------------*/

namespace
{

bool CurrentUserSid(std::vector<uint8_t>& out)
{
    HANDLE token = nullptr;
    if(!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        return false;
    }
    DWORD len = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &len);
    bool ok = false;
    if(len > 0)
    {
        std::vector<uint8_t> buf(len);
        if(GetTokenInformation(token, TokenUser, buf.data(), len, &len))
        {
            const PSID sid = ((const TOKEN_USER*)buf.data())->User.Sid;
            const DWORD n  = GetLengthSid(sid);
            out.assign((const uint8_t*)sid, (const uint8_t*)sid + n);
            ok = true;
        }
    }
    CloseHandle(token);
    return ok;
}

bool ServerProcessIsUser(HANDLE pipe, PSID self)
{
    ULONG pid = 0;
    if(!GetNamedPipeServerProcessId(pipe, &pid) || pid == 0)
    {
        return false;
    }
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                              FALSE, pid);
    if(!proc)
    {
        return false;
    }
    bool   ok    = false;
    HANDLE token = nullptr;
    if(OpenProcessToken(proc, TOKEN_QUERY, &token))
    {
        DWORD len = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &len);
        if(len > 0)
        {
            std::vector<uint8_t> buf(len);
            if(GetTokenInformation(token, TokenUser, buf.data(), len,
                                   &len))
            {
                ok = EqualSid(((const TOKEN_USER*)buf.data())->User.Sid,
                              self) != FALSE;
            }
        }
        CloseHandle(token);
    }
    CloseHandle(proc);
    return ok;
}

} /* anonymous namespace */

bool VerifyPipeServer(void* pipe_handle, std::string& err)
{
    const HANDLE h = (HANDLE)pipe_handle;
    std::vector<uint8_t> self;
    if(!CurrentUserSid(self))
    {
        err = "cannot read the current user SID: " +
              pipe_detail::LastErrorText(GetLastError());
        return false;
    }
    const PSID self_sid = (PSID)self.data();

    /* Owner of the pipe object (READ_CONTROL is part of the
       GENERIC_READ the client opened with). */
    DWORD need = 0;
    GetKernelObjectSecurity(h, OWNER_SECURITY_INFORMATION, nullptr, 0,
                            &need);
    if(need > 0)
    {
        std::vector<uint8_t> sd(need);
        PSID  owner     = nullptr;
        BOOL  defaulted = FALSE;
        if(GetKernelObjectSecurity(h, OWNER_SECURITY_INFORMATION,
                                   (PSECURITY_DESCRIPTOR)sd.data(),
                                   need, &need) &&
           GetSecurityDescriptorOwner((PSECURITY_DESCRIPTOR)sd.data(),
                                      &owner, &defaulted) &&
           owner != nullptr)
        {
            if(EqualSid(owner, self_sid) ||
               IsWellKnownSid(owner, WinLocalSystemSid) ||
               IsWellKnownSid(owner, WinBuiltinAdministratorsSid))
            {
                return true;
            }
        }
    }
    if(ServerProcessIsUser(h, self_sid))
    {
        return true;
    }
    err = "pipe server is not owned by this user (refusing a "
          "possibly squatted pipe name)";
    return false;
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
        /* FILE_FLAG_OVERLAPPED is mandatory: RecvSome/SendAll
           drive this handle with real OVERLAPPED ops — a
           synchronous handle would make every "pending" wait
           behave undefined (deadlock on timeout paths).
           SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION: the
           server may identify this client but never impersonate
           it (the default would be SecurityImpersonation). */
        HANDLE h = CreateFileA(pipe_name.c_str(),
                               GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED |
                                   SECURITY_SQOS_PRESENT |
                                   SECURITY_IDENTIFICATION,
                               nullptr);
        if(h != INVALID_HANDLE_VALUE)
        {
            /* Verify WHO serves the name before a single byte
               goes out — fail closed. */
            std::string verr;
            if(!VerifyPipeServer((void*)h, verr))
            {
                CloseHandle(h);
                err = verr;
                return nullptr;
            }
            /* Byte + wait mode is the client default — nothing
               else to set: deadline control comes from the
               overlapped waits, not PIPE_NOWAIT polling. */
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
bool VerifyPipeServer(void*, std::string& err)
{ err = "unsupported platform"; return false; }

} /* namespace ipc */
} /* namespace openrgb2 */

#endif /* _WIN32 */
