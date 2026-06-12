/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * White-box characterisation of the FqCobalt host-isolation bulk-flow
 * accounting timing (S-17.66).
 *
 * A flow joins its host's bulk-flow count only once it has stayed
 * backlogged across a dequeue rotation — the moment DRR exhausts its
 * deficit while packets remain queued. A short burst that drains within
 * one quantum stays sparse for its whole life and never contributes to
 * host_load. These assertions read the per-host bulk counters directly
 * via GetSrchostBulkFlowCountAt / GetDsthostBulkFlowCountAt and drive the
 * disc one packet at a time so the SPARSE->BULK transition is observable
 * in isolation, independent of any simulated topology.
 */

#include "ns3/core-module.h"
#include "ns3/fq-cobalt-queue-disc.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/test.h"
#include "ns3/traffic-control-module.h"

#include <algorithm>
#include <iostream>

namespace ns3::stratum
{

namespace
{
// FqCobalt default flow/host-slot count: GetSrchostBulkFlowCountAt is valid
// over [0, Flows); summing the whole range yields the live bulk population.
constexpr uint32_t kFlows = 1024;
// Small quantum so that a single ~1000-byte packet exhausts a flow's deficit
// in one service, making the cross-rotation backlog condition deterministic.
constexpr uint32_t kQuantum = 300;
} // namespace

/**
 * @brief Asserts host-isolation bulk counts follow the SPARSE->BULK transition.
 *
 * Sub-case A drives one flow that stays backlogged across a rotation and
 * verifies it is counted only after the promotion (not on activation), then
 * released on drain. Sub-case B drives a single sub-quantum packet and
 * verifies the flow stays sparse and is never counted.
 */
class CakeHostBulkStateTimingCase : public TestCase
{
  public:
    CakeHostBulkStateTimingCase()
        : TestCase("S-17.66 host-isolation bulk count tracks the SPARSE->BULK transition")
    {
    }

  private:
    using Mode = FqCobaltQueueDisc::HostIsolationMode;

    /// Build a standalone host-isolating FqCobalt with a small deterministic quantum.
    static Ptr<FqCobaltQueueDisc> MakeDisc(Mode mode);
    /// Build an IPv4/TCP item with a fixed 5-tuple (same tuple -> same flow).
    static Ptr<Ipv4QueueDiscItem> MakeItem(const char* src,
                                           const char* dst,
                                           uint16_t srcPort,
                                           uint32_t payloadBytes);
    /// Live source-side bulk population across all host slots.
    static uint32_t SumSrcBulk(const Ptr<FqCobaltQueueDisc>& q);
    /// Live destination-side bulk population across all host slots.
    static uint32_t SumDstBulk(const Ptr<FqCobaltQueueDisc>& q);

    void DoRun() override;
};

Ptr<FqCobaltQueueDisc>
CakeHostBulkStateTimingCase::MakeDisc(Mode mode)
{
    Ptr<FqCobaltQueueDisc> q = CreateObject<FqCobaltQueueDisc>();
    q->SetAttribute("Flows", UintegerValue(kFlows));
    q->SetAttribute("EnableSetAssociativeHash", BooleanValue(true));
    q->SetAttribute("SetWays", UintegerValue(8));
    q->SetAttribute("EnableHostIsolation", BooleanValue(true));
    q->SetAttribute("HostIsolationMode", EnumValue(mode));
    q->SetAttribute("MaxSize", QueueSizeValue(QueueSize("100000p")));
    q->SetQuantum(kQuantum); // required when no NetDevice is attached
    q->Initialize();
    return q;
}

Ptr<Ipv4QueueDiscItem>
CakeHostBulkStateTimingCase::MakeItem(const char* src,
                                      const char* dst,
                                      uint16_t srcPort,
                                      uint32_t payloadBytes)
{
    Ptr<Packet> packet = Create<Packet>(payloadBytes);
    TcpHeader tcp;
    tcp.SetSourcePort(srcPort);
    tcp.SetDestinationPort(80);
    tcp.SetSequenceNumber(SequenceNumber32(0));
    tcp.SetAckNumber(SequenceNumber32(0));
    tcp.SetWindowSize(1);
    packet->AddHeader(tcp);

    Ipv4Header ipHdr;
    ipHdr.SetSource(Ipv4Address(src));
    ipHdr.SetDestination(Ipv4Address(dst));
    ipHdr.SetProtocol(6);
    ipHdr.SetPayloadSize(packet->GetSize());

    Address from = InetSocketAddress(Ipv4Address(src), srcPort);
    return Create<Ipv4QueueDiscItem>(packet, from, 0x0800, ipHdr);
}

uint32_t
CakeHostBulkStateTimingCase::SumSrcBulk(const Ptr<FqCobaltQueueDisc>& q)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i < kFlows; ++i)
    {
        sum += q->GetSrchostBulkFlowCountAt(i);
    }
    return sum;
}

uint32_t
CakeHostBulkStateTimingCase::SumDstBulk(const Ptr<FqCobaltQueueDisc>& q)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i < kFlows; ++i)
    {
        sum += q->GetDsthostBulkFlowCountAt(i);
    }
    return sum;
}

void
CakeHostBulkStateTimingCase::DoRun()
{
    // --- Sub-case A: a flow backlogged across a rotation MUST be counted. ---
    // Five packets of one 5-tuple form a single flow whose backlog outlives
    // its first quantum.
    Ptr<FqCobaltQueueDisc> q = MakeDisc(Mode::Triple);
    for (uint32_t i = 0; i < 5; ++i)
    {
        q->Enqueue(MakeItem("10.0.0.1", "10.0.0.2", 40000, 1000));
    }

    const uint32_t preRotationSrc = SumSrcBulk(q);
    const uint32_t preRotationDst = SumDstBulk(q);
    std::cout << "HOSTBULK,bulk,pre_rotation,src=" << preRotationSrc << ",dst=" << preRotationDst
              << std::endl;
    // The decisive timing assertion: after enqueue but before any dequeue
    // rotation the flow is still sparse, so it contributes nothing to either
    // host's bulk load.
    NS_TEST_ASSERT_MSG_EQ(preRotationSrc,
                          0u,
                          "flow counted toward src-host bulk load before staying "
                          "backlogged across a rotation");
    NS_TEST_ASSERT_MSG_EQ(preRotationDst,
                          0u,
                          "flow counted toward dst-host bulk load before staying "
                          "backlogged across a rotation");

    // Drain one packet at a time. The bulk population must rise to exactly one
    // (a single backlogged flow) at the SPARSE->BULK promotion and fall back to
    // zero once the flow fully drains.
    uint32_t maxSrc = preRotationSrc;
    uint32_t maxDst = preRotationDst;
    Ptr<QueueDiscItem> item;
    uint32_t guard = 0;
    while (guard++ < 10000 && (item = q->Dequeue()))
    {
        maxSrc = std::max(maxSrc, SumSrcBulk(q));
        maxDst = std::max(maxDst, SumDstBulk(q));
    }
    const uint32_t postDrainSrc = SumSrcBulk(q);
    const uint32_t postDrainDst = SumDstBulk(q);
    std::cout << "HOSTBULK,bulk,max_during_drain,src=" << maxSrc << ",dst=" << maxDst << std::endl;
    std::cout << "HOSTBULK,bulk,post_drain,src=" << postDrainSrc << ",dst=" << postDrainDst
              << std::endl;
    NS_TEST_ASSERT_MSG_EQ(maxSrc,
                          1u,
                          "backlogged flow never reached its src-host bulk count across "
                          "the rotation");
    NS_TEST_ASSERT_MSG_EQ(maxDst,
                          1u,
                          "backlogged flow never reached its dst-host bulk count across "
                          "the rotation");
    NS_TEST_ASSERT_MSG_EQ(postDrainSrc, 0u, "src-host bulk count not released on drain");
    NS_TEST_ASSERT_MSG_EQ(postDrainDst, 0u, "dst-host bulk count not released on drain");

    // --- Sub-case B: a flow sparse for its whole life MUST NOT be counted. ---
    // One sub-quantum packet drains within its first service, so the deficit
    // never goes non-positive while packets remain: the flow stays sparse.
    Ptr<FqCobaltQueueDisc> q2 = MakeDisc(Mode::Triple);
    q2->Enqueue(MakeItem("10.0.0.5", "10.0.0.6", 40001, 100));

    const uint32_t sparsePre = SumSrcBulk(q2);
    std::cout << "HOSTBULK,sparse,pre_dequeue,src=" << sparsePre << std::endl;
    NS_TEST_ASSERT_MSG_EQ(sparsePre,
                          0u,
                          "single sub-quantum sparse flow counted toward host bulk load "
                          "on activation");

    uint32_t sparseMax = sparsePre;
    guard = 0;
    while (guard++ < 10000 && (item = q2->Dequeue()))
    {
        sparseMax = std::max(sparseMax, SumSrcBulk(q2));
    }
    const uint32_t sparseEnd = SumSrcBulk(q2);
    std::cout << "HOSTBULK,sparse,max_during_drain,src=" << sparseMax << std::endl;
    std::cout << "HOSTBULK,sparse,post_drain,src=" << sparseEnd << std::endl;
    NS_TEST_ASSERT_MSG_EQ(sparseMax,
                          0u,
                          "sparse flow that drained within one quantum was promoted to a "
                          "host bulk count");
    NS_TEST_ASSERT_MSG_EQ(sparseEnd, 0u, "sparse flow left a residual host bulk count");

    Simulator::Destroy();
}

/**
 * @brief Unit suite for FqCobalt host-isolation bulk-state timing.
 */
class CakeHostBulkStateTestSuite : public TestSuite
{
  public:
    CakeHostBulkStateTestSuite()
        : TestSuite("stratum-cake-host-bulk-state", Type::UNIT)
    {
        AddTestCase(new CakeHostBulkStateTimingCase, TestCase::Duration::QUICK);
    }
};

static CakeHostBulkStateTestSuite g_cakeHostBulkStateTestSuite;

} // namespace ns3::stratum
