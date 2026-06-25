/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Characterization tests pinning the non-IP (non-Ipv4QueueDiscItem) fallback
 * behaviour of the four DS-field read predicates and the EdgeQueueDisc write
 * path. These tests PASS against current code — they are a refactor safety
 * net, not TDD red-first tests.
 *
 * All four predicates are private/protected on their respective classes, so
 * each is exercised via the public Enqueue() path. The observable fallback
 * effects are:
 *
 *   Predicate                         | Fallback              | Observed via
 *   ----------------------------------|----------------------|-------------------------------
 *   EdgeQueueDisc::Classify           | returns 0 (DSCP=0)   | DscpTag.GetDscp()==0 on tagged
 *                                     |                      | item after Enqueue; tag absent
 *                                     |                      | on item after Dequeue
 *   RedQueueDisc::GetCodePoint        | returns 0            | Enqueue succeeds; packet lands
 *                                     |                      | in sub-queue 0 (codePt 0 PHB)
 *   RateBasedShaperDispatcher::       | returns 0 → tin 0    | GetInternalQueue(0)->
 *     ClassifyByDscp                  |                      |   GetNPackets()==1
 *   l4s::QueueDisc::IsL4sPacket      | returns false        | item lands in classic AQM,
 *                                     |                      | not the L4S queue
 *
 * Edge write path (DoDequeue):
 *   RemovePacketTag fires unconditionally when a DscpTag is present, even
 *   for non-Ipv4QueueDiscItem. For a tagged non-IP item, the tag is absent
 *   on the dequeued packet.
 */

#include "ns3/address.h"
#include "ns3/ipv4-header.h"
#include "ns3/ipv4-queue-disc-item.h"
#include "ns3/ipv6-header.h"
#include "ns3/ipv6-queue-disc-item.h"
#include "ns3/packet.h"
#include "ns3/queue-disc.h"
#include "ns3/simulator.h"
#include "ns3/stratum-ds-field.h"
#include "ns3/stratum-dscp-tag.h"
#include "ns3/stratum-edge-queue-disc.h"
#include "ns3/stratum-l4s-queue-disc.h"
#include "ns3/stratum-mark-rule.h"
#include "ns3/stratum-policy-classifier.h"
#include "ns3/stratum-policy-entry.h"
#include "ns3/stratum-rate-based-shaper-dispatcher.h"
#include "ns3/stratum-red-queue-disc.h"
#include "ns3/stratum-rr-scheduler.h"
#include "ns3/test.h"

namespace ns3::stratum
{

// ---------------------------------------------------------------------------
// Minimal non-IP QueueDiscItem for tests
//
// A bare subclass of QueueDiscItem that carries a plain Packet but is NOT
// an Ipv4QueueDiscItem. All four predicates DynamicCast to
// Ipv4QueueDiscItem; the cast fails, triggering the non-IP fallback.
// ---------------------------------------------------------------------------

namespace
{

class NonIpItem : public QueueDiscItem
{
  public:
    NonIpItem()
        : QueueDiscItem(Create<Packet>(100), Address(), 0x86DD /* IPv6 ethertype, not IPv4 */)
    {
    }

    void AddHeader() override
    {
    }

    bool Mark() override
    {
        return false;
    }

    bool GetUint8Value(Uint8Values field, uint8_t& value) const override
    {
        // Non-IP: no DS field accessible.
        (void)field;
        (void)value;
        return false;
    }

    void Print(std::ostream& os) const override
    {
        os << "NonIpItem";
    }
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Test 1 — EdgeQueueDisc::Classify non-IP fallback
//
// A non-Ipv4QueueDiscItem has no IPv4 header; Classify() returns 0.
// Observable effects:
//   (a) Enqueue succeeds: the item is accepted into slot 0 with DSCP=0.
//   (b) A DscpTag(0) is stamped on the packet during Enqueue (unconditional
//       in DoEnqueue, regardless of IP version).
//   (c) On Dequeue the DscpTag is removed unconditionally (edge write path).
// ---------------------------------------------------------------------------

class EdgeClassifyNonIpFallbackTest : public TestCase
{
  public:
    EdgeClassifyNonIpFallbackTest()
        : TestCase("DS-field non-IP: EdgeQueueDisc::Classify returns 0 for non-IP item")
    {
    }

  private:
    void DoRun() override
    {
        // Build a minimal EdgeQueueDisc with one inner slot (DSCP 0 → queue 0).
        auto edge = CreateObject<EdgeQueueDisc>();
        auto inner = CreateObject<RedQueueDisc>();
        edge->SetInnerDisc(inner);
        inner->SetNumQueues(1);
        // PHB: DSCP 0 → queue 0, prec 0 (needed so inner's EnqueueWithCodePoint
        // does not drop with NO_PHB_MATCH).
        inner->AddPhbEntry(0, 0, 0);
        auto sched = CreateObjectWithAttributes<RoundRobinScheduler>("NumQueues", UintegerValue(1));
        inner->SetScheduler(sched);

        // Policy: pass-through DSCP 0 (DumbMeter / DumbPolicer).
        PolicyEntry policy;
        policy.codePoint = 0;
        policy.meter = MeterType::DUMB;
        policy.policer = PolicerType::DUMB;
        policy.policyIndex = 0;
        edge->GetPolicyClassifier()->AddPolicyEntry(policy);

        PolicerEntry policer;
        policer.policer = PolicerType::DUMB;
        policer.policyIndex = 0;
        policer.initialCodePt = 0;
        policer.downgrade1 = 0;
        policer.downgrade2 = 0;
        edge->GetPolicyClassifier()->AddPolicerEntry(policer);

        edge->Initialize();

        // (a) Enqueue a non-IP item — should succeed (Classify returns 0,
        // policy passthrough, slot 0 populated).
        auto item = Create<NonIpItem>();
        bool enqueued = edge->Enqueue(item);
        NS_TEST_ASSERT_MSG_EQ(enqueued, true, "non-IP item must be accepted by EdgeQueueDisc");

        // (b) DscpTag(0) is stamped unconditionally by DoEnqueue.
        DscpTag stampedTag;
        bool hasTag = item->GetPacket()->PeekPacketTag(stampedTag);
        NS_TEST_ASSERT_MSG_EQ(hasTag, true, "DscpTag must be present on packet after Enqueue");
        NS_TEST_ASSERT_MSG_EQ(static_cast<uint32_t>(stampedTag.GetDscp()),
                              0u,
                              "DscpTag DSCP must be 0 for non-IP item");

        // (c) On Dequeue the tag is removed unconditionally (edge write path
        // RemovePacketTag fires regardless of whether ipItem cast succeeds).
        Ptr<QueueDiscItem> dequeued = edge->Dequeue();
        NS_TEST_ASSERT_MSG_NE(dequeued, nullptr, "must dequeue the non-IP item");
        if (!dequeued)
        {
            Simulator::Destroy();
            return;
        }
        DscpTag removedTag;
        bool tagStillPresent = dequeued->GetPacket()->PeekPacketTag(removedTag);
        NS_TEST_ASSERT_MSG_EQ(
            tagStillPresent,
            false,
            "DscpTag must be removed from packet after Dequeue (edge write path)");

        Simulator::Destroy();
    }
};

// ---------------------------------------------------------------------------
// Test 2 — EdgeQueueDisc edge write path: tagged non-IP item
//
// When a non-IP item arrives with a pre-existing DscpTag (as if stamped
// by a previous classifier stage), DoDequeue's RemovePacketTag branch still
// fires unconditionally. The tag is absent on the dequeued packet.
// ---------------------------------------------------------------------------

class EdgeWriteTaggedNonIpFallbackTest : public TestCase
{
  public:
    EdgeWriteTaggedNonIpFallbackTest()
        : TestCase(
              "DS-field non-IP: EdgeQueueDisc removes DscpTag unconditionally on dequeue (tagged "
              "non-IP)")
    {
    }

  private:
    void DoRun() override
    {
        auto edge = CreateObject<EdgeQueueDisc>();
        auto inner = CreateObject<RedQueueDisc>();
        edge->SetInnerDisc(inner);
        inner->SetNumQueues(1);
        inner->AddPhbEntry(0, 0, 0);
        auto sched = CreateObjectWithAttributes<RoundRobinScheduler>("NumQueues", UintegerValue(1));
        inner->SetScheduler(sched);

        PolicyEntry policy;
        policy.codePoint = 0;
        policy.meter = MeterType::DUMB;
        policy.policer = PolicerType::DUMB;
        policy.policyIndex = 0;
        edge->GetPolicyClassifier()->AddPolicyEntry(policy);

        PolicerEntry policer;
        policer.policer = PolicerType::DUMB;
        policer.policyIndex = 0;
        policer.initialCodePt = 0;
        policer.downgrade1 = 0;
        policer.downgrade2 = 0;
        edge->GetPolicyClassifier()->AddPolicerEntry(policer);

        edge->Initialize();

        // Stamp a pre-existing DscpTag on the item (simulates a classifier
        // stage upstream — the value does not matter for this test).
        auto item = Create<NonIpItem>();
        DscpTag preTag(7);
        item->GetPacket()->AddPacketTag(preTag);

        bool enqueued = edge->Enqueue(item);
        NS_TEST_ASSERT_MSG_EQ(enqueued, true, "tagged non-IP item must be accepted");

        // DoEnqueue removes the pre-existing tag then stamps a new one (DSCP=0).
        // Verify the tag after Enqueue is DSCP=0 (not 7).
        DscpTag afterEnqueue;
        bool hasTag = item->GetPacket()->PeekPacketTag(afterEnqueue);
        NS_TEST_ASSERT_MSG_EQ(hasTag, true, "DscpTag must be present after enqueue");
        NS_TEST_ASSERT_MSG_EQ(static_cast<uint32_t>(afterEnqueue.GetDscp()),
                              0u,
                              "pre-existing DscpTag replaced with DSCP=0 for non-IP item");

        // After Dequeue the tag must be absent (RemovePacketTag fires
        // unconditionally in DoDequeue when PeekPacketTag returns true).
        Ptr<QueueDiscItem> dequeued = edge->Dequeue();
        NS_TEST_ASSERT_MSG_NE(dequeued, nullptr, "must dequeue the item");
        if (!dequeued)
        {
            Simulator::Destroy();
            return;
        }
        DscpTag residual;
        bool tagRemains = dequeued->GetPacket()->PeekPacketTag(residual);
        NS_TEST_ASSERT_MSG_EQ(tagRemains,
                              false,
                              "DscpTag must be absent after Dequeue for tagged non-IP item");

        Simulator::Destroy();
    }
};

// ---------------------------------------------------------------------------
// Test 3 — RedQueueDisc::GetCodePoint non-IP fallback
//
// GetCodePoint returns 0 for a non-Ipv4QueueDiscItem. Observable effect:
// the item is enqueued to sub-queue 0 (the PHB entry for code point 0)
// rather than being dropped with NO_PHB_MATCH.
// ---------------------------------------------------------------------------

class RedGetCodePointNonIpFallbackTest : public TestCase
{
  public:
    RedGetCodePointNonIpFallbackTest()
        : TestCase("DS-field non-IP: RedQueueDisc::GetCodePoint returns 0 for non-IP item; item "
                   "enqueued to sub-queue 0")
    {
    }

  private:
    void DoRun() override
    {
        auto red = CreateObject<RedQueueDisc>();
        red->SetNumQueues(1);
        // PHB: DSCP 0 → queue 0, prec 0.
        red->AddPhbEntry(0, 0, 0);
        auto sched = CreateObjectWithAttributes<RoundRobinScheduler>("NumQueues", UintegerValue(1));
        red->SetScheduler(sched);
        red->Initialize();

        // A non-IP item with no DscpTag: GetCodePoint is called and returns 0.
        auto item = Create<NonIpItem>();
        bool enqueued = red->Enqueue(item);
        NS_TEST_ASSERT_MSG_EQ(enqueued,
                              true,
                              "non-IP item must be enqueued via GetCodePoint fallback DSCP=0");

        // Confirm one packet in the disc.
        NS_TEST_ASSERT_MSG_EQ(red->GetNPackets(),
                              1u,
                              "RedQueueDisc must hold 1 packet after successful enqueue");

        Simulator::Destroy();
    }
};

// ---------------------------------------------------------------------------
// Test 4 — RateBasedShaperDispatcher::ClassifyByDscp non-IP fallback
//
// ClassifyByDscp returns 0 for a non-Ipv4QueueDiscItem, so the item is
// dispatched to internal queue (tin) 0. Observable via
// GetInternalQueue(0)->GetNPackets()==1 after Enqueue.
// ---------------------------------------------------------------------------

class RateBasedClassifyNonIpFallbackTest : public TestCase
{
  public:
    RateBasedClassifyNonIpFallbackTest()
        : TestCase(
              "DS-field non-IP: RateBasedShaperDispatcher::ClassifyByDscp returns 0; item goes to "
              "tin 0")
    {
    }

  private:
    void DoRun() override
    {
        // CheckConfig auto-creates one DropTailQueue internal queue when
        // m_tinClocks is empty (numTins = max(0,1) = 1). No ConfigureGlobal
        // needed for the enqueue path.
        auto disp = CreateObject<cake::RateBasedShaperDispatcher>();
        disp->Initialize();

        auto item = Create<NonIpItem>();
        bool enqueued = disp->Enqueue(item);
        NS_TEST_ASSERT_MSG_EQ(enqueued,
                              true,
                              "non-IP item must be enqueued to tin 0 via ClassifyByDscp fallback");

        // With a single-tin layout (no per-tin clocks configured), all items
        // enqueue to tin 0. GetNPackets() on the disc == 1 confirms the item
        // landed there (the only tin). ClassifyByDscp non-IP fallback returns
        // 0; that maps to slot 0, the only slot.
        NS_TEST_ASSERT_MSG_EQ(
            disp->GetNPackets(),
            1u,
            "disc must hold 1 packet after ClassifyByDscp non-IP fallback to tin 0");

        Simulator::Destroy();
    }
};

// ---------------------------------------------------------------------------
// Test 5 — l4s::QueueDisc::IsL4sPacket non-IP fallback
//
// IsL4sPacket returns false for a non-Ipv4QueueDiscItem. Observable effect:
// DoEnqueue routes the item to the classic AQM, not the L4S queue.
// After Enqueue: GetClassicAqmDisc()->GetNPackets()==1 and
//                GetL4sQueueDisc()->GetNPackets()==0.
// ---------------------------------------------------------------------------

class L4sIsL4sPacketNonIpFallbackTest : public TestCase
{
  public:
    L4sIsL4sPacketNonIpFallbackTest()
        : TestCase(
              "DS-field non-IP: l4s::QueueDisc::IsL4sPacket returns false; item routed to classic "
              "AQM")
    {
    }

  private:
    void DoRun() override
    {
        // Default construction: m_classicAqm = RedQueueDisc, m_l4sQueue = FifoQueueDisc.
        // CheckConfig wires both as QueueDiscClass children.
        auto l4s = CreateObject<l4s::QueueDisc>();
        // Classic AQM (RedQueueDisc) needs a PHB entry for DSCP 0 so it
        // does not drop with NO_PHB_MATCH when GetCodePoint returns 0.
        // Wire the PHB before Initialize so CheckConfig can set it up.
        // Access via GetClassicAqmDisc() requires the disc to exist;
        // it is created by CheckConfig which fires during Initialize.
        // Use SetClassicAqmDisc to inject a pre-configured RedQueueDisc.
        auto classicRed = CreateObject<RedQueueDisc>();
        classicRed->SetNumQueues(1);
        classicRed->AddPhbEntry(0, 0, 0);
        auto sched = CreateObjectWithAttributes<RoundRobinScheduler>("NumQueues", UintegerValue(1));
        classicRed->SetScheduler(sched);
        l4s->SetClassicAqmDisc(classicRed);

        l4s->Initialize();

        auto item = Create<NonIpItem>();
        bool enqueued = l4s->Enqueue(item);
        NS_TEST_ASSERT_MSG_EQ(enqueued, true, "non-IP item must be accepted by l4s::QueueDisc");

        // IsL4sPacket returned false → item went to classic AQM.
        Ptr<ns3::QueueDisc> classicQd = l4s->GetClassicAqmDisc();
        NS_TEST_ASSERT_MSG_NE(classicQd, nullptr, "classic AQM must be wired");

        NS_TEST_ASSERT_MSG_EQ(classicQd->GetNPackets(),
                              1u,
                              "classic AQM must hold 1 packet (IsL4sPacket non-IP fallback)");

        // L4S queue must be empty.
        Ptr<ns3::QueueDisc> l4sQueue = l4s->GetL4sQueueDisc();
        NS_TEST_ASSERT_MSG_NE(l4sQueue, nullptr, "L4S queue must be wired");
        NS_TEST_ASSERT_MSG_EQ(l4sQueue->GetNPackets(),
                              0u,
                              "L4S queue must be empty; non-IP packet must not enter L4S lane");

        Simulator::Destroy();
    }
};

// ---------------------------------------------------------------------------
// Test 6 — DS-field accessor module (Task 1.2 TDD gate)
//
// Validates the free functions in stratum::GetDsField/GetDscp/GetEcn/
// SetDscpPreservingEcn/GetL3Source/GetL3Destination against:
//   (a) An IPv4 item with ECT(1) set: SetDscpPreservingEcn(item, 46) must
//       change DSCP to 46 and leave the 2 ECN bits intact.
//   (b) Read wrappers GetDscp/GetEcn must read back correctly.
//   (c) GetL3Source/GetL3Destination must extract the IPv4 address.
//   (d) An IPv6 item: SetDscpPreservingEcn must work symmetrically.
//   (e) A non-IP item: SetDscpPreservingEcn/GetDsField/GetDscp/GetEcn/
//       GetL3Source/GetL3Destination must all return false.
// ---------------------------------------------------------------------------

class DsFieldAccessorTest : public TestCase
{
  public:
    DsFieldAccessorTest()
        : TestCase("DS-field accessor: SetDscpPreservingEcn ECN-preserving + read wrappers + "
                   "address extraction + non-IP no-op")
    {
    }

  private:
    void DoRun() override
    {
        // -----------------------------------------------------------------
        // (a/b/c) IPv4 item with ECT(1) — rewrite DSCP=46 (EF), assert ECN preserved
        // -----------------------------------------------------------------
        {
            Ipv4Header h4;
            h4.SetEcn(Ipv4Header::ECN_ECT1);
            h4.SetDscp(Ipv4Header::DSCP_AF11); // initial DSCP = 10
            h4.SetSource(Ipv4Address("192.168.1.1"));
            h4.SetDestination(Ipv4Address("10.0.0.1"));
            auto pkt = Create<Packet>(64);
            auto item4 = Create<Ipv4QueueDiscItem>(pkt, Address(), 0, h4);

            // (a) SetDscpPreservingEcn must succeed and preserve ECN bits
            bool ok = SetDscpPreservingEcn(item4, 46 /* EF */);
            NS_TEST_ASSERT_MSG_EQ(ok, true, "SetDscpPreservingEcn must return true for IPv4 item");
            NS_TEST_ASSERT_MSG_EQ(item4->GetHeader().GetEcn(),
                                  Ipv4Header::ECN_ECT1,
                                  "ECN_ECT1 must be preserved after SetDscpPreservingEcn (IPv4)");
            NS_TEST_ASSERT_MSG_EQ(static_cast<uint8_t>(item4->GetHeader().GetDscp()),
                                  46u,
                                  "DSCP must equal 46 (EF) after SetDscpPreservingEcn (IPv4)");

            // (a) Byte-identity vs legacy arithmetic: TOS == (dscp<<2)|(old_tos&0x3)
            // ECT(1) = 0x01 in the low 2 bits; DSCP 46 = 0x2E; TOS = (46<<2)|1 = 0xB9
            uint8_t expectedTos = static_cast<uint8_t>((46 << 2) | 0x01);
            NS_TEST_ASSERT_MSG_EQ(item4->GetHeader().GetTos(),
                                  expectedTos,
                                  "TOS byte must match (dscp<<2)|ecn — bit-identical to legacy "
                                  "SetTos arithmetic");

            // (b) Read wrappers
            uint8_t dsField = 0;
            NS_TEST_ASSERT_MSG_EQ(GetDsField(item4, dsField),
                                  true,
                                  "GetDsField must return true for IPv4 item");
            NS_TEST_ASSERT_MSG_EQ(dsField, expectedTos, "GetDsField must return the full DS octet");

            uint8_t dscp = 0;
            NS_TEST_ASSERT_MSG_EQ(GetDscp(item4, dscp),
                                  true,
                                  "GetDscp must return true for IPv4 item");
            NS_TEST_ASSERT_MSG_EQ(dscp, 46u, "GetDscp must return 46 (EF)");

            uint8_t ecn = 0;
            NS_TEST_ASSERT_MSG_EQ(GetEcn(item4, ecn),
                                  true,
                                  "GetEcn must return true for IPv4 item");
            NS_TEST_ASSERT_MSG_EQ(ecn, 0x01u, "GetEcn must return ECT(1) == 0x01");

            // (c) Address extraction
            Address src;
            NS_TEST_ASSERT_MSG_EQ(GetL3Source(item4, src),
                                  true,
                                  "GetL3Source must return true for IPv4 item");
            NS_TEST_ASSERT_MSG_EQ(Ipv4Address::ConvertFrom(src),
                                  Ipv4Address("192.168.1.1"),
                                  "GetL3Source must extract source IPv4 address");

            Address dst;
            NS_TEST_ASSERT_MSG_EQ(GetL3Destination(item4, dst),
                                  true,
                                  "GetL3Destination must return true for IPv4 item");
            NS_TEST_ASSERT_MSG_EQ(Ipv4Address::ConvertFrom(dst),
                                  Ipv4Address("10.0.0.1"),
                                  "GetL3Destination must extract destination IPv4 address");
        }

        // -----------------------------------------------------------------
        // (d) IPv6 item — SetDscpPreservingEcn preserves ECN, sets DSCP
        // -----------------------------------------------------------------
        {
            Ipv6Header h6;
            h6.SetEcn(Ipv6Header::ECN_ECT1);
            h6.SetDscp(Ipv6Header::DSCP_AF11); // initial DSCP = 10
            h6.SetSource(Ipv6Address("2001:db8::1"));
            h6.SetDestination(Ipv6Address("2001:db8::2"));
            auto pkt6 = Create<Packet>(64);
            auto item6 = Create<Ipv6QueueDiscItem>(pkt6, Address(), 0, h6);

            bool ok6 = SetDscpPreservingEcn(item6, 46 /* EF */);
            NS_TEST_ASSERT_MSG_EQ(ok6, true, "SetDscpPreservingEcn must return true for IPv6 item");
            NS_TEST_ASSERT_MSG_EQ(item6->GetHeader().GetEcn(),
                                  Ipv6Header::ECN_ECT1,
                                  "ECN_ECT1 must be preserved after SetDscpPreservingEcn (IPv6)");
            NS_TEST_ASSERT_MSG_EQ(static_cast<uint8_t>(item6->GetHeader().GetDscp()),
                                  46u,
                                  "DSCP must equal 46 (EF) after SetDscpPreservingEcn (IPv6)");

            // Read wrappers work for IPv6 too
            uint8_t dscp6 = 0;
            NS_TEST_ASSERT_MSG_EQ(GetDscp(item6, dscp6),
                                  true,
                                  "GetDscp must return true for IPv6 item");
            NS_TEST_ASSERT_MSG_EQ(dscp6, 46u, "GetDscp must return 46 for IPv6 item");

            uint8_t ecn6 = 0;
            NS_TEST_ASSERT_MSG_EQ(GetEcn(item6, ecn6),
                                  true,
                                  "GetEcn must return true for IPv6 item");
            NS_TEST_ASSERT_MSG_EQ(ecn6, 0x01u, "GetEcn must return ECT(1)==0x01 for IPv6 item");

            // Address extraction
            Address src6;
            NS_TEST_ASSERT_MSG_EQ(GetL3Source(item6, src6),
                                  true,
                                  "GetL3Source must return true for IPv6 item");
            NS_TEST_ASSERT_MSG_EQ(Ipv6Address::ConvertFrom(src6),
                                  Ipv6Address("2001:db8::1"),
                                  "GetL3Source must extract source IPv6 address");

            Address dst6;
            NS_TEST_ASSERT_MSG_EQ(GetL3Destination(item6, dst6),
                                  true,
                                  "GetL3Destination must return true for IPv6 item");
            NS_TEST_ASSERT_MSG_EQ(Ipv6Address::ConvertFrom(dst6),
                                  Ipv6Address("2001:db8::2"),
                                  "GetL3Destination must extract destination IPv6 address");
        }

        // -----------------------------------------------------------------
        // (e) Non-IP item — all accessors must return false (no-op)
        // -----------------------------------------------------------------
        {
            auto nonIp = Create<NonIpItem>();

            NS_TEST_ASSERT_MSG_EQ(SetDscpPreservingEcn(nonIp, 46),
                                  false,
                                  "SetDscpPreservingEcn must return false for non-IP item");

            uint8_t dsField = 0;
            NS_TEST_ASSERT_MSG_EQ(GetDsField(nonIp, dsField),
                                  false,
                                  "GetDsField must return false for non-IP item");

            uint8_t dscp = 0;
            NS_TEST_ASSERT_MSG_EQ(GetDscp(nonIp, dscp),
                                  false,
                                  "GetDscp must return false for non-IP item");

            uint8_t ecn = 0;
            NS_TEST_ASSERT_MSG_EQ(GetEcn(nonIp, ecn),
                                  false,
                                  "GetEcn must return false for non-IP item");

            Address src;
            NS_TEST_ASSERT_MSG_EQ(GetL3Source(nonIp, src),
                                  false,
                                  "GetL3Source must return false for non-IP item");

            Address dst;
            NS_TEST_ASSERT_MSG_EQ(GetL3Destination(nonIp, dst),
                                  false,
                                  "GetL3Destination must return false for non-IP item");
        }
    }
};

// ---------------------------------------------------------------------------
// Test 7 — AddrMatch: exact V4 hit/miss and Any wildcard (Task 1.5 TDD gate)
//
// Exercises AddrMatch::Matches(const Address&) for:
//   V4 exact hit:  Matches(Ipv4Address("10.0.0.1").ConvertTo()) == true
//   V4 exact miss: Matches(Ipv4Address("10.0.0.2").ConvertTo()) == false
//   Any wildcard:  default-constructed AddrMatch.Matches(<any address>) == true
// ---------------------------------------------------------------------------

class AddrMatchTest : public TestCase
{
  public:
    AddrMatchTest()
        : TestCase("AddrMatch: exact V4 hit or miss + Any wildcard via Address")
    {
    }

  private:
    void DoRun() override
    {
        // V4 exact match
        AddrMatch m;
        m.family = AddrMatch::V4;
        m.v4 = Ipv4Address("10.0.0.1");
        m.prefixLen = 255;

        Address hit = Ipv4Address("10.0.0.1").ConvertTo();
        Address miss = Ipv4Address("10.0.0.2").ConvertTo();

        NS_TEST_ASSERT_MSG_EQ(m.Matches(hit), true, "V4 exact hit must return true");
        NS_TEST_ASSERT_MSG_EQ(m.Matches(miss), false, "V4 exact miss must return false");

        // Any wildcard (default-constructed AddrMatch)
        AddrMatch any;
        NS_TEST_ASSERT_MSG_EQ(any.Matches(hit), true, "Any wildcard must match any address");
        NS_TEST_ASSERT_MSG_EQ(any.Matches(miss), true, "Any wildcard must match any address (2)");

        // (a) Typed constructor assertions
        {
            // Default-constructed AddrMatch is Any wildcard
            AddrMatch defAny;
            NS_TEST_ASSERT_MSG_EQ(defAny.family,
                                  AddrMatch::Any,
                                  "default ctor must yield family=Any");

            // Ipv4Address implicit ctor
            AddrMatch fromV4(Ipv4Address("1.2.3.4"));
            NS_TEST_ASSERT_MSG_EQ(fromV4.family,
                                  AddrMatch::V4,
                                  "Ipv4Address ctor must yield family=V4");
            NS_TEST_ASSERT_MSG_EQ(fromV4.v4,
                                  Ipv4Address("1.2.3.4"),
                                  "Ipv4Address ctor: v4 field must hold the address");

            // Ipv6Address implicit ctor
            AddrMatch fromV6(Ipv6Address("2001:db8::1"));
            NS_TEST_ASSERT_MSG_EQ(fromV6.family,
                                  AddrMatch::V6,
                                  "Ipv6Address ctor must yield family=V6");
            NS_TEST_ASSERT_MSG_EQ(fromV6.v6,
                                  Ipv6Address("2001:db8::1"),
                                  "Ipv6Address ctor: v6 field must hold the address");
        }

        // (b) Family-mismatch guard: V4 AddrMatch must not match an IPv6 Address
        {
            AddrMatch v4m;
            v4m.family = AddrMatch::V4;
            v4m.v4 = Ipv4Address("10.0.0.1");
            v4m.prefixLen = 255;
            Address v6addr = Ipv6Address("2001:db8::1").ConvertTo();
            NS_TEST_ASSERT_MSG_EQ(v4m.Matches(v6addr),
                                  false,
                                  "V4 AddrMatch must not match an IPv6 address");
        }

        // (c) V6 exact hit / miss via Matches(const Address&)
        {
            AddrMatch v6m(Ipv6Address("2001:db8::1")); // implicit ctor -> family=V6
            Address v6hit = Ipv6Address("2001:db8::1").ConvertTo();
            Address v6miss = Ipv6Address("2001:db8::2").ConvertTo();
            NS_TEST_ASSERT_MSG_EQ(v6m.Matches(v6hit), true, "V6 exact hit must return true");
            NS_TEST_ASSERT_MSG_EQ(v6m.Matches(v6miss), false, "V6 exact miss must return false");

            // Symmetric guard: a V6 rule must not match an IPv4 address
            Address v4addr = Ipv4Address("10.0.0.1").ConvertTo();
            NS_TEST_ASSERT_MSG_EQ(v6m.Matches(v4addr),
                                  false,
                                  "V6 AddrMatch must not match an IPv4 address");
        }
    }
};

// ---------------------------------------------------------------------------
// Test suite registration
// ---------------------------------------------------------------------------

class DsFieldFallbackTestSuite : public TestSuite
{
  public:
    DsFieldFallbackTestSuite()
        : TestSuite("stratum-ds-field-fallback", Type::UNIT)
    {
        // EdgeQueueDisc::Classify non-IP fallback
        AddTestCase(new EdgeClassifyNonIpFallbackTest(), TestCase::Duration::QUICK);
        // EdgeQueueDisc write path: tagged non-IP item
        AddTestCase(new EdgeWriteTaggedNonIpFallbackTest(), TestCase::Duration::QUICK);
        // RedQueueDisc::GetCodePoint non-IP fallback
        AddTestCase(new RedGetCodePointNonIpFallbackTest(), TestCase::Duration::QUICK);
        // RateBasedShaperDispatcher::ClassifyByDscp non-IP fallback
        AddTestCase(new RateBasedClassifyNonIpFallbackTest(), TestCase::Duration::QUICK);
        // l4s::QueueDisc::IsL4sPacket non-IP fallback
        AddTestCase(new L4sIsL4sPacketNonIpFallbackTest(), TestCase::Duration::QUICK);
        // DS-field accessor module (Task 1.2 TDD gate)
        AddTestCase(new DsFieldAccessorTest(), TestCase::Duration::QUICK);
        // AddrMatch struct (Task 1.5 TDD gate)
        AddTestCase(new AddrMatchTest(), TestCase::Duration::QUICK);
    }
};

static DsFieldFallbackTestSuite g_dsFieldFallbackTestSuite;

} // namespace ns3::stratum
