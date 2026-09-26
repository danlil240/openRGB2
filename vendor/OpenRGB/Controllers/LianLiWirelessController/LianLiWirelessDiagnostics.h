/*---------------------------------------------------------*\
|| LianLiWirelessDiagnostics.h                               ||
||                                                           ||
||   Bounded performance diagnostics for the Lian Li        ||
||   wireless path. Fixed-size counters covering the         ||
||   stages named in the baseline measurement contract:      ||
||   submissions and queue replacement, service lock wait,   ||
||   compression, runtime admission, USB sends and reads,    ||
||   discovery freshness, and heartbeat gaps.                ||
||                                                           ||
||   An optional CSV trace (OPENRGB_LIANLI_TRACE=path)       ||
||   records per-event rows with a row cap and a summary     ||
||   file rewritten next to it; both are bounded.            ||
||                                                           ||
||   Trace row schema (time_us,event,first,second,third):     ||
||     set_desired  lock_wait_us  held_us       packets       ||
||     step_all     lock_wait_us  held_us       groups        ||
||     compress     duration_us   bytes_in      bytes_out     ||
||     upload       duration_us   rf_packets    header_reps   ||
||     defer        0             0             0             ||
||     upload_fail  0             0             0             ||
||     poll         duration_us   records      age_ms        ||
||     poll_defer   0             0             0             ||
||     keepalive    gap_ms        duration_us   0             ||
||     keepalive_fail 0           0             0             ||
||     usb_send     duration_us   chunks        0             ||
||     usb_read     duration_us   0             0             ||
||     revalidate   0             0             0             ||
||     foreign_master 0          0             0             ||
||                                                           ||
||   This file is part of the OpenRGB project                ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace LianLiWireless
{
    struct DiagCounters
    {
        /* submissions and queue replacement */
        uint64_t set_desired          = 0;
        uint64_t set_desired_rejected = 0;
        uint64_t replaced_pending     = 0;
        uint64_t set_desired_us       = 0;
        uint64_t set_desired_us_max   = 0;

        /* service mutex contention and worker cost */
        uint64_t lock_wait_us         = 0;
        uint64_t lock_wait_us_max     = 0;
        uint64_t step_all             = 0;
        uint64_t step_all_us          = 0;
        uint64_t step_all_us_max      = 0;

        /* compression */
        uint64_t compress_calls       = 0;
        uint64_t compress_us          = 0;
        uint64_t compress_us_max      = 0;
        uint64_t compress_bytes_in    = 0;
        uint64_t compress_bytes_out   = 0;

        /* runtime admission (RF stream budget) */
        uint64_t uploads_admitted     = 0;
        uint64_t uploads_deferred     = 0;
        uint64_t upload_retries      = 0;
        uint64_t upload_failures     = 0;
        uint64_t rf_packets           = 0;
        uint64_t header_repeats       = 0;
        uint64_t upload_us            = 0;
        uint64_t upload_us_max        = 0;

        /* USB transport */
        uint64_t usb_chunk_batches    = 0;
        uint64_t usb_chunks           = 0;
        uint64_t usb_send_us          = 0;
        uint64_t usb_send_us_max      = 0;
        uint64_t usb_reads            = 0;
        uint64_t usb_read_us          = 0;
        uint64_t usb_read_us_max      = 0;

        /* discovery */
        uint64_t discovery_polls      = 0;
        uint64_t discovery_misses     = 0;
        uint64_t discovery_records    = 0;
        uint64_t discovery_deferred   = 0;
        uint64_t discovery_us         = 0;
        uint64_t discovery_us_max     = 0;
        int64_t  discovery_age_ms     = -1;
        uint64_t discovery_age_ms_max = 0;

        /* heartbeat */
        uint64_t keepalives           = 0;
        uint64_t keepalive_failures   = 0;
        uint64_t keepalive_gap_ms_max = 0;

        /* resets and ownership */
        uint64_t transport_revalidations = 0;
        uint64_t foreign_master_events   = 0;
    };

    class Diagnostics
    {
    public:
        static Diagnostics& Get();

        /* clear counters; trace sink (if open) stays attached */
        void Reset();

        DiagCounters Snapshot() const;

        void RecordSetDesired(bool accepted, bool replaced_pending,
                              uint64_t lock_wait_us, uint64_t held_us,
                              uint64_t packets);
        void RecordStepAll(uint64_t lock_wait_us, uint64_t held_us,
                           uint64_t groups);
        void RecordCompress(uint64_t duration_us, uint64_t bytes_in,
                            uint64_t bytes_out);
        void RecordUploadAdmitted(uint64_t rf_packets, uint64_t header_repeats,
                                  uint64_t duration_us);
        void RecordUploadRetry();
        void RecordUploadDeferred();
        void RecordUploadFailure();
        void RecordDiscovery(uint64_t duration_us, bool target_seen,
                            uint64_t records, int64_t sighting_age_ms);
        void RecordDiscoveryDeferred();
        void RecordKeepAlive(int64_t gap_ms, uint64_t duration_us);
        void RecordKeepAliveFailure();
        void RecordUsbSend(uint64_t chunks, uint64_t duration_us);
        void RecordUsbRead(uint64_t duration_us);
        void RecordTransportRevalidation();
        void RecordForeignMaster();

        /* explicit trace control (tests); production reads
           OPENRGB_LIANLI_TRACE / OPENRGB_LIANLI_TRACE_MAX_ROWS */
        void OpenTrace(const std::string& path, uint64_t max_rows);
        void CloseTrace();
        bool TraceOpen() const;

        /* raw bounded trace row */
        void Trace(const char* event, long long first, long long second,
                   long long third);

        uint64_t TraceRows() const;

    private:
        Diagnostics();
        ~Diagnostics();
        Diagnostics(const Diagnostics&) = delete;
        Diagnostics& operator=(const Diagnostics&) = delete;

        struct TraceSink;
        void TraceLocked(const char* event, long long first, long long second,
                         long long third);
        void WriteSummaryLocked() const;

        mutable std::mutex   mutex;
        DiagCounters         counters;
        std::unique_ptr<TraceSink> sink;
    };
}
