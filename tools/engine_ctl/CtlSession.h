/*---------------------------------------------------------*\
|| CtlSession.h — openrgb2-ctl                               ||
||                                                           ||
||   The CLI's IPC session plus the composite workspace    ||
||   flows that must stay inside ONE session (audit I-10): ||
||   jobs, transfers and staged tokens are session-bound —  ||
||   session teardown cancels queued jobs and drops staged  ||
||   transfers (docs/ipc-protocol.md section 5) — so a CLI  ||
||   that connects per request could enqueue an export but  ||
||   never read it. Split from engine_ctl.cpp so            ||
||   tests/engine/engine_ctl_test.cpp drives the same code  ||
||   against a real engine service over a real pipe.        ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "IpcChannel.h"
#include "IpcProtocol.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace openrgb2
{
namespace ctl
{

/* The read-only diagnostics pipe that pairs with a command pipe
   name: "<pipe>-diag" (the engine's naming — the default
   \\.\pipe\openrgb2-engine maps to \\.\pipe\openrgb2-engine-diag).
   `diag` used to ignore --pipe and always hit the default. */
std::string DiagPipeFor(const std::string& command_pipe);

/* One attached session: HELLO/WELCOME, request/reply with EVENT
   pushes skipped (echoed to `events` when non-null). */
class CtlSession
{
public:
    explicit CtlSession(std::FILE* events = nullptr)
        : events_(events)
    {
    }

    bool Connect(const std::string& pipe, uint32_t timeout_ms,
                 std::string& err);

    /* Sends one request; skips EVENT pushes until the matching
       reply arrives. */
    bool Request(ipc::MsgType type, const nlohmann::json& body,
                 ipc::Message& reply, std::string& err,
                 uint32_t deadline_ms = 5000);

    /* Sends one request without waiting for its reply. */
    bool Send(ipc::MsgType type, const nlohmann::json& body,
              std::string& err, uint32_t deadline_ms = 5000);

    /* Reads the next message (event pump for subscribe). */
    ipc::ReadResult Read(ipc::Message& msg, uint32_t deadline_ms,
                         std::string& detail);

    const nlohmann::json& Welcome() const { return welcome_; }

private:
    std::unique_ptr<ipc::IIpcChannel>    channel_;
    std::unique_ptr<ipc::MessageReader>  reader_;
    uint32_t                             req_id_ = 0;
    nlohmann::json                       welcome_;
    std::FILE*                           events_ = nullptr;
};

/* One WORKSPACE_COMMAND. False on a transport failure or an
   ERROR reply (then `reply` is the error body and err carries
   "<error>: <message>"). */
bool WorkspaceCall(CtlSession& s, const nlohmann::json& body,
                   nlohmann::json& reply, std::string& err);

/* A workspace verb that may start a job: when the reply is
   {accepted:true, job_id}, polls job_status IN THIS SESSION until
   the job is terminal and returns that status record; any other
   reply is returned as-is. False on transport/ERROR, a busy
   refusal (accepted:false) or a job that outlives timeout_ms. */
bool WorkspaceRun(CtlSession& s, const nlohmann::json& body,
                  uint32_t timeout_ms, nlohmann::json& out,
                  std::string& err);

/* transfer_begin(upload) -> ordered chunks -> transfer_finish;
   returns the session-owned `staged` token. */
bool UploadBytes(CtlSession& s, const std::string& kind,
                 const std::string& bytes, std::string& staged,
                 std::string& err);

/* Reads a staged download to EOF, then transfer_finish. */
bool DownloadTransfer(CtlSession& s, const std::string& transfer_id,
                      std::string& bytes, std::string& err);

/* `export` job -> download its bundle -> verify the job's
   declared length + SHA-256. `id` empty = current workspace. */
bool ExportWorkspace(CtlSession& s, const std::string& id,
                     uint32_t timeout_ms, std::string& bundle,
                     nlohmann::json& job_status, std::string& err);

} /* namespace ctl */
} /* namespace openrgb2 */
