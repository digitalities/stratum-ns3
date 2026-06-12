/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Standalone runner for RFC 2697/2698 test vectors.
 *
 * Two modes:
 *   1. STANDALONE — compile with: g++ -std=c++17 -o run-vectors
 * rfc-test-vectors-runner.cc Run with: ./run-vectors Uses trivial stub meters
 * that always return GREEN (demonstrates the runner compiles; stubs will fail
 * most vectors as expected).
 *
 *   2. NS-3 TestSuite — see the commented stub at the bottom.  Plug in
 *      the real meter classes once they exist.
 */

#include "rfc-test-vectors.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace diffserv_test;

// ─── Tolerance for double comparison ────────────────────────────────────────
// Bucket arithmetic is exact (no floating-point accumulation across many ops
// in a single vector), so 1e-6 is extremely conservative.
static constexpr double kEps = 1e-6;

static bool
near(double a, double b)
{
    return std::fabs(a - b) < kEps;
}

static const char*
colourName(Colour c)
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

// ═══════════════════════════════════════════════════════════════════════════
//  Stub meter classes (always return GREEN, no state tracking).
//  Replace these with real implementations to get meaningful results.
// ═══════════════════════════════════════════════════════════════════════════

struct StubTokenBucketMeter
{
    double m_cir{}, m_cBucket{}, m_arrivalTime{};
    uint32_t m_cbs{};

    void init(double cir, uint32_t cbs, double initC, double initT)
    {
        m_cir = cir;
        m_cbs = cbs;
        m_cBucket = initC;
        m_arrivalTime = initT;
    }

    void applyMeter(double now)
    {
        (void)now; // stub: no-op
    }

    Colour applyPolicer(uint32_t size)
    {
        (void)size;
        return Colour::GREEN; // stub: always GREEN
    }

    double cBucket() const
    {
        return m_cBucket;
    }
};

struct StubSrTcmMeter
{
    double m_cir{}, m_cBucket{}, m_eBucket{}, m_arrivalTime{};
    uint32_t m_cbs{}, m_ebs{};

    void init(double cir, uint32_t cbs, uint32_t ebs, double initC, double initE, double initT)
    {
        m_cir = cir;
        m_cbs = cbs;
        m_ebs = ebs;
        m_cBucket = initC;
        m_eBucket = initE;
        m_arrivalTime = initT;
    }

    void applyMeter(double now)
    {
        (void)now;
    }

    Colour applyPolicer(uint32_t size)
    {
        (void)size;
        return Colour::GREEN;
    }

    double cBucket() const
    {
        return m_cBucket;
    }

    double eBucket() const
    {
        return m_eBucket;
    }
};

struct StubTrTcmMeter
{
    double m_cir{}, m_pir{}, m_cBucket{}, m_pBucket{}, m_arrivalTime{};
    uint32_t m_cbs{}, m_pbs{};

    void init(double cir,
              double pir,
              uint32_t cbs,
              uint32_t pbs,
              double initC,
              double initP,
              double initT)
    {
        m_cir = cir;
        m_pir = pir;
        m_cbs = cbs;
        m_pbs = pbs;
        m_cBucket = initC;
        m_pBucket = initP;
        m_arrivalTime = initT;
    }

    void applyMeter(double now)
    {
        (void)now;
    }

    Colour applyPolicer(uint32_t size)
    {
        (void)size;
        return Colour::GREEN;
    }

    double cBucket() const
    {
        return m_cBucket;
    }

    double pBucket() const
    {
        return m_pBucket;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  Runner templates
// ═══════════════════════════════════════════════════════════════════════════

template <typename Meter>
int
runTokenBucketVectors(bool verbose)
{
    int pass = 0;
    int fail = 0;
    for (int v = 0; v < kNumTokenBucketVectors; ++v)
    {
        const auto& vec = kTokenBucketVectors[v];
        Meter m;
        m.init(vec.cir_bytes_per_sec,
               vec.cbs_bytes,
               vec.initial_c_bucket,
               vec.initial_arrival_time);

        bool ok = true;
        for (int i = 0; i < vec.num_events; ++i)
        {
            const auto& e = vec.events[i];
            m.applyMeter(e.arrival_time_s);
            auto c = m.applyPolicer(e.size_bytes);

            if (c != e.expected_colour || !near(m.cBucket(), e.expected_c_bucket))
            {
                ok = false;
                if (verbose)
                {
                    std::printf("  FAIL event %d: got %s c=%.1f, "
                                "want %s c=%.1f\n",
                                i,
                                colourName(c),
                                m.cBucket(),
                                colourName(e.expected_colour),
                                e.expected_c_bucket);
                }
            }
        }
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", vec.name);
        ok ? ++pass : ++fail;
    }
    return fail;
}

template <typename Meter>
int
runSrTcmVectors(bool verbose)
{
    int pass = 0;
    int fail = 0;
    for (int v = 0; v < kNumSrTcmVectors; ++v)
    {
        const auto& vec = kSrTcmVectors[v];
        Meter m;
        m.init(vec.cir_bytes_per_sec,
               vec.cbs_bytes,
               vec.ebs_bytes,
               vec.initial_c_bucket,
               vec.initial_e_bucket,
               vec.initial_arrival_time);

        bool ok = true;
        for (int i = 0; i < vec.num_events; ++i)
        {
            const auto& e = vec.events[i];
            m.applyMeter(e.arrival_time_s);
            auto c = m.applyPolicer(e.size_bytes);

            if (c != e.expected_colour || !near(m.cBucket(), e.expected_c_bucket) ||
                (e.expected_e_bucket >= 0 && !near(m.eBucket(), e.expected_e_bucket)))
            {
                ok = false;
                if (verbose)
                {
                    std::printf("  FAIL event %d: got %s c=%.1f e=%.1f, "
                                "want %s c=%.1f e=%.1f\n",
                                i,
                                colourName(c),
                                m.cBucket(),
                                m.eBucket(),
                                colourName(e.expected_colour),
                                e.expected_c_bucket,
                                e.expected_e_bucket);
                }
            }
        }
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", vec.name);
        ok ? ++pass : ++fail;
    }
    return fail;
}

template <typename Meter>
int
runTrTcmVectors(bool verbose)
{
    int pass = 0;
    int fail = 0;
    for (int v = 0; v < kNumTrTcmVectors; ++v)
    {
        const auto& vec = kTrTcmVectors[v];
        Meter m;
        m.init(vec.cir_bytes_per_sec,
               vec.pir_bytes_per_sec,
               vec.cbs_bytes,
               vec.pbs_bytes,
               vec.initial_c_bucket,
               vec.initial_p_bucket,
               vec.initial_arrival_time);

        bool ok = true;
        for (int i = 0; i < vec.num_events; ++i)
        {
            const auto& e = vec.events[i];
            m.applyMeter(e.arrival_time_s);
            auto c = m.applyPolicer(e.size_bytes);

            if (c != e.expected_colour || !near(m.cBucket(), e.expected_c_bucket) ||
                (e.expected_p_bucket >= 0 && !near(m.pBucket(), e.expected_p_bucket)))
            {
                ok = false;
                if (verbose)
                {
                    std::printf("  FAIL event %d: got %s c=%.1f p=%.1f, "
                                "want %s c=%.1f p=%.1f\n",
                                i,
                                colourName(c),
                                m.cBucket(),
                                m.pBucket(),
                                colourName(e.expected_colour),
                                e.expected_c_bucket,
                                e.expected_p_bucket);
                }
            }
        }
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", vec.name);
        ok ? ++pass : ++fail;
    }
    return fail;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Mode 1: Standalone main()
// ═══════════════════════════════════════════════════════════════════════════

int
main()
{
    bool verbose = true;
    int total_fail = 0;

    std::printf("=== Token Bucket (%d vectors) ===\n", kNumTokenBucketVectors);
    total_fail += runTokenBucketVectors<StubTokenBucketMeter>(verbose);

    std::printf("\n=== srTCM / RFC 2697 (%d vectors) ===\n", kNumSrTcmVectors);
    total_fail += runSrTcmVectors<StubSrTcmMeter>(verbose);

    std::printf("\n=== trTCM / RFC 2698 (%d vectors) ===\n", kNumTrTcmVectors);
    total_fail += runTrTcmVectors<StubTrTcmMeter>(verbose);

    int total = kNumTokenBucketVectors + kNumSrTcmVectors + kNumTrTcmVectors;
    int pass = total - total_fail;
    std::printf("\n=== Summary: %d/%d passed ===\n", pass, total);

    return total_fail > 0 ? 1 : 0;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Mode 2: ns-3 TestSuite stub (uncomment and adapt once ns-3 integration
//  is in place and the real meter classes exist)
// ═══════════════════════════════════════════════════════════════════════════
/*
#include "ns3/test.h"
#include "ns3/stratum-sr-tcm-meter.h"  // the real meter class

namespace ns3::stratum
{

class SrTcmRfcTestCase : public TestCase {
  public:
    SrTcmRfcTestCase(const diffserv_test::SrTcmTestVector& vec)
        : TestCase(vec.name), m_vec(vec) {}

    void DoRun() override {
        SrTcmMeter meter;
        meter.SetAttribute("Cir", DoubleValue(m_vec.cir_bytes_per_sec));
        meter.SetAttribute("Cbs", UintegerValue(m_vec.cbs_bytes));
        meter.SetAttribute("Ebs", UintegerValue(m_vec.ebs_bytes));
        // ... set initial bucket state ...

        for (int i = 0; i < m_vec.num_events; ++i) {
            const auto& e = m_vec.events[i];
            Ptr<Packet> pkt = Create<Packet>(e.size_bytes);
            Simulator::ScheduleWithContext(0, Seconds(e.arrival_time_s),
                [&]() {
                    auto colour = meter.Classify(pkt);
                    NS_TEST_EXPECT_MSG_EQ(colour,
                        static_cast<int>(e.expected_colour),
                        "Colour mismatch at event " << i);
                });
        }
        Simulator::Run();
        Simulator::Destroy();
    }

  private:
    diffserv_test::SrTcmTestVector m_vec;
};

class RfcConformanceTestSuite : public TestSuite {
  public:
    RfcConformanceTestSuite() : TestSuite("stratum-rfc-conformance",
Type::UNIT) { for (int i = 0; i < diffserv_test::kNumSrTcmVectors; ++i)
            AddTestCase(new SrTcmRfcTestCase(diffserv_test::kSrTcmVectors[i]));
        // Similarly for TB and trTCM vectors.
    }
};

static RfcConformanceTestSuite g_rfcSuite;

} // namespace ns3::stratum
*/
