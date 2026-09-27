/*---------------------------------------------------------*\
|| NamedPipe.cpp                                             ||
||                                                           ||
||   SERVER side of the named-pipe transport: the          ||
||   restricted DACL (BuildRestrictedSddl /                ||
||   PipeSecurityAttributes) and NamedPipeListener ->      ||
||   IpcServer::Serve. The CLIENT side — NamedPipeChannel  ||
||   + ConnectNamedPipeChannel — lives in                  ||
||   NamedPipeClient.cpp so codec+transport-only targets   ||
||   (`openrgb2_ipc_wire`, e.g. the UI process) never      ||
||   link the server machinery.                            ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "NamedPipe.h"

#include "NamedPipeInternal.h"

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

using pipe_detail::LastErrorText;
using pipe_detail::SetNonblockingWait;

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
