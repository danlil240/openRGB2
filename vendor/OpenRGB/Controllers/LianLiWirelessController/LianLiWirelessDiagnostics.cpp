/*---------------------------------------------------------*\
|| LianLiWirelessDiagnostics.cpp                             ||
||                                                           ||
||   Bounded performance diagnostics for the Lian Li        ||
||   wireless path.                                          ||
||                                                           ||
||   This file is part of the OpenRGB project                ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "LianLiWirelessDiagnostics.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>

using namespace LianLiWireless;

namespace
{
    constexpr uint64_t DEFAULT_MAX_ROWS     = 1000000;   /* ~50 MB cap */
    constexpr auto     SUMMARY_INTERVAL     = std::chrono::seconds(30);
    constexpr unsigned FLUSH_EVERY_ROWS     = 20;

    uint64_t NowUsSince(const std::chrono::steady_clock::time_point& origin)
    {
        return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - origin).count();
    }

    uint64_t EnvRows(const char* name, uint64_t fallback)
    {
        const char* raw = std::getenv(name);
        if(raw == nullptr || *raw == '\0')
        {
            return fallback;
        }
        char* end    = nullptr;
        long long v  = std::strtoll(raw, &end, 10);
        if(end == raw || v < 0)
        {
            return fallback;
        }
        return (uint64_t)v;
    }
}

struct Diagnostics::TraceSink
{
    std::string                   path;
    std::string                   summary_path;
    std::ofstream                trace;
    uint64_t                      rows          = 0;
    uint64_t                      max_rows      = 0;
    bool                          capped        = false;
    std::chrono::steady_clock::time_point origin;
    std::chrono::steady_clock::time_point last_summary;
};

Diagnostics& Diagnostics::Get()
{
    static Diagnostics instance;
    return instance;
}

Diagnostics::Diagnostics()
{
    const char* path = std::getenv("OPENRGB_LIANLI_TRACE");
    if(path != nullptr && *path != '\0')
    {
        OpenTrace(path, EnvRows("OPENRGB_LIANLI_TRACE_MAX_ROWS", DEFAULT_MAX_ROWS));
    }
}

Diagnostics::~Diagnostics()
{
    std::lock_guard<std::mutex> lock(mutex);
    if(sink != nullptr && sink->trace.is_open())
    {
        WriteSummaryLocked();
        sink->trace.flush();
    }
}

void Diagnostics::Reset()
{
    std::lock_guard<std::mutex> lock(mutex);
    counters = DiagCounters();
}

DiagCounters Diagnostics::Snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return counters;
}

void Diagnostics::RecordSetDesired(bool accepted, bool replaced_pending,
                                    uint64_t lock_wait_us, uint64_t held_us,
                                    uint64_t packets)
{
    std::lock_guard<std::mutex> lock(mutex);
    if(accepted)
    {
        counters.set_desired++;
        if(replaced_pending)
        {
            counters.replaced_pending++;
        }
    }
    else
    {
        counters.set_desired_rejected++;
    }
    counters.lock_wait_us      += lock_wait_us;
    if(lock_wait_us > counters.lock_wait_us_max)
    {
        counters.lock_wait_us_max = lock_wait_us;
    }
    counters.set_desired_us    += held_us;
    if(held_us > counters.set_desired_us_max)
    {
        counters.set_desired_us_max = held_us;
    }
    TraceLocked("set_desired", (long long)lock_wait_us, (long long)held_us,
                (long long)packets);
}

void Diagnostics::RecordStepAll(uint64_t lock_wait_us, uint64_t held_us,
                                uint64_t groups)
{
    std::lock_guard<std::mutex> lock(mutex);
    counters.step_all++;
    counters.lock_wait_us      += lock_wait_us;
    if(lock_wait_us > counters.lock_wait_us_max)
    {
        counters.lock_wait_us_max = lock_wait_us;
    }
    counters.step_all_us       += held_us;
    if(held_us > counters.step_all_us_max)
    {
        counters.step_all_us_max = held_us;
    }
    TraceLocked("step_all", (long long)lock_wait_us, (long long)held_us,
                (long long)groups);
}

void Diagnostics::RecordCompress(uint64_t duration_us, uint64_t bytes_in,
                                  uint64_t bytes_out)
{
    std::lock_guard<std::mutex> lock(mutex);
    counters.compress_calls++;
    counters.compress_us        += duration_us;
    if(duration_us > counters.compress_us_max)
    {
        counters.compress_us_max = duration_us;
    }
    counters.compress_bytes_in += bytes_in;
    counters.compress_bytes_out += bytes_out;
    TraceLocked("compress", (long long)duration_us, (long long)bytes_in,
                (long long)bytes_out);
}

void Diagnostics::RecordUploadAdmitted(uint64_t rf_packets,
                                        uint64_t header_repeats,
                                        uint64_t duration_us)
{
    std::lock_guard<std::mutex> lock(mutex);
    counters.uploads_admitted++;
    counters.rf_packets      += rf_packets;
    counters.header_repeats  += header_repeats;
    counters.upload_us       += duration_us;
    if(duration_us > counters.upload_us_max)
    {
        counters.upload_us_max = duration_us;
    }
    TraceLocked("upload", (long long)duration_us, (long long)rf_packets,
                (long long)header_repeats);
}

void Diagnostics::RecordUploadRetry()
{
    std::lock_guard<std::mutex> lock(mutex);
    counters.upload_retries++;
}

void Diagnostics::RecordUploadDeferred()
{
    std::lock_guard<std::mutex> lock(mutex);
    counters.uploads_deferred++;
    TraceLocked("defer", 0, 0, 0);
}

void Diagnostics::RecordUploadFailure()
{
    std::lock_guard<std::mutex> lock(mutex);
    counters.upload_failures++;
    TraceLocked("upload_fail", 0, 0, 0);
}

void Diagnostics::RecordDiscovery(uint64_t duration_us, bool target_seen,
                                  uint64_t records, int64_t sighting_age_ms)
{
    std::lock_guard<std::mutex> lock(mutex);
    counters.discovery_polls++;
    if(!target_seen)
    {
        counters.discovery_misses++;
    }
    counters.discovery_records += records;
    counters.discovery_us       += duration_us;
    if(duration_us > counters.discovery_us_max)
    {
        counters.discovery_us_max = duration_us;
    }
    if(sighting_age_ms >= 0)
    {
        counters.discovery_age_ms = sighting_age_ms;
        if((uint64_t)sighting_age_ms > counters.discovery_age_ms_max)
        {
            counters.discovery_age_ms_max = (uint64_t)sighting_age_ms;
        }
    }
    TraceLocked("poll", (long long)duration_us, (long long)records,
                (long long)sighting_age_ms);
}

void Diagnostics::RecordDiscoveryDeferred()
{
    std::lock_guard<std::mutex> lock(mutex);
    counters.discovery_deferred++;
    TraceLocked("poll_defer", 0, 0, 0);
}

void Diagnostics::RecordKeepAlive(int64_t gap_ms, uint64_t duration_us)
{
    std::lock_guard<std::mutex> lock(mutex);
    counters.keepalives++;
    if(gap_ms >= 0 && (uint64_t)gap_ms > counters.keepalive_gap_ms_max)
    {
        counters.keepalive_gap_ms_max = (uint64_t)gap_ms;
    }
    TraceLocked("keepalive", (long long)gap_ms, (long long)duration_us, 0);
}

void Diagnostics::RecordKeepAliveFailure()
{
    std::lock_guard<std::mutex> lock(mutex);
    counters.keepalive_failures++;
    TraceLocked("keepalive_fail", 0, 0, 0);
}

void Diagnostics::RecordUsbSend(uint64_t chunks, uint64_t duration_us)
{
    std::lock_guard<std::mutex> lock(mutex);
    counters.usb_chunk_batches++;
    counters.usb_chunks   += chunks;
    counters.usb_send_us  += duration_us;
    if(duration_us > counters.usb_send_us_max)
    {
        counters.usb_send_us_max = duration_us;
    }
    TraceLocked("usb_send", (long long)duration_us, (long long)chunks, 0);
}

void Diagnostics::RecordUsbRead(uint64_t duration_us)
{
    std::lock_guard<std::mutex> lock(mutex);
    counters.usb_reads++;
    counters.usb_read_us += duration_us;
    if(duration_us > counters.usb_read_us_max)
    {
        counters.usb_read_us_max = duration_us;
    }
    TraceLocked("usb_read", (long long)duration_us, 0, 0);
}

void Diagnostics::RecordTransportRevalidation()
{
    std::lock_guard<std::mutex> lock(mutex);
    counters.transport_revalidations++;
    TraceLocked("revalidate", 0, 0, 0);
}

void Diagnostics::RecordForeignMaster()
{
    std::lock_guard<std::mutex> lock(mutex);
    counters.foreign_master_events++;
    TraceLocked("foreign_master", 0, 0, 0);
}

void Diagnostics::OpenTrace(const std::string& path, uint64_t max_rows)
{
    std::lock_guard<std::mutex> lock(mutex);
    sink = std::make_unique<TraceSink>();
    sink->path         = path;
    sink->summary_path = path + ".summary";
    sink->max_rows     = max_rows;
    sink->origin       = std::chrono::steady_clock::now();
    sink->last_summary = sink->origin;
    sink->trace.open(path, std::ios::out | std::ios::trunc);
    if(sink->trace.is_open())
    {
        sink->trace << "time_us,event,first_us,second_us,third\n";
    }
}

void Diagnostics::CloseTrace()
{
    std::lock_guard<std::mutex> lock(mutex);
    if(sink == nullptr)
    {
        return;
    }
    if(sink->trace.is_open())
    {
        WriteSummaryLocked();
        sink->trace.flush();
        sink->trace.close();
    }
}

bool Diagnostics::TraceOpen() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return sink != nullptr && sink->trace.is_open();
}

void Diagnostics::Trace(const char* event, long long first, long long second,
                        long long third)
{
    std::lock_guard<std::mutex> lock(mutex);
    TraceLocked(event, first, second, third);
}

uint64_t Diagnostics::TraceRows() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return sink != nullptr ? sink->rows : 0;
}

void Diagnostics::TraceLocked(const char* event, long long first,
                              long long second, long long third)
{
    if(sink == nullptr || !sink->trace.is_open() || sink->capped)
    {
        return;
    }

    if(sink->rows >= sink->max_rows)
    {
        sink->capped = true;
        sink->trace << NowUsSince(sink->origin) << ",trace_capped,0,0,0\n";
        sink->trace.flush();
        WriteSummaryLocked();
        return;
    }

    sink->trace << NowUsSince(sink->origin) << ',' << event << ',' << first
               << ',' << second << ',' << third << '\n';
    sink->rows++;

    if((sink->rows % FLUSH_EVERY_ROWS) == 0)
    {
        sink->trace.flush();
    }
    if(std::chrono::steady_clock::now() - sink->last_summary > SUMMARY_INTERVAL)
    {
        sink->last_summary = std::chrono::steady_clock::now();
        WriteSummaryLocked();
    }
}

void Diagnostics::WriteSummaryLocked() const
{
    if(sink == nullptr || sink->trace.is_open() == false)
    {
        return;
    }

    std::ofstream summary(sink->summary_path, std::ios::out | std::ios::trunc);
    if(!summary.is_open())
    {
        return;
    }

    summary << "counter,value\n";
    #define DIAG_FIELD(name) summary << #name << ',' << counters.name << '\n';
    DIAG_FIELD(set_desired)
    DIAG_FIELD(set_desired_rejected)
    DIAG_FIELD(replaced_pending)
    DIAG_FIELD(set_desired_us)
    DIAG_FIELD(set_desired_us_max)
    DIAG_FIELD(lock_wait_us)
    DIAG_FIELD(lock_wait_us_max)
    DIAG_FIELD(step_all)
    DIAG_FIELD(step_all_us)
    DIAG_FIELD(step_all_us_max)
    DIAG_FIELD(compress_calls)
    DIAG_FIELD(compress_us)
    DIAG_FIELD(compress_us_max)
    DIAG_FIELD(compress_bytes_in)
    DIAG_FIELD(compress_bytes_out)
    DIAG_FIELD(uploads_admitted)
    DIAG_FIELD(uploads_deferred)
    DIAG_FIELD(upload_retries)
    DIAG_FIELD(upload_failures)
    DIAG_FIELD(rf_packets)
    DIAG_FIELD(header_repeats)
    DIAG_FIELD(upload_us)
    DIAG_FIELD(upload_us_max)
    DIAG_FIELD(usb_chunk_batches)
    DIAG_FIELD(usb_chunks)
    DIAG_FIELD(usb_send_us)
    DIAG_FIELD(usb_send_us_max)
    DIAG_FIELD(usb_reads)
    DIAG_FIELD(usb_read_us)
    DIAG_FIELD(usb_read_us_max)
    DIAG_FIELD(discovery_polls)
    DIAG_FIELD(discovery_misses)
    DIAG_FIELD(discovery_records)
    DIAG_FIELD(discovery_deferred)
    DIAG_FIELD(discovery_us)
    DIAG_FIELD(discovery_us_max)
    DIAG_FIELD(discovery_age_ms)
    DIAG_FIELD(discovery_age_ms_max)
    DIAG_FIELD(keepalives)
    DIAG_FIELD(keepalive_failures)
    DIAG_FIELD(keepalive_gap_ms_max)
    DIAG_FIELD(transport_revalidations)
    DIAG_FIELD(foreign_master_events)
    #undef DIAG_FIELD
    summary << "trace_rows," << sink->rows << '\n';
    summary << "trace_capped," << (sink->capped ? 1 : 0) << '\n';
}
