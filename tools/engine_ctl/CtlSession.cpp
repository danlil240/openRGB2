/*---------------------------------------------------------*\
|| CtlSession.cpp — openrgb2-ctl session + composites        ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "CtlSession.h"

#include "NamedPipe.h"
#include "WorkspaceTransfer.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

namespace openrgb2
{
namespace ctl
{

std::string DiagPipeFor(const std::string& command_pipe)
{
    return command_pipe + "-diag";
}

bool CtlSession::Connect(const std::string& pipe, uint32_t timeout_ms,
                         std::string& err)
{
    channel_ = ipc::ConnectNamedPipeChannel(pipe, timeout_ms, err);
    if(!channel_)
    {
        return false;
    }
    reader_ = std::make_unique<ipc::MessageReader>(*channel_);
    const std::vector<uint8_t> hello = ipc::EncodeMessage(
        ipc::MsgType::Hello, 0,
        nlohmann::json::object({
            { "client_name", "openrgb2-ctl" },
            { "min_version", 1 },
            { "max_version", 1 },
        }));
    if(hello.empty() ||
       channel_->SendAll(hello.data(), hello.size(), 5000, err)
           != ipc::IpcIoResult::Ok)
    {
        return false;
    }
    ipc::Message msg;
    std::string detail;
    if(reader_->Read(msg, 5000, detail) != ipc::ReadResult::Ok ||
       msg.type != ipc::MsgType::Welcome)
    {
        char code[8];
        std::snprintf(code, sizeof code, "%04x", (unsigned)msg.type);
        err = "handshake failed: " + detail + " (reply type 0x" +
              code + ")";
        return false;
    }
    welcome_ = msg.Body();
    return true;
}

bool CtlSession::Request(ipc::MsgType type, const nlohmann::json& body,
                         ipc::Message& reply, std::string& err,
                         uint32_t deadline_ms)
{
    if(!channel_)
    {
        err = "not connected";
        return false;
    }
    const uint32_t id = ++req_id_;
    const std::vector<uint8_t> wire =
        ipc::EncodeMessage(type, id, body);
    if(wire.empty())
    {
        err = "encode failed (body over 1 MiB bound)";
        return false;
    }
    if(channel_->SendAll(wire.data(), wire.size(), deadline_ms,
                         err) != ipc::IpcIoResult::Ok)
    {
        return false;
    }
    for(;;)
    {
        std::string detail;
        const ipc::ReadResult rr =
            reader_->Read(reply, deadline_ms, detail);
        if(rr != ipc::ReadResult::Ok)
        {
            err = "read failed: " + detail;
            return false;
        }
        if(reply.request_id == id)
        {
            return true;
        }
        /* An EVENT push between request and reply — surface it
           and keep waiting for our reply. */
        if(reply.type == ipc::MsgType::Event && events_)
        {
            std::fprintf(events_, "EVENT %s\n",
                         reply.Body().dump().c_str());
        }
    }
}

bool CtlSession::Send(ipc::MsgType type, const nlohmann::json& body,
                      std::string& err, uint32_t deadline_ms)
{
    if(!channel_)
    {
        err = "not connected";
        return false;
    }
    const std::vector<uint8_t> wire =
        ipc::EncodeMessage(type, ++req_id_, body);
    if(wire.empty())
    {
        err = "encode failed (body over 1 MiB bound)";
        return false;
    }
    return channel_->SendAll(wire.data(), wire.size(), deadline_ms,
                             err) == ipc::IpcIoResult::Ok;
}

ipc::ReadResult CtlSession::Read(ipc::Message& msg,
                                 uint32_t deadline_ms,
                                 std::string& detail)
{
    if(!reader_)
    {
        detail = "not connected";
        return ipc::ReadResult::Closed;
    }
    return reader_->Read(msg, deadline_ms, detail);
}

bool WorkspaceCall(CtlSession& s, const nlohmann::json& body,
                   nlohmann::json& reply, std::string& err)
{
    ipc::Message m;
    if(!s.Request(ipc::MsgType::WorkspaceCommand, body, m, err))
    {
        return false;
    }
    reply = m.Body();
    if(m.type == ipc::MsgType::Error)
    {
        err = reply.value("error", std::string("error")) + ": " +
              reply.value("message", std::string());
        return false;
    }
    return true;
}

bool WorkspaceRun(CtlSession& s, const nlohmann::json& body,
                  uint32_t timeout_ms, nlohmann::json& out,
                  std::string& err)
{
    nlohmann::json ack;
    if(!WorkspaceCall(s, body, ack, err))
    {
        out = ack;
        return false;
    }
    if(!ack.is_object() || !ack.contains("accepted"))
    {
        out = ack;                  /* a synchronous verb's reply */
        return true;
    }
    if(!ack.value("accepted", false))
    {
        out = ack;
        err = "job refused: " +
              ack.value("detail", std::string("not accepted"));
        return false;
    }
    const uint64_t job_id = ack.value("job_id", (uint64_t)0);
    if(job_id == 0)
    {
        out = ack;
        err = "accepted reply carries no job_id";
        return false;
    }
    /* Jobs are session-bound: poll job_status on THIS session —
       disconnecting would cancel a queued job. */
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    for(;;)
    {
        nlohmann::json st;
        if(!WorkspaceCall(s, nlohmann::json::object({
                              { "verb",   "job_status" },
                              { "job_id", job_id       },
                          }), st, err))
        {
            out = st;
            return false;
        }
        const std::string state = st.value("state", std::string());
        if(state == "succeeded" || state == "failed" ||
           state == "cancelled")
        {
            out = st;
            return true;
        }
        if(std::chrono::steady_clock::now() >= deadline)
        {
            out = st;
            err = "job " + std::to_string(job_id) +
                  " not finished within " +
                  std::to_string(timeout_ms) + " ms (state " +
                  state + ")";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

bool UploadBytes(CtlSession& s, const std::string& kind,
                 const std::string& bytes, std::string& staged,
                 std::string& err)
{
    nlohmann::json r;
    if(!WorkspaceCall(s, nlohmann::json::object({
                          { "verb",      "transfer_begin"       },
                          { "direction", "upload"               },
                          { "kind",      kind                   },
                          { "length",    (uint64_t)bytes.size() },
                          { "sha256",    ipc::Sha256Hex(bytes)  },
                      }), r, err))
    {
        return false;
    }
    const std::string tid = r.value("transfer_id", std::string());
    if(tid.empty())
    {
        err = "transfer_begin returned no transfer_id";
        return false;
    }
    /* Stay under the engine's declared per-chunk bound. */
    size_t chunk = (size_t)r.value("max_chunk_bytes",
                                   (uint64_t)(192 * 1024));
    chunk = (std::min<size_t>)((std::max<size_t>)(chunk, 1024),
                               192 * 1024);
    size_t off = 0;
    while(off < bytes.size())
    {
        const size_t n = (std::min)(bytes.size() - off, chunk);
        nlohmann::json cr;
        if(!WorkspaceCall(s, nlohmann::json::object({
                              { "verb",        "transfer_chunk" },
                              { "transfer_id", tid              },
                              { "offset",      (uint64_t)off    },
                              { "data", ipc::Base64Encode(
                                            bytes.data() + off, n) },
                          }), cr, err))
        {
            return false;
        }
        off += n;
    }
    nlohmann::json fr;
    if(!WorkspaceCall(s, nlohmann::json::object({
                          { "verb",        "transfer_finish" },
                          { "transfer_id", tid               },
                      }), fr, err))
    {
        return false;
    }
    staged = fr.value("staged", std::string());
    if(staged.empty())
    {
        err = "transfer_finish returned no staged token";
        return false;
    }
    return true;
}

bool DownloadTransfer(CtlSession& s, const std::string& transfer_id,
                      std::string& bytes, std::string& err)
{
    bytes.clear();
    /* Bounded walk: the engine caps a bundle at 64 MiB and a
       chunk at 192 KiB — the cap just keeps a misbehaving server
       from spinning the CLI forever. */
    constexpr int kMaxChunks = 4096;
    bool eof = false;
    for(int i = 0; i < kMaxChunks && !eof; ++i)
    {
        nlohmann::json cr;
        if(!WorkspaceCall(s, nlohmann::json::object({
                              { "verb",        "transfer_chunk"    },
                              { "transfer_id", transfer_id         },
                              { "offset",      (uint64_t)bytes.size() },
                          }), cr, err))
        {
            return false;
        }
        std::vector<uint8_t> dec;
        if(!ipc::Base64Decode(cr.value("data", std::string()), dec,
                              256 * 1024))
        {
            err = "transfer_chunk: bad base64 payload";
            return false;
        }
        bytes.append((const char*)dec.data(), dec.size());
        eof = cr.value("eof", false);
        if(!eof && dec.empty())
        {
            err = "transfer_chunk: no progress before eof";
            return false;
        }
    }
    if(!eof)
    {
        err = "download did not reach eof";
        return false;
    }
    nlohmann::json fr;
    return WorkspaceCall(s, nlohmann::json::object({
                             { "verb",        "transfer_finish" },
                             { "transfer_id", transfer_id       },
                         }), fr, err);
}

bool ExportWorkspace(CtlSession& s, const std::string& id,
                     uint32_t timeout_ms, std::string& bundle,
                     nlohmann::json& job_status, std::string& err)
{
    nlohmann::json body = nlohmann::json::object({
        { "verb", "export" },
    });
    if(!id.empty())
    {
        body["id"] = id;
    }
    if(!WorkspaceRun(s, body, timeout_ms, job_status, err))
    {
        return false;
    }
    if(job_status.value("state", std::string()) != "succeeded")
    {
        err = "export " + job_status.value("state", std::string()) +
              ": " + job_status.value("error", std::string()) +
              (job_status.contains("detail")
                   ? " (" + job_status.value("detail", std::string()) +
                         ")"
                   : std::string());
        return false;
    }
    const nlohmann::json res =
        job_status.value("result", nlohmann::json::object());
    const std::string tid = res.value("transfer_id", std::string());
    if(tid.empty())
    {
        err = "export result carries no transfer_id";
        return false;
    }
    if(!DownloadTransfer(s, tid, bundle, err))
    {
        return false;
    }
    if(bundle.size() != res.value("bytes", (uint64_t)0) ||
       ipc::Sha256Hex(bundle) != res.value("sha256", std::string()))
    {
        err = "downloaded bundle does not match the declared "
              "length/sha256";
        return false;
    }
    return true;
}

} /* namespace ctl */
} /* namespace openrgb2 */
