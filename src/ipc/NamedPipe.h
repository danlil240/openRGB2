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
||   Channel I/O is deadline-bounded (IIpcChannel): the    ||
||   pipe runs in nonblocking-wait mode and reads/writes   ||
||   poll inside the caller's deadline — a wedged peer     ||
||   can never pin a session thread.                       ||
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
||   D:P(A;;GA;;;<token-user-sid>)(A;;GA;;;SY)              ||
|| = protected DACL, GENERIC_ALL to the creating user and   ||
|| to LocalSystem only. No Everyone/Users/Anonymous/Network ||
|| ACE — the pipe is not default-open.                      ||
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
|| HANDLE (server or client end). Nonblocking-wait mode;   ||
|| all I/O honors the per-call deadline.                   ||
\*---------------------------------------------------------*/
class NamedPipeChannel final : public IIpcChannel
{
public:
    /* Takes ownership of `handle` (a connected, duplex,
       byte-mode pipe). */
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
};

/* Client-side connect (test fixtures, Milestone 4 helpers):
   opens an existing pipe with a bounded wait. */
std::unique_ptr<IIpcChannel> ConnectNamedPipeChannel(
    const std::string& pipe_name, uint32_t timeout_ms,
    std::string& err);

/*---------------------------------------------------------*\
|| NamedPipeListener — creates pipe instances under the    ||
|| restricted DACL and hands each accepted connection to   ||
|| IpcServer::Serve on its own accept thread.              ||
||                                                          ||
|| Stop() is bounded: a self-connect releases a blocked    ||
|| ConnectNamedPipe and the thread joins.                  ||
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
