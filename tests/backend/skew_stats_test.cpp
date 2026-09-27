/*---------------------------------------------------------*\
||| skew_stats_test.cpp                                       ||
|||                                                           ||
|||   Offline tests for the skew harness's pure aggregation ||
|||   layer (tools/bench/skew_stats.h): distribution        ||
|||   summaries and per-tick inter-controller skew           ||
|||   grouping. No sockets, no hardware.                     ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#include "skew_stats.h"

#include <cstdio>

using namespace openrgb_bench;

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, msg) do { checks++; if(!(cond)) { failures++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); fflush(stdout); } } while(0)

static SendRecord Rec(uint64_t seq, uint32_t lane,
                      int64_t call_us, int64_t dur_us)
{
    SendRecord r;
    r.seq     = seq;
    r.lane    = lane;
    r.call_us = call_us;
    r.done_us = call_us + dur_us;
    r.dur_us  = dur_us;
    r.status  = 0;
    return r;
}

int main()
{
    /*---- Summarize ------------------------------------------*/
    {
        const Distribution empty = Summarize({});
        CHECK(empty.n == 0 && empty.p50 == 0 && empty.max == 0,
              "empty distribution is all zero");
    }
    {
        const Distribution one = Summarize({42});
        CHECK(one.n == 1 && one.min == 42 && one.p50 == 42 &&
              one.p95 == 42 && one.p99 == 42 && one.max == 42,
              "single-sample distribution");
        CHECK(one.mean == 42.0, "single-sample mean");
    }
    {
        /* 1..100 unsorted */
        std::vector<int64_t> v;
        for(int64_t i = 100; i >= 1; --i)
        {
            v.push_back(i);
        }
        const Distribution d = Summarize(std::move(v));
        CHECK(d.n == 100 && d.min == 1 && d.max == 100,
              "min/max on 1..100");
        /* idx = pct/100 * (n-1): p50 -> idx 49 -> value 50 */
        CHECK(d.p50 == 50, "p50 of 1..100");
        CHECK(d.p95 == 95, "p95 of 1..100");
        CHECK(d.p99 == 99, "p99 of 1..100");
        CHECK(d.mean == 50.5, "mean of 1..100");
    }
    {
        /* negative offsets are legal (send landed before the
           recorded tick stamp, or a lag) */
        const Distribution d = Summarize({-50, -10, 0, 30, 200});
        CHECK(d.min == -50 && d.max == 200 && d.p50 == 0,
              "signed sample handling");
        CHECK(d.mean == 34.0, "signed mean");
    }

    /*---- PerTickSkew ----------------------------------------*/
    {
        const std::vector<TickSkew> none = PerTickSkew({});
        CHECK(none.empty(), "no records -> no ticks");
    }
    {
        /* tick 1: three lanes spread over 300 us at call, 400 at done;
           tick 2: two lanes (lane 2 dropped it); tick 3: one lane */
        const std::vector<SendRecord> recs = {
            Rec(1, 0, 1000, 100),
            Rec(1, 1, 1100, 150),
            Rec(1, 2, 1300, 200),
            Rec(2, 0, 2000, 100),
            Rec(2, 2, 2100, 100),
            Rec(3, 0, 3000, 100),
        };
        const std::vector<TickSkew> sk = PerTickSkew(recs);
        CHECK(sk.size() == 3, "three distinct seqs");
        CHECK(sk[0].seq == 1 && sk[0].lanes == 3,
              "tick 1 has all three lanes");
        CHECK(sk[0].call_spread_us == 300, "tick 1 call spread");
        /* done times: 1100, 1250, 1500 -> spread 400 */
        CHECK(sk[0].done_spread_us == 400, "tick 1 done spread");
        CHECK(sk[0].first_call_us == 1000 && sk[0].last_done_us == 1500,
              "tick 1 first/last timestamps");
        CHECK(sk[1].lanes == 2 && sk[1].call_spread_us == 100,
              "tick 2 partial coverage");
        CHECK(sk[2].lanes == 1 && sk[2].call_spread_us == 0 &&
              sk[2].done_spread_us == 0,
              "single-lane tick has zero spread");
    }
    {
        /* out-of-order arrival (lane threads finish in any order)
           and a much-later send of an old seq still groups by seq */
        const std::vector<SendRecord> recs = {
            Rec(5, 0, 9000, 100),      /* lane 0 sends seq 5 late   */
            Rec(6, 0, 9100, 100),
            Rec(6, 1, 9150, 100),
            Rec(5, 1, 9400, 100),      /* lane 1's seq 5 lands last */
        };
        const std::vector<TickSkew> sk = PerTickSkew(recs);
        CHECK(sk.size() == 2, "two seqs after regroup");
        CHECK(sk[0].seq == 5 && sk[0].call_spread_us == 400,
              "seq 5 spread counts the late send");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
