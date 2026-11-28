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

/*---------------------------------------------------------*\
|| Accept-side connect wait                                  ||
||                                                           ||
|| ConnectNamedPipe on a FILE_FLAG_OVERLAPPED instance      ||
|| must carry a real OVERLAPPED: it pends until a client    ||
|| attaches. The wait polls the listener stop flag so a     ||
|| pending connect unwinds even if Stop()'s self-connect    ||
|| somehow never lands; a stop with the connect still       ||
|| pending cancels + drains it (the OVERLAPPED is stack     ||
|| storage — it must be dead before CloseHandle).           ||
\*---------------------------------------------------------*/
static bool WaitForClientConnect(HANDLE h,
                                 const std::atomic<bool>& stop)
{
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if(!ev)
    {
        return false;
    }
    OVERLAPPED ov{};
    ov.hEvent      = ev;
    bool connected = false;
    if(ConnectNamedPipe(h, &ov))
    {
        connected = true;
    }
    else
    {
        const DWORD e = GetLastError();
        if(e == ERROR_PIPE_CONNECTED)
        {
            connected = true;   /* client beat us to it        */
        }
        else if(e == ERROR_IO_PENDING)
        {
            for(;;)
            {
                const DWORD w = WaitForSingleObject(ev, 100);
                if(w == WAIT_OBJECT_0)
                {
                    DWORD done = 0;
                    connected =
                        GetOverlappedResult(h, &ov, &done,
                                            FALSE) != 0;
                    break;
                }
                if(w != WAIT_TIMEOUT || stop.load())
                {
                    /* Stop with the connect still pending (or a
                       wait failure): cancel + drain. */
                    CancelIoEx(h, &ov);
                    DWORD done = 0;
                    GetOverlappedResult(h, &ov, &done, TRUE);
                    break;
                }
            }
        }
    }
    CloseHandle(ev);
    return connected;
}

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
       Only the owner SID and LocalSystem (SY) get allow ACEs —
       deliberately NO Everyone (WD/S-1-1-0), Builtin\Users
       (BU) or Anonymous (AN) access. The leading DENY for
       Network (NU, S-1-5-2 — present in every network-logon
       token) keeps the creating user's OWN account from
       reaching the pipe over SMB; PIPE_REJECT_REMOTE_CLIENTS
       on every instance enforces the same at the pipe layer.

       S:(ML;;NW;;;ME) = an EXPLICIT Medium no-write-up label
       (audit I-9). An elevated engine's objects are otherwise
       unlabeled (implicitly Medium), so this changes no access
       today — it pins the decided policy instead of leaning on
       the default: medium clients of this user may drive an
       elevated engine (the shipped topology: elevated stack,
       possibly non-elevated UI / openrgb2-ctl), low-integrity
       and sandboxed callers never can. A High label would lock
       the medium UI out; the engine verbs take no client paths,
       so a medium client cannot borrow the engine's rights for
       file writes. The listener re-checks the floor per client
       (ClientIntegrityAllowed). */
    sddl_out = "D:P(D;;GA;;;NU)(A;;GA;;;" + sid + ")(A;;GA;;;SY)"
               "S:(ML;;NW;;;ME)";
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

/* One listening instance with the restricted DACL. Local
   clients only: PIPE_REJECT_REMOTE_CLIENTS refuses SMB
   connections at the pipe layer (the DACL's NU deny is the
   second line). FILE_FLAG_OVERLAPPED: the accepted channel's
   RecvSome/SendAll drive this handle with real OVERLAPPED
   ops — a synchronous instance would wedge those waits. */
static HANDLE CreatePipeInstance(const std::string& name, bool first,
                                 PipeSecurityAttributes& security)
{
    return CreateNamedPipeA(
        name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
            (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        32 /* bounded instance cap */,
        64 * 1024, 64 * 1024, 0,
        (LPSECURITY_ATTRIBUTES)security.Raw());
}

/* Engine-side integrity floor (audit I-9): a connected client
   must run at Medium or above. The Medium no-write-up label
   already keeps low-integrity callers from opening the pipe for
   write; this per-connection check makes the floor independent
   of the descriptor and fails closed when the client's token
   cannot be read. Medium is deliberate (see BuildRestrictedSddl):
   an elevated engine still serves a non-elevated UI. */
static bool ClientIntegrityAllowed(HANDLE pipe)
{
    ULONG pid = 0;
    if(!GetNamedPipeClientProcessId(pipe, &pid) || pid == 0)
    {
        return false;
    }
    uint32_t rid = 0;
    return QueryIntegrity(IntegritySubject::PipeClient, pid, rid) &&
           rid >= kIntegrityMedium;
}

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

    /* `cur` is the first instance (FILE_FLAG_FIRST_PIPE_INSTANCE,
       created in Start()); every later instance is an
       additional instance of the already-owned name. Never
       create an instance without the restricted DACL — no
       default-open fallback.

       The NEXT listening instance is created BEFORE the
       current one is handed to Serve() or closed, so the name
       never drops to zero instances while the listener runs —
       otherwise another account could re-create the name
       (FILE_FLAG_FIRST_PIPE_INSTANCE) with its own DACL in
       that window and capture the next client. */
    void AcceptLoop(HANDLE cur)
    {
        while(!stop.load())
        {
            if(cur == INVALID_HANDLE_VALUE)
            {
                cur = CreatePipeInstance(name, false, security);
                if(cur == INVALID_HANDLE_VALUE)
                {
                    /* Transient failure — retry on a bounded
                       cadence; never spins hot. */
                    Sleep(50);
                    continue;
                }
            }

            const bool connected = WaitForClientConnect(cur, stop);
            if(stop.load())
            {
                break;
            }
            /* May fail (instance cap, resources): then the loop
               top retries — the window only reopens in that
               failure case. */
            HANDLE next = CreatePipeInstance(name, false, security);
            if(connected && ClientIntegrityAllowed(cur))
            {
                server.Serve(std::unique_ptr<IIpcChannel>(
                                 new NamedPipeChannel((void*)cur)),
                             kind);
            }
            else
            {
                CloseHandle(cur);
            }
            cur = next;
        }
        if(cur != INVALID_HANDLE_VALUE)
        {
            CloseHandle(cur);
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
    HANDLE h = CreatePipeInstance(impl_->name, true, impl_->security);
    if(h == INVALID_HANDLE_VALUE)
    {
        err = "CreateNamedPipe " + impl_->name + ": " +
              LastErrorText(GetLastError());
        return false;
    }
    impl_->stop.store(false);
    impl_->running.store(true);
    /* The accept thread parks in an overlapped ConnectNamedPipe
       wait on this first instance (released by a client attach
       or Stop()'s self-connect). */
    impl_->thread = std::thread([impl = impl_, h]()
        {
            impl->AcceptLoop(h);
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
