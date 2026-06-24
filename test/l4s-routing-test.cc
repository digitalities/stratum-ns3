/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Tests for l4s::QueueDisc covering enqueue-side classification
 * and DualPI2 coupling invariants per RFC 9332 §2.1 eq. (1) and
 * Appendix A.1.
 */

#include "ns3/fq-codel-queue-disc.h"
#include "ns3/ipv4-header.h"
#include "ns3/ipv4-queue-disc-item.h"
#include "ns3/ipv6-header.h"
#include "ns3/ipv6-queue-disc-item.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/stratum-l4s-coupled-scheduler.h"
#include "ns3/stratum-l4s-helper.h"
#include "ns3/stratum-l4s-queue-disc.h"
#include "ns3/stratum-pq-scheduler.h"
#include "ns3/test.h"
#include "ns3/uinteger.h"

#include <cmath>
#include <iostream>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

// Scenario-validation test class definitions — textually included so the
// DsL4sQueueDiscSuite constructor can instantiate them with 'new'. The file
// is NOT listed separately in CMakeLists.txt TEST_SOURCES.
#include "l4s-scenario-validation-test.cc"

using namespace ns3;
namespace l4s = ns3::stratum::l4s;
using ns3::stratum::DsL4sScenarioCakeCompositionFairnessTest;
using ns3::stratum::DsL4sScenarioCakeCompositionThroughputParityTest;
using ns3::stratum::DsL4sScenarioDualPi2GprtParityTest;
using ns3::stratum::DsL4sScenarioFqCoDelClassicCompositionalSafetyTest;
using ns3::stratum::DsL4sScenarioFqCoDelComparisonSmokePerModeTest;
using ns3::stratum::DsL4sScenarioPiControlFiresTest;
using ns3::stratum::DsL4sScenarioS1AdvantageLatencyDeltaTest;
using ns3::stratum::DsL4sScenarioS1LatencyDifferentiationTest;
using ns3::stratum::DsL4sScenarioS2CoexistenceThroughputEquivalenceTest;
using ns3::stratum::PriorityScheduler;

namespace
{

/// Build a minimal Ipv4QueueDiscItem with the given DSCP and ECN.
Ptr<Ipv4QueueDiscItem>
MakeItem(Ipv4Header::DscpType dscp, Ipv4Header::EcnType ecn, uint32_t payloadSize = 500)
{
    Ipv4Header hdr;
    hdr.SetSource(Ipv4Address("10.0.0.1"));
    hdr.SetDestination(Ipv4Address("10.0.0.2"));
    hdr.SetProtocol(17); // UDP
    hdr.SetDscp(dscp);
    hdr.SetEcn(ecn);
    hdr.SetPayloadSize(payloadSize);

    Ptr<Packet> pkt = Create<Packet>(payloadSize);
    return Create<Ipv4QueueDiscItem>(pkt, Address(), 0x0800, hdr);
}

/// Build a minimal Ipv6QueueDiscItem with the given DSCP and ECN.
/// Mirrors MakeItem; v6 uses SetNextHeader/SetPayloadLength and EtherType 0x86DD.
Ptr<Ipv6QueueDiscItem>
MakeItemV6(Ipv6Header::DscpType dscp, Ipv6Header::EcnType ecn, uint32_t payloadSize = 500)
{
    Ipv6Header hdr;
    hdr.SetSource(Ipv6Address("2001:db8::1"));
    hdr.SetDestination(Ipv6Address("2001:db8::2"));
    hdr.SetNextHeader(17); // UDP
    hdr.SetDscp(dscp);
    hdr.SetEcn(ecn);
    hdr.SetPayloadLength(payloadSize);

    Ptr<Packet> pkt = Create<Packet>(payloadSize);
    return Create<Ipv6QueueDiscItem>(pkt, Address(), 0x86DD, hdr);
}

/// Build and initialize a 2-queue L4S disc with WRED thresholds wide
/// enough that the parent's WRED never force-drops. Returns the disc
/// with classic at idx 0 and L4S at idx 1.
Ptr<l4s::QueueDisc>
MakeL4sDisc(uint32_t classicQlim = 10000, uint32_t l4sQlim = 10000)
{
    auto disc = CreateObject<l4s::QueueDisc>();
    disc->SetNumQueues(2);
    disc->SetL4sQueueIdx(1);
    disc->SetQueueLimit(0, classicQlim);
    disc->SetQueueLimit(1, l4sQlim);

    // Classic DSCP 0 -> queue 0 prec 0; L4S bypasses PHB lookup.
    disc->AddPhbEntry(0, 0, 0);

    Ptr<PriorityScheduler> sched =
        CreateObjectWithAttributes<PriorityScheduler>("NumQueues", UintegerValue(2));
    disc->SetScheduler(sched);

    disc->Initialize();

    // Generous WRED thresholds keep the parent's RIO machinery inert
    // throughout the coupling-invariant tests; only the L4S coupling
    // logic should drive drops/marks.
    disc->ConfigQueue({.queue = 0, .prec = 0, .thMin = 5000.0, .thMax = 10000.0, .maxP = 0.1});
    disc->ConfigQueue({.queue = 1, .prec = 0, .thMin = 5000.0, .thMax = 10000.0, .maxP = 0.1});
    return disc;
}

/// S-L4S.1: ECT(1)/CE route to the L4S sub-queue; NotECT/ECT(0) route to
/// classic.
class DsL4sRoutingTest : public TestCase
{
  public:
    DsL4sRoutingTest()
        : TestCase("L4S enqueue-side routing by ECN codepoint")
    {
    }

    void DoRun() override
    {
        auto disc = CreateObject<l4s::QueueDisc>();
        disc->SetNumQueues(2);
        disc->SetL4sQueueIdx(1);

        // PHB entry for classic traffic: DSCP 0 (DscpDefault) -> queue 0.
        // L4S traffic bypasses PHB lookup entirely, so no entry needed
        // for it.
        disc->AddPhbEntry(0, 0, 0);

        // Explicit PQ scheduler for determinism; default RR would also work.
        Ptr<PriorityScheduler> sched =
            CreateObjectWithAttributes<PriorityScheduler>("NumQueues", UintegerValue(2));
        disc->SetScheduler(sched);

        disc->Initialize();

        // Set MRED thresholds large enough that WRED never force-drops
        // within this test. Default thMin=thMax=0 triggers "above thMax"
        // forced drop on the first packet that raises vAve > 0.
        disc->ConfigQueue({.queue = 0, .prec = 0, .thMin = 100.0, .thMax = 200.0, .maxP = 0.1});
        disc->ConfigQueue({.queue = 1, .prec = 0, .thMin = 100.0, .thMax = 200.0, .maxP = 0.1});

        // Packet 1: NotECT with DSCP 0 — classic path, goes to queue 0.
        Ptr<Ipv4QueueDiscItem> classicPkt =
            MakeItem(Ipv4Header::DscpDefault, Ipv4Header::ECN_NotECT);
        NS_TEST_ASSERT_MSG_EQ(disc->Enqueue(classicPkt), true, "Classic packet must enqueue");

        // The composer's direct children are fixed — child 0 is the L4S
        // FIFO, child 1 is the classic stratum::RedQueueDisc. Accessors provide
        // typed handles.
        Ptr<QueueDisc> classicQ = disc->GetClassicAqmDisc();
        Ptr<QueueDisc> l4sQ = disc->GetL4sQueueDisc();
        NS_TEST_ASSERT_MSG_EQ(classicQ->GetNPackets(),
                              1U,
                              "Classic sub-queue should hold 1 packet after NotECT enqueue");
        NS_TEST_ASSERT_MSG_EQ(l4sQ->GetNPackets(),
                              0U,
                              "L4S sub-queue should be empty after NotECT enqueue");

        // Packet 2: ECT(1) with DSCP 0 — L4S path, goes to queue 1.
        Ptr<Ipv4QueueDiscItem> l4sPkt = MakeItem(Ipv4Header::DscpDefault, Ipv4Header::ECN_ECT1);
        NS_TEST_ASSERT_MSG_EQ(disc->Enqueue(l4sPkt), true, "L4S packet must enqueue");

        NS_TEST_ASSERT_MSG_EQ(classicQ->GetNPackets(),
                              1U,
                              "Classic sub-queue should still hold 1 packet after ECT(1) enqueue");
        NS_TEST_ASSERT_MSG_EQ(l4sQ->GetNPackets(),
                              1U,
                              "L4S sub-queue should hold 1 packet after ECT(1) enqueue");

        // Packet 3: CE — also L4S path.
        Ptr<Ipv4QueueDiscItem> cePkt = MakeItem(Ipv4Header::DscpDefault, Ipv4Header::ECN_CE);
        NS_TEST_ASSERT_MSG_EQ(disc->Enqueue(cePkt), true, "CE-marked packet must enqueue");

        NS_TEST_ASSERT_MSG_EQ(l4sQ->GetNPackets(),
                              2U,
                              "L4S sub-queue should hold 2 packets after CE enqueue");

        // Packet 4: ECT(0) — classic path (RFC 9331 reserves ECT(1) for L4S).
        Ptr<Ipv4QueueDiscItem> ect0Pkt = MakeItem(Ipv4Header::DscpDefault, Ipv4Header::ECN_ECT0);
        NS_TEST_ASSERT_MSG_EQ(disc->Enqueue(ect0Pkt),
                              true,
                              "ECT(0) packet must enqueue on classic path");

        NS_TEST_ASSERT_MSG_EQ(classicQ->GetNPackets(),
                              2U,
                              "Classic sub-queue should hold 2 packets after ECT(0) enqueue");
        NS_TEST_ASSERT_MSG_EQ(l4sQ->GetNPackets(),
                              2U,
                              "L4S sub-queue should still hold 2 packets after ECT(0) enqueue");
    }
};

/// Q-18.3: IPv6 ECT(1)/CE route to the L4S sub-queue; NotECT/ECT(0) route to
/// classic. Exercises the already-family-agnostic IsL4sPacket seam over v6.
class TestQ18v3L4sEctClassifyIPv6 : public TestCase
{
  public:
    TestQ18v3L4sEctClassifyIPv6()
        : TestCase("L4S enqueue-side routing by ECN codepoint over IPv6")
    {
    }

    void DoRun() override
    {
        auto disc = MakeL4sDisc();
        Ptr<QueueDisc> classicQ = disc->GetClassicAqmDisc();
        Ptr<QueueDisc> l4sQ = disc->GetL4sQueueDisc();

        // NotECT, DSCP 0 -> classic.
        NS_TEST_ASSERT_MSG_EQ(
            disc->Enqueue(MakeItemV6(Ipv6Header::DscpDefault, Ipv6Header::ECN_NotECT)),
            true,
            "v6 NotECT must enqueue");
        NS_TEST_ASSERT_MSG_EQ(classicQ->GetNPackets(), 1U, "v6 NotECT -> classic");
        NS_TEST_ASSERT_MSG_EQ(l4sQ->GetNPackets(), 0U, "v6 NotECT not on L4S");

        // ECT(1) -> L4S.
        NS_TEST_ASSERT_MSG_EQ(
            disc->Enqueue(MakeItemV6(Ipv6Header::DscpDefault, Ipv6Header::ECN_ECT1)),
            true,
            "v6 ECT(1) must enqueue");
        NS_TEST_ASSERT_MSG_EQ(l4sQ->GetNPackets(), 1U, "v6 ECT(1) -> L4S");

        // CE -> L4S.
        NS_TEST_ASSERT_MSG_EQ(
            disc->Enqueue(MakeItemV6(Ipv6Header::DscpDefault, Ipv6Header::ECN_CE)),
            true,
            "v6 CE must enqueue");
        NS_TEST_ASSERT_MSG_EQ(l4sQ->GetNPackets(), 2U, "v6 CE -> L4S");

        // ECT(0) -> classic (RFC 9331 reserves ECT(1) for L4S).
        NS_TEST_ASSERT_MSG_EQ(
            disc->Enqueue(MakeItemV6(Ipv6Header::DscpDefault, Ipv6Header::ECN_ECT0)),
            true,
            "v6 ECT(0) must enqueue");
        NS_TEST_ASSERT_MSG_EQ(classicQ->GetNPackets(), 2U, "v6 ECT(0) -> classic");
        NS_TEST_ASSERT_MSG_EQ(l4sQ->GetNPackets(), 2U, "v6 ECT(0) not on L4S");
    }
};

/// Q-18.3 (S-L4S.5 over v6): an already-CE IPv6 packet must not be re-marked.
/// RED before the fix — the v4-only DynamicCast skips the guard for v6,
/// and Ipv6QueueDiscItem::Mark() returns true for already-CE, so the mark
/// counter double-counts.
class TestQ18v3L4sCeIdempotenceIPv6 : public TestCase
{
  public:
    TestQ18v3L4sCeIdempotenceIPv6()
        : TestCase("CE-marked IPv6 packet stays CE without double-marking")
    {
    }

    void DoRun() override
    {
        auto disc = MakeL4sDisc();
        disc->AssignStreams(11);
        disc->SetL4sTargetSojournMs(1e6); // disable the step branch
        disc->ForceBaseProbForTest(
            0.5); // p_L = min(2*0.5,1) = 1: every above-floor dequeue draws "mark"

        uint64_t marksBefore = disc->GetStats().nTotalMarkedPackets;

        constexpr uint32_t kN = 100;
        for (uint32_t i = 0; i < kN; ++i)
        {
            NS_TEST_ASSERT_MSG_EQ(
                disc->Enqueue(MakeItemV6(Ipv6Header::DscpDefault, Ipv6Header::ECN_CE)),
                true,
                "v6 CE-marked packet must enqueue");
        }
        Ptr<QueueDisc> l4sQ = disc->GetL4sQueueDisc();
        NS_TEST_ASSERT_MSG_EQ(l4sQ->GetNPackets(), kN, "all v6 CE packets on the L4S sub-queue");

        for (uint32_t i = 0; i < kN; ++i)
        {
            NS_TEST_ASSERT_MSG_NE(disc->Dequeue(), nullptr, "v6 CE packet should dequeue");
        }
        uint64_t marksAfter = disc->GetStats().nTotalMarkedPackets;
        NS_TEST_ASSERT_MSG_EQ(marksAfter - marksBefore,
                              0U,
                              "Mark counter must not increment for already-CE IPv6 packets "
                              "(RFC 9331 §5 idempotence)");
    }
};

/// Q-18.3 (e2e proof): IPv6 ECT(1) flows through the coupled-mark path get
/// CE-marked. At p_L = 1 every above-floor dequeue marks, and the dequeued
/// item must come out CE — proving Ipv6QueueDiscItem::Mark() does the
/// ECN-preserving CE write end-to-end.
class TestQ18v3L4sMarkingIPv6 : public TestCase
{
  public:
    TestQ18v3L4sMarkingIPv6()
        : TestCase("L4S coupled marking writes CE on IPv6 ECT(1) flows")
    {
    }

    void DoRun() override
    {
        auto disc = MakeL4sDisc();
        disc->AssignStreams(11);
        disc->SetL4sTargetSojournMs(1e6); // disable the step branch
        disc->ForceBaseProbForTest(0.5);  // p_L = 1

        constexpr uint32_t kM = 100;
        constexpr uint32_t kBacklog = 50; // keeps every measured dequeue above the 2-MTU floor
        for (uint32_t i = 0; i < kM + kBacklog; ++i)
        {
            NS_TEST_ASSERT_MSG_EQ(
                disc->Enqueue(MakeItemV6(Ipv6Header::DscpDefault, Ipv6Header::ECN_ECT1)),
                true,
                "v6 ECT(1) packet must enqueue");
        }

        uint64_t marksBefore = disc->GetStats().nTotalMarkedPackets;
        uint32_t ceOut = 0;
        for (uint32_t i = 0; i < kM; ++i)
        {
            Ptr<QueueDiscItem> out = disc->Dequeue();
            NS_TEST_ASSERT_MSG_NE(out, nullptr, "v6 ECT(1) packet should dequeue");
            Ptr<Ipv6QueueDiscItem> ip = DynamicCast<Ipv6QueueDiscItem>(out);
            NS_TEST_ASSERT_MSG_NE(ip, nullptr, "dequeued item must be Ipv6QueueDiscItem");
            if (ip->GetHeader().GetEcn() == Ipv6Header::ECN_CE)
            {
                ++ceOut;
            }
        }
        uint64_t marksAfter = disc->GetStats().nTotalMarkedPackets;

        NS_TEST_ASSERT_MSG_EQ(marksAfter - marksBefore,
                              kM,
                              "Mark counter must increment by kM for v6 ECT(1) inputs at p_L = 1");
        NS_TEST_ASSERT_MSG_EQ(ceOut, kM, "every dequeued v6 ECT(1) packet must come out CE-marked");
    }
};

/// S-L4S.2: setter/getter round-trip for L4sQueueIdx.
class DsL4sConfigTest : public TestCase
{
  public:
    DsL4sConfigTest()
        : TestCase("L4sQueueIdx setter and getter round-trip")
    {
    }

    void DoRun() override
    {
        auto disc = CreateObject<l4s::QueueDisc>();
        NS_TEST_ASSERT_MSG_EQ(disc->GetL4sQueueIdx(), 1U, "Default L4sQueueIdx should be 1");
        disc->SetL4sQueueIdx(3);
        NS_TEST_ASSERT_MSG_EQ(disc->GetL4sQueueIdx(), 3U, "Setter should update L4sQueueIdx");

        // Attribute defaults — RFC 9332 alignment.
        NS_TEST_ASSERT_MSG_EQ(disc->GetL4sTargetSojournMs(),
                              1.0,
                              "Default L4S target sojourn should be 1 ms (RFC 9332)");
        NS_TEST_ASSERT_MSG_EQ(disc->GetCouplingFactor(),
                              2.0,
                              "Default coupling factor k should be 2 (RFC 9332)");
        NS_TEST_ASSERT_MSG_EQ(static_cast<int>(disc->GetClassicAqm()),
                              static_cast<int>(l4s::QueueDisc::ClassicAqm::Wred),
                              "Default classic AQM should be Wred");
    }
};

/// S-L4S.3: empty L4S queue produces zero coupled drop on classic path.
/// With p' driven only by L4S sojourn time, an empty L4S queue ⇒ p' = 0
/// ⇒ p_C = 0 ⇒ no classic packets dropped by coupling.
class DsL4sZeroLoadCouplingTest : public TestCase
{
  public:
    DsL4sZeroLoadCouplingTest()
        : TestCase("Empty L4S queue yields zero classic coupled drops")
    {
    }

    void DoRun() override
    {
        auto disc = MakeL4sDisc();
        disc->AssignStreams(1);

        constexpr uint32_t kN = 1000;
        for (uint32_t i = 0; i < kN; ++i)
        {
            auto pkt = MakeItem(Ipv4Header::DscpDefault, Ipv4Header::ECN_NotECT);
            bool ok = disc->Enqueue(pkt);
            NS_TEST_ASSERT_MSG_EQ(ok, true, "Classic packet should enqueue with empty L4S queue");
            NS_TEST_ASSERT_MSG_EQ(disc->GetLastClassicCoupledProb(),
                                  0.0,
                                  "Coupled drop probability must be zero with empty L4S queue");
        }

        NS_TEST_ASSERT_MSG_EQ(disc->GetBaseProb(),
                              0.0,
                              "Base probability must remain zero throughout when "
                              "L4S queue stays empty");

        // No coupled drops should have been recorded by the parent's
        // drop counter for "L4S_COUPLED_DROP" reason.
        const auto& nDrops = disc->GetStats().nDroppedPacketsBeforeEnqueue;
        auto it = nDrops.find("L4S_COUPLED_DROP");
        NS_TEST_ASSERT_MSG_EQ(it == nDrops.end(),
                              true,
                              "No coupled drop reason should appear in stats with empty L4S queue");
    }
};

/// S-L4S.4: squared-ratio coupling invariant. With p' pinned to a
/// fixed value, the observed classic drop ratio approaches p'^2 and
/// the observed L4S mark ratio approaches k * p' (RFC 9332 §2.1
/// eq. (1): p_C = (p_CL / k)^2 with p_CL = k * p'; App. A.1 Fig. 6
/// lines 4-5). At p' = 0.2, k = 2: p_C = 0.04, p_L = 0.4.
///
/// (Recalibrated 2026-06-10: this test previously asserted
/// p_C = (k * p')^2 = 0.16, reading the expectation back out of an
/// implementation misreading of eq. (1) while labelling it the
/// "RFC 9332 default". The RFC derivation above is now the source of
/// the expected values; see S-L4S.13 for the exact-arithmetic vector.)
///
/// To keep the L4S immediate-mark step from saturating p_L to 1.0, we
/// enlarge the L4S target sojourn so the queue length used in the test
/// stays well below threshold. Only the coupled p_L = k * p' branch is
/// exercised.
class DsL4sSquaredRatioTest : public TestCase
{
  public:
    DsL4sSquaredRatioTest()
        : TestCase("Coupling formula: classic drops scale as squared L4S mark rate")
    {
    }

    void DoRun() override
    {
        auto disc = MakeL4sDisc();
        disc->AssignStreams(7);
        // Push the immediate-mark threshold far above any qlen the
        // test will reach so the linear p_L = 2 * p' branch is tested
        // in isolation.
        disc->SetL4sTargetSojournMs(1e6);
        disc->SetCouplingFactor(2.0);

        constexpr double kPrime = 0.2;
        disc->ForceBaseProbForTest(kPrime);

        // RFC 9332 App. A.1 Fig. 6 lines 4-5 at k=2, p'=0.2:
        //   p_L = k * p' = 2 * 0.2 = 0.40
        //   p_C = p'^2   = 0.2^2   = 0.04
        //   ratio p_C / p_L = p' / k = 0.10
        const double kExpectedPL = 2.0 * kPrime;
        const double kExpectedPC = kPrime * kPrime;

        constexpr uint32_t kN = 4000;

        // Enqueue classic packets and count coupled drops.
        uint32_t classicDrops = 0;
        for (uint32_t i = 0; i < kN; ++i)
        {
            auto pkt = MakeItem(Ipv4Header::DscpDefault, Ipv4Header::ECN_NotECT);
            bool ok = disc->Enqueue(pkt);
            if (!ok)
            {
                ++classicDrops;
            }
        }
        double observedPC = static_cast<double>(classicDrops) / kN;

        // The L4S coupled CE mark is applied at dequeue (RFC 9332 App. A.1
        // step AQM), so drive the L4S rate on a fresh L4S-only disc: enqueue
        // a backlog above the two-MTU floor, then dequeue and count marks. A
        // spare backlog keeps the floor cleared across every measured dequeue
        // so only the coupled p_L = k * p' branch is exercised.
        auto l4sDisc = MakeL4sDisc();
        l4sDisc->AssignStreams(100);
        l4sDisc->SetL4sTargetSojournMs(1e6);
        l4sDisc->SetCouplingFactor(2.0);
        l4sDisc->ForceBaseProbForTest(kPrime);

        constexpr uint32_t kBacklog = 50;
        for (uint32_t i = 0; i < kN + kBacklog; ++i)
        {
            bool ok = l4sDisc->Enqueue(MakeItem(Ipv4Header::DscpDefault, Ipv4Header::ECN_ECT1));
            NS_TEST_ASSERT_MSG_EQ(ok, true, "L4S packet enqueue should not drop in this test");
        }
        uint64_t marksBefore = l4sDisc->GetStats().nTotalMarkedPackets;
        for (uint32_t i = 0; i < kN; ++i)
        {
            Ptr<QueueDiscItem> out = l4sDisc->Dequeue();
            NS_TEST_ASSERT_MSG_NE(out, nullptr, "L4S packet should dequeue");
        }
        uint64_t marksAfter = l4sDisc->GetStats().nTotalMarkedPackets;
        double observedPL = static_cast<double>(marksAfter - marksBefore) / kN;

        // 4000 samples: 95 % CI half-width is 1.96*sqrt(p(1-p)/4000) =
        // 0.0061 at p = 0.04 and 0.0152 at p = 0.40. Use 0.03 absolute
        // tolerance — ≥2x the wider CI — for headroom across RNG streams.
        constexpr double kTol = 0.03;
        NS_TEST_ASSERT_MSG_EQ_TOL(
            observedPC,
            kExpectedPC,
            kTol,
            "Observed p_C must approach p'^2 within statistical tolerance");
        NS_TEST_ASSERT_MSG_EQ_TOL(observedPL,
                                  kExpectedPL,
                                  kTol,
                                  "Observed p_L must approach k * p' within statistical tolerance");

        // Snapshot accessors expose the last computed values: p_C from the
        // classic disc's last enqueue, p_L from the L4S disc's last dequeue
        // (both above the two-MTU floor).
        NS_TEST_ASSERT_MSG_EQ_TOL(disc->GetLastClassicCoupledProb(),
                                  kExpectedPC,
                                  1e-9,
                                  "GetLastClassicCoupledProb must equal p'^2");
        NS_TEST_ASSERT_MSG_EQ_TOL(l4sDisc->GetLastL4sMarkProb(),
                                  kExpectedPL,
                                  1e-9,
                                  "GetLastL4sMarkProb must equal k * p' (coupled branch)");
    }
};

/// S-L4S.5: CE-mark idempotence. RFC 9331 §5: an already-CE packet
/// must not be re-marked or have its mark cleared. Our pipeline calls
/// QueueDisc::Mark, which delegates to Ipv4QueueDiscItem::Mark — that
/// method returns false when the packet is already CE, leaving the
/// header untouched. Verify the packet still has CE after enqueue and
/// the marked-packet counter does NOT increment for already-CE inputs.
class DsL4sCeIdempotenceTest : public TestCase
{
  public:
    DsL4sCeIdempotenceTest()
        : TestCase("CE-marked packet stays CE without double-marking")
    {
    }

    void DoRun() override
    {
        auto disc = MakeL4sDisc();
        disc->AssignStreams(11);
        disc->SetL4sTargetSojournMs(1e6);
        // Force p_L = 1.0 effectively by pinning p' = 0.5 (linear
        // branch saturates: 2 * 0.5 = 1.0). Every above-floor dequeue draws
        // "mark". Marks are applied at dequeue (App. A.1 step AQM), so the
        // packets are dequeued to drive the mark path.
        disc->ForceBaseProbForTest(0.5);

        uint64_t marksBefore = disc->GetStats().nTotalMarkedPackets;

        constexpr uint32_t kN = 100;
        for (uint32_t i = 0; i < kN; ++i)
        {
            // Pre-marked CE packets: idempotence applies.
            auto pkt = MakeItem(Ipv4Header::DscpDefault, Ipv4Header::ECN_CE);
            bool ok = disc->Enqueue(pkt);
            NS_TEST_ASSERT_MSG_EQ(ok, true, "CE-marked packet must still enqueue");
        }

        // Confirm the L4S queue (composer child 0) received all the packets.
        // Accessor-based lookup is refactor-robust.
        Ptr<QueueDisc> l4sQ = disc->GetL4sQueueDisc();
        NS_TEST_ASSERT_MSG_EQ(l4sQ->GetNPackets(),
                              kN,
                              "All CE packets should be enqueued on the L4S sub-queue");

        // Dequeue them: the mark path runs (p_L = 1.0 above the floor) but an
        // already-CE packet must not be re-marked (RFC 9331 §5), so the mark
        // counter stays put.
        for (uint32_t i = 0; i < kN; ++i)
        {
            Ptr<QueueDiscItem> out = disc->Dequeue();
            NS_TEST_ASSERT_MSG_NE(out, nullptr, "CE packet should dequeue");
        }
        uint64_t marksAfter = disc->GetStats().nTotalMarkedPackets;

        NS_TEST_ASSERT_MSG_EQ(marksAfter - marksBefore,
                              0U,
                              "Mark counter must not increment for already-CE "
                              "packets (RFC 9331 §5 idempotence)");

        // Now compare with ECT(1): the same dequeue rate should produce
        // marks, since the linear coupling saturates. A spare backlog keeps
        // every measured dequeue above the two-MTU floor.
        constexpr uint32_t kM = 100;
        constexpr uint32_t kBacklog = 50;
        for (uint32_t i = 0; i < kM + kBacklog; ++i)
        {
            auto pkt = MakeItem(Ipv4Header::DscpDefault, Ipv4Header::ECN_ECT1);
            bool ok = disc->Enqueue(pkt);
            NS_TEST_ASSERT_MSG_EQ(ok, true, "ECT(1) packet must enqueue");
        }
        uint64_t marksB = disc->GetStats().nTotalMarkedPackets;
        for (uint32_t i = 0; i < kM; ++i)
        {
            Ptr<QueueDiscItem> out = disc->Dequeue();
            NS_TEST_ASSERT_MSG_NE(out, nullptr, "ECT(1) packet should dequeue");
        }
        uint64_t marksAft = disc->GetStats().nTotalMarkedPackets;
        NS_TEST_ASSERT_MSG_EQ(marksAft - marksB,
                              kM,
                              "Mark counter must increment by kM for ECT(1) inputs at p_L = 1");
    }
};

/// S-L4S.6: immediate-mark threshold. When the L4S sub-queue head
/// packet has been queued longer than the target, every newly arriving
/// L4S packet is CE-marked (p_L = 1). Sojourn is measured from
/// the per-packet enqueue tag, so simulation time must advance for the
/// head packet to age past the threshold.
class DsL4sImmediateMarkThresholdTest : public TestCase
{
  public:
    DsL4sImmediateMarkThresholdTest()
        : TestCase("L4S immediate-mark step above target sojourn")
    {
    }

    void DoRun() override
    {
        Simulator::Destroy(); // start clean
        auto disc = MakeL4sDisc();
        disc->AssignStreams(13);

        // Pin p' = 0 so the coupled branch contributes nothing; only the
        // immediate-mark step can produce marks.
        disc->ForceBaseProbForTest(0.0);
        disc->SetL4sTargetSojournMs(1.0);

        // Enqueue kN packets at t = 0 so each carries the same enqueue stamp.
        constexpr uint32_t kN = 50;
        for (uint32_t i = 0; i < kN; ++i)
        {
            bool ok = disc->Enqueue(MakeItem(Ipv4Header::DscpDefault, Ipv4Header::ECN_ECT1));
            NS_TEST_ASSERT_MSG_EQ(ok, true, "ECT(1) packet must enqueue");
        }

        // Dequeue them all at t = 10 ms. Each packet's own sojourn (10 ms)
        // exceeds the 1 ms target, so the step branch CE-marks every one
        // on the way out (the step mark is not gated by the two-MTU floor).
        uint32_t dequeued = 0;
        Simulator::Schedule(MilliSeconds(10), [&disc, &dequeued]() {
            while (Ptr<QueueDiscItem> out = disc->Dequeue())
            {
                ++dequeued;
            }
        });
        uint64_t marksBefore = disc->GetStats().nTotalMarkedPackets;
        Simulator::Stop(MilliSeconds(20));
        Simulator::Run();
        uint64_t marksAfter = disc->GetStats().nTotalMarkedPackets;
        Simulator::Destroy();

        NS_TEST_ASSERT_MSG_EQ(dequeued, kN, "All packets must dequeue");
        NS_TEST_ASSERT_MSG_EQ(marksAfter - marksBefore,
                              kN,
                              "Every packet dequeued above its own target sojourn must be "
                              "CE-marked");
    }
};

/// S-L4S.7: controller step response. Per RFC 9332 App. A.1 Fig. 6
/// line 2, the P.I.² controller integrates the *classic* sub-queue's
/// sojourn against the classic target. With the classic sub-queue
/// holding a packet whose enqueue timestamp ages past that target,
/// the periodic P+I controller drives p' upward across ticks.
/// Verifies (a) p' starts at 0, (b) p' is positive after several ticks
/// of sustained over-target sojourn, (c) p' grows monotonically over
/// a short sequence of samples (the integral term dominates the
/// derivative on a steadily-increasing input).
class DsL4sControllerStepResponseTest : public TestCase
{
  public:
    DsL4sControllerStepResponseTest()
        : TestCase("Controller P+I drives p' upward under sustained over-target "
                   "sojourn")
    {
    }

    void DoRun() override
    {
        Simulator::Destroy();
        auto disc = MakeL4sDisc();
        disc->AssignStreams(17);
        disc->SetControllerInterval(MilliSeconds(16));

        // Pre-load 1 NotECT packet at t=0 into the classic sub-queue
        // (RFC 9332 App. A.1 Fig. 6 line 2: the controller reads classic-queue sojourn,
        // not L4S sojourn — ECT(1) would route to the L4S sub-queue
        // and leave the classic AQM empty, suppressing the controller).
        // Without dequeues, the head sojourn equals the elapsed sim time
        // via the l4s::TimestampTag path in ComputeClassicSojournMs.
        auto warmup = MakeItem(Ipv4Header::DscpDefault, Ipv4Header::ECN_NotECT);
        disc->Enqueue(warmup);

        NS_TEST_ASSERT_MSG_EQ(disc->GetBaseProb(),
                              0.0,
                              "Initial p' must be zero before any controller tick");

        // Sample p' at three points to check monotone growth.
        double p1 = 0.0;
        double p2 = 0.0;
        double p3 = 0.0;
        Simulator::Schedule(MilliSeconds(20), [&]() { p1 = disc->GetBaseProb(); });
        Simulator::Schedule(MilliSeconds(80), [&]() { p2 = disc->GetBaseProb(); });
        Simulator::Schedule(MilliSeconds(160), [&]() { p3 = disc->GetBaseProb(); });
        Simulator::Stop(MilliSeconds(200));
        Simulator::Run();
        Simulator::Destroy();

        NS_TEST_ASSERT_MSG_GT(p1, 0.0, "p' must be positive after the first tick over the target");
        NS_TEST_ASSERT_MSG_GT(p2,
                              p1,
                              "p' must keep growing while sojourn keeps growing past target");
        NS_TEST_ASSERT_MSG_GT(p3,
                              p2,
                              "p' must keep growing across additional ticks under "
                              "sustained overload");
        NS_TEST_ASSERT_MSG_LT(p3, 1.0, "p' must remain in [0, 1] (clamp invariant)");
    }
};

/// S-L4S.8: controller stays at zero with no L4S
/// traffic. Periodic ticks fire 60 times across 1 second; with the
/// L4S sub-queue empty throughout, the proportional term equals
/// alpha * (-target) which is negative — clamped to zero. p' must
/// never drift positive.
class DsL4sControllerNoDriftTest : public TestCase
{
  public:
    DsL4sControllerNoDriftTest()
        : TestCase("Controller p' stays at zero with empty L4S queue across many "
                   "ticks")
    {
    }

    void DoRun() override
    {
        Simulator::Destroy();
        auto disc = MakeL4sDisc();
        disc->AssignStreams(19);

        // No L4S packets ever enqueued.
        Simulator::Stop(Seconds(1.0));
        Simulator::Run();
        double endProb = disc->GetBaseProb();
        Simulator::Destroy();

        NS_TEST_ASSERT_MSG_EQ(endProb,
                              0.0,
                              "p' must remain pinned at 0 when L4S queue stays "
                              "empty (clamped negative error)");
    }
};

/// S-L4S.9: CoupledOnly mode bypasses parent WRED.
/// With p' = 0 (no coupled drops) and qlen well above what would
/// trigger Wred early drops, all classic packets must enqueue. In
/// Wred mode (the default in MakeL4sDisc), the same sequence would
/// see the parent's RIO/WRED early-drop fire once vAve grows.
class DsL4sCoupledOnlyBypassWredTest : public TestCase
{
  public:
    DsL4sCoupledOnlyBypassWredTest()
        : TestCase("CoupledOnly mode bypasses parent WRED early drops")
    {
    }

    void DoRun() override
    {
        // Build a disc with CoupledOnly. Note: do NOT call ConfigQueue
        // afterwards on classic queues — the CoupledOnly InitializeParams
        // auto-config (DROP_TAIL with pass-through thresholds) is the
        // contract, and post-Initialize ConfigQueue would overwrite it.
        auto disc = CreateObject<l4s::QueueDisc>();
        disc->SetNumQueues(2);
        disc->SetL4sQueueIdx(1);
        disc->SetClassicAqm(l4s::QueueDisc::ClassicAqm::CoupledOnly);
        disc->SetQueueLimit(0, 200);
        disc->SetQueueLimit(1, 200);
        disc->AddPhbEntry(0, 0, 0);
        Ptr<PriorityScheduler> sched =
            CreateObjectWithAttributes<PriorityScheduler>("NumQueues", UintegerValue(2));
        disc->SetScheduler(sched);
        disc->Initialize();

        // L4S queue still needs explicit thresholds (it's not classic
        // and InitializeParams doesn't touch it).
        disc->ConfigQueue({.queue = 1, .prec = 0, .thMin = 100.0, .thMax = 200.0, .maxP = 0.1});

        disc->AssignStreams(23);
        disc->ForceBaseProbForTest(0.0); // no coupled drops

        constexpr uint32_t kN = 100;
        uint32_t enqueued = 0;
        for (uint32_t i = 0; i < kN; ++i)
        {
            auto pkt = MakeItem(Ipv4Header::DscpDefault, Ipv4Header::ECN_NotECT);
            if (disc->Enqueue(pkt))
            {
                ++enqueued;
            }
        }
        NS_TEST_ASSERT_MSG_EQ(enqueued,
                              kN,
                              "CoupledOnly + p'=0 must accept every classic packet "
                              "up to physical buffer limit");

        // The classic AQM is composer child 1, retrieved via the
        // typed accessor.
        Ptr<QueueDisc> classicQ = disc->GetClassicAqmDisc();
        NS_TEST_ASSERT_MSG_EQ(classicQ->GetNPackets(),
                              kN,
                              "Classic AQM should hold all kN packets in CoupledOnly mode");
    }
};

/// S-L4S.10: coupled scheduler honours L4S priority
/// up to the burst cap, then forces a classic dequeue. Verifies that
/// (a) all enqueued packets are eventually dequeued, (b) no run of
/// L4S dequeues exceeds burstCap consecutive entries while a classic
/// queue has packets, (c) the diagnostic forced-classic counter
/// matches the number of cap-driven preemptions, and (d) when the
/// classic queue empties, the L4S queue can drain freely.
class DsL4sCoupledSchedulerStarvationTest : public TestCase
{
  public:
    DsL4sCoupledSchedulerStarvationTest()
        : TestCase("Coupled scheduler enforces classic dequeues at burst-cap "
                   "boundaries")
    {
    }

    void DoRun() override
    {
        constexpr uint32_t kNumQueues = 2;
        constexpr uint32_t kL4sIdx = 1;
        constexpr uint32_t kBurst = 3;

        Ptr<l4s::CoupledScheduler> sched =
            CreateObjectWithAttributes<l4s::CoupledScheduler>("NumQueues",
                                                              UintegerValue(kNumQueues),
                                                              "L4sQueueIdx",
                                                              UintegerValue(kL4sIdx),
                                                              "BurstCap",
                                                              UintegerValue(kBurst));

        // Enqueue 12 L4S, 4 classic. With burst cap 3, expected
        // dequeue pattern: every 3rd L4S triggers a classic until
        // classics run out, then remaining L4S drain freely.
        constexpr uint32_t kL4sIn = 12;
        constexpr uint32_t kClassicIn = 4;
        for (uint32_t i = 0; i < kL4sIn; ++i)
        {
            sched->OnEnqueue(kL4sIdx, 1500);
        }
        for (uint32_t i = 0; i < kClassicIn; ++i)
        {
            sched->OnEnqueue(0, 1500);
        }

        std::vector<int> sequence;
        sequence.reserve(kL4sIn + kClassicIn);
        while (true)
        {
            int q = sched->SelectNextQueue();
            if (q < 0)
            {
                break;
            }
            sequence.push_back(q);
        }

        // (a) All packets dequeued exactly once.
        NS_TEST_ASSERT_MSG_EQ(sequence.size(),
                              kL4sIn + kClassicIn,
                              "All enqueued packets must be dequeued");
        uint32_t l4sOut = 0;
        uint32_t classicOut = 0;
        for (int q : sequence)
        {
            if (static_cast<uint32_t>(q) == kL4sIdx)
            {
                ++l4sOut;
            }
            else
            {
                ++classicOut;
            }
        }
        NS_TEST_ASSERT_MSG_EQ(l4sOut, kL4sIn, "L4S dequeue count must match enqueue count");
        NS_TEST_ASSERT_MSG_EQ(classicOut,
                              kClassicIn,
                              "Classic dequeue count must match enqueue count");

        // (b) No L4S run exceeds burstCap *while* a classic queue is
        // still backlogged. Once classics are exhausted, the L4S
        // remainder drains freely (run length unbounded).
        uint32_t classicRemaining = kClassicIn;
        uint32_t currentL4sRun = 0;
        for (int q : sequence)
        {
            if (static_cast<uint32_t>(q) == kL4sIdx)
            {
                ++currentL4sRun;
                NS_TEST_ASSERT_MSG_LT_OR_EQ(
                    currentL4sRun,
                    kBurst,
                    "L4S run cannot exceed burst cap while classic has packets");
            }
            else
            {
                NS_TEST_ASSERT_MSG_GT(classicRemaining,
                                      0U,
                                      "Cannot dequeue classic with none enqueued");
                --classicRemaining;
                currentL4sRun = 0;
                if (classicRemaining == 0)
                {
                    break;
                }
            }
        }

        // (c) Forced-classic counter matches the number of preemptions
        // observed: each classic dequeue while L4S was backlogged was
        // forced (with kClassicIn=4 and L4S always available, all 4
        // classic dequeues are forced).
        NS_TEST_ASSERT_MSG_EQ(sched->GetForcedClassicCount(),
                              kClassicIn,
                              "Forced-classic counter must equal classic dequeues "
                              "(L4S was backlogged for all of them)");
    }
};

/// S-L4S.11: coupled scheduler does not deadlock when only L4S
/// has traffic. The burst cap must not prevent L4S from draining
/// completely if classic is empty.
class DsL4sCoupledSchedulerL4sOnlyTest : public TestCase
{
  public:
    DsL4sCoupledSchedulerL4sOnlyTest()
        : TestCase("Coupled scheduler drains L4S freely when classic is empty")
    {
    }

    void DoRun() override
    {
        Ptr<l4s::CoupledScheduler> sched =
            CreateObjectWithAttributes<l4s::CoupledScheduler>("NumQueues",
                                                              UintegerValue(2),
                                                              "L4sQueueIdx",
                                                              UintegerValue(1),
                                                              "BurstCap",
                                                              UintegerValue(2)); // burst cap = 2

        constexpr uint32_t kN = 20;
        for (uint32_t i = 0; i < kN; ++i)
        {
            sched->OnEnqueue(1, 1500);
        }

        uint32_t served = 0;
        while (true)
        {
            int q = sched->SelectNextQueue();
            if (q < 0)
            {
                break;
            }
            NS_TEST_ASSERT_MSG_EQ(q, 1, "Only L4S queue should be served");
            ++served;
        }
        NS_TEST_ASSERT_MSG_EQ(served, kN, "All L4S packets must drain even past the burst cap");
        NS_TEST_ASSERT_MSG_EQ(sched->GetForcedClassicCount(),
                              0U,
                              "Forced-classic counter stays at 0 when classic queue is empty");
    }
};

/// S-L4S.12: FqCoDelQueueDisc as inner classic AQM.
///
/// Exercises composition when `ClassicAqm::FqCoDel` is selected. The
/// disc must: (a) substitute an `FqCoDelQueueDisc` for the default
/// `stratum::RedQueueDisc`, (b) route ECT(1)/CE to the L4S FIFO and NotECT
/// to the FqCoDel inner, (c) aggregate drops via ns-3's standard
/// `ChildQueueDiscDropFunctor` so they appear in the composer's stats,
/// and (d) fire coupled p_C drops inner-agnostically when p' > 0.
///
/// The test does NOT call any Red-specific forwarder (AddPhbEntry,
/// ConfigQueue, SetNumQueues, SetMredMode, SetNumPrec,
/// SetMeanPacketSize, SetQueueBandwidth). Those are documented as
/// Red-only and would assert on the foreign inner.
class DsL4sFqCoDelInnerAqmTest : public TestCase
{
  public:
    DsL4sFqCoDelInnerAqmTest()
        : TestCase("FqCoDelQueueDisc as inner classic AQM: routing, coupled "
                   "drop, drop aggregation")
    {
    }

    void DoRun() override
    {
        auto disc = CreateObject<l4s::QueueDisc>();
        disc->SetL4sQueueIdx(1);

        // Pre-build the FqCoDel inner ourselves so we can seed the
        // quantum. In an on-device deployment FqCoDel auto-sets the
        // quantum from the NetDevice MTU; in this headless test there
        // is no device, so we call SetQuantum() directly.
        Ptr<FqCoDelQueueDisc> fq = CreateObject<FqCoDelQueueDisc>();
        fq->SetQuantum(1500);
        disc->SetClassicAqmDisc(fq);

        // SetQueueLimit(1, ...) is safe against any inner (it only
        // touches the L4S FIFO child). SetQueueLimit on the classic
        // slot is Red-only and must NOT be called here.
        disc->SetQueueLimit(1, 10000);

        Ptr<PriorityScheduler> sched =
            CreateObjectWithAttributes<PriorityScheduler>("NumQueues", UintegerValue(2));
        disc->SetScheduler(sched);

        disc->Initialize();

        // Inner classic AQM must be an FqCoDelQueueDisc, not RED.
        Ptr<QueueDisc> classicQ = disc->GetClassicAqmDisc();
        NS_TEST_ASSERT_MSG_NE(DynamicCast<FqCoDelQueueDisc>(classicQ),
                              nullptr,
                              "Inner classic AQM must be an FqCoDelQueueDisc");
        NS_TEST_ASSERT_MSG_EQ(DynamicCast<stratum::RedQueueDisc>(classicQ),
                              nullptr,
                              "Inner classic AQM must not be a RedQueueDisc");

        Ptr<QueueDisc> l4sQ = disc->GetL4sQueueDisc();
        NS_TEST_ASSERT_MSG_NE(l4sQ, nullptr, "L4S lane must be initialised");
        if (!l4sQ)
        {
            Simulator::Destroy();
            return;
        }

        // (b) Routing by ECN codepoint: ECT(1) / CE to L4S, NotECT / ECT(0)
        // to FqCoDel. Differentiate the flows by source address so
        // FqCoDel's flow-hashing doesn't collapse them into the same
        // inner bucket, which would still be correct but harder to
        // observe with GetNPackets() on the composite inner.
        disc->AssignStreams(29);
        disc->ForceBaseProbForTest(0.0); // no coupled drops in this first phase

        constexpr uint32_t kNClassic = 20;
        constexpr uint32_t kNL4s = 20;
        for (uint32_t i = 0; i < kNClassic; ++i)
        {
            auto pkt = MakeItem(Ipv4Header::DscpDefault, Ipv4Header::ECN_NotECT);
            bool ok = disc->Enqueue(pkt);
            NS_TEST_ASSERT_MSG_EQ(ok,
                                  true,
                                  "Classic (NotECT) packet must enqueue on FqCoDel inner");
        }
        for (uint32_t i = 0; i < kNL4s; ++i)
        {
            auto pkt = MakeItem(Ipv4Header::DscpDefault, Ipv4Header::ECN_ECT1);
            bool ok = disc->Enqueue(pkt);
            NS_TEST_ASSERT_MSG_EQ(ok, true, "L4S (ECT(1)) packet must enqueue on FIFO lane");
        }

        // FqCoDel aggregates packets across its flow buckets into
        // GetNPackets(); the total must match kNClassic.
        NS_TEST_ASSERT_MSG_EQ(classicQ->GetNPackets(),
                              kNClassic,
                              "All NotECT packets must land on the FqCoDel inner");
        NS_TEST_ASSERT_MSG_EQ(l4sQ->GetNPackets(),
                              kNL4s,
                              "All ECT(1) packets must land on the L4S lane");

        // (d) Coupled-drop firing is inner-agnostic. Pin p' = 1 so every
        // classic enqueue draws a coupled drop: p_C = p'^2 = 1 (RFC 9332
        // App. A.1 Fig. 6 line 5). (Recalibrated 2026-06-10: previously
        // pinned p' = 0.5 under the (k*p')^2 misreading, where
        // (2*0.5)^2 = 1; under the RFC map 0.5^2 = 0.25 is stochastic.)
        disc->ForceBaseProbForTest(1.0);

        uint32_t classicDrops = 0;
        constexpr uint32_t kCouplingProbe = 50;
        for (uint32_t i = 0; i < kCouplingProbe; ++i)
        {
            auto pkt = MakeItem(Ipv4Header::DscpDefault, Ipv4Header::ECN_NotECT);
            if (!disc->Enqueue(pkt))
            {
                ++classicDrops;
            }
        }
        NS_TEST_ASSERT_MSG_EQ(classicDrops,
                              kCouplingProbe,
                              "Every classic packet must be coupled-dropped at p' = 1 (p_C = 1)");

        // (c) Drop accounting: coupled drops appear under
        // "L4S_COUPLED_DROP" in the composer's per-reason map.
        const auto& dropMap = disc->GetStats().nDroppedPacketsBeforeEnqueue;
        auto it = dropMap.find("L4S_COUPLED_DROP");
        NS_TEST_ASSERT_MSG_EQ(it != dropMap.end(),
                              true,
                              "L4S_COUPLED_DROP reason must appear in stats map");
        NS_TEST_ASSERT_MSG_EQ(it->second,
                              kCouplingProbe,
                              "L4S_COUPLED_DROP counter must equal coupling-probe count");

        // Inner FqCoDel stats survive composition: GetNPackets is a
        // live read of its flow buckets. L4S FIFO still holds all
        // kNL4s packets (no coupled drop on the L4S side).
        NS_TEST_ASSERT_MSG_EQ(
            classicQ->GetNPackets(),
            kNClassic,
            "FqCoDel inner must retain kNClassic packets after coupled-drop probe "
            "(coupled drops are composer-level, never reach the inner)");
        NS_TEST_ASSERT_MSG_EQ(l4sQ->GetNPackets(), kNL4s, "L4S lane must retain kNL4s packets");
    }
};

/// S-L4S.13: golden controller vector. The PI² controller's arithmetic
/// is pinned to RFC 9332 Appendix A.1, never to this implementation:
///
///   Fig. 2 line 8 : target = 15 ms
///   Fig. 2 line 13: alpha = 0.1 * Tupdate / RTT_max^2 -> 0.16 Hz
///   Fig. 2 line 14: beta  = 0.3 / RTT_max             -> 3.2 Hz
///                   (RFC-stated defaults for Tupdate = 16 ms,
///                   RTT_max = 100 ms: "alpha = 0.16; beta = 3.2")
///   Fig. 6 line 2 : curq = cq.time()      (classic head sojourn)
///   Fig. 6 line 3 : p' += alpha*(curq - target) + beta*(curq - prevq)
///   Fig. 6 line 4 : p_CL = k * p'
///   Fig. 6 line 5 : p_C  = p'^2
///
/// Part A drives a deterministic sojourn ramp (one classic packet
/// enqueued at t = 24 ms, never dequeued; ticks at t = 16n ms, so
/// curq = (16n - 24) ms after an empty first tick that samples
/// prevq = 0) and asserts p' per tick against the hand-derived table.
/// Part B asserts the Fig. 6 line 4-5 maps at forced p' values,
/// including a non-default coupling factor k = 4 — the cascade must
/// honour the configured k, with k scaling p_L and absent from p_C.
class TestSL4s13GoldenControllerVector : public TestCase
{
  public:
    TestSL4s13GoldenControllerVector()
        : TestCase("S-L4S.13: PI2 controller matches the RFC 9332 App. A.1 golden vector")
    {
    }

    void DoRun() override
    {
        // ---- Part A: 10-tick PI evolution under a deterministic ramp ----
        Simulator::Destroy();
        auto disc = MakeL4sDisc();
        disc->AssignStreams(23);
        disc->SetControllerInterval(MilliSeconds(16));

        // One classic NotECT packet at t = 24 ms, never dequeued. Ticks
        // run at t = 16n ms: tick 1 sees an empty queue (curq = 0,
        // prevq sampled 0 — both the RFC prevq-initialised-to-0 reading
        // and a no-previous-sample reading agree from tick 2 onward, by
        // construction); every later tick sees curq = (16n - 24) ms.
        Simulator::Schedule(MilliSeconds(24), [&disc]() {
            auto pkt = MakeItem(Ipv4Header::DscpDefault, Ipv4Header::ECN_NotECT);
            disc->Enqueue(pkt);
        });

        constexpr uint32_t kTicks = 10;
        std::vector<double> probe(kTicks, -1.0);
        for (uint32_t n = 1; n <= kTicks; ++n)
        {
            // +500 us: strictly after tick n, strictly before tick n+1.
            Simulator::Schedule(MilliSeconds(16 * n) + MicroSeconds(500),
                                [&probe, &disc, n]() { probe[n - 1] = disc->GetBaseProb(); });
        }
        Simulator::Stop(MilliSeconds(170));
        Simulator::Run();
        Simulator::Destroy();

        // Golden p' table, derived from the RFC equations above (e.g.
        // tick 2: p' = 0.16*(0.008 - 0.015) + 3.2*(0.008 - 0) = 0.02448;
        // tick 3: + 0.16*(0.024 - 0.015) + 3.2*0.016 = 0.07712; ...).
        constexpr double kGolden[kTicks] = {0.0,
                                            0.02448,
                                            0.07712,
                                            0.13232,
                                            0.19008,
                                            0.25040,
                                            0.31328,
                                            0.37872,
                                            0.44672,
                                            0.51728};
        // Tolerance rationale: this path is RNG-free and ns-3 Time is
        // exact in integer ns, so the only error source is double
        // accumulation over ~30 FLOPs (~1e-15 absolute). 1e-9 is six
        // orders of headroom — a floating-point allowance, not a
        // statistical band.
        constexpr double kTol = 1e-9;
        for (uint32_t n = 1; n <= kTicks; ++n)
        {
            NS_TEST_ASSERT_MSG_EQ_TOL(probe[n - 1],
                                      kGolden[n - 1],
                                      kTol,
                                      "p' at tick " << n
                                                    << " must match the RFC 9332 App. A.1 "
                                                       "golden vector (alpha = 0.16, beta = 3.2)");
        }

        // ---- Part B: probability maps at forced p' (Fig. 6 lines 4-5) ----
        struct MapProbe
        {
            double pPrime;
            double k;
            double expPc; // p'^2 — k must NOT appear here
            double expPl; // min(k * p', 1)
        };

        constexpr MapProbe kMapProbes[] = {
            {0.2, 2.0, 0.04, 0.4},
            {0.2, 4.0, 0.04, 0.8},
            {0.6, 2.0, 0.36, 1.0},
        };

        // The p_C snapshot is set at a classic enqueue and the p_L snapshot
        // at an L4S dequeue (App. A.1 step AQM applies the L4S mark on the
        // way out). Both coupled snapshots are gated by the two-MTU floor, so
        // each lane is pre-filled above the floor with the coupled signal off
        // before the probe forces p'. Separate discs per lane keep the
        // priority scheduler from serving the classic backlog when the L4S
        // dequeue probe runs.
        constexpr uint32_t kFill = 20;
        for (const auto& mp : kMapProbes)
        {
            // p_C snapshot: classic lane above the floor.
            auto dc = MakeL4sDisc();
            dc->AssignStreams(29);
            dc->SetCouplingFactor(mp.k);
            dc->ForceBaseProbForTest(0.0);
            for (uint32_t i = 0; i < kFill; ++i)
            {
                dc->Enqueue(MakeItem(Ipv4Header::DscpDefault, Ipv4Header::ECN_NotECT));
            }
            dc->ForceBaseProbForTest(mp.pPrime);
            dc->Enqueue(MakeItem(Ipv4Header::DscpDefault, Ipv4Header::ECN_NotECT));
            NS_TEST_ASSERT_MSG_EQ_TOL(dc->GetLastClassicCoupledProb(),
                                      mp.expPc,
                                      kTol,
                                      "p_C must equal p'^2 (RFC 9332 Fig. 6 line 5) at p' = "
                                          << mp.pPrime << ", k = " << mp.k);

            // p_L snapshot: L4S lane above the floor, mark applied at dequeue.
            auto dl = MakeL4sDisc();
            dl->AssignStreams(53);
            dl->SetL4sTargetSojournMs(1e6); // keep the step branch inert
            dl->SetCouplingFactor(mp.k);
            dl->ForceBaseProbForTest(0.0);
            for (uint32_t i = 0; i < kFill; ++i)
            {
                dl->Enqueue(MakeItem(Ipv4Header::DscpDefault, Ipv4Header::ECN_ECT1));
            }
            dl->ForceBaseProbForTest(mp.pPrime);
            Ptr<QueueDiscItem> out = dl->Dequeue();
            NS_TEST_ASSERT_MSG_NE(out, nullptr, "an L4S packet must dequeue to refresh p_L");
            NS_TEST_ASSERT_MSG_EQ_TOL(dl->GetLastL4sMarkProb(),
                                      mp.expPl,
                                      kTol,
                                      "p_L must equal min(k * p', 1) (RFC 9332 Fig. 6 line 4) "
                                      "at p' = "
                                          << mp.pPrime << ", k = " << mp.k);
        }
        Simulator::Destroy();

        // S-L4S.13SUM,<p'@tick2>,<p'@tick10>,<pC at p'=0.2 k=2>,<pL at p'=0.2 k=4> — audit harvest
        std::ostringstream sl4s13Sum;
        sl4s13Sum << "S-L4S.13SUM," << probe[1] << "," << probe[9] << "," << kMapProbes[0].expPc
                  << "," << kMapProbes[1].expPl;
        std::cout << sl4s13Sum.str() << std::endl;
    }
};

/// S-L4S.14: ECN codepoint-transition vector. RFC 9331 §5 permits the
/// network to set CE on ECT(1) packets; nothing in the DualQ may demote
/// ECT(1), re-mark a CE packet, or CE-mark the classic coupled path
/// (which drops instead — drop-not-mark). Each row uses a fresh disc
/// and a forced controller state chosen so the outcome is
/// deterministic (p_L or p_C pinned to 0 or 1). The L4S CE mark is
/// applied at dequeue (App. A.1 step AQM), so the L4S rows dequeue
/// through the composer; the coupled mark/drop is gated by the two-MTU
/// floor, so those rows pre-fill the lane above the floor with the
/// coupled signal off. Classic rows read the classic child directly —
/// the classic lane is never marked.
class TestSL4s14EcnCodepointTransitions : public TestCase
{
  public:
    TestSL4s14EcnCodepointTransitions()
        : TestCase("S-L4S.14: ECN codepoint transitions per RFC 9331 (CE-only, no demotion, "
                   "classic drops)")
    {
    }

    void DoRun() override
    {
        Simulator::Destroy();

        constexpr uint32_t kFill = 20; // packets to clear the two-MTU floor

        // Row 1: ECT(1) at p_L = 1 (p' = 0.6, k = 2 -> min(1.2, 1) = 1):
        // forwarded with CE set. Pre-fill the L4S lane above the floor so the
        // coupled mark fires; the dequeued image (any above-floor ECT(1)
        // packet) must carry CE. The mark draw is deterministic because
        // UniformRandomVariable::GetValue(0, 1) < 1.0 always holds.
        {
            auto d = MakeL4sDisc();
            d->AssignStreams(31);
            d->SetL4sTargetSojournMs(1e6);
            d->ForceBaseProbForTest(0.0);
            for (uint32_t i = 0; i < kFill; ++i)
            {
                d->Enqueue(MakeItem(Ipv4Header::DscpDefault, Ipv4Header::ECN_ECT1));
            }
            d->ForceBaseProbForTest(0.6);
            Ptr<QueueDiscItem> out = d->Dequeue();
            NS_TEST_ASSERT_MSG_NE(out, nullptr, "L4S lane must hold a markable packet");
            auto ip = DynamicCast<const Ipv4QueueDiscItem>(Ptr<const QueueDiscItem>(out));
            NS_TEST_ASSERT_MSG_EQ(ip->GetHeader().GetEcn(),
                                  Ipv4Header::ECN_CE,
                                  "marked ECT(1) packet must carry CE (RFC 9331 §5)");
        }

        // Row 2: ECT(1) at p_L = 0 (p' = 0): forwarded untouched —
        // never demoted to ECT(0)/NotECT.
        {
            auto d = MakeL4sDisc();
            d->AssignStreams(31);
            d->ForceBaseProbForTest(0.0);
            d->SetL4sTargetSojournMs(1e6);
            bool ok = d->Enqueue(MakeItem(Ipv4Header::DscpDefault, Ipv4Header::ECN_ECT1));
            NS_TEST_ASSERT_MSG_EQ(ok, true, "ECT(1) must enqueue at p_L = 0");
            Ptr<QueueDiscItem> out = d->Dequeue();
            NS_TEST_ASSERT_MSG_NE(out, nullptr, "L4S lane must hold the packet");
            auto ip = DynamicCast<const Ipv4QueueDiscItem>(Ptr<const QueueDiscItem>(out));
            NS_TEST_ASSERT_MSG_EQ(ip->GetHeader().GetEcn(),
                                  Ipv4Header::ECN_ECT1,
                                  "unmarked ECT(1) must stay ECT(1) — demotion forbidden "
                                  "(RFC 9331 §5)");
        }

        // Row 3: CE in -> CE out at p_L = 1 (idempotence on the dequeue
        // image; the counter half of this property is S-L4S.5). Above the
        // floor at p_L = 1 the mark path runs but must not re-mark CE.
        {
            auto d = MakeL4sDisc();
            d->AssignStreams(31);
            d->SetL4sTargetSojournMs(1e6);
            d->ForceBaseProbForTest(0.0);
            for (uint32_t i = 0; i < kFill; ++i)
            {
                d->Enqueue(MakeItem(Ipv4Header::DscpDefault, Ipv4Header::ECN_CE));
            }
            d->ForceBaseProbForTest(0.6);
            Ptr<QueueDiscItem> out = d->Dequeue();
            NS_TEST_ASSERT_MSG_NE(out, nullptr, "L4S lane must hold the CE packet");
            auto ip = DynamicCast<const Ipv4QueueDiscItem>(Ptr<const QueueDiscItem>(out));
            NS_TEST_ASSERT_MSG_EQ(ip->GetHeader().GetEcn(),
                                  Ipv4Header::ECN_CE,
                                  "CE packet must stay CE (RFC 9331 §5 idempotence)");
        }

        // Rows 4 + 5: classic NotECT / ECT(0) at p_C = 1 (p' = 1):
        // coupled-dropped before the child enqueue — never CE-flipped. The
        // classic lane is pre-filled above the floor (coupled signal off) so
        // the coupled drop fires; the probe packet must not survive into the
        // classic child.
        for (auto ecn : {Ipv4Header::ECN_NotECT, Ipv4Header::ECN_ECT0})
        {
            auto d = MakeL4sDisc();
            d->AssignStreams(31);
            d->ForceBaseProbForTest(0.0);
            for (uint32_t i = 0; i < kFill; ++i)
            {
                d->Enqueue(MakeItem(Ipv4Header::DscpDefault, ecn));
            }
            d->ForceBaseProbForTest(1.0);
            uint32_t dropsBefore = d->GetStats().nTotalDroppedPacketsBeforeEnqueue;
            bool ok = d->Enqueue(MakeItem(Ipv4Header::DscpDefault, ecn));
            NS_TEST_ASSERT_MSG_EQ(ok,
                                  false,
                                  "classic packet at p_C = 1 must be coupled-dropped "
                                  "(drop-not-mark)");
            NS_TEST_ASSERT_MSG_EQ(d->GetStats().nTotalDroppedPacketsBeforeEnqueue - dropsBefore,
                                  1U,
                                  "coupled drop must be accounted before enqueue");
            NS_TEST_ASSERT_MSG_EQ(d->GetClassicAqmDisc()->GetNPackets(),
                                  kFill,
                                  "the coupled-dropped packet must not survive into the "
                                  "classic child (drop-not-mark)");
        }

        // Row 6: classic ECT(0) at p_C = 0 (p' = 0): forwarded untouched.
        {
            auto d = MakeL4sDisc();
            d->AssignStreams(31);
            d->ForceBaseProbForTest(0.0);
            bool ok = d->Enqueue(MakeItem(Ipv4Header::DscpDefault, Ipv4Header::ECN_ECT0));
            NS_TEST_ASSERT_MSG_EQ(ok, true, "ECT(0) must enqueue at p_C = 0");
            Ptr<QueueDiscItem> out = d->GetClassicAqmDisc()->Dequeue();
            NS_TEST_ASSERT_MSG_NE(out, nullptr, "classic lane must hold the packet");
            auto ip = DynamicCast<const Ipv4QueueDiscItem>(Ptr<const QueueDiscItem>(out));
            NS_TEST_ASSERT_MSG_EQ(ip->GetHeader().GetEcn(),
                                  Ipv4Header::ECN_ECT0,
                                  "classic ECT(0) must pass through untouched at p_C = 0");
        }

        Simulator::Destroy();
        // S-L4S.14SUM,rows=6 — audit harvest (all-deterministic vector)
        std::cout << "S-L4S.14SUM,rows=6" << std::endl;
    }
};

/// S-L4S.15: DSCP preservation through both lanes (behavioural).
/// draft-briscoe-tsvwg-l4s-diffserv §7.1: a DualQ never alters
/// the DSCP of any packet — classification belongs to the outer
/// classifier. Part A forwards {EF, AF11, CS5, default} x
/// {ECT(1), NotECT, ECT(0)} with the controller pinned inactive and
/// asserts the dequeued (DSCP, ECN) multiset equals the enqueued one.
/// Part B repeats the four DSCPs on the L4S lane with CE-marking
/// forced active (p_L = 1) and asserts every packet was marked CE
/// while its DSCP survived the Mark() path unchanged.
class TestSL4s15DscpPreservation : public TestCase
{
  public:
    TestSL4s15DscpPreservation()
        : TestCase("S-L4S.15: DSCP preserved through both lanes, with and without CE-marking")
    {
    }

    void DoRun() override
    {
        Simulator::Destroy();

        constexpr Ipv4Header::DscpType kDscps[] = {Ipv4Header::DscpDefault,
                                                   Ipv4Header::DSCP_AF11,
                                                   Ipv4Header::DSCP_CS5,
                                                   Ipv4Header::DSCP_EF};

        // ---- Part A: controller inactive — pure forwarding ----
        {
            auto d = MakeL4sDisc();
            d->AssignStreams(37);
            d->ForceBaseProbForTest(0.0);
            for (auto dscp : kDscps)
            {
                d->AddPhbEntry(static_cast<uint8_t>(dscp), 0, 0);
            }

            std::multiset<std::pair<int, int>> sent;
            for (auto dscp : kDscps)
            {
                for (auto ecn :
                     {Ipv4Header::ECN_ECT1, Ipv4Header::ECN_NotECT, Ipv4Header::ECN_ECT0})
                {
                    bool ok = d->Enqueue(MakeItem(dscp, ecn));
                    NS_TEST_ASSERT_MSG_EQ(ok, true, "part-A packet must enqueue");
                    sent.insert({static_cast<int>(dscp), static_cast<int>(ecn)});
                }
            }

            std::multiset<std::pair<int, int>> got;
            for (auto child : {d->GetL4sQueueDisc(), d->GetClassicAqmDisc()})
            {
                while (Ptr<QueueDiscItem> out = child->Dequeue())
                {
                    auto ip = DynamicCast<const Ipv4QueueDiscItem>(Ptr<const QueueDiscItem>(out));
                    got.insert({static_cast<int>(ip->GetHeader().GetDscp()),
                                static_cast<int>(ip->GetHeader().GetEcn())});
                }
            }
            NS_TEST_ASSERT_MSG_EQ(got.size(), sent.size(), "part A must dequeue all 12 packets");
            NS_TEST_ASSERT_MSG_EQ((got == sent),
                                  true,
                                  "dequeued (DSCP, ECN) multiset must equal the enqueued one — "
                                  "no DSCP rewrite, no codepoint change with the controller "
                                  "inactive");
        }

        // ---- Part B: CE-marking active on the L4S lane ----
        {
            auto d = MakeL4sDisc();
            d->AssignStreams(37);
            // p' = 0; drive the deterministic step mark, which is independent
            // of the two-MTU floor: enqueue at t = 0 and dequeue past the 1 ms
            // target so p_L = 1 on every packet (App. A.1 step AQM, applied at
            // dequeue through the composer).
            d->ForceBaseProbForTest(0.0);
            d->SetL4sTargetSojournMs(1.0);

            std::multiset<int> sentDscp;
            for (auto dscp : kDscps)
            {
                bool ok = d->Enqueue(MakeItem(dscp, Ipv4Header::ECN_ECT1));
                NS_TEST_ASSERT_MSG_EQ(ok, true, "part-B packet must enqueue");
                sentDscp.insert(static_cast<int>(dscp));
            }

            std::multiset<int> gotDscp;
            uint32_t ceCount = 0;
            Simulator::Schedule(MilliSeconds(10), [&d, &gotDscp, &ceCount]() {
                while (Ptr<QueueDiscItem> out = d->Dequeue())
                {
                    auto ip = DynamicCast<const Ipv4QueueDiscItem>(Ptr<const QueueDiscItem>(out));
                    gotDscp.insert(static_cast<int>(ip->GetHeader().GetDscp()));
                    if (ip->GetHeader().GetEcn() == Ipv4Header::ECN_CE)
                    {
                        ++ceCount;
                    }
                }
            });
            Simulator::Stop(MilliSeconds(20));
            Simulator::Run();
            NS_TEST_ASSERT_MSG_EQ(gotDscp.size(),
                                  sentDscp.size(),
                                  "part B must dequeue all 4 packets");
            NS_TEST_ASSERT_MSG_EQ(ceCount,
                                  static_cast<uint32_t>(sentDscp.size()),
                                  "every ECT(1) packet must be CE-marked at p_L = 1");
            NS_TEST_ASSERT_MSG_EQ((gotDscp == sentDscp),
                                  true,
                                  "DSCP must survive the Mark() path unchanged "
                                  "(atomic-DualQ DSCP preservation)");
        }

        Simulator::Destroy();
        // S-L4S.15SUM,partA=12,partB=4 — audit harvest
        std::cout << "S-L4S.15SUM,partA=12,partB=4" << std::endl;
    }
};

/**
 * @brief On the first PI2 controller tick with a backlogged classic queue, the
 * proportional (beta) term is applied against an initial previous-sojourn of
 * zero, per GPRT and the RFC 9332 App. A.1 pseudocode (prevq = 0).
 *
 * The S-L4S.13 golden vector is built so tick 1 sees an empty queue, so it does
 * not exercise this path. Here a classic packet is enqueued before the first
 * tick: with prevq = 0 the first tick applies beta*(curq - 0), so
 * p' = 0.16*(0.008 - 0.015) + 3.2*(0.008 - 0) = 0.02448. Suppressing the
 * derivative on the first tick would instead leave p' = max(0, -0.00112) = 0.
 */
class TestL4sPi2FirstTickBetaWithBacklog : public TestCase
{
  public:
    TestL4sPi2FirstTickBetaWithBacklog()
        : TestCase("L4S PI2 first tick applies the full beta kick with a backlogged classic queue "
                   "(GPRT / RFC 9332 App. A.1 prevq=0)")
    {
    }

    void DoRun() override
    {
        Simulator::Destroy();
        auto disc = MakeL4sDisc();
        disc->AssignStreams(23);
        disc->SetControllerInterval(MilliSeconds(16));

        // One classic NotECT packet at t = 8 ms (before tick 1 at t = 16 ms),
        // never dequeued: tick 1 sees a non-empty classic queue, sojourn 8 ms.
        Simulator::Schedule(MilliSeconds(8), [&disc]() {
            auto pkt = MakeItem(Ipv4Header::DscpDefault, Ipv4Header::ECN_NotECT);
            disc->Enqueue(pkt);
        });

        double pTick1 = -1.0;
        Simulator::Schedule(MilliSeconds(16) + MicroSeconds(500),
                            [&pTick1, &disc]() { pTick1 = disc->GetBaseProb(); });
        Simulator::Stop(MilliSeconds(20));
        Simulator::Run();
        Simulator::Destroy();

        NS_TEST_ASSERT_MSG_EQ_TOL(pTick1,
                                  0.02448,
                                  1e-9,
                                  "PI2 first tick with a backlogged classic queue must apply "
                                  "beta*(curq - 0) per GPRT / RFC 9332 App. A.1");
    }
};

/// S-L4S.16: step-mark timing. The L4S CE mark is applied when the packet
/// is dequeued, keyed off that packet's own sojourn (RFC 9332 App. A.1
/// StepAqm: qDelay = now - item enqueue time; mark if qDelay > target).
/// A single ECT(1) packet enqueued into an empty queue and held past the
/// target sojourn before dequeue must leave the queue CE-marked. Marking
/// at enqueue (against the head's age) would leave this lone packet
/// unmarked, because at enqueue its own sojourn is zero.
class TestSL4s16DequeueTimeStepMark : public TestCase
{
  public:
    TestSL4s16DequeueTimeStepMark()
        : TestCase("S-L4S.16 L4S step mark applied at dequeue from the packet's own sojourn")
    {
    }

    void DoRun() override
    {
        Simulator::Destroy(); // start clean
        auto disc = MakeL4sDisc();
        disc->AssignStreams(31);

        // Pin p' = 0 so the coupled branch contributes nothing; only the
        // immediate-mark step can mark, and only from the packet's own age.
        disc->ForceBaseProbForTest(0.0);
        disc->SetL4sTargetSojournMs(1.0);

        // One ECT(1) packet enqueued at t = 0; its enqueue timestamp is 0.
        bool ok = disc->Enqueue(MakeItem(Ipv4Header::DscpDefault, Ipv4Header::ECN_ECT1));
        NS_TEST_ASSERT_MSG_EQ(ok, true, "ECT(1) packet must enqueue");

        // Dequeue at t = 2 ms: the packet's own sojourn (2 ms) exceeds the
        // 1 ms target, so the step branch must CE-mark it on the way out.
        Ptr<QueueDiscItem> deq;
        Simulator::Schedule(MilliSeconds(2), [&deq, &disc]() { deq = disc->Dequeue(); });
        Simulator::Stop(MilliSeconds(3));
        Simulator::Run();

        Ptr<Ipv4QueueDiscItem> ip = DynamicCast<Ipv4QueueDiscItem>(deq);
        NS_TEST_ASSERT_MSG_NE(ip, nullptr, "Packet must dequeue");
        if (ip)
        {
            NS_TEST_ASSERT_MSG_EQ(static_cast<int>(ip->GetHeader().GetEcn()),
                                  static_cast<int>(Ipv4Header::ECN_CE),
                                  "L4S packet aged past target must be CE-marked at dequeue "
                                  "from its own sojourn (App. A.1 StepAqm)");
        }
        Simulator::Destroy();
    }
};

/// S-L4S.17: coupled-signal suppression floor. RFC 9332 App. A.1 MustDrop
/// takes no action while the total queue sits below two MTUs
/// (GPRT: m_thLen = 2 * m_mtu). With p' pinned to 1.0 (p_C = 1.0), a
/// classic packet arriving at a near-empty queue must NOT be
/// coupled-dropped; once the backlog crosses two MTUs, the coupled drop
/// fires. The native step branch is disabled here (huge target sojourn)
/// so only the coupled p_C path is exercised.
class TestSL4s17CoupledFloorTwoMtu : public TestCase
{
  public:
    TestSL4s17CoupledFloorTwoMtu()
        : TestCase("S-L4S.17 coupled drop suppressed while total queue below two MTUs")
    {
    }

    void DoRun() override
    {
        auto disc = MakeL4sDisc();
        disc->AssignStreams(37);
        disc->SetL4sTargetSojournMs(1e6); // disable the step branch

        // Below floor: at p' = 1 (p_C = 1.0) a packet arriving at an empty
        // queue must escape the coupled drop, because the backlog is below
        // two MTUs.
        disc->ForceBaseProbForTest(1.0);
        bool okBelow = disc->Enqueue(MakeItem(Ipv4Header::DscpDefault, Ipv4Header::ECN_NotECT));
        NS_TEST_ASSERT_MSG_EQ(okBelow,
                              true,
                              "Coupled drop must be suppressed below the two-MTU floor (p'=1)");

        // Fill the classic backlog past two MTUs with the coupled signal
        // off (p' = 0, p_C = 0) so no drops occur during the fill.
        disc->ForceBaseProbForTest(0.0);
        for (uint32_t i = 0; i < 20; ++i)
        {
            bool ok = disc->Enqueue(MakeItem(Ipv4Header::DscpDefault, Ipv4Header::ECN_NotECT));
            NS_TEST_ASSERT_MSG_EQ(ok, true, "Fill packet must enqueue with coupled signal off");
        }

        // Above floor: re-arm p' = 1 and enqueue one more. The backlog now
        // exceeds two MTUs, so the coupled drop must fire.
        disc->ForceBaseProbForTest(1.0);
        bool okAbove = disc->Enqueue(MakeItem(Ipv4Header::DscpDefault, Ipv4Header::ECN_NotECT));
        NS_TEST_ASSERT_MSG_EQ(okAbove,
                              false,
                              "Coupled drop must fire above the two-MTU floor (p'=1)");

        const auto& nDrops = disc->GetStats().nDroppedPacketsBeforeEnqueue;
        auto it = nDrops.find("L4S_COUPLED_DROP");
        NS_TEST_ASSERT_MSG_EQ(it != nDrops.end() && it->second >= 1,
                              true,
                              "An above-floor coupled drop must be recorded in stats");
        Simulator::Destroy();
    }
};

// Verifies the one-call L4S composer reproduces the native two-lane L4S edge.
class SetAsL4sComposesEdgeTest : public TestCase
{
  public:
    SetAsL4sComposesEdgeTest()
        : TestCase("SetAsL4s composes the native two-lane L4S edge")
    {
    }

    void DoRun() override
    {
        Ptr<ns3::stratum::l4s::QueueDisc> disc = CreateObject<ns3::stratum::l4s::QueueDisc>();
        ns3::stratum::l4s::Helper::SetAsL4s(disc);

        NS_TEST_ASSERT_MSG_EQ(disc->GetNumQueues(), 2u, "L4S edge must have 2 lanes");
        uint8_t q = 0xFF;
        uint8_t p = 0xFF;
        NS_TEST_ASSERT_MSG_EQ(disc->LookupPhb(46, q, p), true, "EF fallback must be mapped");
        NS_TEST_ASSERT_MSG_EQ(q, 0, "EF -> L4S lane (idx 0)");
        NS_TEST_ASSERT_MSG_EQ(disc->LookupPhb(0, q, p), true, "BE must be mapped");
        NS_TEST_ASSERT_MSG_EQ(q, 1, "BE -> classic lane (idx 1)");
        NS_TEST_ASSERT_MSG_NE(disc->GetScheduler(), nullptr, "coupled scheduler must be set");
        Ptr<ns3::stratum::l4s::CoupledScheduler> sched =
            DynamicCast<ns3::stratum::l4s::CoupledScheduler>(disc->GetScheduler());
        NS_TEST_ASSERT_MSG_NE(sched, nullptr, "scheduler must be the L4S CoupledScheduler");
        UintegerValue nq;
        sched->GetAttribute("NumQueues", nq);
        NS_TEST_ASSERT_MSG_EQ(nq.Get(), 2u, "coupled scheduler must serve 2 lanes");
    }
};

class DsL4sQueueDiscSuite : public TestSuite
{
  public:
    DsL4sQueueDiscSuite()
        : TestSuite("stratum-l4s", Type::UNIT)
    {
        AddTestCase(new DsL4sRoutingTest, Duration::QUICK);
        AddTestCase(new DsL4sConfigTest, Duration::QUICK);
        AddTestCase(new DsL4sZeroLoadCouplingTest, Duration::QUICK);
        AddTestCase(new DsL4sSquaredRatioTest, Duration::QUICK);
        AddTestCase(new DsL4sCeIdempotenceTest, Duration::QUICK);
        AddTestCase(new DsL4sImmediateMarkThresholdTest, Duration::QUICK);
        AddTestCase(new DsL4sControllerStepResponseTest, Duration::QUICK);
        AddTestCase(new DsL4sControllerNoDriftTest, Duration::QUICK);
        AddTestCase(new DsL4sCoupledOnlyBypassWredTest, Duration::QUICK);
        AddTestCase(new DsL4sCoupledSchedulerStarvationTest, Duration::QUICK);
        AddTestCase(new DsL4sCoupledSchedulerL4sOnlyTest, Duration::QUICK);
        AddTestCase(new DsL4sFqCoDelInnerAqmTest, Duration::QUICK);
        AddTestCase(new TestSL4s13GoldenControllerVector, Duration::QUICK);
        AddTestCase(new TestL4sPi2FirstTickBetaWithBacklog, Duration::QUICK);
        AddTestCase(new TestSL4s14EcnCodepointTransitions, Duration::QUICK);
        AddTestCase(new TestSL4s15DscpPreservation, Duration::QUICK);
        AddTestCase(new TestSL4s16DequeueTimeStepMark, Duration::QUICK);
        AddTestCase(new TestSL4s17CoupledFloorTwoMtu, Duration::QUICK);
        AddTestCase(new TestQ18v3L4sEctClassifyIPv6, Duration::QUICK);
        AddTestCase(new TestQ18v3L4sCeIdempotenceIPv6, Duration::QUICK);
        AddTestCase(new TestQ18v3L4sMarkingIPv6, Duration::QUICK);
        AddTestCase(new SetAsL4sComposesEdgeTest(), TestCase::Duration::QUICK);
        AddTestCase(new DsL4sScenarioPiControlFiresTest, Duration::EXTENSIVE);
        AddTestCase(new DsL4sScenarioS1LatencyDifferentiationTest, Duration::EXTENSIVE);
        AddTestCase(new DsL4sScenarioS2CoexistenceThroughputEquivalenceTest, Duration::EXTENSIVE);
        AddTestCase(new DsL4sScenarioS1AdvantageLatencyDeltaTest, Duration::EXTENSIVE);
        AddTestCase(new DsL4sScenarioFqCoDelComparisonSmokePerModeTest(), Duration::EXTENSIVE);
        AddTestCase(new DsL4sScenarioFqCoDelClassicCompositionalSafetyTest(), Duration::EXTENSIVE);
        AddTestCase(new DsL4sScenarioDualPi2GprtParityTest(), Duration::EXTENSIVE);
        AddTestCase(new DsL4sScenarioCakeCompositionFairnessTest(), Duration::EXTENSIVE);
        AddTestCase(new DsL4sScenarioCakeCompositionThroughputParityTest(), Duration::EXTENSIVE);
    }
};

DsL4sQueueDiscSuite g_dsL4sSuite;

} // namespace
