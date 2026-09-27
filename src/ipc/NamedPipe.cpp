/*---------------------------------------------------------*\
|| NamedPipe.cpp                                             ||
||                                                           ||
||   Win32 named-pipe transport + the restricted DACL.     ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "NamedPipe.h"

#include <atomic>
#include <chrono>
#include <thread>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>

namespace openrgb2
{
namespace ipc
{

namespace
{

uint64_t NowMs()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint32_t RemainingMs(uint64_t deadline_abs_ms)
{
    const uint64_t now = NowMs();
    if(now >= deadline_abs_ms)
    {
        return 0;
    }
    return (uint32_t)(deadline_abs_ms - now);
}

std::string LastErrorText(DWORD e)
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
bool SetNonblockingWait(HANDLE h)
{
    DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
    return SetNamedPipeHandleState(h, &mode, nullptr, nullptr) != 0;
}

} /* anonymous namespace */

/*---------------------------------------------------------*\
|| Restricted DACL                                          ||
\*---------------------------------------------------------*/

bool BuildRestrictedSddl(std::string& sddl_out, std::string& err)
{
    /* The pipe belongs to the creating user (and LocalSystem
       for service-managed scenarios) — nobody else. The
       owner's SID comes from the process token, so the DACL
       always names a concrete account, not a placeholder. */
    HANDLE token = nullptr;
    if(!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        err = "OpenProcessToken: " + LastErrorText(GetLastError());
        return false;
    }

    char   sid_buf[4096];
    DWORD  sid_len = sizeof(sid_buf);
    bool   ok      = false;
    std::string sid;
    if(GetTokenInformation(token, TokenUser, sid_buf, sid_len,
                           &sid_len))
    {
        const TOKEN_USER* tu = (const TOKEN_USER*)sid_buf;
        LPSTR sid_str        = nullptr;
        if(ConvertSidToStringSidA((PSID)tu->User.Sid, &sid_str))
        {
            sid = sid_str;
            LocalFree(sid_str);
            ok  = true;
        }
    }
    CloseHandle(token);
    if(!ok)
    {
        err = "token user SID: " + LastErrorText(GetLastError());
        return false;
    }

    /* D:P = protected DACL (no inheritance). GA = GENERIC_ALL.
       Only the owner SID and LocalSystem (SY) get ACEs —
       deliberately NO Everyone (WD/S-1-1-0), Builtin\Users
       (BU), Anonymous (AN), or Network (NU) access. */
    sddl_out = "D:P(A;;GA;;;" + sid + ")(A;;GA;;;SY)";
    return true;
}

struct PipeSecurityAttributes::Impl
{
    SECURITY_ATTRIBUTES sa{};
    void*               sd = nullptr;   /* LocalFree on release  */
    std::string         sddl;
    bool                ok = false;
};

PipeSecurityAttributes::PipeSecurityAttributes()
    : impl_(new Impl())
{
    std::string err;
    if(!BuildRestrictedSddl(impl_->sddl, err))
    {
        return;
    }
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorA(
           impl_->sddl.c_str(), SDDL_REVISION_1, &impl_->sd,
           nullptr))
    {
        impl_->sddl.clear();
        return;
    }
    impl_->sa.nLength              = sizeof(SECURITY_ATTRIBUTES);
    impl_->sa.lpSecurityDescriptor = impl_->sd;
    impl_->sa.bInheritHandle       = FALSE;
    impl_->ok                      = true;
}

PipeSecurityAttributes::~PipeSecurityAttributes()
{
    if(impl_->sd)
    {
        LocalFree(impl_->sd);
    }
    delete impl_;
}

void* PipeSecurityAttributes::Raw()
{
    return impl_->ok ? (void*)&impl_->sa : nullptr;
}

const std::string& PipeSecurityAttributes::Sddl() const
{
    return impl_->sddl;
}

bool PipeSecurityAttributes::Ok() const
{
    return impl_->ok;
}

/*---------------------------------------------------------*\
|| NamedPipeChannel                                          ||
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
    const uint64_t deadline_abs = NowMs() + deadline_ms;
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
            err = "PeekNamedPipe: " + LastErrorText(e);
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
                err = "ReadFile: " + LastErrorText(e);
                return IpcIoResult::Error;
            }
            got = n;
            return IpcIoResult::Ok;
        }
        if(RemainingMs(deadline_abs) == 0)
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
    const uint64_t deadline_abs = NowMs() + deadline_ms;
    while(got < len)
    {
        const uint32_t left = RemainingMs(deadline_abs);
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
    const uint64_t deadline_abs = NowMs() + deadline_ms;
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
                err = "WriteFile: " + LastErrorText(e);
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
        if(RemainingMs(deadline_abs) == 0)
        {
            return IpcIoResult::Timeout;
        }
        Sleep(1);
    }
    return IpcIoResult::Ok;
}

/*---------------------------------------------------------*\
|| Client connect                                            ||
\*---------------------------------------------------------*/

std::unique_ptr<IIpcChannel> ConnectNamedPipeChannel(
    const std::string& pipe_name, uint32_t timeout_ms,
    std::string& err)
{
    const uint64_t deadline_abs = NowMs() + timeout_ms;
    for(;;)
    {
        HANDLE h = CreateFileA(pipe_name.c_str(),
                               GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_EXISTING,
                               0, nullptr);
        if(h != INVALID_HANDLE_VALUE)
        {
            if(!SetNonblockingWait(h))
            {
                err = "SetNamedPipeHandleState: " +
                      LastErrorText(GetLastError());
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
            err = "CreateFile: " + LastErrorText(e);
            return nullptr;
        }
        if(RemainingMs(deadline_abs) == 0)
        {
            err = "pipe not available within " +
                  std::to_string(timeout_ms) + " ms";
            return nullptr;
        }
        Sleep(5);
    }
}

/*---------------------------------------------------------*\
|| NamedPipeListener                                         ||
\*---------------------------------------------------------*/

struct NamedPipeListener::Impl
{
    IpcServer&           server;
    ChannelKind          kind;
    std::string          name;
    PipeSecurityAttributes security;

    std::thread          thread;
    std::atomic<bool>    stop{ false };
    std::atomic<bool>    running{ false };

    Impl(IpcServer& s, ChannelKind k, std::string n)
        : server(s), kind(k), name(std::move(n))
    {
    }

    void AcceptLoop()
    {
        /* The first instance (FILE_FLAG_FIRST_PIPE_INSTANCE)
           is created in Start(); every later instance is an
           additional instance of the already-owned name.
           Never create an instance without the restricted
           DACL — no default-open fallback. */
        if(!security.Ok())
        {
            running.store(false);
            return;
        }
        while(!stop.load())
        {
            HANDLE h = CreateNamedPipeA(
                name.c_str(), PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                32 /* bounded instance cap */,
                64 * 1024, 64 * 1024, 0,
                (LPSECURITY_ATTRIBUTES)security.Raw());
            if(h == INVALID_HANDLE_VALUE)
            {
                /* Transient failure — retry on a bounded
                   cadence; never spins hot. */
                Sleep(50);
                continue;
            }

            const BOOL connected = ConnectNamedPipe(h, nullptr)
                || (GetLastError() == ERROR_PIPE_CONNECTED);
            if(stop.load())
            {
                CloseHandle(h);
                break;
            }
            if(!connected)
            {
                CloseHandle(h);
                continue;
            }
            if(!SetNonblockingWait(h))
            {
                CloseHandle(h);
                continue;
            }
            server.Serve(std::unique_ptr<IIpcChannel>(
                             new NamedPipeChannel((void*)h)),
                         kind);
        }
        running.store(false);
    }
};

NamedPipeListener::NamedPipeListener(IpcServer& server,
                                     ChannelKind kind,
                                     std::string pipe_name)
    : impl_(new Impl(server, kind, std::move(pipe_name)))
{
}

NamedPipeListener::~NamedPipeListener()
{
    Stop();
    delete impl_;
}

bool NamedPipeListener::Start(std::string& err)
{
    if(impl_->running.load())
    {
        err = "listener already running";
        return false;
    }
    if(!impl_->security.Ok())
    {
        /* Never fall back to a default-open pipe. */
        err = "restricted pipe ACL could not be built";
        return false;
    }
    /* Probe the first instance synchronously so a taken name
       fails Start() instead of spinning silently. */
    HANDLE h = CreateNamedPipeA(
        impl_->name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        32, 64 * 1024, 64 * 1024, 0,
        (LPSECURITY_ATTRIBUTES)impl_->security.Raw());
    if(h == INVALID_HANDLE_VALUE)
    {
        err = "CreateNamedPipe " + impl_->name + ": " +
              LastErrorText(GetLastError());
        return false;
    }
    impl_->stop.store(false);
    impl_->running.store(true);
    /* The accept thread parks in a blocking ConnectNamedPipe
       on this first instance (PIPE_WAIT, so the connect wait
       is real); once connected it proceeds identically to
       AcceptLoop. */
    impl_->thread = std::thread([impl = impl_, h]()
        {
            const BOOL connected =
                ConnectNamedPipe(h, nullptr)
                || (GetLastError() == ERROR_PIPE_CONNECTED);
            if(connected && !impl->stop.load() &&
               SetNonblockingWait(h))
            {
                impl->server.Serve(std::unique_ptr<IIpcChannel>(
                        new NamedPipeChannel((void*)h)),
                    impl->kind);
            }
            else
            {
                CloseHandle(h);
            }
            impl->AcceptLoop();
        });
    return true;
}

void NamedPipeListener::Stop()
{
    if(!impl_->running.load() && !impl_->thread.joinable())
    {
        return;
    }
    impl_->stop.store(true);
    /* Release a ConnectNamedPipe blocked on a client: a
       self-connect completes it so the loop can observe the
       stop flag. Bounded — never waits on client work. */
    HANDLE h = CreateFileA(impl_->name.c_str(),
                           GENERIC_READ | GENERIC_WRITE,
                           0, nullptr, OPEN_EXISTING, 0, nullptr);
    if(h != INVALID_HANDLE_VALUE)
    {
        CloseHandle(h);
    }
    if(impl_->thread.joinable())
    {
        impl_->thread.join();
    }
    impl_->running.store(false);
}

bool NamedPipeListener::Running() const
{
    return impl_->running.load();
}

const std::string& NamedPipeListener::PipeName() const
{
    return impl_->name;
}

} /* namespace ipc */
} /* namespace openrgb2 */

#else /* !_WIN32 — portable-friendly stubs */

namespace openrgb2
{
namespace ipc
{

bool BuildRestrictedSddl(std::string& sddl_out, std::string& err)
{
    sddl_out.clear();
    err = "named pipes are a Windows transport";
    return false;
}

struct PipeSecurityAttributes::Impl { bool ok = false; std::string sddl; };
PipeSecurityAttributes::PipeSecurityAttributes() : impl_(new Impl()) {}
PipeSecurityAttributes::~PipeSecurityAttributes() { delete impl_; }
void* PipeSecurityAttributes::Raw() { return nullptr; }
const std::string& PipeSecurityAttributes::Sddl() const { return impl_->sddl; }
bool  PipeSecurityAttributes::Ok() const { return false; }

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

struct NamedPipeListener::Impl
{
    IpcServer& server; ChannelKind kind; std::string name;
    Impl(IpcServer& s, ChannelKind k, std::string n)
        : server(s), kind(k), name(std::move(n)) {}
};
NamedPipeListener::NamedPipeListener(IpcServer& s, ChannelKind k,
                                     std::string n)
    : impl_(new Impl(s, k, std::move(n))) {}
NamedPipeListener::~NamedPipeListener() { delete impl_; }
bool NamedPipeListener::Start(std::string& err)
{ err = "unsupported platform"; return false; }
void NamedPipeListener::Stop() {}
bool NamedPipeListener::Running() const { return false; }
const std::string& NamedPipeListener::PipeName() const
{ return impl_->name; }

} /* namespace ipc */
} /* namespace openrgb2 */

#endif /* _WIN32 */
