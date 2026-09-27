/*---------------------------------------------------------*\
||| skew_stats.h                                              ||
|||                                                           ||
|||   Pure aggregation helpers for the skew-measurement     ||
|||   harness (skew_bench, Milestone 5 task 5.1). These     ||
|||   reduce the per-send records an instrumented           ||
|||   transport sink captures into the reportable           ||
|||   quantities:                                            ||
|||                                                           ||
|||     - per-controller send-duration distributions         ||
|||       (submit -> transport-return, i.e. "admitted",      ||
|||       never "displayed")                                 ||
|||     - per-tick inter-controller skew: the spread of      ||
|||       transport call/return times across the lanes       ||
|||       that delivered the SAME logical frame (same         ||
|||       producer sequence)                                 ||
|||                                                           ||
|||   Everything here is header-only and hardware-free —     ||
|||   the offline suite (tests/backend/skew_stats_test.cpp)  ||
|||   covers it without a socket.                            ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

namespace openrgb_bench
{
    /*---------------------------------------------------------*\
    | One transport send as observed by the instrumented sink.  |
    | All times are steady_clock microseconds relative to a    |
    | run-chosen origin so the values serialize directly.      |
    \*---------------------------------------------------------*/
    struct SendRecord
    {
        uint64_t seq;        /* producer tick id (frame.sequence)     */
        uint32_t lane;       /* producer-assigned lane index          */
        int64_t  call_us;    /* sink entry (transport call)           */
        int64_t  done_us;    /* sink exit  (transport return)         */
        int64_t  dur_us;     /* done_us - call_us                     */
        int      status;     /* SubmitStatus as int                   */
    };

    struct Distribution
    {
        uint64_t n    = 0;
        int64_t  min  = 0;
        int64_t  p50  = 0;
        int64_t  p95  = 0;
        int64_t  p99  = 0;
        int64_t  max  = 0;
        double   mean = 0.0;
    };

    inline int64_t PercentileOf(const std::vector<int64_t>& sorted,
                                double pct)
    {
        if(sorted.empty())
        {
            return 0;
        }
        const size_t idx =
            (size_t)(pct / 100.0 * (double)(sorted.size() - 1));
        return sorted[idx];
    }

    /* Sorts a copy — callers keep their record order. */
    inline Distribution Summarize(std::vector<int64_t> samples)
    {
        Distribution d;
        if(samples.empty())
        {
            return d;
        }
        std::sort(samples.begin(), samples.end());
        d.n   = samples.size();
        d.min = samples.front();
        d.max = samples.back();
        d.p50 = PercentileOf(samples, 50.0);
        d.p95 = PercentileOf(samples, 95.0);
        d.p99 = PercentileOf(samples, 99.0);
        long double sum = 0.0;
        for(int64_t v : samples)
        {
            sum += (long double)v;
        }
        d.mean = (double)(sum / (long double)samples.size());
        return d;
    }

    /*---------------------------------------------------------*\
    | One producer tick's cross-lane send spread. Records with  |
    | the same seq are frames the producer stamped with the    |
    | SAME logical timestamp; the spread of their transport    |
    | call/return times is the measurable inter-controller     |
    | skew for that tick. Lanes that dropped the tick           |
    | (latest-wins replacement) simply do not appear — see     |
    | `lanes` vs the total lane count for coverage.            |
    \*---------------------------------------------------------*/
    struct TickSkew
    {
        uint64_t seq            = 0;
        uint32_t lanes          = 0;  /* lanes that sent this seq      */
        int64_t  first_call_us  = 0;  /* min(call_us)                  */
        int64_t  last_done_us   = 0;  /* max(done_us)                  */
        int64_t  call_spread_us = 0;  /* max(call) - min(call)         */
        int64_t  done_spread_us = 0;  /* max(done) - min(done)         */
    };

    inline std::vector<TickSkew> PerTickSkew(
        const std::vector<SendRecord>& records)
    {
        struct Agg
        {
            uint32_t lanes   = 0;
            int64_t  min_call = 0;
            int64_t  max_call = 0;
            int64_t  min_done = 0;
            int64_t  max_done = 0;
        };
        std::map<uint64_t, Agg> by_seq;
        for(const SendRecord& r : records)
        {
            Agg& a = by_seq[r.seq];
            if(a.lanes == 0)
            {
                a.min_call = a.max_call = r.call_us;
                a.min_done = a.max_done = r.done_us;
            }
            else
            {
                a.min_call = std::min(a.min_call, r.call_us);
                a.max_call = std::max(a.max_call, r.call_us);
                a.min_done = std::min(a.min_done, r.done_us);
                a.max_done = std::max(a.max_done, r.done_us);
            }
            ++a.lanes;
        }
        std::vector<TickSkew> out;
        out.reserve(by_seq.size());
        for(const auto& entry : by_seq)
        {
            TickSkew t;
            t.seq            = entry.first;
            t.lanes          = entry.second.lanes;
            t.first_call_us  = entry.second.min_call;
            t.last_done_us   = entry.second.max_done;
            t.call_spread_us = entry.second.max_call - entry.second.min_call;
            t.done_spread_us = entry.second.max_done - entry.second.min_done;
            out.push_back(t);
        }
        return out;
    }
}
