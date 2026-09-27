/*---------------------------------------------------------*\
|| IpcChannel.h                                              ||
||                                                           ||
||   Duplex byte-channel abstraction for the engine IPC    ||
||   layer (plan section 4: versioned local named pipes).  ||
||   The production implementation is NamedPipeChannel     ||
||   (Win32 named pipe, restricted ACL); tests inject an   ||
||   in-process implementation so the offline suite is     ||
||   fully deterministic — no real pipes, no threads on    ||
||   the codec path.                                       ||
||                                                           ||
||   Every I/O call carries a deadline: a wedged or        ||
||   hostile peer can never hold a session thread          ||
||   forever (plan section 6, bounded everything).         ||
||   Deadlines are milliseconds-from-call-start, applied   ||
||   to the WHOLE call including all fragments.            ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace openrgb2
{
namespace ipc
{

enum class IpcIoResult
{
    Ok,        /* exactly the requested bytes moved         */
    Closed,    /* peer performed an orderly close / broken  */
    Timeout,   /* deadline elapsed before completion        */
    Error,     /* transport error — err is set              */
};

class IIpcChannel
{
public:
    virtual ~IIpcChannel() = default;

    /* Reads exactly len bytes (looping over fragments) or
       returns a failure. Timeout/Closed are clean outcomes,
       not errors. deadline_ms bounds the entire call. */
    virtual IpcIoResult RecvExact(void* buf, size_t len,
                                  uint32_t deadline_ms,
                                  std::string& err) = 0;

    /* Reads at least 1 and at most max_len bytes (got is set
       on Ok). Used by the resync scan so it can consume a
       garbage burst in chunks without ever waiting for bytes
       that were never sent. Timeout when nothing arrives by
       the deadline. */
    virtual IpcIoResult RecvSome(void* buf, size_t max_len,
                                 size_t& got, uint32_t deadline_ms,
                                 std::string& err) = 0;

    /* Sends every byte (looping over partial sends), bounded
       by deadline_ms. A peer that never drains the pipe
       produces Timeout, never an unbounded block. */
    virtual IpcIoResult SendAll(const void* data, size_t len,
                                uint32_t deadline_ms,
                                std::string& err) = 0;

    /* Orderly close; further I/O returns Closed/Error. */
    virtual void Close() = 0;
    virtual bool IsOpen() const = 0;
};

} /* namespace ipc */
} /* namespace openrgb2 */
