/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * Copyright (C) 2000 Nortel Networks
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsEdge.{h,cc} class edgeQueue (2001).
 */

#include "stratum-edge-queue-disc.h"

#include "stratum-app-type-tag.h"
#include "stratum-dscp-tag.h"
#include "stratum-dumb-meter.h"
#include "stratum-fw-meter.h"
#include "stratum-l4s-queue-disc.h"
#include "stratum-queue-stats-provider.h"
#include "stratum-red-sub-queue.h"
#include "stratum-sr-tcm-meter.h"
#include "stratum-token-bucket-meter.h"
#include "stratum-tr-tcm-meter.h"
#include "stratum-tsw2cm-meter.h"
#include "stratum-tsw3cm-meter.h"

#include "ns3/boolean.h"
#include "ns3/ipv4-queue-disc-item.h"
#include "ns3/log.h"
#include "ns3/ptr.h"
#include "ns3/queue-disc.h"
#include "ns3/simulator.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::EdgeQueueDisc");

} // namespace ns3

namespace ns3::stratum
{

NS_OBJECT_ENSURE_REGISTERED(EdgeQueueDisc);

TypeId
EdgeQueueDisc::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::EdgeQueueDisc")
                            .SetParent<QueueDisc>()
                            .SetGroupName("Stratum")
                            .AddConstructor<EdgeQueueDisc>()
                            .AddAttribute("Wash",
                                          "Egress DSCP wash (Linux tc-cake `wash` mode). "
                                          "When true, DoDequeue zeros the DSCP bits of the "
                                          "IPv4 TOS byte on every dequeued item while "
                                          "preserving the low two ECN bits. Classification "
                                          "still drives inner-slot routing; only the egress "
                                          "packet's DSCP byte is cleared.",
                                          BooleanValue(false),
                                          MakeBooleanAccessor(&EdgeQueueDisc::m_wash),
                                          MakeBooleanChecker());
    return tid;
}

EdgeQueueDisc::EdgeQueueDisc()
    : QueueDisc(QueueDiscSizePolicy::NO_LIMITS),
      m_numMarkRules(0),
      m_policyClassifier(CreateObject<diffserv::PolicyClassifier>())
{
    NS_LOG_FUNCTION(this);
    // Wire the classifier's meter-strategy lookup hook to this edge disc.
    // Safe in the constructor: the classifier stores a raw pointer for
    // later dispatch. Virtual GetMeter() dispatch only fires at
    // DoEnqueue time, by which point this object is fully constructed.
    m_policyClassifier->SetMeterProvider(this);
    // Default DSCP-to-slot routing: every code point → slot 0.
    // Single-inner installations reach the inner exclusively through
    // this slot. Scenarios wanting heterogeneous routing override via
    // SetDscpToSlot after SetInnerDiscAt.
    m_dscpToSlot.fill(0);
    // Default across-slot dispatch policy: strict priority by slot
    // index. Scenarios that want non-strict policy (CAKE
    // DRR-across-tins, future WFQ/HTB) call SetSlotDispatcher with a
    // subclass before Initialize.
    m_slotDispatcher = CreateObject<StrictPriorityDispatcher>();
}

EdgeQueueDisc::~EdgeQueueDisc()
{
    NS_LOG_FUNCTION(this);
}

// --- Meter strategy slots ---

void
EdgeQueueDisc::SetMeter(MeterType type, Ptr<Meter> meter)
{
    NS_LOG_FUNCTION(this << static_cast<int>(static_cast<uint8_t>(type)) << meter);
    auto idx = static_cast<std::size_t>(static_cast<uint8_t>(type));
    NS_ASSERT_MSG(idx < kMeterPoolSize, "MeterType index " << idx << " out of range");
    m_meters[idx] = meter;
}

Ptr<Meter>
EdgeQueueDisc::GetMeter(MeterType type)
{
    auto idx = static_cast<std::size_t>(static_cast<uint8_t>(type));
    if (idx >= kMeterPoolSize)
    {
        return nullptr;
    }

    // Lazy-create the default implementation on first request, with
    // the same default-meter selection that
    // diffserv::PolicyClassifier::GetOrCreateMeter applies.
    if (!m_meters[idx])
    {
        switch (type)
        {
        case MeterType::DUMB:
            m_meters[idx] = CreateObject<DumbMeter>();
            break;
        case MeterType::TOKEN_BUCKET:
            m_meters[idx] = CreateObject<TokenBucketMeter>();
            break;
        case MeterType::SRTCM:
            m_meters[idx] = CreateObject<SrTcmMeter>();
            break;
        case MeterType::TRTCM:
            m_meters[idx] = CreateObject<TrTcmMeter>();
            break;
        case MeterType::TSW2CM:
            m_meters[idx] = CreateObject<Tsw2cmMeter>();
            break;
        case MeterType::TSW3CM:
            m_meters[idx] = CreateObject<Tsw3cmMeter>();
            break;
        case MeterType::FAIR_WEIGHTED:
            m_meters[idx] = CreateObject<FWMeter>();
            break;
        default:
            NS_LOG_ERROR("Unknown MeterType " << idx);
            return nullptr;
        }
    }
    return m_meters[idx];
}

// --- Edge-specific: classification and metering ---

void
EdgeQueueDisc::AddMarkRule(const MarkRule& rule)
{
    NS_LOG_FUNCTION(this << static_cast<uint32_t>(rule.dscp));

    if (m_numMarkRules >= kMaxMarkRules)
    {
        NS_LOG_ERROR("Mark rule table full (max " << kMaxMarkRules << ")");
        return;
    }
    m_markRules[m_numMarkRules++] = rule;
}

Ptr<diffserv::PolicyClassifier>
EdgeQueueDisc::GetPolicyClassifier() const
{
    return m_policyClassifier;
}

void
EdgeQueueDisc::SetPerFlowClassifier(Ptr<diffserv::PerFlowPolicyClassifier> classifier)
{
    NS_LOG_FUNCTION(this << classifier);
    m_perFlowClassifier = classifier;
}

Ptr<diffserv::PerFlowPolicyClassifier>
EdgeQueueDisc::GetPerFlowClassifier() const
{
    return m_perFlowClassifier;
}

// --- Inner disc accessors (multi-slot) ---

void
EdgeQueueDisc::SetInnerDisc(Ptr<QueueDisc> inner)
{
    NS_LOG_FUNCTION(this << inner);
    // Single-inner overload: anchor at slot 0. The default DSCP-to-
    // slot map routes every code point to slot 0, so single-inner
    // scenarios behave identically to a non-multi-slot installation.
    SetInnerDiscAt(0, inner);
}

Ptr<QueueDisc>
EdgeQueueDisc::GetInnerDisc() const
{
    return GetInnerDiscAt(0);
}

cake::TinStats
EdgeQueueDisc::GetTinStats(uint32_t slot) const
{
    if (!m_slotDispatcher)
    {
        return cake::TinStats{};
    }
    return m_slotDispatcher->GetTinStats(slot, this);
}

void
EdgeQueueDisc::SetInnerDiscAt(uint32_t slot, Ptr<QueueDisc> inner)
{
    NS_LOG_FUNCTION(this << slot << inner);
    NS_ASSERT_MSG(slot < kMaxInnerSlots,
                  "Inner slot " << slot << " out of range (max " << kMaxInnerSlots << ")");
    NS_ASSERT_MSG(inner,
                  "SetInnerDiscAt: inner must not be null "
                  "(clear by reconstructing the edge)");
    NS_ASSERT_MSG(GetNQueueDiscClasses() == 0, "SetInnerDiscAt must be called before Initialize");
    // Slots fill monotonically — enforces the "no gaps" invariant so
    // GetNumInnerSlots can scan-until-empty. Slot 0 is always the
    // first populated; successive slots k require slot k-1 filled.
    for (uint32_t k = 0; k < slot; ++k)
    {
        NS_ASSERT_MSG(m_inners[k],
                      "SetInnerDiscAt(" << slot << ") requires slot " << k
                                        << " to already be populated");
    }
    m_inners[slot] = inner;
}

Ptr<QueueDisc>
EdgeQueueDisc::GetInnerDiscAt(uint32_t slot) const
{
    if (slot >= kMaxInnerSlots)
    {
        return nullptr;
    }
    return m_inners[slot];
}

void
EdgeQueueDisc::SetDscpToSlot(uint8_t dscp, uint32_t slot)
{
    NS_LOG_FUNCTION(this << static_cast<uint32_t>(dscp) << slot);
    NS_ASSERT_MSG(static_cast<int>(dscp) < kMaxCodePoints,
                  "DSCP " << static_cast<uint32_t>(dscp) << " out of range (max " << kMaxCodePoints
                          << ")");
    NS_ASSERT_MSG(slot < kMaxInnerSlots,
                  "Inner slot " << slot << " out of range (max " << kMaxInnerSlots << ")");
    NS_ASSERT_MSG(slot == 0 || m_inners[slot],
                  "SetDscpToSlot(" << static_cast<uint32_t>(dscp) << ", " << slot
                                   << ") requires slot populated via "
                                      "SetInnerDiscAt first");
    m_dscpToSlot[dscp] = slot;
}

uint32_t
EdgeQueueDisc::GetDscpToSlot(uint8_t dscp) const
{
    NS_ASSERT_MSG(static_cast<int>(dscp) < kMaxCodePoints,
                  "DSCP " << static_cast<uint32_t>(dscp) << " out of range (max " << kMaxCodePoints
                          << ")");
    return m_dscpToSlot[dscp];
}

uint32_t
EdgeQueueDisc::GetNumInnerSlots() const
{
    uint32_t n = 0;
    while (n < kMaxInnerSlots && m_inners[n])
    {
        ++n;
    }
    return n;
}

// --- Across-slot dispatch strategy ---

void
EdgeQueueDisc::SetSlotDispatcher(Ptr<SlotDispatcher> dispatcher)
{
    NS_LOG_FUNCTION(this << dispatcher);
    NS_ASSERT_MSG(dispatcher, "SetSlotDispatcher: dispatcher must not be null");
    NS_ASSERT_MSG(GetNQueueDiscClasses() == 0,
                  "SetSlotDispatcher must be called before Initialize");
    m_slotDispatcher = dispatcher;
}

Ptr<SlotDispatcher>
EdgeQueueDisc::GetSlotDispatcher() const
{
    return m_slotDispatcher;
}

void
EdgeQueueDisc::EnsureDefaultInner()
{
    if (!m_inners[0])
    {
        m_inners[0] = CreateObject<RedQueueDisc>();
    }
    // Every populated slot must be exposed as a QueueDiscClass child so
    // that ns-3's base-class iteration (DoInitialize → per-class
    // CheckConfig + InitializeParams) fires on each inner, and so that
    // AddQueueDiscClass auto-forwards each child's DropBeforeEnqueue
    // trace into the outer's drop statistics.
    while (GetNQueueDiscClasses() < GetNumInnerSlots())
    {
        uint32_t idx = GetNQueueDiscClasses();
        Ptr<QueueDiscClass> cls = CreateObject<QueueDiscClass>();
        cls->SetQueueDisc(m_inners[idx]);
        AddQueueDiscClass(cls);
    }
}

// --- Inner configuration ---
//
// Callers configure the inner via its own API before SetInnerDisc,
// or via `helper.InstallRedInner(edge)` which creates + wires a
// RedQueueDisc and returns a typed handle.

// --- Runtime probes via the QueueStatsProvider interface ---
//
// Queue-state probes go through the inner-agnostic QueueStatsProvider
// interface: any inner that implements it (Red and L4S both do) answers
// queue-state queries polymorphically. GetScheduler and PrintPhbTable
// remain Red-only because the scheduler abstraction and PHB table are
// Red-specific concepts; L4S owns its own DualPI2 controller and does
// not expose a `Scheduler`. The zero-argument generic probes
// (GetNumQueues, GetVirtualQueueLen, PrintStats) are convenience
// overloads that delegate to the per-slot form at slot 0.

Ptr<Scheduler>
EdgeQueueDisc::GetScheduler() const
{
    // Red-only: scheduler is RED-specific. L4S's DualPI2 controller is
    // not a Scheduler. Multi-slot scenarios probe slot 0 (the default
    // inner) — scenarios that need per-slot scheduler introspection go
    // through GetInnerDiscAt(i) + DynamicCast<RedQueueDisc>.
    Ptr<QueueDisc> inner = m_inners[0];
    if (!inner)
    {
        return nullptr;
    }
    Ptr<RedQueueDisc> red = DynamicCast<RedQueueDisc>(inner);
    return red ? red->GetScheduler() : nullptr;
}

uint32_t
EdgeQueueDisc::GetNumQueues() const
{
    return GetNumQueues(0);
}

int
EdgeQueueDisc::GetVirtualQueueLen(uint32_t queue, uint32_t prec) const
{
    return GetVirtualQueueLen(0, queue, prec);
}

void
EdgeQueueDisc::PrintStats() const
{
    PrintStats(0);
}

void
EdgeQueueDisc::PrintPhbTable() const
{
    // PHB table is Red-specific (L4S has no PHB concept in the same sense).
    // Slot-0 semantics match the rest of the probes; Red-in-non-zero-slot
    // scenarios should unwrap via GetInnerDiscAt(i).
    Ptr<RedQueueDisc> red = DynamicCast<RedQueueDisc>(m_inners[0]);
    if (red)
    {
        red->PrintPhbTable();
    }
}

// --- Per-slot overloads ---
//
// The zero-argument probes above default to slot 0 for backward
// compatibility. The overloads below let multi-inner scenarios probe
// any populated slot through the same polymorphic `QueueStatsProvider`
// path. Out-of-range or empty slots return conservative zeros.

uint32_t
EdgeQueueDisc::GetNumQueues(uint32_t slot) const
{
    if (slot >= kMaxInnerSlots)
    {
        return 0;
    }
    auto* stats = dynamic_cast<QueueStatsProvider*>(PeekPointer(m_inners[slot]));
    return stats ? stats->GetNumQueues() : 0;
}

int
EdgeQueueDisc::GetVirtualQueueLen(uint32_t slot, uint32_t queue, uint32_t prec) const
{
    if (slot >= kMaxInnerSlots)
    {
        return 0;
    }
    auto* stats = dynamic_cast<QueueStatsProvider*>(PeekPointer(m_inners[slot]));
    return stats ? stats->GetQueueLen(queue, prec) : 0;
}

void
EdgeQueueDisc::PrintStats(uint32_t slot) const
{
    if (slot >= kMaxInnerSlots)
    {
        return;
    }
    auto* stats = dynamic_cast<QueueStatsProvider*>(PeekPointer(m_inners[slot]));
    if (stats)
    {
        stats->PrintStats();
    }
}

int64_t
EdgeQueueDisc::AssignStreams(int64_t stream)
{
    NS_LOG_FUNCTION(this << stream);
    // The edge composer has no own RNG. Cascade into (a) every
    // populated inner slot's sub-cascade and (b) any installed meter
    // slot whose algorithm owns an RNG. Only slots actually holding
    // Tsw2cm / Tsw3cm / Fw meter instances consume a stream, so
    // scenarios that never install a probabilistic meter see no
    // behaviour change. The total stream budget is the sum of the
    // per-slot sub-cascades.
    int64_t consumed = 0;
    for (uint32_t s = 0; s < kMaxInnerSlots; ++s)
    {
        Ptr<QueueDisc> inner = m_inners[s];
        if (!inner)
        {
            break; // slots fill monotonically; stop at first empty
        }
        // Branch on inner type. RedQueueDisc exposes RedSubQueue
        // leaves directly; l4s::QueueDisc has its own AssignStreams
        // that cascades through its L4S + classic children. Other
        // QueueDisc types: no RNG to seat (default).
        Ptr<RedQueueDisc> red = DynamicCast<RedQueueDisc>(inner);
        Ptr<l4s::QueueDisc> l4s = DynamicCast<l4s::QueueDisc>(inner);
        if (red)
        {
            for (uint32_t i = 0; i < red->GetNQueueDiscClasses(); ++i)
            {
                Ptr<RedSubQueue> sub =
                    DynamicCast<RedSubQueue>(red->GetQueueDiscClass(i)->GetQueueDisc());
                if (sub)
                {
                    sub->AssignStreams(stream + consumed);
                    ++consumed;
                }
            }
        }
        else if (l4s)
        {
            consumed += l4s->AssignStreams(stream + consumed);
        }
    }
    // (b) Meter slots. Only the probabilistic-marking meters own RNGs:
    // Tsw2cm, Tsw3cm (RFC 2859) and FWMeter (mode=1 probabilistic). Token
    // bucket, srTCM, trTCM, and Dumb are deterministic and consume nothing.
    // Skip empty slots so we don't spuriously create a default meter
    // we wouldn't otherwise need.
    for (std::size_t idx = 0; idx < kMeterPoolSize; ++idx)
    {
        if (!m_meters[idx])
        {
            continue;
        }
        if (Ptr<Tsw2cmMeter> m = DynamicCast<Tsw2cmMeter>(m_meters[idx]))
        {
            m->AssignStreams(stream + consumed);
            ++consumed;
        }
        else if (Ptr<Tsw3cmMeter> m = DynamicCast<Tsw3cmMeter>(m_meters[idx]))
        {
            m->AssignStreams(stream + consumed);
            ++consumed;
        }
        else if (Ptr<FWMeter> m = DynamicCast<FWMeter>(m_meters[idx]))
        {
            m->AssignStreams(stream + consumed);
            ++consumed;
        }
    }
    return consumed;
}

// --- Classification helper ---

uint8_t
EdgeQueueDisc::Classify(Ptr<const QueueDiscItem> item) const
{
    NS_LOG_FUNCTION(this << item);

    Ptr<const Ipv4QueueDiscItem> ipItem = DynamicCast<const Ipv4QueueDiscItem>(item);
    if (!ipItem)
    {
        NS_LOG_WARN("Non-IPv4 packet; returning code point 0");
        return 0;
    }

    const Ipv4Header& hdr = ipItem->GetHeader();
    int32_t srcAddr = static_cast<int32_t>(hdr.GetSource().Get());
    int32_t dstAddr = static_cast<int32_t>(hdr.GetDestination().Get());
    uint8_t protocol = hdr.GetProtocol();

    AppTypeTag appTag;
    uint32_t appType = kAnyAppType;
    if (item->GetPacket()->PeekPacketTag(appTag))
    {
        appType = appTag.GetAppType();
    }

    // Extract transport-layer ports if protocol is TCP or UDP.
    // Both TCP and UDP headers start with srcPort (2 bytes, network order)
    // then dstPort (2 bytes, network order).
    uint16_t srcPort = kAnyPort;
    uint16_t dstPort = kAnyPort;
    if (protocol == 6 || protocol == 17) // TCP or UDP
    {
        uint8_t portBuf[4];
        if (item->GetPacket()->CopyData(portBuf, 4) == 4)
        {
            srcPort = (static_cast<uint16_t>(portBuf[0]) << 8) | portBuf[1];
            dstPort = (static_cast<uint16_t>(portBuf[2]) << 8) | portBuf[3];
        }
    }

    for (int i = 0; i < m_numMarkRules; ++i)
    {
        const MarkRule& rule = m_markRules[i];

        bool srcMatch = (rule.srcAddr == kAnyHost) || (rule.srcAddr == srcAddr);
        bool dstMatch = (rule.dstAddr == kAnyHost) || (rule.dstAddr == dstAddr);
        bool protoMatch = (rule.protocol == kAnyProtocol) || (rule.protocol == protocol);
        bool appMatch =
            (rule.appType == kAnyAppType) || (appType == kAnyAppType) || (rule.appType == appType);
        bool srcPortMatch = (rule.srcPort == kAnyPort) || (rule.srcPort == srcPort);
        bool dstPortMatch = (rule.dstPort == kAnyPort) || (rule.dstPort == dstPort);

        if (srcMatch && dstMatch && protoMatch && appMatch && srcPortMatch && dstPortMatch)
        {
            NS_LOG_DEBUG("Mark rule " << i
                                      << " matched; DSCP=" << static_cast<uint32_t>(rule.dscp));
            return rule.dscp;
        }
    }

    // No match: preserve original DSCP ( passthrough). Read from
    // the IPv4 header directly — the inner has not been touched so no
    // DscpTag is in play yet.
    return static_cast<uint8_t>(hdr.GetDscp());
}

// --- QueueDisc overrides ---

bool
EdgeQueueDisc::DoEnqueue(Ptr<QueueDiscItem> item)
{
    NS_LOG_FUNCTION(this << item);

    uint8_t initialDscp = Classify(item);

    // Meter on the full on-wire packet size (IP + transport + payload) to
    // match the ns-2.35 reference after the UDP-header fix. ns-2.29 metered
    // on application-payload-only (hdr_cmn::size() before the fix); ns-2.35
    // adds 20 B IP + 8 B UDP so the policer sees the real wire cost, and
    // ns-3 should do the same. item->GetSize() — not GetPacket()->GetSize()
    // — because the former includes the IPv4 header while the latter does
    // not.
    double now = Simulator::Now().GetSeconds();
    uint32_t meterPktSize = item->GetSize();
    uint8_t finalDscp;

    Ptr<const Ipv4QueueDiscItem> ipItem = DynamicCast<const Ipv4QueueDiscItem>(item);
    if (m_perFlowClassifier && ipItem)
    {
        const Ipv4Header& hdr = ipItem->GetHeader();
        uint8_t protocol = hdr.GetProtocol();
        uint16_t srcPort = 0;
        uint16_t dstPort = 0;
        if (protocol == 6 || protocol == 17)
        {
            uint8_t portBuf[4];
            if (item->GetPacket()->CopyData(portBuf, 4) == 4)
            {
                srcPort = (static_cast<uint16_t>(portBuf[0]) << 8) | portBuf[1];
                dstPort = (static_cast<uint16_t>(portBuf[2]) << 8) | portBuf[3];
            }
        }
        FlowKey key{hdr.GetSource(), srcPort, hdr.GetDestination(), dstPort, protocol};
        finalDscp =
            m_perFlowClassifier->ApplyPolicyOrPassthrough(key, meterPktSize, now, initialDscp);
    }
    else
    {
        finalDscp = m_policyClassifier->ApplyPolicy(initialDscp, meterPktSize, now);
    }

    // Stamp the final DSCP for the inner's PHB lookup and for dequeue-time
    // header rewrite. AddPacketTag asserts uniqueness, so any pre-existing
    // tag must be removed first.
    DscpTag oldTag;
    item->GetPacket()->RemovePacketTag(oldTag);
    DscpTag tag(finalDscp);
    item->GetPacket()->AddPacketTag(tag);

    // DSCP-keyed inner-slot dispatch. The default map sends every code
    // point to slot 0, so single-inner scenarios route exclusively to
    // slot 0. Multi-inner scenarios (e.g. EF/46 → L4S at slot 0,
    // AF/BE → Red at slot 1) steer via SetDscpToSlot().
    //
    // Drop aggregation: inner drops (tail, RED early, NO_PHB_MATCH,
    // dropper drops) bubble up via AddQueueDiscClass's
    // ChildQueueDiscDropFunctor — attached per populated slot by
    // EnsureDefaultInner.
    uint32_t slot = m_dscpToSlot[finalDscp];
    NS_ASSERT_MSG(slot < kMaxInnerSlots && m_inners[slot],
                  "DSCP " << static_cast<uint32_t>(finalDscp) << " routes to slot " << slot
                          << " which is not populated");
    bool ok = m_inners[slot]->Enqueue(item);
    // Notify the across-slot dispatcher only on success so stateful
    // subclasses tracking per-slot occupancy do not mis-count inner
    // drops. StrictPriorityDispatcher's OnEnqueue is a no-op and
    // preserves byte-identity.
    if (ok && m_slotDispatcher)
    {
        m_slotDispatcher->OnEnqueue(slot, item, this);
    }
    return ok;
}

Ptr<QueueDiscItem>
EdgeQueueDisc::DoDequeue()
{
    NS_LOG_FUNCTION(this);

    // Across-slot dispatch policy is delegated to the pluggable
    // SlotDispatcher strategy. The default installed in the
    // constructor is StrictPriorityDispatcher (byte-identical
    // strict-priority loop body). TOS rewrite remains the composer's
    // responsibility and runs after the dispatcher selects a slot.
    NS_ASSERT_MSG(m_slotDispatcher, "EdgeQueueDisc requires a non-null slot dispatcher");
    int32_t slot = m_slotDispatcher->SelectDequeueSlot(this);
    if (slot < 0)
    {
        return nullptr;
    }
    Ptr<QueueDiscItem> item = m_inners[slot]->Dequeue();
    if (!item)
    {
        return nullptr;
    }
    m_slotDispatcher->OnDequeue(static_cast<uint32_t>(slot), item, this);

    // Read the DSCP tag and rewrite the IPv4 header TOS field.
    // The inner only peeks the tag; removal is the composer's job.
    // When Wash is enabled, the high six TOS bits are zeroed at egress
    // (Linux tc-cake `wash` semantics) while ECN bits are preserved.
    DscpTag tag;
    if (item->GetPacket()->PeekPacketTag(tag))
    {
        Ptr<Ipv4QueueDiscItem> ipItem = DynamicCast<Ipv4QueueDiscItem>(item);
        if (ipItem)
        {
            uint8_t dscp = tag.GetDscp();
            auto& hdr = const_cast<Ipv4Header&>(ipItem->GetHeader());
            const uint8_t washedDscp = m_wash ? 0u : dscp;
            hdr.SetTos((washedDscp << 2) | (hdr.GetTos() & 0x3));
            NS_LOG_DEBUG("Dequeue: rewrote TOS to DSCP=" << static_cast<uint32_t>(washedDscp)
                                                         << (m_wash ? " (washed)" : ""));
        }
        ConstCast<Packet>(item->GetPacket())->RemovePacketTag(tag);
    }
    else if (m_wash)
    {
        // Untagged path (no classifier mark hit): wash still applies — a
        // packet with a pre-existing DSCP that bypassed our classifier
        // must still leave with DSCP=0 if Wash is on, so downstream
        // forwarders see CS0/Default.
        Ptr<Ipv4QueueDiscItem> ipItem = DynamicCast<Ipv4QueueDiscItem>(item);
        if (ipItem)
        {
            auto& hdr = const_cast<Ipv4Header&>(ipItem->GetHeader());
            hdr.SetTos(hdr.GetTos() & 0x3);
        }
    }

    return item;
}

Ptr<const QueueDiscItem>
EdgeQueueDisc::DoPeek()
{
    NS_LOG_FUNCTION(this);
    // Peek mirrors DoDequeue's dispatcher call via the side-effect-free
    // PeekSlot virtual. StrictPriorityDispatcher's PeekSlot shares its
    // FirstNonEmpty walk with SelectDequeueSlot, so peek-then-dequeue
    // patterns see the same slot in both calls.
    NS_ASSERT_MSG(m_slotDispatcher, "EdgeQueueDisc requires a non-null slot dispatcher");
    int32_t slot = m_slotDispatcher->PeekSlot(this);
    if (slot < 0)
    {
        return nullptr;
    }
    return m_inners[slot]->Peek();
}

bool
EdgeQueueDisc::CheckConfig()
{
    NS_LOG_FUNCTION(this);

    if (GetNInternalQueues() > 0)
    {
        NS_LOG_ERROR("EdgeQueueDisc must not have internal queues");
        return false;
    }

    EnsureDefaultInner();

    // At least one populated inner slot (slot 0). Upper bound is
    // enforced structurally by kMaxInnerSlots and by SetInnerDiscAt's
    // monotonic-fill assertion. EnsureDefaultInner has already
    // mirrored every populated slot into a QueueDiscClass child.
    NS_ASSERT_MSG(GetNQueueDiscClasses() >= 1,
                  "EdgeQueueDisc requires at least one "
                  "QueueDiscClass child (the inner at slot 0); got "
                      << GetNQueueDiscClasses());
    NS_ASSERT_MSG(GetNQueueDiscClasses() == GetNumInnerSlots(),
                  "QueueDiscClass count (" << GetNQueueDiscClasses()
                                           << ") must match populated inner slots ("
                                           << GetNumInnerSlots() << ")");
    if (GetNQueueDiscClasses() < 1)
    {
        return false;
    }

    // follow-up: pre-materialise meter slots for every MeterType
    // referenced by the DSCP-keyed policy table. Without this, a
    // caller that does `edge->AssignStreams(N)` before Initialize
    // would see empty slots and the cascade would miss; the first
    // packet would then create the meter on demand and silently
    // bind it to the global default RNG stream, defeating the
    // opt-in guarantee. Pre-creation is a no-op for scenarios that
    // never call AssignStreams: GetMeter still creates the meter on
    // first request, just from CheckConfig time instead of
    // first-packet time, with byte-identical output.
    if (m_policyClassifier)
    {
        for (MeterType t : m_policyClassifier->GetUsedMeterTypes())
        {
            GetMeter(t); // creates the slot if empty; idempotent
        }
    }

    return true;
}

void
EdgeQueueDisc::InitializeParams()
{
    NS_LOG_FUNCTION(this);
    // No composer-level parameters to initialise. The inner's own
    // InitializeParams fires when QueueDisc::DoInitialize iterates
    // m_classes.
}

void
EdgeQueueDisc::DoDispose()
{
    NS_LOG_FUNCTION(this);
    for (uint32_t s = 0; s < kMaxInnerSlots; ++s)
    {
        m_inners[s] = nullptr;
    }
    if (m_policyClassifier)
    {
        // Classifier holds a raw pointer back to us; clear before we die.
        m_policyClassifier->SetMeterProvider(nullptr);
    }
    m_policyClassifier = nullptr;
    m_perFlowClassifier = nullptr;
    for (std::size_t i = 0; i < kMeterPoolSize; ++i)
    {
        m_meters[i] = nullptr;
    }
    m_slotDispatcher = nullptr;
    QueueDisc::DoDispose();
}

} // namespace ns3::stratum
