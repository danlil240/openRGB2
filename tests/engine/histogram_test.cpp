// SPDX-License-Identifier: GPL-2.0-or-later
//
// Audit E-12c — the fixed-bucket timing histogram
// (src/engine/Histogram.h): bucket boundaries (upper-inclusive),
// percentiles reported as bucket upper edges on known
// distributions, the overflow bucket, the empty histogram, and
// Reset. Qt-free.

#include "Histogram.h"

#include <cmath>
#include <cstdio>
#include <limits>

using namespace openrgb2;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

/* Index of the bucket whose upper edge is `edge_ms`. */
static size_t BucketOf(double edge_ms)
{
    for(size_t i = 0; i < Histogram::kFiniteEdges; ++i)
    {
        if(Histogram::kEdgesMs[i] == edge_ms)
        {
            return i;
        }
    }
    return Histogram::kBuckets - 1;
}

static void TestEdges()
{
    /* The plan's edge set: straddles 16.7 ms and 33.3 ms. */
    const double want[] = { 0.5, 1, 2, 4, 8, 12, 16, 17, 20, 25, 33,
                            40, 50, 75, 100, 250 };
    CHECK(Histogram::kFiniteEdges == 16);
    CHECK(Histogram::kBuckets == 17);
    for(size_t i = 0; i < Histogram::kFiniteEdges; ++i)
    {
        CHECK(Histogram::kEdgesMs[i] == want[i]);
    }
    CHECK(std::isinf(Histogram::UpperEdgeMs(Histogram::kBuckets - 1)));
    CHECK(Histogram::UpperEdgeMs(7) == 17.0);
}

static void TestBucketBoundaries()
{
    /* Upper-inclusive: a value equal to an edge lands in that
       edge's bucket; just above goes to the next one. */
    CHECK(Histogram::BucketIndex(0.0)   == BucketOf(0.5));
    CHECK(Histogram::BucketIndex(0.5)   == BucketOf(0.5));
    CHECK(Histogram::BucketIndex(0.51)  == BucketOf(1));
    CHECK(Histogram::BucketIndex(16.0)  == BucketOf(16));
    CHECK(Histogram::BucketIndex(16.7)  == BucketOf(17));
    CHECK(Histogram::BucketIndex(17.0)  == BucketOf(17));
    CHECK(Histogram::BucketIndex(31.0)  == BucketOf(33));
    CHECK(Histogram::BucketIndex(33.3)  == BucketOf(40));
    CHECK(Histogram::BucketIndex(250.0) == BucketOf(250));
    CHECK(Histogram::BucketIndex(250.1) == Histogram::kBuckets - 1);
    CHECK(Histogram::BucketIndex(1e9)   == Histogram::kBuckets - 1);

    Histogram h;
    CHECK(h.Record(16.7));
    CHECK(h.Record(31.0));
    CHECK(h.Counts()[BucketOf(17)] == 1);
    CHECK(h.Counts()[BucketOf(33)] == 1);
    CHECK(h.Total() == 2);
    CHECK(std::fabs(h.SumMs() - 47.7) < 1e-9);

    /* Not a duration: refused, nothing counted. */
    CHECK(!h.Record(-0.001));
    CHECK(!h.Record(std::numeric_limits<double>::quiet_NaN()));
    CHECK(h.Total() == 2);
}

static void TestEmpty()
{
    Histogram h;
    CHECK(h.Total() == 0);
    CHECK(h.SumMs() == 0.0);
    CHECK(std::isnan(h.Percentile(50)));
    CHECK(std::isnan(h.Percentile(99)));
    for(uint64_t c : h.Counts())
    {
        CHECK(c == 0);
    }
}

static void TestPercentiles()
{
    /* 100 samples: 90 at 16.5 ms (bucket 17), 9 at 30 ms
       (bucket 33), 1 at 300 ms (overflow). */
    Histogram h;
    for(int i = 0; i < 90; ++i) h.Record(16.5);
    for(int i = 0; i < 9; ++i)  h.Record(30.0);
    h.Record(300.0);
    CHECK(h.Total() == 100);
    CHECK(h.Percentile(50) == 17.0);   /* bucket upper edge */
    CHECK(h.Percentile(90) == 17.0);   /* rank 90 = last 16.5 */
    CHECK(h.Percentile(91) == 33.0);
    CHECK(h.Percentile(95) == 33.0);
    CHECK(h.Percentile(99) == 33.0);   /* rank 99 */
    CHECK(std::isinf(h.Percentile(100)));
    CHECK(h.Percentile(0) == 17.0);    /* rank floors at 1 */

    /* A single sample: every percentile is its bucket. */
    Histogram one;
    one.Record(3.0);
    CHECK(one.Percentile(1) == 4.0);
    CHECK(one.Percentile(50) == 4.0);
    CHECK(one.Percentile(99) == 4.0);

    /* Uniform 1..100 ms: p50 = rank 50 = 50 ms -> bucket 50. */
    Histogram u;
    for(int i = 1; i <= 100; ++i) u.Record(double(i));
    CHECK(u.Percentile(50) == 50.0);
    CHECK(u.Percentile(95) == 100.0);  /* rank 95 -> (75,100] */
    CHECK(u.Percentile(10) == 12.0);   /* rank 10 -> (8,12]   */
}

static void TestReset()
{
    Histogram h;
    h.Record(5.0);
    h.Record(500.0);
    h.Reset();
    CHECK(h.Total() == 0);
    CHECK(h.SumMs() == 0.0);
    CHECK(std::isnan(h.Percentile(50)));
    for(uint64_t c : h.Counts())
    {
        CHECK(c == 0);
    }
    h.Record(5.0);
    CHECK(h.Total() == 1);
    CHECK(h.Percentile(50) == 8.0);
}

int main()
{
    TestEdges();
    TestBucketBoundaries();
    TestEmpty();
    TestPercentiles();
    TestReset();
    std::printf("histogram: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
