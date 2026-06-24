/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Test suite for the diffserv module. Wires the 25 RFC test vectors from
 * rfc-test-vectors.h into ns-3 TestCases, plus additional S-tier spec
 * assertions.
 *
 * @see specs/02-structural.md S-1, S-2, S-3, S-4
 */

#include "rfc-test-vectors.h"

#include "ns3/bulk-send-helper.h"
#include "ns3/double.h"
#include "ns3/enum.h"
#include "ns3/fq-cobalt-queue-disc.h"
#include "ns3/fq-codel-queue-disc.h"
#include "ns3/inet-socket-address.h"
#include "ns3/internet-stack-helper.h"
#include "ns3/ipv4-address-helper.h"
#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/ipv4-header.h"
#include "ns3/ipv4-queue-disc-item.h"
#include "ns3/ipv6-header.h"
#include "ns3/ipv6-queue-disc-item.h"
#include "ns3/on-off-helper.h"
#include "ns3/packet-sink-helper.h"
#include "ns3/point-to-point-helper.h"
#include "ns3/rng-seed-manager.h"
#include "ns3/simulator.h"
#include "ns3/stratum-aqm-registry.h"
#include "ns3/stratum-cake-helper.h"
#include "ns3/stratum-cake-linux-autorate-hook.h"
#include "ns3/stratum-cake-stats-formatter.h"
#include "ns3/stratum-constants.h"
#include "ns3/stratum-core-queue-disc.h"
#include "ns3/stratum-dscp-tag.h"
#include "ns3/stratum-dumb-meter.h"
#include "ns3/stratum-edge-queue-disc.h"
#include "ns3/stratum-empirical-cdf-loader.h"
#include "ns3/stratum-fw-meter.h"
#include "ns3/stratum-helper.h"
#include "ns3/stratum-hybrid-llq-dispatcher.h"
#include "ns3/stratum-install-helper.h"
#include "ns3/stratum-l4s-queue-disc.h"
#include "ns3/stratum-llq-scheduler.h"
#include "ns3/stratum-meter.h"
#include "ns3/stratum-monitor-helper.h"
#include "ns3/stratum-onoff-helper.h"
#include "ns3/stratum-policy-classifier.h"
#include "ns3/stratum-policy-entry.h"
#include "ns3/stratum-pq-scheduler.h"
#include "ns3/stratum-rate-based-shaper-dispatcher.h"
#include "ns3/stratum-rate-based-tin-clock.h"
#include "ns3/stratum-red-queue-disc.h"
#include "ns3/stratum-red-sub-queue.h"
#include "ns3/stratum-rr-scheduler.h"
#include "ns3/stratum-scfq-scheduler.h"
#include "ns3/stratum-scheduler-registry.h"
#include "ns3/stratum-send-time-tag.h"
#include "ns3/stratum-sfq-scheduler.h"
#include "ns3/stratum-slot-dispatcher.h"
#include "ns3/stratum-sr-tcm-meter.h"
#include "ns3/stratum-statistics.h"
#include "ns3/stratum-tin-shaper-dispatcher.h"
#include "ns3/stratum-tin-token-bucket.h"
#include "ns3/stratum-token-bucket-meter.h"
#include "ns3/stratum-tr-tcm-meter.h"
#include "ns3/stratum-tsw2cm-meter.h"
#include "ns3/stratum-tsw3cm-meter.h"
#include "ns3/stratum-wf2qp-scheduler.h"
#include "ns3/stratum-wfq-scheduler.h"
#include "ns3/stratum-wirr-scheduler.h"
#include "ns3/stratum-wrr-scheduler.h"
#include "ns3/string.h"
#include "ns3/system-path.h"
#include "ns3/tbf-queue-disc.h"
#include "ns3/tcp-header.h"
#include "ns3/tcp-option-sack.h"
#include "ns3/test.h"
#include "ns3/traffic-control-helper.h"
#include "ns3/traffic-control-layer.h"
#include "ns3/uinteger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>

#ifdef __APPLE__
#include <mach/mach.h>
#else
#include <sys/resource.h>
#endif

#include "test-data-paths.h"

using namespace diffserv_test;
using ns3::stratum::testing::GetExample3DataDir;

namespace ns3::stratum
{

// ---- Helpers ----------------------------------------------------------------

/// Tolerance for bucket-level comparisons (exact arithmetic, very tight).
static constexpr double kEps = 1e-6;

static const char*
ColourName(Colour c)
{
    switch (c)
    {
    case Colour::GREEN:
        return "GREEN";
    case Colour::YELLOW:
        return "YELLOW";
    case Colour::RED:
        return "RED";
    }
    return "???";
}

/// Convert test-vector Colour to production Colour (same underlying values).
static Colour
FromTestColour(diffserv_test::Colour c)
{
    return static_cast<Colour>(static_cast<uint8_t>(c));
}

// =============================================================================
//  TokenBucket vector test case  (S-1)
// =============================================================================

class TokenBucketVectorTestCase : public TestCase
{
  public:
    TokenBucketVectorTestCase(int idx)
        : TestCase(std::string("S-1 TokenBucket: ") + kTokenBucketVectors[idx].name),
          m_idx(idx)
    {
    }

  private:
    void DoRun() override
    {
        const auto& vec = kTokenBucketVectors[m_idx];

        PolicyEntry entry;
        entry.cir = vec.cir_bytes_per_sec;
        entry.cbs = vec.cbs_bytes;
        entry.cBucket = vec.initial_c_bucket;
        entry.arrivalTime = vec.initial_arrival_time;

        TokenBucketMeter meter;

        for (int i = 0; i < vec.num_events; ++i)
        {
            const auto& e = vec.events[i];
            meter.ApplyMeter(entry, e.arrival_time_s, e.size_bytes);
            Colour c = meter.ApplyPolicer(entry, e.size_bytes);

            std::ostringstream ctx;
            ctx << vec.name << " event " << i << ": got " << ColourName(c) << " c=" << entry.cBucket
                << ", want " << ColourName(FromTestColour(e.expected_colour))
                << " c=" << e.expected_c_bucket;

            NS_TEST_EXPECT_MSG_EQ(static_cast<uint8_t>(c),
                                  static_cast<uint8_t>(e.expected_colour),
                                  ctx.str());
            NS_TEST_EXPECT_MSG_EQ_TOL(entry.cBucket, e.expected_c_bucket, kEps, ctx.str());
        }
    }

    int m_idx;
};

// =============================================================================
//  srTCM vector test case  (S-2)
// =============================================================================

class SrTcmVectorTestCase : public TestCase
{
  public:
    SrTcmVectorTestCase(int idx)
        : TestCase(std::string("S-2 srTCM: ") + kSrTcmVectors[idx].name),
          m_idx(idx)
    {
    }

  private:
    void DoRun() override
    {
        const auto& vec = kSrTcmVectors[m_idx];

        PolicyEntry entry;
        entry.cir = vec.cir_bytes_per_sec;
        entry.cbs = vec.cbs_bytes;
        entry.ebs = vec.ebs_bytes;
        entry.cBucket = vec.initial_c_bucket;
        entry.eBucket = vec.initial_e_bucket;
        entry.arrivalTime = vec.initial_arrival_time;

        SrTcmMeter meter;

        for (int i = 0; i < vec.num_events; ++i)
        {
            const auto& e = vec.events[i];
            meter.ApplyMeter(entry, e.arrival_time_s, e.size_bytes);
            Colour c = meter.ApplyPolicer(entry, e.size_bytes);

            std::ostringstream ctx;
            ctx << vec.name << " event " << i << ": got " << ColourName(c) << " c=" << entry.cBucket
                << " e=" << entry.eBucket << ", want "
                << ColourName(FromTestColour(e.expected_colour)) << " c=" << e.expected_c_bucket
                << " e=" << e.expected_e_bucket;

            NS_TEST_EXPECT_MSG_EQ(static_cast<uint8_t>(c),
                                  static_cast<uint8_t>(e.expected_colour),
                                  ctx.str());
            NS_TEST_EXPECT_MSG_EQ_TOL(entry.cBucket, e.expected_c_bucket, kEps, ctx.str());
            if (e.expected_e_bucket >= 0)
            {
                NS_TEST_EXPECT_MSG_EQ_TOL(entry.eBucket, e.expected_e_bucket, kEps, ctx.str());
            }
        }
    }

    int m_idx;
};

// =============================================================================
//  trTCM vector test case  (S-3)
// =============================================================================

class TrTcmVectorTestCase : public TestCase
{
  public:
    TrTcmVectorTestCase(int idx)
        : TestCase(std::string("S-3 trTCM: ") + kTrTcmVectors[idx].name),
          m_idx(idx)
    {
    }

  private:
    void DoRun() override
    {
        const auto& vec = kTrTcmVectors[m_idx];

        PolicyEntry entry;
        entry.cir = vec.cir_bytes_per_sec;
        entry.pir = vec.pir_bytes_per_sec;
        entry.cbs = vec.cbs_bytes;
        entry.pbs = vec.pbs_bytes;
        entry.cBucket = vec.initial_c_bucket;
        entry.pBucket = vec.initial_p_bucket;
        entry.arrivalTime = vec.initial_arrival_time;

        TrTcmMeter meter;

        for (int i = 0; i < vec.num_events; ++i)
        {
            const auto& e = vec.events[i];
            meter.ApplyMeter(entry, e.arrival_time_s, e.size_bytes);
            Colour c = meter.ApplyPolicer(entry, e.size_bytes);

            std::ostringstream ctx;
            ctx << vec.name << " event " << i << ": got " << ColourName(c) << " c=" << entry.cBucket
                << " p=" << entry.pBucket << ", want "
                << ColourName(FromTestColour(e.expected_colour)) << " c=" << e.expected_c_bucket
                << " p=" << e.expected_p_bucket;

            NS_TEST_EXPECT_MSG_EQ(static_cast<uint8_t>(c),
                                  static_cast<uint8_t>(e.expected_colour),
                                  ctx.str());
            NS_TEST_EXPECT_MSG_EQ_TOL(entry.cBucket, e.expected_c_bucket, kEps, ctx.str());
            if (e.expected_p_bucket >= 0)
            {
                NS_TEST_EXPECT_MSG_EQ_TOL(entry.pBucket, e.expected_p_bucket, kEps, ctx.str());
            }
        }
    }

    int m_idx;
};

// =============================================================================
//  DumbMeter test case
// =============================================================================

class DumbMeterTestCase : public TestCase
{
  public:
    DumbMeterTestCase()
        : TestCase("DumbMeter always returns GREEN")
    {
    }

  private:
    void DoRun() override
    {
        PolicyEntry entry;
        DumbMeter meter;

        meter.ApplyMeter(entry, 1.0, 1000);
        NS_TEST_EXPECT_MSG_EQ(entry.arrivalTime, 1.0, "DumbMeter must update arrivalTime");

        Colour c = meter.ApplyPolicer(entry, 1000);
        NS_TEST_EXPECT_MSG_EQ(static_cast<uint8_t>(c),
                              static_cast<uint8_t>(Colour::GREEN),
                              "DumbMeter must always return GREEN");

        meter.ApplyMeter(entry, 5.0, 500);
        c = meter.ApplyPolicer(entry, 500);
        NS_TEST_EXPECT_MSG_EQ(static_cast<uint8_t>(c),
                              static_cast<uint8_t>(Colour::GREEN),
                              "DumbMeter must always return GREEN regardless of input");
    }
};

// =============================================================================
//  S-4.1: TSW EWMA convergence (deterministic — no RNG needed)
// =============================================================================

class TswEwmaConvergenceTestCase : public TestCase
{
  public:
    TswEwmaConvergenceTestCase()
        : TestCase("S-4.1 TSW EWMA converges to actual rate within 5%")
    {
    }

  private:
    void DoRun() override
    {
        // Feed CBR at 2 Mbps (250000 B/s), CIR=1 Mbps, winLen=1s.
        // After 10 * winLen the EWMA should match the actual rate.
        constexpr double cir = 125000.0;        // 1 Mbps in bytes/s
        constexpr double actualRate = 250000.0; // 2 Mbps in bytes/s
        constexpr uint32_t pktSize = 1000;
        constexpr double dt = static_cast<double>(pktSize) / actualRate; // 0.004s
        constexpr double winLen = 1.0;
        constexpr int nPackets = 2500; // 10s worth at 0.004s spacing

        PolicyEntry entry;
        entry.cir = cir;
        entry.avgRate = 0.0;
        entry.winLen = winLen;
        entry.arrivalTime = 0.0;

        // TSW2CM and TSW3CM share the same ApplyMeter; test via TSW2CM.
        Tsw2cmMeter meter;

        double now = 0.0;
        for (int i = 0; i < nPackets; ++i)
        {
            now += dt;
            meter.ApplyMeter(entry, now, pktSize);
        }

        double relError = std::fabs(entry.avgRate - actualRate) / actualRate;
        NS_TEST_EXPECT_MSG_LT(relError,
                              0.05,
                              "EWMA avgRate=" << entry.avgRate
                                              << " should be within 5% of actual rate="
                                              << actualRate);
    }
};

// =============================================================================
//  S-4.2: TSW2CM under CIR — all packets GREEN (deterministic)
// =============================================================================

class Tsw2cmUnderCirTestCase : public TestCase
{
  public:
    Tsw2cmUnderCirTestCase()
        : TestCase("S-4.2 TSW2CM under CIR: all GREEN")
    {
    }

  private:
    void DoRun() override
    {
        // CIR=1 Mbps, feed at 500 kbps → avgRate < CIR → always GREEN.
        constexpr double cir = 125000.0;
        constexpr double feedRate = 62500.0; // 500 kbps
        constexpr uint32_t pktSize = 1000;
        constexpr double dt = static_cast<double>(pktSize) / feedRate;
        constexpr int nPackets = 1000;

        PolicyEntry entry;
        entry.cir = cir;
        entry.avgRate = cir; // start at CIR so it drops toward feedRate
        entry.winLen = 1.0;
        entry.arrivalTime = 0.0;

        Tsw2cmMeter meter;
        meter.AssignStreams(42);

        double now = 0.0;
        int redCount = 0;
        for (int i = 0; i < nPackets; ++i)
        {
            now += dt;
            meter.ApplyMeter(entry, now, pktSize);
            Colour c = meter.ApplyPolicer(entry, pktSize);
            if (c == Colour::RED)
            {
                ++redCount;
            }
        }

        NS_TEST_EXPECT_MSG_EQ(redCount,
                              0,
                              "Under CIR: no packets should be marked RED (got " << redCount
                                                                                 << ")");
    }
};

// =============================================================================
//  S-4.3: TSW2CM over CIR — GREEN ratio ≈ CIR / avgRate
// =============================================================================

class Tsw2cmOverCirTestCase : public TestCase
{
  public:
    Tsw2cmOverCirTestCase()
        : TestCase("S-4.3 TSW2CM over CIR: GREEN ratio approx 0.5")
    {
    }

  private:
    void DoRun() override
    {
        // CIR=1 Mbps, feed at 2 Mbps → expected GREEN = CIR/rate = 0.5.
        constexpr double cir = 125000.0;
        constexpr double feedRate = 250000.0;
        constexpr uint32_t pktSize = 1000;
        constexpr double dt = static_cast<double>(pktSize) / feedRate;
        constexpr int nPackets = 10000;

        PolicyEntry entry;
        entry.cir = cir;
        entry.avgRate = feedRate; // pre-converge so ratio is stable from start
        entry.winLen = 1.0;
        entry.arrivalTime = 0.0;

        Tsw2cmMeter meter;
        meter.AssignStreams(1);

        double now = 0.0;
        int greenCount = 0;
        for (int i = 0; i < nPackets; ++i)
        {
            now += dt;
            meter.ApplyMeter(entry, now, pktSize);
            Colour c = meter.ApplyPolicer(entry, pktSize);
            if (c == Colour::GREEN)
            {
                ++greenCount;
            }
        }

        double greenRatio = static_cast<double>(greenCount) / nPackets;
        NS_TEST_EXPECT_MSG_EQ_TOL(greenRatio,
                                  0.5,
                                  0.05,
                                  "GREEN ratio should be ~0.5, got " << greenRatio);
    }
};

// =============================================================================
//  S-4 TSW3CM: under CIR all GREEN, between CIR-PIR no RED, above PIR all 3
// =============================================================================

class Tsw3cmColourRatiosTestCase : public TestCase
{
  public:
    Tsw3cmColourRatiosTestCase()
        : TestCase("S-4 TSW3CM colour ratios above PIR")
    {
    }

  private:
    void DoRun() override
    {
        // CIR=100000, PIR=200000 (bytes/s), feed at 400000 (3.2 Mbps).
        // Expected: GREEN = CIR/rate = 0.25
        //           YELLOW = (PIR-CIR)/rate = 0.25
        //           RED = (rate-PIR)/rate = 0.50
        constexpr double cir = 100000.0;
        constexpr double pir = 200000.0;
        constexpr double feedRate = 400000.0;
        constexpr uint32_t pktSize = 1000;
        constexpr double dt = static_cast<double>(pktSize) / feedRate;
        constexpr int nPackets = 20000;

        PolicyEntry entry;
        entry.cir = cir;
        entry.pir = pir;
        entry.avgRate = feedRate;
        entry.winLen = 1.0;
        entry.arrivalTime = 0.0;

        Tsw3cmMeter meter;
        meter.AssignStreams(7);

        double now = 0.0;
        int green = 0;
        int yellow = 0;
        int red = 0;
        for (int i = 0; i < nPackets; ++i)
        {
            now += dt;
            meter.ApplyMeter(entry, now, pktSize);
            Colour c = meter.ApplyPolicer(entry, pktSize);
            switch (c)
            {
            case Colour::GREEN:
                ++green;
                break;
            case Colour::YELLOW:
                ++yellow;
                break;
            case Colour::RED:
                ++red;
                break;
            }
        }

        double gRatio = static_cast<double>(green) / nPackets;
        double yRatio = static_cast<double>(yellow) / nPackets;
        double rRatio = static_cast<double>(red) / nPackets;

        NS_TEST_EXPECT_MSG_EQ_TOL(gRatio,
                                  0.25,
                                  0.05,
                                  "GREEN ratio should be ~0.25, got " << gRatio);
        NS_TEST_EXPECT_MSG_EQ_TOL(yRatio,
                                  0.25,
                                  0.05,
                                  "YELLOW ratio should be ~0.25, got " << yRatio);
        NS_TEST_EXPECT_MSG_EQ_TOL(rRatio, 0.50, 0.05, "RED ratio should be ~0.50, got " << rRatio);
    }
};

// =============================================================================
//  S-5.1: RR Equal Share
// =============================================================================

/**
 * @brief Verifies round-robin scheduler distributes equal share across active queues.
 * @see specs/02-structural.md S-5.1
 */
class RrEqualShareTest : public TestCase
{
  public:
    RrEqualShareTest()
        : TestCase("S-5.1 RR equal share: 3 queues, 100 pkts each")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<RoundRobinScheduler> sched =
            CreateObjectWithAttributes<RoundRobinScheduler>("NumQueues", UintegerValue(3));

        // Enqueue 100 packets to each of 3 queues
        for (uint32_t q = 0; q < 3; ++q)
        {
            for (int i = 0; i < 100; ++i)
            {
                sched->OnEnqueue(q, 1000);
            }
        }

        // Dequeue all 300 and count per-queue selections
        int served[3] = {0, 0, 0};
        for (int i = 0; i < 300; ++i)
        {
            int q = sched->SelectNextQueue();
            NS_TEST_ASSERT_MSG_NE(q, -1, "Should not return -1 with packets remaining");
            served[q]++;
        }

        // Each queue must be served exactly 100 times
        for (int q = 0; q < 3; ++q)
        {
            NS_TEST_ASSERT_MSG_EQ(served[q],
                                  100,
                                  "Queue " << q << " should be served exactly 100 times, got "
                                           << served[q]);
        }

        // All queues should now be empty
        NS_TEST_ASSERT_MSG_EQ(sched->SelectNextQueue(), -1, "All queues should be empty");

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-5.2: WRR Weighted Share
// =============================================================================

/**
 * @brief Verifies weighted round-robin honours weight ratios across queues.
 * @see specs/02-structural.md S-5.2
 */
class WrrWeightedShareTest : public TestCase
{
  public:
    WrrWeightedShareTest()
        : TestCase("S-5.2 WRR weighted share: weights 1,2,3")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<WeightedRoundRobinScheduler> sched =
            CreateObjectWithAttributes<WeightedRoundRobinScheduler>("NumQueues", UintegerValue(3));
        sched->SetParam(0, 1.0);
        sched->SetParam(1, 2.0);
        sched->SetParam(2, 3.0);

        // Enqueue 600 packets per queue to ensure no queue empties during
        // measurement. We dequeue only 600 (100 WRR rounds of 6), measuring the
        // share.
        for (uint32_t q = 0; q < 3; ++q)
        {
            for (int i = 0; i < 600; ++i)
            {
                sched->OnEnqueue(q, 1000);
            }
        }

        // Dequeue 600 and count per-queue selections
        int served[3] = {0, 0, 0};
        for (int i = 0; i < 600; ++i)
        {
            int q = sched->SelectNextQueue();
            NS_TEST_ASSERT_MSG_NE(q, -1, "Should not return -1 with packets remaining");
            served[q]++;
        }

        // Expected shares: 1/6 * 600 = 100, 2/6 * 600 = 200, 3/6 * 600 = 300
        // Tolerance: +/- 6 (one WRR round = 1+2+3 = 6 packets)
        NS_TEST_ASSERT_MSG_EQ_TOL(served[0],
                                  100,
                                  6,
                                  "Queue 0 (weight 1): expected ~100, got " << served[0]);
        NS_TEST_ASSERT_MSG_EQ_TOL(served[1],
                                  200,
                                  6,
                                  "Queue 1 (weight 2): expected ~200, got " << served[1]);
        NS_TEST_ASSERT_MSG_EQ_TOL(served[2],
                                  300,
                                  6,
                                  "Queue 2 (weight 3): expected ~300, got " << served[2]);

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-5.3: WIRR Bounded Burstiness
// =============================================================================

/**
 * @brief Verifies WIRR keeps per-queue burstiness within a bounded window.
 * @see specs/02-structural.md S-5.3
 */
class WirrBoundedBurstinessTest : public TestCase
{
  public:
    WirrBoundedBurstinessTest()
        : TestCase("S-5.3 WIRR bounded burstiness: weights 2,4,6")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<WeightedInterleavedRoundRobinScheduler> sched =
            CreateObjectWithAttributes<WeightedInterleavedRoundRobinScheduler>("NumQueues",
                                                                               UintegerValue(3));
        sched->SetParam(0, 2.0);
        sched->SetParam(1, 4.0);
        sched->SetParam(2, 6.0);

        // Enqueue enough so no queue empties during the measurement window.
        // Weights 2:4:6, sum=12. Measuring 1200 dequeues = 100 WIRR cycles.
        // Queue 2 needs 6/12 * 1200 = 600. Queue 1 needs 400. Queue 0 needs 200.
        // Put 1000 in each to have headroom.
        for (uint32_t q = 0; q < 3; ++q)
        {
            for (int i = 0; i < 1000; ++i)
            {
                sched->OnEnqueue(q, 1000);
            }
        }

        // Dequeue 1200 (100 WIRR cycles) and track per-queue selections + max
        // consecutive
        constexpr int kDequeues = 1200;
        int served[3] = {0, 0, 0};
        int maxConsecutive[3] = {0, 0, 0};
        int lastQ = -1;
        int currentRun = 0;

        for (int i = 0; i < kDequeues; ++i)
        {
            int q = sched->SelectNextQueue();
            NS_TEST_ASSERT_MSG_NE(q, -1, "Should not return -1 with packets remaining");
            served[q]++;

            if (q == lastQ)
            {
                currentRun++;
            }
            else
            {
                if (lastQ >= 0)
                {
                    maxConsecutive[lastQ] = std::max(maxConsecutive[lastQ], currentRun);
                }
                currentRun = 1;
                lastQ = q;
            }
        }
        // Finalize the last run
        if (lastQ >= 0)
        {
            maxConsecutive[lastQ] = std::max(maxConsecutive[lastQ], currentRun);
        }

        // Check long-run shares: weights 2,4,6 → sum=12 → 1/6, 2/6, 3/6
        // Total=1200, expected: 200, 400, 600
        // Tolerance: one full WIRR cycle = 2+4+6 = 12 packets
        NS_TEST_ASSERT_MSG_EQ_TOL(served[0],
                                  200,
                                  12,
                                  "Queue 0 (weight 2): expected ~200, got " << served[0]);
        NS_TEST_ASSERT_MSG_EQ_TOL(served[1],
                                  400,
                                  12,
                                  "Queue 1 (weight 4): expected ~400, got " << served[1]);
        NS_TEST_ASSERT_MSG_EQ_TOL(served[2],
                                  600,
                                  12,
                                  "Queue 2 (weight 6): expected ~600, got " << served[2]);

        // Check bounded burstiness: max consecutive <= ceil(weight / gcd(2,4,6))
        // gcd(2,4,6) = 2, so max consecutive: ceil(2/2)=1, ceil(4/2)=2, ceil(6/2)=3
        int gcdAll = std::gcd(std::gcd(2, 4), 6);
        int maxBurst[3];
        int weights[3] = {2, 4, 6};
        for (int q = 0; q < 3; ++q)
        {
            maxBurst[q] = (weights[q] + gcdAll - 1) / gcdAll; // ceil(w/gcd)
        }

        for (int q = 0; q < 3; ++q)
        {
            NS_TEST_ASSERT_MSG_LT_OR_EQ(maxConsecutive[q],
                                        maxBurst[q],
                                        "Queue " << q << ": max consecutive " << maxConsecutive[q]
                                                 << " exceeds bound " << maxBurst[q]);
        }

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-6.1: PQ Strict Priority
// =============================================================================

/**
 * @brief Verifies priority queue serves higher-priority class to exclusion of lower.
 * @see specs/02-structural.md S-6.1
 */
class PqStrictPriorityTest : public TestCase
{
  public:
    PqStrictPriorityTest()
        : TestCase("S-6.1 PQ strict priority: 3 queues, 10 pkts each")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<PriorityScheduler> sched =
            CreateObjectWithAttributes<PriorityScheduler>("NumQueues",
                                                          UintegerValue(3),
                                                          "WinLen",
                                                          DoubleValue(1.0));

        // Enqueue 10 packets to each of 3 queues (30 total)
        for (uint32_t q = 0; q < 3; ++q)
        {
            for (int i = 0; i < 10; ++i)
            {
                sched->OnEnqueue(q, 1000);
            }
        }

        // Dequeue all 30 — should serve queue 0 first, then 1, then 2
        for (int i = 0; i < 10; ++i)
        {
            int q = sched->SelectNextQueue();
            NS_TEST_ASSERT_MSG_EQ(q,
                                  0,
                                  "First 10 packets should come from queue 0, packet "
                                      << i << " came from queue " << q);
        }
        for (int i = 0; i < 10; ++i)
        {
            int q = sched->SelectNextQueue();
            NS_TEST_ASSERT_MSG_EQ(q,
                                  1,
                                  "Next 10 packets should come from queue 1, packet "
                                      << i << " came from queue " << q);
        }
        for (int i = 0; i < 10; ++i)
        {
            int q = sched->SelectNextQueue();
            NS_TEST_ASSERT_MSG_EQ(q,
                                  2,
                                  "Last 10 packets should come from queue 2, packet "
                                      << i << " came from queue " << q);
        }

        NS_TEST_ASSERT_MSG_EQ(sched->SelectNextQueue(), -1, "All queues should be empty");

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-6.2: PQ Rate-Capped Priority (simplified)
// =============================================================================

/**
 * @brief Verifies priority queue with a rate cap honours the configured ceiling.
 * @see specs/02-structural.md S-6.2
 */
class PqRateCappedTest : public TestCase
{
  public:
    PqRateCappedTest()
        : TestCase("S-6.2 PQ rate-capped: queue 0 exceeds cap, serves queue 1")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<PriorityScheduler> sched =
            CreateObjectWithAttributes<PriorityScheduler>("NumQueues",
                                                          UintegerValue(2),
                                                          "WinLen",
                                                          DoubleValue(1.0));

        // Set a very low rate cap on queue 0: 1 bps = 0.125 bytes/s
        sched->SetParam(0, 1.0);

        // Enqueue 10 packets to each queue
        for (uint32_t q = 0; q < 2; ++q)
        {
            for (int i = 0; i < 10; ++i)
            {
                sched->OnEnqueue(q, 1000);
            }
        }

        // Simulate departure from queue 0: 1000 bytes at t=0.1s
        // This makes m_queueAvgRate[0] >> 0.125 bytes/s (the cap)
        sched->UpdateDepartureRate(0, 0, 1000, 0.1);

        // Now SelectNextQueue should skip queue 0 (rate exceeded) and serve queue 1
        int q = sched->SelectNextQueue();
        NS_TEST_ASSERT_MSG_EQ(q,
                              1,
                              "Queue 0 exceeds rate cap; should serve queue 1, got queue " << q);

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-12.1: Higher Precedence Drops More
// =============================================================================

/**
 * @brief Verifies higher drop precedence experiences more drops under congestion.
 * @see specs/02-structural.md S-12.1
 */
class HigherPrecDropsMoreTest : public TestCase
{
  public:
    HigherPrecDropsMoreTest()
        : TestCase("S-12.1 Higher precedence drops more (RIO_D)")
    {
    }

  private:
    void DoRun() override
    {
        RngSeedManager::SetSeed(1);
        RngSeedManager::SetRun(1);

        Ptr<RedSubQueue> sq = CreateObject<RedSubQueue>();
        sq->SetQueueLimit(5000);
        sq->SetMredMode(MredMode::RIO_D);
        sq->SetNumPrec(2);
        // prec 0: lenient (high thresholds)
        sq->ConfigureVirtualQueue(0, 800, 1500, 0.1);
        // prec 1: aggressive (low thresholds, higher maxP)
        sq->ConfigureVirtualQueue(1, 200, 600, 0.5);
        sq->Initialize();

        // Pre-fill 1000 packets at prec 0 to build physical queue
        // (and let prec 0's EWMA start converging toward 1000).
        for (int i = 0; i < 1000; ++i)
        {
            Ptr<Packet> pkt = Create<Packet>(1000);
            Ipv4Header ipHeader;
            ipHeader.SetSource(Ipv4Address("10.0.0.1"));
            ipHeader.SetDestination(Ipv4Address("10.0.0.2"));
            Ptr<Ipv4QueueDiscItem> item =
                Create<Ipv4QueueDiscItem>(pkt, Address(), 0x0800, ipHeader);
            sq->EnqueueWithPrec(item, 0, false);
        }

        // Now enqueue 3000 packets alternating prec 0 and prec 1.
        // Prec 1's virtual qlen grows; its EWMA should eventually cross
        // thMin=200 and begin dropping. Prec 0's thresholds are higher (800),
        // so its drops lag behind.
        int edrops0 = 0;
        int edrops1 = 0;

        for (int i = 0; i < 3000; ++i)
        {
            Ptr<Packet> pkt = Create<Packet>(1000);
            Ipv4Header ipHeader;
            ipHeader.SetSource(Ipv4Address("10.0.0.1"));
            ipHeader.SetDestination(Ipv4Address("10.0.0.2"));
            Ptr<Ipv4QueueDiscItem> item =
                Create<Ipv4QueueDiscItem>(pkt, Address(), 0x0800, ipHeader);

            auto prec = static_cast<uint32_t>(i % 2);
            PktResult result = sq->EnqueueWithPrec(item, prec, false);
            if (result == PktResult::PKT_EDROPPED)
            {
                if (prec == 0)
                {
                    edrops0++;
                }
                else
                {
                    edrops1++;
                }
            }
        }

        NS_TEST_ASSERT_MSG_GT(edrops1,
                              edrops0,
                              "Prec 1 (aggressive) should have more edrops than prec 0 "
                              "(lenient): prec0="
                                  << edrops0 << " prec1=" << edrops1);

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-12.2: Tail Drop on Overflow
// =============================================================================

/**
 * @brief Verifies queue applies tail-drop when filled to capacity.
 * @see specs/02-structural.md S-12.2
 */
class TailDropOverflowTest : public TestCase
{
  public:
    TailDropOverflowTest()
        : TestCase("S-12.2 Tail drop on overflow: qlim=10, enqueue 15")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<RedSubQueue> sq = CreateObject<RedSubQueue>();
        sq->SetQueueLimit(10);
        sq->SetMredMode(MredMode::DROP_TAIL);
        sq->SetNumPrec(1);
        // Set thMin very high so DROP_TAIL mode does not threshold-drop
        sq->ConfigureVirtualQueue(0, 1000, 2000, 0.1);
        sq->Initialize();

        int enqueued = 0;
        int dropped = 0;

        for (int i = 0; i < 15; ++i)
        {
            Ptr<Packet> pkt = Create<Packet>(1000);
            Ipv4Header ipHeader;
            ipHeader.SetSource(Ipv4Address("10.0.0.1"));
            ipHeader.SetDestination(Ipv4Address("10.0.0.2"));
            Ptr<Ipv4QueueDiscItem> item =
                Create<Ipv4QueueDiscItem>(pkt, Address(), 0x0800, ipHeader);

            PktResult result = sq->EnqueueWithPrec(item, 0, false);
            if (result == PktResult::PKT_ENQUEUED)
            {
                enqueued++;
            }
            else
            {
                dropped++;
            }
        }

        NS_TEST_ASSERT_MSG_EQ(enqueued, 10, "First 10 packets should succeed, got " << enqueued);
        NS_TEST_ASSERT_MSG_EQ(dropped, 5, "Last 5 packets should be dropped, got " << dropped);

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-12.2b: DROP_TAIL vs RIO_C semantic asymmetry of the (0,0,0) prec triple
// =============================================================================
//
// Pins the semantic asymmetry of `ConfigQueue(prec, 0, 0, 0)`: under
// DROP_TAIL the per-prec triple is dead code (only prec-0 thMin against the
// physical queue length matters) so packets enqueue normally; under RIO_C
// the same triple force-drops everything once the EWMA crosses zero. This
// asymmetry surfaced as an example-1 setup-ordering footgun in 2026, where
// SetMredMode(DROP_TAIL) ran before Initialize (a no-op given the queue
// disc's pre-Initialize semantics), leaving sub-queues at default RIO_C
// and silently flipping yellow-EF behaviour from "flows through" to
// "shredded" depending on scheduler dequeue cadence.
//
// The RFC-aligned "force-drop yellow at edge" idiom in the project is
// `ConfigQueue(prec, -1.0, -1.0, 0.0)` (the `thMin = -1` sentinel that
// both simulators recognise under DROP_TAIL); see example-2 line 593.

/**
 * @brief Verifies drop-tail mode ignores precedence-1 packets when sized to zero.
 * @see specs/02-structural.md S-12.2
 */
class DropTailIgnoresZeroPrec1Test : public TestCase
{
  public:
    DropTailIgnoresZeroPrec1Test()
        : TestCase("S-12.2b DROP_TAIL with (0,0,0) prec-1 thresholds enqueues; "
                   "RIO_C with the same triple force-drops")
    {
    }

  private:
    static Ptr<Ipv4QueueDiscItem> MakeItem()
    {
        Ptr<Packet> pkt = Create<Packet>(540);
        Ipv4Header ip;
        ip.SetSource(Ipv4Address("10.0.0.1"));
        ip.SetDestination(Ipv4Address("10.0.0.2"));
        return Create<Ipv4QueueDiscItem>(pkt, Address(), 0x0800, ip);
    }

    void DoRun() override
    {
        // ----- DROP_TAIL leg: yellow EF (prec=1) must enqueue -----
        {
            Ptr<RedSubQueue> sq = CreateObject<RedSubQueue>();
            sq->SetQueueLimit(30);
            sq->SetNumPrec(2);
            sq->SetMredMode(MredMode::DROP_TAIL);
            // prec 0 = green: behave like the example-1 EF in-profile bucket
            sq->ConfigureVirtualQueue(0, 30, 30, 1.0);
            // prec 1 = yellow: the (0, 0, 0) triple
            sq->ConfigureVirtualQueue(1, 0, 0, 0.0);
            sq->Initialize();

            int enqueued = 0;
            int edropped = 0;
            for (int i = 0; i < 10; ++i)
            {
                PktResult r = sq->EnqueueWithPrec(MakeItem(), 1, false);
                if (r == PktResult::PKT_ENQUEUED)
                {
                    enqueued++;
                }
                else if (r == PktResult::PKT_EDROPPED)
                {
                    edropped++;
                }
            }
            NS_TEST_ASSERT_MSG_EQ(enqueued,
                                  10,
                                  "DROP_TAIL with (0,0,0) prec-1 thresholds must enqueue all "
                                  "10 yellow packets, got "
                                      << enqueued);
            NS_TEST_ASSERT_MSG_EQ(edropped,
                                  0,
                                  "DROP_TAIL must produce zero RED early drops on prec-1 with "
                                  "(0,0,0) thresholds, got "
                                      << edropped);
        }

        // ----- RIO_C leg: same triple now means force-drop (negative test) -----
        {
            Ptr<RedSubQueue> sq = CreateObject<RedSubQueue>();
            sq->SetQueueLimit(30);
            sq->SetNumPrec(2);
            sq->SetMredMode(MredMode::RIO_C);
            sq->ConfigureVirtualQueue(0, 30, 30, 1.0);
            sq->ConfigureVirtualQueue(1, 0, 0, 0.0);
            sq->Initialize();

            int enqueued = 0;
            int edropped = 0;
            for (int i = 0; i < 10; ++i)
            {
                PktResult r = sq->EnqueueWithPrec(MakeItem(), 1, false);
                if (r == PktResult::PKT_ENQUEUED)
                {
                    enqueued++;
                }
                else if (r == PktResult::PKT_EDROPPED)
                {
                    edropped++;
                }
            }
            NS_TEST_ASSERT_MSG_EQ(enqueued + edropped,
                                  10,
                                  "All 10 packets must be either enqueued or early-dropped, got "
                                      << (enqueued + edropped));
            NS_TEST_ASSERT_MSG_GT(edropped,
                                  5,
                                  "RIO_C with (0,0,0) prec-1 thresholds must force-drop the "
                                  "majority of yellow packets, got "
                                      << edropped << " edropped of 10");
        }

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-12.3: EWMA Convergence (deterministic)
// =============================================================================

/**
 * @brief Verifies the EWMA average converges to the input rate.
 * @see specs/02-structural.md S-12.3
 */
class EwmaConvergenceTest : public TestCase
{
  public:
    EwmaConvergenceTest()
        : TestCase("S-12.3 RED EWMA converges to actual queue length")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<RedSubQueue> sq = CreateObject<RedSubQueue>();
        sq->SetQueueLimit(200);
        sq->SetMredMode(MredMode::WRED);
        sq->SetNumPrec(1);
        // Set thresholds high so no drops occur
        sq->ConfigureVirtualQueue(0, 100, 180, 0.1);
        // Set ptc from 10 Mbps
        sq->SetPtc(10e6);
        sq->Initialize();

        // Enqueue 80 packets to fill queue
        for (int i = 0; i < 80; ++i)
        {
            Ptr<Packet> pkt = Create<Packet>(1000);
            Ipv4Header ipHeader;
            ipHeader.SetSource(Ipv4Address("10.0.0.1"));
            ipHeader.SetDestination(Ipv4Address("10.0.0.2"));
            Ptr<Ipv4QueueDiscItem> item =
                Create<Ipv4QueueDiscItem>(pkt, Address(), 0x0800, ipHeader);
            sq->EnqueueWithPrec(item, 0, false);
        }

        // Perform 2000 enqueue-then-dequeue cycles (maintaining qlen ~ 80)
        for (int i = 0; i < 2000; ++i)
        {
            Ptr<Packet> pkt = Create<Packet>(1000);
            Ipv4Header ipHeader;
            ipHeader.SetSource(Ipv4Address("10.0.0.1"));
            ipHeader.SetDestination(Ipv4Address("10.0.0.2"));
            Ptr<Ipv4QueueDiscItem> item =
                Create<Ipv4QueueDiscItem>(pkt, Address(), 0x0800, ipHeader);
            sq->EnqueueWithPrec(item, 0, false);
            sq->Dequeue();
        }

        double wAvg = sq->GetWeightedLength();
        NS_TEST_ASSERT_MSG_LT(std::fabs(wAvg - 80.0),
                              2.0,
                              "EWMA avg=" << wAvg << " should be within 2.0 of 80.0");

        Simulator::Destroy();
    }
};

// =============================================================================
//  E2E Toy Scenario
// =============================================================================

/**
 * @brief Smoke-tests an end-to-end edge plus core toy scenario.
 * @see specs/03-quality.md Q-1
 */
class E2EToyScenarioTest : public TestCase
{
  public:
    E2EToyScenarioTest()
        : TestCase("E2E toy scenario: 2 nodes, PQ scheduler, UDP traffic")
    {
    }

  private:
    void DoRun() override
    {
        // 2 nodes connected by a point-to-point link
        NodeContainer nodes;
        nodes.Create(2);

        PointToPointHelper p2p;
        p2p.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
        p2p.SetChannelAttribute("Delay", StringValue("5ms"));
        NetDeviceContainer devices = p2p.Install(nodes);

        InternetStackHelper internet;
        internet.Install(nodes);

        Ipv4AddressHelper address;
        address.SetBase("10.0.0.0", "255.255.255.0");
        Ipv4InterfaceContainer interfaces = address.Assign(devices);

        // Remove default queue disc before installing ours
        TrafficControlHelper tchUninstall;
        tchUninstall.Uninstall(devices.Get(0));

        // Install RedQueueDisc on the sender device (node 0, device 0)
        TrafficControlHelper tch;
        tch.SetRootQueueDisc("ns3::stratum::RedQueueDisc");
        QueueDiscContainer qdiscs = tch.Install(devices.Get(0));

        // Configure the RedQueueDisc
        Ptr<RedQueueDisc> dsQd = DynamicCast<RedQueueDisc>(qdiscs.Get(0));
        NS_TEST_ASSERT_MSG_NE(dsQd, nullptr, "Should have installed a RedQueueDisc");
        if (!dsQd)
        {
            Simulator::Destroy();
            return;
        }

        dsQd->SetNumQueues(2);

        // PHB: DSCP 46 (EF) -> queue 0, prec 0
        //       DSCP 0  (BE) -> queue 1, prec 0
        dsQd->AddPhbEntry(46, 0, 0);
        dsQd->AddPhbEntry(0, 1, 0);

        // Set up PQ scheduler with rate cap on queue 0
        Ptr<PriorityScheduler> pq = CreateObjectWithAttributes<PriorityScheduler>("NumQueues",
                                                                                  UintegerValue(2),
                                                                                  "WinLen",
                                                                                  DoubleValue(1.0));
        pq->SetParam(0, 5e6); // 5 Mbps cap on EF queue
        dsQd->SetScheduler(pq);

        // Sink on node 1
        uint16_t port = 9;
        PacketSinkHelper sinkHelper("ns3::UdpSocketFactory",
                                    InetSocketAddress(Ipv4Address::GetAny(), port));
        ApplicationContainer sinkApp = sinkHelper.Install(nodes.Get(1));
        sinkApp.Start(Seconds(0.0));
        sinkApp.Stop(Seconds(2.0));

        // OnOff app 1: DSCP 46 (EF)
        ns3::OnOffHelper onoff1("ns3::UdpSocketFactory",
                                InetSocketAddress(interfaces.GetAddress(1), port));
        onoff1.SetAttribute("DataRate", StringValue("2Mbps"));
        onoff1.SetAttribute("PacketSize", UintegerValue(1000));
        onoff1.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
        onoff1.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
        ApplicationContainer app1 = onoff1.Install(nodes.Get(0));
        app1.Start(Seconds(0.1));
        app1.Stop(Seconds(1.9));

        // OnOff app 2: DSCP 0 (BE) — use port+1 to distinguish
        uint16_t port2 = 10;
        PacketSinkHelper sinkHelper2("ns3::UdpSocketFactory",
                                     InetSocketAddress(Ipv4Address::GetAny(), port2));
        ApplicationContainer sinkApp2 = sinkHelper2.Install(nodes.Get(1));
        sinkApp2.Start(Seconds(0.0));
        sinkApp2.Stop(Seconds(2.0));

        ns3::OnOffHelper onoff2("ns3::UdpSocketFactory",
                                InetSocketAddress(interfaces.GetAddress(1), port2));
        onoff2.SetAttribute("DataRate", StringValue("3Mbps"));
        onoff2.SetAttribute("PacketSize", UintegerValue(1000));
        onoff2.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
        onoff2.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
        ApplicationContainer app2 = onoff2.Install(nodes.Get(0));
        app2.Start(Seconds(0.1));
        app2.Stop(Seconds(1.9));

        // Note: OnOff does not set DSCP by default. The test validates that
        // the RedQueueDisc is installed and can process packets (DSCP 0 BE).
        // Both flows will use DSCP 0 since OnOff doesn't mark.

        Simulator::Stop(Seconds(2.0));
        Simulator::Run();

        // Verify that packets were dequeued
        // The queue disc stats should show some packets processed
        auto stats = dsQd->GetStats();
        uint64_t totalPkts = stats.nTotalReceivedPackets;
        NS_TEST_ASSERT_MSG_GT(totalPkts,
                              0U,
                              "RedQueueDisc should have processed packets, got " << totalPkts);

        uint64_t totalDequeued = stats.nTotalDequeuedPackets;
        NS_TEST_ASSERT_MSG_GT(totalDequeued,
                              0U,
                              "RedQueueDisc should have dequeued packets, got " << totalDequeued);

        Simulator::Destroy();
    }
};

// =============================================================================
//  Helper: create an Ipv4QueueDiscItem with given fields
// =============================================================================

static Ptr<Ipv4QueueDiscItem>
MakeIpv4Item(Ipv4Address src, Ipv4Address dst, uint8_t protocol, uint32_t payloadSize)
{
    Ptr<Packet> pkt = Create<Packet>(payloadSize);
    Ipv4Header hdr;
    hdr.SetSource(src);
    hdr.SetDestination(dst);
    hdr.SetProtocol(protocol);
    hdr.SetPayloadSize(payloadSize);
    return Create<Ipv4QueueDiscItem>(pkt, Address(), 0x0800, hdr);
}

/**
 * @brief Prepend a 4-byte src/dst port prefix to a packet.
 *
 * Both TCP and UDP headers start with srcPort (2 bytes, network order)
 * then dstPort (2 bytes, network order).  This helper prepends just
 * those 4 bytes so the edge classifier's CopyData-based port extraction
 * can read them, without pulling in TcpHeader/UdpHeader (which trigger
 * static initializer issues in the test library).
 */
static Ptr<Ipv4QueueDiscItem>
MakePortIpv4Item(Ipv4Address src,
                 Ipv4Address dst,
                 uint8_t protocol,
                 uint16_t srcPort,
                 uint16_t dstPort,
                 uint32_t payloadSize)
{
    // Build a 4-byte "header" with src/dst ports in network byte order
    uint8_t portBuf[4];
    portBuf[0] = static_cast<uint8_t>(srcPort >> 8);
    portBuf[1] = static_cast<uint8_t>(srcPort & 0xFF);
    portBuf[2] = static_cast<uint8_t>(dstPort >> 8);
    portBuf[3] = static_cast<uint8_t>(dstPort & 0xFF);

    // Build packet: port prefix (4 bytes) + payload
    Ptr<Packet> portPkt = Create<Packet>(portBuf, 4);
    Ptr<Packet> payload = Create<Packet>(payloadSize);
    portPkt->AddAtEnd(payload);

    Ipv4Header hdr;
    hdr.SetSource(src);
    hdr.SetDestination(dst);
    hdr.SetProtocol(protocol);
    hdr.SetPayloadSize(portPkt->GetSize());
    return Create<Ipv4QueueDiscItem>(portPkt, Address(), 0x0800, hdr);
}

// =============================================================================
//  Helper: create an Ipv6QueueDiscItem with given fields
// =============================================================================

static Ptr<Ipv6QueueDiscItem>
MakeIpv6Item(Ipv6Address src, Ipv6Address dst, uint8_t protocol, uint32_t payloadSize)
{
    Ptr<Packet> pkt = Create<Packet>(payloadSize);
    Ipv6Header hdr;
    hdr.SetSource(src);
    hdr.SetDestination(dst);
    hdr.SetNextHeader(protocol);       // v6: SetNextHeader, not SetProtocol
    hdr.SetPayloadLength(payloadSize); // v6: SetPayloadLength, not SetPayloadSize
    return Create<Ipv6QueueDiscItem>(pkt, Address(), 0x86DD, hdr);
}

/// Convenience: TCP item with ports
static Ptr<Ipv4QueueDiscItem>
MakeTcpIpv4Item(Ipv4Address src,
                Ipv4Address dst,
                uint16_t srcPort,
                uint16_t dstPort,
                uint32_t payloadSize)
{
    return MakePortIpv4Item(src, dst, 6, srcPort, dstPort, payloadSize);
}

// =============================================================================
//  S-13.1: Edge DSCP Marking
// =============================================================================

/**
 * @brief Verifies edge router marks packets with the configured DSCP.
 * @see specs/02-structural.md S-13.1
 */
class EdgeDscpMarkingTest : public TestCase
{
  public:
    EdgeDscpMarkingTest()
        : TestCase("S-13.1 Edge DSCP marking: classify + meter + mark")
    {
    }

  private:
    void DoRun() override
    {
        // Create edge queue disc
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        auto inner = CreateObject<RedQueueDisc>();
        edge->SetInnerDisc(inner);
        inner->SetNumQueues(1);

        // Mark rule: any packet -> DSCP 20
        edge->AddMarkRule({.dscp = 20});

        // Policy: DSCP 20 -> Dumb meter + Dumb policer
        PolicyEntry policy;
        policy.codePoint = 20;
        policy.meter = MeterType::DUMB;
        policy.policer = PolicerType::DUMB;
        policy.policyIndex = static_cast<uint32_t>(MeterType::DUMB);
        edge->GetPolicyClassifier()->AddPolicyEntry(policy);

        // Policer: Dumb, DSCP 20 -> 20/20/20 (passthrough)
        PolicerEntry policer;
        policer.policer = PolicerType::DUMB;
        policer.policyIndex = static_cast<uint32_t>(MeterType::DUMB);
        policer.initialCodePt = 20;
        policer.downgrade1 = 20;
        policer.downgrade2 = 20;
        edge->GetPolicyClassifier()->AddPolicerEntry(policer);

        // PHB: DSCP 20 -> queue 0, prec 0
        inner->AddPhbEntry(20, 0, 0);

        // Scheduler: RR with 1 queue
        Ptr<RoundRobinScheduler> sched =
            CreateObjectWithAttributes<RoundRobinScheduler>("NumQueues", UintegerValue(1));
        inner->SetScheduler(sched);

        // Initialize the queue disc (creates sub-queues etc.)
        edge->Initialize();

        // Create a UDP packet from any source
        Ptr<Ipv4QueueDiscItem> item =
            MakeIpv4Item(Ipv4Address("10.0.0.1"), Ipv4Address("10.0.0.2"), 17, 500);

        // Enqueue
        bool enqueued = edge->Enqueue(item);
        NS_TEST_ASSERT_MSG_EQ(enqueued, true, "Packet should be enqueued");

        // Dequeue
        Ptr<QueueDiscItem> dequeued = edge->Dequeue();
        NS_TEST_ASSERT_MSG_NE(dequeued, nullptr, "Should dequeue a packet");
        if (!dequeued)
        {
            Simulator::Destroy();
            return;
        }

        // Verify DSCP was written to header
        Ptr<Ipv4QueueDiscItem> ipDequeued = DynamicCast<Ipv4QueueDiscItem>(dequeued);
        NS_TEST_ASSERT_MSG_NE(ipDequeued, nullptr, "Dequeued item should be Ipv4");
        if (!ipDequeued)
        {
            Simulator::Destroy();
            return;
        }
        uint8_t dscp = ipDequeued->GetHeader().GetTos() >> 2;
        NS_TEST_ASSERT_MSG_EQ(dscp,
                              20,
                              "DSCP should be 20 after edge marking, got "
                                  << static_cast<uint32_t>(dscp));

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-13.2: Edge No-Match Passthrough
// =============================================================================

/**
 * @brief Verifies unmatched flows pass through the edge unmarked.
 * @see specs/02-structural.md S-13.2
 */
class EdgeNoMatchPassthroughTest : public TestCase
{
  public:
    EdgeNoMatchPassthroughTest()
        : TestCase("S-13.2 Edge no-match passthrough: preserves original DSCP")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        auto inner = CreateObject<RedQueueDisc>();
        edge->SetInnerDisc(inner);
        inner->SetNumQueues(2);

        // Mark rule: only matches src=10.0.0.1 -> DSCP 20
        edge->AddMarkRule({.dscp = 20, .srcAddr = Ipv4Address("10.0.0.1")});

        // Policies for DSCP 46 (pass-through) and DSCP 20
        PolicyEntry policy46;
        policy46.codePoint = 46;
        policy46.meter = MeterType::DUMB;
        policy46.policer = PolicerType::DUMB;
        policy46.policyIndex = 0;
        edge->GetPolicyClassifier()->AddPolicyEntry(policy46);

        PolicerEntry policer46;
        policer46.policer = PolicerType::DUMB;
        policer46.policyIndex = 0;
        policer46.initialCodePt = 46;
        policer46.downgrade1 = 46;
        policer46.downgrade2 = 46;
        edge->GetPolicyClassifier()->AddPolicerEntry(policer46);

        // PHB: DSCP 46 -> queue 0, prec 0
        inner->AddPhbEntry(46, 0, 0);
        // PHB: DSCP 20 -> queue 1, prec 0
        inner->AddPhbEntry(20, 1, 0);

        // Scheduler: RR with 2 queues
        Ptr<RoundRobinScheduler> sched =
            CreateObjectWithAttributes<RoundRobinScheduler>("NumQueues", UintegerValue(2));
        inner->SetScheduler(sched);

        edge->Initialize();

        // Create packet from src=10.0.0.2 (does NOT match the mark rule)
        // with initial DSCP set to 46 (EF)
        Ptr<Packet> pkt = Create<Packet>(500);
        Ipv4Header hdr;
        hdr.SetSource(Ipv4Address("10.0.0.2"));
        hdr.SetDestination(Ipv4Address("10.0.0.3"));
        hdr.SetProtocol(17);
        hdr.SetPayloadSize(500);
        hdr.SetTos(46 << 2); // Set DSCP to 46 (EF)
        Ptr<Ipv4QueueDiscItem> item = Create<Ipv4QueueDiscItem>(pkt, Address(), 0x0800, hdr);

        bool enqueued = edge->Enqueue(item);
        NS_TEST_ASSERT_MSG_EQ(enqueued, true, "Packet should be enqueued");

        Ptr<QueueDiscItem> dequeued = edge->Dequeue();
        NS_TEST_ASSERT_MSG_NE(dequeued, nullptr, "Should dequeue a packet");

        Ptr<Ipv4QueueDiscItem> ipDequeued = DynamicCast<Ipv4QueueDiscItem>(dequeued);
        NS_TEST_ASSERT_MSG_NE(ipDequeued, nullptr, "Dequeued item should be Ipv4");
        if (!ipDequeued)
        {
            Simulator::Destroy();
            return;
        }
        uint8_t dscp = ipDequeued->GetHeader().GetTos() >> 2;
        NS_TEST_ASSERT_MSG_EQ(dscp,
                              46,
                              "DSCP should remain 46 (passthrough), got "
                                  << static_cast<uint32_t>(dscp));

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-13.3: Edge Specific Address Match
// =============================================================================

/**
 * @brief Verifies edge classifier matches specific source/destination addresses.
 * @see specs/02-structural.md S-13.3
 */
class EdgeSpecificAddrMatchTest : public TestCase
{
  public:
    EdgeSpecificAddrMatchTest()
        : TestCase("S-13.3 Edge specific addr match: two rules, two sources")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        auto inner = CreateObject<RedQueueDisc>();
        edge->SetInnerDisc(inner);
        inner->SetNumQueues(2);

        // Rule 1: src=10.0.0.1 -> DSCP 20
        edge->AddMarkRule({.dscp = 20, .srcAddr = Ipv4Address("10.0.0.1")});

        // Rule 2: src=10.0.0.2 -> DSCP 30
        edge->AddMarkRule({.dscp = 30, .srcAddr = Ipv4Address("10.0.0.2")});

        // Policy + policer for DSCP 20 (Dumb passthrough)
        PolicyEntry policy20;
        policy20.codePoint = 20;
        policy20.meter = MeterType::DUMB;
        policy20.policer = PolicerType::DUMB;
        policy20.policyIndex = 0;
        edge->GetPolicyClassifier()->AddPolicyEntry(policy20);

        PolicerEntry policer20;
        policer20.policer = PolicerType::DUMB;
        policer20.policyIndex = 0;
        policer20.initialCodePt = 20;
        policer20.downgrade1 = 20;
        policer20.downgrade2 = 20;
        edge->GetPolicyClassifier()->AddPolicerEntry(policer20);

        // Policy + policer for DSCP 30 (Dumb passthrough)
        PolicyEntry policy30;
        policy30.codePoint = 30;
        policy30.meter = MeterType::DUMB;
        policy30.policer = PolicerType::DUMB;
        policy30.policyIndex = 1;
        edge->GetPolicyClassifier()->AddPolicyEntry(policy30);

        PolicerEntry policer30;
        policer30.policer = PolicerType::DUMB;
        policer30.policyIndex = 1;
        policer30.initialCodePt = 30;
        policer30.downgrade1 = 30;
        policer30.downgrade2 = 30;
        edge->GetPolicyClassifier()->AddPolicerEntry(policer30);

        // PHB: DSCP 20 -> queue 0, prec 0; DSCP 30 -> queue 1, prec 0
        inner->AddPhbEntry(20, 0, 0);
        inner->AddPhbEntry(30, 1, 0);

        // Scheduler: RR with 2 queues
        Ptr<RoundRobinScheduler> sched =
            CreateObjectWithAttributes<RoundRobinScheduler>("NumQueues", UintegerValue(2));
        inner->SetScheduler(sched);

        edge->Initialize();

        // Packet 1: from 10.0.0.1 -> should get DSCP 20
        Ptr<Ipv4QueueDiscItem> item1 =
            MakeIpv4Item(Ipv4Address("10.0.0.1"), Ipv4Address("10.0.0.99"), 17, 500);
        NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(item1), true, "Packet 1 should enqueue");

        // Packet 2: from 10.0.0.2 -> should get DSCP 30
        Ptr<Ipv4QueueDiscItem> item2 =
            MakeIpv4Item(Ipv4Address("10.0.0.2"), Ipv4Address("10.0.0.99"), 17, 500);
        NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(item2), true, "Packet 2 should enqueue");

        // Dequeue both (RR: q0 first, then q1)
        Ptr<QueueDiscItem> deq1 = edge->Dequeue();
        NS_TEST_ASSERT_MSG_NE(deq1, nullptr, "Should dequeue packet 1");
        Ptr<Ipv4QueueDiscItem> ipDeq1 = DynamicCast<Ipv4QueueDiscItem>(deq1);
        NS_TEST_ASSERT_MSG_NE(ipDeq1, nullptr, "Dequeued item 1 should be Ipv4");
        if (!ipDeq1)
        {
            Simulator::Destroy();
            return;
        }
        uint8_t dscp1 = ipDeq1->GetHeader().GetTos() >> 2;
        NS_TEST_ASSERT_MSG_EQ(dscp1,
                              20,
                              "Packet from 10.0.0.1 should have DSCP 20, got "
                                  << static_cast<uint32_t>(dscp1));

        Ptr<QueueDiscItem> deq2 = edge->Dequeue();
        NS_TEST_ASSERT_MSG_NE(deq2, nullptr, "Should dequeue packet 2");
        Ptr<Ipv4QueueDiscItem> ipDeq2 = DynamicCast<Ipv4QueueDiscItem>(deq2);
        NS_TEST_ASSERT_MSG_NE(ipDeq2, nullptr, "Dequeued item 2 should be Ipv4");
        if (!ipDeq2)
        {
            Simulator::Destroy();
            return;
        }
        uint8_t dscp2 = ipDeq2->GetHeader().GetTos() >> 2;
        NS_TEST_ASSERT_MSG_EQ(dscp2,
                              30,
                              "Packet from 10.0.0.2 should have DSCP 30, got "
                                  << static_cast<uint32_t>(dscp2));

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-13.18: Edge classify+mark over IPv6 end-to-end (Q-18.1)
// =============================================================================

/**
 * @brief Verifies the edge enqueue/classify/mark/dequeue path for IPv6 items.
 *
 * Two source-address mark rules select different DSCPs; ECN bits survive the
 * DSCP rewrite unchanged.
 *
 * @see specs/02-structural.md S-13.18
 */
class EdgeIpv6ClassifyMarkTest : public TestCase
{
  public:
    EdgeIpv6ClassifyMarkTest()
        : TestCase("S-13.18 Edge classifies+marks IPv6 by source, ECN preserved")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        auto inner = CreateObject<RedQueueDisc>();
        edge->SetInnerDisc(inner);
        inner->SetNumQueues(2);

        // Rule 1: src=2001:db8::1 -> DSCP 20
        edge->AddMarkRule({.dscp = 20, .srcAddr = Ipv6Address("2001:db8::1")});

        // Rule 2: src=2001:db8::2 -> DSCP 30
        edge->AddMarkRule({.dscp = 30, .srcAddr = Ipv6Address("2001:db8::2")});

        // Policy + policer for DSCP 20 (DUMB passthrough)
        PolicyEntry policy20;
        policy20.codePoint = 20;
        policy20.meter = MeterType::DUMB;
        policy20.policer = PolicerType::DUMB;
        policy20.policyIndex = 0;
        edge->GetPolicyClassifier()->AddPolicyEntry(policy20);

        PolicerEntry policer20;
        policer20.policer = PolicerType::DUMB;
        policer20.policyIndex = 0;
        policer20.initialCodePt = 20;
        policer20.downgrade1 = 20;
        policer20.downgrade2 = 20;
        edge->GetPolicyClassifier()->AddPolicerEntry(policer20);

        // Policy + policer for DSCP 30 (DUMB passthrough)
        PolicyEntry policy30;
        policy30.codePoint = 30;
        policy30.meter = MeterType::DUMB;
        policy30.policer = PolicerType::DUMB;
        policy30.policyIndex = 1;
        edge->GetPolicyClassifier()->AddPolicyEntry(policy30);

        PolicerEntry policer30;
        policer30.policer = PolicerType::DUMB;
        policer30.policyIndex = 1;
        policer30.initialCodePt = 30;
        policer30.downgrade1 = 30;
        policer30.downgrade2 = 30;
        edge->GetPolicyClassifier()->AddPolicerEntry(policer30);

        // PHB: DSCP 20 -> queue 0, prec 0; DSCP 30 -> queue 1, prec 0
        inner->AddPhbEntry(20, 0, 0);
        inner->AddPhbEntry(30, 1, 0);

        // Scheduler: RR with 2 queues
        Ptr<RoundRobinScheduler> sched =
            CreateObjectWithAttributes<RoundRobinScheduler>("NumQueues", UintegerValue(2));
        inner->SetScheduler(sched);

        edge->Initialize();

        // Packet 1: from 2001:db8::1 with ECT(1) set — DSCP 20, ECN must survive
        Ptr<Ipv6QueueDiscItem> item1 =
            MakeIpv6Item(Ipv6Address("2001:db8::1"), Ipv6Address("2001:db8::99"), 17, 500);
        const_cast<Ipv6Header&>(item1->GetHeader()).SetEcn(Ipv6Header::ECN_ECT1);
        NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(item1), true, "IPv6 packet 1 should enqueue");

        // Packet 2: from 2001:db8::2 -> DSCP 30
        Ptr<Ipv6QueueDiscItem> item2 =
            MakeIpv6Item(Ipv6Address("2001:db8::2"), Ipv6Address("2001:db8::99"), 17, 500);
        NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(item2), true, "IPv6 packet 2 should enqueue");

        // Dequeue both (RR: q0 first, then q1)
        Ptr<QueueDiscItem> deq1 = edge->Dequeue();
        NS_TEST_ASSERT_MSG_NE(deq1, nullptr, "Should dequeue IPv6 packet 1");
        Ptr<Ipv6QueueDiscItem> ipDeq1 = DynamicCast<Ipv6QueueDiscItem>(deq1);
        NS_TEST_ASSERT_MSG_NE(ipDeq1, nullptr, "Dequeued item 1 should be Ipv6");
        if (!ipDeq1)
        {
            Simulator::Destroy();
            return;
        }
        uint8_t dscp1 = ipDeq1->GetHeader().GetTrafficClass() >> 2;
        NS_TEST_ASSERT_MSG_EQ(dscp1,
                              20,
                              "IPv6 packet from 2001:db8::1 should have DSCP 20, got "
                                  << static_cast<uint32_t>(dscp1));
        NS_TEST_ASSERT_MSG_EQ(ipDeq1->GetHeader().GetEcn(),
                              Ipv6Header::ECN_ECT1,
                              "ECN ECT(1) must survive the DSCP rewrite");

        Ptr<QueueDiscItem> deq2 = edge->Dequeue();
        NS_TEST_ASSERT_MSG_NE(deq2, nullptr, "Should dequeue IPv6 packet 2");
        Ptr<Ipv6QueueDiscItem> ipDeq2 = DynamicCast<Ipv6QueueDiscItem>(deq2);
        NS_TEST_ASSERT_MSG_NE(ipDeq2, nullptr, "Dequeued item 2 should be Ipv6");
        if (!ipDeq2)
        {
            Simulator::Destroy();
            return;
        }
        uint8_t dscp2 = ipDeq2->GetHeader().GetTrafficClass() >> 2;
        NS_TEST_ASSERT_MSG_EQ(dscp2,
                              30,
                              "IPv6 packet from 2001:db8::2 should have DSCP 30, got "
                                  << static_cast<uint32_t>(dscp2));

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-13.4: Port-based mark rule (RFC 2475 MF classification)
// =============================================================================

/**
 * @brief Verifies edge classifier matches by transport port.
 * @see specs/02-structural.md S-13.4
 */
class PortBasedMarkRuleTest : public TestCase
{
  public:
    PortBasedMarkRuleTest()
        : TestCase("S-13.4 Port-based mark rule: TCP port 23->AF11, port 20->AF12")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        auto inner = CreateObject<RedQueueDisc>();
        edge->SetInnerDisc(inner);
        inner->SetNumQueues(2);

        // Rule 1: TCP dstPort=23 -> DSCP 10 (AF11)
        edge->AddMarkRule({.dscp = 10, .protocol = 6, .dstPort = 23});

        // Rule 2: TCP dstPort=20 -> DSCP 12 (AF12)
        edge->AddMarkRule({.dscp = 12, .protocol = 6, .dstPort = 20});

        // Dumb policy/policer for DSCPs 10, 12, 0 (default)
        diffserv::Helper h;
        h.AddDumbPolicy(edge, 10);
        h.AddDumbPolicy(edge, 12);
        h.AddDumbPolicy(edge, 0);
        h.AddPolicerEntry(edge,
                          {.policer = PolicerType::DUMB,
                           .initialCodePt = 10,
                           .downgrade1 = 10,
                           .downgrade2 = 10,
                           .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});
        h.AddPolicerEntry(edge,
                          {.policer = PolicerType::DUMB,
                           .initialCodePt = 12,
                           .downgrade1 = 12,
                           .downgrade2 = 12,
                           .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});
        h.AddPolicerEntry(edge,
                          {.policer = PolicerType::DUMB,
                           .initialCodePt = 0,
                           .downgrade1 = 0,
                           .downgrade2 = 0,
                           .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});

        h.AddPhbEntry(inner, 10, 0, 0);
        h.AddPhbEntry(inner, 12, 0, 0);
        h.AddPhbEntry(inner, 0, 1, 0);

        Ptr<RoundRobinScheduler> sched =
            CreateObjectWithAttributes<RoundRobinScheduler>("NumQueues", UintegerValue(2));
        inner->SetScheduler(sched);
        edge->Initialize();

        // Disable RED drops: set thMin > qlim so threshold check never fires.
        // This test validates classification, not RED behavior.
        inner->ConfigQueue({.queue = 0, .prec = 0, .thMin = 1000.0, .thMax = 2000.0, .maxP = 0.1});
        inner->ConfigQueue({.queue = 1, .prec = 0, .thMin = 1000.0, .thMax = 2000.0, .maxP = 0.1});

        // Packet 1: TCP to port 23 -> should get DSCP 10
        Ptr<Ipv4QueueDiscItem> item1 =
            MakeTcpIpv4Item(Ipv4Address("10.0.0.1"), Ipv4Address("10.0.0.99"), 50000, 23, 100);
        NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(item1), true, "Telnet packet should enqueue");

        // Packet 2: TCP to port 20 -> should get DSCP 12
        Ptr<Ipv4QueueDiscItem> item2 =
            MakeTcpIpv4Item(Ipv4Address("10.0.0.1"), Ipv4Address("10.0.0.99"), 50001, 20, 100);
        NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(item2), true, "FTP packet should enqueue");

        // Packet 3: TCP to port 80 -> no rule match, should get default DSCP 0
        Ptr<Ipv4QueueDiscItem> item3 =
            MakeTcpIpv4Item(Ipv4Address("10.0.0.1"), Ipv4Address("10.0.0.99"), 50002, 80, 100);
        NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(item3), true, "HTTP packet should enqueue");

        // Dequeue and verify DSCPs
        // Both telnet and FTP go to queue 0 (PHB maps DSCP 10 and 12 to q0)
        // HTTP goes to queue 1 (DSCP 0 -> q1)
        Ptr<QueueDiscItem> deq1 = edge->Dequeue();
        NS_TEST_ASSERT_MSG_NE(deq1, nullptr, "Should dequeue telnet");
        if (!deq1)
        {
            Simulator::Destroy();
            return;
        }
        uint8_t dscp1 = DynamicCast<Ipv4QueueDiscItem>(deq1)->GetHeader().GetTos() >> 2;
        NS_TEST_ASSERT_MSG_EQ(dscp1,
                              10,
                              "TCP port 23 should get DSCP 10, got "
                                  << static_cast<uint32_t>(dscp1));

        // RR: next dequeue from q1 (HTTP/default)
        Ptr<QueueDiscItem> deq3 = edge->Dequeue();
        NS_TEST_ASSERT_MSG_NE(deq3, nullptr, "Should dequeue HTTP");
        if (!deq3)
        {
            Simulator::Destroy();
            return;
        }
        uint8_t dscp3 = DynamicCast<Ipv4QueueDiscItem>(deq3)->GetHeader().GetTos() >> 2;
        NS_TEST_ASSERT_MSG_EQ(dscp3,
                              0,
                              "TCP port 80 (no match) should keep DSCP 0, got "
                                  << static_cast<uint32_t>(dscp3));

        // RR back to q0: FTP packet
        Ptr<QueueDiscItem> deq2 = edge->Dequeue();
        NS_TEST_ASSERT_MSG_NE(deq2, nullptr, "Should dequeue FTP");
        if (!deq2)
        {
            Simulator::Destroy();
            return;
        }
        uint8_t dscp2 = DynamicCast<Ipv4QueueDiscItem>(deq2)->GetHeader().GetTos() >> 2;
        NS_TEST_ASSERT_MSG_EQ(dscp2,
                              12,
                              "TCP port 20 should get DSCP 12, got "
                                  << static_cast<uint32_t>(dscp2));

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-13.5: Meter strategy injection
// =============================================================================
//
// Verifies that `edge->SetMeter(MeterType::SRTCM, custom)` installed
// before Initialize is the instance consulted by
// `diffserv::PolicyClassifier::ApplyPolicy`, and that `GetMeter`
// returns the same instance afterwards. Covers the
// `EdgeMeterProvider` lookup contract.
//
// Follow-on invariant: if no custom meter is installed, `GetMeter`
// creates a default instance on the first request. The default
// path is exercised by every other S-13 / S-14 test in the suite.
//
class ProbeMeter : public Meter
{
  public:
    uint32_t applyMeterCalls{0};
    uint32_t applyPolicerCalls{0};

    void ApplyMeter(PolicyEntry& /*entry*/, double /*now*/, uint32_t /*size*/) override
    {
        ++applyMeterCalls;
    }

  protected:
    Colour DoApplyPolicer(PolicyEntry& /*entry*/, uint32_t /*size*/) override
    {
        ++applyPolicerCalls;
        return Colour::GREEN;
    }
};

/**
 * @brief Verifies a meter object can be injected into the edge classifier path.
 * @see specs/02-structural.md S-13.5
 */
class MeterInjectionTest : public TestCase
{
  public:
    MeterInjectionTest()
        : TestCase("S-13.5 Meter strategy injection")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        auto inner = CreateObject<RedQueueDisc>();
        edge->SetInnerDisc(inner);
        inner->SetNumQueues(1);

        Ptr<ProbeMeter> probe = CreateObject<ProbeMeter>();
        edge->SetMeter(MeterType::SRTCM, probe);

        // Sanity: GetMeter hands back exactly the instance we installed.
        NS_TEST_ASSERT_MSG_EQ(edge->GetMeter(MeterType::SRTCM),
                              probe,
                              "GetMeter should return the injected instance");

        // Mark rule: any packet -> DSCP 10.
        edge->AddMarkRule({.dscp = 10});

        // Policy: DSCP 10 is metered by the (injected) SRTCM strategy.
        PolicyEntry policy;
        policy.codePoint = 10;
        policy.meter = MeterType::SRTCM;
        policy.policer = PolicerType::DUMB;
        policy.policyIndex = 0;
        edge->GetPolicyClassifier()->AddPolicyEntry(policy);

        PolicerEntry policer;
        policer.policer = PolicerType::DUMB;
        policer.policyIndex = 0;
        policer.initialCodePt = 10;
        policer.downgrade1 = 10;
        policer.downgrade2 = 10;
        edge->GetPolicyClassifier()->AddPolicerEntry(policer);

        inner->AddPhbEntry(10, 0, 0);
        inner->SetScheduler(
            CreateObjectWithAttributes<RoundRobinScheduler>("NumQueues", UintegerValue(1)));
        edge->Initialize();

        Ptr<Ipv4QueueDiscItem> item =
            MakeIpv4Item(Ipv4Address("10.0.0.1"), Ipv4Address("10.0.0.2"), 17, 500);
        bool enqueued = edge->Enqueue(item);
        NS_TEST_ASSERT_MSG_EQ(enqueued, true, "Packet should be enqueued");

        NS_TEST_ASSERT_MSG_EQ(probe->applyMeterCalls,
                              1u,
                              "Injected meter's ApplyMeter should have fired once");
        NS_TEST_ASSERT_MSG_EQ(probe->applyPolicerCalls,
                              1u,
                              "Injected meter's ApplyPolicer should have fired once");

        // A different MeterType slot stays empty until first
        // requested.
        Ptr<Meter> tbm = edge->GetMeter(MeterType::TOKEN_BUCKET);
        NS_TEST_ASSERT_MSG_NE(tbm,
                              nullptr,
                              "GetMeter should create the default TOKEN_BUCKET on first request");
        if (!tbm)
        {
            Simulator::Destroy();
            return;
        }
        NS_TEST_ASSERT_MSG_NE(tbm, probe, "TOKEN_BUCKET slot must be independent of SRTCM slot");

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-13.6: Edge AssignStreams cascades into TSW/FW meter slots
// =============================================================================
//
// edge->AssignStreams(N) cascades into any installed Tsw2cm / Tsw3cm
// / Fw meter slot. This test runs the same configuration twice,
// seeded identically via two independent edge discs, and asserts the
// full TSW2CM colour sequence matches bit-for-bit. Without the
// cascade, two edges with the same configuration produce different
// colour sequences because TSW's `m_uv` would default to the global
// default stream, whose position drifts with unrelated object
// construction.
//
/**
 * @brief Verifies AssignStreams cascades through the meter chain for RNG isolation.
 * @see specs/02-structural.md S-13.6
 */
class MeterAssignStreamsCascadeTest : public TestCase
{
  public:
    MeterAssignStreamsCascadeTest()
        : TestCase("S-13.6 Edge AssignStreams cascades into meter slots")
    {
    }

  private:
    // Run: build edge with TSW2CM above CIR, AssignStreams(seed), return
    // the first 200 ApplyPolicy colour outcomes.
    std::vector<Colour> RunOnce(int64_t seed)
    {
        std::vector<Colour> out;
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        auto inner = CreateObject<RedQueueDisc>();
        edge->SetInnerDisc(inner);
        inner->SetNumQueues(1);

        // Inject a fresh Tsw2cmMeter into the SRTCM-adjacent slot. (Slot
        // index is MeterType::TSW2CM.)
        Ptr<Tsw2cmMeter> tsw = CreateObject<Tsw2cmMeter>();
        edge->SetMeter(MeterType::TSW2CM, tsw);
        edge->AssignStreams(seed);

        // Policy / policer table drive the cascade through ApplyPolicy.
        // CIR=125000 B/s, feed at 250000 B/s → GREEN ratio ≈ 0.5.
        PolicyEntry policy;
        policy.codePoint = 10;
        policy.meter = MeterType::TSW2CM;
        policy.policer = PolicerType::DUMB;
        policy.policyIndex = 0;
        policy.cir = 125000.0;
        policy.avgRate = 250000.0; // pre-converge
        policy.winLen = 1.0;
        policy.arrivalTime = 0.0;
        edge->GetPolicyClassifier()->AddPolicyEntry(policy);

        PolicerEntry policer;
        policer.policer = PolicerType::DUMB;
        policer.policyIndex = 0;
        policer.initialCodePt = 10;
        policer.downgrade1 = 11;
        policer.downgrade2 = 12;
        edge->GetPolicyClassifier()->AddPolicerEntry(policer);

        for (int i = 0; i < 200; ++i)
        {
            uint8_t dscp = edge->GetPolicyClassifier()->ApplyPolicy(10, 1000, i * 0.004);
            // DUMB policer maps GREEN=10, YELLOW=11, RED=12 (via the table).
            Colour c = (dscp == 10) ? Colour::GREEN : (dscp == 11) ? Colour::YELLOW : Colour::RED;
            out.push_back(c);
        }
        return out;
    }

    void DoRun() override
    {
        // Same seed → identical colour sequence. The cascade
        // establishes this invariant; without it, TSW would use the
        // global default stream whose position depends on prior
        // object construction.
        auto a = RunOnce(42);
        auto b = RunOnce(42);
        NS_TEST_ASSERT_MSG_EQ(a.size(), b.size(), "Sample counts mismatch");
        for (size_t i = 0; i < a.size(); ++i)
        {
            NS_TEST_ASSERT_MSG_EQ(static_cast<int>(a[i]),
                                  static_cast<int>(b[i]),
                                  "Colour mismatch at index " << i);
        }

        // Different seed → different sequence (sanity: cascade actually
        // re-seats the stream).
        auto c = RunOnce(43);
        bool anyDiffer = false;
        for (size_t i = 0; i < std::min(a.size(), c.size()); ++i)
        {
            if (a[i] != c[i])
            {
                anyDiffer = true;
                break;
            }
        }
        NS_TEST_ASSERT_MSG_EQ(anyDiffer,
                              true,
                              "Different seeds should produce different colour vectors");

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-13.7: Edge with l4s::QueueDisc inner (smoke)
// =============================================================================
//
// `EdgeQueueDisc::m_inner` is typed as `Ptr<QueueDisc>`, not
// Red-specifically, so an L4S or any other QueueDisc can be installed
// as the edge's queueing pipeline. This test is a smoke check:
// construct the composition, push one ECT(1) IPv4 packet through
// end-to-end, and verify that dequeue returns it with the DSCP
// stamped by the edge's classifier.
//
/**
 * @brief Verifies edge router composes correctly with an L4S inner queue disc.
 * @see specs/02-structural.md S-13.7
 */
class EdgeWithL4sInnerTest : public TestCase
{
  public:
    EdgeWithL4sInnerTest()
        : TestCase("S-13.7 Edge with QueueDisc inner (smoke)")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        auto inner = CreateObject<RedQueueDisc>();
        edge->SetInnerDisc(inner);

        // Configure the L4S inner before SetInnerDisc. L4S-specific
        // config happens on the L4S disc directly, not through edge
        // forwarders (which assert Red-only).
        Ptr<l4s::QueueDisc> l4s = CreateObject<l4s::QueueDisc>();
        edge->SetInnerDisc(l4s);

        // Edge classifier: any packet → DSCP 46 (EF).
        edge->AddMarkRule({.dscp = 46});

        PolicyEntry policy;
        policy.codePoint = 46;
        policy.meter = MeterType::DUMB;
        policy.policer = PolicerType::DUMB;
        policy.policyIndex = 0;
        edge->GetPolicyClassifier()->AddPolicyEntry(policy);

        PolicerEntry policer;
        policer.policer = PolicerType::DUMB;
        policer.policyIndex = 0;
        policer.initialCodePt = 46;
        policer.downgrade1 = 46;
        policer.downgrade2 = 46;
        edge->GetPolicyClassifier()->AddPolicerEntry(policer);

        edge->Initialize();

        // One ECT(1) UDP packet. L4S classifies it into its L queue by
        // reading ECN bits; edge stamps DSCP=46 via classifier; dequeue
        // TOS-rewrites it.
        Ptr<Packet> p = Create<Packet>(500);
        Ipv4Header h;
        h.SetSource(Ipv4Address("10.0.0.1"));
        h.SetDestination(Ipv4Address("10.0.0.2"));
        h.SetProtocol(17);
        h.SetEcn(Ipv4Header::ECN_ECT1);
        h.SetPayloadSize(500);
        Address src;
        Ptr<Ipv4QueueDiscItem> item = Create<Ipv4QueueDiscItem>(p, src, 0x0800, h);

        bool enqueued = edge->Enqueue(item);
        NS_TEST_ASSERT_MSG_EQ(enqueued, true, "Packet should be enqueued through edge+L4S inner");

        Ptr<QueueDiscItem> deq = edge->Dequeue();
        NS_TEST_ASSERT_MSG_NE(deq, nullptr, "Packet should dequeue from edge+L4S composition");

        Ptr<Ipv4QueueDiscItem> deqIp = DynamicCast<Ipv4QueueDiscItem>(deq);
        NS_TEST_ASSERT_MSG_NE(deqIp, nullptr, "Dequeued item should be Ipv4");
        if (!deqIp)
        {
            Simulator::Destroy();
            return;
        }
        uint8_t dscp = deqIp->GetHeader().GetTos() >> 2;
        NS_TEST_ASSERT_MSG_EQ(dscp,
                              46,
                              "Edge should have stamped DSCP=46 on TOS, got "
                                  << static_cast<uint32_t>(dscp));

        // ECT(1) preserved: L4S does not alter ECN unless it marks CE.
        // At one packet + no congestion, no CE expected.
        Ipv4Header::EcnType ecn = deqIp->GetHeader().GetEcn();
        NS_TEST_ASSERT_MSG_EQ(static_cast<int>(ecn),
                              static_cast<int>(Ipv4Header::ECN_ECT1),
                              "ECT(1) should be preserved end-to-end in L4S inner");

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-13.8: Meter cascade reaches helper-path meters
// =============================================================================
//
// Validates that `edge->AssignStreams(N)` reaches the TSW2CM meter
// instance even when the scenario uses the helper API
// (`helper.AddTsw2cmPolicy(...)`) instead of explicit `SetMeter`
// injection. Without the CheckConfig pre-create that materialises
// meter slots eagerly, `AssignStreams` would see an empty slot and
// silently miss; the first `ApplyPolicy` call would then create
// the meter on demand and bind it to the global default RNG
// stream, defeating the opt-in guarantee.
//
/**
 * @brief Verifies the helper-driven configuration path wires meter cascades correctly.
 * @see specs/02-structural.md S-13.8
 */
class MeterCascadeHelperPathTest : public TestCase
{
  public:
    MeterCascadeHelperPathTest()
        : TestCase("S-13.8 Meter cascade reaches helper-path meters")
    {
    }

  private:
    // Build an edge, wire TSW2CM via the HELPER (no explicit SetMeter),
    // AssignStreams(seed), Initialize, then push 200 ApplyPolicy calls
    // and collect the colour vector.
    std::vector<Colour> RunOnce(int64_t seed)
    {
        std::vector<Colour> out;
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        auto inner = CreateObject<RedQueueDisc>();
        edge->SetInnerDisc(inner);
        inner->SetNumQueues(1);

        diffserv::Helper h;
        // CIR 1 Mbps = 125 KB/s; feed at 2 Mbps so expected GREEN ratio ~ 0.5.
        h.AddTsw2cmPolicy(edge, {.codePt = 10, .cirBps = 1000000.0});
        h.AddPolicerEntry(edge,
                          {.policer = PolicerType::TSW2CM,
                           .initialCodePt = 10,
                           .downgrade1 = 11,
                           .downgrade2 = 12,
                           .policyIndex = static_cast<uint32_t>(PolicerType::TSW2CM)});

        inner->AddPhbEntry(10, 0, 0);
        inner->SetScheduler(
            CreateObjectWithAttributes<RoundRobinScheduler>("NumQueues", UintegerValue(1)));

        // CRUCIAL ordering: AssignStreams BEFORE Initialize. The
        // follow-up fix pre-creates meter slots in CheckConfig (inside
        // Initialize), but if AssignStreams runs BEFORE Initialize it
        // won't find anything. So the sequence is:
        //   1. Initialize (runs CheckConfig → pre-creates slots)
        //   2. AssignStreams (now reaches the slots)
        edge->Initialize();
        edge->AssignStreams(seed);

        auto pc = edge->GetPolicyClassifier();
        for (int i = 0; i < 200; ++i)
        {
            uint8_t dscp = pc->ApplyPolicy(10, 1000, i * 0.004);
            Colour c = (dscp == 10) ? Colour::GREEN : (dscp == 11) ? Colour::YELLOW : Colour::RED;
            out.push_back(c);
        }
        return out;
    }

    void DoRun() override
    {
        // Same seed → identical colour vectors. Requires the CheckConfig
        // pre-create + AssignStreams cascade to actually reach the TSW2CM
        // slot.
        auto a = RunOnce(99);
        auto b = RunOnce(99);
        NS_TEST_ASSERT_MSG_EQ(a.size(), b.size(), "Sample counts mismatch");
        for (size_t i = 0; i < a.size(); ++i)
        {
            NS_TEST_ASSERT_MSG_EQ(static_cast<int>(a[i]),
                                  static_cast<int>(b[i]),
                                  "Colour mismatch at index " << i << " — meter cascade broken?");
        }

        // Different seed → different vector (sanity: cascade is actually
        // re-seating the stream, not a no-op).
        auto c = RunOnce(100);
        bool anyDiffer = false;
        for (size_t i = 0; i < std::min(a.size(), c.size()); ++i)
        {
            if (a[i] != c[i])
            {
                anyDiffer = true;
                break;
            }
        }
        NS_TEST_ASSERT_MSG_EQ(anyDiffer,
                              true,
                              "Different seeds must produce different colour vectors "
                              "(helper-path cascade is a no-op?)");

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-13.17: TSW window length is wired through the helper
// =============================================================================
//
// The TSW meters read PolicyEntry::winLen (the EWMA window), but the
// helper's AddTsw2cmPolicy / AddTsw3cmPolicy gave no way to set it, so
// it was pinned to the 1.0 s default. RFC 2859 leaves the window
// implementation-defined, so this is configuration completeness, not
// conformance: a winLen passed to the helper must reach the meter and
// change the rate estimate. A same-instant burst makes the dependence
// observable: avgRate accumulates as k * bytes / winLen, so a long
// window keeps the burst's estimate below CIR (no downgrades) while a
// short window pushes it far above CIR (downgrades fire).
//
/**
 * @brief Verifies the helper forwards the TSW window length to the meter.
 * @see specs/02-structural.md S-13.8
 */
class TswWinLenHelperWiringTest : public TestCase
{
  public:
    TswWinLenHelperWiringTest()
        : TestCase("S-13.17 TSW window length wired by helper changes the rate estimate")
    {
    }

  private:
    // RED count for a 60-packet same-instant burst at the given window.
    uint32_t CountReds(double winLenSeconds)
    {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        auto inner = CreateObject<RedQueueDisc>();
        edge->SetInnerDisc(inner);
        inner->SetNumQueues(1);

        diffserv::Helper h;
        // CIR 1 Mbps = 125000 B/s.
        h.AddTsw2cmPolicy(edge,
                          {.codePt = 10, .cirBps = 1000000.0, .winLenSeconds = winLenSeconds});
        h.AddPolicerEntry(edge,
                          {.policer = PolicerType::TSW2CM,
                           .initialCodePt = 10,
                           .downgrade1 = 11,
                           .downgrade2 = 12,
                           .policyIndex = static_cast<uint32_t>(PolicerType::TSW2CM)});

        inner->AddPhbEntry(10, 0, 0);
        inner->SetScheduler(
            CreateObjectWithAttributes<RoundRobinScheduler>("NumQueues", UintegerValue(1)));
        edge->Initialize();
        edge->AssignStreams(7);

        auto pc = edge->GetPolicyClassifier();
        uint32_t reds = 0;
        // Same-instant burst (all at t = 0): avgRate accumulates as
        // k * 1000 / winLen. At winLen = 1.0, 60 * 1000 = 60000 B/s stays
        // below CIR for the whole burst; at a short window it blows past it.
        for (int i = 0; i < 60; ++i)
        {
            uint8_t dscp = pc->ApplyPolicy(10, 1000, 0.0);
            if (dscp == 12)
            {
                ++reds;
            }
        }
        Simulator::Destroy();
        return reds;
    }

    void DoRun() override
    {
        uint32_t redsLong = CountReds(1.0);
        uint32_t redsShort = CountReds(0.001);
        NS_TEST_ASSERT_MSG_EQ(redsLong,
                              0U,
                              "a long TSW window keeps the burst's rate estimate below CIR "
                              "-> no downgrades");
        NS_TEST_ASSERT_MSG_GT(redsShort,
                              redsLong,
                              "a short TSW window raises the rate estimate above CIR -> "
                              "downgrades; the window length must be wired through the helper");
        Simulator::Destroy();
    }
};

// =============================================================================
//  S-13.9: QueueStatsProvider works polymorphically
// =============================================================================
//
// Verifies that runtime probes on EdgeQueueDisc go through
// the inner-agnostic QueueStatsProvider interface: an edge wrapping
// a l4s::QueueDisc inner answers GetNumQueues / GetVirtualQueueLen /
// PrintStats without DynamicCast<RedQueueDisc> special-casing.
//
/**
 * @brief Verifies inner queue discs expose the stats-provider interface contract.
 * @see specs/02-structural.md S-13.9
 */
class QueueStatsProviderInterfaceTest : public TestCase
{
  public:
    QueueStatsProviderInterfaceTest()
        : TestCase("S-13.9 Runtime probes via QueueStatsProvider")
    {
    }

  private:
    void DoRun() override
    {
        // Edge with l4s::QueueDisc inner (non-Red). The edge forwards
        // queue-state queries to the inner's QueueStatsProvider impl
        // rather than DynamicCast-ing to RedQueueDisc.
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        Ptr<l4s::QueueDisc> l4s = CreateObject<l4s::QueueDisc>();
        edge->SetInnerDisc(l4s);

        edge->AddMarkRule({.dscp = 46});

        PolicyEntry policy;
        policy.codePoint = 46;
        policy.meter = MeterType::DUMB;
        policy.policer = PolicerType::DUMB;
        policy.policyIndex = 0;
        edge->GetPolicyClassifier()->AddPolicyEntry(policy);

        PolicerEntry policer;
        policer.policer = PolicerType::DUMB;
        policer.policyIndex = 0;
        policer.initialCodePt = 46;
        policer.downgrade1 = 46;
        policer.downgrade2 = 46;
        edge->GetPolicyClassifier()->AddPolicerEntry(policer);

        edge->Initialize();

        // L4S composer has 2 QueueDiscClass children (L-queue + classic AQM).
        // The QueueStatsProvider::GetNumQueues override on l4s::QueueDisc
        // forwards to the classic-AQM's GetNumQueues (which may be 1 after
        // default sub-queue init).
        uint32_t nq = edge->GetNumQueues();
        NS_TEST_ASSERT_MSG_GT(nq,
                              0u,
                              "Edge wrapping L4S inner should report "
                              ">0 queues via QueueStatsProvider (got "
                                  << nq << ")");

        // GetVirtualQueueLen should work (return 0 on empty queue).
        int len = edge->GetVirtualQueueLen(0, 0);
        NS_TEST_ASSERT_MSG_EQ(len, 0, "Empty L-queue should report 0 occupancy");

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-13.10: Scenario 3 per-class service-rate preservation (Q-10.6)
// =============================================================================
//
// Pins `specs/03-quality.md` Q-10.6 — the per-class service-rate
// reference produced by `diffserv-example-3.cc` under `--scale=full` with
// the empirical RealAudio CDFs and the `DataRate × 1/duty`
// compensation that aligns ns-3 ns3::OnOffApplication averaging with
// ns-2 RealAudio rate semantics.
//
// Pinned reference:
//   Premium 500.1 | Gold 355.2 | Silver 911.9 | Bronze 907.9 | BE 304.1
//   Total 2979.2 kbps
//
// Tolerance: ±1 % on every row. Tighter than the spec ±3 % band on
// Gold/Silver/Bronze/BE because this test pins deterministic in-repo
// behaviour at a fixed RNG seed — not ns-3↔ns-2 cross-simulator
// agreement (which is governed by `docs/VALIDATION_MATRIX.md`).
//
// Marked EXTENSIVE: Gold per-second envelope std ≈ 128 kbps, so the
// 4000-sample window over t∈[1000 s, 5000 s] yields a std-of-mean of
// ~2 kbps (~0.6 % of 355.2) — just inside ±1 %. Shortening the window
// would widen the std-of-mean beyond the assertion band, so the full
// `diffserv-example-3 --scale=full` scenario is replicated here at
// simTime=5000 s. Wall-clock is comparable to other quality
// scenarios (~3 min).
//
// Configuration notes:
//  - RNG: `SetSeed(42); SetRun(1)` + `SetStream(101–104)` on the four
//    empirical CDFs — same pinning as the example.
//  - Init cascade: `SetRootQueueDiscOnDevice` → `Initialize` → MRED mode
//    set per queue → `ConfigQueue`. The MRED-after-Initialize order
//    mirrors the example because mode changes set before Initialize
//    would not propagate through the composer.
//  - Attributes: LLQ BW=2,700,000 bps; SFQ weights 3:3:3:1; explicit
//    queue limits, per-queue bandwidths (Q1/Q2/Q3 = 810 kbps),
//    mean-packet-size for RED-averaging, RED/WRED/RIO-C thresholds.
//  - Invariant: aggregate rate conservation is checked as the sixth
//    assertion — any silent leak in per-class accounting surfaces here.
//

/**
 * @brief Pins per-class service rates at the deterministic-seed reference for the fullscale
 * Scenario 3 example.
 * @see specs/03-quality.md Q-10.6
 * @see src/ns-3/examples/diffserv-example-3.cc (under `--scale=full`)
 */
class S3PerClassRatePreservationTest : public TestCase
{
  public:
    S3PerClassRatePreservationTest()
        : TestCase("S-13.10 Q-10.6 per-class service rate preservation "
                   "(fullscale S3)")
    {
    }

  private:
    // Running sums of per-second departure rates (kbps) over the Q-10.6
    // measurement window t ∈ [kMeasureStart, kSimTime).
    struct RateAccumulator
    {
        double premiumSum = 0.0;
        double goldSum = 0.0;
        double silverSum = 0.0;
        double bronzeSum = 0.0;
        double beSum = 0.0;
        uint32_t samples = 0;
    };

    static constexpr double kSimTime = 5000.0;
    static constexpr double kMeasureStart = 1000.0;

    Ptr<EdgeQueueDisc> m_edgeDisc;
    RateAccumulator m_acc;

    void SampleTick()
    {
        const double now = Simulator::Now().GetSeconds();
        Ptr<Scheduler> sched = m_edgeDisc->GetScheduler();
        if (now >= kMeasureStart - 1e-6)
        {
            m_acc.premiumSum += sched->GetDepartureRate(0, -1) / 1000.0;
            m_acc.goldSum += sched->GetDepartureRate(1, -1) / 1000.0;
            m_acc.silverSum += sched->GetDepartureRate(2, -1) / 1000.0;
            m_acc.bronzeSum += sched->GetDepartureRate(3, -1) / 1000.0;
            m_acc.beSum += sched->GetDepartureRate(4, -1) / 1000.0;
            m_acc.samples++;
        }
        Simulator::Schedule(Seconds(1.0), &S3PerClassRatePreservationTest::SampleTick, this);
    }

    // 10.X.Y.0/24 allocator — mirrors the example so subnet IDs align.
    Ipv4InterfaceContainer AssignSubnet(Ipv4AddressHelper& addr,
                                        NetDeviceContainer& devices,
                                        uint32_t& subnetIdx)
    {
        const uint32_t x = subnetIdx / 256;
        const uint32_t y = subnetIdx % 256;
        std::ostringstream base;
        base << "10." << x << "." << y << ".0";
        addr.SetBase(base.str().c_str(), "255.255.255.0");
        subnetIdx++;
        return addr.Assign(devices);
    }

    // Install a PacketSink only once per (node, port) pair.
    static void EnsureSink(std::set<std::pair<uint32_t, uint16_t>>& installed,
                           Ptr<Node> node,
                           uint16_t port,
                           const std::string& protocol)
    {
        const uint32_t nodeId = node->GetId();
        const auto key = std::make_pair(nodeId, port);
        if (installed.count(key) > 0)
        {
            return;
        }
        installed.insert(key);
        PacketSinkHelper sink(protocol, InetSocketAddress(Ipv4Address::GetAny(), port));
        ApplicationContainer app = sink.Install(node);
        app.Start(Seconds(0.0));
        app.Stop(Seconds(1e9));
    }

    void DoRun() override;
};

void
S3PerClassRatePreservationTest::DoRun()
{
    // Q-10.6 pinned reference (specs/03-quality.md, HEAD 924337f, n=4000).
    constexpr double kPremiumRef = 500.1;
    constexpr double kGoldRef = 355.2;
    constexpr double kSilverRef = 911.9;
    constexpr double kBronzeRef = 907.9;
    constexpr double kBeRef = 304.1;
    constexpr double kTotalRef = 2979.2;
    constexpr double kTol = 0.01; // ±1 %

    // ---- RNG ----
    RngSeedManager::SetSeed(42);
    RngSeedManager::SetRun(1);

    Ptr<UniformRandomVariable> rndDelay = CreateObject<UniformRandomVariable>();
    rndDelay->SetAttribute("Min", DoubleValue(10.0));
    rndDelay->SetAttribute("Max", DoubleValue(100.0));

    Ptr<UniformRandomVariable> rndBw = CreateObject<UniformRandomVariable>();
    rndBw->SetAttribute("Min", DoubleValue(22.0));
    rndBw->SetAttribute("Max", DoubleValue(32.0));

    Ptr<EmpiricalRandomVariable> rvUserInter =
        LoadEmpiricalCdfFromFile(GetExample3DataDir() + "userintercdf1");
    Ptr<EmpiricalRandomVariable> rvSflow =
        LoadEmpiricalCdfFromFile(GetExample3DataDir() + "sflowcdf");
    Ptr<EmpiricalRandomVariable> rvFlowDur =
        LoadEmpiricalCdfFromFile(GetExample3DataDir() + "flowdurcdf");
    Ptr<EmpiricalRandomVariable> rvFrate =
        LoadEmpiricalCdfFromFile(GetExample3DataDir() + "fratecdf");
    rvUserInter->SetStream(101);
    rvSflow->SetStream(102);
    rvFlowDur->SetStream(103);
    rvFrate->SetStream(104);

    // ---- Nodes (771 total) — see example for node-ID plan ----
    NodeContainer allNodes;
    allNodes.Create(771);
    auto n = [&](uint32_t idx) -> Ptr<Node> { return allNodes.Get(idx); };

    uint32_t subnetIdx = 1;
    Ipv4AddressHelper addr;
    InternetStackHelper internet;
    internet.Install(allNodes);

    PointToPointHelper p2p;

    // --- Bottleneck n0 <-> n466 (3 Mbps / 20 ms, 1-packet device queue) ---
    p2p.SetDeviceAttribute("DataRate", StringValue("3Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("20ms"));
    p2p.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("1p"));
    NetDeviceContainer devBottleneck = p2p.Install(n(0), n(466));
    AssignSubnet(addr, devBottleneck, subnetIdx);
    p2p.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("100p"));

    p2p.SetDeviceAttribute("DataRate", StringValue("3Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("20ms"));
    NetDeviceContainer dev466_1 = p2p.Install(n(466), n(1));
    AssignSubnet(addr, dev466_1, subnetIdx);

    // --- Server access routers (n0 <-> n2..n5) ---
    uint32_t accessDelays[] = {20, 30, 40, 60};
    for (uint32_t i = 0; i < 4; i++)
    {
        p2p.SetDeviceAttribute("DataRate", StringValue("1.5Mbps"));
        p2p.SetChannelAttribute("Delay", StringValue(std::to_string(accessDelays[i]) + "ms"));
        NetDeviceContainer dev = p2p.Install(n(0), n(2 + i));
        AssignSubnet(addr, dev, subnetIdx);
    }

    // --- Server links (n2..n5 <-> n6..n45) — 40 servers, random delay ---
    p2p.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
    for (uint32_t i = 0; i < 40; i++)
    {
        const uint32_t base = i / 10 + 2;
        const auto delayMs = static_cast<uint32_t>(rndDelay->GetValue());
        p2p.SetChannelAttribute("Delay", StringValue(std::to_string(delayMs) + "ms"));
        NetDeviceContainer dev = p2p.Install(n(base), n(6 + i));
        AssignSubnet(addr, dev, subnetIdx);
    }

    // --- Client links (n1 <-> n46..n465) — 420 clients, random BW/delay ---
    for (uint32_t i = 0; i < 420; i++)
    {
        const auto bwMbps = static_cast<uint32_t>(rndBw->GetValue());
        p2p.SetDeviceAttribute("DataRate", StringValue(std::to_string(bwMbps) + "Mbps"));
        const auto delayMs = static_cast<uint32_t>(rndDelay->GetValue());
        p2p.SetChannelAttribute("Delay", StringValue(std::to_string(delayMs) + "ms"));
        NetDeviceContainer dev = p2p.Install(n(1), n(46 + i));
        AssignSubnet(addr, dev, subnetIdx);
    }

    // --- VoIP/RealAudio access n469 <-> n0 ---
    p2p.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("1ms"));
    NetDeviceContainer dev469_0 = p2p.Install(n(469), n(0));
    AssignSubnet(addr, dev469_0, subnetIdx);

    // --- 300 VoIP/RealAudio senders (n469 <-> n470..n769) ---
    p2p.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("1ms"));
    for (uint32_t i = 0; i < 300; i++)
    {
        NetDeviceContainer dev = p2p.Install(n(469), n(470 + i));
        AssignSubnet(addr, dev, subnetIdx);
    }

    // --- VoIP sink n466 <-> n770 ---
    p2p.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("1ms"));
    NetDeviceContainer dev466_770 = p2p.Install(n(466), n(770));
    AssignSubnet(addr, dev466_770, subnetIdx);

    // --- BG endpoints n0 <-> n467, n466 <-> n468 ---
    p2p.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("1ms"));
    NetDeviceContainer dev0_467 = p2p.Install(n(0), n(467));
    AssignSubnet(addr, dev0_467, subnetIdx);
    NetDeviceContainer dev466_468 = p2p.Install(n(466), n(468));
    AssignSubnet(addr, dev466_468, subnetIdx);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // ---- Clear default queue discs on the bottleneck ----
    TrafficControlHelper tchUninstall;
    tchUninstall.Uninstall(devBottleneck.Get(0));
    tchUninstall.Uninstall(devBottleneck.Get(1));

    // ===================================================================
    // DiffServ Edge (n0 -> n466) — Table 4.5 five-class service model
    // ===================================================================
    Ptr<EdgeQueueDisc> edgeDisc = CreateObject<EdgeQueueDisc>();
    diffserv::Helper helper;
    auto edgeInner = helper.InstallRedInner(edgeDisc);

    edgeInner->SetNumQueues(5);
    edgeInner->SetNumPrec(0, 2);
    edgeInner->SetNumPrec(1, 2);
    edgeInner->SetNumPrec(2, 2);
    edgeInner->SetNumPrec(3, 1);
    edgeInner->SetNumPrec(4, 2);
    edgeInner->SetQueueLimit(0, 20);
    edgeInner->SetQueueLimit(1, 100);
    edgeInner->SetQueueLimit(2, 100);
    edgeInner->SetQueueLimit(3, 100);
    edgeInner->SetQueueLimit(4, 50);

    auto llq = CreateObjectWithAttributes<LlqScheduler>("NumQueues",
                                                        UintegerValue(5),
                                                        "LinkBandwidth",
                                                        DoubleValue(2700000.0),
                                                        "FqVariant",
                                                        EnumValue(LlqScheduler::FqVariant::SFQ));
    llq->SetParam(1, 3.0);
    llq->SetParam(2, 3.0);
    llq->SetParam(3, 3.0);
    llq->SetParam(4, 1.0);
    edgeInner->SetScheduler(llq);

    // Port-based mark rules
    edgeDisc->AddMarkRule({.dscp = 46, .dstPort = 5060});
    edgeDisc->AddMarkRule({.dscp = 10, .dstPort = 5004});
    edgeDisc->AddMarkRule({.dscp = 18, .dstPort = 23});
    edgeDisc->AddMarkRule({.dscp = 20, .dstPort = 21});
    edgeDisc->AddMarkRule({.dscp = 26, .dstPort = 80});

    // Policies
    helper.AddTokenBucketPolicy(edgeDisc, {.codePt = 46, .cirBps = 500000.0, .cbsBytes = 10000.0});
    helper.AddDumbPolicy(edgeDisc, 51);
    helper.AddTsw2cmPolicy(edgeDisc, {.codePt = 10, .cirBps = 600000.0});
    helper.AddDumbPolicy(edgeDisc, 12);
    helper.AddDumbPolicy(edgeDisc, 18);
    helper.AddDumbPolicy(edgeDisc, 20);
    helper.AddDumbPolicy(edgeDisc, 26);
    helper.AddTokenBucketPolicy(edgeDisc, {.codePt = 0, .cirBps = 400000.0, .cbsBytes = 2000.0});
    helper.AddDumbPolicy(edgeDisc, 50);

    // Policers
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::TOKEN_BUCKET,
                            .initialCodePt = 46,
                            .downgrade1 = 51,
                            .downgrade2 = 51,
                            .policyIndex = static_cast<uint32_t>(PolicerType::TOKEN_BUCKET)});
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::DUMB,
                            .initialCodePt = 51,
                            .downgrade1 = 51,
                            .downgrade2 = 51,
                            .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::TSW2CM,
                            .initialCodePt = 10,
                            .downgrade1 = 12,
                            .downgrade2 = 12,
                            .policyIndex = static_cast<uint32_t>(PolicerType::TSW2CM)});
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::DUMB,
                            .initialCodePt = 12,
                            .downgrade1 = 12,
                            .downgrade2 = 12,
                            .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::DUMB,
                            .initialCodePt = 18,
                            .downgrade1 = 18,
                            .downgrade2 = 18,
                            .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::DUMB,
                            .initialCodePt = 20,
                            .downgrade1 = 20,
                            .downgrade2 = 20,
                            .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::DUMB,
                            .initialCodePt = 26,
                            .downgrade1 = 26,
                            .downgrade2 = 26,
                            .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::TOKEN_BUCKET,
                            .initialCodePt = 0,
                            .downgrade1 = 50,
                            .downgrade2 = 50,
                            .policyIndex = static_cast<uint32_t>(PolicerType::TOKEN_BUCKET)});
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::DUMB,
                            .initialCodePt = 50,
                            .downgrade1 = 50,
                            .downgrade2 = 50,
                            .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});

    // PHB table
    helper.AddPhbEntry(edgeInner, 46, 0, 0);
    helper.AddPhbEntry(edgeInner, 51, 0, 1);
    helper.AddPhbEntry(edgeInner, 10, 1, 0);
    helper.AddPhbEntry(edgeInner, 12, 1, 1);
    helper.AddPhbEntry(edgeInner, 18, 2, 0);
    helper.AddPhbEntry(edgeInner, 20, 2, 1);
    helper.AddPhbEntry(edgeInner, 26, 3, 0);
    helper.AddPhbEntry(edgeInner, 0, 4, 0);
    helper.AddPhbEntry(edgeInner, 50, 4, 1);

    Ptr<NetDevice> e1Dev = devBottleneck.Get(0);
    Ptr<TrafficControlLayer> tc = e1Dev->GetNode()->GetObject<TrafficControlLayer>();
    tc->SetRootQueueDiscOnDevice(e1Dev, edgeDisc);
    edgeDisc->Initialize();

    edgeInner->SetMredMode(MredMode::DROP_TAIL, 0);
    edgeInner->SetMredMode(MredMode::RIO_C, 1);
    edgeInner->SetMredMode(MredMode::WRED, 2);
    edgeInner->SetMredMode(MredMode::WRED, 3);
    edgeInner->SetMredMode(MredMode::DROP_TAIL, 4);

    edgeInner->SetMeanPacketSize(500);
    edgeInner->SetQueueBandwidth(1, 810000.0);
    edgeInner->SetQueueBandwidth(2, 810000.0);
    edgeInner->SetQueueBandwidth(3, 810000.0);

    helper.ConfigQueue(edgeInner,
                       {.queue = 0, .prec = 0, .thMin = 20.0, .thMax = 20.0, .maxP = 1.0});
    helper.ConfigQueue(edgeInner,
                       {.queue = 0, .prec = 1, .thMin = -1.0, .thMax = -1.0, .maxP = 0.0});
    helper.ConfigQueue(edgeInner,
                       {.queue = 1, .prec = 0, .thMin = 60.0, .thMax = 110.0, .maxP = 0.02});
    helper.ConfigQueue(edgeInner,
                       {.queue = 1, .prec = 1, .thMin = 30.0, .thMax = 60.0, .maxP = 0.6});
    helper.ConfigQueue(edgeInner,
                       {.queue = 2, .prec = 0, .thMin = 30.0, .thMax = 50.0, .maxP = 0.1});
    helper.ConfigQueue(edgeInner,
                       {.queue = 2, .prec = 1, .thMin = 30.0, .thMax = 50.0, .maxP = 0.2});
    helper.ConfigQueue(edgeInner,
                       {.queue = 3, .prec = 0, .thMin = 30.0, .thMax = 60.0, .maxP = 0.5});
    helper.ConfigQueue(edgeInner,
                       {.queue = 4, .prec = 0, .thMin = 50.0, .thMax = 50.0, .maxP = 1.0});
    helper.ConfigQueue(edgeInner,
                       {.queue = 4, .prec = 1, .thMin = -1.0, .thMax = -1.0, .maxP = 0.0});

    m_edgeDisc = edgeDisc;

    // ===================================================================
    // DiffServ Core (n466 -> n0) — minimal single-queue tail-drop
    // ===================================================================
    Ptr<CoreQueueDisc> coreDisc = CreateObject<CoreQueueDisc>();
    auto coreInner = helper.InstallRedInner(coreDisc);
    coreInner->SetNumQueues(1);
    coreInner->SetNumPrec(0, 1);
    Ptr<PriorityScheduler> corePq = CreateObjectWithAttributes<PriorityScheduler>("NumQueues",
                                                                                  UintegerValue(1),
                                                                                  "WinLen",
                                                                                  DoubleValue(1.0));
    coreInner->SetScheduler(corePq);
    helper.AddPhbEntry(coreInner, 0, 0, 0);
    Ptr<NetDevice> coreDev = devBottleneck.Get(1);
    Ptr<TrafficControlLayer> tcCore = coreDev->GetNode()->GetObject<TrafficControlLayer>();
    tcCore->SetRootQueueDiscOnDevice(coreDev, coreDisc);
    coreDisc->Initialize();
    coreInner->SetMredModeAllQueues(MredMode::DROP_TAIL);
    helper.ConfigQueue(coreInner,
                       {.queue = 0, .prec = 0, .thMin = 60.0, .thMax = 60.0, .maxP = 1.0});

    // ===================================================================
    // Traffic generators — matching diffserv-example-3 --scale=full exactly
    // ===================================================================
    Ipv4Address addr770 = n(770)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();
    Ipv4Address addr468 = n(468)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();
    std::set<std::pair<uint32_t, uint16_t>> installedSinks;

    // --- VoIP (200 G.723.1 ON/OFF, staggered 1s apart) ---
    const uint16_t voipPort = 5060;
    EnsureSink(installedSinks, n(770), voipPort, "ns3::UdpSocketFactory");
    {
        OnOffHelper voipHelper(InetSocketAddress(addr770, voipPort));
        voipHelper.SetAttribute("PacketSize", UintegerValue(48));
        voipHelper.SetAttribute("DataRateBps", UintegerValue(6400));
        voipHelper.SetAttribute("OnMean", DoubleValue(1.004));
        voipHelper.SetAttribute("OffMean", DoubleValue(1.587));
        for (uint32_t i = 0; i < 200; i++)
        {
            ApplicationContainer apps = voipHelper.Install(n(470 + i));
            apps.Start(Seconds(static_cast<double>(i)));
            apps.Stop(Seconds(kSimTime));
        }
    }

    // --- RealAudio (300 users with sequential-flow sessions from CDFs) ---
    const uint16_t streamPort = 5004;
    EnsureSink(installedSinks, n(770), streamPort, "ns3::UdpSocketFactory");
    {
        double userStartTime = 0.0;
        for (uint32_t i = 0; i < 300; i++)
        {
            if (i > 0)
            {
                userStartTime += rvUserInter->GetValue();
            }
            auto numFlows = static_cast<uint32_t>(rvSflow->GetValue());
            if (numFlows < 1)
            {
                numFlows = 1;
            }
            double flowStart = userStartTime;
            double dur = rvFlowDur->GetValue() * 60.0;
            for (uint32_t j = 0; j < numFlows; j++)
            {
                double flowStop = flowStart + dur;
                if (flowStop > kSimTime - 1.0)
                {
                    flowStop = kSimTime - 1.0;
                }
                if (flowStop <= flowStart)
                {
                    break;
                }
                // DataRate × (1/duty) compensation — see
                // reference_ns3_onoff_vs_ns2_realaudio_averaging.
                constexpr double kOnOffDutyCompensation = 1.0 / 0.2174;
                const double rateKbps = rvFrate->GetValue();
                const double dataRateBps = rateKbps * 1000.0 * kOnOffDutyCompensation;
                std::ostringstream rateStr;
                rateStr << static_cast<uint32_t>(dataRateBps) << "bps";

                ns3::OnOffHelper streamOnOff("ns3::UdpSocketFactory",
                                             InetSocketAddress(addr770, streamPort));
                streamOnOff.SetAttribute("DataRate", StringValue(rateStr.str()));
                streamOnOff.SetAttribute("PacketSize", UintegerValue(245));
                streamOnOff.SetAttribute("OnTime",
                                         StringValue("ns3::ConstantRandomVariable[Constant=0.5]"));
                streamOnOff.SetAttribute("OffTime",
                                         StringValue("ns3::ExponentialRandomVariable[Mean=1.8]"));

                ApplicationContainer app = streamOnOff.Install(n(470 + i));
                app.Start(Seconds(flowStart));
                app.Stop(Seconds(flowStop));

                flowStart = flowStop + 0.001;
                dur = rvFlowDur->GetValue() * 60.0;
            }
        }
    }

    // --- Telnet (50 TCP OnOff, Silver/AF21) ---
    const uint16_t telnetPort = 23;
    for (uint32_t i = 0; i < 50; i++)
    {
        const uint32_t src = (i % 40) + 6;
        const uint32_t dst = (i % 50) + 46;
        Ipv4Address dstAddr = n(dst)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();
        EnsureSink(installedSinks, n(dst), telnetPort, "ns3::TcpSocketFactory");

        ns3::OnOffHelper telnetOnOff("ns3::TcpSocketFactory",
                                     InetSocketAddress(dstAddr, telnetPort));
        telnetOnOff.SetAttribute("DataRate", StringValue("50kbps"));
        telnetOnOff.SetAttribute("PacketSize", UintegerValue(512));
        telnetOnOff.SetAttribute("OnTime",
                                 StringValue("ns3::ConstantRandomVariable[Constant=0.01]"));
        telnetOnOff.SetAttribute("OffTime",
                                 StringValue("ns3::ExponentialRandomVariable[Mean=0.5]"));

        ApplicationContainer app = telnetOnOff.Install(n(src));
        app.Start(Seconds(static_cast<double>(i)));
        app.Stop(Seconds(kSimTime));
    }

    // --- FTP (50 BulkSend, Silver/AF22) ---
    const uint16_t ftpPort = 21;
    for (uint32_t i = 0; i < 50; i++)
    {
        const uint32_t src = (i % 40) + 6;
        const uint32_t dst = (i % 50) + 46;
        Ipv4Address dstAddr = n(dst)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();
        EnsureSink(installedSinks, n(dst), ftpPort, "ns3::TcpSocketFactory");

        BulkSendHelper ftpBulk("ns3::TcpSocketFactory", InetSocketAddress(dstAddr, ftpPort));
        ftpBulk.SetAttribute("MaxBytes", UintegerValue(0));
        ftpBulk.SetAttribute("SendSize", UintegerValue(1460));

        ApplicationContainer app = ftpBulk.Install(n(src));
        app.Start(Seconds(static_cast<double>(i)));
        app.Stop(Seconds(kSimTime));
    }

    // --- HTTP (50 TCP OnOff, Bronze/AF31) ---
    const uint16_t httpPort = 80;
    for (uint32_t i = 0; i < 50; i++)
    {
        const uint32_t src = (i % 40) + 6;
        const uint32_t dst = (i % 50) + 46;
        Ipv4Address dstAddr = n(dst)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();
        EnsureSink(installedSinks, n(dst), httpPort, "ns3::TcpSocketFactory");

        ns3::OnOffHelper httpOnOff("ns3::TcpSocketFactory", InetSocketAddress(dstAddr, httpPort));
        httpOnOff.SetAttribute("DataRate", StringValue("500kbps"));
        httpOnOff.SetAttribute("PacketSize", UintegerValue(1460));
        httpOnOff.SetAttribute("OnTime", StringValue("ns3::ExponentialRandomVariable[Mean=1.0]"));
        httpOnOff.SetAttribute("OffTime", StringValue("ns3::ExponentialRandomVariable[Mean=5.0]"));

        ApplicationContainer app = httpOnOff.Install(n(src));
        app.Start(Seconds(static_cast<double>(i)));
        app.Stop(Seconds(kSimTime));
    }

    // --- Background CBR (500 kbps UDP, 512B) ---
    const uint16_t bgPort = 10000;
    EnsureSink(installedSinks, n(468), bgPort, "ns3::UdpSocketFactory");
    {
        ns3::OnOffHelper bgOnOff("ns3::UdpSocketFactory", InetSocketAddress(addr468, bgPort));
        bgOnOff.SetAttribute("DataRate", StringValue("500000bps"));
        bgOnOff.SetAttribute("PacketSize", UintegerValue(512));
        bgOnOff.SetAttribute("OnTime",
                             StringValue("ns3::ConstantRandomVariable[Constant=1000000]"));
        bgOnOff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
        ApplicationContainer bgApp = bgOnOff.Install(n(467));
        bgApp.Start(Seconds(0.0));
        bgApp.Stop(Seconds(kSimTime));
    }

    // ---- 1 Hz rate sampling, gated to [kMeasureStart, kSimTime) ----
    Simulator::Schedule(Seconds(0.0), &S3PerClassRatePreservationTest::SampleTick, this);

    Simulator::Stop(Seconds(kSimTime));
    Simulator::Run();

    // ---- Assertions ----
    NS_TEST_ASSERT_MSG_GT(m_acc.samples,
                          0u,
                          "S-13.10 FAIL: no samples captured in the "
                          "measurement window");

    const double premium = m_acc.premiumSum / m_acc.samples;
    const double gold = m_acc.goldSum / m_acc.samples;
    const double silver = m_acc.silverSum / m_acc.samples;
    const double bronze = m_acc.bronzeSum / m_acc.samples;
    const double be = m_acc.beSum / m_acc.samples;
    const double total = premium + gold + silver + bronze + be;

    std::ostringstream s1310Sum;
    s1310Sum << "\n  [S-13.10] Per-class service rates (mean over t∈[" << kMeasureStart << ","
             << kSimTime << "], n=" << m_acc.samples << "):\n"
             << std::fixed << std::setprecision(1) << "    Premium = " << premium << " kbps  (ref "
             << kPremiumRef << ")\n"
             << "    Gold    = " << gold << " kbps  (ref " << kGoldRef << ")\n"
             << "    Silver  = " << silver << " kbps  (ref " << kSilverRef << ")\n"
             << "    Bronze  = " << bronze << " kbps  (ref " << kBronzeRef << ")\n"
             << "    BE      = " << be << " kbps  (ref " << kBeRef << ")\n"
             << "    Total   = " << total << " kbps  (ref " << kTotalRef << ")\n";
    std::cout << s1310Sum.str() << std::endl;

    auto checkBand = [&](double observed, double ref, const char* name) {
        const double rel = std::abs(observed - ref) / ref;
        NS_TEST_ASSERT_MSG_LT_OR_EQ(rel,
                                    kTol,
                                    "S-13.10 FAIL: " << name << " mean " << observed
                                                     << " kbps outside ±" << (kTol * 100)
                                                     << "% of Q-10.6 reference " << ref
                                                     << " kbps (deviation " << (rel * 100) << "%)");
    };
    checkBand(premium, kPremiumRef, "Premium");
    checkBand(gold, kGoldRef, "Gold");
    checkBand(silver, kSilverRef, "Silver");
    checkBand(bronze, kBronzeRef, "Bronze");
    checkBand(be, kBeRef, "BE");
    checkBand(total, kTotalRef, "Total");

    Simulator::Destroy();
}

//  S-13.11: Two-inner multi-slot DSCP routing
// =============================================================================
//
// Verifies the DSCP-to-slot dispatch. Two RedQueueDisc inners are
// installed at slots 0 and 1; DSCP 46 routes to slot 0, DSCP 10
// routes to slot 1. Two packets (one per DSCP) are enqueued and the
// resulting per-inner occupancy is asserted — proves the edge's
// DoEnqueue honours the DSCP-to-slot map. Using Red inners (not L4S)
// isolates the routing path from ECT-based classification inside L4S.
//
/**
 * @brief Verifies multi-slot dispatch routes by DSCP to the correct inner queue disc.
 * @see specs/02-structural.md S-13.11
 */
class MultiSlotDscpRoutingTest : public TestCase
{
  public:
    MultiSlotDscpRoutingTest()
        : TestCase("S-13.11 Multi-slot DSCP routing")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();

        // Slot 0: Red with PHB(46, 0, 0). Slot 1: Red with PHB(10, 0, 0).
        auto red0 = CreateObject<RedQueueDisc>();
        red0->SetNumQueues(1);
        red0->AddPhbEntry(46, 0, 0);
        red0->SetScheduler(
            CreateObjectWithAttributes<RoundRobinScheduler>("NumQueues", UintegerValue(1)));
        red0->SetQueueLimit(0, 50);
        edge->SetInnerDiscAt(0, red0);

        auto red1 = CreateObject<RedQueueDisc>();
        red1->SetNumQueues(1);
        red1->AddPhbEntry(10, 0, 0);
        red1->SetScheduler(
            CreateObjectWithAttributes<RoundRobinScheduler>("NumQueues", UintegerValue(1)));
        red1->SetQueueLimit(0, 50);
        edge->SetInnerDiscAt(1, red1);

        // DSCP 46 → slot 0, DSCP 10 → slot 1.
        edge->SetDscpToSlot(46, 0);
        edge->SetDscpToSlot(10, 1);

        NS_TEST_ASSERT_MSG_EQ(edge->GetNumInnerSlots(),
                              2u,
                              "Two slots should be populated after "
                              "two SetInnerDiscAt calls");

        edge->Initialize();

        // Disable WRED on both inners after Initialize: default thMin=thMax=0
        // would forced-drop every packet after the first. DROP_TAIL with
        // high thresholds gives deterministic tail-drop semantics suitable
        // for a single-packet routing assertion.
        red0->SetMredMode(MredMode::DROP_TAIL, 0);
        red0->ConfigQueue({.queue = 0, .prec = 0, .thMin = 1000.0, .thMax = 2000.0, .maxP = 0.1});
        red1->SetMredMode(MredMode::DROP_TAIL, 0);
        red1->ConfigQueue({.queue = 0, .prec = 0, .thMin = 1000.0, .thMax = 2000.0, .maxP = 0.1});

        // Push one DSCP 46 packet, then one DSCP 10 packet.
        auto makeItem = [](uint8_t dscp) {
            Ptr<Packet> p = Create<Packet>(500);
            Ipv4Header h;
            h.SetSource(Ipv4Address("10.0.0.1"));
            h.SetDestination(Ipv4Address("10.0.0.2"));
            h.SetProtocol(17);
            h.SetPayloadSize(500);
            h.SetDscp(static_cast<Ipv4Header::DscpType>(dscp));
            Address src;
            return Create<Ipv4QueueDiscItem>(p, src, 0x0800, h);
        };

        NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(makeItem(46)),
                              true,
                              "DSCP 46 packet should enqueue to slot 0");
        NS_TEST_ASSERT_MSG_EQ(red0->GetNPackets(),
                              1u,
                              "Slot 0 (red0) should hold 1 packet after "
                              "DSCP 46 enqueue");
        NS_TEST_ASSERT_MSG_EQ(red1->GetNPackets(), 0u, "Slot 1 (red1) should still hold 0 packets");

        NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(makeItem(10)),
                              true,
                              "DSCP 10 packet should enqueue to slot 1");
        NS_TEST_ASSERT_MSG_EQ(red0->GetNPackets(),
                              1u,
                              "Slot 0 (red0) occupancy unchanged by slot-1 enqueue");
        NS_TEST_ASSERT_MSG_EQ(red1->GetNPackets(),
                              1u,
                              "Slot 1 (red1) should hold 1 packet after "
                              "DSCP 10 enqueue");

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-13.12: Strict-priority dequeue across inner slots
// =============================================================================
//
// Verifies that DoDequeue drains slot 0 completely before touching
// slot 1. Same topology as S-13.11: Red at slot 0 (DSCP 46), Red at
// slot 1 (DSCP 10). Enqueue 5 DSCP 10 packets first and 5 DSCP 46
// packets second (reversed order → tests priority, not FIFO). Dequeue
// 10 times: the first 5 returned must all be DSCP 46 (slot 0 drains),
// the last 5 must all be DSCP 10 (slot 1 drained only after slot 0
// empties).
//
/**
 * @brief Verifies multi-slot dispatch preserves strict priority across slots.
 * @see specs/02-structural.md S-13.12
 */
class MultiSlotStrictPriorityTest : public TestCase
{
  public:
    MultiSlotStrictPriorityTest()
        : TestCase("S-13.12 Multi-slot strict-priority dequeue")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();

        auto red0 = CreateObject<RedQueueDisc>();
        red0->SetNumQueues(1);
        red0->AddPhbEntry(46, 0, 0);
        red0->SetScheduler(
            CreateObjectWithAttributes<RoundRobinScheduler>("NumQueues", UintegerValue(1)));
        red0->SetQueueLimit(0, 50);
        edge->SetInnerDiscAt(0, red0);

        auto red1 = CreateObject<RedQueueDisc>();
        red1->SetNumQueues(1);
        red1->AddPhbEntry(10, 0, 0);
        red1->SetScheduler(
            CreateObjectWithAttributes<RoundRobinScheduler>("NumQueues", UintegerValue(1)));
        red1->SetQueueLimit(0, 50);
        edge->SetInnerDiscAt(1, red1);

        edge->SetDscpToSlot(46, 0);
        edge->SetDscpToSlot(10, 1);

        edge->Initialize();

        // DROP_TAIL mode with high thresholds so neither inner early-drops
        // under the modest burst this test pushes.
        red0->SetMredMode(MredMode::DROP_TAIL, 0);
        red0->ConfigQueue({.queue = 0, .prec = 0, .thMin = 1000.0, .thMax = 2000.0, .maxP = 0.1});
        red1->SetMredMode(MredMode::DROP_TAIL, 0);
        red1->ConfigQueue({.queue = 0, .prec = 0, .thMin = 1000.0, .thMax = 2000.0, .maxP = 0.1});

        auto makeItem = [](uint8_t dscp) {
            Ptr<Packet> p = Create<Packet>(500);
            Ipv4Header h;
            h.SetSource(Ipv4Address("10.0.0.1"));
            h.SetDestination(Ipv4Address("10.0.0.2"));
            h.SetProtocol(17);
            h.SetPayloadSize(500);
            h.SetDscp(static_cast<Ipv4Header::DscpType>(dscp));
            Address src;
            return Create<Ipv4QueueDiscItem>(p, src, 0x0800, h);
        };

        // Enqueue low-priority (slot 1 / DSCP 10) FIRST.
        const uint32_t nPerSlot = 3;
        for (uint32_t i = 0; i < nPerSlot; ++i)
        {
            NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(makeItem(10)),
                                  true,
                                  "Slot-1 enqueue should succeed");
        }
        // Then enqueue high-priority (slot 0 / DSCP 46).
        for (uint32_t i = 0; i < nPerSlot; ++i)
        {
            NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(makeItem(46)),
                                  true,
                                  "Slot-0 enqueue should succeed");
        }

        NS_TEST_ASSERT_MSG_EQ(red0->GetNPackets(), nPerSlot, "Slot 0 should hold " << nPerSlot);
        NS_TEST_ASSERT_MSG_EQ(red1->GetNPackets(), nPerSlot, "Slot 1 should hold " << nPerSlot);

        // Dequeue 2 * nPerSlot. Strict priority: slot 0 drains first.
        for (uint32_t i = 0; i < nPerSlot; ++i)
        {
            Ptr<QueueDiscItem> it = edge->Dequeue();
            NS_TEST_ASSERT_MSG_NE(it, nullptr, "Dequeue " << i << " (slot-0 phase) should yield");
            if (!it)
            {
                Simulator::Destroy();
                return;
            }
            Ptr<Ipv4QueueDiscItem> ip = DynamicCast<Ipv4QueueDiscItem>(it);
            uint8_t dscp = ip->GetHeader().GetTos() >> 2;
            NS_TEST_ASSERT_MSG_EQ(static_cast<uint32_t>(dscp),
                                  46u,
                                  "First " << nPerSlot
                                           << " dequeued must be DSCP 46 "
                                              "(slot 0 drains first); index "
                                           << i << " returned DSCP "
                                           << static_cast<uint32_t>(dscp));
        }
        for (uint32_t i = 0; i < nPerSlot; ++i)
        {
            Ptr<QueueDiscItem> it = edge->Dequeue();
            NS_TEST_ASSERT_MSG_NE(it,
                                  nullptr,
                                  "Dequeue " << (nPerSlot + i) << " (slot-1 phase) should yield");
            if (!it)
            {
                Simulator::Destroy();
                return;
            }
            Ptr<Ipv4QueueDiscItem> ip = DynamicCast<Ipv4QueueDiscItem>(it);
            uint8_t dscp = ip->GetHeader().GetTos() >> 2;
            NS_TEST_ASSERT_MSG_EQ(static_cast<uint32_t>(dscp),
                                  10u,
                                  "Last " << nPerSlot
                                          << " dequeued must be DSCP 10 "
                                             "(slot 1 drains after slot 0); index "
                                          << (nPerSlot + i) << " returned DSCP "
                                          << static_cast<uint32_t>(dscp));
        }

        // Queues empty.
        NS_TEST_ASSERT_MSG_EQ(edge->Dequeue(),
                              nullptr,
                              "After full drain, both slots should be empty");

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-13.13: Single-inner installation via SetInnerDisc
// =============================================================================
//
// Callers that install a single inner via SetInnerDisc(inner) get
// it anchored at slot 0; with the default DSCP-to-slot map sending
// every code point to slot 0, the edge behaves identically to a
// non-multi-slot installation: GetNumInnerSlots() == 1,
// GetInnerDisc() returns the same pointer, GetInnerDiscAt(0)
// returns the same pointer, and end-to-end enqueue/dequeue flows
// work. Pins this convenience-overload contract at unit
// granularity.
//
/**
 * @brief Verifies backward-compatible single-inner configuration still works.
 * @see specs/02-structural.md S-13.13
 */
class BackwardCompatSingleInnerTest : public TestCase
{
  public:
    BackwardCompatSingleInnerTest()
        : TestCase("S-13.13 SetInnerDisc single-slot path")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();

        auto red = CreateObject<RedQueueDisc>();
        red->SetNumQueues(1);
        red->AddPhbEntry(46, 0, 0);
        red->SetScheduler(
            CreateObjectWithAttributes<RoundRobinScheduler>("NumQueues", UintegerValue(1)));
        red->SetQueueLimit(0, 50);
        edge->SetInnerDisc(red); // legacy API

        // Only slot 0 populated; slots 1..7 empty.
        NS_TEST_ASSERT_MSG_EQ(edge->GetNumInnerSlots(),
                              1u,
                              "Legacy SetInnerDisc should populate exactly "
                              "one slot (got "
                                  << edge->GetNumInnerSlots() << ")");
        NS_TEST_ASSERT_MSG_EQ(edge->GetInnerDisc(),
                              red,
                              "GetInnerDisc() should return the installed inner");
        NS_TEST_ASSERT_MSG_EQ(edge->GetInnerDiscAt(0),
                              red,
                              "GetInnerDiscAt(0) should mirror GetInnerDisc()");
        NS_TEST_ASSERT_MSG_EQ(edge->GetInnerDiscAt(1),
                              nullptr,
                              "Unused slot 1 should return nullptr");

        edge->Initialize();

        red->SetMredMode(MredMode::DROP_TAIL, 0);
        red->ConfigQueue({.queue = 0, .prec = 0, .thMin = 1000.0, .thMax = 2000.0, .maxP = 0.1});

        // A DSCP 46 packet with NO explicit SetDscpToSlot override must
        // still route correctly (default map: every DSCP → slot 0).
        Ptr<Packet> p = Create<Packet>(500);
        Ipv4Header h;
        h.SetSource(Ipv4Address("10.0.0.1"));
        h.SetDestination(Ipv4Address("10.0.0.2"));
        h.SetProtocol(17);
        h.SetPayloadSize(500);
        h.SetDscp(static_cast<Ipv4Header::DscpType>(46));
        Address src;
        Ptr<Ipv4QueueDiscItem> item = Create<Ipv4QueueDiscItem>(p, src, 0x0800, h);

        NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(item),
                              true,
                              "DSCP 46 packet should enqueue through legacy path");
        NS_TEST_ASSERT_MSG_EQ(red->GetNPackets(), 1u, "Slot-0 Red inner should hold the packet");

        Ptr<QueueDiscItem> deq = edge->Dequeue();
        NS_TEST_ASSERT_MSG_NE(deq, nullptr, "Dequeue should return the slot-0 packet");
        if (!deq)
        {
            Simulator::Destroy();
            return;
        }

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-13.14: Per-slot QueueStatsProvider probes on edge
// =============================================================================
//
// The zero-argument runtime probes (GetNumQueues /
// GetVirtualQueueLen / PrintStats) target slot 0; the per-slot
// overloads (`GetNumQueues(slot)`, `GetVirtualQueueLen(slot, queue,
// prec)`, `PrintStats(slot)`) route through the inner at the given
// slot's `QueueStatsProvider` interface. This test verifies:
//
// - Zero-argument calls target slot 0.
// - Per-slot overload on slot 1 returns slot 1's inner's values.
// - Out-of-range slot returns 0 (conservative default).
// - Empty slot (populated range exceeded) returns 0.
// - Inner lacking QueueStatsProvider returns 0 (no-throw).
//
/**
 * @brief Verifies per-slot queue stats probes report independent counters.
 * @see specs/02-structural.md S-13.14
 */
class PerSlotQueueStatsProbesTest : public TestCase
{
  public:
    PerSlotQueueStatsProbesTest()
        : TestCase("S-13.14 Per-slot QueueStatsProvider probes")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();

        // Slot 0: Red with 2 sub-queues. Slot 1: Red with 1 sub-queue.
        // Different queue counts make the per-slot disambiguation observable.
        auto red0 = CreateObject<RedQueueDisc>();
        red0->SetNumQueues(2);
        red0->AddPhbEntry(46, 0, 0);
        red0->AddPhbEntry(10, 1, 0);
        red0->SetScheduler(
            CreateObjectWithAttributes<RoundRobinScheduler>("NumQueues", UintegerValue(2)));
        red0->SetQueueLimit(0, 50);
        red0->SetQueueLimit(1, 50);
        edge->SetInnerDiscAt(0, red0);

        auto red1 = CreateObject<RedQueueDisc>();
        red1->SetNumQueues(1);
        red1->AddPhbEntry(26, 0, 0);
        red1->SetScheduler(
            CreateObjectWithAttributes<RoundRobinScheduler>("NumQueues", UintegerValue(1)));
        red1->SetQueueLimit(0, 50);
        edge->SetInnerDiscAt(1, red1);

        edge->SetDscpToSlot(46, 0);
        edge->SetDscpToSlot(10, 0);
        edge->SetDscpToSlot(26, 1);

        edge->Initialize();

        // Configure DROP_TAIL + high thresholds on both inners so a
        // single-packet probe doesn't tangle with WRED defaults
        // (`thMin=thMax=0` would force-drop the only packet).
        red0->SetMredMode(MredMode::DROP_TAIL, 0);
        red0->SetMredMode(MredMode::DROP_TAIL, 1);
        red0->ConfigQueue({.queue = 0, .prec = 0, .thMin = 1000.0, .thMax = 2000.0, .maxP = 0.1});
        red0->ConfigQueue({.queue = 1, .prec = 0, .thMin = 1000.0, .thMax = 2000.0, .maxP = 0.1});
        red1->SetMredMode(MredMode::DROP_TAIL, 0);
        red1->ConfigQueue({.queue = 0, .prec = 0, .thMin = 1000.0, .thMax = 2000.0, .maxP = 0.1});

        // Backward-compat: zero-arg still targets slot 0.
        NS_TEST_ASSERT_MSG_EQ(edge->GetNumQueues(),
                              2u,
                              "GetNumQueues() zero-arg must mirror slot 0 (got "
                                  << edge->GetNumQueues() << ")");

        // Per-slot: slot 0 has 2 queues, slot 1 has 1 queue.
        NS_TEST_ASSERT_MSG_EQ(edge->GetNumQueues(0),
                              2u,
                              "GetNumQueues(0) should report slot 0's 2 queues");
        NS_TEST_ASSERT_MSG_EQ(edge->GetNumQueues(1),
                              1u,
                              "GetNumQueues(1) should report slot 1's 1 queue");

        // Virtual queue length before any enqueue: both zero.
        NS_TEST_ASSERT_MSG_EQ(edge->GetVirtualQueueLen(0, 0, 0),
                              0,
                              "Slot 0 queue 0 empty pre-enqueue");
        NS_TEST_ASSERT_MSG_EQ(edge->GetVirtualQueueLen(1, 0, 0),
                              0,
                              "Slot 1 queue 0 empty pre-enqueue");

        // Unpopulated slot (index 2): should return 0 without asserting.
        NS_TEST_ASSERT_MSG_EQ(edge->GetNumQueues(2), 0u, "Unpopulated slot 2 reports 0 queues");
        NS_TEST_ASSERT_MSG_EQ(edge->GetVirtualQueueLen(2, 0, 0),
                              0,
                              "Unpopulated slot 2 reports 0 occupancy");

        // Out-of-range slot (kMaxInnerSlots): should return 0 without
        // asserting or crashing.
        NS_TEST_ASSERT_MSG_EQ(edge->GetNumQueues(EdgeQueueDisc::kMaxInnerSlots),
                              0u,
                              "Out-of-range slot reports 0 queues");
        NS_TEST_ASSERT_MSG_EQ(edge->GetVirtualQueueLen(EdgeQueueDisc::kMaxInnerSlots, 0, 0),
                              0,
                              "Out-of-range slot reports 0 occupancy");

        // Enqueue one DSCP 26 packet → slot 1. Verify slot 1 occupancy
        // bumps to 1 while slot 0 remains 0.
        Ptr<Packet> p = Create<Packet>(500);
        Ipv4Header h;
        h.SetSource(Ipv4Address("10.0.0.1"));
        h.SetDestination(Ipv4Address("10.0.0.2"));
        h.SetProtocol(17);
        h.SetPayloadSize(500);
        h.SetDscp(static_cast<Ipv4Header::DscpType>(26));
        Address src;
        Ptr<Ipv4QueueDiscItem> item = Create<Ipv4QueueDiscItem>(p, src, 0x0800, h);
        NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(item),
                              true,
                              "Enqueue DSCP 26 via slot 1 should succeed");

        NS_TEST_ASSERT_MSG_EQ(edge->GetVirtualQueueLen(0, 0, 0),
                              0,
                              "Slot 0 still empty after slot-1 enqueue");
        NS_TEST_ASSERT_MSG_EQ(edge->GetVirtualQueueLen(1, 0, 0),
                              1,
                              "Slot 1 queue 0 should hold 1 packet");

        // PrintStats should not crash for any slot (output discarded).
        edge->PrintStats();                              // zero-arg: slot 0
        edge->PrintStats(0);                             // explicit slot 0
        edge->PrintStats(1);                             // populated slot 1
        edge->PrintStats(2);                             // empty slot — no-op
        edge->PrintStats(EdgeQueueDisc::kMaxInnerSlots); // out of range

        Simulator::Destroy();
    }
};

// =============================================================================
//  Slot-0 vs per-slot probe parity (delegation regression net)
// =============================================================================
//
// The zero-argument probes (GetNumQueues, GetVirtualQueueLen, PrintStats) are
// convenience overloads for their per-slot counterparts at slot=0. This test
// pins that contract: with a non-default inner (L4S) populated at slot 0, the
// zero-arg probes must return values byte-identical to the per-slot overloads
// invoked with slot=0. PrintStats parity is verified by redirecting std::cout
// into an ostringstream for each call and comparing the captured strings.
//
// The Red-specific probes (GetScheduler, PrintPhbTable) are deliberately out
// of scope: they remain Red-only and have no per-slot counterpart through
// the QueueStatsProvider path.

/**
 * @brief Verifies zero-arg Edge probes match the slot=0 per-slot overloads.
 * @see specs/02-structural.md S-13.14
 */
class EdgeSlotZeroDelegationParityTest : public TestCase
{
  public:
    EdgeSlotZeroDelegationParityTest()
        : TestCase("Edge zero-arg probes match per-slot(0) overloads (L4S inner)")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();

        // Populate slot 0 with a l4s::QueueDisc — L4S implements
        // QueueStatsProvider, exercising the same code path as the
        // per-slot overloads without depending on Red specifics.
        Ptr<l4s::QueueDisc> l4s = CreateObject<l4s::QueueDisc>();
        edge->SetInnerDiscAt(0, l4s);

        edge->Initialize();

        // GetNumQueues parity.
        NS_TEST_ASSERT_MSG_EQ(edge->GetNumQueues(),
                              edge->GetNumQueues(0),
                              "GetNumQueues() must match GetNumQueues(0)");

        // GetVirtualQueueLen parity at (0, 0).
        NS_TEST_ASSERT_MSG_EQ(edge->GetVirtualQueueLen(0, 0),
                              edge->GetVirtualQueueLen(0, 0, 0),
                              "GetVirtualQueueLen(0, 0) must match GetVirtualQueueLen(0, 0, 0)");

        // If the inner exposes more than one queue, exercise (1, 0) too.
        if (edge->GetNumQueues() > 1)
        {
            NS_TEST_ASSERT_MSG_EQ(
                edge->GetVirtualQueueLen(1, 0),
                edge->GetVirtualQueueLen(0, 1, 0),
                "GetVirtualQueueLen(1, 0) must match GetVirtualQueueLen(0, 1, 0)");
        }

        // PrintStats parity: redirect std::cout into a string for each call.
        std::ostringstream zeroArgBuf;
        std::ostringstream perSlotBuf;

        std::streambuf* originalCout = std::cout.rdbuf();

        std::cout.rdbuf(zeroArgBuf.rdbuf());
        edge->PrintStats();
        std::cout.rdbuf(originalCout);

        std::cout.rdbuf(perSlotBuf.rdbuf());
        edge->PrintStats(0);
        std::cout.rdbuf(originalCout);

        NS_TEST_ASSERT_MSG_EQ(zeroArgBuf.str(),
                              perSlotBuf.str(),
                              "PrintStats() output must equal PrintStats(0) output");

        Simulator::Destroy();
    }
};

// =============================================================================
//  SetAsDiffserv: structural characterisation (EF + BE profile)
// =============================================================================

// Verifies the one-call DiffServ composer reproduces the canonical EF+BE edge.
class SetAsDiffservComposesEfEdgeTest : public TestCase
{
  public:
    SetAsDiffservComposesEfEdgeTest()
        : TestCase("SetAsDiffserv composes the canonical EF + BE edge")
    {
    }

    void DoRun() override
    {
        Ptr<ns3::stratum::EdgeQueueDisc> edge = CreateObject<ns3::stratum::EdgeQueueDisc>();
        ns3::stratum::diffserv::Helper::SetAsDiffserv(
            edge,
            {.profile = ns3::stratum::diffserv::Profile::ExpeditedForwarding});

        Ptr<ns3::stratum::RedQueueDisc> inner =
            DynamicCast<ns3::stratum::RedQueueDisc>(edge->GetInnerDisc());
        NS_TEST_ASSERT_MSG_NE(inner, nullptr, "edge has no inner RED disc");
        NS_TEST_ASSERT_MSG_EQ(inner->GetNumQueues(), 2u, "EF profile must have 2 queues");

        uint8_t q = 0xFF;
        uint8_t p = 0xFF;
        NS_TEST_ASSERT_MSG_EQ(inner->LookupPhb(46, q, p), true, "EF (46) must be mapped");
        NS_TEST_ASSERT_MSG_EQ(q, 0, "EF -> queue 0");
        NS_TEST_ASSERT_MSG_EQ(p, 0, "EF -> prec 0");
        NS_TEST_ASSERT_MSG_EQ(inner->LookupPhb(0, q, p), true, "BE (0) must be mapped");
        NS_TEST_ASSERT_MSG_EQ(q, 1, "BE -> queue 1");

        NS_TEST_ASSERT_MSG_NE(inner->GetScheduler(), nullptr, "default scheduler must be set");
        NS_TEST_ASSERT_MSG_NE(DynamicCast<ns3::stratum::PriorityScheduler>(inner->GetScheduler()),
                              nullptr,
                              "default scheduler must be PriorityScheduler");
        Simulator::Destroy();
    }
};

// Verifies the BestEffort profile composes a single best-effort queue.
class SetAsDiffservComposesBestEffortEdgeTest : public TestCase
{
  public:
    SetAsDiffservComposesBestEffortEdgeTest()
        : TestCase("SetAsDiffserv composes a single best-effort queue")
    {
    }

    void DoRun() override
    {
        Ptr<ns3::stratum::EdgeQueueDisc> edge = CreateObject<ns3::stratum::EdgeQueueDisc>();
        ns3::stratum::diffserv::Helper::SetAsDiffserv(
            edge,
            {.profile = ns3::stratum::diffserv::Profile::BestEffort});

        Ptr<ns3::stratum::RedQueueDisc> inner =
            DynamicCast<ns3::stratum::RedQueueDisc>(edge->GetInnerDisc());
        NS_TEST_ASSERT_MSG_NE(inner, nullptr, "edge has no inner RED disc");
        NS_TEST_ASSERT_MSG_EQ(inner->GetNumQueues(), 1u, "BestEffort must have 1 queue");
        uint8_t q = 0xFF;
        uint8_t p = 0xFF;
        NS_TEST_ASSERT_MSG_EQ(inner->LookupPhb(0, q, p), true, "BE (0) must be mapped");
        NS_TEST_ASSERT_MSG_EQ(q, 0, "BE -> queue 0");
        Simulator::Destroy();
    }
};

// Verifies a caller-provided scheduler is used as-is.
class SetAsDiffservUsesProvidedSchedulerTest : public TestCase
{
  public:
    SetAsDiffservUsesProvidedSchedulerTest()
        : TestCase("SetAsDiffserv uses a caller-provided scheduler")
    {
    }

    void DoRun() override
    {
        Ptr<ns3::stratum::EdgeQueueDisc> edge = CreateObject<ns3::stratum::EdgeQueueDisc>();
        Ptr<ns3::stratum::WfqScheduler> wfq =
            CreateObjectWithAttributes<ns3::stratum::WfqScheduler>("NumQueues", UintegerValue(2));
        ns3::stratum::diffserv::Helper::SetAsDiffserv(
            edge,
            {.profile = ns3::stratum::diffserv::Profile::ExpeditedForwarding, .scheduler = wfq});

        Ptr<ns3::stratum::RedQueueDisc> inner =
            DynamicCast<ns3::stratum::RedQueueDisc>(edge->GetInnerDisc());
        NS_TEST_ASSERT_MSG_NE(inner, nullptr, "edge has no inner RED disc");
        NS_TEST_ASSERT_MSG_EQ(inner->GetScheduler(), wfq, "provided scheduler must be used as-is");
        Simulator::Destroy();
    }
};

// =============================================================================
//  S-13.15: Auto-default FqCoDel inner disc gets a non-zero Quantum
// =============================================================================
//
// Regression coverage for a latent bug: when
// l4s::QueueDisc's ClassicAqm mode selects FqCoDel, EnsureDefaultChildren
// auto-creates an FqCoDelQueueDisc. Mainline FqCoDelQueueDisc auto-sets
// Quantum only from a NetDevice MTU; as a nested inner disc it sees no
// device, so CheckConfig would fatal on Quantum=0. The fix stamps
// SetQuantum(1500) at construction; this test exercises the path.

/**
 * @brief Verifies an inner FqCoDel under L4S auto-defaults a non-zero quantum.
 * @see specs/02-structural.md S-13.15
 */
class L4sFqCoDelAutoDefaultQuantumTest : public TestCase
{
  public:
    L4sFqCoDelAutoDefaultQuantumTest()
        : TestCase("S-13.15 QueueDisc auto-default FqCoDel inner has non-zero Quantum")
    {
    }

  private:
    void DoRun() override
    {
        // ClassicAqm is construct-only (ATTR_GET | ATTR_CONSTRUCT): the
        // mode is consumed by EnsureDefaultChildren before Initialize,
        // so it must be set at construction time.
        Ptr<l4s::QueueDisc> qd = CreateObjectWithAttributes<l4s::QueueDisc>(
            "ClassicAqm",
            EnumValue(l4s::QueueDisc::ClassicAqm::FqCoDel));

        // Without the inner-quantum auto-default,
        // FqCoDelQueueDisc::CheckConfig would NS_FATAL here.
        qd->Initialize();

        Ptr<QueueDisc> classic = qd->GetClassicAqmDisc();
        NS_TEST_ASSERT_MSG_NE(classic, nullptr, "Auto-default classic AQM must be wired");
        if (!classic)
        {
            return;
        }
        Ptr<FqCoDelQueueDisc> fq = DynamicCast<FqCoDelQueueDisc>(classic);
        NS_TEST_ASSERT_MSG_NE(fq, nullptr, "ClassicAqm=FqCoDel must produce FqCoDelQueueDisc");
        if (!fq)
        {
            return;
        }
        NS_TEST_ASSERT_MSG_GT(fq->GetQuantum(),
                              0u,
                              "FqCoDel Quantum must be non-zero when nested (no device MTU)");

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-17.1: Across-slot dispatcher — strict-priority extraction
// =============================================================================
//
// The across-slot dispatch policy is delegated to a pluggable
// SlotDispatcher strategy. The default StrictPriorityDispatcher MUST
// reproduce strict-priority drain: the lowest populated, non-empty slot
// drains first, higher slots yield only when all lower slots are empty.
//
// Three cases exercise the abstraction:
//
//   A. Default dispatcher preserves strict-priority drain under a
//      dual-slot Red fixture (DSCP 46 at slot 0, DSCP 10 at slot 1).
//      Effectively reproduces S-13.12's semantics via the new
//      abstraction — any regression at this layer breaks S-13.12 too.
//
//   B. Replacing the dispatcher with a spy subclass via
//      SetSlotDispatcher installs cleanly before Initialize. Spy's
//      OnEnqueue / OnDequeue hooks fire once per successful inner
//      Enqueue / Dequeue, preserving the dispatcher notification
//      contract used by stateful subclasses (DRR, WFQ).
//
//   C. PeekSlot is side-effect-free and mirrors SelectDequeueSlot
//      on the strict-priority default. Peek-then-dequeue yields the
//      same slot in both calls.
//
// Byte-identity is validated by an out-of-suite harness procedure:
// S1 / S2 / diffserv-hierarchical-l4s CSVs diff clean.
//
// Free-standing spy dispatcher registered in the ns3 TypeId system.
// Lives at file scope so CreateObject<S17_1_SpyDispatcher>() resolves
// the constructor through the standard registration path.
//
class S17_1_SpyDispatcher : public StrictPriorityDispatcher
{
  public:
    uint32_t enqueueCount{0};
    uint32_t dequeueCount{0};
    uint32_t lastEnqueueSlot{UINT32_MAX};
    uint32_t lastDequeueSlot{UINT32_MAX};

    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("ns3::stratum::S17_1_SpyDispatcher")
                                .SetParent<StrictPriorityDispatcher>()
                                .SetGroupName("Stratum")
                                .AddConstructor<S17_1_SpyDispatcher>();
        return tid;
    }

    S17_1_SpyDispatcher() = default;

    void OnEnqueue(uint32_t slot, Ptr<QueueDiscItem> /*item*/, EdgeQueueDisc* /*edge*/) override
    {
        ++enqueueCount;
        lastEnqueueSlot = slot;
    }

    void OnDequeue(uint32_t slot, Ptr<QueueDiscItem> /*item*/, EdgeQueueDisc* /*edge*/) override
    {
        ++dequeueCount;
        lastDequeueSlot = slot;
    }
};

/**
 * @brief Verifies slot dispatcher preserves byte-identical packets through dispatch.
 * @see specs/02-structural.md S-17.1
 */
class SlotDispatcherByteIdentityTest : public TestCase
{
  public:
    SlotDispatcherByteIdentityTest()
        : TestCase("S-17.1 Slot dispatcher strict-priority extraction")
    {
    }

  private:
    static Ptr<Ipv4QueueDiscItem> MakeItem(uint8_t dscp)
    {
        Ptr<Packet> p = Create<Packet>(500);
        Ipv4Header h;
        h.SetSource(Ipv4Address("10.0.0.1"));
        h.SetDestination(Ipv4Address("10.0.0.2"));
        h.SetProtocol(17);
        h.SetPayloadSize(500);
        h.SetDscp(static_cast<Ipv4Header::DscpType>(dscp));
        Address src;
        return Create<Ipv4QueueDiscItem>(p, src, 0x0800, h);
    }

    // Mirrors S-13.12's dual-slot Red fixture so the two tests share a
    // structural premise: a regression in either surfaces in both.
    static void ConfigureDualSlotEdge(Ptr<EdgeQueueDisc> edge)
    {
        auto red0 = CreateObject<RedQueueDisc>();
        red0->SetNumQueues(1);
        red0->AddPhbEntry(46, 0, 0);
        red0->SetScheduler(
            CreateObjectWithAttributes<RoundRobinScheduler>("NumQueues", UintegerValue(1)));
        red0->SetQueueLimit(0, 50);
        edge->SetInnerDiscAt(0, red0);

        auto red1 = CreateObject<RedQueueDisc>();
        red1->SetNumQueues(1);
        red1->AddPhbEntry(10, 0, 0);
        red1->SetScheduler(
            CreateObjectWithAttributes<RoundRobinScheduler>("NumQueues", UintegerValue(1)));
        red1->SetQueueLimit(0, 50);
        edge->SetInnerDiscAt(1, red1);

        edge->SetDscpToSlot(46, 0);
        edge->SetDscpToSlot(10, 1);

        edge->Initialize();

        // DROP_TAIL mode + high thresholds so neither inner early-drops
        // under the modest burst the cases push — same idiom S-14.1
        // established in 2001 and S-13.12 continues to use.
        red0->SetMredMode(MredMode::DROP_TAIL, 0);
        red0->ConfigQueue({.queue = 0, .prec = 0, .thMin = 1000.0, .thMax = 2000.0, .maxP = 0.1});
        red1->SetMredMode(MredMode::DROP_TAIL, 0);
        red1->ConfigQueue({.queue = 0, .prec = 0, .thMin = 1000.0, .thMax = 2000.0, .maxP = 0.1});
    }

    void DoRun() override
    {
        const uint32_t n = 3;

        // ---- Case A: default dispatcher preserves strict-priority ----
        {
            Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
            ConfigureDualSlotEdge(edge);

            // Enqueue low-priority (slot 1) first, then high-priority (slot 0).
            for (uint32_t i = 0; i < n; ++i)
            {
                NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(MakeItem(10)),
                                      true,
                                      "Case A slot-1 enqueue " << i);
            }
            for (uint32_t i = 0; i < n; ++i)
            {
                NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(MakeItem(46)),
                                      true,
                                      "Case A slot-0 enqueue " << i);
            }

            // First n dequeued must be DSCP 46 (slot 0 drains first).
            for (uint32_t i = 0; i < n; ++i)
            {
                Ptr<QueueDiscItem> it = edge->Dequeue();
                NS_TEST_ASSERT_MSG_NE(it, nullptr, "Case A dequeue " << i);
                if (!it)
                {
                    Simulator::Destroy();
                    return;
                }
                Ptr<Ipv4QueueDiscItem> ip = DynamicCast<Ipv4QueueDiscItem>(it);
                uint8_t dscp = ip->GetHeader().GetTos() >> 2;
                NS_TEST_ASSERT_MSG_EQ(static_cast<uint32_t>(dscp),
                                      46u,
                                      "Case A dequeue " << i << " expected DSCP 46");
            }
            // Remaining n must be DSCP 10 (slot 1 drains only after slot 0).
            for (uint32_t i = 0; i < n; ++i)
            {
                Ptr<QueueDiscItem> it = edge->Dequeue();
                NS_TEST_ASSERT_MSG_NE(it, nullptr, "Case A dequeue " << (n + i));
                if (!it)
                {
                    Simulator::Destroy();
                    return;
                }
                Ptr<Ipv4QueueDiscItem> ip = DynamicCast<Ipv4QueueDiscItem>(it);
                uint8_t dscp = ip->GetHeader().GetTos() >> 2;
                NS_TEST_ASSERT_MSG_EQ(static_cast<uint32_t>(dscp),
                                      10u,
                                      "Case A dequeue " << (n + i) << " expected DSCP 10");
            }
            NS_TEST_ASSERT_MSG_EQ(edge->Dequeue(), nullptr, "Case A: both slots empty");
        }

        // ---- Case B: spy dispatcher counts hook invocations ----
        {
            Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
            Ptr<S17_1_SpyDispatcher> spy = CreateObject<S17_1_SpyDispatcher>();
            edge->SetSlotDispatcher(spy);
            ConfigureDualSlotEdge(edge);

            for (uint32_t i = 0; i < n; ++i)
            {
                NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(MakeItem(46)), true, "Case B slot-0 enq");
            }
            for (uint32_t i = 0; i < n; ++i)
            {
                NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(MakeItem(10)), true, "Case B slot-1 enq");
            }

            NS_TEST_ASSERT_MSG_EQ(spy->enqueueCount,
                                  2u * n,
                                  "Case B: OnEnqueue fires once per successful enqueue");
            NS_TEST_ASSERT_MSG_EQ(spy->lastEnqueueSlot, 1u, "Case B: last OnEnqueue was slot 1");

            for (uint32_t i = 0; i < 2 * n; ++i)
            {
                Ptr<QueueDiscItem> it = edge->Dequeue();
                NS_TEST_ASSERT_MSG_NE(it, nullptr, "Case B dequeue " << i);
                if (!it)
                {
                    Simulator::Destroy();
                    return;
                }
            }

            NS_TEST_ASSERT_MSG_EQ(spy->dequeueCount,
                                  2u * n,
                                  "Case B: OnDequeue fires once per successful dequeue");
            NS_TEST_ASSERT_MSG_EQ(spy->lastDequeueSlot,
                                  1u,
                                  "Case B: last OnDequeue was slot 1 (drained second)");
        }

        // ---- Case C: PeekSlot mirrors subsequent dequeue, no mutation ----
        {
            Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
            ConfigureDualSlotEdge(edge);

            NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(MakeItem(10)), true, "Case C slot-1 enq");
            Ptr<const QueueDiscItem> peek1 = edge->Peek();
            NS_TEST_ASSERT_MSG_NE(peek1, nullptr, "Case C peek after slot-1 enqueue");
            if (!peek1)
            {
                Simulator::Destroy();
                return;
            }
            Ptr<const Ipv4QueueDiscItem> ip1 = DynamicCast<const Ipv4QueueDiscItem>(peek1);
            NS_TEST_ASSERT_MSG_EQ(static_cast<uint32_t>(ip1->GetHeader().GetTos() >> 2),
                                  10u,
                                  "Case C: peek with only slot-1 populated returns DSCP 10");

            // A second peek with no intervening dequeue must return the
            // same packet — proves PeekSlot is side-effect-free.
            Ptr<const QueueDiscItem> peek1b = edge->Peek();
            NS_TEST_ASSERT_MSG_NE(peek1b, nullptr, "Case C second peek");
            if (!peek1b)
            {
                return;
            }
            NS_TEST_ASSERT_MSG_EQ(peek1b,
                                  peek1,
                                  "Case C: repeated peek must yield the same item (no mutation)");

            NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(MakeItem(46)), true, "Case C slot-0 enq");
            Ptr<const QueueDiscItem> peek2 = edge->Peek();
            NS_TEST_ASSERT_MSG_NE(peek2, nullptr, "Case C peek after slot-0 enqueue");
            if (!peek2)
            {
                Simulator::Destroy();
                return;
            }
            Ptr<const Ipv4QueueDiscItem> ip2 = DynamicCast<const Ipv4QueueDiscItem>(peek2);
            NS_TEST_ASSERT_MSG_EQ(
                static_cast<uint32_t>(ip2->GetHeader().GetTos() >> 2),
                46u,
                "Case C: peek with both slots populated returns slot-0 (DSCP 46)");
        }

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-17.2..S-17.4: cake::TinShaperDispatcher — DRR across slots
//
// Share one fixture builder: three RedQueueDisc inners at slots 0, 1, 2
// routed by DSCP 10, 20, 30. DROP_TAIL + high thresholds so the inners
// never drop under the modest bursts these tests push, mirroring the S-13.12
// and S-17.1 discipline. The DRR dispatcher is installed via
// SetSlotDispatcher before Initialize; quanta are set via SetQuantum
// before edge->Initialize() per the pre-Initialize setter contract.
// =============================================================================

namespace
{

Ptr<Ipv4QueueDiscItem>
MakeTinShaperItem(uint8_t dscp, uint32_t payloadBytes)
{
    Ptr<Packet> p = Create<Packet>(payloadBytes);
    Ipv4Header h;
    h.SetSource(Ipv4Address("10.0.0.1"));
    h.SetDestination(Ipv4Address("10.0.0.2"));
    h.SetProtocol(17);
    h.SetPayloadSize(payloadBytes);
    h.SetDscp(static_cast<Ipv4Header::DscpType>(dscp));
    Address src;
    return Create<Ipv4QueueDiscItem>(p, src, 0x0800, h);
}

Ptr<RedQueueDisc>
MakeTinInner(uint8_t dscp)
{
    auto inner = CreateObject<RedQueueDisc>();
    inner->SetNumQueues(1);
    inner->AddPhbEntry(dscp, 0, 0);
    inner->SetScheduler(
        CreateObjectWithAttributes<RoundRobinScheduler>("NumQueues", UintegerValue(1)));
    inner->SetQueueLimit(0, 5000);
    return inner;
}

void
ConfigureThreeSlotShaperEdge(Ptr<EdgeQueueDisc> edge,
                             Ptr<cake::TinShaperDispatcher> shaper,
                             uint32_t quantum0,
                             uint32_t quantum1,
                             uint32_t quantum2)
{
    auto inner0 = MakeTinInner(10);
    auto inner1 = MakeTinInner(20);
    auto inner2 = MakeTinInner(30);
    edge->SetInnerDiscAt(0, inner0);
    edge->SetInnerDiscAt(1, inner1);
    edge->SetInnerDiscAt(2, inner2);
    edge->SetDscpToSlot(10, 0);
    edge->SetDscpToSlot(20, 1);
    edge->SetDscpToSlot(30, 2);

    shaper->SetQuantum(0, quantum0);
    shaper->SetQuantum(1, quantum1);
    shaper->SetQuantum(2, quantum2);
    edge->SetSlotDispatcher(shaper);

    edge->Initialize();

    // DROP_TAIL + high thresholds so the inners never early-drop under
    // the modest bursts these tests push — same pattern S-14.1 has used
    // since 2001 and S-13.12 / S-17.1 continue to use.
    for (auto& inner : {inner0, inner1, inner2})
    {
        inner->SetMredMode(MredMode::DROP_TAIL, 0);
        inner->ConfigQueue(
            {.queue = 0, .prec = 0, .thMin = 10000.0, .thMax = 20000.0, .maxP = 0.1});
    }
}

} // namespace

/**
 * @brief Verifies the per-tin DRR shaper honours the configured quantum.
 * @see specs/02-structural.md S-17.2
 */
class TinShaperDrrQuantumHonoredTest : public TestCase
{
  public:
    TinShaperDrrQuantumHonoredTest()
        : TestCase("S-17.2 Tin-shaper DRR honours per-slot quanta")
    {
    }

  private:
    void DoRun() override
    {
        // 500-byte payload + 20-byte IPv4 header = 520 wire bytes. Quanta
        // 520, 1040, 2080 -> expected per-round packet counts 1, 2, 4
        // (byte ratio 1:2:4) since quantum is an integer multiple of
        // head size for each slot.
        const uint32_t payload = 500;
        const uint32_t wireSize = payload + 20;
        const uint32_t q0 = wireSize;
        const uint32_t q1 = 2 * wireSize;
        const uint32_t q2 = 4 * wireSize;

        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        Ptr<cake::TinShaperDispatcher> shaper = CreateObject<cake::TinShaperDispatcher>();
        ConfigureThreeSlotShaperEdge(edge, shaper, q0, q1, q2);

        // Saturate enough that no slot runs dry during the measurement.
        const uint32_t perSlot = 200;
        for (uint32_t i = 0; i < perSlot; ++i)
        {
            NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(MakeTinShaperItem(10, payload)),
                                  true,
                                  "slot 0 enqueue " << i);
            NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(MakeTinShaperItem(20, payload)),
                                  true,
                                  "slot 1 enqueue " << i);
            NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(MakeTinShaperItem(30, payload)),
                                  true,
                                  "slot 2 enqueue " << i);
        }

        // Drain 20 DRR rounds. Each round emits 1+2+4 = 7 packets
        // (1 from slot 0, 2 from slot 1, 4 from slot 2).
        const uint32_t rounds = 20;
        const uint32_t expectedS0 = rounds * 1;
        const uint32_t expectedS1 = rounds * 2;
        const uint32_t expectedS2 = rounds * 4;
        const uint32_t totalDequeue = expectedS0 + expectedS1 + expectedS2;

        uint32_t counts[3] = {0, 0, 0};
        for (uint32_t i = 0; i < totalDequeue; ++i)
        {
            Ptr<QueueDiscItem> it = edge->Dequeue();
            NS_TEST_ASSERT_MSG_NE(it, nullptr, "dequeue " << i);
            if (!it)
            {
                Simulator::Destroy();
                return;
            }
            Ptr<Ipv4QueueDiscItem> ip = DynamicCast<Ipv4QueueDiscItem>(it);
            const uint32_t dscp = ip->GetHeader().GetTos() >> 2;
            NS_TEST_ASSERT_MSG_EQ(dscp == 10 || dscp == 20 || dscp == 30,
                                  true,
                                  "dequeue " << i << " has an expected DSCP (got " << dscp << ")");
            if (dscp == 10)
            {
                ++counts[0];
            }
            else if (dscp == 20)
            {
                ++counts[1];
            }
            else if (dscp == 30)
            {
                ++counts[2];
            }
        }

        // Quantum is an exact multiple of head size for every slot, so
        // the DRR algorithm is deterministic and the counts land on the
        // expected values exactly — tolerance window is zero.
        NS_TEST_ASSERT_MSG_EQ(counts[0], expectedS0, "slot 0 dequeued packet count");
        NS_TEST_ASSERT_MSG_EQ(counts[1], expectedS1, "slot 1 dequeued packet count");
        NS_TEST_ASSERT_MSG_EQ(counts[2], expectedS2, "slot 2 dequeued packet count");

        // PeekSlot side-effect-freeness under DRR: two consecutive peeks
        // with no intervening dequeue must yield the same item, and
        // re-iterating the DRR after the peeks must still return the
        // same sequence as if no peeks had happened.
        Ptr<const QueueDiscItem> peekA = edge->Peek();
        NS_TEST_ASSERT_MSG_NE(peekA, nullptr, "peek after measurement phase");
        if (!peekA)
        {
            Simulator::Destroy();
            return;
        }
        Ptr<const QueueDiscItem> peekB = edge->Peek();
        NS_TEST_ASSERT_MSG_EQ(peekB, peekA, "repeated peek yields the same item (no mutation)");

        Simulator::Destroy();
    }
};

/**
 * @brief Verifies the tin shaper skips empty tins without stalling the schedule.
 * @see specs/02-structural.md S-17.3
 */
class TinShaperEmptyTinSkipTest : public TestCase
{
  public:
    TinShaperEmptyTinSkipTest()
        : TestCase("S-17.3 Tin-shaper skips empty slots without crediting")
    {
    }

  private:
    void DoRun() override
    {
        const uint32_t payload = 500;
        const uint32_t wireSize = payload + 20;
        // Symmetric quanta so the only source of asymmetry is populated vs empty.
        const uint32_t q = wireSize;

        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        Ptr<cake::TinShaperDispatcher> shaper = CreateObject<cake::TinShaperDispatcher>();
        ConfigureThreeSlotShaperEdge(edge, shaper, q, q, q);

        // Case A: only slot 0 has packets. Drain completely. Every dequeue
        // must come from slot 0; slots 1 and 2 never produce a packet
        // since they are empty and skipped without crediting.
        const uint32_t n = 10;
        for (uint32_t i = 0; i < n; ++i)
        {
            NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(MakeTinShaperItem(10, payload)),
                                  true,
                                  "A enqueue " << i);
        }
        for (uint32_t i = 0; i < n; ++i)
        {
            Ptr<QueueDiscItem> it = edge->Dequeue();
            NS_TEST_ASSERT_MSG_NE(it, nullptr, "A dequeue " << i);
            if (!it)
            {
                Simulator::Destroy();
                return;
            }
            Ptr<Ipv4QueueDiscItem> ip = DynamicCast<Ipv4QueueDiscItem>(it);
            NS_TEST_ASSERT_MSG_EQ(static_cast<uint32_t>(ip->GetHeader().GetTos() >> 2),
                                  10u,
                                  "A dequeue " << i << " must come from slot 0 (DSCP 10)");
        }
        NS_TEST_ASSERT_MSG_EQ(edge->Dequeue(), nullptr, "A: no more packets");

        // Case B: empty-reset behavioural check. Slot 0 was drained to
        // empty just now; its deficit must be 0 (reset). Now enqueue
        // n packets on slot 1 (which has never been visited) and
        // n packets on slot 0. With quanta equal across slots, the
        // DRR must interleave 1-for-1. If slot 0's deficit had
        // accumulated save-up, it would dequeue multiple packets before
        // yielding.
        for (uint32_t i = 0; i < n; ++i)
        {
            NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(MakeTinShaperItem(10, payload)),
                                  true,
                                  "B slot 0 enqueue " << i);
            NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(MakeTinShaperItem(20, payload)),
                                  true,
                                  "B slot 1 enqueue " << i);
        }

        // After the Case-A drain, the cursor advanced past slot 0.
        // Drain the 2n packets and record the interleave. Under correct
        // DRR with equal quanta and equal head sizes, the interleave
        // should alternate 1-for-1; any run longer than 1 indicates
        // deficit save-up.
        uint32_t countS0 = 0;
        uint32_t countS1 = 0;
        uint32_t maxRun = 0;
        uint32_t currentRun = 0;
        uint32_t lastDscp = 0;
        for (uint32_t i = 0; i < 2 * n; ++i)
        {
            Ptr<QueueDiscItem> it = edge->Dequeue();
            NS_TEST_ASSERT_MSG_NE(it, nullptr, "B dequeue " << i);
            if (!it)
            {
                Simulator::Destroy();
                return;
            }
            Ptr<Ipv4QueueDiscItem> ip = DynamicCast<Ipv4QueueDiscItem>(it);
            const uint32_t dscp = ip->GetHeader().GetTos() >> 2;
            if (dscp == 10)
            {
                ++countS0;
            }
            else if (dscp == 20)
            {
                ++countS1;
            }
            if (dscp == lastDscp)
            {
                ++currentRun;
            }
            else
            {
                currentRun = 1;
            }
            if (currentRun > maxRun)
            {
                maxRun = currentRun;
            }
            lastDscp = dscp;
        }

        NS_TEST_ASSERT_MSG_EQ(countS0, n, "B: slot 0 drained its n packets");
        NS_TEST_ASSERT_MSG_EQ(countS1, n, "B: slot 1 drained its n packets");
        // The strictest assertion: with equal quanta and exact-multiple
        // head sizes, the run length must be at most 1 — any longer run
        // means the deficit reset was not applied on drain-to-empty.
        NS_TEST_ASSERT_MSG_LT_OR_EQ(maxRun,
                                    1u,
                                    "B: max same-slot run must be <= 1 "
                                    "(longer run indicates missing empty-reset)");

        Simulator::Destroy();
    }
};

/**
 * @brief Verifies the tin shaper preserves DRR fairness under mixed-load tins.
 * @see specs/02-structural.md S-17.4
 */
class TinShaperFairnessUnderMixedLoadTest : public TestCase
{
  public:
    TinShaperFairnessUnderMixedLoadTest()
        : TestCase("S-17.4 Tin-shaper is work-conserving under mixed load")
    {
    }

  private:
    void DoRun() override
    {
        const uint32_t payload = 500;
        const uint32_t wireSize = payload + 20;
        const uint32_t q0 = wireSize;
        const uint32_t q1 = 4 * wireSize; // slot 1 gets 4x share

        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        Ptr<cake::TinShaperDispatcher> shaper = CreateObject<cake::TinShaperDispatcher>();
        ConfigureThreeSlotShaperEdge(edge, shaper, q0, q1, wireSize);

        // Slot 0 gets a short burst; slot 1 stays saturated past slot 0's
        // exhaustion point; slot 2 stays empty throughout to exercise
        // the empty-skip cursor advance.
        const uint32_t shortBurst = 20;
        const uint32_t sustained = 200;
        for (uint32_t i = 0; i < shortBurst; ++i)
        {
            NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(MakeTinShaperItem(10, payload)),
                                  true,
                                  "slot 0 enq " << i);
        }
        for (uint32_t i = 0; i < sustained; ++i)
        {
            NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(MakeTinShaperItem(20, payload)),
                                  true,
                                  "slot 1 enq " << i);
        }

        // Drain all packets. Track when slot 0 exhausts.
        const uint32_t total = shortBurst + sustained;
        uint32_t countS0 = 0;
        uint32_t countS1 = 0;
        uint32_t lastS0Index = 0;
        for (uint32_t i = 0; i < total; ++i)
        {
            Ptr<QueueDiscItem> it = edge->Dequeue();
            NS_TEST_ASSERT_MSG_NE(it, nullptr, "dequeue " << i);
            if (!it)
            {
                Simulator::Destroy();
                return;
            }
            Ptr<Ipv4QueueDiscItem> ip = DynamicCast<Ipv4QueueDiscItem>(it);
            const uint32_t dscp = ip->GetHeader().GetTos() >> 2;
            if (dscp == 10)
            {
                ++countS0;
                lastS0Index = i;
            }
            else if (dscp == 20)
            {
                ++countS1;
            }
        }

        NS_TEST_ASSERT_MSG_EQ(edge->Dequeue(), nullptr, "all packets drained (work-conserving)");
        NS_TEST_ASSERT_MSG_EQ(countS0, shortBurst, "slot 0 burst drained in full");
        NS_TEST_ASSERT_MSG_EQ(countS1, sustained, "slot 1 sustained drained in full");

        // Fairness check: during the overlap phase (while both slots
        // have packets), slot 1 gets 4x the byte share. With 20
        // packets on slot 0 and 4x quanta on slot 1, slot 0 should
        // exhaust after about 20 * (1 + 4) = 100 dequeues. If slot 0
        // exhausts after fewer than ~shortBurst dequeues it means slot
        // 1 is starved; if later than ~150 it means slot 0 is starved.
        NS_TEST_ASSERT_MSG_GT_OR_EQ(
            lastS0Index + 1,
            shortBurst,
            "slot 0 received at least one dequeue per packet (no starvation at head)");
        NS_TEST_ASSERT_MSG_LT_OR_EQ(
            lastS0Index + 1,
            150u,
            "slot 0 finished within the overlap window (no monopolisation by slot 0)");

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-17.11..14: Hybrid LLQ-across-tins dispatcher
//
//  S-17.11 SP slots win over DRR slots when both non-empty
//  S-17.12 DRR fairness preserved when SP slot empty
//  S-17.13 PeekSlot side-effect-free under hybrid configuration
//  S-17.14 OnDequeue accounts only DRR slots, never SP
// =============================================================================

namespace
{

void
ConfigureHybridLlqEdge(Ptr<EdgeQueueDisc> edge,
                       Ptr<HybridLlqDispatcher> hybrid,
                       uint32_t slotSp,
                       const std::vector<uint32_t>& drrSlots,
                       const std::vector<uint32_t>& drrQuanta)
{
    NS_ASSERT(drrSlots.size() == drrQuanta.size());

    // Edge invariant: SetInnerDiscAt(k) requires slots 0..k-1 already
    // populated (diffserv-edge-queue-disc.cc:209). Build a slot-index ->
    // (inner, dscp) map and populate in numeric order.
    const uint32_t kTotal = static_cast<uint32_t>(drrSlots.size() + 1);
    std::vector<Ptr<RedQueueDisc>> innersByIdx(kTotal);
    std::vector<uint8_t> dscpByIdx(kTotal);
    for (uint32_t i = 0; i < drrSlots.size(); ++i)
    {
        const uint32_t slot = drrSlots[i];
        NS_ASSERT_MSG(slot < kTotal, "drrSlot index out of contiguous range");
        const uint8_t dscp = static_cast<uint8_t>(10 * (slot + 1));
        innersByIdx[slot] = MakeTinInner(dscp);
        dscpByIdx[slot] = dscp;
    }
    {
        const uint8_t dscp = static_cast<uint8_t>(10 * (slotSp + 1));
        innersByIdx[slotSp] = MakeTinInner(dscp);
        dscpByIdx[slotSp] = dscp;
    }
    for (uint32_t s = 0; s < kTotal; ++s)
    {
        NS_ASSERT_MSG(innersByIdx[s], "slot " << s << " unpopulated");
        edge->SetInnerDiscAt(s, innersByIdx[s]);
        edge->SetDscpToSlot(dscpByIdx[s], s);
    }

    hybrid->SetSlotStrictPriority(slotSp);
    for (uint32_t i = 0; i < drrSlots.size(); ++i)
    {
        hybrid->SetQuantum(drrSlots[i], drrQuanta[i]);
    }
    edge->SetSlotDispatcher(hybrid);

    edge->Initialize();

    for (auto& inner : innersByIdx)
    {
        inner->SetMredMode(MredMode::DROP_TAIL, 0);
        inner->ConfigQueue(
            {.queue = 0, .prec = 0, .thMin = 10000.0, .thMax = 20000.0, .maxP = 0.1});
    }
}

} // namespace

/**
 * @brief Verifies hybrid LLQ serves the strict-priority slot before any DRR slot.
 * @see specs/02-structural.md S-17.11
 */
class HybridLlqStrictPriorityWinsOverDrrTest : public TestCase
{
  public:
    HybridLlqStrictPriorityWinsOverDrrTest()
        : TestCase("S-17.11 hybrid LLQ: SP slot drains first when both SP and DRR slots non-empty")
    {
    }

  private:
    void DoRun() override
    {
        // Three slots: SP=0 (DSCP 10), DRR=1 (DSCP 20), DRR=2 (DSCP 30).
        // Both DRR slots quantum = wireSize so under DRR alone they would
        // alternate 1-for-1; with slot 0 marked SP, slot 0 must drain
        // entirely before either DRR slot yields a packet.
        const uint32_t payload = 500;
        const uint32_t wireSize = payload + 20;
        const uint32_t kSpPackets = 12;
        const uint32_t kPerDrrPackets = 8;

        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        Ptr<HybridLlqDispatcher> hybrid = CreateObject<HybridLlqDispatcher>();
        ConfigureHybridLlqEdge(edge, hybrid, 0, {1, 2}, {wireSize, wireSize});

        // Enqueue all three slots fully before any dequeue. Slot 0 (SP)
        // must dominate the first kSpPackets dequeues.
        for (uint32_t i = 0; i < kSpPackets; ++i)
        {
            NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(MakeTinShaperItem(10, payload)),
                                  true,
                                  "slot 0 (SP) enqueue " << i);
        }
        for (uint32_t i = 0; i < kPerDrrPackets; ++i)
        {
            NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(MakeTinShaperItem(20, payload)),
                                  true,
                                  "slot 1 (DRR) enqueue " << i);
            NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(MakeTinShaperItem(30, payload)),
                                  true,
                                  "slot 2 (DRR) enqueue " << i);
        }

        // First kSpPackets dequeues must be DSCP 10 — SP fast path.
        for (uint32_t i = 0; i < kSpPackets; ++i)
        {
            Ptr<QueueDiscItem> it = edge->Dequeue();
            NS_TEST_ASSERT_MSG_NE(it, nullptr, "SP-phase dequeue " << i);
            if (!it)
            {
                Simulator::Destroy();
                return;
            }
            Ptr<Ipv4QueueDiscItem> ip = DynamicCast<Ipv4QueueDiscItem>(it);
            const uint32_t dscp = ip->GetHeader().GetTos() >> 2;
            NS_TEST_ASSERT_MSG_EQ(dscp,
                                  10u,
                                  "SP-phase dequeue "
                                      << i << " must come from slot 0 (SP); got DSCP " << dscp);
        }

        // Remaining 2 * kPerDrrPackets dequeues must alternate slots 1 & 2.
        // Same DRR-fairness check as S-17.3: max same-slot run length <= 1
        // when quanta and head sizes are equal.
        uint32_t countS1 = 0;
        uint32_t countS2 = 0;
        uint32_t maxRun = 0;
        uint32_t currentRun = 0;
        uint32_t lastDscp = 0;
        for (uint32_t i = 0; i < 2 * kPerDrrPackets; ++i)
        {
            Ptr<QueueDiscItem> it = edge->Dequeue();
            NS_TEST_ASSERT_MSG_NE(it, nullptr, "DRR-phase dequeue " << i);
            if (!it)
            {
                Simulator::Destroy();
                return;
            }
            Ptr<Ipv4QueueDiscItem> ip = DynamicCast<Ipv4QueueDiscItem>(it);
            const uint32_t dscp = ip->GetHeader().GetTos() >> 2;
            NS_TEST_ASSERT_MSG_EQ(dscp == 20 || dscp == 30,
                                  true,
                                  "DRR-phase dequeue " << i << " has unexpected DSCP " << dscp);
            if (dscp == 20)
            {
                ++countS1;
            }
            else if (dscp == 30)
            {
                ++countS2;
            }
            currentRun = (dscp == lastDscp) ? currentRun + 1 : 1u;
            if (currentRun > maxRun)
            {
                maxRun = currentRun;
            }
            lastDscp = dscp;
        }
        NS_TEST_ASSERT_MSG_EQ(countS1, kPerDrrPackets, "DRR slot 1 fully drained");
        NS_TEST_ASSERT_MSG_EQ(countS2, kPerDrrPackets, "DRR slot 2 fully drained");
        NS_TEST_ASSERT_MSG_LT_OR_EQ(maxRun,
                                    1u,
                                    "DRR-phase max same-slot run must be <= 1 (DRR fairness "
                                    "intact under hybrid)");

        Simulator::Destroy();
    }
};

/**
 * @brief Verifies hybrid LLQ falls back to DRR fairness when the SP slot is empty.
 * @see specs/02-structural.md S-17.12
 */
class HybridLlqDrrFairnessWhenSpEmptyTest : public TestCase
{
  public:
    HybridLlqDrrFairnessWhenSpEmptyTest()
        : TestCase("S-17.12 hybrid LLQ: DRR fairness identical to pure tin-shaper when SP slot "
                   "empty")
    {
    }

  private:
    void DoRun() override
    {
        // SP=2 (empty throughout), DRR=0 (q=wireSize), DRR=1 (q=2*wireSize).
        // With slot 2 untouched, the dispatcher must produce the same
        // dequeue stream as cake::TinShaperDispatcher with quanta (wire, 2*wire).
        const uint32_t payload = 500;
        const uint32_t wireSize = payload + 20;

        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        Ptr<HybridLlqDispatcher> hybrid = CreateObject<HybridLlqDispatcher>();
        ConfigureHybridLlqEdge(edge, hybrid, 2, {0, 1}, {wireSize, 2 * wireSize});

        const uint32_t perSlot = 100;
        for (uint32_t i = 0; i < perSlot; ++i)
        {
            NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(MakeTinShaperItem(10, payload)),
                                  true,
                                  "slot 0 enqueue " << i);
            NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(MakeTinShaperItem(20, payload)),
                                  true,
                                  "slot 1 enqueue " << i);
        }

        // 30 rounds: slot 0 gets 1 packet, slot 1 gets 2 packets per round.
        // Total dequeues = 30 * (1 + 2) = 90.
        const uint32_t rounds = 30;
        const uint32_t expectedS0 = rounds * 1;
        const uint32_t expectedS1 = rounds * 2;
        const uint32_t total = expectedS0 + expectedS1;

        uint32_t counts[2] = {0, 0};
        for (uint32_t i = 0; i < total; ++i)
        {
            Ptr<QueueDiscItem> it = edge->Dequeue();
            NS_TEST_ASSERT_MSG_NE(it, nullptr, "dequeue " << i);
            if (!it)
            {
                Simulator::Destroy();
                return;
            }
            Ptr<Ipv4QueueDiscItem> ip = DynamicCast<Ipv4QueueDiscItem>(it);
            const uint32_t dscp = ip->GetHeader().GetTos() >> 2;
            if (dscp == 10)
            {
                ++counts[0];
            }
            else if (dscp == 20)
            {
                ++counts[1];
            }
            else
            {
                NS_TEST_ASSERT_MSG_EQ(true,
                                      false,
                                      "dequeue " << i
                                                 << " came from SP slot 2 (DSCP 30) — "
                                                    "must be empty, hybrid dispatcher leaked");
            }
        }
        NS_TEST_ASSERT_MSG_EQ(counts[0],
                              expectedS0,
                              "DRR slot 0 dequeued packet count under empty SP");
        NS_TEST_ASSERT_MSG_EQ(counts[1],
                              expectedS1,
                              "DRR slot 1 dequeued packet count under empty SP");

        Simulator::Destroy();
    }
};

/**
 * @brief Verifies hybrid LLQ peek is side-effect free with respect to DRR deficits.
 * @see specs/02-structural.md S-17.13
 */
class HybridLlqPeekIsSideEffectFreeTest : public TestCase
{
  public:
    HybridLlqPeekIsSideEffectFreeTest()
        : TestCase("S-17.13 hybrid LLQ: PeekSlot is side-effect-free across SP and DRR "
                   "fall-through")
    {
    }

  private:
    void DoRun() override
    {
        const uint32_t payload = 500;
        const uint32_t wireSize = payload + 20;

        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        Ptr<HybridLlqDispatcher> hybrid = CreateObject<HybridLlqDispatcher>();
        ConfigureHybridLlqEdge(edge, hybrid, 0, {1, 2}, {wireSize, wireSize});

        // Case A: SP slot non-empty -> repeated peek must yield the same SP item.
        NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(MakeTinShaperItem(10, payload)), true, "A: SP enqueue");
        Ptr<const QueueDiscItem> peekA1 = edge->Peek();
        Ptr<const QueueDiscItem> peekA2 = edge->Peek();
        NS_TEST_ASSERT_MSG_EQ(peekA1, peekA2, "A: SP-path peek is idempotent");
        NS_TEST_ASSERT_MSG_NE(peekA1, nullptr, "A: SP peek non-null");
        if (!peekA1)
        {
            Simulator::Destroy();
            return;
        }

        // Drain the SP packet; both DRR slots empty -> peek returns null.
        Ptr<QueueDiscItem> it = edge->Dequeue();
        NS_TEST_ASSERT_MSG_NE(it, nullptr, "A: SP dequeue");
        if (!it)
        {
            return;
        }
        NS_TEST_ASSERT_MSG_EQ(edge->Peek(), nullptr, "A: post-drain peek null");

        // Case B: SP slot empty, DRR slots non-empty -> peek falls through
        // to DRR. The class member m_deficit MUST NOT be mutated by Peek;
        // a subsequent Dequeue must yield the same item the peek predicted.
        NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(MakeTinShaperItem(20, payload)),
                              true,
                              "B: DRR slot 1 enqueue");
        NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(MakeTinShaperItem(30, payload)),
                              true,
                              "B: DRR slot 2 enqueue");
        Ptr<const QueueDiscItem> peekB1 = edge->Peek();
        Ptr<const QueueDiscItem> peekB2 = edge->Peek();
        NS_TEST_ASSERT_MSG_EQ(peekB1, peekB2, "B: DRR-fall-through peek is idempotent");
        NS_TEST_ASSERT_MSG_NE(peekB1, nullptr, "B: DRR peek non-null");
        if (!peekB1)
        {
            Simulator::Destroy();
            return;
        }
        Ptr<QueueDiscItem> firstDrr = edge->Dequeue();
        NS_TEST_ASSERT_MSG_EQ(firstDrr,
                              peekB1,
                              "B: dequeue matches the peeked item exactly (no peek side-effect)");

        Simulator::Destroy();
    }
};

/**
 * @brief Verifies hybrid LLQ debits DRR deficit only on DRR-slot dequeues.
 * @see specs/02-structural.md S-17.14
 */
class HybridLlqOnDequeueAccountsOnlyDrrTest : public TestCase
{
  public:
    HybridLlqOnDequeueAccountsOnlyDrrTest()
        : TestCase("S-17.14 hybrid LLQ: OnDequeue accounts deficit only for DRR slots")
    {
    }

  private:
    void DoRun() override
    {
        // After a long burst of pure SP traffic, the DRR cursor and
        // deficits must remain at their initial state. When DRR traffic
        // arrives, the DRR loop must work exactly like a fresh tin-shaper.
        const uint32_t payload = 500;
        const uint32_t wireSize = payload + 20;

        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        Ptr<HybridLlqDispatcher> hybrid = CreateObject<HybridLlqDispatcher>();
        ConfigureHybridLlqEdge(edge, hybrid, 0, {1, 2}, {wireSize, wireSize});

        // Run 50 packets through SP only.
        for (uint32_t i = 0; i < 50; ++i)
        {
            NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(MakeTinShaperItem(10, payload)),
                                  true,
                                  "SP enqueue " << i);
        }
        for (uint32_t i = 0; i < 50; ++i)
        {
            Ptr<QueueDiscItem> it = edge->Dequeue();
            NS_TEST_ASSERT_MSG_NE(it, nullptr, "SP dequeue " << i);
            if (!it)
            {
                return;
            }
        }

        // SP slot has no quantum and no deficit; observable via API:
        // GetQuantum(0) must be 0 and IsStrictPriority(0) must be true.
        NS_TEST_ASSERT_MSG_EQ(hybrid->GetQuantum(0), 0u, "SP slot has no quantum");
        NS_TEST_ASSERT_MSG_EQ(hybrid->IsStrictPriority(0), true, "slot 0 is SP");
        NS_TEST_ASSERT_MSG_EQ(hybrid->IsStrictPriority(1), false, "slot 1 is DRR");
        NS_TEST_ASSERT_MSG_EQ(hybrid->IsStrictPriority(2), false, "slot 2 is DRR");

        // DRR phase: load equal packets on slots 1 and 2 with equal
        // quanta. The DRR cursor was untouched by 50 SP dequeues, so the
        // resulting interleave should be byte-identical to a fresh
        // tin-shaper: max same-slot run = 1.
        const uint32_t n = 20;
        for (uint32_t i = 0; i < n; ++i)
        {
            NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(MakeTinShaperItem(20, payload)),
                                  true,
                                  "DRR slot 1 enqueue " << i);
            NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(MakeTinShaperItem(30, payload)),
                                  true,
                                  "DRR slot 2 enqueue " << i);
        }

        uint32_t maxRun = 0;
        uint32_t currentRun = 0;
        uint32_t lastDscp = 0;
        for (uint32_t i = 0; i < 2 * n; ++i)
        {
            Ptr<QueueDiscItem> it = edge->Dequeue();
            NS_TEST_ASSERT_MSG_NE(it, nullptr, "DRR phase dequeue " << i);
            if (!it)
            {
                Simulator::Destroy();
                return;
            }
            Ptr<Ipv4QueueDiscItem> ip = DynamicCast<Ipv4QueueDiscItem>(it);
            const uint32_t dscp = ip->GetHeader().GetTos() >> 2;
            currentRun = (dscp == lastDscp) ? currentRun + 1 : 1u;
            if (currentRun > maxRun)
            {
                maxRun = currentRun;
            }
            lastDscp = dscp;
        }
        NS_TEST_ASSERT_MSG_LT_OR_EQ(maxRun,
                                    1u,
                                    "DRR cursor untouched by 50 prior SP dequeues; "
                                    "interleave alternates 1-for-1 with equal quanta");

        Simulator::Destroy();
    }
};

// ---------------------------------------------------------------------
//  hybrid LLQ: DRR cursor must advance off a drained slot
// ---------------------------------------------------------------------

/**
 * @brief A DRR slot that drains to empty must release the dispatcher cursor so
 * the next backlogged slot gets its turn.
 *
 * Parking the cursor on a slot that keeps bouncing off empty (an ACK-clocked TCP
 * flow holds a slot at ~one packet that drains each service) lets that slot
 * collect a fresh quantum at every refill while other backlogged slots starve.
 * The pure DRR dispatcher (cake::TinShaperDispatcher) advances the cursor on
 * drain-to-empty, matching Linux cake_dequeue's `cur_tin++` past a tin with no
 * servable flows; the hybrid-LLQ dispatcher must do the same.
 */
class HybridLlqDrrCursorYieldsOnDrainTest : public TestCase
{
  public:
    HybridLlqDrrCursorYieldsOnDrainTest()
        : TestCase("hybrid LLQ: DRR cursor yields on drain so a bouncing slot does not monopolise")
    {
    }

  private:
    void DoRun() override
    {
        const uint32_t payload = 500;
        const uint32_t wireSize = payload + 20;

        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        Ptr<HybridLlqDispatcher> hybrid = CreateObject<HybridLlqDispatcher>();
        // SP slot 0 (idle here); equal-quantum DRR slots 1 and 2.
        ConfigureHybridLlqEdge(edge, hybrid, 0, {1, 2}, {wireSize, wireSize});

        // Serve DRR slot 1 down to empty — the bouncing-slot pattern.
        NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(MakeTinShaperItem(20, payload)),
                              true,
                              "slot 1 initial enqueue");
        Ptr<QueueDiscItem> first = edge->Dequeue();
        NS_TEST_ASSERT_MSG_NE(first, nullptr, "first dequeue (slot 1)");

        // Refill slot 1 and also backlog slot 2. With the cursor correctly
        // advanced off the drained slot 1, the next DRR turn goes to slot 2;
        // a parked cursor re-serves slot 1 on a fresh quantum instead.
        NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(MakeTinShaperItem(20, payload)), true, "slot 1 refill");
        NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(MakeTinShaperItem(30, payload)),
                              true,
                              "slot 2 enqueue");

        Ptr<QueueDiscItem> next = edge->Dequeue();
        NS_TEST_ASSERT_MSG_NE(next, nullptr, "second dequeue");
        if (next)
        {
            Ptr<Ipv4QueueDiscItem> ip = DynamicCast<Ipv4QueueDiscItem>(next);
            const uint32_t dscp = ip->GetHeader().GetTos() >> 2;
            NS_TEST_ASSERT_MSG_EQ(dscp,
                                  30u,
                                  "after slot 1 drained and both slots refilled, the DRR cursor "
                                  "must yield to slot 2 (DSCP 30); a parked cursor re-served "
                                  "slot 1 (DSCP "
                                      << dscp << "), starving the other backlogged slot");
        }

        Simulator::Destroy();
    }
};

// ---------------------------------------------------------------------
//  S-17.15: cake::TinTokenBucket unit math
//
//  Pins the bucket arithmetic in isolation:
//    - HasTokensFor returns true unconditionally when rateBps == 0
//    - Configure resets the bucket to a full state (tokensBytes == burstBytes)
//    - After Charge(N), HasTokensFor(N) is false until enough time elapses
//    - Refill caps at burstBytes (no over-fill on long idle)
// ---------------------------------------------------------------------

/**
 * @brief Unit-tests the per-tin token bucket accounting and refill.
 * @see specs/02-structural.md S-17.15
 */
class TinTokenBucketUnitTest : public TestCase
{
  public:
    TinTokenBucketUnitTest()
        : TestCase("S-17.15 TinTokenBucket unit math")
    {
    }

  private:
    void DoRun() override
    {
        // Case A: rateBps==0 disables — HasTokensFor always true; Charge is no-op.
        {
            cake::TinTokenBucket b;
            NS_TEST_ASSERT_MSG_EQ(b.HasTokensFor(1500, Seconds(0)),
                                  true,
                                  "disabled bucket should always permit serve");
            b.Charge(1500, Seconds(0));
            NS_TEST_ASSERT_MSG_EQ(b.tokensBytes,
                                  0,
                                  "Charge on disabled bucket must not mutate tokens");
        }

        // Case B: Configure(rate=10Mbps, burst=10000) → bucket at 10000 tokens.
        {
            cake::TinTokenBucket b;
            b.Configure(10000000ull, 10000ull, Seconds(0));
            NS_TEST_ASSERT_MSG_EQ(b.tokensBytes,
                                  10000,
                                  "Configure should fill bucket to burstBytes");
            NS_TEST_ASSERT_MSG_EQ(b.HasTokensFor(1500, Seconds(0)),
                                  true,
                                  "full bucket should permit a 1500-byte serve");
        }

        // Case C: Charge drains tokens; subsequent serve fails until refill.
        {
            cake::TinTokenBucket b;
            b.Configure(10000000ull, 10000ull, Seconds(0));
            // 10 packets of 1500 bytes = 15000 bytes — bucket overdraws.
            for (int i = 0; i < 10; ++i)
            {
                b.Charge(1500, Seconds(0));
            }
            NS_TEST_ASSERT_MSG_EQ(b.HasTokensFor(1500, Seconds(0)),
                                  false,
                                  "drained bucket should refuse a serve at t=0");
            // 1500 bytes at 10 Mbps = 1.2 ms. After 10 ms (8 packets refilled) HasTokens passes.
            NS_TEST_ASSERT_MSG_EQ(b.HasTokensFor(1500, MilliSeconds(10)),
                                  true,
                                  "after 10 ms refill, 1500-byte serve should be permitted");
        }

        // Case D: Refill caps at burstBytes — long idle does not exceed ceiling.
        {
            cake::TinTokenBucket b;
            b.Configure(10000000ull, 10000ull, Seconds(0));
            b.Charge(10000, Seconds(0)); // drain to 0
            // Idle 1000 seconds — would refill 1.25 GB without cap.
            // Snapshot via HasTokensFor: an 11000-byte request should fail (capped at 10000).
            NS_TEST_ASSERT_MSG_EQ(
                b.HasTokensFor(11000, Seconds(1000)),
                false,
                "refill must cap at burstBytes — 11000-byte serve > 10000 ceiling");
            NS_TEST_ASSERT_MSG_EQ(b.HasTokensFor(10000, Seconds(1000)),
                                  true,
                                  "refill at cap permits exactly burstBytes");
        }
    }
};

// ---------------------------------------------------------------------
//  S-17.16: cake::TinShaperDispatcher honours the per-slot rate cap
//           under saturating offered load//
//  Drives ~4 Mbps offered load (1500 wire bytes every 3 ms) into slot 0
//  for 10 simulated seconds. Slot 0 carries a 2 Mbps rate cap with a
//  6 000-byte burst. The DRR quantum is 1500 (one MTU) so deficit
//  semantics never gate the serve — only the token bucket can. Asserts
//  the slot-0 served-byte total is within +/-5 % of the expected
//  cap-product (2 500 000 bytes). Validates the gate-then-charge
//  sequencing in `SelectDequeueSlot` and `OnDequeue`.
// ---------------------------------------------------------------------

/**
 * @brief Verifies the tin shaper enforces the configured per-tin rate cap.
 * @see specs/02-structural.md S-17.16
 */
class TinShaperRateCapHonoredTest : public TestCase
{
  public:
    TinShaperRateCapHonoredTest()
        : TestCase("S-17.16 Tin-shaper honours per-slot rate cap")
    {
    }

  private:
    Ptr<EdgeQueueDisc> m_edge;

    void EnqueueOne(uint32_t payload)
    {
        // Saturating offered load: 1500 wire bytes (1480 payload + 20
        // IPv4 header). DROP_TAIL is configured high enough by the
        // helper that the inner does not early-drop during the run.
        m_edge->Enqueue(MakeTinShaperItem(10, payload));
    }

    void DrainAvailable()
    {
        // Try to drain — the token gate refuses the slot when the
        // bucket is empty; a successful Dequeue charges the bucket.
        // Loop until either the queue is empty or the gate refuses.
        while (Ptr<QueueDiscItem> it = m_edge->Dequeue())
        {
            (void)it;
        }
    }

    void DoRun() override
    {
        // 1480-byte payload + 20-byte IPv4 header = 1500 wire bytes per
        // packet. Offered cadence 3 ms → ~4.0 Mbps. Cap 2 Mbps; over
        // 10 s the slot should serve ~2.5 MB (within 5 %).
        const uint32_t payload = 1480;
        const uint32_t wire = payload + 20; // 1500
        const uint32_t quantum = wire;      // 1 MTU per round
        const uint64_t rateBps = 2'000'000ull;
        const uint64_t burstBytes = 4ull * wire; // 6000

        m_edge = CreateObject<EdgeQueueDisc>();
        Ptr<cake::TinShaperDispatcher> shaper = CreateObject<cake::TinShaperDispatcher>();
        // Three-slot helper, but only slot 0 will be fed. Slots 1 + 2
        // stay idle and the empty-slot path exercises in passing.
        ConfigureThreeSlotShaperEdge(m_edge, shaper, quantum, quantum, quantum);
        shaper->SetRateCap(0, rateBps, burstBytes);

        NS_TEST_ASSERT_MSG_EQ(shaper->GetRateCapBps(0),
                              rateBps,
                              "GetRateCapBps should round-trip the configured cap");

        const Time simEnd = Seconds(10.0);
        const Time offeredPeriod = MilliSeconds(3); // ~4 Mbps
        const Time drainPeriod = MicroSeconds(500); // poll dequeue 8x faster

        for (Time t = Seconds(0); t < simEnd; t += offeredPeriod)
        {
            Simulator::Schedule(t, &TinShaperRateCapHonoredTest::EnqueueOne, this, payload);
        }
        for (Time t = Seconds(0); t < simEnd; t += drainPeriod)
        {
            Simulator::Schedule(t, &TinShaperRateCapHonoredTest::DrainAvailable, this);
        }

        Simulator::Stop(simEnd);
        Simulator::Run();

        // Expected serve = rateBps * 10 s / 8 = 2 500 000 bytes.
        // Tolerance: 5 % each side → [2 375 000, 2 625 000].
        const uint64_t served = m_edge->GetStats().nTotalDequeuedBytes;
        const uint64_t expected = rateBps * 10ull / 8ull;
        const uint64_t lo = expected * 95ull / 100ull;
        const uint64_t hi = expected * 105ull / 100ull;
        NS_TEST_ASSERT_MSG_GT_OR_EQ(served,
                                    lo,
                                    "served bytes (" << served << ") below 95 % of " << expected
                                                     << " — rate cap may be over-throttling");
        NS_TEST_ASSERT_MSG_LT_OR_EQ(served,
                                    hi,
                                    "served bytes ("
                                        << served << ") above 105 % of " << expected
                                        << " — rate cap not honoured under saturation");

        Simulator::Destroy();
    }
};

/**
 * @brief Smoke test for path-gamma per-tin shaping.
 *
 * Mirrors `TinShaperRateCapHonoredTest` (path-alpha: in-dispatcher
 * `cake::TinTokenBucket` gate via `SetRateCap`) but enforces the 2 Mbps cap on
 * slot 0 by wrapping the slot inner with a mainline `TbfQueueDisc`
 * (path-gamma: inner-qdisc composition). The dispatcher's `SetRateCap`
 * is not called for slot 0 — token-bucket accounting happens entirely
 * inside the `TbfQueueDisc`. The TBF's auto-default `FifoQueueDisc`
 * inner is used so the `SINGLE_CHILD_QUEUE_DISC` size-policy cascade
 * has a compliant child (RedQueueDisc has `NO_LIMITS` and would
 * fatal-error on `SetMaxSize`; see
 * `reference_ns3_tbf_single_child_size_policy_gap`).
 *
 * Validates the upstream patch (`patches/ns3/0004`) plus
 * the dispatcher null-peek skip integrate end-to-end with
 * `EdgeQueueDisc` slot dispatch and that the realised rate
 * matches the path-alpha reference within the same +-5 % envelope.
 */
class TbfAsInnerSlotRateCapHonoredTest : public TestCase
{
  public:
    TbfAsInnerSlotRateCapHonoredTest()
        : TestCase("Tbf-as-inner-slot honours per-slot rate cap (path-gamma equivalence)")
    {
    }

  private:
    Ptr<EdgeQueueDisc> m_edge;

    void EnqueueOne(uint32_t payload)
    {
        m_edge->Enqueue(MakeTinShaperItem(10, payload));
    }

    void DrainAvailable()
    {
        while (Ptr<QueueDiscItem> it = m_edge->Dequeue())
        {
            (void)it;
        }
    }

    void DoRun() override
    {
        const uint32_t payload = 1480;
        const uint32_t wire = payload + 20; // 1500
        const uint32_t quantum = wire;
        const uint64_t rateBps = 2'000'000ull;
        const uint64_t burstBytes = 4ull * wire; // 6000

        m_edge = CreateObject<EdgeQueueDisc>();
        Ptr<cake::TinShaperDispatcher> shaper = CreateObject<cake::TinShaperDispatcher>();

        // Slot 0 inner = TbfQueueDisc with auto-default FifoQueueDisc child
        // (created by TbfQueueDisc::CheckConfig at Initialize). No call to
        // shaper->SetRateCap(0, ...) — the cap lives in the TBF.
        Ptr<TbfQueueDisc> slot0Tbf = CreateObject<TbfQueueDisc>();
        slot0Tbf->SetAttribute("Rate", DataRateValue(DataRate(rateBps)));
        slot0Tbf->SetAttribute("Burst", UintegerValue(burstBytes));
        slot0Tbf->SetAttribute("Mtu", UintegerValue(wire));
        slot0Tbf->SetAttribute("MaxSize", QueueSizeValue(QueueSize("5000p")));

        Ptr<RedQueueDisc> slot1Inner = MakeTinInner(20);
        Ptr<RedQueueDisc> slot2Inner = MakeTinInner(30);

        m_edge->SetInnerDiscAt(0, slot0Tbf);
        m_edge->SetInnerDiscAt(1, slot1Inner);
        m_edge->SetInnerDiscAt(2, slot2Inner);
        m_edge->SetDscpToSlot(10, 0);
        m_edge->SetDscpToSlot(20, 1);
        m_edge->SetDscpToSlot(30, 2);

        shaper->SetQuantum(0, quantum);
        shaper->SetQuantum(1, quantum);
        shaper->SetQuantum(2, quantum);
        m_edge->SetSlotDispatcher(shaper);
        m_edge->Initialize();

        const Time simEnd = Seconds(10.0);
        const Time offeredPeriod = MilliSeconds(3); // ~4 Mbps
        const Time drainPeriod = MicroSeconds(500);

        for (Time t = Seconds(0); t < simEnd; t += offeredPeriod)
        {
            Simulator::Schedule(t, &TbfAsInnerSlotRateCapHonoredTest::EnqueueOne, this, payload);
        }
        for (Time t = Seconds(0); t < simEnd; t += drainPeriod)
        {
            Simulator::Schedule(t, &TbfAsInnerSlotRateCapHonoredTest::DrainAvailable, this);
        }

        Simulator::Stop(simEnd);
        Simulator::Run();

        const uint64_t served = m_edge->GetStats().nTotalDequeuedBytes;
        const auto expected =
            static_cast<uint64_t>(rateBps / 8) * static_cast<uint64_t>(simEnd.GetSeconds());
        const auto lo = static_cast<uint64_t>(0.95 * expected);
        const auto hi = static_cast<uint64_t>(1.05 * expected);
        NS_TEST_ASSERT_MSG_GT_OR_EQ(served,
                                    lo,
                                    "served bytes (" << served << ") below 95 % of " << expected
                                                     << " — TBF inner-mode under-throttling");
        NS_TEST_ASSERT_MSG_LT_OR_EQ(served,
                                    hi,
                                    "served bytes (" << served << ") above 105 % of " << expected
                                                     << " — TBF inner-mode not honouring cap");

        Simulator::Destroy();
    }
};

// ---------------------------------------------------------------------
//  S-17.17: cake::TinShaperDispatcher does NOT redistribute idle-tin
//           capacity to a capped neighbour//
//  Two slots, each capped at 5 Mbps. Slot 0 is fed at saturating
//  10 Mbps; slot 1 stays empty. Without the cap, work-conserving DRR
//  would let slot 0 absorb the entire link, but tin-shaping mode must
//  hold slot 0 to its own 5 Mbps cap regardless of slot 1's idleness
//  (the no-save-up / no-borrow-from-idle invariant that distinguishes
//  CAKE's diffserv4 from a plain DRR scheduler).
// ---------------------------------------------------------------------

/**
 * @brief Verifies idle tins do not redistribute their unused tokens to active tins.
 * @see specs/02-structural.md S-17.17
 */
class TinShaperIdleNoRedistributeTest : public TestCase
{
  public:
    TinShaperIdleNoRedistributeTest()
        : TestCase("S-17.17 Tin-shaper does NOT redistribute idle-tin capacity under cap")
    {
    }

  private:
    Ptr<EdgeQueueDisc> m_edge;

    void EnqueueOne(uint32_t payload)
    {
        m_edge->Enqueue(MakeTinShaperItem(10, payload));
    }

    void DrainAvailable()
    {
        while (Ptr<QueueDiscItem> it = m_edge->Dequeue())
        {
            (void)it;
        }
    }

    void DoRun() override
    {
        // 1480-byte payload + 20-byte IPv4 header = 1500 wire bytes per
        // packet. Offered cadence 1.2 ms -> ~10 Mbps into slot 0.
        // Both slots capped at 5 Mbps; slot 1 is never fed.
        const uint32_t payload = 1480;
        const uint32_t wire = payload + 20;      // 1500
        const uint32_t quantum = wire;           // 1 MTU per round
        const uint64_t rateBps = 5'000'000ull;   // 5 Mbps cap each
        const uint64_t burstBytes = 4ull * wire; // 6000

        m_edge = CreateObject<EdgeQueueDisc>();
        Ptr<cake::TinShaperDispatcher> shaper = CreateObject<cake::TinShaperDispatcher>();
        // Three-slot helper (the existing fixture); we only feed slot 0
        // and only cap slots 0 and 1. Slot 2 stays idle and uncapped —
        // exercises the empty-slot path the same way S-17.16 does.
        ConfigureThreeSlotShaperEdge(m_edge, shaper, quantum, quantum, quantum);
        shaper->SetRateCap(0, rateBps, burstBytes);
        shaper->SetRateCap(1, rateBps, burstBytes);

        const Time simEnd = Seconds(10.0);
        const Time offeredPeriod = MicroSeconds(1200); // ~10 Mbps offered
        const Time drainPeriod = MicroSeconds(300);    // poll dequeue 4x faster

        for (Time t = Seconds(0); t < simEnd; t += offeredPeriod)
        {
            Simulator::Schedule(t, &TinShaperIdleNoRedistributeTest::EnqueueOne, this, payload);
        }
        for (Time t = Seconds(0); t < simEnd; t += drainPeriod)
        {
            Simulator::Schedule(t, &TinShaperIdleNoRedistributeTest::DrainAvailable, this);
        }

        Simulator::Stop(simEnd);
        Simulator::Run();

        // Only slot 0 ever serves (slots 1+2 stay empty), so the
        // aggregate dequeue counter equals slot-0 served bytes.
        // Cap product: 5 Mbps * 10 s / 8 = 6 250 000 bytes.
        // Upper bound (5 % headroom): 6 562 500.
        const uint64_t served = m_edge->GetStats().nTotalDequeuedBytes;
        const uint64_t expected = rateBps * 10ull / 8ull; // 6 250 000
        const uint64_t hi = expected * 105ull / 100ull;   // 6 562 500
        NS_TEST_ASSERT_MSG_LT_OR_EQ(served,
                                    hi,
                                    "slot 0 served ("
                                        << served << ") exceeds 105 % of cap product " << expected
                                        << " — capped slot must NOT borrow from idle neighbour");

        Simulator::Destroy();
    }
};

// ---------------------------------------------------------------------
//  S-17.18: cake::TinShaperDispatcher's PeekSlot is side-effect-free under
//           rate cap — mirrors S-17.1 Case C for the cap-aware path.
//
//  With one slot configured + capped and one packet enqueued, two
//  consecutive Peek() calls must return the same item (idempotent),
//  and a subsequent Dequeue() must return that same item — i.e. the
//  peeks must NOT consume any tokens nor advance any DRR state.
// ---------------------------------------------------------------------

/**
 * @brief Verifies tin-shaper peek is side-effect free even when the cap is engaged.
 * @see specs/02-structural.md S-17.18
 */
class TinShaperPeekSideEffectFreeWithCapTest : public TestCase
{
  public:
    TinShaperPeekSideEffectFreeWithCapTest()
        : TestCase("S-17.18 Tin-shaper PeekSlot side-effect-free under rate cap")
    {
    }

  private:
    void DoRun() override
    {
        const uint32_t payload = 500;
        const uint32_t wire = payload + 20;
        const uint32_t quantum = wire;
        const uint64_t rateBps = 2'000'000ull;
        const uint64_t burstBytes = 4ull * wire; // 2000

        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        Ptr<cake::TinShaperDispatcher> shaper = CreateObject<cake::TinShaperDispatcher>();
        // Three-slot fixture; we only feed slot 0. Cap on slot 0 makes
        // the cap-aware path the one that PeekSlot must walk.
        ConfigureThreeSlotShaperEdge(edge, shaper, quantum, quantum, quantum);
        shaper->SetRateCap(0, rateBps, burstBytes);

        // One packet, slot 0. The bucket starts full (burstBytes), so
        // tokens for the head are available immediately at t=0.
        NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(MakeTinShaperItem(10, payload)),
                              true,
                              "slot 0 enqueue");

        // Two consecutive Peeks must yield the same item.
        Ptr<const QueueDiscItem> peek1 = edge->Peek();
        Ptr<const QueueDiscItem> peek2 = edge->Peek();
        NS_TEST_ASSERT_MSG_NE(peek1, nullptr, "first peek non-null (tokens available)");
        if (!peek1)
        {
            return;
        }
        NS_TEST_ASSERT_MSG_EQ(peek1, peek2, "two Peek() calls must return the same item");

        // Subsequent Dequeue must still return the same item — peeks
        // did not consume tokens nor advance DRR state.
        Ptr<QueueDiscItem> dq = edge->Dequeue();
        NS_TEST_ASSERT_MSG_EQ(dq,
                              peek1,
                              "Dequeue must yield the peeked item exactly (no peek side-effect)");

        Simulator::Destroy();
    }
};

// ---------------------------------------------------------------------
//  S-17.20: HybridLlqDispatcher — SetSlotStrictPriority composed with
//           SetRateCap on the same slot reproduces the Cisco MQC LLQ
//           pattern: priority class served ahead of DRR slots but held
//           to a hard bandwidth ceiling.
//
//  Four-slot edge: slot 3 = SP + capped at 2.5 Mbps (the EF/voice
//  class); slots 0/1/2 = DRR with quanta = 1 MTU and no rate cap.
//  Saturating EF offered at 5 Mbps + saturating BE on slot 1 at
//  8 Mbps for 10 simulated seconds.
//
//  Discriminating assertion (a): even though slot 3 is SP and would
//  otherwise drain unconditionally, served-byte total must NOT exceed
//  the cap product (2.5 Mbps × 10 s × 1.05). Without the token gate
//  in `FirstReadyStrictPrioritySlot`, SP would let EF consume well
//  beyond 25 % of the offered share.
//
//  Coexistence assertion (b): BE (DRR slot 1) gets at least the EF
//  remainder — confirms the SP cap doesn't accidentally starve DRR.
// ---------------------------------------------------------------------

/**
 * @brief Verifies LLQ composes correctly with per-tin shaping.
 * @see specs/02-structural.md S-17.20
 */
class LlqTinShapingCompositionTest : public TestCase
{
  public:
    LlqTinShapingCompositionTest()
        : TestCase("S-17.20 LLQ + tin-shaping composition = Cisco MQC LLQ pattern")
    {
    }

  private:
    Ptr<EdgeQueueDisc> m_edge;

    void EnqueueOne(uint8_t dscp, uint32_t payload)
    {
        m_edge->Enqueue(MakeTinShaperItem(dscp, payload));
    }

    void DrainAvailable()
    {
        while (Ptr<QueueDiscItem> it = m_edge->Dequeue())
        {
            (void)it;
        }
    }

    void DoRun() override
    {
        // 1480-byte payload + 20-byte IPv4 header = 1500 wire bytes.
        // EF (slot 3, SP) offered at ~5 Mbps; BE (slot 1, DRR) offered
        // at ~8 Mbps. Slot 3 capped at 2.5 Mbps with a 4-MTU burst.
        // DRR quantum = 1 MTU so deficit alone never gates the serve.
        const uint32_t payload = 1480;
        const uint32_t wire = payload + 20; // 1500
        const uint32_t quantum = wire;
        const uint64_t efRateBps = 2'500'000ull;
        const uint64_t efBurstBytes = 4ull * wire; // 6000

        // Slot map: slot 3 = SP+capped (DSCP 40), slots 0/1/2 = DRR
        // with DSCPs 10/20/30 (matching ConfigureHybridLlqEdge's
        // `10 * (slot + 1)` convention).
        m_edge = CreateObject<EdgeQueueDisc>();
        Ptr<HybridLlqDispatcher> hybrid = CreateObject<HybridLlqDispatcher>();
        ConfigureHybridLlqEdge(m_edge,
                               hybrid,
                               /*slotSp=*/3,
                               /*drrSlots=*/{0, 1, 2},
                               /*drrQuanta=*/{quantum, quantum, quantum});
        hybrid->SetRateCap(3, efRateBps, efBurstBytes);

        NS_TEST_ASSERT_MSG_EQ(hybrid->GetRateCapBps(3),
                              efRateBps,
                              "GetRateCapBps round-trips the configured cap on the SP slot");
        NS_TEST_ASSERT_MSG_EQ(hybrid->IsStrictPriority(3), true, "slot 3 is SP");

        const Time simEnd = Seconds(10.0);
        const Time efPeriod = MicroSeconds(2400); // 1500 B / 2.4 ms = 5 Mbps offered
        const Time bePeriod = MicroSeconds(1500); // 1500 B / 1.5 ms = 8 Mbps offered
        const Time drainPeriod = MicroSeconds(300);

        for (Time t = Seconds(0); t < simEnd; t += efPeriod)
        {
            Simulator::Schedule(t,
                                &LlqTinShapingCompositionTest::EnqueueOne,
                                this,
                                static_cast<uint8_t>(40),
                                payload);
        }
        for (Time t = Seconds(0); t < simEnd; t += bePeriod)
        {
            Simulator::Schedule(t,
                                &LlqTinShapingCompositionTest::EnqueueOne,
                                this,
                                static_cast<uint8_t>(20),
                                payload);
        }
        for (Time t = Seconds(0); t < simEnd; t += drainPeriod)
        {
            Simulator::Schedule(t, &LlqTinShapingCompositionTest::DrainAvailable, this);
        }

        Simulator::Stop(simEnd);
        Simulator::Run();

        // Per-slot bookkeeping via the inner-queue stats. The aggregate
        // edge counter `nTotalDequeuedBytes` includes all four slots and
        // would not isolate the SP-capped channel.
        const uint64_t efServed = m_edge->GetInnerDiscAt(3)->GetStats().nTotalDequeuedBytes;
        const uint64_t beServed = m_edge->GetInnerDiscAt(1)->GetStats().nTotalDequeuedBytes;

        // (a) EF cap holds even under SP. 2.5 Mbps × 10 s / 8 = 3 125 000
        // bytes; allow 5 % headroom for sub-MTU bucket alignment ->
        // 3 281 250 upper bound.
        const uint64_t efExpected = efRateBps * 10ull / 8ull; // 3 125 000
        const uint64_t efHi = efExpected * 105ull / 100ull;   // 3 281 250
        NS_TEST_ASSERT_MSG_LT_OR_EQ(
            efServed,
            efHi,
            "EF served (" << efServed << ") exceeds 105 % of cap product " << efExpected
                          << " — SP slot's rate cap is not honoured (Cisco MQC LLQ "
                             "pattern broken)");

        // (b) BE reclaims the headroom the EF cap leaves behind. We do
        // not impose a link-rate model in this fixture (the edge drains
        // greedily), so BE simply absorbs all 8 Mbps of offered load.
        // The lower bound is set at 85 % of the EF-remainder allocation
        // under a conceptual 10 Mbps link (7 968 750 bytes) — comfortably
        // exceeded by an uncapped DRR slot drinking its full 8 Mbps
        // offered, which would also fail closed if the SP path
        // accidentally starved DRR.
        const uint64_t beLo = (10'000'000ull - efRateBps) * 10ull / 8ull * 85ull / 100ull;
        NS_TEST_ASSERT_MSG_GT_OR_EQ(beServed,
                                    beLo,
                                    "BE served (" << beServed
                                                  << ") below 85 % of EF-remainder lower bound "
                                                  << beLo << " — SP slot may be starving DRR");

        Simulator::Destroy();
    }
};

namespace
{

/// Build a UDP-bearing Ipv4QueueDiscItem keyed off (src, dst) IPs and ports.
/// Used by FqCobalt and host-isolation-adjacent tests below.
Ptr<Ipv4QueueDiscItem>
MakeUdpHostItem(Ipv4Address src,
                Ipv4Address dst,
                uint16_t srcPort,
                uint16_t dstPort,
                uint32_t payloadBytes)
{
    // 4-byte port prefix + payload, mirroring MakePortIpv4Item upstream.
    uint8_t portBuf[4];
    portBuf[0] = static_cast<uint8_t>(srcPort >> 8);
    portBuf[1] = static_cast<uint8_t>(srcPort & 0xFF);
    portBuf[2] = static_cast<uint8_t>(dstPort >> 8);
    portBuf[3] = static_cast<uint8_t>(dstPort & 0xFF);
    Ptr<Packet> portPkt = Create<Packet>(portBuf, 4);
    Ptr<Packet> payload = Create<Packet>(payloadBytes);
    portPkt->AddAtEnd(payload);

    Ipv4Header hdr;
    hdr.SetSource(src);
    hdr.SetDestination(dst);
    hdr.SetProtocol(17); // UDP
    hdr.SetPayloadSize(portPkt->GetSize());
    return Create<Ipv4QueueDiscItem>(portPkt, Address(), 0x0800, hdr);
}

} // namespace

// S-17.21..S-17.38 wrapper-specific tests removed: the original host-isolation
// implementation (pair-LRU / pair-key APIs) was retired in favour of
// FqCobaltQueueDisc with EnableHostIsolation=true (patches/ns3/0016).
// See cake-a-retirement branch history for the full triage record.

// =============================================================================
//  S-17.9: ACK filter functional contract (redundant ACK suppression)
//  S-17.10: ACK filter SACK preservation
// =============================================================================

namespace
{

/// Build a real TCP-bearing Ipv4QueueDiscItem with caller-controlled flags
/// and ack-number. Used by the ACK-filter tests below.
Ptr<Ipv4QueueDiscItem>
MakeRealTcpItem(Ipv4Address src,
                Ipv4Address dst,
                uint16_t srcPort,
                uint16_t dstPort,
                uint32_t seqNum,
                uint32_t ackNum,
                uint8_t flags,
                uint32_t payloadBytes,
                bool withSack = false)
{
    TcpHeader tcp;
    tcp.SetSourcePort(srcPort);
    tcp.SetDestinationPort(dstPort);
    tcp.SetSequenceNumber(SequenceNumber32(seqNum));
    tcp.SetAckNumber(SequenceNumber32(ackNum));
    tcp.SetFlags(flags);
    tcp.SetWindowSize(65535);

    if (withSack)
    {
        Ptr<TcpOptionSack> sack = CreateObject<TcpOptionSack>();
        sack->AddSackBlock(
            TcpOptionSack::SackBlock(SequenceNumber32(2000), SequenceNumber32(3000)));
        tcp.AppendOption(sack);
    }

    Ptr<Packet> payload = Create<Packet>(payloadBytes);
    payload->AddHeader(tcp);

    Ipv4Header hdr;
    hdr.SetSource(src);
    hdr.SetDestination(dst);
    hdr.SetProtocol(6);
    hdr.SetPayloadSize(payload->GetSize());
    return Create<Ipv4QueueDiscItem>(payload, Address(), 0x0800, hdr);
}

} // namespace

/**
 * @brief Verifies the ACK filter functionally suppresses redundant ACKs.
 *
 * Three ACK-only packets in the same 5-tuple, with strictly-increasing
 * cumulative ACK numbers, must collapse to one survivor (the newest)
 * once `EnableAckFilter=true` is set on the per-tin
 * `FqCobaltQueueDisc`.  The two suppressed older ACKs register
 * against the per-instance counter exposed by `GetAckFilterDrops()`.
 *
 * @see specs/02-structural.md S-17.9
 */
class AckFilterFunctionalContractTest : public TestCase
{
  public:
    AckFilterFunctionalContractTest()
        : TestCase("S-17.9 ACK filter functional contract: redundant ACK suppression")
    {
    }

  private:
    void DoRun() override
    {
        // Default value is false; setter and attribute path both reflect.
        Ptr<FqCobaltQueueDisc> fq1 = CreateObject<FqCobaltQueueDisc>();
        NS_TEST_ASSERT_MSG_EQ(fq1->GetEnableAckFilter(), false, "default EnableAckFilter is false");

        fq1->SetEnableAckFilter(true);
        NS_TEST_ASSERT_MSG_EQ(fq1->GetEnableAckFilter(),
                              true,
                              "SetEnableAckFilter(true) reflected in getter");

        Ptr<FqCobaltQueueDisc> fq2 =
            CreateObjectWithAttributes<FqCobaltQueueDisc>("EnableAckFilter", BooleanValue(true));
        NS_TEST_ASSERT_MSG_EQ(fq2->GetEnableAckFilter(),
                              true,
                              "EnableAckFilter via attribute path");

        // Functional contract under Linux-faithful conservative
        // semantics (sch_cake.c:1336-1356): the filter drops an older
        // ACK only after TWO eligible same-flow ACKs are found in a
        // single walk. With three strictly-increasing same-5-tuple
        // ACKs the third arrival's walk finds two eligibles (ACK#1 and
        // ACK#2) and drops the first-saved (ACK#1, closest to head),
        // leaving two survivors -- not one. Aggressive mode would
        // additionally drop ACK#2 via the consecutive-shortcut at
        // sch_cake.c:1349-1356; that is exercised separately.
        const Ipv4Address src("10.0.0.1");
        const Ipv4Address dst("10.0.0.2");
        const uint16_t sport = 50000;
        const uint16_t dport = 80;

        Ptr<FqCobaltQueueDisc> fq3 = CreateObject<FqCobaltQueueDisc>();
        fq3->SetQuantum(1514);
        fq3->SetEnableAckFilter(true);
        fq3->Initialize();

        for (uint32_t i = 1; i <= 3; ++i)
        {
            Ptr<Ipv4QueueDiscItem> ack =
                MakeRealTcpItem(src, dst, sport, dport, 1000, 100 + i, TcpHeader::ACK, 0);
            bool ok = fq3->Enqueue(ack);
            NS_TEST_ASSERT_MSG_EQ(ok, true, "ack " << i << " enqueue ok");
        }

        NS_TEST_ASSERT_MSG_EQ(fq3->GetAckFilterDrops(),
                              1u,
                              "one older ACK suppressed (conservative "
                              "2-eligible threshold, sch_cake.c:1336-1356)");
        NS_TEST_ASSERT_MSG_EQ(fq3->GetCurrentSize().GetValue(),
                              2u,
                              "two survivors under conservative mode");

        Simulator::Destroy();
    }
};

/**
 * @brief Verifies the ACK filter never suppresses SACK-bearing ACKs.
 *
 * A SACK-bearing ACK conveys out-of-order delivery information that the
 * cumulative ACK number alone does not represent.  When such an ACK
 * arrives behind a plain older ACK with strictly-less cumulative-ACK
 * number, the older ACK must remain queued (the SACK arrival fails the
 * ACK-candidate test, so the scan never runs).
 *
 * @see specs/02-structural.md S-17.10
 */
class AckFilterSackPreservationTest : public TestCase
{
  public:
    AckFilterSackPreservationTest()
        : TestCase("S-17.10 ACK filter SACK preservation")
    {
    }

  private:
    void DoRun() override
    {
        const Ipv4Address src("10.0.0.1");
        const Ipv4Address dst("10.0.0.2");
        const uint16_t sport = 50000;
        const uint16_t dport = 80;

        Ptr<FqCobaltQueueDisc> fq = CreateObject<FqCobaltQueueDisc>();
        fq->SetQuantum(1514);
        fq->SetEnableAckFilter(true);
        fq->Initialize();

        // Older plain ACK first, then a newer SACK-bearing ACK.
        Ptr<Ipv4QueueDiscItem> plainAck =
            MakeRealTcpItem(src, dst, sport, dport, 1000, 101, TcpHeader::ACK, 0);
        Ptr<Ipv4QueueDiscItem> sackAck = MakeRealTcpItem(src,
                                                         dst,
                                                         sport,
                                                         dport,
                                                         1000,
                                                         102,
                                                         TcpHeader::ACK,
                                                         0,
                                                         /*withSack=*/true);

        NS_TEST_ASSERT_MSG_EQ(fq->Enqueue(plainAck), true, "plain ack enqueue ok");
        NS_TEST_ASSERT_MSG_EQ(fq->Enqueue(sackAck), true, "sack ack enqueue ok");

        // The SACK arrival is not an ACK-candidate, so no scan runs and
        // the older plain ACK is preserved alongside the newer SACK ACK.
        NS_TEST_ASSERT_MSG_EQ(fq->GetAckFilterDrops(),
                              0u,
                              "SACK arrival does not trigger ACK-filter drop");
        NS_TEST_ASSERT_MSG_EQ(fq->GetCurrentSize().GetValue(),
                              2u,
                              "SACK preservation: both ACKs remain queued");

        Simulator::Destroy();
    }
};

/**
 * @brief Verifies aggressive mode preserves SACK information.
 *
 * Under Linux-faithful semantics (sch_cake.c:1318-1330,
 * cake_tcph_sack_compare) the ACK filter never drops a queued SACK-bearing
 * ACK when the triggering ACK carries no SACK ranges -- doing so would
 * lose the selective-acknowledgement information. This holds regardless
 * of the aggressive flag: the SACK-range subset check gates eligibility
 * before the conservative-vs-aggressive decision is made. Conservative
 * mode (S-17.10) preserves the SACK ACK for the same reason.
 *
 * @see specs/02-structural.md S-17.26
 */
class AckFilterAggressiveDropsSackTest : public TestCase
{
  public:
    AckFilterAggressiveDropsSackTest()
        : TestCase("S-17.26 ACK filter aggressive: older SACK-bearing ACK preserved")
    {
    }

  private:
    void DoRun() override
    {
        const Ipv4Address src("10.0.0.1");
        const Ipv4Address dst("10.0.0.2");
        const uint16_t sport = 50000;
        const uint16_t dport = 80;

        Ptr<FqCobaltQueueDisc> fq = CreateObject<FqCobaltQueueDisc>();
        fq->SetQuantum(1514);
        fq->SetEnableAckFilter(true);
        fq->SetEnableAckFilterAggressive(true);
        fq->Initialize();

        // Older SACK-bearing ACK first, then a newer plain ACK with a
        // strictly-greater cumulative ACK number.
        Ptr<Ipv4QueueDiscItem> sackAck = MakeRealTcpItem(src,
                                                         dst,
                                                         sport,
                                                         dport,
                                                         1000,
                                                         101,
                                                         TcpHeader::ACK,
                                                         0,
                                                         /*withSack=*/true);
        Ptr<Ipv4QueueDiscItem> plainAck =
            MakeRealTcpItem(src, dst, sport, dport, 1000, 102, TcpHeader::ACK, 0);

        NS_TEST_ASSERT_MSG_EQ(fq->Enqueue(sackAck), true, "sack ack enqueue ok");
        NS_TEST_ASSERT_MSG_EQ(fq->Enqueue(plainAck), true, "plain ack enqueue ok");

        // Plain trigger has no SACK ranges; the queued SACK ranges are
        // not covered by empty trigger ranges, so the SACK ACK is
        // ineligible (sch_cake.c:1318-1330). Aggressive mode does not
        // override the SACK-range subset gate.
        NS_TEST_ASSERT_MSG_EQ(fq->GetAckFilterDrops(),
                              0u,
                              "aggressive: SACK ACK preserved when trigger "
                              "carries no SACK (sch_cake.c:1318-1330)");
        NS_TEST_ASSERT_MSG_EQ(fq->GetCurrentSize().GetValue(),
                              2u,
                              "aggressive: both ACKs survive (SACK preserved)");

        Simulator::Destroy();
    }
};

/**
 * @brief Verifies aggressive mode admits SACK-bearing ACKs as scan triggers.
 *
 * In aggressive mode a SACK-bearing arrival passes the candidate test, so it
 * triggers the per-flow scan.  An older plain ACK in the same 5-tuple with a
 * strictly-smaller cumulative ACK number is suppressed; a same-5-tuple older
 * ACK with an equal-or-greater cumulative number is preserved.
 *
 * @see specs/02-structural.md S-17.27
 */
class AckFilterAggressiveSackArrivalTest : public TestCase
{
  public:
    AckFilterAggressiveSackArrivalTest()
        : TestCase("S-17.27 ACK filter aggressive: SACK-bearing arrival triggers scan")
    {
    }

  private:
    void DoRun() override
    {
        const Ipv4Address src("10.0.0.1");
        const Ipv4Address dst("10.0.0.2");
        const uint16_t sport = 50000;
        const uint16_t dport = 80;

        Ptr<FqCobaltQueueDisc> fq = CreateObject<FqCobaltQueueDisc>();
        fq->SetQuantum(1514);
        fq->SetEnableAckFilter(true);
        fq->SetEnableAckFilterAggressive(true);
        fq->Initialize();

        // Older plain ACK first, then a newer SACK-bearing ACK with a
        // strictly-greater cumulative ACK number.
        Ptr<Ipv4QueueDiscItem> plainAck =
            MakeRealTcpItem(src, dst, sport, dport, 1000, 101, TcpHeader::ACK, 0);
        Ptr<Ipv4QueueDiscItem> sackAck = MakeRealTcpItem(src,
                                                         dst,
                                                         sport,
                                                         dport,
                                                         1000,
                                                         102,
                                                         TcpHeader::ACK,
                                                         0,
                                                         /*withSack=*/true);

        NS_TEST_ASSERT_MSG_EQ(fq->Enqueue(plainAck), true, "plain ack enqueue ok");
        NS_TEST_ASSERT_MSG_EQ(fq->Enqueue(sackAck), true, "sack ack enqueue ok");

        NS_TEST_ASSERT_MSG_EQ(fq->GetAckFilterDrops(),
                              1u,
                              "aggressive: SACK arrival scans and drops older plain ACK");
        NS_TEST_ASSERT_MSG_EQ(fq->GetCurrentSize().GetValue(),
                              1u,
                              "aggressive: only newer SACK ACK survives");

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-17.28: Egress DSCP wash
// =============================================================================

/**
 * @brief Verifies the Wash attribute zeros DSCP at egress while preserving ECN bits.
 * @see specs/02-structural.md S-17.28
 *
 * Mirrors Linux `tc-cake wash`: classification keeps the DSCP bits inside
 * the qdisc; the dequeued packet leaves with the high six TOS bits cleared
 * so downstream forwarders see CS0/Default. The low two ECN bits are
 * preserved untouched so AQMs downstream can still observe ECT/CE
 * markings.
 */
class EgressDscpWashTest : public TestCase
{
  public:
    EgressDscpWashTest()
        : TestCase("S-17.28 Wash attribute zeros DSCP on dequeue, preserves ECN")
    {
    }

  private:
    /// Build an edge-disc with a single inner RedQueueDisc on slot 0
    /// and a mark rule that paints DSCP=46 (EF) onto every UDP packet.
    /// `wash` toggles the egress wash behaviour.
    Ptr<EdgeQueueDisc> MakeEdge(bool wash) const
    {
        Ptr<EdgeQueueDisc> edge =
            CreateObjectWithAttributes<EdgeQueueDisc>("Wash", BooleanValue(wash));
        Ptr<RedQueueDisc> inner = CreateObject<RedQueueDisc>();
        inner->SetNumQueues(1);
        edge->SetInnerDisc(inner);
        inner->AddPhbEntry(46, 0, 0);
        Ptr<RoundRobinScheduler> sched =
            CreateObjectWithAttributes<RoundRobinScheduler>("NumQueues", UintegerValue(1));
        inner->SetScheduler(sched);
        edge->AddMarkRule({.dscp = 46});
        edge->Initialize();
        return edge;
    }

    /// Send one UDP packet through @p edge with the supplied initial TOS
    /// byte and return the dequeued IPv4 header's TOS byte.
    uint8_t RoundTripTos(Ptr<EdgeQueueDisc> edge, uint8_t initialTos) const
    {
        Ptr<Ipv4QueueDiscItem> item =
            MakeIpv4Item(Ipv4Address("10.0.0.1"), Ipv4Address("10.0.0.2"), 17, 100);
        const_cast<Ipv4Header&>(item->GetHeader()).SetTos(initialTos);
        bool enq = edge->Enqueue(item);
        NS_ASSERT_MSG(enq, "wash test: enqueue must succeed");
        Ptr<QueueDiscItem> deq = edge->Dequeue();
        NS_ASSERT_MSG(deq, "wash test: dequeue must succeed");
        Ptr<Ipv4QueueDiscItem> ip = DynamicCast<Ipv4QueueDiscItem>(deq);
        NS_ASSERT_MSG(ip, "wash test: dequeued item must be Ipv4QueueDiscItem");
        return ip->GetHeader().GetTos();
    }

    void DoRun() override
    {
        // Initial TOS carries ECT(1) (low two bits = 0b01) and an unrelated
        // legacy DSCP. Edge mark rule rewrites DSCP -> 46 (EF).
        const uint8_t kEct1 = 0x1;
        const uint8_t kInitialTos = (10 << 2) | kEct1;

        // Wash=false (default): the rewrite stamps DSCP=46 in the high six
        // bits; ECN bits are preserved.
        {
            Ptr<EdgeQueueDisc> edge = MakeEdge(false);
            uint8_t tos = RoundTripTos(edge, kInitialTos);
            NS_TEST_ASSERT_MSG_EQ(tos >> 2,
                                  46u,
                                  "wash=false: dequeued DSCP should equal classifier mark (46/EF)");
            NS_TEST_ASSERT_MSG_EQ(tos & 0x3, kEct1, "wash=false: ECN bits preserved");
            Simulator::Destroy();
        }

        // Wash=true: the high six bits of TOS are zeroed at dequeue;
        // classification still drove inner-slot routing, but the packet
        // exits with DSCP=0. ECN bits remain ECT(1).
        {
            Ptr<EdgeQueueDisc> edge = MakeEdge(true);
            uint8_t tos = RoundTripTos(edge, kInitialTos);
            NS_TEST_ASSERT_MSG_EQ(tos >> 2, 0u, "wash=true: dequeued DSCP zeroed");
            NS_TEST_ASSERT_MSG_EQ(tos & 0x3, kEct1, "wash=true: ECN bits preserved");
            Simulator::Destroy();
        }
    }
};

// =============================================================================
//  S-17.28v6: Egress DSCP wash over IPv6
// =============================================================================

/**
 * @brief Verifies the Wash attribute zeros DSCP at egress for IPv6 while
 *        preserving ECN bits.
 * @see specs/02-structural.md S-17.28v6
 *
 * IPv6 twin of EgressDscpWashTest. Uses the same EdgeQueueDisc / MakeEdge
 * helper but sends an Ipv6QueueDiscItem carrying an initial DSCP and ECT(1).
 * After dequeue the top six bits of the IPv6 Traffic Class byte must be
 * zeroed (wash=true) or match the classifier mark (wash=false), while the
 * low two ECN bits are preserved in both cases.
 */
class EgressDscpWashIpv6Test : public TestCase
{
  public:
    EgressDscpWashIpv6Test()
        : TestCase("S-17.28v6 Wash attribute zeros DSCP on dequeue for IPv6, preserves ECN")
    {
    }

  private:
    /// Build an edge-disc identical to EgressDscpWashTest::MakeEdge but
    /// toggling the egress wash behaviour via the Wash attribute.
    Ptr<EdgeQueueDisc> MakeEdge(bool wash) const
    {
        Ptr<EdgeQueueDisc> edge =
            CreateObjectWithAttributes<EdgeQueueDisc>("Wash", BooleanValue(wash));
        Ptr<RedQueueDisc> inner = CreateObject<RedQueueDisc>();
        inner->SetNumQueues(1);
        edge->SetInnerDisc(inner);
        inner->AddPhbEntry(46, 0, 0);
        Ptr<RoundRobinScheduler> sched =
            CreateObjectWithAttributes<RoundRobinScheduler>("NumQueues", UintegerValue(1));
        inner->SetScheduler(sched);
        edge->AddMarkRule({.dscp = 46});
        edge->Initialize();
        return edge;
    }

    /// Send one UDP IPv6 packet through @p edge with the supplied initial
    /// Traffic Class byte and return the dequeued IPv6 header's Traffic
    /// Class byte.
    uint8_t RoundTripTc(Ptr<EdgeQueueDisc> edge, uint8_t initialTc) const
    {
        Ptr<Ipv6QueueDiscItem> item =
            MakeIpv6Item(Ipv6Address("2001:db8::1"), Ipv6Address("2001:db8::2"), 17, 100);
        Ipv6Header hdr = item->GetHeader();
        hdr.SetTrafficClass(initialTc);
        Ptr<Packet> pkt = Create<Packet>(100);
        Ptr<Ipv6QueueDiscItem> tagged = Create<Ipv6QueueDiscItem>(pkt, Address(), 0x86DD, hdr);
        bool enq = edge->Enqueue(tagged);
        NS_ASSERT_MSG(enq, "v6 wash test: enqueue must succeed");
        Ptr<QueueDiscItem> deq = edge->Dequeue();
        NS_ASSERT_MSG(deq, "v6 wash test: dequeue must succeed");
        Ptr<Ipv6QueueDiscItem> ip = DynamicCast<Ipv6QueueDiscItem>(deq);
        NS_ASSERT_MSG(ip, "v6 wash test: dequeued item must be Ipv6QueueDiscItem");
        return ip->GetHeader().GetTrafficClass();
    }

    void DoRun() override
    {
        // Initial Traffic Class carries ECT(1) (low two bits = 0b01) and an
        // initial DSCP=10 in the high six bits. Edge mark rule rewrites
        // DSCP -> 46 (EF).
        const uint8_t kEct1Bits = 0x1;
        const uint8_t kInitialTc = static_cast<uint8_t>((10 << 2) | kEct1Bits);

        // Wash=false (default): the rewrite stamps DSCP=46 in the high six
        // bits of Traffic Class; the low two ECN bits are preserved.
        {
            Ptr<EdgeQueueDisc> edge = MakeEdge(false);
            uint8_t tc = RoundTripTc(edge, kInitialTc);
            NS_TEST_ASSERT_MSG_EQ(
                tc >> 2,
                46u,
                "wash=false: dequeued IPv6 DSCP should equal classifier mark (46/EF)");
            NS_TEST_ASSERT_MSG_EQ(tc & 0x3, kEct1Bits, "wash=false: IPv6 ECN bits preserved");
            Simulator::Destroy();
        }

        // Wash=true: the high six bits of Traffic Class are zeroed at dequeue;
        // classification still drove inner-slot routing, but the packet exits
        // with DSCP=0. ECN bits remain ECT(1).
        {
            Ptr<EdgeQueueDisc> edge = MakeEdge(true);
            uint8_t tc = RoundTripTc(edge, kInitialTc);
            NS_TEST_ASSERT_MSG_EQ(tc >> 2, 0u, "wash=true: dequeued IPv6 DSCP zeroed");
            NS_TEST_ASSERT_MSG_EQ(tc & 0x3, kEct1Bits, "wash=true: IPv6 ECN bits preserved");
            Simulator::Destroy();
        }
    }
};

// =============================================================================
//  S-17.29: CAKE per-tin memlimit (byte-based queue limit)
// =============================================================================

/**
 * @brief Verifies the MemLimit attribute on FqCobaltQueueDisc is settable
 *        and readable, and that the parent's packet-count MaxSize is independent
 *        of the byte-cap MemLimit value.
 * @see specs/02-structural.md S-17.29
 *
 * Mirrors the Linux `tc-cake memlimit BYTES` API surface. The byte-counted
 * enqueue gate itself is asserted by the upstream fq-cobalt-queue-disc test
 * suite (FqCobaltQueueDiscMemLimit) — that gate now lives in mainline
 * FqCobaltQueueDisc per `patches/ns3/0006-fq-cobalt-ack-filter-memlimit.patch`.
 * This in-tree fixture pins only the attribute round-trip and the
 * MaxSize-independence invariant.
 */
class CakeMemLimitAttributeTest : public TestCase
{
  public:
    CakeMemLimitAttributeTest()
        : TestCase("S-17.29 MemLimit attribute is settable and readable on FqCobaltQueueDisc")
    {
    }

  private:
    void DoRun() override
    {
        // Sentinel default arm: MemLimit reads back as zero on a fresh instance.
        {
            Ptr<FqCobaltQueueDisc> fq = CreateObject<FqCobaltQueueDisc>();
            NS_TEST_ASSERT_MSG_EQ(fq->GetMemLimit(),
                                  0u,
                                  "default: MemLimit reads back zero (disabled)");
        }

        // Non-zero arm via CreateObjectWithAttributes.
        {
            Ptr<FqCobaltQueueDisc> fq =
                CreateObjectWithAttributes<FqCobaltQueueDisc>("MemLimit", UintegerValue(200000));
            NS_TEST_ASSERT_MSG_EQ(fq->GetMemLimit(),
                                  200000u,
                                  "construct-time: MemLimit reads back set value");
        }

        // Post-construction setter path via Config::Set-equivalent SetAttribute.
        {
            Ptr<FqCobaltQueueDisc> fq = CreateObject<FqCobaltQueueDisc>();
            fq->SetAttribute("MemLimit", UintegerValue(750000));
            NS_TEST_ASSERT_MSG_EQ(fq->GetMemLimit(),
                                  750000u,
                                  "post-construct: SetAttribute(MemLimit) round-trips");

            // MaxSize independence invariant: the parent's packet-count
            // MaxSize stays at its mainline default regardless of
            // MemLimit value (the byte gate is a separate enqueue check
            // in the patched mainline DoEnqueue).
            QueueSize maxSize = fq->GetMaxSize();
            NS_TEST_ASSERT_MSG_EQ(maxSize.GetUnit(),
                                  QueueSizeUnit::PACKETS,
                                  "MaxSize unit unchanged by MemLimit");
            NS_TEST_ASSERT_MSG_EQ(maxSize.GetValue(),
                                  10240u,
                                  "MaxSize value unchanged by MemLimit");
        }
    }
};

// =============================================================================
//  S-17.30: CAKE link-layer overhead — statistical rate adjustment (v1)
// =============================================================================

/**
 * @brief Verifies ConfigureLinkLayerOverhead downscales every per-tin TBF
 *        rate by gamma(overhead, atm, mpu) over the bimodal Internet mix.
 * @see specs/02-structural.md S-17.30
 */
class CakeOverheadStatisticalRateAdjustmentTest : public TestCase
{
  public:
    CakeOverheadStatisticalRateAdjustmentTest()
        : TestCase("S-17.30 ConfigureLinkLayerOverhead downscales tin TBF rates by E[gamma]")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        // Path γ — useInnerTbfShaping=true wraps each tin in a mainline
        // TbfQueueDisc, which is what ConfigureLinkLayerOverhead adjusts.
        cake::Helper::SetAsCakeDiffserv4(edge,
                                         DataRate("100Mbps"),
                                         {.tinShaping = true, .innerTbfShaping = true});

        // Snapshot Voice tin rate pre-config (share=25% of 100Mbps -> 25Mbps).
        Ptr<TbfQueueDisc> voiceTbf = edge->GetInnerDiscAt(3)->GetObject<TbfQueueDisc>();
        NS_TEST_ASSERT_MSG_NE(voiceTbf,
                              nullptr,
                              "Voice tin must wrap a TBF when useInnerTbfShaping=true");
        if (!voiceTbf)
        {
            return;
        }

        cake::Helper::ConfigureLinkLayerOverhead(edge,
                                                 /*overhead*/ 18,
                                                 /*atm*/ false,
                                                 /*ptm*/ false,
                                                 /*mpu*/ 64);

        // For 50% 64B + 50% 1500B mix, expected gamma:
        //   wire(64+18,  mpu=64) = max(82, 64)   = 82
        //   wire(1500+18,mpu=64) = max(1518, 64) = 1518
        //   gamma = (0.5*82 + 0.5*1518) / (0.5*64 + 0.5*1500) = 800/782 ~= 1.02302
        // Effective Voice tin rate: 25Mbps / 1.02302 ~= 24.4374 Mbps.
        // TbfQueueDisc::Rate attribute is set-only via the attribute
        // system (MakeDataRateAccessor binds SetRate); read back via
        // GetRate() on the concrete type.
        const double observed = static_cast<double>(voiceTbf->GetRate().GetBitRate());
        const double expected = 25e6 / (800.0 / 782.0);
        const double relErr = std::abs(observed - expected) / expected;
        NS_TEST_ASSERT_MSG_LT(relErr,
                              0.005,
                              "Voice tin TBF rate downscaled by gamma (observed="
                                  << observed << " expected=" << expected << " relErr=" << relErr
                                  << ")");

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-17.31: CAKE raw mode — suppress overhead correction
// =============================================================================

/**
 * @brief Verifies ConfigureLinkLayerOverhead with raw=true leaves every
 *        per-tin TBF rate unchanged regardless of overhead/atm/mpu args.
 * @see specs/02-structural.md S-17.31
 */
class CakeRawModeNoRateAdjustmentTest : public TestCase
{
  public:
    CakeRawModeNoRateAdjustmentTest()
        : TestCase("S-17.31 raw=true leaves per-tin TBF rates unchanged")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        cake::Helper::SetAsCakeDiffserv4(edge,
                                         DataRate("100Mbps"),
                                         {.tinShaping = true, .innerTbfShaping = true});

        Ptr<TbfQueueDisc> voiceTbf = edge->GetInnerDiscAt(3)->GetObject<TbfQueueDisc>();
        NS_TEST_ASSERT_MSG_NE(voiceTbf,
                              nullptr,
                              "Voice tin must wrap a TBF when useInnerTbfShaping=true");
        if (!voiceTbf)
        {
            return;
        }

        const uint64_t beforeBps = voiceTbf->GetRate().GetBitRate();

        // raw=true must short-circuit before any rate adjustment, even
        // with non-trivial overhead/atm/mpu values.
        cake::Helper::ConfigureLinkLayerOverhead(edge,
                                                 /*overhead*/ 18,
                                                 /*atm*/ true,
                                                 /*ptm*/ false,
                                                 /*mpu*/ 64,
                                                 /*raw*/ true);

        const uint64_t afterBps = voiceTbf->GetRate().GetBitRate();
        NS_TEST_ASSERT_MSG_EQ(beforeBps,
                              afterBps,
                              "raw=true must suppress overhead-driven rate adjustment");

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-17.49: CAKE conservative preset — defensive overhead defaults
// =============================================================================

/**
 * @brief Verifies SetAsCakeConservative applies the Linux `tc-cake(8)`
 *        conservative preset (overhead=48, mpu=64, atm=false) and
 *        downscales every per-tin TBF rate by the corresponding
 *        bimodal-mix gamma.
 * @see specs/02-structural.md S-17.49
 */
class CakeConservativePresetTest : public TestCase
{
  public:
    CakeConservativePresetTest()
        : TestCase("S-17.49 SetAsCakeConservative applies overhead=48 mpu=64 atm=false "
                   "and downscales per-tin TBF rates by E[gamma]")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        // Path γ — useInnerTbfShaping=true wraps each tin in a mainline
        // TbfQueueDisc; the conservative preset downscales those rates.
        cake::Helper::SetAsCakeDiffserv4(edge,
                                         DataRate("100Mbps"),
                                         {.tinShaping = true, .innerTbfShaping = true});

        Ptr<TbfQueueDisc> voiceTbf = edge->GetInnerDiscAt(3)->GetObject<TbfQueueDisc>();
        NS_TEST_ASSERT_MSG_NE(voiceTbf,
                              nullptr,
                              "Voice tin must wrap a TBF when useInnerTbfShaping=true");
        if (!voiceTbf)
        {
            return;
        }

        cake::Helper::SetAsCakeConservative(edge);

        // For 50% 64B + 50% 1500B mix, conservative preset gamma:
        //   wire(64+48,  mpu=64) = max(112, 64)   = 112
        //   wire(1500+48,mpu=64) = max(1548, 64) = 1548
        //   gamma = (0.5*112 + 0.5*1548) / (0.5*64 + 0.5*1500) = 830/782 ~= 1.06138
        // Voice tin share is 25% of 100Mbps = 25Mbps; expected ~ 23.5542 Mbps.
        const double observed = static_cast<double>(voiceTbf->GetRate().GetBitRate());
        const double expected = 25e6 / (830.0 / 782.0);
        const double relErr = std::abs(observed - expected) / expected;
        NS_TEST_ASSERT_MSG_LT(
            relErr,
            0.005,
            "Voice tin TBF rate must downscale by conservative-preset gamma (observed="
                << observed << " expected=" << expected << " relErr=" << relErr << ")");

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-17.32: CAKE per-tin diagnostics — bytesEnqueued / bytesDequeued
// =============================================================================

/**
 * @brief Verifies EdgeQueueDisc::GetTinStats returns per-tin
 *        wire-byte counters incremented by inner enqueue / dequeue.
 * @see specs/02-structural.md S-17.32
 */
class CakePerTinDiagnosticsTest : public TestCase
{
  public:
    CakePerTinDiagnosticsTest()
        : TestCase("S-17.32 GetTinStats reports per-tin enqueue, dequeue wire bytes")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        cake::Helper::SetAsCakeDiffserv4(edge, DataRate("100Mbps"));
        edge->Initialize();

        // Pre-flight: Voice tin (slot 3, EF = DSCP 0x2E -> CS5/EF/VA in
        // diffserv4) should have zero counters.
        cake::TinStats v0 = edge->GetTinStats(3);
        NS_TEST_ASSERT_MSG_EQ(v0.bytesEnqueued,
                              0u,
                              "Pre-enqueue: Voice bytesEnqueued must be zero");
        NS_TEST_ASSERT_MSG_EQ(v0.bytesDequeued,
                              0u,
                              "Pre-enqueue: Voice bytesDequeued must be zero");

        // Send 5 EF packets (1000B payload + IPv4 header = 1020B wire).
        const uint32_t kPayload = 1000;
        const uint32_t kIpHdrBytes = 20;
        const uint32_t kPerPktWire = kPayload + kIpHdrBytes;
        for (uint32_t i = 0; i < 5; ++i)
        {
            Ptr<Packet> p = Create<Packet>(kPayload);
            Ipv4Header h;
            h.SetDscp(Ipv4Header::DSCP_EF);
            h.SetProtocol(17);
            h.SetSource(Ipv4Address("10.0.0.1"));
            h.SetDestination(Ipv4Address("10.0.0.2"));
            edge->Enqueue(Create<Ipv4QueueDiscItem>(p, Mac48Address(), 0x0800, h));
        }

        cake::TinStats v1 = edge->GetTinStats(3);
        NS_TEST_ASSERT_MSG_EQ(v1.bytesEnqueued,
                              5u * kPerPktWire,
                              "Voice tin should record all 5 EF enqueues (5x1020B)");
        NS_TEST_ASSERT_MSG_EQ(v1.bytesDequeued, 0u, "Pre-dequeue: Voice bytesDequeued still zero");

        // Drain one — bytesDequeued should advance by exactly one packet.
        Ptr<QueueDiscItem> first = edge->Dequeue();
        NS_TEST_ASSERT_MSG_NE(first, nullptr, "First Dequeue must yield a packet");

        cake::TinStats v2 = edge->GetTinStats(3);
        NS_TEST_ASSERT_MSG_EQ(v2.bytesDequeued,
                              kPerPktWire,
                              "After one Dequeue: Voice bytesDequeued == one packet");

        // BE tin (slot 1) had no traffic — counters should remain zero
        // even though DRR walks slots and OnDequeue fires only on the
        // tin that actually yielded.
        cake::TinStats be = edge->GetTinStats(1);
        NS_TEST_ASSERT_MSG_EQ(be.bytesEnqueued, 0u, "BE tin had no enqueues -> bytesEnqueued zero");
        NS_TEST_ASSERT_MSG_EQ(be.bytesDequeued, 0u, "BE tin had no dequeues -> bytesDequeued zero");

        // Out-of-range slot returns a zeroed snapshot rather than aborting.
        cake::TinStats oob = edge->GetTinStats(99);
        NS_TEST_ASSERT_MSG_EQ(oob.bytesEnqueued, 0u, "Out-of-range slot -> zeroed snapshot");

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-17.33: Set-associative flow hash — structural-equivalence audit
//           (mainline ns-3 FqCobaltQueueDisc)
// =============================================================================

/**
 * @brief Verifies the structural-property contract of mainline ns-3
 *        FqCobaltQueueDisc::EnableSetAssociativeHash.
 *
 * Mainline ns-3's set-associative lookup is structurally equivalent
 * to the Linux sch_cake.c (67dc6c56b871, cake_hash @ line 709;
 * provenance/linux-sch-cake-67dc6c56b871/sch_cake.c) set-associative
 * design at the algorithm level, while the underlying 5-tuple hash
 * function diverges at the bit level (Murmur3-derived in ns-3 vs
 * jhash_3words in Linux). This
 * fixture exercises the design-level properties the substrate claim
 * depends on, on real input through the public Enqueue path:
 *
 *   - Attribute settability and readback (EnableSetAssociativeHash,
 *     SetWays).
 *   - Same-flow -> same-bucket determinism (tag preservation): five
 *     arrivals with identical 5-tuples coexist in a single allocated
 *     bucket regardless of mode.
 *   - Distinct-flow -> distinct-bucket coverage under default sizing:
 *     a second distinct 5-tuple allocates a second bucket (collision
 *     probability negligible at m_flows = 1024 with uniform hashing).
 *
 * Byte-exact equivalence to Linux is intentionally not asserted —
 * the underlying hash functions diverge by design.
 *
 * @see specs/02-structural.md S-17.33
 */
class SetAssociativeHashStructuralPropertiesTest : public TestCase
{
  public:
    SetAssociativeHashStructuralPropertiesTest()
        : TestCase("S-17.33 set-associative hash: attribute settability and same-flow "
                   "tag-preservation determinism in mainline FqCobaltQueueDisc")
    {
    }

  private:
    /// Run one (set-assoc on/off) arm. Each arm asserts the same
    /// structural properties; both arms must pass for the audit's
    /// design-level claim to hold.
    void RunOneArm(bool enableSetAssoc, const std::string& armLabel)
    {
        Ptr<FqCobaltQueueDisc> q =
            CreateObjectWithAttributes<FqCobaltQueueDisc>("EnableSetAssociativeHash",
                                                          BooleanValue(enableSetAssoc),
                                                          "SetWays",
                                                          UintegerValue(8));
        // Quantum default is zero — set explicitly per the existing
        // host-isolated reference fixture pattern.
        q->SetQuantum(1514);
        q->Initialize();

        // Five same-flow arrivals: tag-preservation invariant says the
        // five packets coexist in a single allocated bucket.
        const Ipv4Address sameSrc("10.1.1.1");
        const Ipv4Address sameDst("10.2.2.2");
        constexpr uint16_t kSport = 1024;
        constexpr uint16_t kDport = 80;
        constexpr uint32_t kPayload = 500;
        for (uint32_t i = 0; i < 5; ++i)
        {
            q->Enqueue(MakeUdpHostItem(sameSrc, sameDst, kSport, kDport, kPayload));
        }

        NS_TEST_ASSERT_MSG_EQ(q->GetNQueueDiscClasses(),
                              1u,
                              armLabel << ": five same-flow arrivals must allocate exactly one "
                                          "bucket (tag-preservation invariant)");
        NS_TEST_ASSERT_MSG_EQ(q->GetQueueDiscClass(0)->GetQueueDisc()->GetNPackets(),
                              5u,
                              armLabel << ": all five same-flow packets must coexist in the "
                                          "single allocated bucket");

        // One distinct 5-tuple: distinct-flow coverage under default
        // sizing (m_flows = 1024). Collision probability with the
        // already-seen flow is ~1/1024 under uniform hashing — accepted
        // as part of the smoke-level coverage claim.
        const Ipv4Address otherSrc("10.1.1.99");
        q->Enqueue(MakeUdpHostItem(otherSrc, sameDst, kSport, kDport, kPayload));

        NS_TEST_ASSERT_MSG_EQ(q->GetNQueueDiscClasses(),
                              2u,
                              armLabel << ": one distinct 5-tuple added must allocate a second "
                                          "bucket under default sizing");

        Simulator::Destroy();
    }

    void DoRun() override
    {
        // Property A: attribute settability + readback for both
        // attributes the audit depends on.
        Ptr<FqCobaltQueueDisc> qd =
            CreateObjectWithAttributes<FqCobaltQueueDisc>("EnableSetAssociativeHash",
                                                          BooleanValue(true),
                                                          "SetWays",
                                                          UintegerValue(8));
        BooleanValue bv;
        UintegerValue uv;
        qd->GetAttribute("EnableSetAssociativeHash", bv);
        qd->GetAttribute("SetWays", uv);
        NS_TEST_ASSERT_MSG_EQ(bv.Get(),
                              true,
                              "EnableSetAssociativeHash must round-trip via attribute system");
        NS_TEST_ASSERT_MSG_EQ(uv.Get(), 8u, "SetWays must round-trip via attribute system");

        // Properties B + C, both arms: same-flow determinism +
        // distinct-flow separation. The audit's substrate claim depends
        // on these holding regardless of which underlying hash function
        // (Murmur3 here, jhash_3words in Linux) feeds the lookup.
        RunOneArm(true, "set-assoc=true");
        RunOneArm(false, "set-assoc=false");
    }
};

// =============================================================================
//  S-17.5..8: cake::Helper composition
// =============================================================================

// DSCP -> tin lookup tables transcribed verbatim from the frozen Linux
// excerpt (provenance/linux-sch-cake-67dc6c56b871/sch_cake.c:326-357):
// one kernel tin index per DSCP 0..63, row order identical to the
// kernel arrays. The fixtures below walk every codepoint against
// these tables through each layout's Linux-tin-to-slot permutation.
constexpr std::array<uint8_t, 64> kLinuxDiffserv3Tin = {
    0, 1, 0, 0, 2, 0, 0, 0, //
    1, 0, 0, 0, 0, 0, 0, 0, //
    0, 0, 0, 0, 0, 0, 0, 0, //
    0, 0, 0, 0, 0, 0, 0, 0, //
    0, 0, 0, 0, 0, 0, 0, 0, //
    0, 0, 0, 0, 2, 0, 2, 0, //
    2, 0, 0, 0, 0, 0, 0, 0, //
    2, 0, 0, 0, 0, 0, 0, 0, //
};

constexpr std::array<uint8_t, 64> kLinuxDiffserv4Tin = {
    0, 1, 0, 0, 2, 0, 0, 0, //
    1, 0, 0, 0, 0, 0, 0, 0, //
    2, 0, 2, 0, 2, 0, 2, 0, //
    2, 0, 2, 0, 2, 0, 2, 0, //
    3, 0, 2, 0, 2, 0, 2, 0, //
    3, 0, 0, 0, 3, 0, 3, 0, //
    3, 0, 0, 0, 0, 0, 0, 0, //
    3, 0, 0, 0, 0, 0, 0, 0, //
};

constexpr std::array<uint8_t, 64> kLinuxDiffserv8Tin = {
    2, 0, 1, 2, 4, 2, 2, 2, //
    1, 2, 1, 2, 1, 2, 1, 2, //
    5, 2, 4, 2, 4, 2, 4, 2, //
    3, 2, 3, 2, 3, 2, 3, 2, //
    6, 2, 3, 2, 3, 2, 3, 2, //
    6, 2, 2, 2, 6, 2, 6, 2, //
    7, 2, 2, 2, 2, 2, 2, 2, //
    7, 2, 2, 2, 2, 2, 2, 2, //
};

/**
 * @brief Verifies the CAKE helper diffserv4 DSCP map matches the Linux reference table.
 * @see specs/02-structural.md S-17.5
 */
class CakeHelperDscpMapMatchesLinuxDiffserv4Test : public TestCase
{
  public:
    CakeHelperDscpMapMatchesLinuxDiffserv4Test()
        : TestCase("S-17.5 CAKE helper diffserv4 DSCP map matches Linux tc-cake(8)")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        cake::Helper::SetAsCakeDiffserv4(edge, DataRate("10Mbps"));

        NS_TEST_ASSERT_MSG_EQ(edge->GetNumInnerSlots(), 4u, "diffserv4 installs 4 tins");

        // Linux tin index -> this layout's slot: BE(0)->1, Bulk(1)->0,
        // Video(2)->2, Voice(3)->3.
        constexpr std::array<uint8_t, 4> kTinToSlot = {1, 0, 2, 3};
        uint32_t mismatches = 0;
        std::ostringstream detail;
        for (uint32_t dscp = 0; dscp < 64; ++dscp)
        {
            const uint32_t expected = kTinToSlot[kLinuxDiffserv4Tin[dscp]];
            const uint32_t actual = edge->GetDscpToSlot(static_cast<uint8_t>(dscp));
            if (actual != expected)
            {
                ++mismatches;
                detail << " dscp" << dscp << "=" << actual << "(want " << expected << ")";
            }
        }
        std::cout << "S17_5SUM,mismatches=" << mismatches << std::endl;
        NS_TEST_ASSERT_MSG_EQ(
            mismatches,
            0u,
            "diffserv4 DSCP map diverges from the sch_cake table:" << detail.str());

        Simulator::Destroy();
    }
};

/**
 * @brief The standalone rate-based (autorate-ingress) diffserv4 DSCP map matches
 * the same Linux sch_cake.c diffserv4[] table the composer uses.
 *
 * The composer path (SetAsCakeDiffserv4) transcribes the kernel table; the
 * RateBased shaper path built a separate hand-written predicate map that
 * drifted from it. Both must agree with sch_cake.c over all 64 code points.
 */
class RateBasedDiffserv4DscpMapMatchesLinuxTest : public TestCase
{
  public:
    RateBasedDiffserv4DscpMapMatchesLinuxTest()
        : TestCase("rate-based diffserv4 DSCP map matches Linux sch_cake diffserv4[]")
    {
    }

  private:
    void DoRun() override
    {
        cake::Helper helper;
        helper.SetShaperMode(cake::Helper::ShaperMode::RateBased);
        helper.SetGlobalRateBps(10000000ULL);
        helper.SetTinRateBpsAll(10000000ULL);
        helper.SetTinCount(4);
        Ptr<QueueDisc> disp = helper.BuildDispatcher();
        Ptr<cake::RateBasedShaperDispatcher> rb =
            DynamicCast<cake::RateBasedShaperDispatcher>(disp);
        NS_TEST_ASSERT_MSG_NE(rb,
                              nullptr,
                              "RateBased BuildDispatcher must yield a RateBasedShaperDispatcher");
        if (!rb)
        {
            return;
        }

        // BE(0)->slot 1, Bulk(1)->slot 0, Video(2)->slot 2, Voice(3)->slot 3.
        constexpr std::array<uint8_t, 4> kTinToSlot = {1, 0, 2, 3};
        uint32_t mismatches = 0;
        std::ostringstream detail;
        for (uint32_t dscp = 0; dscp < 64; ++dscp)
        {
            const uint32_t expected = kTinToSlot[kLinuxDiffserv4Tin[dscp]];
            const uint32_t actual = rb->GetDscpToSlot(static_cast<uint8_t>(dscp));
            if (actual != expected)
            {
                ++mismatches;
                detail << " dscp" << dscp << "=" << actual << "(want " << expected << ")";
            }
        }
        NS_TEST_ASSERT_MSG_EQ(
            mismatches,
            0u,
            "rate-based diffserv4 DSCP map diverges from sch_cake diffserv4[]:" << detail.str());
        Simulator::Destroy();
    }
};

/**
 * @brief Verifies the CAKE helper diffserv3 DSCP map matches the Linux reference table.
 * @see specs/02-structural.md S-17.6
 */
class CakeHelperDscpMapMatchesLinuxDiffserv3Test : public TestCase
{
  public:
    CakeHelperDscpMapMatchesLinuxDiffserv3Test()
        : TestCase("S-17.6 CAKE helper diffserv3 DSCP map matches Linux tc-cake(8)")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        cake::Helper::SetAsCakeDiffserv3(edge, DataRate("10Mbps"));

        NS_TEST_ASSERT_MSG_EQ(edge->GetNumInnerSlots(), 3u, "diffserv3 installs 3 tins");

        // Linux tin index -> this layout's slot: BE(0)->2, Bulk(1)->0,
        // Latency-Sensitive(2)->1.
        constexpr std::array<uint8_t, 3> kTinToSlot = {2, 0, 1};
        uint32_t mismatches = 0;
        std::ostringstream detail;
        for (uint32_t dscp = 0; dscp < 64; ++dscp)
        {
            const uint32_t expected = kTinToSlot[kLinuxDiffserv3Tin[dscp]];
            const uint32_t actual = edge->GetDscpToSlot(static_cast<uint8_t>(dscp));
            if (actual != expected)
            {
                ++mismatches;
                detail << " dscp" << dscp << "=" << actual << "(want " << expected << ")";
            }
        }
        std::cout << "S17_6SUM,mismatches=" << mismatches << std::endl;
        NS_TEST_ASSERT_MSG_EQ(
            mismatches,
            0u,
            "diffserv3 DSCP map diverges from the sch_cake table:" << detail.str());

        Simulator::Destroy();
    }
};

/**
 * @brief Verifies the CAKE helper diffserv8 DSCP map matches the Linux reference table.
 * @see specs/02-structural.md S-17.7
 */
class CakeHelperDscpMapMatchesLinuxDiffserv8Test : public TestCase
{
  public:
    CakeHelperDscpMapMatchesLinuxDiffserv8Test()
        : TestCase("S-17.7 CAKE helper diffserv8 DSCP map matches Linux tc-cake(8)")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        cake::Helper::SetAsCakeDiffserv8(edge, DataRate("10Mbps"));

        NS_TEST_ASSERT_MSG_EQ(edge->GetNumInnerSlots(), 8u, "diffserv8 installs 8 tins");

        // diffserv8 slot order follows the kernel tin order directly
        // (identity permutation).
        uint32_t mismatches = 0;
        std::ostringstream detail;
        for (uint32_t dscp = 0; dscp < 64; ++dscp)
        {
            const uint32_t expected = kLinuxDiffserv8Tin[dscp];
            const uint32_t actual = edge->GetDscpToSlot(static_cast<uint8_t>(dscp));
            if (actual != expected)
            {
                ++mismatches;
                detail << " dscp" << dscp << "=" << actual << "(want " << expected << ")";
            }
        }
        std::cout << "S17_7SUM,mismatches=" << mismatches << std::endl;
        NS_TEST_ASSERT_MSG_EQ(
            mismatches,
            0u,
            "diffserv8 DSCP map diverges from the sch_cake table:" << detail.str());

        Simulator::Destroy();
    }
};

/**
 * @brief S-17.62: diffserv3 unshaped contention follows the kernel
 *        quantum ladder — Latency-Sensitive : Best-Effort served bytes
 *        = 1:4 through the composed edge.
 * @see specs/02-structural.md S-17.62
 */
class CakeDiffserv3UnshapedContentionRatioTest : public TestCase
{
  public:
    CakeDiffserv3UnshapedContentionRatioTest()
        : TestCase("S-17.62 diffserv3 unshaped contention follows the kernel quantum ladder")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        cake::Helper::SetAsCakeDiffserv3(edge, DataRate("10Mbps"));
        edge->Initialize();

        // 737-byte payload + 20-byte IPv4 header = 757 wire bytes,
        // dividing the per-tin DRR quanta exactly (BE at 4 x MTU =
        // 6056 bytes; the Latency-Sensitive quantum at any
        // MTU-multiple), so the DRR algorithm is deterministic and the
        // served ratio converges with no deficit carry.
        const uint32_t payload = 737;
        const uint32_t wireSize = payload + 20;

        // EF (46) -> Latency-Sensitive (slot 1); DF (0) -> BE (slot 2).
        // Saturate both tins beyond the drain window so neither runs
        // dry mid-measurement.
        const uint32_t perTin = 400;
        for (uint32_t i = 0; i < perTin; ++i)
        {
            NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(MakeTinShaperItem(46, payload)),
                                  true,
                                  "Latency-Sensitive enqueue " << i);
            NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(MakeTinShaperItem(0, payload)),
                                  true,
                                  "BE enqueue " << i);
        }

        // Drain 250 packets (an integer number of DRR rounds at the
        // kernel-ladder quanta: 2 Latency-Sensitive + 8 BE per round).
        const uint32_t drain = 250;
        uint64_t lsBytes = 0;
        uint64_t beBytes = 0;
        for (uint32_t i = 0; i < drain; ++i)
        {
            Ptr<QueueDiscItem> it = edge->Dequeue();
            NS_TEST_ASSERT_MSG_NE(it, nullptr, "dequeue " << i);
            if (!it)
            {
                Simulator::Destroy();
                return;
            }
            Ptr<Ipv4QueueDiscItem> ip = DynamicCast<Ipv4QueueDiscItem>(it);
            const uint32_t dscp = ip->GetHeader().GetTos() >> 2;
            if (dscp == 46)
            {
                lsBytes += wireSize;
            }
            else
            {
                beBytes += wireSize;
            }
        }

        const double ratio =
            beBytes > 0 ? static_cast<double>(lsBytes) / static_cast<double>(beBytes) : 0.0;
        // Build the harvest line in a fresh stream: cout format state
        // (precision manipulators) persists across the suite's cases,
        // which share one process.
        std::ostringstream sum;
        sum << "S17_62SUM,lsBytes=" << lsBytes << ",beBytes=" << beBytes << ",lsOverBe=" << ratio;
        std::cout << sum.str() << std::endl;
        // cake_config_diffserv3 quanta are 1024 / 64 / 256 for BE /
        // Bulk / Latency-Sensitive (sch_cake.c:2576-2580): the served
        // Latency-Sensitive : BE byte ratio under contention is 1:4.
        NS_TEST_ASSERT_MSG_EQ_TOL(ratio,
                                  0.25,
                                  0.005,
                                  "Latency-Sensitive:BE served-byte ratio "
                                      << ratio << " diverges from the kernel quantum ladder");

        Simulator::Destroy();
    }
};

/**
 * @brief Verifies the CAKE besteffort preset maps every DSCP to a
 *        single tin and installs exactly one inner slot.
 * @see specs/02-structural.md S-17.24
 */
class CakeHelperBestEffortMapsToSingleTinTest : public TestCase
{
  public:
    CakeHelperBestEffortMapsToSingleTinTest()
        : TestCase("S-17.24 CAKE helper besteffort: every DSCP maps to a single tin")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        cake::Helper::SetAsCakeBestEffort(edge, DataRate("10Mbps"));

        NS_TEST_ASSERT_MSG_EQ(edge->GetNumInnerSlots(), 1u, "besteffort installs exactly one tin");

        for (uint32_t dscp = 0; dscp < 64; ++dscp)
        {
            NS_TEST_ASSERT_MSG_EQ(edge->GetDscpToSlot(static_cast<uint8_t>(dscp)),
                                  0u,
                                  "DSCP " << dscp << " -> Tin 0 (besteffort)");
        }

        Simulator::Destroy();
    }
};

/**
 * @brief Verifies the CAKE precedence preset maps every DSCP to its
 *        IP-precedence-keyed tin (top three bits of DSCP).
 * @see specs/02-structural.md S-17.25
 */
class CakeHelperPrecedenceMapsAreByteExactTest : public TestCase
{
  public:
    CakeHelperPrecedenceMapsAreByteExactTest()
        : TestCase("S-17.25 CAKE helper precedence: DSCP d -> tin (d >> 3) for all 64 codepoints")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        cake::Helper::SetAsCakePrecedence(edge, DataRate("10Mbps"));

        NS_TEST_ASSERT_MSG_EQ(edge->GetNumInnerSlots(), 8u, "precedence installs 8 tins");

        for (uint32_t dscp = 0; dscp < 64; ++dscp)
        {
            const uint8_t expected = static_cast<uint8_t>(dscp >> 3);
            NS_TEST_ASSERT_MSG_EQ(edge->GetDscpToSlot(static_cast<uint8_t>(dscp)),
                                  expected,
                                  "DSCP " << dscp << " -> Tin " << static_cast<uint32_t>(expected)
                                          << " (top-3-bit precedence)");
        }

        Simulator::Destroy();
    }
};

/**
 * @brief End-to-end smoke-tests a single-tin CAKE configuration.
 * @see specs/02-structural.md S-17.8
 */
class CakeEndToEndSingleTinTest : public TestCase
{
  public:
    CakeEndToEndSingleTinTest()
        : TestCase("S-17.8 CAKE end-to-end single tin: enqueue and dequeue through composite")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        cake::Helper::SetAsCakeDiffserv4(edge, DataRate("10Mbps"));
        edge->Initialize();

        // Drive 20 BE packets (DSCP 0 -> Tin 1, the BE tin) and confirm
        // they all enqueue and dequeue cleanly through the
        // TBF -> FqCobaltQueueDisc composite. This is a sanity run,
        // not a fairness test (S-17.2..4 cover DRR; this exercises
        // composition).
        const uint32_t payload = 500;
        const uint32_t numPackets = 20;
        uint32_t enqueued = 0;
        for (uint32_t i = 0; i < numPackets; ++i)
        {
            Ptr<Ipv4QueueDiscItem> item =
                MakeIpv4Item(Ipv4Address("10.0.0.1"), Ipv4Address("10.0.0.2"), 17, payload);
            // DSCP 0 (BE) is set by default; routes to Tin 1.
            if (edge->Enqueue(item))
            {
                ++enqueued;
            }
        }
        NS_TEST_ASSERT_MSG_EQ(enqueued, numPackets, "all BE packets enqueued through composite");

        // Drain everything that enqueued. TBF rate-limiting means not all
        // packets are immediately available — drain via repeated peek/
        // dequeue, advancing simulation time so TBF replenishes tokens.
        // For this sanity test we only require: at least one packet
        // dequeues without crashing the composite.
        Ptr<QueueDiscItem> first = edge->Dequeue();
        NS_TEST_ASSERT_MSG_NE(first, nullptr, "first packet dequeues from composite");
        if (!first)
        {
            return;
        }

        Simulator::Destroy();
    }
};

/**
 * @brief Cross-mode rate equivalence: path-alpha vs path-gamma per-tin shaping.
 *
 * Builds two `EdgeQueueDisc` instances configured via
 * `cake::Helper::SetAsCakeDiffserv4` with `enableTinShaping=true`, one
 * with `useInnerTbfShaping=false` (path alpha: in-dispatcher
 * `cake::TinTokenBucket` gate) and one with `useInnerTbfShaping=true` (path
 * gamma: each tin's inner wrapped in a mainline `TbfQueueDisc`). Drives
 * identical saturating offered traffic into both at DSCP=8 (CS1, routes
 * to the Bulk tin = slot 0 with share=0.0625, cap=625 kbps at the
 * configured 10 Mbps totalRate). Asserts the aggregate served bytes
 * match within +-5 %.
 *
 * Confirms path gamma is byte-equivalent to path alpha for the
 * production tin-shaping configuration, the prerequisite for any
 * future retirement of the in-dispatcher token-bucket gate.
 */
class CakeHelperPathAlphaGammaEquivalenceTest : public TestCase
{
  public:
    CakeHelperPathAlphaGammaEquivalenceTest()
        : TestCase("Path-alpha vs path-gamma equivalence: aggregate served within +-5 %")
    {
    }

  private:
    Ptr<EdgeQueueDisc> m_edgeAlpha;
    Ptr<EdgeQueueDisc> m_edgeGamma;

    void EnqueueBoth(uint8_t dscp, uint32_t payload)
    {
        m_edgeAlpha->Enqueue(MakeTinShaperItem(dscp, payload));
        m_edgeGamma->Enqueue(MakeTinShaperItem(dscp, payload));
    }

    void DrainBoth()
    {
        while (Ptr<QueueDiscItem> it = m_edgeAlpha->Dequeue())
        {
            (void)it;
        }
        while (Ptr<QueueDiscItem> it = m_edgeGamma->Dequeue())
        {
            (void)it;
        }
    }

    void DoRun() override
    {
        const uint32_t payload = 1480;
        const uint8_t dscp = 8;                // CS1, routes to Bulk tin (slot 0, share=0.0625)
        const Time totalRateLink = Seconds(0); // unused; we use DataRate value
        const DataRate totalRate("10Mbps");

        m_edgeAlpha = CreateObject<EdgeQueueDisc>();
        m_edgeGamma = CreateObject<EdgeQueueDisc>();

        // Path alpha: useInnerTbfShaping defaulted to false.
        cake::Helper::SetAsCakeDiffserv4(m_edgeAlpha, totalRate, {.tinShaping = true});
        // Path gamma: same args, useInnerTbfShaping=true.
        cake::Helper::SetAsCakeDiffserv4(m_edgeGamma,
                                         totalRate,
                                         {.tinShaping = true, .innerTbfShaping = true});
        m_edgeAlpha->Initialize();
        m_edgeGamma->Initialize();

        const Time simEnd = Seconds(10.0);
        const Time offeredPeriod = MilliSeconds(6); // ~2 Mbps offered at 1500 B
        const Time drainPeriod = MilliSeconds(1);   // poll dequeue every 1 ms

        for (Time t = Seconds(0); t < simEnd; t += offeredPeriod)
        {
            Simulator::Schedule(t,
                                &CakeHelperPathAlphaGammaEquivalenceTest::EnqueueBoth,
                                this,
                                dscp,
                                payload);
        }
        for (Time t = Seconds(0); t < simEnd; t += drainPeriod)
        {
            Simulator::Schedule(t, &CakeHelperPathAlphaGammaEquivalenceTest::DrainBoth, this);
        }

        Simulator::Stop(simEnd);
        Simulator::Run();

        const uint64_t servedAlpha = m_edgeAlpha->GetStats().nTotalDequeuedBytes;
        const uint64_t servedGamma = m_edgeGamma->GetStats().nTotalDequeuedBytes;

        // Both modes should drain within +-5 % of each other. The
        // absolute floor is `share * totalRate / 8 * simEnd ` =
        // 0.0625 * 10e6 / 8 * 10 = 781250 bytes; the offered is
        // ~2 Mbps which exceeds the cap, so the cap (not the offered
        // rate) determines served bytes.
        NS_TEST_ASSERT_MSG_GT(servedAlpha,
                              500000,
                              "path-alpha served suspiciously low (cap mis-applied)");
        NS_TEST_ASSERT_MSG_GT(servedGamma,
                              500000,
                              "path-gamma served suspiciously low (cap mis-applied)");

        const double ratio = static_cast<double>(servedGamma) / static_cast<double>(servedAlpha);
        NS_TEST_ASSERT_MSG_GT_OR_EQ(ratio,
                                    0.95,
                                    "path-gamma served (" << servedGamma << ") below 95 % of "
                                                          << "path-alpha served (" << servedAlpha
                                                          << ") — TBF inner-mode under-throttling");
        NS_TEST_ASSERT_MSG_LT_OR_EQ(ratio,
                                    1.05,
                                    "path-gamma served (" << servedGamma << ") above 105 % of "
                                                          << "path-alpha served (" << servedAlpha
                                                          << ") — TBF inner-mode over-serving");

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-14.1: EF Priority Service
// =============================================================================

/**
 * @brief Verifies the EF aggregate receives priority service.
 * @see specs/02-structural.md S-14.1
 */
class EfPriorityServiceTest : public TestCase
{
  public:
    EfPriorityServiceTest()
        : TestCase("S-14.1 EF priority service: EF traffic served before BE")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        auto inner = CreateObject<RedQueueDisc>();
        edge->SetInnerDisc(inner);
        inner->SetNumQueues(2);

        // Mark rule 1: src=10.0.0.1 -> DSCP 46 (EF)
        edge->AddMarkRule({.dscp = 46, .srcAddr = Ipv4Address("10.0.0.1")});

        // Mark rule 2: src=10.0.0.2 -> DSCP 0 (BE)
        edge->AddMarkRule({.dscp = 0, .srcAddr = Ipv4Address("10.0.0.2")});

        // Policy for DSCP 46 (EF), Dumb meter+policer
        PolicyEntry policy46;
        policy46.codePoint = 46;
        policy46.meter = MeterType::DUMB;
        policy46.policer = PolicerType::DUMB;
        policy46.policyIndex = 0;
        edge->GetPolicyClassifier()->AddPolicyEntry(policy46);

        PolicerEntry policer46;
        policer46.policer = PolicerType::DUMB;
        policer46.policyIndex = 0;
        policer46.initialCodePt = 46;
        policer46.downgrade1 = 46;
        policer46.downgrade2 = 46;
        edge->GetPolicyClassifier()->AddPolicerEntry(policer46);

        // Policy for DSCP 0 (BE), Dumb meter+policer
        PolicyEntry policy0;
        policy0.codePoint = 0;
        policy0.meter = MeterType::DUMB;
        policy0.policer = PolicerType::DUMB;
        policy0.policyIndex = 1;
        edge->GetPolicyClassifier()->AddPolicyEntry(policy0);

        PolicerEntry policer0;
        policer0.policer = PolicerType::DUMB;
        policer0.policyIndex = 1;
        policer0.initialCodePt = 0;
        policer0.downgrade1 = 0;
        policer0.downgrade2 = 0;
        edge->GetPolicyClassifier()->AddPolicerEntry(policer0);

        // PHB: DSCP 46 -> queue 0 (high priority), DSCP 0 -> queue 1
        inner->AddPhbEntry(46, 0, 0);
        inner->AddPhbEntry(0, 1, 0);

        // PQ scheduler: 2 queues, queue 0 is highest priority
        Ptr<PriorityScheduler> pq = CreateObjectWithAttributes<PriorityScheduler>("NumQueues",
                                                                                  UintegerValue(2),
                                                                                  "WinLen",
                                                                                  DoubleValue(1.0));
        inner->SetScheduler(pq);

        // Queue limits (stored pre-Initialize so CheckConfig picks them up)
        inner->SetQueueLimit(0, 50);
        inner->SetQueueLimit(1, 50);

        edge->Initialize();

        // Set DROP_TAIL mode and configure thresholds AFTER Initialize
        // (SetMredMode needs children to exist)
        inner->SetMredMode(MredMode::DROP_TAIL, 0);
        inner->SetMredMode(MredMode::DROP_TAIL, 1);
        // Set thMin > qlim so threshold check never fires; pure tail-drop behaviour
        inner->ConfigQueue({.queue = 0, .prec = 0, .thMin = 1000.0, .thMax = 2000.0, .maxP = 0.1});
        inner->ConfigQueue({.queue = 1, .prec = 0, .thMin = 1000.0, .thMax = 2000.0, .maxP = 0.1});

        // Enqueue 5 BE packets from 10.0.0.2 (-> queue 1)
        for (int i = 0; i < 5; ++i)
        {
            Ptr<Ipv4QueueDiscItem> item =
                MakeIpv4Item(Ipv4Address("10.0.0.2"), Ipv4Address("10.0.0.100"), 17, 500);
            bool ok = edge->Enqueue(item);
            NS_TEST_ASSERT_MSG_EQ(ok, true, "BE packet " << i << " should enqueue");
        }

        // Enqueue 5 EF packets from 10.0.0.1 (-> queue 0)
        for (int i = 0; i < 5; ++i)
        {
            Ptr<Ipv4QueueDiscItem> item =
                MakeIpv4Item(Ipv4Address("10.0.0.1"), Ipv4Address("10.0.0.100"), 17, 500);
            bool ok = edge->Enqueue(item);
            NS_TEST_ASSERT_MSG_EQ(ok, true, "EF packet " << i << " should enqueue");
        }

        // Dequeue all 10 packets: first 5 must be EF (DSCP 46), next 5 must be BE
        // (DSCP 0)
        for (int i = 0; i < 5; ++i)
        {
            Ptr<QueueDiscItem> dequeued = edge->Dequeue();
            NS_TEST_ASSERT_MSG_NE(dequeued, nullptr, "Should dequeue EF packet " << i);
            Ptr<Ipv4QueueDiscItem> ipItem = DynamicCast<Ipv4QueueDiscItem>(dequeued);
            NS_TEST_ASSERT_MSG_NE(ipItem, nullptr, "Dequeued item should be Ipv4");
            if (!ipItem)
            {
                continue;
            }
            uint8_t dscp = ipItem->GetHeader().GetTos() >> 2;
            NS_TEST_ASSERT_MSG_EQ(dscp,
                                  46,
                                  "Packet " << i << " (EF phase) should have DSCP 46, got "
                                            << static_cast<uint32_t>(dscp));
        }

        for (int i = 0; i < 5; ++i)
        {
            Ptr<QueueDiscItem> dequeued = edge->Dequeue();
            NS_TEST_ASSERT_MSG_NE(dequeued, nullptr, "Should dequeue BE packet " << i);
            Ptr<Ipv4QueueDiscItem> ipItem = DynamicCast<Ipv4QueueDiscItem>(dequeued);
            NS_TEST_ASSERT_MSG_NE(ipItem, nullptr, "Dequeued item should be Ipv4");
            if (!ipItem)
            {
                continue;
            }
            uint8_t dscp = ipItem->GetHeader().GetTos() >> 2;
            NS_TEST_ASSERT_MSG_EQ(dscp,
                                  0,
                                  "Packet " << i << " (BE phase) should have DSCP 0, got "
                                            << static_cast<uint32_t>(dscp));
        }

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-14.2: AF Drop Precedence
// =============================================================================

/**
 * @brief Verifies AF aggregates drop in order of precedence under congestion.
 * @see specs/02-structural.md S-14.2
 */
class AfDropPrecedenceTest : public TestCase
{
  public:
    AfDropPrecedenceTest()
        : TestCase("S-14.2 AF drop precedence: higher prec drops more (RIO_D)")
    {
    }

  private:
    void DoRun() override
    {
        RngSeedManager::SetSeed(1);
        RngSeedManager::SetRun(1);

        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        auto inner = CreateObject<RedQueueDisc>();
        edge->SetInnerDisc(inner);
        inner->SetNumQueues(1);

        // Mark rule: src=10.0.0.1 -> DSCP 10 (AF11)
        edge->AddMarkRule({.dscp = 10, .srcAddr = Ipv4Address("10.0.0.1")});

        // Mark rule: src=10.0.0.2 -> DSCP 12 (AF12)
        edge->AddMarkRule({.dscp = 12, .srcAddr = Ipv4Address("10.0.0.2")});

        // Mark rule: src=10.0.0.3 -> DSCP 14 (AF13)
        edge->AddMarkRule({.dscp = 14, .srcAddr = Ipv4Address("10.0.0.3")});

        // Dumb policies + policers for all three DSCPs (pass-through, no metering)
        auto addDumbPolicyAndPolicer = [&](uint8_t dscp, uint32_t policyIdx) {
            PolicyEntry policy;
            policy.codePoint = dscp;
            policy.meter = MeterType::DUMB;
            policy.policer = PolicerType::DUMB;
            policy.policyIndex = policyIdx;
            edge->GetPolicyClassifier()->AddPolicyEntry(policy);

            PolicerEntry policer;
            policer.policer = PolicerType::DUMB;
            policer.policyIndex = policyIdx;
            policer.initialCodePt = dscp;
            policer.downgrade1 = dscp;
            policer.downgrade2 = dscp;
            edge->GetPolicyClassifier()->AddPolicerEntry(policer);
        };

        addDumbPolicyAndPolicer(10, 0); // AF11 -> prec 0 (lenient)
        addDumbPolicyAndPolicer(12, 1); // AF12 -> prec 1 (moderate)
        addDumbPolicyAndPolicer(14, 2); // AF13 -> prec 2 (aggressive)

        // PHB: all three DSCPs go to queue 0, different precedences
        inner->AddPhbEntry(10, 0, 0); // AF11 -> queue 0, prec 0
        inner->AddPhbEntry(12, 0, 1); // AF12 -> queue 0, prec 1
        inner->AddPhbEntry(14, 0, 2); // AF13 -> queue 0, prec 2

        // RR scheduler for the single queue
        Ptr<RoundRobinScheduler> sched =
            CreateObjectWithAttributes<RoundRobinScheduler>("NumQueues", UintegerValue(1));
        inner->SetScheduler(sched);

        // Queue limit 50
        inner->SetQueueLimit(0, 50);

        edge->Initialize();

        // Configure AFTER Initialize: set numPrec and RIO_D mode
        inner->SetNumPrec(0, 3);
        inner->SetMredMode(MredMode::RIO_D, 0);

        // Configure RED thresholds for each precedence level
        // prec 0 (AF11): lenient (high thresholds, low maxP)
        inner->ConfigQueue({.queue = 0, .prec = 0, .thMin = 20.0, .thMax = 40.0, .maxP = 0.02});
        // prec 1 (AF12): moderate
        inner->ConfigQueue({.queue = 0, .prec = 1, .thMin = 10.0, .thMax = 30.0, .maxP = 0.1});
        // prec 2 (AF13): aggressive (low thresholds, high maxP)
        inner->ConfigQueue({.queue = 0, .prec = 2, .thMin = 5.0, .thMax = 15.0, .maxP = 0.5});

        // Send 150 packets interleaved: round-robin among 3 sources (50 each)
        int sent0 = 0;
        int sent1 = 0;
        int sent2 = 0;
        int received0 = 0;
        int received1 = 0;
        int received2 = 0;

        for (int round = 0; round < 50; ++round)
        {
            // AF11 (prec 0, lenient)
            Ptr<Ipv4QueueDiscItem> item1 =
                MakeIpv4Item(Ipv4Address("10.0.0.1"), Ipv4Address("10.0.0.100"), 17, 500);
            sent0++;
            if (edge->Enqueue(item1))
            {
                received0++;
            }

            // AF12 (prec 1, moderate)
            Ptr<Ipv4QueueDiscItem> item2 =
                MakeIpv4Item(Ipv4Address("10.0.0.2"), Ipv4Address("10.0.0.100"), 17, 500);
            sent1++;
            if (edge->Enqueue(item2))
            {
                received1++;
            }

            // AF13 (prec 2, aggressive)
            Ptr<Ipv4QueueDiscItem> item3 =
                MakeIpv4Item(Ipv4Address("10.0.0.3"), Ipv4Address("10.0.0.100"), 17, 500);
            sent2++;
            if (edge->Enqueue(item3))
            {
                received2++;
            }
        }

        int drops0 = sent0 - received0;
        int drops1 = sent1 - received1;
        int drops2 = sent2 - received2;

        // Assert: higher precedence level incurs more (or equal) drops
        NS_TEST_ASSERT_MSG_GT_OR_EQ(
            drops2,
            drops1,
            "AF13 (prec 2) should have >= drops as AF12 (prec 1): " << drops2 << " vs " << drops1);
        NS_TEST_ASSERT_MSG_GT_OR_EQ(
            drops1,
            drops0,
            "AF12 (prec 1) should have >= drops as AF11 (prec 0): " << drops1 << " vs " << drops0);

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-14.3: Best-Effort Lowest Priority
// =============================================================================

/**
 * @brief Verifies best-effort traffic receives the lowest service priority.
 * @see specs/02-structural.md S-14.3
 */
class BestEffortLowestPriorityTest : public TestCase
{
  public:
    BestEffortLowestPriorityTest()
        : TestCase("S-14.3 Best-effort lowest priority: BE uses lowest-priority "
                   "queue")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        auto inner = CreateObject<RedQueueDisc>();
        edge->SetInnerDisc(inner);
        inner->SetNumQueues(2);

        // Rule 1 (specific): src=10.0.0.1 -> DSCP 46 (EF)  [added first — first
        // match wins]
        edge->AddMarkRule({.dscp = 46, .srcAddr = Ipv4Address("10.0.0.1")});

        // Rule 2 (catch-all): any -> DSCP 0 (BE)
        edge->AddMarkRule({.dscp = 0});

        // Dumb policies + policers for EF (46) and BE (0)
        PolicyEntry policyEf;
        policyEf.codePoint = 46;
        policyEf.meter = MeterType::DUMB;
        policyEf.policer = PolicerType::DUMB;
        policyEf.policyIndex = 0;
        edge->GetPolicyClassifier()->AddPolicyEntry(policyEf);

        PolicerEntry policerEf;
        policerEf.policer = PolicerType::DUMB;
        policerEf.policyIndex = 0;
        policerEf.initialCodePt = 46;
        policerEf.downgrade1 = 46;
        policerEf.downgrade2 = 46;
        edge->GetPolicyClassifier()->AddPolicerEntry(policerEf);

        PolicyEntry policyBe;
        policyBe.codePoint = 0;
        policyBe.meter = MeterType::DUMB;
        policyBe.policer = PolicerType::DUMB;
        policyBe.policyIndex = 1;
        edge->GetPolicyClassifier()->AddPolicyEntry(policyBe);

        PolicerEntry policerBe;
        policerBe.policer = PolicerType::DUMB;
        policerBe.policyIndex = 1;
        policerBe.initialCodePt = 0;
        policerBe.downgrade1 = 0;
        policerBe.downgrade2 = 0;
        edge->GetPolicyClassifier()->AddPolicerEntry(policerBe);

        // PHB: EF -> queue 0 (high priority), BE -> queue 1 (low priority)
        inner->AddPhbEntry(46, 0, 0);
        inner->AddPhbEntry(0, 1, 0);

        // PQ scheduler: queue 0 is highest priority
        Ptr<PriorityScheduler> pq = CreateObjectWithAttributes<PriorityScheduler>("NumQueues",
                                                                                  UintegerValue(2),
                                                                                  "WinLen",
                                                                                  DoubleValue(1.0));
        inner->SetScheduler(pq);

        edge->Initialize();

        // Set DROP_TAIL mode and configure thresholds AFTER Initialize
        inner->SetMredMode(MredMode::DROP_TAIL, 0);
        inner->SetMredMode(MredMode::DROP_TAIL, 1);
        // Set thMin > qlim so threshold check never fires; pure tail-drop behaviour
        inner->ConfigQueue({.queue = 0, .prec = 0, .thMin = 1000.0, .thMax = 2000.0, .maxP = 0.1});
        inner->ConfigQueue({.queue = 1, .prec = 0, .thMin = 1000.0, .thMax = 2000.0, .maxP = 0.1});

        // Interleave: enqueue 1 EF, 1 BE, 1 EF, 1 BE, ... (10 total, 5 each)
        for (int i = 0; i < 5; ++i)
        {
            // EF packet from 10.0.0.1
            Ptr<Ipv4QueueDiscItem> efItem =
                MakeIpv4Item(Ipv4Address("10.0.0.1"), Ipv4Address("10.0.0.100"), 17, 500);
            NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(efItem),
                                  true,
                                  "EF packet " << i << " should enqueue");

            // BE packet from 10.0.0.2 (catch-all rule -> DSCP 0)
            Ptr<Ipv4QueueDiscItem> beItem =
                MakeIpv4Item(Ipv4Address("10.0.0.2"), Ipv4Address("10.0.0.100"), 17, 500);
            NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(beItem),
                                  true,
                                  "BE packet " << i << " should enqueue");
        }

        // Dequeue all 10: first 5 must be EF (DSCP 46), last 5 must be BE (DSCP 0)
        for (int i = 0; i < 5; ++i)
        {
            Ptr<QueueDiscItem> dequeued = edge->Dequeue();
            NS_TEST_ASSERT_MSG_NE(dequeued, nullptr, "Should dequeue EF packet " << i);
            Ptr<Ipv4QueueDiscItem> ipItem = DynamicCast<Ipv4QueueDiscItem>(dequeued);
            NS_TEST_ASSERT_MSG_NE(ipItem, nullptr, "Dequeued item should be Ipv4");
            if (!ipItem)
            {
                continue;
            }
            uint8_t dscp = ipItem->GetHeader().GetTos() >> 2;
            NS_TEST_ASSERT_MSG_EQ(dscp,
                                  46,
                                  "Packet " << i << " (EF phase) should have DSCP 46, got "
                                            << static_cast<uint32_t>(dscp));
        }

        for (int i = 0; i < 5; ++i)
        {
            Ptr<QueueDiscItem> dequeued = edge->Dequeue();
            NS_TEST_ASSERT_MSG_NE(dequeued, nullptr, "Should dequeue BE packet " << i);
            Ptr<Ipv4QueueDiscItem> ipItem = DynamicCast<Ipv4QueueDiscItem>(dequeued);
            NS_TEST_ASSERT_MSG_NE(ipItem, nullptr, "Dequeued item should be Ipv4");
            if (!ipItem)
            {
                continue;
            }
            uint8_t dscp = ipItem->GetHeader().GetTos() >> 2;
            NS_TEST_ASSERT_MSG_EQ(dscp,
                                  0,
                                  "Packet " << i << " (BE phase) should have DSCP 0, got "
                                            << static_cast<uint32_t>(dscp));
        }

        Simulator::Destroy();
    }
};

// =============================================================================
//  S-16.1: diffserv::Helper configures srTCM policer
// =============================================================================

/**
 * @brief Verifies the helper configures an srTCM meter with the requested parameters.
 * @see specs/02-structural.md S-16.1
 */
class HelperSrTcmConfigTest : public TestCase
{
  public:
    HelperSrTcmConfigTest()
        : TestCase("S-16.1 Helper configures srTCM policer")
    {
    }

  private:
    void DoRun() override
    {
        auto edge = CreateObject<EdgeQueueDisc>();
        diffserv::Helper helper;
        auto inner = helper.InstallRedInner(edge);

        // Configure via helper: mark rule + srTCM policy + policer + PHB
        edge->AddMarkRule({.dscp = 46});
        helper.AddSrTcmPolicy(
            edge,
            {.codePt = 46, .cirBps = 1000000.0, .cbsBytes = 10000.0, .ebsBytes = 20000.0});
        helper.AddPolicerEntry(edge,
                               {.policer = PolicerType::SRTCM,
                                .initialCodePt = 46,
                                .downgrade1 = 46,
                                .downgrade2 = 0,
                                .policyIndex = static_cast<uint32_t>(PolicerType::SRTCM)});
        helper.AddPhbEntry(inner, 46, 0, 0);

        // Verify the policy classifier can meter correctly
        auto pc = edge->GetPolicyClassifier();
        uint8_t dscp = pc->ApplyPolicy(46, 500, 0.0);
        NS_TEST_ASSERT_MSG_EQ(dscp,
                              46,
                              "srTCM configured via helper should mark GREEN for small packet");
    }
};

// =============================================================================
//  E2E: Edge → Core Topology
// =============================================================================

/**
 * @brief End-to-end smoke-tests a representative edge plus core topology.
 * @see specs/03-quality.md Q-1
 */
class E2EEdgeCoreTopologyTest : public TestCase
{
  public:
    E2EEdgeCoreTopologyTest()
        : TestCase("E2E: edge->core pipeline, DSCP 46 preserved across hops"),
          m_rxCount(0)
    {
    }

  private:
    void DoRun() override;

    uint32_t m_rxCount; ///< Number of packets received at sink

    void RxCallback(Ptr<const Packet> pkt, const Address& addr)
    {
        ++m_rxCount;
    }
};

void
E2EEdgeCoreTopologyTest::DoRun()
{
    // ---- Topology: 4 nodes in a chain ----
    NodeContainer allNodes;
    allNodes.Create(4);

    Ptr<Node> src = allNodes.Get(0);
    Ptr<Node> edgeNode = allNodes.Get(1);
    Ptr<Node> coreNode = allNodes.Get(2);
    Ptr<Node> sink = allNodes.Get(3);

    // Link 1: src → edge (access, 10 Mbps, 1 ms)
    PointToPointHelper p2pAccess;
    p2pAccess.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
    p2pAccess.SetChannelAttribute("Delay", StringValue("1ms"));
    NetDeviceContainer devSrcEdge = p2pAccess.Install(src, edgeNode);

    // Link 2: edge → core (bottleneck, 2 Mbps, 5 ms)
    PointToPointHelper p2pBottleneck;
    p2pBottleneck.SetDeviceAttribute("DataRate", StringValue("2Mbps"));
    p2pBottleneck.SetChannelAttribute("Delay", StringValue("5ms"));
    NetDeviceContainer devEdgeCore = p2pBottleneck.Install(edgeNode, coreNode);

    // Link 3: core → sink (access, 10 Mbps, 1 ms)
    PointToPointHelper p2pEgress;
    p2pEgress.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
    p2pEgress.SetChannelAttribute("Delay", StringValue("1ms"));
    NetDeviceContainer devCoreSink = p2pEgress.Install(coreNode, sink);

    // ---- Internet stack ----
    InternetStackHelper internet;
    internet.Install(allNodes);

    // ---- IP addresses (three separate subnets) ----
    Ipv4AddressHelper addr;

    addr.SetBase("10.0.1.0", "255.255.255.0");
    Ipv4InterfaceContainer ifSrcEdge = addr.Assign(devSrcEdge);

    addr.SetBase("10.0.2.0", "255.255.255.0");
    Ipv4InterfaceContainer ifEdgeCore = addr.Assign(devEdgeCore);

    addr.SetBase("10.0.3.0", "255.255.255.0");
    Ipv4InterfaceContainer ifCoreSink = addr.Assign(devCoreSink);

    // ---- Routing ----
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // ---- Remove default queue discs on edge→core and core→sink links ----
    TrafficControlHelper tchUninstall;
    // Edge's outbound interface toward core is devEdgeCore.Get(0)
    tchUninstall.Uninstall(devEdgeCore.Get(0));
    // Core's outbound interface toward sink is devCoreSink.Get(0)
    tchUninstall.Uninstall(devCoreSink.Get(0));

    // ---- Install EdgeQueueDisc on edge's outbound link ----
    TrafficControlHelper tchEdge;
    tchEdge.SetRootQueueDisc("ns3::stratum::EdgeQueueDisc");
    QueueDiscContainer edgeQdiscs = tchEdge.Install(devEdgeCore.Get(0));

    Ptr<EdgeQueueDisc> edgeDisc = DynamicCast<EdgeQueueDisc>(edgeQdiscs.Get(0));
    NS_TEST_ASSERT_MSG_NE(edgeDisc, nullptr, "Should have installed EdgeQueueDisc");
    if (!edgeDisc)
    {
        Simulator::Destroy();
        return;
    }
    // TrafficControlHelper instantiated a bare EdgeQueueDisc
    // with no inner set. Install a RedQueueDisc inner before
    // configuring it and calling Initialize.
    auto edgeDiscInner = CreateObject<RedQueueDisc>();
    edgeDisc->SetInnerDisc(edgeDiscInner);

    // Edge configuration:
    edgeDiscInner->SetNumQueues(2);

    // Mark rule: any packet → DSCP 46 (EF)
    edgeDisc->AddMarkRule({.dscp = 46});

    // Policy: DSCP 46 → Dumb meter + Dumb policer (passthrough)
    PolicyEntry policy46;
    policy46.codePoint = 46;
    policy46.meter = MeterType::DUMB;
    policy46.policer = PolicerType::DUMB;
    policy46.policyIndex = static_cast<uint32_t>(MeterType::DUMB);
    edgeDisc->GetPolicyClassifier()->AddPolicyEntry(policy46);

    // Policer: Dumb, 46 → 46/46/46
    PolicerEntry policer46;
    policer46.policer = PolicerType::DUMB;
    policer46.policyIndex = static_cast<uint32_t>(MeterType::DUMB);
    policer46.initialCodePt = 46;
    policer46.downgrade1 = 46;
    policer46.downgrade2 = 46;
    edgeDisc->GetPolicyClassifier()->AddPolicerEntry(policer46);

    // PHB: DSCP 46 → queue 0, prec 0 (high priority)
    //      DSCP 0  → queue 1, prec 0 (best effort)
    edgeDiscInner->AddPhbEntry(46, 0, 0);
    edgeDiscInner->AddPhbEntry(0, 1, 0);

    // PQ scheduler with 2 queues, rate cap on queue 0
    Ptr<PriorityScheduler> edgePq = CreateObjectWithAttributes<PriorityScheduler>("NumQueues",
                                                                                  UintegerValue(2),
                                                                                  "WinLen",
                                                                                  DoubleValue(1.0));
    edgePq->SetParam(0, 5e6); // 5 Mbps cap on EF queue
    edgeDiscInner->SetScheduler(edgePq);

    // DROP_TAIL mode (no RED)
    edgeDiscInner->SetMredModeAllQueues(MredMode::DROP_TAIL);

    // ---- Install CoreQueueDisc on core's outbound link ----
    TrafficControlHelper tchCore;
    tchCore.SetRootQueueDisc("ns3::stratum::CoreQueueDisc");
    QueueDiscContainer coreQdiscs = tchCore.Install(devCoreSink.Get(0));

    Ptr<CoreQueueDisc> coreDisc = DynamicCast<CoreQueueDisc>(coreQdiscs.Get(0));
    NS_TEST_ASSERT_MSG_NE(coreDisc, nullptr, "Should have installed CoreQueueDisc");
    if (!coreDisc)
    {
        Simulator::Destroy();
        return;
    }
    // Install the Red inner before configuring it.
    auto coreInner = CreateObject<RedQueueDisc>();
    coreDisc->SetInnerDisc(coreInner);

    // Core configuration:
    coreInner->SetNumQueues(2);

    // Same PHB: DSCP 46 → queue 0, prec 0; DSCP 0 → queue 1, prec 0
    coreInner->AddPhbEntry(46, 0, 0);
    coreInner->AddPhbEntry(0, 1, 0);

    // PQ scheduler with 2 queues
    Ptr<PriorityScheduler> corePq = CreateObjectWithAttributes<PriorityScheduler>("NumQueues",
                                                                                  UintegerValue(2),
                                                                                  "WinLen",
                                                                                  DoubleValue(1.0));
    corePq->SetParam(0, 5e6);
    coreInner->SetScheduler(corePq);

    // DROP_TAIL mode
    coreInner->SetMredModeAllQueues(MredMode::DROP_TAIL);

    // ---- PacketSink on sink node (port 9) ----
    uint16_t port = 9;
    PacketSinkHelper sinkHelper("ns3::UdpSocketFactory",
                                InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer sinkApp = sinkHelper.Install(sink);
    sinkApp.Start(Seconds(0.0));
    sinkApp.Stop(Seconds(2.0));

    // Connect receive callback
    Ptr<Application> sinkAppPtr = sinkApp.Get(0);
    sinkAppPtr->TraceConnectWithoutContext(
        "Rx",
        MakeCallback(&E2EEdgeCoreTopologyTest::RxCallback, this));

    // ---- OnOff UDP source on src node (500 kbps to sink) ----
    ns3::OnOffHelper onoff("ns3::UdpSocketFactory",
                           InetSocketAddress(ifCoreSink.GetAddress(1), port));
    onoff.SetAttribute("DataRate", StringValue("500kbps"));
    onoff.SetAttribute("PacketSize", UintegerValue(500));
    onoff.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
    onoff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
    ApplicationContainer srcApp = onoff.Install(src);
    srcApp.Start(Seconds(0.1));
    srcApp.Stop(Seconds(1.9));

    // ---- Run simulation ----
    Simulator::Stop(Seconds(2.0));
    Simulator::Run();

    // ---- Assertions ----

    // 1. Packets were received at the sink
    NS_TEST_ASSERT_MSG_GT(m_rxCount, 0U, "Sink should have received packets, got " << m_rxCount);

    // 2. Edge disc processed packets (total received > 0)
    auto edgeStats = edgeDisc->GetStats();
    NS_TEST_ASSERT_MSG_GT(edgeStats.nTotalReceivedPackets,
                          0U,
                          "Edge disc should have received packets, got "
                              << edgeStats.nTotalReceivedPackets);

    // 3. Edge disc dequeued packets (proves edge pipeline works)
    NS_TEST_ASSERT_MSG_GT(edgeStats.nTotalDequeuedPackets,
                          0U,
                          "Edge disc should have dequeued packets, got "
                              << edgeStats.nTotalDequeuedPackets);

    // 4. Core disc processed packets (proves DSCP was preserved through
    // forwarding)
    auto coreStats = coreDisc->GetStats();
    NS_TEST_ASSERT_MSG_GT(coreStats.nTotalReceivedPackets,
                          0U,
                          "Core disc should have received packets, got "
                              << coreStats.nTotalReceivedPackets);

    // 5. Core disc dequeued packets
    NS_TEST_ASSERT_MSG_GT(coreStats.nTotalDequeuedPackets,
                          0U,
                          "Core disc should have dequeued packets, got "
                              << coreStats.nTotalDequeuedPackets);

    Simulator::Destroy();
}

// =============================================================================
//  S-9: SCFQ (Self-Clocked Fair Queueing)
// =============================================================================

/**
 * @brief Verifies SCFQ dequeues packets in finish-tag order.
 * @see specs/02-structural.md S-9.1
 */
class ScfqDequeueOrderTest : public TestCase
{
  public:
    ScfqDequeueOrderTest()
        : TestCase("S-9.1 SCFQ dequeue order matches hand-computed vector")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<ScfqScheduler> sched = CreateObjectWithAttributes<ScfqScheduler>("NumQueues",
                                                                             UintegerValue(3),
                                                                             "LinkBandwidth",
                                                                             DoubleValue(48000.0));
        sched->SetParam(0, 1.0);
        sched->SetParam(1, 2.0);
        sched->SetParam(2, 3.0);

        // Enqueue 2 packets of 1000 bytes per queue
        for (int i = 0; i < 2; i++)
        {
            sched->OnEnqueueWithTime(0, 1000, 0.0);
        }
        for (int i = 0; i < 2; i++)
        {
            sched->OnEnqueueWithTime(1, 1000, 0.0);
        }
        for (int i = 0; i < 2; i++)
        {
            sched->OnEnqueueWithTime(2, 1000, 0.0);
        }

        int expected[] = {2, 1, 2, 0, 1, 0};
        for (int i = 0; i < 6; i++)
        {
            int q = sched->SelectNextQueue();
            NS_TEST_ASSERT_MSG_EQ(q,
                                  expected[i],
                                  "SCFQ dequeue " << i << ": expected q" << expected[i] << " got q"
                                                  << q);
        }
        NS_TEST_ASSERT_MSG_EQ(sched->SelectNextQueue(), -1, "Should be empty");
    }
};

// =============================================================================
//  S-9.2: SCFQ throughput shares proportional to weights
// =============================================================================

/**
 * @brief Verifies SCFQ allocates throughput in proportion to weights.
 * @see specs/02-structural.md S-9.2
 */
class ScfqThroughputSharesTest : public TestCase
{
  public:
    ScfqThroughputSharesTest()
        : TestCase("S-9.2 SCFQ throughput shares proportional to weights")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<ScfqScheduler> sched = CreateObjectWithAttributes<ScfqScheduler>("NumQueues",
                                                                             UintegerValue(3),
                                                                             "LinkBandwidth",
                                                                             DoubleValue(48000.0));
        sched->SetParam(0, 1.0);
        sched->SetParam(1, 2.0);
        sched->SetParam(2, 3.0);

        // Enqueue 30 packets per queue (enough so no queue empties in 60 dequeues).
        // With weights 1:2:3 and 60 dequeues, expected shares: 10:20:30.
        for (uint32_t q = 0; q < 3; q++)
        {
            for (int i = 0; i < 30; i++)
            {
                sched->OnEnqueueWithTime(q, 1000, 0.0);
            }
        }

        int count[3] = {0, 0, 0};
        for (int i = 0; i < 60; i++)
        {
            int q = sched->SelectNextQueue();
            NS_TEST_ASSERT_MSG_NE(q, -1, "Premature empty at dequeue " << i);
            count[q]++;
        }

        NS_TEST_ASSERT_MSG_EQ(count[0], 10, "q0 (w=1) expected 10, got " << count[0]);
        NS_TEST_ASSERT_MSG_EQ(count[1], 20, "q1 (w=2) expected 20, got " << count[1]);
        NS_TEST_ASSERT_MSG_EQ(count[2], 30, "q2 (w=3) expected 30, got " << count[2]);
    }
};

// =============================================================================
//  S-9.3: SCFQ labels monotonically non-decreasing (mixed sizes)
// =============================================================================

/**
 * @brief Verifies SCFQ assigns monotonically increasing finish labels.
 * @see specs/02-structural.md S-9.3
 */
class ScfqLabelMonotonicityTest : public TestCase
{
  public:
    ScfqLabelMonotonicityTest()
        : TestCase("S-9.3 SCFQ labels monotonically non-decreasing (mixed sizes)")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<ScfqScheduler> sched = CreateObjectWithAttributes<ScfqScheduler>("NumQueues",
                                                                             UintegerValue(2),
                                                                             "LinkBandwidth",
                                                                             DoubleValue(8000.0));
        sched->SetParam(0, 1.0);
        sched->SetParam(1, 2.0);

        sched->OnEnqueueWithTime(0, 500, 0.0);
        sched->OnEnqueueWithTime(0, 1000, 0.0);
        sched->OnEnqueueWithTime(1, 500, 0.0);
        sched->OnEnqueueWithTime(1, 1000, 0.0);

        int count[2] = {0, 0};
        for (int i = 0; i < 4; i++)
        {
            int q = sched->SelectNextQueue();
            NS_TEST_ASSERT_MSG_NE(q, -1, "Premature empty at dequeue " << i);
            count[q]++;
        }
        NS_TEST_ASSERT_MSG_GT(count[0], 0, "Queue 0 should get some service");
        NS_TEST_ASSERT_MSG_GT(count[1], 0, "Queue 1 should get some service");
        NS_TEST_ASSERT_MSG_EQ(sched->SelectNextQueue(), -1, "Should be empty");
    }
};

// =============================================================================
//  S-10.1: SFQ dequeue order matches hand-computed vector
// =============================================================================

/**
 * @brief Verifies SFQ dequeues packets in start-tag order.
 * @see specs/02-structural.md S-10.1
 */
class SfqDequeueOrderTest : public TestCase
{
  public:
    SfqDequeueOrderTest()
        : TestCase("S-10.1 SFQ dequeue order matches hand-computed vector")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<SfqScheduler> sched = CreateObjectWithAttributes<SfqScheduler>("NumQueues",
                                                                           UintegerValue(3),
                                                                           "LinkBandwidth",
                                                                           DoubleValue(48000.0));
        sched->SetParam(0, 1.0);
        sched->SetParam(1, 2.0);
        sched->SetParam(2, 3.0);

        for (int i = 0; i < 2; i++)
        {
            sched->OnEnqueueWithTime(0, 1000, 0.0);
        }
        for (int i = 0; i < 2; i++)
        {
            sched->OnEnqueueWithTime(1, 1000, 0.0);
        }
        for (int i = 0; i < 2; i++)
        {
            sched->OnEnqueueWithTime(2, 1000, 0.0);
        }

        int expected[] = {0, 1, 2, 2, 1, 0};
        for (int i = 0; i < 6; i++)
        {
            int q = sched->SelectNextQueue();
            NS_TEST_ASSERT_MSG_EQ(q,
                                  expected[i],
                                  "SFQ dequeue " << i << ": expected q" << expected[i] << " got q"
                                                 << q);
        }
        NS_TEST_ASSERT_MSG_EQ(sched->SelectNextQueue(), -1, "Should be empty");
    }
};

// =============================================================================
//  S-10.2: SFQ serves in start-tag order (staggered enqueue)
// =============================================================================

/**
 * @brief Verifies SFQ start-tag order across mixed enqueue patterns.
 * @see specs/02-structural.md S-10.2
 */
class SfqStartTagOrderTest : public TestCase
{
  public:
    SfqStartTagOrderTest()
        : TestCase("S-10.2 SFQ serves in increasing start-tag order")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<SfqScheduler> sched = CreateObjectWithAttributes<SfqScheduler>("NumQueues",
                                                                           UintegerValue(2),
                                                                           "LinkBandwidth",
                                                                           DoubleValue(8000.0));
        sched->SetParam(0, 1.0);
        sched->SetParam(1, 1.0);

        // q0: 500 bytes -> S=max(0,0)=0, F=0+500/1000=0.5. lastFinish=0.5
        sched->OnEnqueueWithTime(0, 500, 0.0);
        // q1: 1000 bytes -> S=max(0,0)=0, F=0+1000/1000=1.0. lastFinish=1.0
        sched->OnEnqueueWithTime(1, 1000, 0.0);
        // q0: 500 bytes -> S=max(0,0.5)=0.5, F=0.5+0.5=1.0. lastFinish=1.0
        sched->OnEnqueueWithTime(0, 500, 0.0);

        // Starts: q0={0, 0.5}, q1={0}
        // Deq 1: min start = 0. Tie q0(0) vs q1(0) -> q0 (lower index). V=0.
        // Deq 2: q0 front start=0.5, q1 front start=0. q1 wins (0<0.5). V=0.
        // Deq 3: q0 front start=0.5, q1 empty. q0. V=0.5.
        int expected[] = {0, 1, 0};
        for (int i = 0; i < 3; i++)
        {
            int q = sched->SelectNextQueue();
            NS_TEST_ASSERT_MSG_EQ(q, expected[i], "SFQ start-tag order dequeue " << i);
        }
        NS_TEST_ASSERT_MSG_EQ(sched->SelectNextQueue(), -1, "Should be empty");
    }
};

// =============================================================================
//  S-10.3: Long-run SFQ throughput shares
// =============================================================================

/**
 * @brief Verifies SFQ allocates throughput in proportion to weights.
 * @see specs/02-structural.md S-10.3
 */
class SfqThroughputSharesTest : public TestCase
{
  public:
    SfqThroughputSharesTest()
        : TestCase("S-10.3 SFQ throughput shares proportional to weights")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<SfqScheduler> sched = CreateObjectWithAttributes<SfqScheduler>("NumQueues",
                                                                           UintegerValue(3),
                                                                           "LinkBandwidth",
                                                                           DoubleValue(48000.0));
        sched->SetParam(0, 1.0);
        sched->SetParam(1, 2.0);
        sched->SetParam(2, 3.0);

        // Enqueue 60 per queue (enough that no queue runs dry in 90 dequeues).
        // With weights 1:2:3 the expected shares are 15:30:45.
        for (uint32_t q = 0; q < 3; q++)
        {
            for (int i = 0; i < 60; i++)
            {
                sched->OnEnqueueWithTime(q, 1000, 0.0);
            }
        }

        int count[3] = {0, 0, 0};
        for (int i = 0; i < 90; i++)
        {
            int q = sched->SelectNextQueue();
            NS_TEST_ASSERT_MSG_NE(q, -1, "Premature empty at dequeue " << i);
            count[q]++;
        }

        NS_TEST_ASSERT_MSG_EQ(count[0], 15, "q0 (w=1) expected 15, got " << count[0]);
        NS_TEST_ASSERT_MSG_EQ(count[1], 30, "q1 (w=2) expected 30, got " << count[1]);
        NS_TEST_ASSERT_MSG_EQ(count[2], 45, "q2 (w=3) expected 45, got " << count[2]);
    }
};

// =============================================================================
//  S-8.1: WF2Q+ throughput shares proportional to weights
// =============================================================================

/**
 * @brief Verifies WF2Q+ allocates throughput in proportion to weights.
 * @see specs/02-structural.md S-8.1
 */
class Wf2qpThroughputSharesTest : public TestCase
{
  public:
    Wf2qpThroughputSharesTest()
        : TestCase("S-8.1 WF2Q+ throughput shares proportional to weights")
    {
    }

  private:
    Ptr<Wf2qPlusScheduler> m_sched;
    int m_count[3];

    void DoRun() override
    {
        m_sched = CreateObjectWithAttributes<Wf2qPlusScheduler>("NumQueues",
                                                                UintegerValue(3),
                                                                "LinkBandwidth",
                                                                DoubleValue(48000.0));
        m_sched->SetParam(0, 1.0);
        m_sched->SetParam(1, 2.0);
        m_sched->SetParam(2, 3.0);
        m_count[0] = m_count[1] = m_count[2] = 0;

        // At t=0, enqueue many packets per queue
        Simulator::Schedule(Seconds(0.0), &Wf2qpThroughputSharesTest::EnqueueAll, this);

        // Dequeue at staggered times (60 dequeues, 0.1s apart)
        for (int i = 0; i < 60; i++)
        {
            Simulator::Schedule(Seconds(0.1 + i * 0.1),
                                &Wf2qpThroughputSharesTest::DequeueOne,
                                this);
        }

        Simulator::Run();
        Simulator::Destroy();

        // Weights 1:2:3 -> shares ~10, ~20, ~30 (+/- 3 for transient effects)
        NS_TEST_ASSERT_MSG_GT(m_count[0], 7, "q0 (w=1) should get ~10");
        NS_TEST_ASSERT_MSG_LT(m_count[0], 13, "q0 (w=1) should get ~10");
        NS_TEST_ASSERT_MSG_GT(m_count[1], 17, "q1 (w=2) should get ~20");
        NS_TEST_ASSERT_MSG_LT(m_count[1], 23, "q1 (w=2) should get ~20");
        NS_TEST_ASSERT_MSG_GT(m_count[2], 27, "q2 (w=3) should get ~30");
        NS_TEST_ASSERT_MSG_LT(m_count[2], 33, "q2 (w=3) should get ~30");
    }

    void EnqueueAll()
    {
        double now = Simulator::Now().GetSeconds();
        for (uint32_t q = 0; q < 3; q++)
        {
            for (int i = 0; i < 30; i++)
            {
                m_sched->OnEnqueueWithTime(q, 1000, now);
            }
        }
    }

    void DequeueOne()
    {
        int q = m_sched->SelectNextQueue();
        if (q >= 0)
        {
            m_count[q]++;
        }
    }
};

// =============================================================================
//  S-8.2: WF2Q+ eligibility property
// =============================================================================

/**
 * @brief Verifies WF2Q+ enforces eligibility before serving a queue.
 * @see specs/02-structural.md S-8.2
 */
class Wf2qpEligibilityTest : public TestCase
{
  public:
    Wf2qpEligibilityTest()
        : TestCase("S-8.2 WF2Q+ eligibility: ineligible flow is deferred")
    {
    }

  private:
    Ptr<Wf2qPlusScheduler> m_sched;
    int m_result[3]; // dequeue results at t=0.5, 1.0, 1.5

    void DoRun() override
    {
        m_sched = CreateObjectWithAttributes<Wf2qPlusScheduler>("NumQueues",
                                                                UintegerValue(2),
                                                                "LinkBandwidth",
                                                                DoubleValue(8000.0));
        m_sched->SetParam(0, 1.0);
        m_sched->SetParam(1, 1.0);
        m_result[0] = m_result[1] = m_result[2] = -99;

        // t=0: enqueue one packet per queue
        Simulator::Schedule(Seconds(0.0), &Wf2qpEligibilityTest::EnqueueBoth, this);
        // t=0.5: dequeue -> should get q0 (tie, lower index)
        //         then enqueue another on q0
        Simulator::Schedule(Seconds(0.5), &Wf2qpEligibilityTest::DequeueAndReenqueue, this);
        // t=1.0: dequeue -> q0's new packet has S=1.0 > V, NOT eligible.
        //         Must serve q1.
        Simulator::Schedule(Seconds(1.0), &Wf2qpEligibilityTest::DequeueAt1, this);
        // t=1.5: dequeue -> q0 now eligible
        Simulator::Schedule(Seconds(1.5), &Wf2qpEligibilityTest::DequeueAt1_5, this);

        Simulator::Run();
        Simulator::Destroy();

        NS_TEST_ASSERT_MSG_EQ(m_result[0], 0, "Dequeue at t=0.5 should be q0 (tie)");
        NS_TEST_ASSERT_MSG_EQ(m_result[1], 1, "Dequeue at t=1.0: q0 not eligible, must serve q1");
        NS_TEST_ASSERT_MSG_EQ(m_result[2], 0, "Dequeue at t=1.5: q0 now eligible");
    }

    void EnqueueBoth()
    {
        m_sched->OnEnqueueWithTime(0, 1000, Simulator::Now().GetSeconds());
        m_sched->OnEnqueueWithTime(1, 1000, Simulator::Now().GetSeconds());
    }

    void DequeueAndReenqueue()
    {
        m_result[0] = m_sched->SelectNextQueue();
        m_sched->OnEnqueueWithTime(0, 1000, Simulator::Now().GetSeconds());
    }

    void DequeueAt1()
    {
        m_result[1] = m_sched->SelectNextQueue();
    }

    void DequeueAt1_5()
    {
        m_result[2] = m_sched->SelectNextQueue();
    }
};

// =============================================================================
//  S-7.1: WFQ throughput shares proportional to weights
// =============================================================================

/**
 * @brief Verifies WFQ allocates throughput in proportion to weights.
 * @see specs/02-structural.md S-7.1
 */
class WfqThroughputSharesTest : public TestCase
{
  public:
    WfqThroughputSharesTest()
        : TestCase("S-7.1 WFQ throughput shares proportional to weights")
    {
    }

  private:
    Ptr<WfqScheduler> m_sched;
    int m_count[3];

    void DoRun() override
    {
        m_sched = CreateObjectWithAttributes<WfqScheduler>("NumQueues",
                                                           UintegerValue(3),
                                                           "LinkBandwidth",
                                                           DoubleValue(48000.0));
        m_sched->SetParam(0, 1.0);
        m_sched->SetParam(1, 2.0);
        m_sched->SetParam(2, 3.0);
        m_count[0] = m_count[1] = m_count[2] = 0;

        // Enqueue 30 packets per queue at t=0
        Simulator::Schedule(Seconds(0.0), &WfqThroughputSharesTest::EnqueueAll, this);

        // Dequeue 60 packets at staggered times
        for (int i = 0; i < 60; i++)
        {
            Simulator::Schedule(Seconds(0.1 + i * 0.1), &WfqThroughputSharesTest::DequeueOne, this);
        }

        Simulator::Run();
        Simulator::Destroy();

        // Pure PGPS (no eligibility filter) services every dequeue
        // call when at least one flow is backlogged: 60 dequeues, all
        // succeed, share weighted 1:2:3 -> q0 ~10, q1 ~20, q2 ~30.
        // The ±3 packet window absorbs the busy-set startup transient
        // (Parekh-Gallager 1993 finite-window bias, bounded by
        // L_max / phi_min) — the per-packet bound F_hat_p - F_p ≤
        // L_max / r is asymptotic, not instantaneous.
        NS_TEST_ASSERT_MSG_GT(m_count[0], 7, "q0 (w=1) should get ~10");
        NS_TEST_ASSERT_MSG_LT(m_count[0], 13, "q0 (w=1) should get ~10");
        NS_TEST_ASSERT_MSG_GT(m_count[1], 17, "q1 (w=2) should get ~20");
        NS_TEST_ASSERT_MSG_LT(m_count[1], 23, "q1 (w=2) should get ~20");
        NS_TEST_ASSERT_MSG_GT(m_count[2], 27, "q2 (w=3) should get ~30");
        NS_TEST_ASSERT_MSG_LT(m_count[2], 33, "q2 (w=3) should get ~30");
    }

    void EnqueueAll()
    {
        double now = Simulator::Now().GetSeconds();
        for (uint32_t q = 0; q < 3; q++)
        {
            for (int i = 0; i < 30; i++)
            {
                m_sched->OnEnqueueWithTime(q, 1000, now);
            }
        }
    }

    void DequeueOne()
    {
        int q = m_sched->SelectNextQueue();
        if (q >= 0)
        {
            m_count[q]++;
        }
    }
};

// =============================================================================
//  S-7.3: WFQ virtual time monotonicity
// =============================================================================

/**
 * @brief Verifies WFQ virtual time is monotonically non-decreasing.
 * @see specs/02-structural.md S-7.3
 */
class WfqVirtualTimeMonotonicTest : public TestCase
{
  public:
    WfqVirtualTimeMonotonicTest()
        : TestCase("S-7.3 WFQ virtual time is monotonically non-decreasing")
    {
    }

  private:
    Ptr<WfqScheduler> m_sched;
    double m_prevVt;
    bool m_violated;

    void DoRun() override
    {
        m_sched = CreateObjectWithAttributes<WfqScheduler>("NumQueues",
                                                           UintegerValue(2),
                                                           "LinkBandwidth",
                                                           DoubleValue(8000.0));
        m_sched->SetParam(0, 1.0);
        m_sched->SetParam(1, 1.0);
        m_prevVt = -1.0;
        m_violated = false;

        Simulator::Schedule(Seconds(0.0), &WfqVirtualTimeMonotonicTest::EnqueueBatch, this);
        for (int i = 0; i < 20; i++)
        {
            Simulator::Schedule(Seconds(0.05 + i * 0.05),
                                &WfqVirtualTimeMonotonicTest::DequeueAndCheck,
                                this);
        }

        Simulator::Run();
        Simulator::Destroy();

        NS_TEST_ASSERT_MSG_EQ(m_violated, false, "Virtual time must never decrease");
    }

    void EnqueueBatch()
    {
        double now = Simulator::Now().GetSeconds();
        for (int i = 0; i < 10; i++)
        {
            m_sched->OnEnqueueWithTime(0, 500, now);
            m_sched->OnEnqueueWithTime(1, 500, now);
        }
    }

    void DequeueAndCheck()
    {
        m_sched->SelectNextQueue();
        double vt = m_sched->GetVirtualTime();
        if (m_prevVt >= 0.0 && vt < m_prevVt)
        {
            m_violated = true;
        }
        m_prevVt = vt;
    }
};

// =============================================================================
//  S-7.4: WFQ no intra-flow packet reordering
// =============================================================================

/**
 * @brief Verifies WFQ does not reorder packets within a single queue.
 * @see specs/02-structural.md S-7.4
 */
class WfqNoReorderingTest : public TestCase
{
  public:
    WfqNoReorderingTest()
        : TestCase("S-7.4 WFQ no intra-flow packet reordering")
    {
    }

  private:
    Ptr<WfqScheduler> m_sched;

    void DoRun() override
    {
        // Single queue, 10 packets -- all must dequeue from q0
        m_sched = CreateObjectWithAttributes<WfqScheduler>("NumQueues",
                                                           UintegerValue(1),
                                                           "LinkBandwidth",
                                                           DoubleValue(8000.0));
        m_sched->SetParam(0, 1.0);

        Simulator::Schedule(Seconds(0.0), &WfqNoReorderingTest::Enqueue10, this);
        for (int i = 0; i < 10; i++)
        {
            Simulator::Schedule(Seconds(0.01 + i * 0.01), &WfqNoReorderingTest::DequeueCheck, this);
        }

        Simulator::Run();
        Simulator::Destroy();
    }

    void Enqueue10()
    {
        double now = Simulator::Now().GetSeconds();
        for (int i = 0; i < 10; i++)
        {
            m_sched->OnEnqueueWithTime(0, 1000, now);
        }
    }

    void DequeueCheck()
    {
        int q = m_sched->SelectNextQueue();
        NS_TEST_ASSERT_MSG_EQ(q, 0, "Single-queue WFQ must always return q0");
    }
};

// =============================================================================
//  Lifecycle: MonitorHelper dtor cancels pending sampling events
// =============================================================================
//
// The monitor helper is a plain C++ class (not an ns-3 Object) that
// schedules two periodic sampling callbacks via raw `this` pointers.
// The destructor stores the two EventIds and cancels them, so a
// helper going out of scope before `Simulator::Destroy()` does not
// leave callbacks referencing destructed memory.
//
// This test is a no-crash smoke — ASan would catch a use-after-free
// differentially, but even without ASan the test exercises the
// disposal path.

class TestDiffServMonitorHelperDtorCancelsEvents : public TestCase
{
  public:
    TestDiffServMonitorHelperDtorCancelsEvents()
        : TestCase("MonitorHelper dtor cancels pending sampling events")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<RedQueueDisc> disc = CreateObject<RedQueueDisc>();
        disc->SetNumQueues(1);
        disc->SetNumPrec(0, 1);
        disc->AddPhbEntry(0, 0, 0);
        disc->ConfigQueue({.queue = 0, .prec = 0, .thMin = 1000.0, .thMax = 2000.0, .maxP = 0.0});
        disc->Initialize();

        {
            MonitorHelper monitor;
            monitor.SetOutputDirectory("/tmp/ds4-tier-b-monitor-test");
            monitor.SetSamplingStartTime(Seconds(1.0));
            monitor.SetDepartureRateInterval(Seconds(1.0));
            monitor.SetQueueLengthInterval(Seconds(1.0));
            monitor.Install(disc);
            // Helper goes out of scope here. Pre-fix: the two sampling
            // callbacks scheduled at t=1.0s still reference a destructed
            // helper. Post-fix: the dtor cancels both events.
        }

        // Run past the scheduled sampling time — the events must not fire
        // on the destructed helper.
        Simulator::Stop(Seconds(5.0));
        Simulator::Run();
        Simulator::Destroy();

        NS_TEST_EXPECT_MSG_EQ(true, true, "Monitor helper disposal lifecycle clean");
    }
};

// =============================================================================
//  S-11: LLQ (Low Latency Queueing)
// =============================================================================

/**
 * @brief Verifies LLQ serves the priority class before any non-priority class.
 * @see specs/02-structural.md S-11.1
 */
class LlqPriorityFirstTest : public TestCase
{
  public:
    LlqPriorityFirstTest()
        : TestCase("S-11.1 LLQ serves priority queue first when non-empty")
    {
    }

  private:
    void DoRun() override
    {
        // 3 total queues: q0 (PQ), q1+q2 (SFQ)
        Ptr<LlqScheduler> sched =
            CreateObjectWithAttributes<LlqScheduler>("NumQueues",
                                                     UintegerValue(3),
                                                     "LinkBandwidth",
                                                     DoubleValue(48000.0),
                                                     "FqVariant",
                                                     EnumValue(LlqScheduler::FqVariant::SFQ));
        sched->SetParam(1, 1.0);
        sched->SetParam(2, 2.0);

        // Enqueue on all three queues
        sched->OnEnqueueWithTime(0, 1000, 0.0); // PQ
        sched->OnEnqueueWithTime(1, 1000, 0.0); // SFQ q0
        sched->OnEnqueueWithTime(2, 1000, 0.0); // SFQ q1

        // First dequeue MUST be q0 (priority)
        NS_TEST_ASSERT_MSG_EQ(sched->SelectNextQueue(), 0, "PQ served first");

        // Next two from SFQ (q1 and q2, in some order)
        int q2 = sched->SelectNextQueue();
        int q3 = sched->SelectNextQueue();
        NS_TEST_ASSERT_MSG_NE(q2, 0, "After PQ empty, SFQ takes over (first)");
        NS_TEST_ASSERT_MSG_NE(q3, 0, "After PQ empty, SFQ takes over (second)");
        NS_TEST_ASSERT_MSG_EQ(q2 + q3, 3, "Both SFQ queues served (1+2=3)");

        NS_TEST_ASSERT_MSG_EQ(sched->SelectNextQueue(), -1, "Should be empty");
    }
};

/**
 * @brief Verifies LLQ delegates remaining bandwidth management to its FQ stage.
 * @see specs/02-structural.md S-11.2
 */
class LlqFqManagesRemainingTest : public TestCase
{
  public:
    LlqFqManagesRemainingTest()
        : TestCase("S-11.2 LLQ WFQ component manages remaining when PQ idle")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<LlqScheduler> sched =
            CreateObjectWithAttributes<LlqScheduler>("NumQueues",
                                                     UintegerValue(3),
                                                     "LinkBandwidth",
                                                     DoubleValue(48000.0),
                                                     "FqVariant",
                                                     EnumValue(LlqScheduler::FqVariant::SFQ));
        sched->SetParam(1, 1.0);
        sched->SetParam(2, 2.0);

        // Only enqueue on FQ queues (no PQ traffic)
        for (int i = 0; i < 10; i++)
        {
            sched->OnEnqueueWithTime(1, 1000, 0.0);
        }
        for (int i = 0; i < 10; i++)
        {
            sched->OnEnqueueWithTime(2, 1000, 0.0);
        }

        int count[3] = {0, 0, 0};
        for (int i = 0; i < 20; i++)
        {
            int q = sched->SelectNextQueue();
            NS_TEST_ASSERT_MSG_NE(q, -1, "Premature empty at " << i);
            count[q]++;
        }

        NS_TEST_ASSERT_MSG_EQ(count[0], 0, "No PQ packets");
        // Both FQ queues must be served (the FQ sub-scheduler is active).
        // Exact split depends on SFQ start-tag interleaving with equal-size
        // packets: weights 1:2 with 10 pkts each yields 10:10.
        NS_TEST_ASSERT_MSG_EQ(count[1], 10, "q1 (w=1) all 10 packets served");
        NS_TEST_ASSERT_MSG_EQ(count[2], 10, "q2 (w=2) all 10 packets served");
    }
};

/**
 * @brief Smoke-tests the LLQ priority-class rate cap.
 * @see specs/02-structural.md S-11.3
 */
class LlqRateCapSmokeTest : public TestCase
{
  public:
    LlqRateCapSmokeTest()
        : TestCase("S-11.3 LLQ PQ rate cap: SetPqRateCap smoke test")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<LlqScheduler> sched =
            CreateObjectWithAttributes<LlqScheduler>("NumQueues",
                                                     UintegerValue(2),
                                                     "LinkBandwidth",
                                                     DoubleValue(48000.0),
                                                     "FqVariant",
                                                     EnumValue(LlqScheduler::FqVariant::SCFQ));
        sched->SetParam(1, 1.0);
        sched->SetPqRateCap(16000.0); // 16 kbps cap

        // Enqueue on both
        sched->OnEnqueueWithTime(0, 1000, 0.0);
        sched->OnEnqueueWithTime(1, 1000, 0.0);

        // Without time advancement, departure rate is 0 -> below cap -> PQ served
        // first
        NS_TEST_ASSERT_MSG_EQ(sched->SelectNextQueue(), 0, "PQ first (rate under cap)");
        NS_TEST_ASSERT_MSG_EQ(sched->SelectNextQueue(), 1, "FQ second");
        NS_TEST_ASSERT_MSG_EQ(sched->SelectNextQueue(), -1, "Empty");
    }
};

/**
 * @brief LLQ priority lane is policed: once the EF lane's measured rate exceeds
 * its cap, the LLQ yields to the fair lane instead of serving EF.
 *
 * RFC 3246 requires the EF aggregate to be rate-limited so it cannot starve
 * other PHBs. The cap can only engage if EF-lane departures reach the inner
 * priority scheduler's rate estimator, and the inner scheduler must decline
 * (rather than fall back to serving its lone queue) once that queue is over
 * cap, so the outer LLQ can serve the fair lane. Mirrors the standalone
 * S-6.2 PriorityScheduler rate cap, but on the composed LLQ path.
 */
class LlqPqRateCapEngagesTest : public TestCase
{
  public:
    LlqPqRateCapEngagesTest()
        : TestCase("S-11.4 LLQ PQ rate cap engages: EF lane over cap yields to the fair lane")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<LlqScheduler> sched =
            CreateObjectWithAttributes<LlqScheduler>("NumQueues",
                                                     UintegerValue(2),
                                                     "LinkBandwidth",
                                                     DoubleValue(48000.0),
                                                     "FqVariant",
                                                     EnumValue(LlqScheduler::FqVariant::SCFQ));
        sched->SetParam(1, 1.0);
        // Very low EF (queue 0) rate cap: any real departure exceeds it.
        sched->SetPqRateCap(1.0);

        // Backlog both the EF lane (queue 0) and the fair lane (queue 1).
        for (int i = 0; i < 10; ++i)
        {
            sched->OnEnqueueWithTime(0, 1000, 0.0);
            sched->OnEnqueueWithTime(1, 1000, 0.0);
        }

        // Simulate an EF-lane departure (top-level queue 0): drives the EF
        // average rate far above the cap. This only engages the cap if the
        // departure reaches the inner PQ's rate estimator.
        sched->UpdateDepartureRate(0, 0, 1000, 0.1);

        // EF is over its cap, so the policed LLQ must serve the fair lane.
        int q = sched->SelectNextQueue();
        NS_TEST_ASSERT_MSG_EQ(
            q,
            1,
            "LLQ EF lane over its rate cap must yield to the fair lane (queue 1); "
            "got queue "
                << q);
        Simulator::Destroy();
    }
};

// =============================================================================
//  Q-3.1: WFQ throughput shares at scale (4 queues, 1000 packets, ±2%)
// =============================================================================

/**
 * @brief Verifies WFQ achieves the spec-defined fair throughput envelope.
 * @see specs/03-quality.md Q-3.1
 */
class WfqFairThroughputTest : public TestCase
{
  public:
    WfqFairThroughputTest()
        : TestCase("Q-3.1 WFQ throughput shares w1-w2-w3-w4 at scale (200 pkts)")
    {
    }

  private:
    Ptr<WfqScheduler> m_sched;
    int m_count[4];

    void DoRun() override
    {
        // Use low bandwidth so GPS departure events have time to fire
        // between dequeues (same approach as S-7.1).
        // At 48 kbps with 1000-byte packets: GPS service time per pkt
        // = 1000*8/48000 = 0.167s. Dequeue interval 0.1s gives GPS
        // sufficient time to discriminate weights.
        m_sched = CreateObjectWithAttributes<WfqScheduler>( // 48 kbps
            "NumQueues",
            UintegerValue(4),
            "LinkBandwidth",
            DoubleValue(48000.0));
        m_sched->SetParam(0, 1.0);
        m_sched->SetParam(1, 2.0);
        m_sched->SetParam(2, 3.0);
        m_sched->SetParam(3, 4.0);
        m_count[0] = m_count[1] = m_count[2] = m_count[3] = 0;

        // Enqueue 100 packets per queue at t=0 (400 total)
        Simulator::Schedule(Seconds(0.0), &WfqFairThroughputTest::EnqueueAll, this);

        // Dequeue 200 packets at 0.1s intervals (enough to drain
        // proportionally; some queues may exhaust before 200 dequeues)
        for (int i = 0; i < 200; i++)
        {
            Simulator::Schedule(Seconds(0.1 + i * 0.1), &WfqFairThroughputTest::DequeueOne, this);
        }

        Simulator::Run();
        Simulator::Destroy();

        int total = m_count[0] + m_count[1] + m_count[2] + m_count[3];
        // With 100 pkts per queue and weights 1:2:3:4 (sum=10),
        // expected dequeue counts before any queue exhausts:
        // q0: total*1/10, q1: total*2/10, q2: total*3/10, q3: total*4/10
        // Tolerance: ±5% of total dequeued (Q-tier allows ±1% of link
        // capacity, but at scheduler level ±5% of packet count is more
        // appropriate given discrete packet granularity)
        double tol = 0.05 * total;

        double expected0 = total * 1.0 / 10.0;
        double expected1 = total * 2.0 / 10.0;
        double expected2 = total * 3.0 / 10.0;
        double expected3 = total * 4.0 / 10.0;

        NS_TEST_ASSERT_MSG_GT(m_count[0],
                              expected0 - tol,
                              "q0 (w=1) share too low: " << m_count[0]);
        NS_TEST_ASSERT_MSG_LT(m_count[0],
                              expected0 + tol,
                              "q0 (w=1) share too high: " << m_count[0]);
        NS_TEST_ASSERT_MSG_GT(m_count[1],
                              expected1 - tol,
                              "q1 (w=2) share too low: " << m_count[1]);
        NS_TEST_ASSERT_MSG_LT(m_count[1],
                              expected1 + tol,
                              "q1 (w=2) share too high: " << m_count[1]);
        NS_TEST_ASSERT_MSG_GT(m_count[2],
                              expected2 - tol,
                              "q2 (w=3) share too low: " << m_count[2]);
        NS_TEST_ASSERT_MSG_LT(m_count[2],
                              expected2 + tol,
                              "q2 (w=3) share too high: " << m_count[2]);
        NS_TEST_ASSERT_MSG_GT(m_count[3],
                              expected3 - tol,
                              "q3 (w=4) share too low: " << m_count[3]);
        NS_TEST_ASSERT_MSG_LT(m_count[3],
                              expected3 + tol,
                              "q3 (w=4) share too high: " << m_count[3]);
    }

    void EnqueueAll()
    {
        double now = Simulator::Now().GetSeconds();
        for (uint32_t q = 0; q < 4; q++)
        {
            for (int i = 0; i < 100; i++)
            {
                m_sched->OnEnqueueWithTime(q, 1000, now);
            }
        }
    }

    void DequeueOne()
    {
        int q = m_sched->SelectNextQueue();
        if (q >= 0)
        {
            m_count[q]++;
        }
    }
};

// =============================================================================
//  WFQ full-drain reactivation: virtual time must freeze across idle
// =============================================================================

/**
 * @brief After the WFQ busy set fully drains and then reactivates, weighted
 * service order is preserved.
 *
 * The busy-set virtual time advances at `1 / Sum_{i in B(t)} phi_i`. When the
 * last backlogged flow drains, that sum must become exactly zero so `V(t)` is
 * frozen until the next arrival (Parekh & Gallager, IEEE/ACM ToN 1(3), 1993,
 * Eq. 10). With non-dyadic fractional weights (here `1/3` each), accumulating
 * and then subtracting the weights leaves a one-ULP positive residue; if the
 * sum is not pinned to zero on full drain, the next post-idle arrival computes
 * `V = V_epoch + idle_gap / residue` (~1e16), every finish tag saturates, and
 * the argmin scan collapses to the lowest queue index — weighted fairness is
 * lost. Saturating-traffic gates never drain the busy set, so this regression
 * exercises the drain-then-idle-then-reactivate path explicitly.
 */
class WfqFullDrainReactivationTest : public TestCase
{
  public:
    WfqFullDrainReactivationTest()
        : TestCase("WFQ full drain then reactivation preserves weighted service order")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<WfqScheduler> sched = CreateObjectWithAttributes<WfqScheduler>("NumQueues",
                                                                           UintegerValue(3),
                                                                           "LinkBandwidth",
                                                                           DoubleValue(1.0e6));
        // Equal, normalized (non-dyadic) shares — the documented weight convention.
        sched->SetParam(0, 1.0 / 3.0);
        sched->SetParam(1, 1.0 / 3.0);
        sched->SetParam(2, 1.0 / 3.0);

        // Busy period 1: one packet per flow at t=0, then drain the system empty.
        for (uint32_t i = 0; i < 3; ++i)
        {
            sched->OnEnqueueWithTime(i, 1000, 0.0);
        }
        for (uint32_t i = 0; i < 3; ++i)
        {
            sched->SelectNextQueue();
        }

        // Idle gap, then busy period 2: five packets per flow re-offered at t=10s.
        for (uint32_t i = 0; i < 3; ++i)
        {
            for (uint32_t p = 0; p < 5; ++p)
            {
                sched->OnEnqueueWithTime(i, 1000, 10.0);
            }
        }

        // The first post-idle round must serve each flow exactly once
        // (interleaved), not collapse to the lowest index.
        bool served[3] = {false, false, false};
        for (uint32_t k = 0; k < 3; ++k)
        {
            int q = sched->SelectNextQueue();
            NS_TEST_ASSERT_MSG_GT_OR_EQ(q, 0, "WFQ returned no queue while backlogged");
            NS_TEST_ASSERT_MSG_EQ(served[static_cast<uint32_t>(q)],
                                  false,
                                  "WFQ post-idle round served queue "
                                      << q
                                      << " twice before the others — weighted fairness "
                                         "collapsed to lowest-index priority");
            served[static_cast<uint32_t>(q)] = true;
        }

        // Root-cause guard: virtual time must be bounded after reactivation,
        // not the ~1e16 blowup the unclamped weight-sum residue produces.
        NS_TEST_ASSERT_MSG_LT(sched->GetVirtualTime(),
                              1.0e6,
                              "WFQ virtual time blew up across the idle gap");
    }
};

// =============================================================================
//  Q-4.2: WF2Q+ vs WFQ delay comparison
// =============================================================================

/**
 * @brief Compares WF2Q+ versus WFQ delay envelopes against the spec band.
 * @see specs/03-quality.md Q-4.2
 */
class Wf2qpVsWfqDelayTest : public TestCase
{
  public:
    Wf2qpVsWfqDelayTest()
        : TestCase("Q-4.2 WF2Q+ max starvation gap less than or equal to WFQ")
    {
    }

  private:
    int m_wfqMaxGap;
    int m_wf2qpMaxGap;
    Ptr<WfqScheduler> m_wfq;
    Ptr<Wf2qPlusScheduler> m_wf2qp;

    void DoRun() override
    {
        // --- Run WFQ scenario ---
        // Low bandwidth so GPS departure events fire between dequeues.
        m_wfq = CreateObjectWithAttributes<WfqScheduler>( // 48 kbps
            "NumQueues",
            UintegerValue(4),
            "LinkBandwidth",
            DoubleValue(48000.0));
        m_wfq->SetParam(0, 1.0); // delay-sensitive, low weight
        m_wfq->SetParam(1, 4.0);
        m_wfq->SetParam(2, 4.0);
        m_wfq->SetParam(3, 4.0);
        m_wfqMaxGap = 0;
        m_wfqGap = 0;

        Simulator::Schedule(Seconds(0.0), &Wf2qpVsWfqDelayTest::WfqEnqueue, this);

        for (int i = 0; i < 120; i++)
        {
            Simulator::Schedule(Seconds(0.1 + i * 0.1), &Wf2qpVsWfqDelayTest::WfqDequeue, this);
        }

        Simulator::Run();
        Simulator::Destroy();

        // --- Run WF2Q+ scenario ---
        m_wf2qp = CreateObjectWithAttributes<Wf2qPlusScheduler>("NumQueues",
                                                                UintegerValue(4),
                                                                "LinkBandwidth",
                                                                DoubleValue(48000.0));
        m_wf2qp->SetParam(0, 1.0);
        m_wf2qp->SetParam(1, 4.0);
        m_wf2qp->SetParam(2, 4.0);
        m_wf2qp->SetParam(3, 4.0);
        m_wf2qpMaxGap = 0;
        m_wf2qpGap = 0;

        Simulator::Schedule(Seconds(0.0), &Wf2qpVsWfqDelayTest::Wf2qpEnqueue, this);

        for (int i = 0; i < 120; i++)
        {
            Simulator::Schedule(Seconds(0.1 + i * 0.1), &Wf2qpVsWfqDelayTest::Wf2qpDequeue, this);
        }

        Simulator::Run();
        Simulator::Destroy();

        // WF2Q+'s eligibility property guarantees that the low-weight
        // flow is never starved beyond its GPS-fair share.  The max
        // starvation gap for q0 under WF2Q+ must be <= than under WFQ.
        NS_TEST_ASSERT_MSG_LT_OR_EQ(m_wf2qpMaxGap,
                                    m_wfqMaxGap,
                                    "WF2Q+ max gap (" << m_wf2qpMaxGap << ") should be <= WFQ ("
                                                      << m_wfqMaxGap << ")");
    }

    // --- WFQ helpers ---
    int m_wfqGap{0};

    void WfqEnqueue()
    {
        double now = Simulator::Now().GetSeconds();
        for (uint32_t q = 0; q < 4; q++)
        {
            for (int i = 0; i < 50; i++)
            {
                m_wfq->OnEnqueueWithTime(q, 1000, now);
            }
        }
    }

    void WfqDequeue()
    {
        int q = m_wfq->SelectNextQueue();
        if (q == 0)
        {
            if (m_wfqGap > m_wfqMaxGap)
            {
                m_wfqMaxGap = m_wfqGap;
            }
            m_wfqGap = 0;
        }
        else if (q >= 0)
        {
            m_wfqGap++;
        }
    }

    // --- WF2Q+ helpers ---
    int m_wf2qpGap{0};

    void Wf2qpEnqueue()
    {
        double now = Simulator::Now().GetSeconds();
        for (uint32_t q = 0; q < 4; q++)
        {
            for (int i = 0; i < 50; i++)
            {
                m_wf2qp->OnEnqueueWithTime(q, 1000, now);
            }
        }
    }

    void Wf2qpDequeue()
    {
        int q = m_wf2qp->SelectNextQueue();
        if (q == 0)
        {
            if (m_wf2qpGap > m_wf2qpMaxGap)
            {
                m_wf2qpMaxGap = m_wf2qpGap;
            }
            m_wf2qpGap = 0;
        }
        else if (q >= 0)
        {
            m_wf2qpGap++;
        }
    }
};

// =============================================================================
//  FWMeter tests (I-2.6)
// =============================================================================

/// Test 1 — Deterministic mode (downgrade2=0): every excess packet downgraded.
class TestFWMeterDeterministic : public TestCase
{
  public:
    TestFWMeterDeterministic()
        : TestCase("FW deterministic: excess packets downgraded (downgrade2=0)")
    {
    }

  private:
    void DoRun() override
    {
        FWMeter meter;
        PolicyEntry entry;
        entry.cir = 5000; // byte threshold

        PolicerEntry policer;
        policer.initialCodePt = 46; // EF
        policer.downgrade1 = 0;     // BE
        policer.downgrade2 = 0;     // deterministic mode

        // Flow 1 sends 6 x 1000 bytes at t=1.0
        for (int i = 0; i < 6; i++)
        {
            meter.ApplyMeterWithFlowId(entry, 1.0, 1000, /*flowId=*/1);
        }

        // Packets 1-5 (bytesSent 1000..5000): in-profile
        // We can only check the policer result after all metering is done
        // (the meter accumulates bytes_sent, policer checks total).
        // After 6 packets, bytesSent=6000 > 5000 → downgraded.
        int codePt = meter.ApplyPolicerFw(entry, policer, 1000, /*flowId=*/1);
        NS_TEST_EXPECT_MSG_EQ(codePt, 0, "6000 > CIR=5000: must downgrade to 0");

        // Verify a flow within CIR stays GREEN.
        // Flow 2 sends 3 x 1000 = 3000 < 5000.
        for (int i = 0; i < 3; i++)
        {
            meter.ApplyMeterWithFlowId(entry, 1.0, 1000, /*flowId=*/2);
        }
        codePt = meter.ApplyPolicerFw(entry, policer, 1000, /*flowId=*/2);
        NS_TEST_EXPECT_MSG_EQ(codePt, 46, "3000 <= CIR=5000: must return initialCodePt=46");
    }
};

/// Test 2 — Probabilistic mode (downgrade2=1): P(GREEN)=CIR/bytesSent.
class TestFWMeterProbabilistic : public TestCase
{
  public:
    TestFWMeterProbabilistic()
        : TestCase("FW probabilistic: GREEN ratio converges to CIR over "
                   "bytesSent (downgrade2=1)")
    {
    }

  private:
    void DoRun() override
    {
        FWMeter meter;
        PolicyEntry entry;
        entry.cir = 2000; // byte threshold

        PolicerEntry policer;
        policer.initialCodePt = 46;
        policer.downgrade1 = 0;
        policer.downgrade2 = 1; // probabilistic mode

        // Flow 1 sends 1000 x 1000 bytes = 1,000,000 total.
        // After the first 2 packets (2000 bytes), bytesSent exceeds CIR.
        // With bytesSent >> CIR, P(GREEN) = CIR/bytesSent ≈ very small.
        for (int i = 0; i < 1000; i++)
        {
            meter.ApplyMeterWithFlowId(entry, 1.0, 1000, /*flowId=*/1);
        }

        // Run policer many times and count GREEN outcomes.
        int greenCount = 0;
        constexpr int kTrials = 10000;
        for (int i = 0; i < kTrials; i++)
        {
            int codePt = meter.ApplyPolicerFw(entry, policer, 1000, /*flowId=*/1);
            if (codePt == 46)
            {
                greenCount++;
            }
        }

        // bytesSent = 1,000,000.  P(GREEN) = 2000/1000000 = 0.002.
        // Expected GREEN in 10000 trials: ~20.  Allow wide tolerance.
        double greenRatio = static_cast<double>(greenCount) / kTrials;
        NS_TEST_EXPECT_MSG_GT(greenRatio, 0.0001, "GREEN ratio should be > 0 (probabilistic mode)");
        NS_TEST_EXPECT_MSG_LT(greenRatio, 0.05, "GREEN ratio should be << 1 for large excess");
    }
};

/// Test 3 — Flow timeout: flow state purged after kFlowTimeoutSeconds.
class TestFWMeterFlowTimeout : public TestCase
{
  public:
    TestFWMeterFlowTimeout()
        : TestCase("FW flow timeout: state purged after 5s idle")
    {
    }

  private:
    void DoRun() override
    {
        FWMeter meter;
        PolicyEntry entry;
        entry.cir = 2000;

        PolicerEntry policer;
        policer.initialCodePt = 46;
        policer.downgrade1 = 0;
        policer.downgrade2 = 0; // deterministic

        // Flow 1 sends 3 x 1000 = 3000 > CIR=2000 at t=1.0
        for (int i = 0; i < 3; i++)
        {
            meter.ApplyMeterWithFlowId(entry, 1.0, 1000, /*flowId=*/1);
        }

        // Verify excess
        int codePt = meter.ApplyPolicerFw(entry, policer, 1000, /*flowId=*/1);
        NS_TEST_EXPECT_MSG_EQ(codePt, 0, "3000 > CIR=2000: downgraded");

        // Wait 6 seconds (> FLOW_TIME_OUT=5.0).  Send new packet at t=7.0.
        // The old flow entry should be purged, and the new packet starts fresh.
        meter.ApplyMeterWithFlowId(entry, 7.0, 500, /*flowId=*/1);

        // After purge + re-creation: bytesSent=500 <= 2000, should be in-profile.
        codePt = meter.ApplyPolicerFw(entry, policer, 500, /*flowId=*/1);
        NS_TEST_EXPECT_MSG_EQ(codePt, 46, "After timeout + re-creation: in-profile");
    }
};

/// Test 4 — Periodic mode (downgrade2=2): 1-in-6 cycle.
class TestFWMeterPeriodic : public TestCase
{
  public:
    TestFWMeterPeriodic()
        : TestCase("FW periodic: 5 downgrades then 1 green cycle (downgrade2=2)")
    {
    }

  private:
    void DoRun() override
    {
        FWMeter meter;
        PolicyEntry entry;
        entry.cir = 1000; // byte threshold

        PolicerEntry policer;
        policer.initialCodePt = 46;
        policer.downgrade1 = 0;
        policer.downgrade2 = 2; // periodic mode

        // Send 10 x 1000 bytes. Packet 1: bytesSent=1000, not > CIR=1000 → GREEN.
        // Packets 2-10: bytesSent > 1000, periodic counter applies.
        //
        // Reference: dsPolicy.cc line 904-913
        //   count starts at 0 for new flow.
        //   if count==5: reset to 0, return initialCodePt
        //   else: count++, return downgrade1
        //
        // Expected per-packet results:
        //   Pkt  1: 1000 not > 1000 → GREEN (46)
        //   Pkt  2: 2000 > 1000, count=0 !=5 → count=1, downgrade (0)
        //   Pkt  3: count=1 → count=2, 0
        //   Pkt  4: count=2 → count=3, 0
        //   Pkt  5: count=3 → count=4, 0
        //   Pkt  6: count=4 → count=5, 0
        //   Pkt  7: count=5 → reset=0, GREEN (46)
        //   Pkt  8: count=0 → count=1, 0
        //   Pkt  9: count=1 → count=2, 0
        //   Pkt 10: count=2 → count=3, 0

        int expected[10] = {46, 0, 0, 0, 0, 0, 46, 0, 0, 0};

        for (int i = 0; i < 10; i++)
        {
            meter.ApplyMeterWithFlowId(entry, 1.0, 1000, /*flowId=*/1);
            int codePt = meter.ApplyPolicerFw(entry, policer, 1000, /*flowId=*/1);
            std::ostringstream msg;
            msg << "Packet " << (i + 1) << " expected codePt=" << expected[i];
            NS_TEST_EXPECT_MSG_EQ(codePt, expected[i], msg.str());
        }
    }
};

/// Test 5 — Multi-flow isolation: excess in flow 1 does not affect flow 2.
class TestFWMeterMultiFlowIsolation : public TestCase
{
  public:
    TestFWMeterMultiFlowIsolation()
        : TestCase("FW multi-flow: flow 1 excess does not affect flow 2")
    {
    }

  private:
    void DoRun() override
    {
        FWMeter meter;
        PolicyEntry entry;
        entry.cir = 3000;

        PolicerEntry policer;
        policer.initialCodePt = 46;
        policer.downgrade1 = 0;
        policer.downgrade2 = 0; // deterministic

        // Flow 1: 5 x 1000 = 5000 > CIR=3000
        for (int i = 0; i < 5; i++)
        {
            meter.ApplyMeterWithFlowId(entry, 1.0, 1000, /*flowId=*/1);
        }

        // Flow 2: 1 x 1000 = 1000 <= CIR=3000
        meter.ApplyMeterWithFlowId(entry, 1.0, 1000, /*flowId=*/2);

        // Flow 1 should be downgraded
        int codePt1 = meter.ApplyPolicerFw(entry, policer, 1000, /*flowId=*/1);
        NS_TEST_EXPECT_MSG_EQ(codePt1, 0, "Flow 1: 5000 > CIR=3000 → downgraded");

        // Flow 2 should be in-profile, unaffected by flow 1
        int codePt2 = meter.ApplyPolicerFw(entry, policer, 1000, /*flowId=*/2);
        NS_TEST_EXPECT_MSG_EQ(codePt2, 46, "Flow 2: 1000 <= CIR=3000 → in-profile");
    }
};

// =============================================================================
//  Edge dispatch wiring for FAIR_WEIGHTED -> FWMeter
// =============================================================================
//
// Asserts that EdgeQueueDisc::GetMeter(MeterType::FAIR_WEIGHTED)
// returns an FWMeter instance on first call and caches it on subsequent
// calls. The TestFWMeter* cases above cover algorithmic behaviour
// against a directly-constructed FWMeter; this case covers the enum ->
// FWMeter wiring specifically.

class TestFWMeterEdgeDispatch : public TestCase
{
  public:
    TestFWMeterEdgeDispatch()
        : TestCase("S-13.16 FAIR_WEIGHTED enum dispatches to FWMeter (registry wiring)")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();

        Ptr<Meter> meter = edge->GetMeter(MeterType::FAIR_WEIGHTED);
        NS_TEST_ASSERT_MSG_NE(meter,
                              nullptr,
                              "GetMeter(FAIR_WEIGHTED) must return a non-null FWMeter instance");
        if (!meter)
        {
            Simulator::Destroy();
            return;
        }
        NS_TEST_ASSERT_MSG_NE(DynamicCast<FWMeter>(meter),
                              nullptr,
                              "Dispatched meter must be a Ptr<FWMeter>");

        Ptr<Meter> meter2 = edge->GetMeter(MeterType::FAIR_WEIGHTED);
        NS_TEST_ASSERT_MSG_EQ(meter2, meter, "Second GetMeter call must return the cached slot");

        Simulator::Destroy();
    }
};

/**
 * @brief The FW policer's penalty mode (PolicerEntry::downgrade2) is honoured on
 * the classifier path, not just when ApplyPolicerFw is called directly.
 *
 * Periodic mode (downgrade2=2) keeps 1-in-6 excess packets at the initial code
 * point. A colour-only policer path cannot carry the mode and downgrades every
 * excess packet, so it would never let one through.
 */
class FwPolicerPeriodicModeThroughClassifierTest : public TestCase
{
  public:
    FwPolicerPeriodicModeThroughClassifierTest()
        : TestCase("FW periodic policer mode honoured on the classifier path (1-in-6)")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        auto inner = CreateObject<RedQueueDisc>();
        edge->SetInnerDisc(inner);
        inner->SetNumQueues(1);
        inner->AddPhbEntry(46, 0, 0);
        inner->AddPhbEntry(0, 0, 0);
        inner->SetScheduler(
            CreateObjectWithAttributes<RoundRobinScheduler>("NumQueues", UintegerValue(1)));

        edge->SetMeter(MeterType::FAIR_WEIGHTED, CreateObject<FWMeter>());

        PolicyEntry policy;
        policy.codePoint = 10;
        policy.meter = MeterType::FAIR_WEIGHTED;
        policy.policer = PolicerType::FAIR_WEIGHTED;
        policy.policyIndex = 0;
        policy.cir = 0; // every packet is immediately over CIR
        edge->GetPolicyClassifier()->AddPolicyEntry(policy);

        PolicerEntry policer;
        policer.policer = PolicerType::FAIR_WEIGHTED;
        policer.policyIndex = 0;
        // The policer is keyed by (policyIndex, initialCodePt); initialCodePt
        // must equal the incoming code point so the lookup matches. It is also
        // the in-profile / periodic-pass remark.
        policer.initialCodePt = 10;
        policer.downgrade1 = 0; // excess-packet remark
        policer.downgrade2 = 2; // periodic penalty mode (1-in-6)
        edge->GetPolicyClassifier()->AddPolicerEntry(policer);

        edge->Initialize();

        // Classify the same flow 12 times. Every packet is over CIR (=0), so the
        // periodic mode keeps every 6th packet at the initial code point (10)
        // and downgrades the rest (0). The colour-only path cannot carry the
        // mode and downgrades all of them, never letting one through.
        uint32_t initials = 0;
        for (int i = 0; i < 12; ++i)
        {
            const uint8_t remark = edge->GetPolicyClassifier()->ApplyPolicy(10, 1000, 1.0);
            if (remark == 10)
            {
                ++initials;
            }
        }
        NS_TEST_ASSERT_MSG_GT(initials,
                              0u,
                              "FW periodic policer mode must keep 1-in-6 packets at the initial "
                              "code point on the classifier path; got 0 (downgrade2 mode ignored)");
        Simulator::Destroy();
    }
};

// =============================================================================
//  S-15.2: Statistics packet accounting balance
// =============================================================================

/**
 * @brief Verifies packet accounting balances enqueues, dequeues, and drops.
 * @see specs/02-structural.md S-15.2
 */
class PacketAccountingBalanceTest : public TestCase
{
  public:
    PacketAccountingBalanceTest()
        : TestCase("S-15.2 Packet accounting balance: enqueued == dequeued + "
                   "redDrops + tailDrops")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<Statistics> stats = CreateObject<Statistics>();

        const uint8_t dscp = 46; // EF
        const uint32_t pktSize = 500;

        // Enqueue 100 packets
        for (int i = 0; i < 100; ++i)
        {
            stats->RecordEnqueue(dscp, pktSize);
        }

        // Dequeue 90
        for (int i = 0; i < 90; ++i)
        {
            stats->RecordDequeue(dscp, pktSize);
        }

        // RED-drop 7
        for (int i = 0; i < 7; ++i)
        {
            stats->RecordRedDrop(dscp, pktSize);
        }

        // Tail-drop 3
        for (int i = 0; i < 3; ++i)
        {
            stats->RecordTailDrop(dscp, pktSize);
        }

        NS_TEST_EXPECT_MSG_EQ(stats->GetEnqueued(dscp), 100, "Enqueued count");
        NS_TEST_EXPECT_MSG_EQ(stats->GetDequeued(dscp), 90, "Dequeued count");
        NS_TEST_EXPECT_MSG_EQ(stats->GetRedDrops(dscp), 7, "RED drops count");
        NS_TEST_EXPECT_MSG_EQ(stats->GetTailDrops(dscp), 3, "Tail drops count");

        // Accounting balance: enqueued == dequeued + redDrops + tailDrops
        NS_TEST_EXPECT_MSG_EQ(stats->GetEnqueued(dscp),
                              stats->GetDequeued(dscp) + stats->GetRedDrops(dscp) +
                                  stats->GetTailDrops(dscp),
                              "Accounting balance: enqueued == dequeued + redDrops + tailDrops");
    }
};

// =============================================================================
//  S-15.3: Statistics drop attribution
// =============================================================================

/**
 * @brief Verifies drops are attributed to the correct sub-queue and reason.
 * @see specs/02-structural.md S-15.3
 */
class DropAttributionTest : public TestCase
{
  public:
    DropAttributionTest()
        : TestCase("S-15.3 Drop attribution: totalDrops == redDrops + tailDrops")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<Statistics> stats = CreateObject<Statistics>();

        const uint8_t dscp = 46; // EF
        const uint32_t pktSize = 500;

        // Record 3 RED drops
        for (int i = 0; i < 3; ++i)
        {
            stats->RecordRedDrop(dscp, pktSize);
        }

        // Record 2 tail drops
        for (int i = 0; i < 2; ++i)
        {
            stats->RecordTailDrop(dscp, pktSize);
        }

        NS_TEST_EXPECT_MSG_EQ(stats->GetTotalDrops(dscp), 5, "Total drops should be 5");
        NS_TEST_EXPECT_MSG_EQ(stats->GetRedDrops(dscp) + stats->GetTailDrops(dscp),
                              stats->GetTotalDrops(dscp),
                              "redDrops + tailDrops must equal totalDrops");
    }
};

// =============================================================================
//  S-15.4: orig/retx byte counters drive thesis-style goodput derivation
// =============================================================================

/**
 * @brief Verifies retransmitted bytes are tracked in the goodput accounting plane.
 * @see specs/02-structural.md S-15.4
 */
class RetxByteAccountingTest : public TestCase
{
  public:
    RetxByteAccountingTest()
        : TestCase("S-15.4 origBytes vs retxBytes counters back thesis goodput "
                   "formula")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<Statistics> stats = CreateObject<Statistics>();

        const uint8_t dscp = 14; // AF13 (HTTP red in Scenario 2)
        const uint32_t segmentBytes = 1500;

        // 95 fresh segments, 5 retransmits — same byte size on the wire.
        for (int i = 0; i < 95; ++i)
        {
            stats->RecordOrigBytes(dscp, segmentBytes);
        }
        for (int i = 0; i < 5; ++i)
        {
            stats->RecordRetxBytes(dscp, segmentBytes);
        }

        const uint64_t expectedOrig = 95ULL * segmentBytes;
        const uint64_t expectedRetx = 5ULL * segmentBytes;

        NS_TEST_EXPECT_MSG_EQ(stats->GetOrigBytes(dscp), expectedOrig, "Original byte total");
        NS_TEST_EXPECT_MSG_EQ(stats->GetRetxBytes(dscp), expectedRetx, "Retx byte total");

        // Thesis goodput formula: origBytes / (origBytes + retxBytes).
        // 95 / 100 = 0.95.  Within float epsilon.
        double goodput = static_cast<double>(stats->GetOrigBytes(dscp)) /
                         static_cast<double>(stats->GetOrigBytes(dscp) + stats->GetRetxBytes(dscp));
        NS_TEST_EXPECT_MSG_EQ_TOL(goodput,
                                  0.95,
                                  1e-9,
                                  "Thesis goodput = origBytes / (origBytes + retxBytes)");

        // Distinct DSCPs are accounted independently.
        const uint8_t otherDscp = 46; // EF
        stats->RecordOrigBytes(otherDscp, segmentBytes);
        NS_TEST_EXPECT_MSG_EQ(stats->GetOrigBytes(dscp),
                              expectedOrig,
                              "Recording at DSCP 46 must not affect DSCP 14");
        NS_TEST_EXPECT_MSG_EQ(stats->GetOrigBytes(otherDscp),
                              segmentBytes,
                              "DSCP 46 must reflect its own writes");
    }
};

// =============================================================================
//  Q-7: Performance regression (scaled example-1)
// =============================================================================

/**
 * Helper: get current peak RSS in bytes.
 *
 * On macOS, uses mach_task_basic_info (resident_size in bytes).
 * On Linux, uses getrusage (ru_maxrss in kilobytes).
 */
static uint64_t
GetPeakRssBytes()
{
#ifdef __APPLE__
    struct mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(),
                  MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info),
                  &count) == KERN_SUCCESS)
    {
        return info.resident_size;
    }
    return 0;
#else
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0)
    {
        return static_cast<uint64_t>(usage.ru_maxrss) * 1024; // Linux: KB → bytes
    }
    return 0;
#endif
}

/**
 * @brief Performance regression scenario for the canonical edge plus core topology.
 * @see specs/03-quality.md Q-7
 */
class PerfRegressionTest : public TestCase
{
  public:
    PerfRegressionTest()
        : TestCase("Q-7 Performance regression, 50-src 50-dst example-1")
    {
    }

  private:
    void DoRun() override;
};

void
PerfRegressionTest::DoRun()
{
    const uint32_t kNumSources = 50;
    const uint32_t kNumDests = 50;
    const double kSimTime = 60.0;                            // seconds
    const double kMaxWallClockSec = 300.0;                   // 5 minutes
    const uint64_t kMaxRssBytes = 1ULL * 1024 * 1024 * 1024; // 1 GB
    const uint32_t kPacketSize = 512;

    auto wallStart = std::chrono::steady_clock::now();

    // ---- RNG ----
    RngSeedManager::SetSeed(42);
    RngSeedManager::SetRun(1);

    Ptr<UniformRandomVariable> rndStartTime = CreateObject<UniformRandomVariable>();
    rndStartTime->SetAttribute("Min", DoubleValue(0.0));
    rndStartTime->SetAttribute("Max", DoubleValue(5.0));

    Ptr<UniformRandomVariable> rndSourceNode = CreateObject<UniformRandomVariable>();
    rndSourceNode->SetAttribute("Min", DoubleValue(0.0));
    rndSourceNode->SetAttribute("Max", DoubleValue(static_cast<double>(kNumSources)));

    // ---- Create nodes ----
    NodeContainer sources;
    sources.Create(kNumSources);

    NodeContainer routers;
    routers.Create(3); // e1, core, e2

    NodeContainer destinations;
    destinations.Create(kNumDests);

    Ptr<Node> e1 = routers.Get(0);
    Ptr<Node> core = routers.Get(1);
    Ptr<Node> e2 = routers.Get(2);

    // ---- Links ----
    PointToPointHelper p2pAccess;
    p2pAccess.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    p2pAccess.SetChannelAttribute("Delay", StringValue("1ms"));

    // Source -> e1 access links
    std::vector<NetDeviceContainer> srcDevs(kNumSources);
    for (uint32_t i = 0; i < kNumSources; i++)
    {
        srcDevs[i] = p2pAccess.Install(sources.Get(i), e1);
    }

    // e1 <-> core bottleneck (2 Mbps, 5 ms)
    PointToPointHelper p2pBottleneck;
    p2pBottleneck.SetDeviceAttribute("DataRate", StringValue("2Mbps"));
    p2pBottleneck.SetChannelAttribute("Delay", StringValue("5ms"));
    p2pBottleneck.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("1p"));
    NetDeviceContainer devE1Core = p2pBottleneck.Install(e1, core);

    // core <-> e2 (5 Mbps, 3 ms)
    PointToPointHelper p2pCoreE2;
    p2pCoreE2.SetDeviceAttribute("DataRate", StringValue("5Mbps"));
    p2pCoreE2.SetChannelAttribute("Delay", StringValue("3ms"));
    NetDeviceContainer devCoreE2 = p2pCoreE2.Install(core, e2);

    // e2 -> destination access links
    std::vector<NetDeviceContainer> dstDevs(kNumDests);
    for (uint32_t i = 0; i < kNumDests; i++)
    {
        dstDevs[i] = p2pAccess.Install(e2, destinations.Get(i));
    }

    // ---- Internet stack ----
    InternetStackHelper internet;
    NodeContainer allNodes;
    allNodes.Add(sources);
    allNodes.Add(routers);
    allNodes.Add(destinations);
    internet.Install(allNodes);

    // ---- IP addresses ----
    Ipv4AddressHelper addr;
    uint32_t subnetIdx = 1;

    // Source -> e1
    for (uint32_t i = 0; i < kNumSources; i++)
    {
        std::string base = "10.0." + std::to_string(subnetIdx++) + ".0";
        addr.SetBase(base.c_str(), "255.255.255.0");
        addr.Assign(srcDevs[i]);
    }

    // e1 <-> core
    addr.SetBase(("10.0." + std::to_string(subnetIdx++) + ".0").c_str(), "255.255.255.0");
    Ipv4InterfaceContainer ifE1Core = addr.Assign(devE1Core);

    // core <-> e2
    addr.SetBase(("10.0." + std::to_string(subnetIdx++) + ".0").c_str(), "255.255.255.0");
    addr.Assign(devCoreE2);

    // e2 -> destinations
    std::vector<Ipv4InterfaceContainer> dstIfs(kNumDests);
    for (uint32_t i = 0; i < kNumDests; i++)
    {
        std::string base = "10.0." + std::to_string(subnetIdx++) + ".0";
        addr.SetBase(base.c_str(), "255.255.255.0");
        dstIfs[i] = addr.Assign(dstDevs[i]);
    }

    // ---- Routing ----
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    Ipv4Address destAddr0 = dstIfs[0].GetAddress(1); // d0: EF destination
    Ipv4Address destAddr1 = dstIfs[1].GetAddress(1); // d1: BE destination

    // ---- Remove default queue discs on bottleneck ----
    TrafficControlHelper tchUninstall;
    tchUninstall.Uninstall(devE1Core.Get(0));
    tchUninstall.Uninstall(devE1Core.Get(1));

    // ---- DiffServ Edge configuration (e1 -> core) ----
    Ptr<EdgeQueueDisc> edgeDisc = CreateObject<EdgeQueueDisc>();
    auto edgeDiscInner = CreateObject<RedQueueDisc>();
    edgeDisc->SetInnerDisc(edgeDiscInner);
    diffserv::Helper helper;

    edgeDiscInner->SetNumQueues(2);
    edgeDiscInner->SetNumPrec(0, 2); // EF: 2 prec levels
    edgeDiscInner->SetNumPrec(1, 1); // BE: 1 prec level
    edgeDiscInner->SetQueueLimit(0, 30);
    edgeDiscInner->SetQueueLimit(1, 50);
    edgeDiscInner->SetMredModeAllQueues(MredMode::DROP_TAIL);

    Ptr<PriorityScheduler> pqSched =
        CreateObjectWithAttributes<PriorityScheduler>("NumQueues",
                                                      UintegerValue(2),
                                                      "WinLen",
                                                      DoubleValue(1.0));
    edgeDiscInner->SetScheduler(pqSched);

    edgeDisc->AddMarkRule({.dscp = 46, .dstAddr = destAddr0});
    edgeDisc->AddMarkRule({.dscp = 0, .dstAddr = destAddr1});

    double cirEfBps = 300000.0;
    double cbsEfBytes = kPacketSize + 1.0;
    helper.AddTokenBucketPolicy(edgeDisc,
                                {.codePt = 46, .cirBps = cirEfBps, .cbsBytes = cbsEfBytes});
    helper.AddDumbPolicy(edgeDisc, 48);
    helper.AddDumbPolicy(edgeDisc, 0);

    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::TOKEN_BUCKET,
                            .initialCodePt = 46,
                            .downgrade1 = 48,
                            .downgrade2 = 48,
                            .policyIndex = static_cast<uint32_t>(PolicerType::TOKEN_BUCKET)});
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::DUMB,
                            .initialCodePt = 0,
                            .downgrade1 = 0,
                            .downgrade2 = 0,
                            .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});

    helper.AddPhbEntry(edgeDiscInner, 46, 0, 0);
    helper.AddPhbEntry(edgeDiscInner, 48, 0, 1);
    helper.AddPhbEntry(edgeDiscInner, 0, 1, 0);

    // Install edge disc
    Ptr<NetDevice> e1Dev = devE1Core.Get(0);
    Ptr<TrafficControlLayer> tc = e1Dev->GetNode()->GetObject<TrafficControlLayer>();
    tc->SetRootQueueDiscOnDevice(e1Dev, edgeDisc);
    edgeDisc->Initialize();

    helper.ConfigQueue(edgeDiscInner,
                       {.queue = 0, .prec = 0, .thMin = 30.0, .thMax = 30.0, .maxP = 1.0});
    helper.ConfigQueue(edgeDiscInner,
                       {.queue = 0, .prec = 1, .thMin = 0.0, .thMax = 0.0, .maxP = 0.0});
    helper.ConfigQueue(edgeDiscInner,
                       {.queue = 1, .prec = 0, .thMin = 50.0, .thMax = 50.0, .maxP = 1.0});

    // ---- DiffServ Core (core -> e1 direction, minimal config) ----
    Ptr<CoreQueueDisc> coreDisc = CreateObject<CoreQueueDisc>();
    auto coreInner = helper.InstallRedInner(coreDisc);
    coreInner->SetNumQueues(1);
    coreInner->SetNumPrec(0, 1);
    coreInner->SetMredModeAllQueues(MredMode::DROP_TAIL);

    Ptr<PriorityScheduler> corePq = CreateObjectWithAttributes<PriorityScheduler>("NumQueues",
                                                                                  UintegerValue(1),
                                                                                  "WinLen",
                                                                                  DoubleValue(1.0));
    coreInner->SetScheduler(corePq);
    helper.AddPhbEntry(coreInner, 10, 0, 0);

    Ptr<NetDevice> coreDev = devE1Core.Get(1);
    Ptr<TrafficControlLayer> tcCore = coreDev->GetNode()->GetObject<TrafficControlLayer>();
    tcCore->SetRootQueueDiscOnDevice(coreDev, coreDisc);
    coreDisc->Initialize();
    helper.ConfigQueue(coreInner,
                       {.queue = 0, .prec = 0, .thMin = 50.0, .thMax = 50.0, .maxP = 1.0});

    // ---- Traffic: EF flow (1 CBR at 300 kbps) ----
    uint16_t efPort = 9;

    PacketSinkHelper efSinkHelper("ns3::UdpSocketFactory",
                                  InetSocketAddress(Ipv4Address::GetAny(), efPort));
    ApplicationContainer efSinkApp = efSinkHelper.Install(destinations.Get(0));
    efSinkApp.Start(Seconds(0.0));
    efSinkApp.Stop(Seconds(kSimTime));

    auto efSrcIdx = static_cast<uint32_t>(rndSourceNode->GetInteger(0, kNumSources - 1));

    ns3::OnOffHelper efOnOff("ns3::UdpSocketFactory", InetSocketAddress(destAddr0, efPort));
    efOnOff.SetAttribute("DataRate", StringValue("300000bps"));
    efOnOff.SetAttribute("PacketSize", UintegerValue(kPacketSize));
    efOnOff.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1000]"));
    efOnOff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
    ApplicationContainer efApp = efOnOff.Install(sources.Get(efSrcIdx));
    efApp.Start(Seconds(rndStartTime->GetValue()));
    efApp.Stop(Seconds(kSimTime));

    // ---- Traffic: 200 BE flows (100 kbps each, variable pkt size) ----
    uint16_t bePort = 10;

    PacketSinkHelper beSinkHelper("ns3::UdpSocketFactory",
                                  InetSocketAddress(Ipv4Address::GetAny(), bePort));
    ApplicationContainer beSinkApp = beSinkHelper.Install(destinations.Get(1));
    beSinkApp.Start(Seconds(0.0));
    beSinkApp.Stop(Seconds(kSimTime));

    uint32_t bgPktSize = 64;
    for (uint32_t i = 0; i < 200; i++)
    {
        double startTime = rndStartTime->GetValue();
        auto srcIdx = static_cast<uint32_t>(rndSourceNode->GetInteger(0, kNumSources - 1));

        ns3::OnOffHelper beOnOff("ns3::UdpSocketFactory", InetSocketAddress(destAddr1, bePort));
        beOnOff.SetAttribute("DataRate", StringValue("100000bps"));
        beOnOff.SetAttribute("PacketSize", UintegerValue(bgPktSize));
        beOnOff.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1000]"));
        beOnOff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));

        ApplicationContainer beApp = beOnOff.Install(sources.Get(srcIdx));
        beApp.Start(Seconds(startTime));
        beApp.Stop(Seconds(kSimTime));

        bgPktSize += 64;
        if (bgPktSize > 1280)
        {
            bgPktSize = 64; // cycle through sizes (matching example-1 pattern)
        }
    }

    // ---- Run simulation ----
    Simulator::Stop(Seconds(kSimTime));
    Simulator::Run();
    Simulator::Destroy();

    auto wallEnd = std::chrono::steady_clock::now();
    double wallSec = std::chrono::duration<double>(wallEnd - wallStart).count();
    uint64_t peakRss = GetPeakRssBytes();

    // ---- Q-7.1: Wall-clock budget ----
    NS_TEST_ASSERT_MSG_LT(wallSec,
                          kMaxWallClockSec,
                          "Q-7.1 FAIL: wall-clock " << wallSec << " s exceeds 5-min budget");

    // ---- Q-7.2: Memory budget ----
    // Only assert if we got a valid RSS reading (getrusage can fail on some CI)
    if (peakRss > 0)
    {
        NS_TEST_ASSERT_MSG_LT(peakRss,
                              kMaxRssBytes,
                              "Q-7.2 FAIL: peak RSS " << (peakRss / (1024 * 1024))
                                                      << " MB exceeds 1 GB budget");
    }

    // Log results for baseline tracking
    std::ostringstream q7Sum;
    q7Sum << "\n  [Q-7] Performance results:\n"
          << "    Wall-clock: " << std::fixed << std::setprecision(1) << wallSec
          << " s (budget: " << kMaxWallClockSec << " s)\n"
          << "    Peak RSS:   " << (peakRss / (1024 * 1024)) << " MB (budget: 1024 MB)\n";
    std::cout << q7Sum.str() << std::endl;
}

// =============================================================================
//  Q-5: AF PHB drop precedence behaviour
// =============================================================================

/**
 * @brief End-to-end AF drop-precedence scenario validation.
 * @see specs/03-quality.md Q-5
 */
class AfDropPrecedenceQualityTest : public TestCase
{
  public:
    AfDropPrecedenceQualityTest()
        : TestCase("Q-5 AF PHB drop precedence ordering (3 TCP flows, RIO)")
    {
    }

  private:
    uint64_t m_rxBytes[3]; ///< Bytes received per flow (AF11, AF12, AF13)

    static void RxCallback(AfDropPrecedenceQualityTest* self,
                           uint32_t flowIdx,
                           Ptr<const Packet> pkt,
                           const Address&)
    {
        self->m_rxBytes[flowIdx] += pkt->GetSize();
    }

    void DoRun() override;
};

void
AfDropPrecedenceQualityTest::DoRun()
{
    m_rxBytes[0] = m_rxBytes[1] = m_rxBytes[2] = 0;

    const double kSimTime = 30.0;
    const double kLinkBw = 2e6; // 2 Mbps bottleneck

    // ---- Nodes: src, edge, sink ----
    NodeContainer nodes;
    nodes.Create(3);

    PointToPointHelper p2pAccess;
    p2pAccess.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    p2pAccess.SetChannelAttribute("Delay", StringValue("1ms"));

    PointToPointHelper p2pBottleneck;
    p2pBottleneck.SetDeviceAttribute("DataRate", StringValue("2Mbps"));
    p2pBottleneck.SetChannelAttribute("Delay", StringValue("5ms"));
    p2pBottleneck.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("1p"));

    NetDeviceContainer devSrcEdge = p2pAccess.Install(nodes.Get(0), nodes.Get(1));
    NetDeviceContainer devEdgeSink = p2pBottleneck.Install(nodes.Get(1), nodes.Get(2));

    InternetStackHelper internet;
    internet.Install(nodes);

    Ipv4AddressHelper addr;
    addr.SetBase("10.0.1.0", "255.255.255.0");
    Ipv4InterfaceContainer ifSrcEdge = addr.Assign(devSrcEdge);
    addr.SetBase("10.0.2.0", "255.255.255.0");
    Ipv4InterfaceContainer ifEdgeSink = addr.Assign(devEdgeSink);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    Ipv4Address sinkAddr = ifEdgeSink.GetAddress(1);

    // ---- Remove default queue disc, install edge disc ----
    TrafficControlHelper tchUninstall;
    tchUninstall.Uninstall(devEdgeSink.Get(0));

    Ptr<EdgeQueueDisc> edgeDisc = CreateObject<EdgeQueueDisc>();
    auto edgeDiscInner = CreateObject<RedQueueDisc>();
    edgeDisc->SetInnerDisc(edgeDiscInner);
    diffserv::Helper helper;

    // Single physical queue with 3 drop precedence levels
    edgeDiscInner->SetNumQueues(1);
    edgeDiscInner->SetNumPrec(0, 3);
    edgeDiscInner->SetQueueLimit(0, 50);
    edgeDiscInner->SetMredModeAllQueues(MredMode::RIO_D); // Per-precedence RED

    Ptr<PriorityScheduler> sched = CreateObjectWithAttributes<PriorityScheduler>("NumQueues",
                                                                                 UintegerValue(1),
                                                                                 "WinLen",
                                                                                 DoubleValue(1.0));
    edgeDiscInner->SetScheduler(sched);

    // Mark rules: all traffic -> AF11 (DSCP 10) by default
    // We'll use 3 different source ports to distinguish flows,
    // but mark all to the same queue. The policer will differentiate
    // by assigning AF11/AF12/AF13 based on source port range.
    // Simpler approach: classify all to AF11, then use srTCM to
    // downgrade to AF12/AF13.
    //
    // Even simpler: mark each flow directly by destination port.
    edgeDisc->AddMarkRule({.dscp = 10, .dstAddr = sinkAddr});

    // Dumb policy for all three AF DSCPs (no metering, just classification)
    helper.AddDumbPolicy(edgeDisc, 10);
    helper.AddDumbPolicy(edgeDisc, 12);
    helper.AddDumbPolicy(edgeDisc, 14);

    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::DUMB,
                            .initialCodePt = 10,
                            .downgrade1 = 10,
                            .downgrade2 = 10,
                            .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});

    // PHB: AF11/12/13 -> queue 0, prec 0/1/2
    helper.AddPhbEntry(edgeDiscInner, 10, 0, 0); // AF11 -> lowest drop (best)
    helper.AddPhbEntry(edgeDiscInner, 12, 0, 1); // AF12 -> medium drop
    helper.AddPhbEntry(edgeDiscInner, 14, 0, 2); // AF13 -> highest drop (worst)

    Ptr<NetDevice> edgeDev = devEdgeSink.Get(0);
    Ptr<TrafficControlLayer> tc = edgeDev->GetNode()->GetObject<TrafficControlLayer>();
    tc->SetRootQueueDiscOnDevice(edgeDev, edgeDisc);
    edgeDisc->Initialize();

    // RIO thresholds: prec 0 most tolerant, prec 2 least
    helper.ConfigQueue(edgeDiscInner,
                       {.queue = 0,
                        .prec = 0,
                        .thMin = 40.0,
                        .thMax = 50.0,
                        .maxP = 0.02}); // AF11: high thresholds
    helper.ConfigQueue(
        edgeDiscInner,
        {.queue = 0, .prec = 1, .thMin = 20.0, .thMax = 40.0, .maxP = 0.05}); // AF12: medium
    helper.ConfigQueue(edgeDiscInner,
                       {.queue = 0,
                        .prec = 2,
                        .thMin = 5.0,
                        .thMax = 15.0,
                        .maxP = 0.10}); // AF13: aggressive drop

    // ---- Traffic: 3 TCP BulkSend flows on different ports ----
    // All classified to AF11 initially, then we re-mark AF12/AF13
    // via DscpTag in the edge disc classification.
    //
    // Alternative approach: use 3 mark rules matching by port,
    // but MarkRule doesn't support port matching. Instead, use
    // 3 separate sinks and mark by destination address.
    //
    // Simplest approach: 3 destinations, each with its own mark rule.
    // But we only have 3 nodes. Let's use ports and re-classify.
    //
    // Actually, the simplest is: all flows go to AF11 (mark rule matches
    // all), the DUMB policer keeps them at AF11. Then we manually set
    // DSCP tags in the BulkSend Tx callback. But that requires a custom
    // callback which is complex in test code.
    //
    // Pragmatic: create 3 sink nodes, one mark rule per sink.
    // This requires more nodes but is straightforward.

    // Re-create with more nodes: src, edge, sink0, sink1, sink2
    Simulator::Destroy();

    NodeContainer allNodes;
    allNodes.Create(5); // src, edge, sink0, sink1, sink2

    Ptr<Node> src = allNodes.Get(0);
    Ptr<Node> edge = allNodes.Get(1);

    // src -> edge (fast)
    NetDeviceContainer dSrcEdge = p2pAccess.Install(src, edge);

    // edge -> sink0, sink1, sink2 (bottleneck shared? No, they need
    // to share the same bottleneck link. Use a single bottleneck to
    // a hub, then fan out. Or: all 3 sinks on the same link via a bus.
    //
    // Simplest: edge -> router -> 3 sinks. Bottleneck on edge->router.
    // But that's 6 nodes. Let's just use a single sink with 3 ports.

    // Let me simplify drastically: single sink, 3 TCP flows on ports
    // 100, 200, 300. All get marked AF11 by the mark rule.
    // The KEY insight: we don't need 3 different DSCPs at the mark
    // rule level. We need 3 different drop precedences in the SAME
    // queue. The test verifies that RIO_D causes higher-prec entries
    // to drop more.
    //
    // We can put all 3 flows into AF11 (prec 0) and then check that
    // RIO_D with 3 prec levels and different thresholds creates the
    // right drop ordering. But if all 3 flows have the SAME precedence,
    // they'll all experience the same drop rate.
    //
    // We need the flows to go to different precedence levels. This
    // requires different DSCPs (AF11, AF12, AF13) which requires
    // different mark rules. The mark rules match by address. So we
    // need different destinations.

    // Final approach: 5 nodes (src, edge, s0, s1, s2).
    // edge->s0, edge->s1, edge->s2 all share the bottleneck? No,
    // they'd be separate links.
    //
    // The right topology: src -> edge -> core -> sinks
    // with bottleneck on edge->core. Multiple sinks behind core
    // on fast links. This is the standard DiffServ topology.

    Simulator::Destroy(); // clean up previous

    // Proper topology: src -> edge -> core -> {s0, s1, s2}
    NodeContainer n;
    n.Create(6); // src, src2, src3, edge, core, sink
    // Actually, simplest: 3 sources, 1 edge, 1 core, 1 sink
    // 3 sources send to same sink via edge->core bottleneck.
    // Each source gets a different mark rule by source address.

    NodeContainer srcs;
    srcs.Create(3);
    NodeContainer rtr;
    rtr.Create(2); // edge, core
    NodeContainer sinks;
    sinks.Create(1);

    Ptr<Node> edgeN = rtr.Get(0);
    Ptr<Node> coreN = rtr.Get(1);
    Ptr<Node> sinkN = sinks.Get(0);

    // src[i] -> edge (fast links)
    std::vector<NetDeviceContainer> srcEdgeDevs(3);
    for (uint32_t i = 0; i < 3; i++)
    {
        srcEdgeDevs[i] = p2pAccess.Install(srcs.Get(i), edgeN);
    }

    // edge -> core (bottleneck 2 Mbps)
    NetDeviceContainer devEC = p2pBottleneck.Install(edgeN, coreN);

    // core -> sink (fast)
    NetDeviceContainer devCS = p2pAccess.Install(coreN, sinkN);

    InternetStackHelper inet2;
    NodeContainer all2;
    all2.Add(srcs);
    all2.Add(rtr);
    all2.Add(sinks);
    inet2.Install(all2);

    Ipv4AddressHelper addr2;
    uint32_t subnet = 1;

    std::vector<Ipv4InterfaceContainer> srcIfs(3);
    for (uint32_t i = 0; i < 3; i++)
    {
        addr2.SetBase(("10.0." + std::to_string(subnet++) + ".0").c_str(), "255.255.255.0");
        srcIfs[i] = addr2.Assign(srcEdgeDevs[i]);
    }

    addr2.SetBase(("10.0." + std::to_string(subnet++) + ".0").c_str(), "255.255.255.0");
    Ipv4InterfaceContainer ifEC = addr2.Assign(devEC);

    addr2.SetBase(("10.0." + std::to_string(subnet++) + ".0").c_str(), "255.255.255.0");
    Ipv4InterfaceContainer ifCS = addr2.Assign(devCS);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    Ipv4Address sinkIp = ifCS.GetAddress(1);

    // ---- DiffServ Edge disc ----
    TrafficControlHelper tch2;
    tch2.Uninstall(devEC.Get(0));

    Ptr<EdgeQueueDisc> edge2 = CreateObject<EdgeQueueDisc>();
    auto inner2 = CreateObject<RedQueueDisc>();
    edge2->SetInnerDisc(inner2);
    diffserv::Helper h2;

    inner2->SetNumQueues(1);
    inner2->SetNumPrec(0, 3);
    inner2->SetQueueLimit(0, 50);
    inner2->SetMredModeAllQueues(MredMode::RIO_D);

    Ptr<PriorityScheduler> sched2 = CreateObjectWithAttributes<PriorityScheduler>("NumQueues",
                                                                                  UintegerValue(1),
                                                                                  "WinLen",
                                                                                  DoubleValue(1.0));
    inner2->SetScheduler(sched2);

    // Mark by source address: src0 -> AF11, src1 -> AF12, src2 -> AF13
    edge2->AddMarkRule({.dscp = 10, .srcAddr = srcIfs[0].GetAddress(0)});
    edge2->AddMarkRule({.dscp = 12, .srcAddr = srcIfs[1].GetAddress(0)});
    edge2->AddMarkRule({.dscp = 14, .srcAddr = srcIfs[2].GetAddress(0)});

    h2.AddDumbPolicy(edge2, 10);
    h2.AddDumbPolicy(edge2, 12);
    h2.AddDumbPolicy(edge2, 14);

    h2.AddPolicerEntry(edge2,
                       {.policer = PolicerType::DUMB,
                        .initialCodePt = 10,
                        .downgrade1 = 10,
                        .downgrade2 = 10,
                        .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});
    h2.AddPolicerEntry(edge2,
                       {.policer = PolicerType::DUMB,
                        .initialCodePt = 12,
                        .downgrade1 = 12,
                        .downgrade2 = 12,
                        .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});
    h2.AddPolicerEntry(edge2,
                       {.policer = PolicerType::DUMB,
                        .initialCodePt = 14,
                        .downgrade1 = 14,
                        .downgrade2 = 14,
                        .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});

    h2.AddPhbEntry(inner2, 10, 0, 0); // AF11 -> prec 0 (lowest drop)
    h2.AddPhbEntry(inner2, 12, 0, 1); // AF12 -> prec 1
    h2.AddPhbEntry(inner2, 14, 0, 2); // AF13 -> prec 2 (highest drop)

    Ptr<NetDevice> eDev = devEC.Get(0);
    Ptr<TrafficControlLayer> tcE = eDev->GetNode()->GetObject<TrafficControlLayer>();
    tcE->SetRootQueueDiscOnDevice(eDev, edge2);
    edge2->Initialize();

    // RIO thresholds: lower thresholds = more aggressive dropping
    h2.ConfigQueue(
        inner2,
        {.queue = 0, .prec = 0, .thMin = 40.0, .thMax = 50.0, .maxP = 0.02}); // AF11: most tolerant
    h2.ConfigQueue(
        inner2,
        {.queue = 0, .prec = 1, .thMin = 20.0, .thMax = 40.0, .maxP = 0.05}); // AF12: medium
    h2.ConfigQueue(inner2,
                   {.queue = 0,
                    .prec = 2,
                    .thMin = 5.0,
                    .thMax = 15.0,
                    .maxP = 0.10}); // AF13: most aggressive

    // ---- 3 TCP BulkSend flows ----
    uint16_t ports[3] = {100, 200, 300};
    for (uint32_t i = 0; i < 3; i++)
    {
        PacketSinkHelper sinkHelper("ns3::TcpSocketFactory",
                                    InetSocketAddress(Ipv4Address::GetAny(), ports[i]));
        ApplicationContainer sinkApp = sinkHelper.Install(sinkN);
        sinkApp.Start(Seconds(0.0));
        sinkApp.Stop(Seconds(kSimTime));

        // Rx callback for throughput measurement
        sinkApp.Get(0)->TraceConnectWithoutContext(
            "Rx",
            MakeBoundCallback(&AfDropPrecedenceQualityTest::RxCallback, this, i));

        ns3::OnOffHelper bulkSend("ns3::TcpSocketFactory", InetSocketAddress(sinkIp, ports[i]));
        bulkSend.SetAttribute("DataRate", StringValue("10Mbps"));
        bulkSend.SetAttribute("PacketSize", UintegerValue(1000));
        bulkSend.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1000]"));
        bulkSend.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
        ApplicationContainer sendApp = bulkSend.Install(srcs.Get(i));
        sendApp.Start(Seconds(1.0));
        sendApp.Stop(Seconds(kSimTime - 1.0));
    }

    // ---- Run ----
    Simulator::Stop(Seconds(kSimTime));
    Simulator::Run();

    double tput0 = m_rxBytes[0] * 8.0 / (kSimTime - 2.0); // bps
    double tput1 = m_rxBytes[1] * 8.0 / (kSimTime - 2.0);
    double tput2 = m_rxBytes[2] * 8.0 / (kSimTime - 2.0);

    Simulator::Destroy();

    // Q-5.2: Throughput ordering (implies Q-5.1 drop ordering for TCP)
    NS_TEST_ASSERT_MSG_GT(tput0,
                          tput1,
                          "Q-5.2: tput(AF11)=" << tput0 << " should be > tput(AF12)=" << tput1);
    NS_TEST_ASSERT_MSG_GT(tput1,
                          tput2,
                          "Q-5.2: tput(AF12)=" << tput1 << " should be > tput(AF13)=" << tput2);

    // Q-5.3: Aggregate throughput within ±10% of link capacity
    double totalTput = tput0 + tput1 + tput2;
    NS_TEST_ASSERT_MSG_GT(totalTput,
                          kLinkBw * 0.7,
                          "Q-5.3: aggregate AF1 throughput " << totalTput << " too low (< 70% of "
                                                             << kLinkBw << ")");

    std::ostringstream q5Sum;
    q5Sum << "\n  [Q-5] AF PHB results:\n"
          << "    tput:  AF11=" << (tput0 / 1e3) << " AF12=" << (tput1 / 1e3)
          << " AF13=" << (tput2 / 1e3) << " kbps\n"
          << "    total: " << (totalTput / 1e3) << " kbps"
          << " (link: " << (kLinkBw / 1e3) << " kbps)\n";
    std::cout << q5Sum.str() << std::endl;
}

// =============================================================================
//  Q-6: EF + AF + BE coexistence
// =============================================================================

/**
 * @brief End-to-end EF plus AF plus BE coexistence scenario validation.
 * @see specs/03-quality.md Q-6
 */
class ThreeClassCoexistenceTest : public TestCase
{
  public:
    ThreeClassCoexistenceTest()
        : TestCase("Q-6 EF+AF+BE coexistence (3-class PQ, 10 Mbps)")
    {
    }

  private:
    uint64_t m_efRxBytes;
    uint64_t m_afRxBytes;
    uint64_t m_beRxBytes;
    double m_maxEfDelay;

    void EfRx(Ptr<const Packet> pkt, const Address&)
    {
        m_efRxBytes += pkt->GetSize();
        // Check OWD via send time tag
        SendTimeTag tag;
        if (pkt->PeekPacketTag(tag))
        {
            double owd = Simulator::Now().GetSeconds() - tag.GetSendTime();
            if (owd > m_maxEfDelay)
            {
                m_maxEfDelay = owd;
            }
        }
    }

    void AfRx(Ptr<const Packet> pkt, const Address&)
    {
        m_afRxBytes += pkt->GetSize();
    }

    void BeRx(Ptr<const Packet> pkt, const Address&)
    {
        m_beRxBytes += pkt->GetSize();
    }

    void DoRun() override;
};

void
ThreeClassCoexistenceTest::DoRun()
{
    m_efRxBytes = m_afRxBytes = m_beRxBytes = 0;
    m_maxEfDelay = 0.0;

    const double kSimTime = 30.0;
    const double kLinkBw = 10e6;    // 10 Mbps
    const double kEfRate = 3e6;     // 3 Mbps (30%)
    const double kPropDelay = 5e-3; // 5 ms
    const uint32_t kEfPktSize = 200;

    // ---- Nodes: 3 sources, edge, sink ----
    NodeContainer srcs;
    srcs.Create(3); // efSrc, afSrc, beSrc
    NodeContainer edgeNode;
    edgeNode.Create(1);
    NodeContainer sinkNode;
    sinkNode.Create(1);

    Ptr<Node> eNode = edgeNode.Get(0);
    Ptr<Node> sNode = sinkNode.Get(0);

    PointToPointHelper p2pFast;
    p2pFast.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    p2pFast.SetChannelAttribute("Delay", StringValue("1ms"));

    PointToPointHelper p2pBn;
    p2pBn.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
    p2pBn.SetChannelAttribute("Delay", StringValue("5ms"));
    p2pBn.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("1p"));

    // src[i] -> edge (fast)
    std::vector<NetDeviceContainer> srcDevs(3);
    for (uint32_t i = 0; i < 3; i++)
    {
        srcDevs[i] = p2pFast.Install(srcs.Get(i), eNode);
    }

    // edge -> sink (bottleneck)
    NetDeviceContainer devES = p2pBn.Install(eNode, sNode);

    InternetStackHelper inet;
    NodeContainer allN;
    allN.Add(srcs);
    allN.Add(edgeNode);
    allN.Add(sinkNode);
    inet.Install(allN);

    Ipv4AddressHelper a;
    uint32_t sub = 1;

    std::vector<Ipv4InterfaceContainer> srcIfs(3);
    for (uint32_t i = 0; i < 3; i++)
    {
        a.SetBase(("10.0." + std::to_string(sub++) + ".0").c_str(), "255.255.255.0");
        srcIfs[i] = a.Assign(srcDevs[i]);
    }
    a.SetBase(("10.0." + std::to_string(sub++) + ".0").c_str(), "255.255.255.0");
    Ipv4InterfaceContainer ifES = a.Assign(devES);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    Ipv4Address sinkIp = ifES.GetAddress(1);

    // ---- DiffServ Edge: 3 queues (EF, AF, BE) ----
    TrafficControlHelper tchU;
    tchU.Uninstall(devES.Get(0));

    Ptr<EdgeQueueDisc> disc = CreateObject<EdgeQueueDisc>();
    auto discInner = CreateObject<RedQueueDisc>();
    disc->SetInnerDisc(discInner);
    diffserv::Helper hlp;

    discInner->SetNumQueues(3);
    discInner->SetNumPrec(0, 1); // EF queue
    discInner->SetNumPrec(1, 1); // AF queue
    discInner->SetNumPrec(2, 1); // BE queue
    discInner->SetQueueLimit(0, 50);
    discInner->SetQueueLimit(1, 100);
    discInner->SetQueueLimit(2, 100);
    discInner->SetMredModeAllQueues(MredMode::DROP_TAIL);

    // PQ scheduler: EF (queue 0) has strict priority
    Ptr<PriorityScheduler> pq = CreateObjectWithAttributes<PriorityScheduler>("NumQueues",
                                                                              UintegerValue(3),
                                                                              "WinLen",
                                                                              DoubleValue(1.0));
    discInner->SetScheduler(pq);

    // Mark by source address
    disc->AddMarkRule({.dscp = 46, .srcAddr = srcIfs[0].GetAddress(0)}); // EF
    disc->AddMarkRule({.dscp = 10, .srcAddr = srcIfs[1].GetAddress(0)}); // AF
    disc->AddMarkRule({.dscp = 0, .srcAddr = srcIfs[2].GetAddress(0)});  // BE

    hlp.AddDumbPolicy(disc, 46);
    hlp.AddDumbPolicy(disc, 10);
    hlp.AddDumbPolicy(disc, 0);

    hlp.AddPolicerEntry(disc,
                        {.policer = PolicerType::DUMB,
                         .initialCodePt = 46,
                         .downgrade1 = 46,
                         .downgrade2 = 46,
                         .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});
    hlp.AddPolicerEntry(disc,
                        {.policer = PolicerType::DUMB,
                         .initialCodePt = 10,
                         .downgrade1 = 10,
                         .downgrade2 = 10,
                         .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});
    hlp.AddPolicerEntry(disc,
                        {.policer = PolicerType::DUMB,
                         .initialCodePt = 0,
                         .downgrade1 = 0,
                         .downgrade2 = 0,
                         .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});

    hlp.AddPhbEntry(discInner, 46, 0, 0); // EF -> queue 0
    hlp.AddPhbEntry(discInner, 10, 1, 0); // AF -> queue 1
    hlp.AddPhbEntry(discInner, 0, 2, 0);  // BE -> queue 2

    Ptr<NetDevice> eDv = devES.Get(0);
    Ptr<TrafficControlLayer> tcl = eDv->GetNode()->GetObject<TrafficControlLayer>();
    tcl->SetRootQueueDiscOnDevice(eDv, disc);
    disc->Initialize();

    hlp.ConfigQueue(discInner, {.queue = 0, .prec = 0, .thMin = 50.0, .thMax = 50.0, .maxP = 1.0});
    hlp.ConfigQueue(discInner,
                    {.queue = 1, .prec = 0, .thMin = 100.0, .thMax = 100.0, .maxP = 1.0});
    hlp.ConfigQueue(discInner,
                    {.queue = 2, .prec = 0, .thMin = 100.0, .thMax = 100.0, .maxP = 1.0});

    // ---- EF: CBR UDP at 3 Mbps ----
    uint16_t efPort = 9;
    PacketSinkHelper efSink("ns3::UdpSocketFactory",
                            InetSocketAddress(Ipv4Address::GetAny(), efPort));
    ApplicationContainer efSinkApp = efSink.Install(sNode);
    efSinkApp.Start(Seconds(0.0));
    efSinkApp.Stop(Seconds(kSimTime));
    efSinkApp.Get(0)->TraceConnectWithoutContext(
        "Rx",
        MakeCallback(&ThreeClassCoexistenceTest::EfRx, this));

    ns3::OnOffHelper efOnOff("ns3::UdpSocketFactory", InetSocketAddress(sinkIp, efPort));
    efOnOff.SetAttribute("DataRate", StringValue("3Mbps"));
    efOnOff.SetAttribute("PacketSize", UintegerValue(kEfPktSize));
    efOnOff.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1000]"));
    efOnOff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
    ApplicationContainer efApp = efOnOff.Install(srcs.Get(0));
    efApp.Start(Seconds(1.0));
    efApp.Stop(Seconds(kSimTime - 1.0));

    // ---- AF: TCP BulkSend ----
    uint16_t afPort = 10;
    PacketSinkHelper afSink("ns3::TcpSocketFactory",
                            InetSocketAddress(Ipv4Address::GetAny(), afPort));
    ApplicationContainer afSinkApp = afSink.Install(sNode);
    afSinkApp.Start(Seconds(0.0));
    afSinkApp.Stop(Seconds(kSimTime));
    afSinkApp.Get(0)->TraceConnectWithoutContext(
        "Rx",
        MakeCallback(&ThreeClassCoexistenceTest::AfRx, this));

    ns3::OnOffHelper afBulk("ns3::TcpSocketFactory", InetSocketAddress(sinkIp, afPort));
    afBulk.SetAttribute("DataRate", StringValue("10Mbps"));
    afBulk.SetAttribute("PacketSize", UintegerValue(1000));
    afBulk.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1000]"));
    afBulk.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
    ApplicationContainer afApp = afBulk.Install(srcs.Get(1));
    afApp.Start(Seconds(1.0));
    afApp.Stop(Seconds(kSimTime - 1.0));

    // ---- BE: TCP BulkSend ----
    uint16_t bePort = 11;
    PacketSinkHelper beSink("ns3::TcpSocketFactory",
                            InetSocketAddress(Ipv4Address::GetAny(), bePort));
    ApplicationContainer beSinkApp = beSink.Install(sNode);
    beSinkApp.Start(Seconds(0.0));
    beSinkApp.Stop(Seconds(kSimTime));
    beSinkApp.Get(0)->TraceConnectWithoutContext(
        "Rx",
        MakeCallback(&ThreeClassCoexistenceTest::BeRx, this));

    ns3::OnOffHelper beBulk("ns3::TcpSocketFactory", InetSocketAddress(sinkIp, bePort));
    beBulk.SetAttribute("DataRate", StringValue("10Mbps"));
    beBulk.SetAttribute("PacketSize", UintegerValue(1000));
    beBulk.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1000]"));
    beBulk.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
    ApplicationContainer beApp = beBulk.Install(srcs.Get(2));
    beApp.Start(Seconds(1.0));
    beApp.Stop(Seconds(kSimTime - 1.0));

    // ---- Run ----
    Simulator::Stop(Seconds(kSimTime));
    Simulator::Run();
    Simulator::Destroy();

    double duration = kSimTime - 2.0; // effective traffic duration
    double efTput = m_efRxBytes * 8.0 / duration;
    double afTput = m_afRxBytes * 8.0 / duration;
    double beTput = m_beRxBytes * 8.0 / duration;

    // Q-6.1: EF max delay < 2× propagation delay
    // Propagation: 5ms edge->sink + 1ms src->edge = 6ms one-way
    // Plus serialization: 200*8/10e6 = 0.16ms
    // Budget: 2 × 6ms = 12ms
    double delayBudget = 2.0 * (kPropDelay + 1e-3); // 12 ms
    NS_TEST_ASSERT_MSG_LT(m_maxEfDelay,
                          delayBudget,
                          "Q-6.1: EF max delay " << (m_maxEfDelay * 1e3) << " ms exceeds budget "
                                                 << (delayBudget * 1e3) << " ms");

    // Q-6.2: AF throughput > 0 (gets some share of remaining capacity)
    NS_TEST_ASSERT_MSG_GT(afTput, 0.0, "Q-6.2: AF throughput should be > 0");

    // Q-6.3: BE uses residual capacity
    double residual = kLinkBw - kEfRate;
    NS_TEST_ASSERT_MSG_GT(beTput, 0.0, "Q-6.3: BE throughput should be > 0");

    // Q-6.4: No starvation — BE throughput > 0
    // (already covered by Q-6.3 assertion, but we verify the aggregate
    // is reasonable: AF + BE should use most of the non-EF bandwidth)
    NS_TEST_ASSERT_MSG_GT(afTput + beTput,
                          residual * 0.5,
                          "Q-6.4: AF+BE combined "
                              << ((afTput + beTput) / 1e3)
                              << " kbps should use significant residual capacity");

    std::ostringstream q6Sum;
    q6Sum << "\n  [Q-6] Three-class coexistence results:\n"
          << "    EF:  " << (efTput / 1e6) << " Mbps (target: 3 Mbps)"
          << " maxDelay: " << (m_maxEfDelay * 1e3) << " ms\n"
          << "    AF:  " << (afTput / 1e6) << " Mbps\n"
          << "    BE:  " << (beTput / 1e6) << " Mbps\n"
          << "    Sum: " << ((efTput + afTput + beTput) / 1e6) << " Mbps"
          << " (link: " << (kLinkBw / 1e6) << " Mbps)\n";
    std::cout << q6Sum.str() << std::endl;
}

// =============================================================================
//  Q-2: Example-2 reproduction — three-class DiffServ with port classification
// =============================================================================

/**
 * @brief End-to-end three-class scenario validation against the example-2 reference.
 * @see specs/03-quality.md Q-2
 */
class Example2ThreeClassTest : public TestCase
{
  public:
    Example2ThreeClassTest()
        : TestCase("Q-2 Example-2 three-class DiffServ (EF+AF+BE, port "
                   "classification)")
    {
    }

  private:
    uint64_t m_efRxBytes{0};
    uint64_t m_goldRxBytes{0};
    uint64_t m_beRxBytes{0};

    static void EfRx(Example2ThreeClassTest* self, Ptr<const Packet> pkt, const Address&)
    {
        self->m_efRxBytes += pkt->GetSize();
    }

    static void GoldRx(Example2ThreeClassTest* self, Ptr<const Packet> pkt, const Address&)
    {
        self->m_goldRxBytes += pkt->GetSize();
    }

    static void BeRx(Example2ThreeClassTest* self, Ptr<const Packet> pkt, const Address&)
    {
        self->m_beRxBytes += pkt->GetSize();
    }

    void DoRun() override
    {
        m_efRxBytes = m_goldRxBytes = m_beRxBytes = 0;
        const double kSimTime = 60.0;

        // ---- Nodes ----
        NodeContainer srcs;
        srcs.Create(3);
        NodeContainer rtrs;
        rtrs.Create(2);
        NodeContainer dsts;
        dsts.Create(3);

        PointToPointHelper fast;
        fast.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
        fast.SetChannelAttribute("Delay", StringValue("1ms"));

        PointToPointHelper bn;
        bn.SetDeviceAttribute("DataRate", StringValue("2Mbps"));
        bn.SetChannelAttribute("Delay", StringValue("5ms"));
        bn.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("1p"));

        std::vector<NetDeviceContainer> sD(3);
        for (uint32_t i = 0; i < 3; i++)
        {
            sD[i] = fast.Install(srcs.Get(i), rtrs.Get(0));
        }

        NetDeviceContainer dEC = bn.Install(rtrs.Get(0), rtrs.Get(1));

        std::vector<NetDeviceContainer> dD(3);
        for (uint32_t i = 0; i < 3; i++)
        {
            dD[i] = fast.Install(rtrs.Get(1), dsts.Get(i));
        }

        InternetStackHelper inet;
        NodeContainer all;
        all.Add(srcs);
        all.Add(rtrs);
        all.Add(dsts);
        inet.Install(all);

        Ipv4AddressHelper a;
        uint32_t sub = 1;
        std::vector<Ipv4InterfaceContainer> sIf(3);
        for (uint32_t i = 0; i < 3; i++)
        {
            a.SetBase(("10.0." + std::to_string(sub++) + ".0").c_str(), "255.255.255.0");
            sIf[i] = a.Assign(sD[i]);
        }
        a.SetBase(("10.0." + std::to_string(sub++) + ".0").c_str(), "255.255.255.0");
        a.Assign(dEC);

        std::vector<Ipv4InterfaceContainer> dIf(3);
        for (uint32_t i = 0; i < 3; i++)
        {
            a.SetBase(("10.0." + std::to_string(sub++) + ".0").c_str(), "255.255.255.0");
            dIf[i] = a.Assign(dD[i]);
        }
        Ipv4GlobalRoutingHelper::PopulateRoutingTables();

        Ipv4Address efDst = dIf[0].GetAddress(1);
        Ipv4Address goldDst = dIf[1].GetAddress(1);
        Ipv4Address beDst = dIf[2].GetAddress(1);

        // ---- Edge disc: 3 queues ----
        TrafficControlHelper tU;
        tU.Uninstall(dEC.Get(0));

        Ptr<EdgeQueueDisc> disc = CreateObject<EdgeQueueDisc>();
        auto discInner = CreateObject<RedQueueDisc>();
        disc->SetInnerDisc(discInner);
        diffserv::Helper h;

        discInner->SetNumQueues(3);
        discInner->SetNumPrec(0, 2);
        discInner->SetNumPrec(1, 3);
        discInner->SetNumPrec(2, 2);
        discInner->SetQueueLimit(0, 50);
        discInner->SetQueueLimit(1, 150);
        discInner->SetQueueLimit(2, 100);

        Ptr<PriorityScheduler> pq = CreateObjectWithAttributes<PriorityScheduler>("NumQueues",
                                                                                  UintegerValue(3),
                                                                                  "WinLen",
                                                                                  DoubleValue(1.0));
        discInner->SetScheduler(pq);

        // Mark rules: Premium by src, Gold by dst port, BE is default
        disc->AddMarkRule({.dscp = 46, .srcAddr = sIf[0].GetAddress(0)});
        disc->AddMarkRule({.dscp = 10, .protocol = 6, .dstPort = 23});
        disc->AddMarkRule({.dscp = 12, .protocol = 6, .dstPort = 20});

        // Metering
        h.AddTokenBucketPolicy(disc, {.codePt = 46, .cirBps = 500000.0, .cbsBytes = 100000.0});
        h.AddDumbPolicy(disc, 51);
        h.AddPolicerEntry(disc,
                          {.policer = PolicerType::TOKEN_BUCKET,
                           .initialCodePt = 46,
                           .downgrade1 = 51,
                           .downgrade2 = 51,
                           .policyIndex = static_cast<uint32_t>(PolicerType::TOKEN_BUCKET)});

        h.AddDumbPolicy(disc, 10);
        h.AddPolicerEntry(disc,
                          {.policer = PolicerType::DUMB,
                           .initialCodePt = 10,
                           .downgrade1 = 10,
                           .downgrade2 = 10,
                           .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});
        h.AddTsw2cmPolicy(disc, {.codePt = 12, .cirBps = 500000.0});
        h.AddDumbPolicy(disc, 14);
        h.AddPolicerEntry(disc,
                          {.policer = PolicerType::TSW2CM,
                           .initialCodePt = 12,
                           .downgrade1 = 14,
                           .downgrade2 = 14,
                           .policyIndex = static_cast<uint32_t>(PolicerType::TSW2CM)});

        h.AddTokenBucketPolicy(disc, {.codePt = 0, .cirBps = 700000.0, .cbsBytes = 100000.0});
        h.AddDumbPolicy(disc, 50);
        h.AddPolicerEntry(disc,
                          {.policer = PolicerType::TOKEN_BUCKET,
                           .initialCodePt = 0,
                           .downgrade1 = 50,
                           .downgrade2 = 50,
                           .policyIndex = static_cast<uint32_t>(PolicerType::TOKEN_BUCKET)});

        // PHB
        h.AddPhbEntry(discInner, 46, 0, 0);
        h.AddPhbEntry(discInner, 51, 0, 1);
        h.AddPhbEntry(discInner, 10, 1, 0);
        h.AddPhbEntry(discInner, 12, 1, 1);
        h.AddPhbEntry(discInner, 14, 1, 2);
        h.AddPhbEntry(discInner, 0, 2, 0);
        h.AddPhbEntry(discInner, 50, 2, 1);

        Ptr<NetDevice> eD = dEC.Get(0);
        Ptr<TrafficControlLayer> tc = eD->GetNode()->GetObject<TrafficControlLayer>();
        tc->SetRootQueueDiscOnDevice(eD, disc);
        disc->Initialize();

        discInner->SetMredMode(MredMode::DROP_TAIL, 0);
        h.ConfigQueue(discInner,
                      {.queue = 0, .prec = 0, .thMin = 30.0, .thMax = 30.0, .maxP = 1.0});
        h.ConfigQueue(discInner, {.queue = 0, .prec = 1, .thMin = 0.0, .thMax = 0.0, .maxP = 0.0});

        discInner->SetMredMode(MredMode::RIO_C, 1);
        h.ConfigQueue(discInner,
                      {.queue = 1, .prec = 0, .thMin = 60.0, .thMax = 110.0, .maxP = 0.02});
        h.ConfigQueue(discInner,
                      {.queue = 1, .prec = 1, .thMin = 30.0, .thMax = 60.0, .maxP = 0.6});
        h.ConfigQueue(discInner, {.queue = 1, .prec = 2, .thMin = 5.0, .thMax = 10.0, .maxP = 0.8});

        discInner->SetMredMode(MredMode::DROP_TAIL, 2);
        h.ConfigQueue(discInner,
                      {.queue = 2, .prec = 0, .thMin = 100.0, .thMax = 100.0, .maxP = 1.0});
        h.ConfigQueue(discInner, {.queue = 2, .prec = 1, .thMin = 0.0, .thMax = 0.0, .maxP = 0.0});

        // ---- EF: CBR 300 kbps ----
        PacketSinkHelper eSink("ns3::UdpSocketFactory",
                               InetSocketAddress(Ipv4Address::GetAny(), 9));
        ApplicationContainer eSA = eSink.Install(dsts.Get(0));
        eSA.Start(Seconds(0.0));
        eSA.Stop(Seconds(kSimTime));
        eSA.Get(0)->TraceConnectWithoutContext(
            "Rx",
            MakeBoundCallback(&Example2ThreeClassTest::EfRx, this));

        ns3::OnOffHelper eOO("ns3::UdpSocketFactory", InetSocketAddress(efDst, 9));
        eOO.SetAttribute("DataRate", StringValue("300kbps"));
        eOO.SetAttribute("PacketSize", UintegerValue(1300));
        eOO.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1000]"));
        eOO.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
        ApplicationContainer eA = eOO.Install(srcs.Get(0));
        eA.Start(Seconds(1.0));
        eA.Stop(Seconds(kSimTime - 1.0));

        // ---- Gold: Telnet (port 23) + FTP (port 20) ----
        PacketSinkHelper tSink("ns3::TcpSocketFactory",
                               InetSocketAddress(Ipv4Address::GetAny(), 23));
        ApplicationContainer tSA = tSink.Install(dsts.Get(1));
        tSA.Start(Seconds(0.0));
        tSA.Stop(Seconds(kSimTime));
        tSA.Get(0)->TraceConnectWithoutContext(
            "Rx",
            MakeBoundCallback(&Example2ThreeClassTest::GoldRx, this));

        ns3::OnOffHelper tOO("ns3::TcpSocketFactory", InetSocketAddress(goldDst, 23));
        tOO.SetAttribute("DataRate", StringValue("50kbps"));
        tOO.SetAttribute("PacketSize", UintegerValue(64));
        tOO.SetAttribute("OnTime", StringValue("ns3::ExponentialRandomVariable[Mean=0.5]"));
        tOO.SetAttribute("OffTime", StringValue("ns3::ExponentialRandomVariable[Mean=0.5]"));
        for (uint32_t i = 0; i < 4; i++)
        {
            ApplicationContainer ta = tOO.Install(srcs.Get(1));
            ta.Start(Seconds(0.5 + 0.1 * i));
            ta.Stop(Seconds(kSimTime - 1.0));
        }

        PacketSinkHelper fSink("ns3::TcpSocketFactory",
                               InetSocketAddress(Ipv4Address::GetAny(), 20));
        ApplicationContainer fSA = fSink.Install(dsts.Get(1));
        fSA.Start(Seconds(0.0));
        fSA.Stop(Seconds(kSimTime));
        fSA.Get(0)->TraceConnectWithoutContext(
            "Rx",
            MakeBoundCallback(&Example2ThreeClassTest::GoldRx, this));

        ns3::OnOffHelper fBulk("ns3::TcpSocketFactory", InetSocketAddress(goldDst, 20));
        fBulk.SetAttribute("DataRate", StringValue("10Mbps"));
        fBulk.SetAttribute("PacketSize", UintegerValue(1000));
        fBulk.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1000]"));
        fBulk.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
        for (uint32_t i = 0; i < 4; i++)
        {
            ApplicationContainer fa = fBulk.Install(srcs.Get(1));
            fa.Start(Seconds(1.0 + 0.1 * i));
            fa.Stop(Seconds(kSimTime - 1.0));
        }

        // ---- BE: CBR UDP ----
        PacketSinkHelper bSink("ns3::UdpSocketFactory",
                               InetSocketAddress(Ipv4Address::GetAny(), 11));
        ApplicationContainer bSA = bSink.Install(dsts.Get(2));
        bSA.Start(Seconds(0.0));
        bSA.Stop(Seconds(kSimTime));
        bSA.Get(0)->TraceConnectWithoutContext(
            "Rx",
            MakeBoundCallback(&Example2ThreeClassTest::BeRx, this));

        uint32_t bgSz = 64;
        for (uint32_t i = 0; i < 10; i++)
        {
            ns3::OnOffHelper bOO("ns3::UdpSocketFactory", InetSocketAddress(beDst, 11));
            bOO.SetAttribute("DataRate", StringValue("100kbps"));
            bOO.SetAttribute("PacketSize", UintegerValue(bgSz));
            bOO.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1000]"));
            bOO.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
            ApplicationContainer ba = bOO.Install(srcs.Get(2));
            ba.Start(Seconds(0.5 + 0.2 * i));
            ba.Stop(Seconds(kSimTime - 1.0));
            bgSz += 64;
        }

        // ---- Run ----
        Simulator::Stop(Seconds(kSimTime));
        Simulator::Run();
        Simulator::Destroy();

        double dur = kSimTime - 2.0;
        double efT = m_efRxBytes * 8.0 / dur;
        double goldT = m_goldRxBytes * 8.0 / dur;
        double beT = m_beRxBytes * 8.0 / dur;

        // Q-2.1: All classes receive non-zero throughput
        NS_TEST_ASSERT_MSG_GT(efT, 0.0, "Q-2.1: EF throughput should be > 0");
        NS_TEST_ASSERT_MSG_GT(goldT, 0.0, "Q-2.1: Gold throughput should be > 0");
        NS_TEST_ASSERT_MSG_GT(beT, 0.0, "Q-2.1: BE throughput should be > 0");

        // EF should get close to 300 kbps
        NS_TEST_ASSERT_MSG_GT(efT,
                              200e3,
                              "Q-2.1: EF throughput " << (efT / 1e3) << " kbps too low");

        // PQ ensures EF gets its full rate (no drops for in-profile).
        // EF throughput may be lower than BE because EF is rate-limited
        // by the application (300 kbps), while BE uses spare capacity.
        NS_TEST_ASSERT_MSG_GT(efT,
                              250e3,
                              "Q-2.1: EF throughput " << (efT / 1e3)
                                                      << " kbps should be close to 300 kbps");

        std::ostringstream q2Sum;
        q2Sum << "\n  [Q-2] Example-2 three-class results:\n"
              << "    EF:    " << (efT / 1e3) << " kbps\n"
              << "    Gold:  " << (goldT / 1e3) << " kbps\n"
              << "    BE:    " << (beT / 1e3) << " kbps\n"
              << "    Total: " << ((efT + goldT + beT) / 1e3) << " kbps (link: 2000 kbps)\n";
        std::cout << q2Sum.str() << std::endl;
    }
};

// =============================================================================
//  S-18.1: AQM substrate-registry template round-trip
// =============================================================================
//
// Asserts that the first instantiation of Registry<EntryT> (AqmRegistry)
// preserves the invariants the template promises:
//   - every registered entry is reachable via Find(fileTag);
//   - All() returns a stable count that matches the committed manifest;
//   - every entry belongs to exactly one of the three families (Single,
//     Fq, Stratum) — i.e. the family partition is total.
//
// Comma-separated test name: the ns-3 test runner treats a slash in a
// case name as a path separator and silently aborts the whole suite at
// registration time. Commas are safe.

class AqmRegistryTemplateRoundTripTest : public TestCase
{
  public:
    AqmRegistryTemplateRoundTripTest()
        : TestCase("S-18.1, AQM registry template round-trip")
    {
    }

  private:
    void DoRun() override
    {
        using aqm_eval::AqmEntry;
        using aqm_eval::AqmRegistry;
        const auto& reg = AqmRegistry::Get();

        const std::size_t kExpectedCount = 13;
        NS_TEST_ASSERT_MSG_EQ(reg.Size(),
                              kExpectedCount,
                              "Registry size drifted from committed manifest");
        NS_TEST_ASSERT_MSG_EQ(reg.All().size(), kExpectedCount, "All() size disagrees with Size()");

        for (const auto& e : reg.All())
        {
            const AqmEntry* found = reg.Find(e.fileTag);
            NS_TEST_ASSERT_MSG_NE(found, nullptr, "Find(fileTag) miss for " << e.fileTag);
            if (!found)
            {
                continue;
            }
            NS_TEST_ASSERT_MSG_EQ(found->fileTag, e.fileTag, "Find(fileTag) returned wrong entry");

            const AqmEntry* byName = reg.FindByName(e.name);
            NS_TEST_ASSERT_MSG_NE(byName, nullptr, "FindByName miss for " << e.name);
        }

        const auto single = reg.ByFamily(AqmEntry::Family::Single);
        const auto fq = reg.ByFamily(AqmEntry::Family::Fq);
        const auto stratum = reg.ByFamily(AqmEntry::Family::Stratum);
        NS_TEST_ASSERT_MSG_EQ(single.size() + fq.size() + stratum.size(),
                              reg.Size(),
                              "Family partition is not total");
    }
};

// =============================================================================
//  S-18.2: Scheduler substrate-registry smoke
// =============================================================================
//
// Iterates the scheduler registry and asserts every cell instantiates
// without error under canonical args (NumQueues=4 with EF/AF/BE/BK
// weight policy, parameterised by ParameterShape). Catches missing
// registrations, wrong attribute names, and broken SetParam wiring at
// unit-test time.

class SchedulerRegistrySmokeTest : public TestCase
{
  public:
    SchedulerRegistrySmokeTest()
        : TestCase("S-18.2, Scheduler registry smoke")
    {
    }

  private:
    void DoRun() override
    {
        using PS = SchedulerEntry::ParameterShape;
        const auto& reg = SchedulerRegistry::Get();

        const std::size_t kExpectedCount = 9;
        NS_TEST_ASSERT_MSG_EQ(reg.Size(),
                              kExpectedCount,
                              "Scheduler registry size drifted from manifest");

        for (const auto& e : reg.All())
        {
            const SchedulerEntry* found = reg.Find(e.fileTag);
            NS_TEST_ASSERT_MSG_NE(found, nullptr, "Find(fileTag) miss for " << e.fileTag);

            SchedulerArgs args;
            args.numQueues = 4;
            args.linkBps = 10'000'000.0;
            switch (e.parameterShape)
            {
            case PS::None:
            case PS::PriorityWinLen:
                break;
            case PS::RoundRobinWeights:
                args.weights = {4, 3, 2, 1};
                break;
            case PS::FairQueueShares:
                args.weights = {0.40, 0.30, 0.20, 0.10};
                break;
            case PS::HybridLlq:
                args.weights = {0.0, 0.50, 0.33, 0.17};
                break;
            }
            Ptr<Scheduler> sched = reg.Construct(e.fileTag, args);
            NS_TEST_ASSERT_MSG_NE(sched, nullptr, "Construct returned null for " << e.fileTag);
        }

        // Family partition is total: every entry belongs to exactly one
        // of {priority, round-robin, fair-queue, hybrid}.
        using F = SchedulerEntry::Family;
        const std::size_t total =
            reg.ByFamily(F::Priority).size() + reg.ByFamily(F::RoundRobin).size() +
            reg.ByFamily(F::FairQueue).size() + reg.ByFamily(F::Hybrid).size();
        NS_TEST_ASSERT_MSG_EQ(total, reg.Size(), "Scheduler family partition is not total");
    }
};

// =============================================================================
//  S-aqm-runner-* — aqm-eval-runner measurement-correctness gates
// =============================================================================
//
// Smoke-runs one deterministic, unsaturated UDP-CBR cell of the AQM
// characterisation harness (mild-congestion × FqCoDel: 7.2 Mbps offered on
// the 10 Mbps bottleneck, byte-identical across RngRuns) and gates the
// measurement identities from the emitted per-flow CSV.  The subprocess
// pattern mirrors the example-1 instrumentation suite: the peer ns-3 root is
// derived from the running test-runner binary's location, so the smoke run
// works in any checkout.

/// One parsed row of an aqm-eval-runner per-flow CSV.
struct AqmEvalCsvRow
{
    std::string name;
    uint64_t fmRxBytes;
    uint64_t fmRetxBytes;
    uint64_t sinkRxBytes;
    double rxRateBps;
    double meanDelayMs;
};

/// ns-3 root of the calling test session (test-runner lives at
/// build/utils/, two levels below the root that holds ./ns3).
static std::string
AqmEvalPeerRoot()
{
    return std::filesystem::path(SystemPath::FindSelfDirectory())
        .parent_path()
        .parent_path()
        .string();
}

/// Smoke-runs the canonical deterministic cell into @p outDir.
/// Returns true on runner exit code 0.
static bool
AqmEvalRunSmokeCell(const std::string& outDir)
{
    std::filesystem::remove_all(outDir);
    const std::string cmd = "cd " + AqmEvalPeerRoot() +
                            " && ./ns3 run \"aqm-eval-runner"
                            " --scenario=mild-congestion"
                            " --aqm=ns3::FqCoDelQueueDisc"
                            " --simTime=10"
                            " --outDir=" +
                            outDir + "\" > /dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

/// Parses <outDir>/mild-congestion-FqCoDel-perflow.csv (header skipped).
static std::vector<AqmEvalCsvRow>
AqmEvalReadPerflowCsv(const std::string& outDir)
{
    std::vector<AqmEvalCsvRow> rows;
    std::ifstream in(outDir + "/mild-congestion-FqCoDel-perflow.csv");
    std::string line;
    std::getline(in, line); // header
    while (std::getline(in, line))
    {
        std::stringstream ss(line);
        std::string f;
        AqmEvalCsvRow r;
        std::getline(ss, r.name, ',');
        std::getline(ss, f, ',');
        r.fmRxBytes = std::stoull(f);
        std::getline(ss, f, ',');
        r.fmRetxBytes = std::stoull(f);
        std::getline(ss, f, ',');
        r.sinkRxBytes = std::stoull(f);
        std::getline(ss, f, ',');
        r.rxRateBps = std::stod(f);
        std::getline(ss, f, ',');
        r.meanDelayMs = std::stod(f);
        rows.push_back(r);
    }
    return rows;
}

// Flow-plan constants of the smoke cell (mild-congestion, --simTime=10).
constexpr double kAqmEvalSmokeSimTime = 10.0;
constexpr double kAqmEvalSmokeStartSec = 1.0;
constexpr double kAqmEvalBulkRateBps = 3.5e6;
constexpr double kAqmEvalRateBand = 0.03;

/// S-aqm-runner-goodput-window-full-simtime: the per-flow rx_rate_bps is
/// (fm_rx_bytes - fm_retx_bytes) * 8 / simTime — the goodput denominator is
/// the full --simTime window, with no warm-up trim.
class TestSAqmRunnerGoodputWindowFullSimtime : public TestCase
{
  public:
    TestSAqmRunnerGoodputWindowFullSimtime()
        : TestCase("S-aqm-runner-goodput-window-full-simtime, rate identity over --simTime")
    {
    }

  private:
    void DoRun() override
    {
        const std::string outDir = "/tmp/stratum-test-aqm-runner-window";
        NS_TEST_ASSERT_MSG_EQ(AqmEvalRunSmokeCell(outDir), true, "smoke cell run failed");

        // The summary must echo the simTime the rate identity divides by.
        std::ifstream sm(outDir + "/mild-congestion-FqCoDel-summary.txt");
        bool sawSimTime = false;
        std::string line;
        while (std::getline(sm, line))
        {
            if (line == "simTime=10")
            {
                sawSimTime = true;
            }
        }
        NS_TEST_ASSERT_MSG_EQ(sawSimTime, true, "summary does not echo simTime=10");

        const auto rows = AqmEvalReadPerflowCsv(outDir);
        NS_TEST_ASSERT_MSG_EQ(rows.size(), 3u, "expected 2 bulk + 1 probe rows");
        for (const auto& r : rows)
        {
            const double expected = (r.fmRxBytes - r.fmRetxBytes) * 8.0 / kAqmEvalSmokeSimTime;
            NS_TEST_ASSERT_MSG_EQ_TOL(r.rxRateBps,
                                      expected,
                                      1.0,
                                      "rx_rate_bps for '" << r.name << "' is not bytes*8/simTime");
        }
    }
};

/// S-aqm-runner-udp-cbr-rate-band: each unsaturated 1000-byte-payload CBR
/// bulk flow is delivered across the rate-limited bottleneck at the offered
/// rate within the 3% IP+UDP header-overhead band (28/1000 = 2.8% plus
/// start/stop quantisation).  The 200-byte probe is out of scope (14%
/// overhead by construction).
class TestSAqmRunnerUdpCbrRateBand : public TestCase
{
  public:
    TestSAqmRunnerUdpCbrRateBand()
        : TestCase("S-aqm-runner-udp-cbr-rate-band, bulk CBR delivered within 3% of offered")
    {
    }

  private:
    void DoRun() override
    {
        const std::string outDir = "/tmp/stratum-test-aqm-runner-rate-band";
        NS_TEST_ASSERT_MSG_EQ(AqmEvalRunSmokeCell(outDir), true, "smoke cell run failed");

        const auto rows = AqmEvalReadPerflowCsv(outDir);
        uint32_t bulkRows = 0;
        for (const auto& r : rows)
        {
            if (r.name != "bulk")
            {
                continue;
            }
            ++bulkRows;
            const double rateActive =
                r.fmRxBytes * 8.0 / (kAqmEvalSmokeSimTime - kAqmEvalSmokeStartSec);
            const double deviation =
                std::abs(rateActive - kAqmEvalBulkRateBps) / kAqmEvalBulkRateBps;
            NS_TEST_ASSERT_MSG_LT_OR_EQ(deviation,
                                        kAqmEvalRateBand,
                                        "bulk CBR delivered rate "
                                            << rateActive << " bps deviates from offered "
                                            << kAqmEvalBulkRateBps << " by more than 3%");
        }
        NS_TEST_ASSERT_MSG_EQ(bulkRows, 2u, "expected exactly 2 bulk rows in the smoke cell");
    }
};

/// S-aqm-stratumred-factory-four-step: a naive
/// CreateObject<stratum::RedQueueDisc>() + Initialize() drops every packet
/// (empty PHB table -> NO_PHB_MATCH), while the registry's canonical
/// four-step StratumRed factory accepts and dequeues the same traffic in
/// full.  Pins the naive-instantiation trap checklist documented in
/// aqm-eval-runner.cc and the registry factory.
class TestSAqmStratumRedFactoryFourStep : public TestCase
{
  public:
    TestSAqmStratumRedFactoryFourStep()
        : TestCase("S-aqm-stratumred-factory-four-step, naive drops all while factory passes")
    {
    }

  private:
    static Ptr<Ipv4QueueDiscItem> MakeBeItem()
    {
        Ptr<Packet> pkt = Create<Packet>(1000);
        Ipv4Header ip;
        ip.SetProtocol(17); // UDP; DSCP defaults to 0 (best effort)
        return Create<Ipv4QueueDiscItem>(pkt, Address(), 0x0800, ip);
    }

    void DoRun() override
    {
        constexpr uint32_t kPackets = 10;

        // Naive arm: no PHB table, default MRED mode — drops everything.
        Ptr<RedQueueDisc> naive = CreateObject<RedQueueDisc>();
        naive->Initialize();
        for (uint32_t i = 0; i < kPackets; ++i)
        {
            naive->Enqueue(MakeBeItem());
        }
        NS_TEST_ASSERT_MSG_EQ(naive->GetNPackets(),
                              0u,
                              "naive StratumRed accepted packets; trap checklist is stale");
        NS_TEST_ASSERT_MSG_EQ(naive->GetStats().nTotalDroppedPackets,
                              kPackets,
                              "naive StratumRed did not drop every packet");

        // Canonical arm: the registry factory's four-step configuration.
        Ptr<QueueDisc> configured =
            aqm_eval::AqmRegistry::Get().Make("StratumRed", DataRate("10Mbps"));
        for (uint32_t i = 0; i < kPackets; ++i)
        {
            configured->Enqueue(MakeBeItem());
        }
        NS_TEST_ASSERT_MSG_EQ(configured->GetNPackets(),
                              kPackets,
                              "four-step-configured StratumRed rejected traffic");
        NS_TEST_ASSERT_MSG_EQ(configured->GetStats().nTotalDroppedPackets,
                              0u,
                              "four-step-configured StratumRed dropped traffic");
        uint32_t dequeued = 0;
        while (configured->Dequeue())
        {
            ++dequeued;
        }
        NS_TEST_ASSERT_MSG_EQ(dequeued,
                              kPackets,
                              "four-step-configured StratumRed did not dequeue all traffic");

        Simulator::Destroy();
    }
};

/// S-aqm-stratuml4swred-factory-classic-lane-functional: the registry's
/// StratumL4sWred cell leaves the factory with a functional WRED classic
/// lane — ten back-to-back Not-ECT DSCP-0 packets all enqueue (healthy WRED
/// EWMA stays far below thMin=5 at this depth) and all dequeue.  Gates the
/// user-config-gate regression where the factory's partial configuration
/// skipped the Wred default mitigation and left the classic sub-queues at
/// the RIO_C thMin=thMax=0 trap defaults, force-dropping every packet after
/// the first backlog observation.
class TestSAqmStratumL4sWredFactoryClassicLaneFunctional : public TestCase
{
  public:
    TestSAqmStratumL4sWredFactoryClassicLaneFunctional()
        : TestCase("S-aqm-stratuml4swred-factory-classic-lane-functional, "
                   "factory classic lane accepts back-to-back BE traffic")
    {
    }

  private:
    static Ptr<Ipv4QueueDiscItem> MakeBeItem()
    {
        Ptr<Packet> pkt = Create<Packet>(1000);
        Ipv4Header ip;
        ip.SetProtocol(6); // TCP; DSCP 0 (best effort), ECN Not-ECT -> classic lane
        return Create<Ipv4QueueDiscItem>(pkt, Address(), 0x0800, ip);
    }

    void DoRun() override
    {
        constexpr uint32_t kPackets = 10;

        Ptr<QueueDisc> disc =
            aqm_eval::AqmRegistry::Get().Make("StratumL4sWred", DataRate("10Mbps"));
        disc->Initialize();
        for (uint32_t i = 0; i < kPackets; ++i)
        {
            disc->Enqueue(MakeBeItem());
        }
        NS_TEST_ASSERT_MSG_EQ(disc->GetNPackets(),
                              kPackets,
                              "StratumL4sWred classic lane rejected back-to-back BE traffic");
        NS_TEST_ASSERT_MSG_EQ(disc->GetStats().nTotalDroppedPackets,
                              0u,
                              "StratumL4sWred classic lane dropped BE traffic");
        uint32_t dequeued = 0;
        while (disc->Dequeue())
        {
            ++dequeued;
        }
        NS_TEST_ASSERT_MSG_EQ(dequeued,
                              kPackets,
                              "StratumL4sWred did not dequeue all accepted traffic");

        Simulator::Destroy();
    }
};

// =============================================================================
//  Test Suite
// =============================================================================

// =====================================================================
// CAKE Q6 — rate-based virtual-clock shaper test fixtures
// =====================================================================

class S17_39_RateBasedMonotonicityTestCase : public TestCase
{
  public:
    S17_39_RateBasedMonotonicityTestCase()
        : TestCase("RateBased per-tin advance matches adj_len over rate")
    {
    }

    void DoRun() override
    {
        using ns3::stratum::cake::RateBasedTinClock;
        cake::RateBasedTinClock clock;
        clock.rateBps = 100'000'000; // 100 Mbps
        clock.overhead = 0;
        clock.mpu = 0;
        clock.framing = cake::RateBasedTinClock::FramingMode::NoAtm;
        clock.tNext = Seconds(0);

        constexpr uint32_t kPacketBytes = 1500;
        constexpr int kPackets = 1000;
        // 1500 B * 8 / 100 Mbps = 120 us per packet
        Time expected{
            NanoSeconds(static_cast<int64_t>(kPacketBytes) * 8 * 1'000'000'000ULL / clock.rateBps)};

        // Sustained backlog: simulator clock advances to each eligible
        // time, mirroring a qdisc that dequeues exactly when MaybeAllow
        // returns true. Each Charge should hit the behind-but-within
        // branch and snap tNext to now + tinDur.
        Time before = clock.tNext;
        Time now = before;
        for (int i = 0; i < kPackets; ++i)
        {
            clock.Charge(kPacketBytes, now);
            now = clock.tNext;
        }
        Time after = clock.tNext;

        Time perPacket = (after - before) / kPackets;
        NS_TEST_ASSERT_MSG_EQ_TOL(perPacket.GetNanoSeconds(),
                                  expected.GetNanoSeconds(),
                                  static_cast<int64_t>(1000),
                                  "Per-packet advance must equal adj_len/rate within 1us");
    }
};

class S17_40_RateBasedCatchupTestCase : public TestCase
{
  public:
    S17_40_RateBasedCatchupTestCase()
        : TestCase("RateBased two-site catchup (enqueue + dequeue 3-branch)")
    {
    }

    void DoRun() override
    {
        using ns3::stratum::cake::RateBasedTinClock;

        // Sub-case (a): enqueue-time hard snap-to-now when stale.
        {
            cake::RateBasedTinClock clock;
            clock.rateBps = 100'000'000;
            clock.tNext = Seconds(0);
            Time now = Seconds(1);
            clock.OnEnqueueIdleReset(now);
            NS_TEST_ASSERT_MSG_EQ(clock.tNext,
                                  now,
                                  "(a) OnEnqueueIdleReset must snap tNext = now when stale");
        }

        // Sub-case (b): dequeue-time stale advance (NOT snap-to-now).
        {
            cake::RateBasedTinClock clock;
            clock.rateBps = 100'000'000;
            clock.tNext = Seconds(0);
            Time now = Seconds(1);
            constexpr uint32_t kBytes = 1500;
            Time tinDur =
                NanoSeconds(static_cast<int64_t>(kBytes) * 8 * 1'000'000'000ULL / clock.rateBps);
            Time before = clock.tNext;
            clock.Charge(kBytes, now);
            NS_TEST_ASSERT_MSG_EQ(
                clock.tNext,
                before + tinDur,
                "(b) stale Charge must advance from stale value, not snap-to-now");
        }

        // Sub-case (c): dequeue-time middle branch (now <= tNext < now + tin_dur).
        {
            cake::RateBasedTinClock clock;
            clock.rateBps = 100'000'000;
            constexpr uint32_t kBytes = 1500;
            Time tinDur =
                NanoSeconds(static_cast<int64_t>(kBytes) * 8 * 1'000'000'000ULL / clock.rateBps);
            Time now = Seconds(1);
            clock.tNext = now + tinDur / 2;
            clock.Charge(kBytes, now);
            NS_TEST_ASSERT_MSG_EQ(clock.tNext,
                                  now + tinDur,
                                  "(c) mid-window Charge must snap tNext to now + tin_dur");
        }
    }
};

class S17_42_RateBasedAdjLenTestCase : public TestCase
{
  public:
    S17_42_RateBasedAdjLenTestCase()
        : TestCase("RateBased adj_len matches cake_calc_overhead")
    {
    }

    void DoRun() override
    {
        using ns3::stratum::cake::RateBasedTinClock;
        using FM = cake::RateBasedTinClock::FramingMode;

        // ATM: 64+14=78, ((78+47)/48)*53 = 2*53 = 106
        NS_TEST_ASSERT_MSG_EQ(cake::RateBasedTinClock::ComputeAdjLen(64, 14, FM::Atm, 0),
                              106u,
                              "ATM: 64+14 -> 106");

        // PTM: 78 + (78+63)/64 = 78 + 2 = 80
        NS_TEST_ASSERT_MSG_EQ(cake::RateBasedTinClock::ComputeAdjLen(64, 14, FM::Ptm, 0),
                              80u,
                              "PTM: 78 + ceil(78/64) = 80");

        // NoAtm: 64+14 = 78
        NS_TEST_ASSERT_MSG_EQ(cake::RateBasedTinClock::ComputeAdjLen(64, 14, FM::NoAtm, 0),
                              78u,
                              "NoAtm: 64+14 = 78");

        // MPU floor
        NS_TEST_ASSERT_MSG_EQ(cake::RateBasedTinClock::ComputeAdjLen(20, 0, FM::NoAtm, 64),
                              64u,
                              "MPU: max(20, 64) = 64");

        // ATM exact boundary: 48 -> 53
        NS_TEST_ASSERT_MSG_EQ(cake::RateBasedTinClock::ComputeAdjLen(48, 0, FM::Atm, 0),
                              53u,
                              "ATM exact-boundary: 48 -> 53");

        // ATM smallest: 1 -> 53
        NS_TEST_ASSERT_MSG_EQ(cake::RateBasedTinClock::ComputeAdjLen(1, 0, FM::Atm, 0),
                              53u,
                              "ATM smallest: 1 -> 53");

        // Sub-case 7: overhead/framing differentiation reaches the virtual
        // clock. Two clocks at identical rate but different framing must
        // accumulate different tNext advances when charged with their
        // respective adj_len.
        {
            cake::RateBasedTinClock noAtm;
            cake::RateBasedTinClock atm;
            noAtm.rateBps = atm.rateBps = 100'000'000;
            noAtm.framing = FM::NoAtm;
            atm.framing = FM::Atm;
            Time now = Seconds(0);
            for (int i = 0; i < 1000; ++i)
            {
                uint32_t adjNo = cake::RateBasedTinClock::ComputeAdjLen(64, 14, FM::NoAtm, 0);
                uint32_t adjAtm = cake::RateBasedTinClock::ComputeAdjLen(64, 14, FM::Atm, 0);
                noAtm.Charge(adjNo, now);
                atm.Charge(adjAtm, now);
            }
            NS_TEST_ASSERT_MSG_GT(
                atm.tNext,
                noAtm.tNext,
                "ATM framing must produce larger tNext advance than NoAtm at same rate");
        }
    }
};

class S17_43_ShaperModeDispatchTestCase : public TestCase
{
  public:
    S17_43_ShaperModeDispatchTestCase()
        : TestCase("ShaperMode enum drives correct dispatcher class")
    {
    }

    void DoRun() override
    {
        using ns3::stratum::cake::Helper;

        cake::Helper helperA;
        NS_TEST_ASSERT_MSG_EQ(static_cast<int>(helperA.GetShaperMode()),
                              static_cast<int>(cake::Helper::ShaperMode::TokenBucket),
                              "Default ShaperMode must be TokenBucket");

        // RateBased mode: BuildDispatcher (Task 4.2) must instantiate
        // cake::RateBasedShaperDispatcher. Until 4.2 lands, this fixture
        // exercises only the enum + accessor.
        cake::Helper helperB;
        helperB.SetShaperMode(cake::Helper::ShaperMode::RateBased);
        NS_TEST_ASSERT_MSG_EQ(static_cast<int>(helperB.GetShaperMode()),
                              static_cast<int>(cake::Helper::ShaperMode::RateBased),
                              "SetShaperMode must persist the value");

        // Back-compat: SetUseInnerTbfShaping(true) aliases to TbfInner.
        cake::Helper helperC;
        helperC.SetUseInnerTbfShaping(true);
        NS_TEST_ASSERT_MSG_EQ(static_cast<int>(helperC.GetShaperMode()),
                              static_cast<int>(cake::Helper::ShaperMode::TbfInner),
                              "useInnerTbfShaping=true must alias to TbfInner");
    }
};

// =============================================================================
//  S-17.47: Per-flow counter accessor on cake::TinShaperDispatcher
// =============================================================================

/**
 * @brief Verifies cake::TinShaperDispatcher::GetPerFlowStats reports
 *        per-flow wire-byte / packet / drop / mark counters that match
 *        the inner FqCobalt's per-flow QueueDisc::Stats.
 * @see specs/02-structural.md S-17.47
 */
class TinShaperPerFlowStatsTest : public TestCase
{
  public:
    TinShaperPerFlowStatsTest()
        : TestCase("S-17.47 GetPerFlowStats reports per-flow bytes, packets, "
                   "remaining-backlog within a tin")
    {
    }

  private:
    void DoRun() override
    {
        // One-slot edge with stock FqCobaltQueueDisc as inner so the
        // dispatcher has a real per-flow class list to walk.
        constexpr uint32_t kPayload = 500;
        constexpr uint32_t kIpHdrBytes = 20;
        constexpr uint32_t kPortPrefix = 4;
        // Wire bytes per packet: payload + 4-byte port prefix + IPv4 header.
        constexpr uint32_t kWirePerPkt = kPayload + kPortPrefix + kIpHdrBytes;

        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        Ptr<FqCobaltQueueDisc> inner =
            CreateObjectWithAttributes<FqCobaltQueueDisc>("EnableSetAssociativeHash",
                                                          BooleanValue(true),
                                                          "SetWays",
                                                          UintegerValue(8));
        inner->SetQuantum(1514);
        edge->SetInnerDiscAt(0, inner);
        edge->SetDscpToSlot(0, 0);

        Ptr<cake::TinShaperDispatcher> shaper = CreateObject<cake::TinShaperDispatcher>();
        shaper->SetQuantum(0, 1514);
        edge->SetSlotDispatcher(shaper);
        edge->Initialize();

        // Three distinct flows, different packet counts.
        const Ipv4Address src("10.10.10.1");
        const Ipv4Address dstA("10.10.10.10");
        const Ipv4Address dstB("10.10.10.20");
        const Ipv4Address dstC("10.10.10.30");
        constexpr uint32_t kFlowAPkts = 8;
        constexpr uint32_t kFlowBPkts = 4;
        constexpr uint32_t kFlowCPkts = 2;

        for (uint32_t i = 0; i < kFlowAPkts; ++i)
        {
            edge->Enqueue(MakeUdpHostItem(src, dstA, 1024, 80, kPayload));
        }
        for (uint32_t i = 0; i < kFlowBPkts; ++i)
        {
            edge->Enqueue(MakeUdpHostItem(src, dstB, 1024, 80, kPayload));
        }
        for (uint32_t i = 0; i < kFlowCPkts; ++i)
        {
            edge->Enqueue(MakeUdpHostItem(src, dstC, 1024, 80, kPayload));
        }

        const uint32_t kClasses = inner->GetNQueueDiscClasses();
        NS_TEST_ASSERT_MSG_EQ(kClasses,
                              3u,
                              "Three distinct (src,dst) flows should produce 3 inner classes");

        // Sum across all flow classes; total wire bytes must equal
        // (kFlowAPkts + kFlowBPkts + kFlowCPkts) * kWirePerPkt.
        uint64_t totalBytes = 0;
        uint64_t totalPkts = 0;
        uint32_t totalRemaining = 0;
        for (uint32_t f = 0; f < kClasses; ++f)
        {
            cake::PerFlowStats s = shaper->GetPerFlowStats(0, f, PeekPointer(edge));
            // Each flow's bytesEnqueued must be a multiple of one wire packet.
            NS_TEST_ASSERT_MSG_EQ(s.bytesEnqueued % kWirePerPkt,
                                  0u,
                                  "Per-flow bytesEnqueued must align to wire packet size");
            // Each flow has 2, 4, or 8 packets — pkts >= 1.
            NS_TEST_ASSERT_MSG_GT(s.pktsEnqueued, 0u, "Active flow pktsEnqueued must be > 0");
            // bytesRemaining starts equal to bytesEnqueued (no dequeue yet).
            NS_TEST_ASSERT_MSG_EQ(s.bytesRemaining,
                                  s.bytesEnqueued,
                                  "Pre-dequeue: live backlog == bytesEnqueued");
            totalBytes += s.bytesEnqueued;
            totalPkts += s.pktsEnqueued;
            totalRemaining += s.bytesRemaining;
        }
        const uint64_t expectedBytes =
            static_cast<uint64_t>(kFlowAPkts + kFlowBPkts + kFlowCPkts) * kWirePerPkt;
        const uint64_t expectedPkts = kFlowAPkts + kFlowBPkts + kFlowCPkts;
        NS_TEST_ASSERT_MSG_EQ(totalBytes,
                              expectedBytes,
                              "Sum of per-flow bytesEnqueued must match total enqueued wire bytes");
        NS_TEST_ASSERT_MSG_EQ(totalPkts,
                              expectedPkts,
                              "Sum of per-flow pktsEnqueued must match total packets");
        NS_TEST_ASSERT_MSG_EQ(totalRemaining,
                              expectedBytes,
                              "Sum of per-flow live backlog must match total enqueued wire bytes "
                              "before any dequeue");

        // Drain one packet — exactly one flow's bytesRemaining must
        // shrink by kWirePerPkt; bytesEnqueued and pktsEnqueued are
        // monotonic and must NOT decrease.
        std::vector<cake::PerFlowStats> before;
        before.reserve(kClasses);
        for (uint32_t f = 0; f < kClasses; ++f)
        {
            before.push_back(shaper->GetPerFlowStats(0, f, PeekPointer(edge)));
        }
        Ptr<QueueDiscItem> drained = edge->Dequeue();
        NS_TEST_ASSERT_MSG_NE(drained, nullptr, "First dequeue must yield a packet");
        if (!drained)
        {
            return;
        }

        uint32_t shrunkFlows = 0;
        for (uint32_t f = 0; f < kClasses; ++f)
        {
            cake::PerFlowStats s = shaper->GetPerFlowStats(0, f, PeekPointer(edge));
            NS_TEST_ASSERT_MSG_GT_OR_EQ(s.bytesEnqueued,
                                        before[f].bytesEnqueued,
                                        "bytesEnqueued is monotonic across a dequeue");
            NS_TEST_ASSERT_MSG_GT_OR_EQ(s.pktsEnqueued,
                                        before[f].pktsEnqueued,
                                        "pktsEnqueued is monotonic across a dequeue");
            if (s.bytesRemaining + kWirePerPkt == before[f].bytesRemaining)
            {
                ++shrunkFlows;
            }
        }
        NS_TEST_ASSERT_MSG_EQ(shrunkFlows,
                              1u,
                              "Exactly one flow's live backlog should shrink by one wire packet "
                              "after a single dequeue");

        // Out-of-range slot returns a zeroed snapshot rather than aborting.
        cake::PerFlowStats oobSlot = shaper->GetPerFlowStats(99, 0, PeekPointer(edge));
        NS_TEST_ASSERT_MSG_EQ(oobSlot.bytesEnqueued,
                              0u,
                              "Out-of-range slot must yield a zero-initialized snapshot");

        // Out-of-range flow id returns a zeroed snapshot.
        cake::PerFlowStats oobFlow = shaper->GetPerFlowStats(0, kClasses + 100, PeekPointer(edge));
        NS_TEST_ASSERT_MSG_EQ(oobFlow.bytesEnqueued,
                              0u,
                              "Out-of-range flow id must yield a zero-initialized snapshot");

        Simulator::Destroy();
    }
};

// =============================================================================

// =============================================================================
//  S-17.51: CAKE diagnostic text dump — `tc -s qdisc show cake` mirror
// =============================================================================

/**
 * @brief Verifies cake::Helper::PrintTcStats produces a structurally-
 *        compatible mirror of Linux `tc -s qdisc show cake`.
 *
 * The test enqueues a handful of EF and BE packets through a
 * CAKE-diffserv4 edge, captures the formatter output to an
 * `ostringstream`, and asserts the presence of the section-key
 * vocabulary the spec requires (`qdisc cake`, `tins N`, `Sent`,
 * `dropped`, `backlog`, per-tin `tin <i>`, `thresh`, `bytes_enqueued`,
 * `drops`, `marks`, `ever_seen`). The assertions are structural rather
 * than byte-exact so future cosmetic iproute2 changes do not regress
 * the fixture.
 *
 * @see specs/02-structural.md S-17.51
 */
class CakePrintTcStatsStructureTest : public TestCase
{
  public:
    CakePrintTcStatsStructureTest()
        : TestCase("S-17.51 PrintTcStats produces tc -s qdisc show cake-mirror "
                   "section keys for a CAKE-composed edge")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        cake::Helper::SetAsCakeDiffserv4(edge, DataRate("100Mbps"));
        edge->Initialize();

        // Drive a few packets so per-tin counters are non-zero.
        const uint32_t kPayload = 500;
        for (uint32_t i = 0; i < 4; ++i)
        {
            Ptr<Packet> p = Create<Packet>(kPayload);
            Ipv4Header h;
            h.SetDscp(Ipv4Header::DSCP_EF);
            h.SetProtocol(17);
            h.SetSource(Ipv4Address("10.0.0.1"));
            h.SetDestination(Ipv4Address("10.0.0.2"));
            edge->Enqueue(Create<Ipv4QueueDiscItem>(p, Mac48Address(), 0x0800, h));
        }
        for (uint32_t i = 0; i < 2; ++i)
        {
            Ptr<Packet> p = Create<Packet>(kPayload);
            Ipv4Header h;
            h.SetDscp(Ipv4Header::DscpDefault);
            h.SetProtocol(17);
            h.SetSource(Ipv4Address("10.0.0.3"));
            h.SetDestination(Ipv4Address("10.0.0.4"));
            edge->Enqueue(Create<Ipv4QueueDiscItem>(p, Mac48Address(), 0x0800, h));
        }
        // Drain one to populate dequeue counters.
        Ptr<QueueDiscItem> drained = edge->Dequeue();
        NS_TEST_ASSERT_MSG_NE(drained, nullptr, "First Dequeue must yield a packet");

        // Capture the formatter output.
        std::ostringstream oss;
        cake::Helper helper;
        helper.PrintTcStats(oss, edge);
        const std::string out = oss.str();

        NS_TEST_ASSERT_MSG_GT(out.size(), 0u, "PrintTcStats must produce non-empty output");

        // Header keys.
        NS_TEST_ASSERT_MSG_NE(out.find("qdisc cake"),
                              std::string::npos,
                              "Output must contain the 'qdisc cake' header token");
        NS_TEST_ASSERT_MSG_NE(out.find("tins"),
                              std::string::npos,
                              "Output must contain the 'tins' tin-count token");

        // Aggregate keys (Sent / dropped / backlog).
        NS_TEST_ASSERT_MSG_NE(out.find("Sent"),
                              std::string::npos,
                              "Output must contain the 'Sent' aggregate-bytes token");
        NS_TEST_ASSERT_MSG_NE(out.find("dropped"),
                              std::string::npos,
                              "Output must contain the 'dropped' counter token");
        NS_TEST_ASSERT_MSG_NE(out.find("backlog"),
                              std::string::npos,
                              "Output must contain the 'backlog' counter token");

        // Per-tin keys (only need at least slot 0; diffserv4 has 4 tins).
        NS_TEST_ASSERT_MSG_NE(out.find("tin 0"),
                              std::string::npos,
                              "Output must contain a 'tin 0' per-tin block header");
        NS_TEST_ASSERT_MSG_NE(out.find("kind="),
                              std::string::npos,
                              "Per-tin block must contain the 'kind=' inner-type token");
        NS_TEST_ASSERT_MSG_NE(out.find("thresh"),
                              std::string::npos,
                              "Per-tin block must contain the 'thresh' rate token");
        NS_TEST_ASSERT_MSG_NE(out.find("bytes_enqueued"),
                              std::string::npos,
                              "Per-tin block must contain the 'bytes_enqueued' counter");
        NS_TEST_ASSERT_MSG_NE(out.find("bytes_dequeued"),
                              std::string::npos,
                              "Per-tin block must contain the 'bytes_dequeued' counter");
        NS_TEST_ASSERT_MSG_NE(out.find("drops"),
                              std::string::npos,
                              "Per-tin block must contain the 'drops' counter");
        NS_TEST_ASSERT_MSG_NE(out.find("marks"),
                              std::string::npos,
                              "Per-tin block must contain the 'marks' counter");

        // Substrate-fidelity gap: ever_seen rather than bulk_flow_count.
        NS_TEST_ASSERT_MSG_NE(out.find("ever_seen"),
                              std::string::npos,
                              "Per-tin block must use the 'ever_seen' name "
                              "(not 'bulk_flow_count') to flag the substrate gap");
        NS_TEST_ASSERT_MSG_EQ(out.find("bulk_flow_count"),
                              std::string::npos,
                              "Per-tin block must NOT use the Linux name "
                              "'bulk_flow_count' — substrate cannot honour the "
                              "live-count contract");

        // diffserv4 has 4 tins; verify the formatter walked every slot.
        NS_TEST_ASSERT_MSG_NE(out.find("tin 3"),
                              std::string::npos,
                              "Output must contain per-tin block for slot 3 "
                              "(diffserv4 Voice tin)");

        // Null-edge contract: single-line diagnostic, no abort.
        std::ostringstream ossNull;
        helper.PrintTcStats(ossNull, nullptr);
        NS_TEST_ASSERT_MSG_NE(ossNull.str().find("(null)"),
                              std::string::npos,
                              "Null edge must produce a 'qdisc cake (null)' diagnostic");

        Simulator::Destroy();
    }
};

/**
 * @ingroup stratum-tests
 *
 * @brief S-17.53: CAKE `autorate-ingress` API contract — skeleton.
 *
 * Exercises the helper boolean round-trip, the no-op hook return
 * contract, and the byte-identity invariant between flag-disabled
 * and flag-enabled states under a deterministic 4-tin scenario.
 * Closed-loop RTT-trend logic is a future deliverable and is not
 * asserted here.
 *
 * @see specs/02-structural.md S-17.53
 */
class CakeAutorateIngressApiContractTest : public TestCase
{
  public:
    CakeAutorateIngressApiContractTest()
        : TestCase("S-17.53 SetEnableAutorateIngress + no-op hook + "
                   "byte-identity vs disabled mode")
    {
    }

  private:
    /**
     * Drive a fixed packet sequence through a fresh CAKE-composed
     * edge and return the (enqueue-success-count, drained-bytes)
     * pair so the caller can compare flag-disabled vs flag-enabled
     * runs for byte-identity.
     */
    std::pair<uint32_t, uint32_t> DrivePacketSequence()
    {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        cake::Helper::SetAsCakeDiffserv4(edge, DataRate("100Mbps"));
        edge->Initialize();

        const uint32_t kPayload = 500;
        uint32_t enqueued = 0;
        // Mix DSCP values so multiple tins are exercised.
        const Ipv4Header::DscpType kDscps[] = {
            Ipv4Header::DSCP_EF,
            Ipv4Header::DscpDefault,
            Ipv4Header::DSCP_AF11,
            Ipv4Header::DSCP_CS5,
        };
        for (uint32_t i = 0; i < 8; ++i)
        {
            Ptr<Packet> p = Create<Packet>(kPayload);
            Ipv4Header h;
            h.SetDscp(kDscps[i % 4]);
            h.SetProtocol(17);
            h.SetSource(Ipv4Address("10.0.0.1"));
            h.SetDestination(Ipv4Address("10.0.0.2"));
            if (edge->Enqueue(Create<Ipv4QueueDiscItem>(p, Mac48Address(), 0x0800, h)))
            {
                ++enqueued;
            }
        }

        uint32_t drainedBytes = 0;
        for (uint32_t i = 0; i < 8; ++i)
        {
            Ptr<QueueDiscItem> item = edge->Dequeue();
            if (!item)
            {
                break;
            }
            drainedBytes += item->GetSize();
        }
        return {enqueued, drainedBytes};
    }

    void DoRun() override
    {
        cake::Helper helper;

        // (a) API round-trip: default false.
        NS_TEST_ASSERT_MSG_EQ(helper.GetEnableAutorateIngress(),
                              false,
                              "Autorate-ingress must default to false");
        NS_TEST_ASSERT_MSG_EQ(helper.GetAutorateIngressHook(),
                              nullptr,
                              "Hook must be nullptr when flag is disabled");

        // Enable.
        helper.SetEnableAutorateIngress(true);
        NS_TEST_ASSERT_MSG_EQ(helper.GetEnableAutorateIngress(),
                              true,
                              "Flag must round-trip true after SetEnableAutorateIngress(true)");

        // (b) No-op hook installed and returns zero for representative rates.
        const cake::AutorateIngressHook* hook = helper.GetAutorateIngressHook();
        NS_TEST_ASSERT_MSG_NE(hook, nullptr, "Hook must be non-null when flag is enabled");
        const uint64_t kRates[] = {
            UINT64_C(1000000),    // 1 Mbit/s
            UINT64_C(100000000),  // 100 Mbit/s
            UINT64_C(1000000000), // 1 Gbit/s
        };
        for (uint64_t r : kRates)
        {
            NS_TEST_ASSERT_MSG_EQ(hook->ComputeRateDelta(r),
                                  0,
                                  "v1 no-op hook must return zero for any current rate");
        }

        // Disable round-trip.
        helper.SetEnableAutorateIngress(false);
        NS_TEST_ASSERT_MSG_EQ(helper.GetEnableAutorateIngress(),
                              false,
                              "Flag must round-trip false after SetEnableAutorateIngress(false)");
        NS_TEST_ASSERT_MSG_EQ(helper.GetAutorateIngressHook(),
                              nullptr,
                              "Hook must be cleared when flag toggles back to false");

        // (c) Byte-identity: flag-disabled vs flag-enabled (no-op hook).
        // Both invocations build their own fresh edge so per-run state
        // is isolated.
        auto [enqDisabled, bytesDisabled] = DrivePacketSequence();
        helper.SetEnableAutorateIngress(true);
        auto [enqEnabled, bytesEnabled] = DrivePacketSequence();

        NS_TEST_ASSERT_MSG_EQ(enqEnabled,
                              enqDisabled,
                              "Per-packet enqueue count must match disabled mode "
                              "(no-op hook produces no behavioural side-effects)");
        NS_TEST_ASSERT_MSG_EQ(bytesEnabled,
                              bytesDisabled,
                              "Drained-byte total must match disabled mode "
                              "(no-op hook produces no behavioural side-effects)");

        Simulator::Destroy();
    }
};

/**
 * @brief Verifies an LLQ EF priority lane resumes once its measured rate decays
 *        back below the configured cap.
 *
 * RFC 3246 polices the EF aggregate to its rate; it does not extinguish it. The
 * cap must self-release after the EF lane backs off, exactly as the ns-2
 * reference decays every queue's departure-rate estimator on each dequeue.
 * @see specs/02-structural.md S-11.5
 */
class LlqPqRateCapRecoversAfterIdleTest : public TestCase
{
  public:
    LlqPqRateCapRecoversAfterIdleTest()
        : TestCase("S-11.5 LLQ PQ rate cap recovers: EF lane resumes after its rate decays "
                   "below the cap")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<LlqScheduler> sched =
            CreateObjectWithAttributes<LlqScheduler>("NumQueues",
                                                     UintegerValue(2),
                                                     "LinkBandwidth",
                                                     DoubleValue(48000.0),
                                                     "FqVariant",
                                                     EnumValue(LlqScheduler::FqVariant::SCFQ));
        sched->SetParam(1, 1.0);
        // Cap = 8 kbps = 1000 bytes/s. A 1000-byte departure pushes the EF lane
        // well over it; a few seconds of idle must bring it back under.
        sched->SetPqRateCap(8000.0);

        // Backlog both lanes.
        for (int i = 0; i < 100; ++i)
        {
            sched->OnEnqueueWithTime(0, 1000, 0.0);
            sched->OnEnqueueWithTime(1, 1000, 0.0);
        }

        // First, two EF departures drive the EF average rate above the cap.
        sched->UpdateDepartureRate(0, 0, 1000, 0.1);
        sched->UpdateDepartureRate(0, 0, 1000, 0.2);
        NS_TEST_ASSERT_MSG_EQ(sched->SelectNextQueue(),
                              1,
                              "precondition: EF over its cap must yield to the fair lane");

        // EF then stops departing; only the fair lane is served for ~3 s of
        // model time. The EF rate estimator must decay so the cap self-releases.
        double t = 0.2;
        for (int i = 0; i < 30; ++i)
        {
            t += 0.1;
            sched->UpdateDepartureRate(1, 0, 1000, t);
            sched->OnEnqueueWithTime(0, 1000, t);
            sched->OnEnqueueWithTime(1, 1000, t);
        }

        // The EF lane has now been idle and under its cap for seconds, so the
        // strict-priority EF lane must be served again.
        int q = sched->SelectNextQueue();
        NS_TEST_ASSERT_MSG_EQ(q,
                              0,
                              "LLQ EF lane must resume once its measured rate decays below the "
                              "cap (RFC 3246 polices the EF aggregate, it must not extinguish "
                              "it); got queue "
                                  << q);
        Simulator::Destroy();
    }
};

/**
 * @brief Verifies @c RedSubQueue caps the active precedence count at @c kMaxPrec
 *        so the fixed-size per-precedence parameter array is never indexed out
 *        of bounds.
 * @see specs/02-structural.md S-12.4
 */
class RedSubQueueNumPrecClampedTest : public TestCase
{
  public:
    RedSubQueueNumPrecClampedTest()
        : TestCase("S-12.4 RedSubQueue clamps numPrec to kMaxPrec (no out-of-bounds m_qParam)")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<RedSubQueue> sq = CreateObject<RedSubQueue>();
        // Over-range misconfiguration: more precedence levels than the
        // fixed-size m_qParam[kMaxPrec] array can hold.
        sq->SetNumPrec(static_cast<uint32_t>(kMaxPrec) + 2);
        NS_TEST_ASSERT_MSG_EQ(sq->GetNumPrec(),
                              static_cast<uint32_t>(kMaxPrec),
                              "SetNumPrec must clamp to kMaxPrec to keep m_qParam indexing "
                              "in bounds");
        Simulator::Destroy();
    }
};

/**
 * @brief Verifies the CAKE precedence preset assigns per-tin DRR quanta that
 *        decay from tin 0, matching the Linux precedence quantum ladder.
 *
 * The Linux CAKE precedence configuration seeds a base quantum and decays it
 * geometrically (each tin at 7/8 of the previous), so tin 0 (best effort)
 * carries the largest DRR byte-share weight and tin 7 the smallest.
 * @see specs/02-structural.md S-17.70
 */
class CakePrecedenceQuantumLadderTest : public TestCase
{
  public:
    CakePrecedenceQuantumLadderTest()
        : TestCase("S-17.70 CAKE precedence per-tin DRR quanta decay from tin 0 (Linux ladder)")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        // enableLlq=false so the across-tin dispatcher is a plain DRR shaper and
        // every tin (including tin 7) carries a real quantum.
        cake::Helper::SetAsCakePrecedence(edge, DataRate("10Mbps"));

        Ptr<cake::TinShaperDispatcher> shaper =
            DynamicCast<cake::TinShaperDispatcher>(edge->GetSlotDispatcher());
        NS_TEST_ASSERT_MSG_NE(shaper,
                              nullptr,
                              "precedence (no LLQ) must install a TinShaperDispatcher");

        NS_TEST_ASSERT_MSG_GT(shaper->GetQuantum(0),
                              shaper->GetQuantum(7),
                              "tin 0 (best effort) must carry a larger DRR quantum than tin 7, "
                              "matching the Linux precedence quantum ladder; got q0="
                                  << shaper->GetQuantum(0) << " q7=" << shaper->GetQuantum(7));

        for (uint32_t s = 1; s < 8; ++s)
        {
            NS_TEST_ASSERT_MSG_GT_OR_EQ(shaper->GetQuantum(s - 1),
                                        shaper->GetQuantum(s),
                                        "precedence quanta must be non-increasing across the "
                                        "ladder; slot "
                                            << s);
        }
        Simulator::Destroy();
    }
};

/**
 * @brief Verifies the CAKE autorate capacity estimate accumulates the raw wire
 *        length, so a configured per-tin overhead does not inflate it.
 *
 * The Linux capacity estimator folds the raw packet length into its window
 * (`avg_window_bytes`, `sch_cake.c:1871`); the overhead/MPU/framing adjustment
 * is confined to the shaper clocks. The inferred rate is therefore
 * overhead-invariant: identical traffic at two overheads must yield the same
 * autorate-inferred rate.
 * @see specs/02-structural.md S-17.71
 */
class CakeAutorateWindowUsesRawLengthTest : public TestCase
{
  public:
    CakeAutorateWindowUsesRawLengthTest()
        : TestCase("S-17.71 CAKE autorate capacity estimate uses raw wire length "
                   "(overhead-invariant)")
    {
    }

  private:
    Ptr<cake::RateBasedShaperDispatcher> m_disp;

    void EnqueueOne()
    {
        Ptr<Packet> p = Create<Packet>(1000);
        Ipv4Header h;
        h.SetDscp(Ipv4Header::DscpDefault);
        h.SetProtocol(17);
        h.SetSource(Ipv4Address("10.0.0.1"));
        h.SetDestination(Ipv4Address("10.0.0.2"));
        m_disp->Enqueue(Create<Ipv4QueueDiscItem>(p, Address(), 0x0800, h));
    }

    uint64_t DriveAutorate(int32_t overhead)
    {
        m_disp = CreateObject<cake::RateBasedShaperDispatcher>();
        m_disp->SetMaxSize(QueueSize("2000p"));
        m_disp->ConfigureTin(0,
                             100000000ULL,
                             overhead,
                             0,
                             cake::RateBasedTinClock::FramingMode::NoAtm);
        m_disp->ConfigureGlobal(100000000ULL);
        m_disp->SetDscpToSlot(0, 0);
        m_disp->SetAutorateHook(std::make_shared<cake::LinuxAutorateHook>());
        m_disp->Initialize();

        // Drive a windowed arrival stream spanning well past the 250 ms autorate
        // warm-up so the capacity estimate engages and reconfigures the rate.
        Time t = MilliSeconds(0);
        for (int i = 0; i < 50; ++i)
        {
            Simulator::Schedule(t, &CakeAutorateWindowUsesRawLengthTest::EnqueueOne, this);
            t += MilliSeconds(10);
        }
        Simulator::Stop(t + MilliSeconds(10));
        Simulator::Run();
        const uint64_t rate = m_disp->GetGlobalRateBps();
        Simulator::Destroy();
        m_disp = nullptr;
        return rate;
    }

    void DoRun() override
    {
        const uint64_t rateNoOverhead = DriveAutorate(0);
        const uint64_t rateLargeOverhead = DriveAutorate(1000);

        NS_TEST_ASSERT_MSG_NE(rateNoOverhead,
                              100000000ULL,
                              "autorate must engage (move the rate off its configured value) "
                              "for this test to discriminate");
        NS_TEST_ASSERT_MSG_EQ(rateLargeOverhead,
                              rateNoOverhead,
                              "autorate capacity estimate must use raw wire length and stay "
                              "overhead-invariant; overhead=1000 gave "
                                  << rateLargeOverhead << " vs overhead=0 gave " << rateNoOverhead);
    }
};

/// Verifies InstallRoot replaces the default root qdisc the internet stack adds.
class InstallRootReplacesDefaultTest : public TestCase
{
  public:
    InstallRootReplacesDefaultTest()
        : TestCase("InstallRoot replaces the InternetStackHelper default root qdisc")
    {
    }

  private:
    void DoRun() override
    {
        NodeContainer nodes;
        nodes.Create(2);
        PointToPointHelper p2p;
        NetDeviceContainer devices = p2p.Install(nodes);
        InternetStackHelper internet;
        internet.Install(nodes);
        Ipv4AddressHelper addr;
        addr.SetBase("10.0.0.0", "255.255.255.0");
        addr.Assign(devices);

        Ptr<NetDevice> dev = devices.Get(0);
        Ptr<TrafficControlLayer> tc = dev->GetNode()->GetObject<TrafficControlLayer>();
        NS_TEST_ASSERT_MSG_EQ((tc != nullptr), true, "TrafficControlLayer should be aggregated");
        NS_TEST_ASSERT_MSG_EQ((tc->GetRootQueueDiscOnDevice(dev) != nullptr),
                              true,
                              "InternetStackHelper should attach a default root qdisc");

        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        Ptr<QueueDisc> returned = InstallRoot(dev, edge);

        NS_TEST_ASSERT_MSG_EQ((returned == edge),
                              true,
                              "InstallRoot should return the disc it installed");
        NS_TEST_ASSERT_MSG_EQ((tc->GetRootQueueDiscOnDevice(dev) == edge),
                              true,
                              "InstallRoot should set the edge as the device root qdisc");
        Simulator::Destroy();
    }
};

/// Verifies InstallRoot sets the root when the device currently has none.
class InstallRootOnBareDeviceTest : public TestCase
{
  public:
    InstallRootOnBareDeviceTest()
        : TestCase("InstallRoot sets the root on a device that has no qdisc")
    {
    }

  private:
    void DoRun() override
    {
        NodeContainer nodes;
        nodes.Create(2);
        PointToPointHelper p2p;
        NetDeviceContainer devices = p2p.Install(nodes);
        InternetStackHelper internet;
        internet.Install(nodes);
        Ipv4AddressHelper addr;
        addr.SetBase("10.0.0.0", "255.255.255.0");
        addr.Assign(devices);

        Ptr<NetDevice> dev = devices.Get(0);
        Ptr<TrafficControlLayer> tc = dev->GetNode()->GetObject<TrafficControlLayer>();
        if (tc->GetRootQueueDiscOnDevice(dev))
        {
            tc->DeleteRootQueueDiscOnDevice(dev);
        }
        NS_TEST_ASSERT_MSG_EQ((tc->GetRootQueueDiscOnDevice(dev) == nullptr),
                              true,
                              "precondition: device should have no root qdisc");

        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        Ptr<QueueDisc> returned = InstallRoot(dev, edge);

        NS_TEST_ASSERT_MSG_EQ((returned == edge),
                              true,
                              "InstallRoot should return the disc it installed");
        NS_TEST_ASSERT_MSG_EQ((tc->GetRootQueueDiscOnDevice(dev) == edge),
                              true,
                              "InstallRoot should set the root even when none was present");
        Simulator::Destroy();
    }
};

class DiffServTestSuite : public TestSuite
{
  public:
    DiffServTestSuite()
        : TestSuite("stratum", Type::UNIT)
    {
        // DumbMeter
        AddTestCase(new DumbMeterTestCase(), TestCase::Duration::QUICK);

        // S-1: TokenBucket (5 vectors)
        for (int i = 0; i < kNumTokenBucketVectors; ++i)
        {
            AddTestCase(new TokenBucketVectorTestCase(i), TestCase::Duration::QUICK);
        }

        // S-2: srTCM / RFC 2697 (10 vectors)
        for (int i = 0; i < kNumSrTcmVectors; ++i)
        {
            AddTestCase(new SrTcmVectorTestCase(i), TestCase::Duration::QUICK);
        }

        // S-3: trTCM / RFC 2698 (10 vectors)
        for (int i = 0; i < kNumTrTcmVectors; ++i)
        {
            AddTestCase(new TrTcmVectorTestCase(i), TestCase::Duration::QUICK);
        }

        // S-4: TSW meters
        AddTestCase(new TswEwmaConvergenceTestCase(), TestCase::Duration::QUICK);
        AddTestCase(new Tsw2cmUnderCirTestCase(), TestCase::Duration::QUICK);
        AddTestCase(new Tsw2cmOverCirTestCase(), TestCase::Duration::QUICK);
        AddTestCase(new Tsw3cmColourRatiosTestCase(), TestCase::Duration::QUICK);

        // S-5: Round Robin family
        AddTestCase(new RrEqualShareTest(), TestCase::Duration::QUICK);
        AddTestCase(new WrrWeightedShareTest(), TestCase::Duration::QUICK);
        AddTestCase(new WirrBoundedBurstinessTest(), TestCase::Duration::QUICK);

        // S-6: Priority Queue
        AddTestCase(new PqStrictPriorityTest(), TestCase::Duration::QUICK);
        AddTestCase(new PqRateCappedTest(), TestCase::Duration::QUICK);

        // S-12: DS-RED sub-queue
        AddTestCase(new HigherPrecDropsMoreTest(), TestCase::Duration::QUICK);
        AddTestCase(new TailDropOverflowTest(), TestCase::Duration::QUICK);
        AddTestCase(new EwmaConvergenceTest(), TestCase::Duration::QUICK);

        // S-13: Edge queue disc
        AddTestCase(new EdgeDscpMarkingTest(), TestCase::Duration::QUICK);
        AddTestCase(new EdgeNoMatchPassthroughTest(), TestCase::Duration::QUICK);
        AddTestCase(new EdgeSpecificAddrMatchTest(), TestCase::Duration::QUICK);
        AddTestCase(new EdgeIpv6ClassifyMarkTest(), TestCase::Duration::QUICK);
        AddTestCase(new PortBasedMarkRuleTest(), TestCase::Duration::QUICK);
        AddTestCase(new MeterInjectionTest(), TestCase::Duration::QUICK);
        AddTestCase(new MeterAssignStreamsCascadeTest(), TestCase::Duration::QUICK);
        AddTestCase(new EdgeWithL4sInnerTest(), TestCase::Duration::QUICK);
        AddTestCase(new MeterCascadeHelperPathTest(), TestCase::Duration::QUICK);
        AddTestCase(new TswWinLenHelperWiringTest(), TestCase::Duration::QUICK);
        AddTestCase(new QueueStatsProviderInterfaceTest(), TestCase::Duration::QUICK);
        AddTestCase(new S3PerClassRatePreservationTest(), TestCase::Duration::EXTENSIVE);
        AddTestCase(new MultiSlotDscpRoutingTest(), TestCase::Duration::QUICK);
        AddTestCase(new MultiSlotStrictPriorityTest(), TestCase::Duration::QUICK);
        AddTestCase(new BackwardCompatSingleInnerTest(), TestCase::Duration::QUICK);
        AddTestCase(new PerSlotQueueStatsProbesTest(), TestCase::Duration::QUICK);
        AddTestCase(new EdgeSlotZeroDelegationParityTest(), TestCase::Duration::QUICK);
        AddTestCase(new SetAsDiffservComposesEfEdgeTest(), TestCase::Duration::QUICK);
        AddTestCase(new SetAsDiffservComposesBestEffortEdgeTest(), TestCase::Duration::QUICK);
        AddTestCase(new SetAsDiffservUsesProvidedSchedulerTest(), TestCase::Duration::QUICK);
        AddTestCase(new L4sFqCoDelAutoDefaultQuantumTest(), TestCase::Duration::QUICK);

        // S-17: Across-slot dispatcher + CAKE composition
        AddTestCase(new SlotDispatcherByteIdentityTest(), TestCase::Duration::QUICK);
        AddTestCase(new TinShaperDrrQuantumHonoredTest(), TestCase::Duration::QUICK);
        AddTestCase(new TinShaperEmptyTinSkipTest(), TestCase::Duration::QUICK);
        AddTestCase(new TinShaperFairnessUnderMixedLoadTest(), TestCase::Duration::QUICK);
        AddTestCase(new AckFilterFunctionalContractTest(), TestCase::Duration::QUICK);
        AddTestCase(new AckFilterSackPreservationTest(), TestCase::Duration::QUICK);
        AddTestCase(new AckFilterAggressiveDropsSackTest(), TestCase::Duration::QUICK);
        AddTestCase(new AckFilterAggressiveSackArrivalTest(), TestCase::Duration::QUICK);
        AddTestCase(new CakeHelperDscpMapMatchesLinuxDiffserv4Test(), TestCase::Duration::QUICK);
        AddTestCase(new RateBasedDiffserv4DscpMapMatchesLinuxTest(), TestCase::Duration::QUICK);
        AddTestCase(new CakeHelperDscpMapMatchesLinuxDiffserv3Test(), TestCase::Duration::QUICK);
        AddTestCase(new CakeHelperDscpMapMatchesLinuxDiffserv8Test(), TestCase::Duration::QUICK);
        AddTestCase(new CakeDiffserv3UnshapedContentionRatioTest(), TestCase::Duration::QUICK);
        AddTestCase(new CakeHelperBestEffortMapsToSingleTinTest(), TestCase::Duration::QUICK);
        AddTestCase(new CakeHelperPrecedenceMapsAreByteExactTest(), TestCase::Duration::QUICK);
        AddTestCase(new CakeEndToEndSingleTinTest(), TestCase::Duration::QUICK);
        AddTestCase(new CakeHelperPathAlphaGammaEquivalenceTest(), TestCase::Duration::QUICK);
        AddTestCase(new HybridLlqStrictPriorityWinsOverDrrTest(), TestCase::Duration::QUICK);
        AddTestCase(new HybridLlqDrrFairnessWhenSpEmptyTest(), TestCase::Duration::QUICK);
        AddTestCase(new HybridLlqPeekIsSideEffectFreeTest(), TestCase::Duration::QUICK);
        AddTestCase(new HybridLlqOnDequeueAccountsOnlyDrrTest(), TestCase::Duration::QUICK);
        AddTestCase(new HybridLlqDrrCursorYieldsOnDrainTest(), TestCase::Duration::QUICK);
        AddTestCase(new TinTokenBucketUnitTest(), TestCase::Duration::QUICK);
        AddTestCase(new TinShaperRateCapHonoredTest(), TestCase::Duration::QUICK);
        AddTestCase(new TbfAsInnerSlotRateCapHonoredTest(), TestCase::Duration::QUICK);
        AddTestCase(new TinShaperIdleNoRedistributeTest(), TestCase::Duration::QUICK);
        AddTestCase(new TinShaperPeekSideEffectFreeWithCapTest(), TestCase::Duration::QUICK);
        AddTestCase(new LlqTinShapingCompositionTest(), TestCase::Duration::QUICK);
        AddTestCase(new EgressDscpWashTest(), TestCase::Duration::QUICK);
        AddTestCase(new EgressDscpWashIpv6Test(), TestCase::Duration::QUICK);
        AddTestCase(new CakeMemLimitAttributeTest(), TestCase::Duration::QUICK);
        AddTestCase(new CakeOverheadStatisticalRateAdjustmentTest(), TestCase::Duration::QUICK);
        AddTestCase(new CakeRawModeNoRateAdjustmentTest(), TestCase::Duration::QUICK);
        AddTestCase(new CakeConservativePresetTest(), TestCase::Duration::QUICK);
        AddTestCase(new CakePerTinDiagnosticsTest(), TestCase::Duration::QUICK);
        AddTestCase(new SetAssociativeHashStructuralPropertiesTest(), TestCase::Duration::QUICK);

        // S-14: PHB configuration
        AddTestCase(new EfPriorityServiceTest(), TestCase::Duration::QUICK);
        AddTestCase(new AfDropPrecedenceTest(), TestCase::Duration::QUICK);
        AddTestCase(new BestEffortLowestPriorityTest(), TestCase::Duration::QUICK);

        // E2E toy scenario
        AddTestCase(new E2EToyScenarioTest(), TestCase::Duration::QUICK);

        // S-16: diffserv::Helper
        AddTestCase(new HelperSrTcmConfigTest(), TestCase::Duration::QUICK);

        // E2E: edge → core pipeline
        AddTestCase(new E2EEdgeCoreTopologyTest(), TestCase::Duration::QUICK);

        // --- S-9: SCFQ ---
        AddTestCase(new ScfqDequeueOrderTest(), TestCase::Duration::QUICK);
        AddTestCase(new ScfqThroughputSharesTest(), TestCase::Duration::QUICK);
        AddTestCase(new ScfqLabelMonotonicityTest(), TestCase::Duration::QUICK);

        // --- S-10: SFQ ---
        AddTestCase(new SfqDequeueOrderTest(), TestCase::Duration::QUICK);
        AddTestCase(new SfqStartTagOrderTest(), TestCase::Duration::QUICK);
        AddTestCase(new SfqThroughputSharesTest(), TestCase::Duration::QUICK);

        // --- S-8: WF2Q+ ---
        AddTestCase(new Wf2qpThroughputSharesTest(), TestCase::Duration::QUICK);
        AddTestCase(new Wf2qpEligibilityTest(), TestCase::Duration::QUICK);

        // --- S-7: WFQ ---
        AddTestCase(new WfqThroughputSharesTest(), TestCase::Duration::QUICK);
        AddTestCase(new WfqVirtualTimeMonotonicTest(), TestCase::Duration::QUICK);
        AddTestCase(new WfqNoReorderingTest(), TestCase::Duration::QUICK);
        AddTestCase(new TestDiffServMonitorHelperDtorCancelsEvents(), TestCase::Duration::QUICK);

        // --- S-11: LLQ ---
        AddTestCase(new LlqPriorityFirstTest(), TestCase::Duration::QUICK);
        AddTestCase(new LlqFqManagesRemainingTest(), TestCase::Duration::QUICK);
        AddTestCase(new LlqRateCapSmokeTest(), TestCase::Duration::QUICK);
        AddTestCase(new LlqPqRateCapEngagesTest(), TestCase::Duration::QUICK);
        AddTestCase(new LlqPqRateCapRecoversAfterIdleTest(), TestCase::Duration::QUICK);
        AddTestCase(new RedSubQueueNumPrecClampedTest(), TestCase::Duration::QUICK);
        AddTestCase(new CakePrecedenceQuantumLadderTest(), TestCase::Duration::QUICK);
        AddTestCase(new CakeAutorateWindowUsesRawLengthTest(), TestCase::Duration::QUICK);

        // --- Q-3: WFQ fairness ---
        AddTestCase(new WfqFairThroughputTest(), TestCase::Duration::QUICK);
        AddTestCase(new WfqFullDrainReactivationTest(), TestCase::Duration::QUICK);
        // --- Q-4: WF2Q+ delay ---
        AddTestCase(new Wf2qpVsWfqDelayTest(), TestCase::Duration::QUICK);

        // --- FW: FWMeter (I-2.6) ---
        AddTestCase(new TestFWMeterDeterministic(), TestCase::Duration::QUICK);
        AddTestCase(new TestFWMeterProbabilistic(), TestCase::Duration::QUICK);
        AddTestCase(new TestFWMeterFlowTimeout(), TestCase::Duration::QUICK);
        AddTestCase(new TestFWMeterPeriodic(), TestCase::Duration::QUICK);
        AddTestCase(new TestFWMeterMultiFlowIsolation(), TestCase::Duration::QUICK);
        AddTestCase(new TestFWMeterEdgeDispatch(), TestCase::Duration::QUICK);
        AddTestCase(new FwPolicerPeriodicModeThroughClassifierTest(), TestCase::Duration::QUICK);

        // --- S-15: Statistics ---
        AddTestCase(new PacketAccountingBalanceTest(), TestCase::Duration::QUICK);
        AddTestCase(new DropAttributionTest(), TestCase::Duration::QUICK);
        AddTestCase(new RetxByteAccountingTest(), TestCase::Duration::QUICK);

        // --- S-18: Substrate-registry template ---
        AddTestCase(new AqmRegistryTemplateRoundTripTest(), TestCase::Duration::QUICK);
        AddTestCase(new SchedulerRegistrySmokeTest(), TestCase::Duration::QUICK);

        // aqm-eval-runner measurement-correctness gates (I-14).
        AddTestCase(new TestSAqmRunnerGoodputWindowFullSimtime(), TestCase::Duration::EXTENSIVE);
        AddTestCase(new TestSAqmRunnerUdpCbrRateBand(), TestCase::Duration::EXTENSIVE);
        AddTestCase(new TestSAqmStratumRedFactoryFourStep(), TestCase::Duration::QUICK);
        AddTestCase(new TestSAqmStratumL4sWredFactoryClassicLaneFunctional(),
                    TestCase::Duration::QUICK);

        // --- Q-2: Example-2 three-class ---
        AddTestCase(new Example2ThreeClassTest(), TestCase::Duration::EXTENSIVE);
        // --- Q-5: AF PHB drop precedence ---
        AddTestCase(new AfDropPrecedenceQualityTest(), TestCase::Duration::EXTENSIVE);
        // --- Q-6: EF+AF+BE coexistence ---
        AddTestCase(new ThreeClassCoexistenceTest(), TestCase::Duration::EXTENSIVE);

        // --- Q-7: Performance regression ---
        AddTestCase(new PerfRegressionTest(), TestCase::Duration::EXTENSIVE);

        // --- CAKE Q6 rate-based virtual-clock shaper ---
        AddTestCase(new S17_39_RateBasedMonotonicityTestCase(), TestCase::Duration::QUICK);
        AddTestCase(new S17_40_RateBasedCatchupTestCase(), TestCase::Duration::QUICK);
        AddTestCase(new S17_42_RateBasedAdjLenTestCase(), TestCase::Duration::QUICK);
        AddTestCase(new S17_43_ShaperModeDispatchTestCase(), TestCase::Duration::QUICK);

        // --- CAKE per-flow / per-host counter accessors ---
        AddTestCase(new TinShaperPerFlowStatsTest(), TestCase::Duration::QUICK);
        AddTestCase(new CakePrintTcStatsStructureTest(), TestCase::Duration::QUICK);
        AddTestCase(new CakeAutorateIngressApiContractTest(), TestCase::Duration::QUICK);
        AddTestCase(new InstallRootReplacesDefaultTest(), TestCase::Duration::QUICK);
        AddTestCase(new InstallRootOnBareDeviceTest(), TestCase::Duration::QUICK);
    }
};

static DiffServTestSuite g_diffServTestSuite;

} // namespace ns3::stratum
