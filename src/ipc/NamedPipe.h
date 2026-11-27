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

/* Server check on a connected client pipe HANDLE: true when
   the pipe object's owner is this user, LocalSystem or
   BUILTIN\Administrators (an ELEVATED engine of the same
   user owns its objects as Administrators by default), or
   else when the server process's token user is this user. */
bool VerifyPipeServer(void* pipe_handle, std::string& err);

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
