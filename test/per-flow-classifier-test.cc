/*
 * Copyright (C) 2026 Sergio Andreozzi
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Unit tests for diffserv::PerFlowPolicyClassifier.
 */

#include "ns3/ipv4-header.h"
#include "ns3/ipv4-queue-disc-item.h"
#include "ns3/ipv6-address.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/stratum-dscp-tag.h"
#include "ns3/stratum-edge-queue-disc.h"
#include "ns3/stratum-per-flow-policy-classifier.h"
#include "ns3/stratum-pq-scheduler.h"
#include "ns3/test.h"
#include "ns3/uinteger.h"

using namespace ns3;
namespace diffserv = ns3::stratum::diffserv;
using ns3::stratum::EdgeQueueDisc;
using ns3::stratum::FlowKey;
using ns3::stratum::PriorityScheduler;

namespace
{

/// Two flows at the same DSCP maintain independent bucket state.
class PerFlowIsolationTest : public TestCase
{
  public:
    PerFlowIsolationTest()
        : TestCase("PerFlow bucket isolation")
    {
    }

    void DoRun() override
    {
        auto classifier = CreateObject<diffserv::PerFlowPolicyClassifier>();

        FlowKey flowA{Ipv4Address("10.0.0.1"), 2000, Ipv4Address("10.0.0.2"), 80, 6};
        FlowKey flowB{Ipv4Address("10.0.0.3"), 3000, Ipv4Address("10.0.0.2"), 80, 6};

        // CIR=1 Mbps, CBS=1500 B, EBS=1500 B. Green band = 1500 B.
        classifier->AddSrTcmRule(flowA,
                                 10 /*green*/,
                                 12 /*yellow*/,
                                 14 /*red*/,
                                 125000.0 /*cir B/s*/,
                                 1500.0,
                                 1500.0);
        classifier->AddSrTcmRule(flowB, 10, 12, 14, 125000.0, 1500.0, 1500.0);

        // Drain flow A with a 3000-byte burst: expect GREEN (1500) then YELLOW
        // (1500).
        uint8_t dscpA1 = classifier->ApplyPolicy(flowA, 1500, 0.0);
        uint8_t dscpA2 = classifier->ApplyPolicy(flowA, 1500, 0.0);
        NS_TEST_ASSERT_MSG_EQ(dscpA1, 10, "Flow A first 1500 B must be GREEN");
        NS_TEST_ASSERT_MSG_EQ(dscpA2, 12, "Flow A second 1500 B must be YELLOW");

        // Flow B at the same instant must still see full GREEN bucket.
        uint8_t dscpB1 = classifier->ApplyPolicy(flowB, 1500, 0.0);
        NS_TEST_ASSERT_MSG_EQ(dscpB1,
                              10,
                              "Flow B GREEN bucket must be untouched by flow A activity");
    }
};

/// Bucket refill respects CIR.
class PerFlowRefillTest : public TestCase
{
  public:
    PerFlowRefillTest()
        : TestCase("PerFlow bucket refill")
    {
    }

    void DoRun() override
    {
        auto classifier = CreateObject<diffserv::PerFlowPolicyClassifier>();
        FlowKey flow{Ipv4Address("10.0.0.1"), 2000, Ipv4Address("10.0.0.2"), 80, 6};

        // CIR=1000 B/s, CBS=1500, EBS=0. After draining green, wait 1 s -> 1000 B
        // back.
        classifier->AddSrTcmRule(flow, 10, 12, 14, 1000.0, 1500.0, 0.0);

        // Burn green
        classifier->ApplyPolicy(flow, 1500, 0.0);
        // 0.5 s later: 500 B refilled -> 1000 B packet should be RED (no EBS).
        uint8_t c = classifier->ApplyPolicy(flow, 1000, 0.5);
        NS_TEST_ASSERT_MSG_EQ(c, 14, "Partial refill must not satisfy full packet");
        // 1.0 s after burn: another 500 B refilled -> cumulatively 1000 B.
        // That previous 1000 B call did not decrement (it went RED), so bucket is
        // still at 1000 B.
        c = classifier->ApplyPolicy(flow, 1000, 1.0);
        NS_TEST_ASSERT_MSG_EQ(c, 10, "Full refill must satisfy 1000 B packet as GREEN");
    }
};

/// Unknown flow passes through with its existing DSCP preserved.
class PerFlowPassthroughTest : public TestCase
{
  public:
    PerFlowPassthroughTest()
        : TestCase("PerFlow unknown-flow passthrough")
    {
    }

    void DoRun() override
    {
        auto classifier = CreateObject<diffserv::PerFlowPolicyClassifier>();
        FlowKey unknown{Ipv4Address("10.0.0.99"), 9999, Ipv4Address("10.0.0.100"), 80, 6};

        uint8_t c = classifier->ApplyPolicyOrPassthrough(unknown, 1500, 0.0, 46);
        NS_TEST_ASSERT_MSG_EQ(c, 46, "Unknown flow must keep incoming DSCP");
    }
};

/**
 * @brief Build an Ipv4QueueDiscItem with a 4-byte port prefix at the head of
 *        the payload.
 *
 * EdgeQueueDisc::DoEnqueue (and Classify) extract src/dst ports via
 * Packet::CopyData(portBuf, 4) at offset 0. Both TCP and UDP headers begin
 * with (srcPort, dstPort) in network byte order, so prepending just those
 * 4 bytes is sufficient for the classifier's 5-tuple key — we don't need a
 * full TcpHeader (which would also pull in static-init machinery).
 */
Ptr<Ipv4QueueDiscItem>
MakeTcpItem(Ipv4Address src,
            Ipv4Address dst,
            uint16_t srcPort,
            uint16_t dstPort,
            uint32_t payloadSize)
{
    uint8_t portBuf[4];
    portBuf[0] = static_cast<uint8_t>(srcPort >> 8);
    portBuf[1] = static_cast<uint8_t>(srcPort & 0xFF);
    portBuf[2] = static_cast<uint8_t>(dstPort >> 8);
    portBuf[3] = static_cast<uint8_t>(dstPort & 0xFF);

    Ptr<Packet> portPkt = Create<Packet>(portBuf, 4);
    Ptr<Packet> payload = Create<Packet>(payloadSize);
    portPkt->AddAtEnd(payload);

    Ipv4Header hdr;
    hdr.SetSource(src);
    hdr.SetDestination(dst);
    hdr.SetProtocol(6); // TCP
    hdr.SetPayloadSize(portPkt->GetSize());
    return Create<Ipv4QueueDiscItem>(portPkt, Address(), 0x0800, hdr);
}

/// Edge queue disc dispatches to per-flow classifier when installed.
/// Registered flow is metered (srTCM -> GREEN on first packet); unregistered
/// flow passes through with its initial DSCP.
class EdgeDispatchTest : public TestCase
{
  public:
    EdgeDispatchTest()
        : TestCase("Edge disc dispatches to per-flow classifier")
    {
    }

    void DoRun() override
    {
        // Build an edge queue disc with 1 queue, no mark rules, no DSCP-keyed
        // policy: we want the per-flow path to be the sole policer.
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        auto inner = CreateObject<stratum::RedQueueDisc>();
        edge->SetInnerDisc(inner);
        inner->SetNumQueues(1);

        // Two DSCPs used below: AF11 (10, green) and BE (0, passthrough).
        // Both must have a PHB entry so EnqueueWithCodePoint can find a queue.
        inner->AddPhbEntry(0, 0, 0);
        inner->AddPhbEntry(10, 0, 0);

        Ptr<PriorityScheduler> sched =
            CreateObjectWithAttributes<PriorityScheduler>("NumQueues", UintegerValue(1));
        inner->SetScheduler(sched);

        // Install per-flow classifier with a single registered flow.
        auto pfc = CreateObject<diffserv::PerFlowPolicyClassifier>();
        FlowKey registered{Ipv4Address("10.0.0.1"), 1000, Ipv4Address("10.0.0.2"), 80, 6};
        // CIR=1 Mbps, CBS=1500, EBS=0 → first 1500 B is GREEN (DSCP 10).
        pfc->AddSrTcmRule(registered,
                          10 /*green*/,
                          12 /*yellow*/,
                          14 /*red*/,
                          125000.0,
                          1500.0,
                          0.0);
        edge->SetPerFlowClassifier(pfc);

        edge->Initialize();

        // Packet 1: registered flow. No mark rules → initial DSCP is packet's
        // existing DSCP (0). Per-flow classifier runs srTCM on the registered
        // key → returns greenDscp=10.
        Ptr<Ipv4QueueDiscItem> registeredItem =
            MakeTcpItem(Ipv4Address("10.0.0.1"), Ipv4Address("10.0.0.2"), 1000, 80, 500);
        NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(registeredItem),
                              true,
                              "Registered flow packet must enqueue");

        Ptr<QueueDiscItem> out1 = edge->Dequeue();
        NS_TEST_ASSERT_MSG_NE(out1, nullptr, "Should dequeue registered flow packet");
        Ptr<Ipv4QueueDiscItem> ipOut1 = DynamicCast<Ipv4QueueDiscItem>(out1);
        NS_TEST_ASSERT_MSG_NE(ipOut1, nullptr, "Dequeued item should be IPv4");
        if (!ipOut1)
        {
            Simulator::Destroy();
            return;
        }
        uint8_t dscp1 = ipOut1->GetHeader().GetTos() >> 2;
        NS_TEST_ASSERT_MSG_EQ(dscp1,
                              10,
                              "Registered flow must be marked GREEN by per-flow classifier, got "
                                  << static_cast<uint32_t>(dscp1));

        // Packet 2: unregistered flow. Per-flow classifier returns
        // passthrough → initial DSCP (0) preserved.
        Ptr<Ipv4QueueDiscItem> unknownItem =
            MakeTcpItem(Ipv4Address("10.0.0.9"), Ipv4Address("10.0.0.2"), 9999, 80, 500);
        NS_TEST_ASSERT_MSG_EQ(edge->Enqueue(unknownItem),
                              true,
                              "Unregistered flow packet must enqueue");

        Ptr<QueueDiscItem> out2 = edge->Dequeue();
        NS_TEST_ASSERT_MSG_NE(out2, nullptr, "Should dequeue unregistered flow packet");
        Ptr<Ipv4QueueDiscItem> ipOut2 = DynamicCast<Ipv4QueueDiscItem>(out2);
        NS_TEST_ASSERT_MSG_NE(ipOut2, nullptr, "Dequeued item should be IPv4");
        if (!ipOut2)
        {
            Simulator::Destroy();
            return;
        }
        uint8_t dscp2 = ipOut2->GetHeader().GetTos() >> 2;
        NS_TEST_ASSERT_MSG_EQ(dscp2,
                              0,
                              "Unregistered flow must passthrough with initial DSCP 0, got "
                                  << static_cast<uint32_t>(dscp2));

        Simulator::Destroy();
    }
};

/// A srcPort=0 wildcard rule matches any ephemeral source port.
/// TCP workloads cannot predict client ephemeral ports at rule-install time;
/// installing a single rule with srcPort=0 must meter all inbound packets
/// that share (srcIp, dstIp, dstPort, proto).
class PerFlowWildcardTest : public TestCase
{
  public:
    PerFlowWildcardTest()
        : TestCase("PerFlow srcPort=0 wildcard matches")
    {
    }

    void DoRun() override
    {
        auto classifier = CreateObject<diffserv::PerFlowPolicyClassifier>();

        // Register a wildcard rule: srcPort=0, CIR=1 Mbps, CBS=1500, EBS=0.
        FlowKey wildcardKey{Ipv4Address("10.0.0.1"), 0, Ipv4Address("10.0.0.2"), 80, 6};
        classifier->AddSrTcmRule(wildcardKey,
                                 10 /*green*/,
                                 12 /*yellow*/,
                                 14 /*red*/,
                                 125000.0,
                                 1500.0,
                                 0.0);

        // Packet 1: concrete ephemeral srcPort 49152 -> wildcard fallback,
        // 1500 B fits GREEN bucket.
        FlowKey probeA{Ipv4Address("10.0.0.1"), 49152, Ipv4Address("10.0.0.2"), 80, 6};
        uint8_t c1 = classifier->ApplyPolicyOrPassthrough(probeA, 1500, 0.0, 46);
        NS_TEST_ASSERT_MSG_EQ(c1,
                              10,
                              "First 1500 B via wildcard fallback must be GREEN (got "
                                  << static_cast<uint32_t>(c1) << ")");

        // Packet 2: different ephemeral srcPort but same dst -> same wildcard
        // bucket (which was just drained). Expect RED (no EBS).
        FlowKey probeB{Ipv4Address("10.0.0.1"), 33333, Ipv4Address("10.0.0.2"), 80, 6};
        uint8_t c2 = classifier->ApplyPolicyOrPassthrough(probeB, 1500, 0.0, 46);
        NS_TEST_ASSERT_MSG_EQ(c2,
                              14,
                              "Second probe must share the drained wildcard bucket -> RED (got "
                                  << static_cast<uint32_t>(c2) << ")");

        // Packet 3: unrelated dstIp -> neither exact nor wildcard matches ->
        // passthrough with caller's DSCP.
        FlowKey unrelated{Ipv4Address("10.0.0.1"), 49152, Ipv4Address("10.0.0.99"), 80, 6};
        uint8_t c3 = classifier->ApplyPolicyOrPassthrough(unrelated, 100, 1.0, 46);
        NS_TEST_ASSERT_MSG_EQ(c3, 46, "Unrelated flow must passthrough with incoming DSCP");
    }
};

/**
 * @brief FlowKey IPv6 extension tests.
 *
 * Covers:
 * - V6 keys with identical 5-tuples compare equal.
 * - V6 keys differing in any field compare unequal.
 * - A V4 FlowKey is NOT equal to a V6 FlowKey even when ports/proto match
 *   (family discriminator governs).
 */
class FlowKeyV6Test : public TestCase
{
  public:
    FlowKeyV6Test()
        : TestCase("FlowKey IPv6 family extension")
    {
    }

    void DoRun() override
    {
        // Build two identical V6 keys.
        FlowKey a;
        a.family = FlowKey::V6;
        a.srcIp6 = Ipv6Address("2001:db8::1");
        a.srcPort = 2000;
        a.dstIp6 = Ipv6Address("2001:db8::2");
        a.dstPort = 80;
        a.proto = 6;

        FlowKey b;
        b.family = FlowKey::V6;
        b.srcIp6 = Ipv6Address("2001:db8::1");
        b.srcPort = 2000;
        b.dstIp6 = Ipv6Address("2001:db8::2");
        b.dstPort = 80;
        b.proto = 6;

        NS_TEST_ASSERT_MSG_EQ((a == b), true, "Identical V6 keys must be equal");

        // Hash must be equal for equal V6 keys.
        std::hash<FlowKey> hasher;
        NS_TEST_ASSERT_MSG_EQ(hasher(a), hasher(b), "Identical V6 keys must hash equal");

        // Differ in srcIp6.
        FlowKey c = a;
        c.srcIp6 = Ipv6Address("2001:db8::ff");
        NS_TEST_ASSERT_MSG_EQ((a == c), false, "Different srcIp6 must not be equal");
        NS_TEST_ASSERT_MSG_NE(hasher(a), hasher(c), "distinct srcIp6 should hash differently");

        // Differ in dstIp6.
        FlowKey d = a;
        d.dstIp6 = Ipv6Address("2001:db8::ff");
        NS_TEST_ASSERT_MSG_EQ((a == d), false, "Different dstIp6 must not be equal");

        // Differ in srcPort.
        FlowKey e = a;
        e.srcPort = 3000;
        NS_TEST_ASSERT_MSG_EQ((a == e), false, "Different srcPort must not be equal");

        // Differ in dstPort.
        FlowKey f = a;
        f.dstPort = 443;
        NS_TEST_ASSERT_MSG_EQ((a == f), false, "Different dstPort must not be equal");

        // Differ in proto.
        FlowKey g = a;
        g.proto = 17;
        NS_TEST_ASSERT_MSG_EQ((a == g), false, "Different proto must not be equal");

        // A V4 FlowKey with same ports/proto must NOT equal the V6 key.
        FlowKey v4{Ipv4Address("10.0.0.1"), 2000, Ipv4Address("10.0.0.2"), 80, 6};
        NS_TEST_ASSERT_MSG_EQ((v4 == a), false, "V4 key must not equal V6 key (family differs)");
        NS_TEST_ASSERT_MSG_EQ((a == v4), false, "V6 key must not equal V4 key (family differs)");
    }
};

class PerFlowPolicyClassifierSuite : public TestSuite
{
  public:
    PerFlowPolicyClassifierSuite()
        : TestSuite("stratum-per-flow-classifier", Type::UNIT)
    {
        AddTestCase(new PerFlowIsolationTest, Duration::QUICK);
        AddTestCase(new PerFlowRefillTest, Duration::QUICK);
        AddTestCase(new PerFlowPassthroughTest, Duration::QUICK);
        AddTestCase(new EdgeDispatchTest, Duration::QUICK);
        AddTestCase(new PerFlowWildcardTest, Duration::QUICK);
        AddTestCase(new FlowKeyV6Test, Duration::QUICK);
    }
};

PerFlowPolicyClassifierSuite g_perFlowSuite;

} // namespace
