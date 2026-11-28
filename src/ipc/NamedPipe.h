/*---------------------------------------------------------*\
|| NamedPipe.h                                               ||
||                                                           ||
||   Windows named-pipe transport for the engine IPC       ||
||   (plan section 4 — "restricted local named pipes for   ||
||   application IPC"). Two pipes per engine:              ||
||                                                           ||
||     \\.\pipe\openrgb2-engine        command channel      ||
||     \\.\pipe\openrgb2-engine-diag   READ-ONLY diagnostics||
||                                                           ||
||   ACL: every pipe instance is created with an explicit  ||
||   restricted DACL granting GENERIC_ALL only to the      ||
||   creating user and LocalSystem — never default-open    ||
||   (other local users cannot even connect). The SDDL is  ||
||   built by BuildRestrictedSddl() so the intent is       ||
||   testable offline.                                     ||
||                                                           ||
||   Channel I/O is deadline-bounded (IIpcChannel): pipe   ||
||   handles are opened FILE_FLAG_OVERLAPPED and every     ||
||   read/write pends against a manual-reset event waited  ||
||   with the caller's deadline — a wedged peer can never  ||
||   pin a session thread, and no busy poll touches the    ||
||   pipe between request and reply.                       ||
||                                                           ||
||   The HANDLE stays inside the .cpp — no Win32 types     ||
||   leak into this header or its dependents.              ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "IpcChannel.h"
#include "IpcService.h"
#include "IpcServer.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace openrgb2
{
namespace ipc
{

/* Well-known pipe names (docs/ipc-protocol.md). */
constexpr const char* kCommandPipeName     = "\\\\.\\pipe\\openrgb2-engine";
constexpr const char* kDiagnosticsPipeName = "\\\\.\\pipe\\openrgb2-engine-diag";

/*---------------------------------------------------------*\
|| Pipe security — the restricted-DACL builder is split    ||
|| out so tests can assert the descriptor's intent without ||
|| a second user account.                                   ||
||                                                          ||
|| Produces SDDL                                            ||
||   D:P(D;;GA;;;NU)(A;;GA;;;<token-user-sid>)(A;;GA;;;SY)  ||
|| = protected DACL, GENERIC_ALL to the creating user and   ||
|| to LocalSystem only, with Network logons denied first    ||
|| (the creating user over SMB included). No Everyone/      ||
|| Users/Anonymous allow ACE — the pipe is not default-     ||
|| open. Every instance is also created with                ||
|| PIPE_REJECT_REMOTE_CLIENTS.                              ||
\*---------------------------------------------------------*/
bool BuildRestrictedSddl(std::string& sddl_out, std::string& err);

/* SECURITY_ATTRIBUTES carrying a heap descriptor built from
   BuildRestrictedSddl. The wrapper owns the descriptor
   memory — keep it alive for every CreateNamedPipe call. */
class PipeSecurityAttributes
{
public:
    PipeSecurityAttributes();
    ~PipeSecurityAttributes();

    /* Returns nullptr + err on failure (then callers may
       choose to fail pipe creation rather than fall back to
       a default-open descriptor — we never do). */
    void* Raw();                              /* SECURITY_ATTRIBUTES* */
    const std::string& Sddl() const;
    bool  Ok() const;

private:
    struct Impl;
    Impl* impl_;
};

/*---------------------------------------------------------*\
|| NamedPipeChannel — IIpcChannel over one connected pipe   ||
|| HANDLE (server or client end). Overlapped I/O: each     ||
|| call issues one ReadFile/WriteFile and waits on a       ||
|| per-call event with the caller's deadline; timeout or   ||
|| close cancels + drains the pending op.                  ||
\*---------------------------------------------------------*/
class NamedPipeChannel final : public IIpcChannel
{
public:
    /* Takes ownership of `handle` (a connected, duplex,
       byte-mode pipe opened with FILE_FLAG_OVERLAPPED —
       every channel handle must support overlapped I/O or
       the deadline waits cannot fire). */
    explicit NamedPipeChannel(void* handle);
    ~NamedPipeChannel() override;               /* Close() */

    NamedPipeChannel(const NamedPipeChannel&)            = delete;
    NamedPipeChannel& operator=(const NamedPipeChannel&) = delete;

    IpcIoResult RecvExact(void* buf, size_t len,
                          uint32_t deadline_ms,
                          std::string& err) override;
    IpcIoResult RecvSome(void* buf, size_t max_len, size_t& got,
                         uint32_t deadline_ms,
                         std::string& err) override;
    IpcIoResult SendAll(const void* data, size_t len,
                        uint32_t deadline_ms,
                        std::string& err) override;
    void        Close() override;
    bool        IsOpen() const override;

private:
    std::atomic<void*> handle_{ nullptr };
    /* Read/write pinning, per direction: RecvSome holds
       read_mutex_ for the whole overlapped op (issue + wait +
       GetOverlappedResult) and SendAll holds write_mutex_
       likewise, so CloseHandle can never release (and Windows
       recycle) the handle mid-op — the F01 discipline. The
       directions use SEPARATE mutexes because the pipe is
       full duplex: a pending read must never block a queued
       write (sessions run concurrent reader/writer threads).
       Close() signals close_event_ + CancelIoEx BEFORE taking
       both mutexes, so a parked wait unwinds on the close —
       not on its deadline. */
    std::mutex         read_mutex_;
    std::mutex         write_mutex_;
    /* Manual-reset event (HANDLE stored as void*) signaled
       once by Close(). Overlapped waits include it in their
       wait set so teardown wakes instantly; a creation
       failure leaves it null and waits just ride their
       deadline. */
    void*              close_event_ = nullptr;
};

/* Client-side connect (test fixtures, Milestone 4 helpers):
   opens an existing pipe with a bounded wait, identification-
   only impersonation level (SECURITY_SQOS_PRESENT |
   SECURITY_IDENTIFICATION), and VERIFIES the server before
   returning (VerifyPipeServer) — a squatted name fails with
   err set instead of yielding a channel. */
std::unique_ptr<IIpcChannel> ConnectNamedPipeChannel(
    const std::string& pipe_name, uint32_t timeout_ms,
    std::string& err);

/* Server check on a connected client pipe HANDLE: gathers
   PipeServerFacts (owner, server process user, the pipe
   object's mandatory label, the server process's integrity
   level, this process's integrity level) and applies
   EvaluatePipeServerTrust. */
bool VerifyPipeServer(void* pipe_handle, std::string& err);

/*---------------------------------------------------------*\
|| Integrity policy (audit I-9 / U-6)                        ||
||                                                          ||
|| The shipped topology runs the engine ELEVATED while the  ||
|| UI / openrgb2-ctl may run at medium integrity, so the    ||
|| engine pipe deliberately admits medium clients of the    ||
|| same user (explicit Medium no-write-up label — see       ||
|| BuildRestrictedSddl). The engine verbs never take client ||
|| paths, so such a client cannot borrow the engine's       ||
|| rights for file writes. What IS refused:                  ||
||                                                          ||
||   client \ server   medium    elevated   SYSTEM          ||
||   medium            allow     allow      allow           ||
||   elevated          REFUSE    allow      allow           ||
||   low / sandboxed   REFUSE (label + engine-side floor)   ||
||   any client vs a low-integrity server: REFUSE           ||
||                                                          ||
|| i.e. a client never trusts a server below its own        ||
|| integrity level (an elevated UI must not act on paths    ||
|| or data from a medium squatter), and an elevated client  ||
|| additionally requires an Administrators/LocalSystem      ||
|| pipe owner — or, on machines whose policy makes the      ||
|| creator the owner, the server token read as at least     ||
|| its own level.                                           ||
\*---------------------------------------------------------*/

/* Mandatory-label RIDs (SECURITY_MANDATORY_*_RID). */
constexpr uint32_t kIntegrityUntrusted = 0x0000;
constexpr uint32_t kIntegrityLow       = 0x1000;
constexpr uint32_t kIntegrityMedium    = 0x2000;
constexpr uint32_t kIntegrityHigh      = 0x3000;
constexpr uint32_t kIntegritySystem    = 0x4000;

enum class PipeOwnerKind
{
    Unknown,            /* owner could not be read            */
    Self,               /* this process's token user          */
    LocalSystem,
    Administrators,     /* BUILTIN\Administrators             */
    Other,
};

/* What a client knows about the server end of a pipe. */
struct PipeServerFacts
{
    PipeOwnerKind owner               = PipeOwnerKind::Unknown;
    /* GetNamedPipeServerProcessId -> token user == ours. */
    bool          server_user_is_self = false;
    /* The pipe OBJECT's mandatory label (an unlabeled object
       counts as Medium). A creator can only label at or below
       its own level, and a low-integrity creator's objects
       are always labeled low — a lower bound that needs no
       access to the server process. */
    bool          label_known         = false;
    uint32_t      label_integrity     = kIntegrityMedium;
    /* The server PROCESS's token integrity, when readable (a
       medium client usually cannot open an elevated token). */
    bool          token_known         = false;
    uint32_t      token_integrity     = 0;
    /* This (client) process's integrity level. */
    uint32_t      client_integrity    = kIntegrityMedium;
};

/* Pure trust decision over the facts (table above). Returns
   false with err set when the server must be refused. */
bool EvaluatePipeServerTrust(const PipeServerFacts& facts,
                             std::string& err);

/* Integrity-level lookups go through one probe so tests can
   simulate an elevated client / a low server / an unreadable
   token without elevating. */
enum class IntegritySubject
{
    Self,               /* this process                        */
    PipeServer,         /* client side: the server's process   */
    PipeClient,         /* listener side: the client's process */
};
using IntegrityProbe = bool (*)(IntegritySubject subject,
                                unsigned long pid,
                                uint32_t& rid_out);
/* nullptr restores the real token query. */
void SetIntegrityProbeForTesting(IntegrityProbe probe);
/* Integrity RID of `pid` (ignored for Self) through the probe;
   false when the token cannot be read. */
bool QueryIntegrity(IntegritySubject subject, unsigned long pid,
                    uint32_t& rid_out);

/*---------------------------------------------------------*\
|| NamedPipeListener — creates pipe instances under the    ||
|| restricted DACL and hands each accepted connection to   ||
|| IpcServer::Serve on its own accept thread.              ||
||                                                          ||
|| Stop() is bounded: a self-connect releases a blocked    ||
|| ConnectNamedPipe and the thread joins. The next         ||
|| listening instance is created before the current one is ||
|| served or closed, so the name never has zero instances  ||
|| (no squatting window) while the listener runs.          ||
\*---------------------------------------------------------*/
class NamedPipeListener
{
public:
    /* kind selects which verb surface accepted channels get. */
    NamedPipeListener(IpcServer& server, ChannelKind kind,
                    std::string pipe_name);
    ~NamedPipeListener();                       /* Stop() */

    NamedPipeListener(const NamedPipeListener&)            = delete;
    NamedPipeListener& operator=(const NamedPipeListener&) = delete;

    /* Creates the first instance + accept thread. Fails when
       the pipe name is taken (another engine owns it) or the
       restricted descriptor cannot be built — it never falls
       back to a weaker ACL. */
    bool        Start(std::string& err);
    void        Stop();
    bool        Running() const;
    const std::string& PipeName() const;

private:
    struct Impl;
    Impl* impl_;
};

} /* namespace ipc */
} /* namespace openrgb2 */
