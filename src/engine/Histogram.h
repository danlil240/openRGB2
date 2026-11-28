/*---------------------------------------------------------*\
|| Histogram.h                                               |
||                                                           |
||   Audit E-12c: a fixed-bucket timing histogram for the   |
||   engine's cadence and latency diagnostics — the scene   |
||   clock's tick interval and each delivery lane's         |
||   sample->send-start (queue) and send-start->completion  |
||   (send) times.                                          |
||                                                           |
||   Buckets are log-ish, in milliseconds, upper-inclusive: |
||   bucket i holds values in (kEdgesMs[i-1], kEdgesMs[i]]  |
||   (bucket 0 holds [0, 0.5]); the last bucket is the      |
||   overflow (> 250 ms). The edges straddle 16.7 ms and    |
||   33.3 ms on purpose so 60 Hz tick slip and the wireless |
||   pacing are visible.                                    |
||                                                           |
||   Percentile(p) reports the UPPER EDGE of the bucket the |
||   p-th sample falls in — honest about bucket resolution, |
||   never an interpolated precision the data does not      |
||   have. NaN = empty; +inf = the overflow bucket.         |
||                                                           |
||   Fixed size, no allocation: Record is a bounded linear  |
||   search over 16 edges. Not thread-safe — callers record |
||   under the lock that already guards their stats.        |
||                                                           |
||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace openrgb2
{

class Histogram
{
public:
    static constexpr size_t kFiniteEdges = 16;
    static constexpr size_t kBuckets     = kFiniteEdges + 1;
    static constexpr double kEdgesMs[kFiniteEdges] = {
        0.5, 1, 2, 4, 8, 12, 16, 17, 20, 25, 33, 40, 50, 75, 100, 250,
    };

    /* Upper edge of bucket i in ms; +inf for the overflow. */
    static double UpperEdgeMs(size_t i)
    {
        return i < kFiniteEdges
            ? kEdgesMs[i]
            : std::numeric_limits<double>::infinity();
    }

    /* The bucket a (non-negative) value lands in. */
    static size_t BucketIndex(double ms)
    {
        for(size_t i = 0; i < kFiniteEdges; ++i)
        {
            if(ms <= kEdgesMs[i])
            {
                return i;
            }
        }
        return kFiniteEdges;
    }

    /* Records one duration. A negative or NaN value is not a
       duration (an out-of-order stamp) — refused, returns
       false, nothing counted. */
    bool Record(double ms)
    {
        if(!(ms >= 0.0))
        {
            return false;
        }
        ++counts_[BucketIndex(ms)];
        ++total_;
        sum_ms_ += ms;
        return true;
    }

    void Reset()
    {
        counts_.fill(0);
        total_  = 0;
        sum_ms_ = 0.0;
    }

    uint64_t Total() const { return total_; }
    double   SumMs() const { return sum_ms_; }
    const std::array<uint64_t, kBuckets>& Counts() const
    {
        return counts_;
    }

    /* p in [0, 100]. The rank is ceil(p% of total), floored at
       1; the result is that sample's bucket upper edge. */
    double Percentile(double p) const
    {
        if(total_ == 0)
        {
            return std::numeric_limits<double>::quiet_NaN();
        }
        if(p < 0.0)   p = 0.0;
        if(p > 100.0) p = 100.0;
        /* p * total first: 91 * 100 / 100 is exactly 91, where
           0.91 * 100 rounds up past it. The epsilon absorbs any
           remaining representation error before the ceil. */
        uint64_t rank = (uint64_t)std::ceil(
            p * double(total_) / 100.0 - 1e-9);
        if(rank < 1)       rank = 1;
        if(rank > total_)  rank = total_;
        uint64_t cum = 0;
        for(size_t i = 0; i < kBuckets; ++i)
        {
            cum += counts_[i];
            if(cum >= rank)
            {
                return UpperEdgeMs(i);
            }
        }
        return UpperEdgeMs(kBuckets - 1);
    }

private:
    std::array<uint64_t, kBuckets> counts_{};
    uint64_t                       total_  = 0;
    double                         sum_ms_ = 0.0;
};

} /* namespace openrgb2 */
