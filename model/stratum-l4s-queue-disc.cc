/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * L4S queue disc — composition over ns3::QueueDisc with a pluggable
 * inner classic AQM.
 */

#include "stratum-l4s-queue-disc.h"

#include "stratum-l4s-timestamp-tag.h"
#include "stratum-red-sub-queue.h"
#include "stratum-rr-scheduler.h"

#include "ns3/double.h"
#include "ns3/enum.h"
#include "ns3/fifo-queue-disc.h"
#include "ns3/fq-codel-queue-disc.h"
#include "ns3/ipv4-header.h"
#include "ns3/ipv4-queue-disc-item.h"
#include "ns3/log.h"
#include "ns3/nstime.h"
#include "ns3/packet.h"
#include "ns3/queue-disc.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"

#include <algorithm>
#include <cmath>

namespace ns3::stratum::l4s
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::l4s::QueueDisc");

NS_OBJECT_ENSURE_REGISTERED(QueueDisc);

TypeId
QueueDisc::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::stratum::l4s::QueueDisc")
            .SetParent<ns3::QueueDisc>()
            .SetGroupName("Stratum")
            .AddConstructor<QueueDisc>()
            .AddAttribute("L4sQueueIdx",
                          "Scheduler-slot index the attached scheduler treats as "
                          "the L4S lane; must match the scheduler's own "
                          "L4sQueueIdx. The composer's child queue discs sit at "
                          "fixed child slots (L4S = 0, classic = 1) regardless.",
                          UintegerValue(1),
                          MakeUintegerAccessor(&QueueDisc::m_l4sQueueIdxLegacy),
                          MakeUintegerChecker<uint32_t>(0, kMaxQueues - 1))
            .AddAttribute("L4sTargetSojournMs",
                          "L4S step-marking threshold in milliseconds. Linux/GPRT "
                          "DualPI2 default 1.0 ms; RFC 9332 App. A.1 gives an "
                          "800 us + 400 us ramp example, configurable as a step.",
                          DoubleValue(1.0),
                          MakeDoubleAccessor(&QueueDisc::SetL4sTargetSojournMs,
                                             &QueueDisc::GetL4sTargetSojournMs),
                          MakeDoubleChecker<double>(0.0))
            .AddAttribute("ClassicTargetSojournMs",
                          "Classic-queue target sojourn time in milliseconds; drives the "
                          "P.I.² controller's integrator. RFC 9332 App. A.1 Fig. 2 "
                          "default 15.0 ms.",
                          DoubleValue(15.0),
                          MakeDoubleAccessor(&QueueDisc::SetClassicTargetSojournMs,
                                             &QueueDisc::GetClassicTargetSojournMs),
                          MakeDoubleChecker<double>(0.0))
            .AddAttribute(
                "CouplingFactor",
                "Coupling factor k. The coupled L4S mark probability is "
                "k * p'; the classic drop probability p'^2 is k-independent "
                "(RFC 9332 Section 2.1 equation (1)). RFC 9332 default k = 2.",
                DoubleValue(2.0),
                MakeDoubleAccessor(&QueueDisc::SetCouplingFactor, &QueueDisc::GetCouplingFactor),
                MakeDoubleChecker<double>(0.0))
            .AddAttribute(
                "ClassicAqm",
                "Classic-queue AQM strategy. Wred = parent WRED + coupled drop "
                "overlay. CoupledOnly = coupled drop is sole AQM. "
                "FqCoDel = mainline FqCoDelQueueDisc as inner classic AQM. "
                "Construct-only: consumed by CheckConfig to pick "
                "the default inner AQM; Config::Set after Initialize has no "
                "effect because the inner is already materialised.",
                TypeId::ATTR_GET | TypeId::ATTR_CONSTRUCT,
                EnumValue(ClassicAqm::Wred),
                MakeEnumAccessor<ClassicAqm>(&QueueDisc::SetClassicAqm, &QueueDisc::GetClassicAqm),
                MakeEnumChecker(ClassicAqm::Wred,
                                "Wred",
                                ClassicAqm::CoupledOnly,
                                "CoupledOnly",
                                ClassicAqm::FqCoDel,
                                "FqCoDel"))
            .AddAttribute(
                "L4sBandwidthBps",
                "Bandwidth used as a sojourn-time fallback proxy when no "
                "per-packet enqueue-time tracking is available. Default 1 Gbps.",
                DoubleValue(1e9),
                MakeDoubleAccessor(&QueueDisc::SetL4sBandwidthBps, &QueueDisc::GetL4sBandwidthBps),
                MakeDoubleChecker<double>(1.0))
            .AddAttribute("ControllerInterval",
                          "Periodic P.I controller tick interval. RFC 9332 App. A.1 "
                          "default Tupdate = 16 ms.",
                          TimeValue(MilliSeconds(16)),
                          MakeTimeAccessor(&QueueDisc::SetControllerInterval,
                                           &QueueDisc::GetControllerInterval),
                          MakeTimeChecker(MilliSeconds(1)))
            .AddTraceSource("L4sClassified",
                            "Fires per enqueue with a routing flag: true for "
                            "L4S sub-queue, false for classic path.",
                            MakeTraceSourceAccessor(&QueueDisc::m_l4sClassifiedTrace),
                            "ns3::stratum::l4s::QueueDisc::ClassifiedTracedCallback")
            .AddTraceSource("L4sMarked",
                            "Fires when a packet is CE-marked by the L4S immediate-mark "
                            "step.",
                            MakeTraceSourceAccessor(&QueueDisc::m_l4sMarkTrace),
                            "ns3::stratum::l4s::QueueDisc::L4sMarkedTracedCallback")
            .AddTraceSource("ClassicCoupledDrop",
                            "Fires when a classic-path packet is dropped by the coupled p_C.",
                            MakeTraceSourceAccessor(&QueueDisc::m_classicCoupledDropTrace),
                            "ns3::stratum::l4s::QueueDisc::"
                            "ClassicCoupledDropTracedCallback");
    return tid;
}

QueueDisc::QueueDisc()
    : ns3::QueueDisc(QueueDiscSizePolicy::NO_LIMITS, QueueSizeUnit::PACKETS),
      m_l4sQueueIdxLegacy(1),
      m_l4sTargetSojournMs(1.0),
      m_classicTargetSojournMs(15.0),
      m_couplingFactor(2.0),
      m_classicAqmMode(ClassicAqm::Wred),
      m_l4sBandwidthBps(1e9),
      m_mtu(1500),
      m_baseProb(0.0),
      m_controllerInterval(MilliSeconds(16)),
      m_lastSojournMs(0.0),
      m_forceBaseProbForTest(false),
      m_lastCoupledProb(0.0),
      m_lastL4sMarkProb(0.0)
{
    NS_LOG_FUNCTION(this);
    m_rng = CreateObject<UniformRandomVariable>();
}

QueueDisc::~QueueDisc()
{
    NS_LOG_FUNCTION(this);
}

// --- Strategy injection ---

void
QueueDisc::EnsureDefaultChildren()
{
    if (!m_classicAqm)
    {
        // The enum mode chooses the default inner disc when the caller
        // did not inject one via SetClassicAqmDisc. FqCoDel produces a
        // mainline FqCoDelQueueDisc; Wred and CoupledOnly
        // both keep the RedQueueDisc default (CoupledOnly differs at
        // DoInitialize time, not at construction time).
        if (m_classicAqmMode == ClassicAqm::FqCoDel)
        {
            auto fq = CreateObject<FqCoDelQueueDisc>();
            // FqCoDel's Quantum auto-sets from the NetDevice MTU only when
            // installed on a device. As a nested inner disc there is no
            // device, so CheckConfig would fail on Quantum=0. Force the
            // Ethernet-MTU default used by l4s-routing-test and
            // diffserv-l4s-fqcodel-comparison.
            fq->SetQuantum(1500);
            m_classicAqm = fq;
        }
        else
        {
            m_classicAqm = CreateObject<RedQueueDisc>();
        }
    }
    if (!m_l4sQueue)
    {
        m_l4sQueue = CreateObject<FifoQueueDisc>();
    }
    // Register as QueueDiscClass children if not yet present. The two
    // slots are fixed by the kL4sChildIdx / kClassicChildIdx constants.
    if (GetNQueueDiscClasses() == 0)
    {
        Ptr<QueueDiscClass> l4sCls = CreateObject<QueueDiscClass>();
        l4sCls->SetQueueDisc(m_l4sQueue);
        AddQueueDiscClass(l4sCls);

        Ptr<QueueDiscClass> classicCls = CreateObject<QueueDiscClass>();
        classicCls->SetQueueDisc(m_classicAqm);
        AddQueueDiscClass(classicCls);
    }
}

void
QueueDisc::SetClassicAqmDisc(Ptr<ns3::QueueDisc> aqm)
{
    NS_LOG_FUNCTION(this << aqm);
    NS_ASSERT_MSG(GetNQueueDiscClasses() == 0,
                  "SetClassicAqmDisc must be called before Initialize");
    m_classicAqm = aqm;
}

Ptr<ns3::QueueDisc>
QueueDisc::GetClassicAqmDisc() const
{
    return m_classicAqm;
}

void
QueueDisc::SetL4sQueueDisc(Ptr<ns3::QueueDisc> l4s)
{
    NS_LOG_FUNCTION(this << l4s);
    NS_ASSERT_MSG(GetNQueueDiscClasses() == 0, "SetL4sQueueDisc must be called before Initialize");
    m_l4sQueue = l4s;
}

Ptr<ns3::QueueDisc>
QueueDisc::GetL4sQueueDisc() const
{
    return m_l4sQueue;
}

void
QueueDisc::SetScheduler(Ptr<Scheduler> scheduler)
{
    NS_LOG_FUNCTION(this << scheduler);
    m_scheduler = scheduler;
}

Ptr<Scheduler>
QueueDisc::GetScheduler() const
{
    return m_scheduler;
}

Ptr<RedQueueDisc>
QueueDisc::GetClassicAsRed() const
{
    Ptr<RedQueueDisc> red = DynamicCast<RedQueueDisc>(m_classicAqm);
    NS_ASSERT_MSG(red,
                  "Classic AQM is not a RedQueueDisc; this accessor "
                  "is only valid when the inner classic AQM is Red");
    return red;
}

// --- PHB forwarders ---

void
QueueDisc::AddPhbEntry(uint8_t codePt, uint8_t queue, uint8_t prec)
{
    NS_LOG_FUNCTION(this << static_cast<uint32_t>(codePt));
    if (!m_classicAqm)
    {
        EnsureDefaultChildren();
    }
    m_classicUserConfigured = true;
    GetClassicAsRed()->AddPhbEntry(codePt, queue, prec);
}

bool
QueueDisc::LookupPhb(uint8_t codePt, uint8_t& queue, uint8_t& prec) const
{
    return GetClassicAsRed()->LookupPhb(codePt, queue, prec);
}

// --- Red-specific forwarders ---

void
QueueDisc::SetNumQueues(uint32_t numQueues)
{
    if (!m_classicAqm)
    {
        EnsureDefaultChildren();
    }
    GetClassicAsRed()->SetNumQueues(numQueues);
}

uint32_t
QueueDisc::GetNumQueues() const
{
    return GetClassicAsRed()->GetNumQueues();
}

void
QueueDisc::PrintStats() const
{
    // QueueStatsProvider override. Simple summary: forward to the
    // classic-AQM side's stats if Red; L4S-side occupancy is already
    // observable via QueueLen sampling.
    Ptr<RedQueueDisc> red = DynamicCast<RedQueueDisc>(m_classicAqm);
    if (red)
    {
        red->PrintStats();
    }
}

void
QueueDisc::ConfigQueue(uint32_t q, uint32_t prec, double thMin, double thMax, double maxP)
{
    m_classicUserConfigured = true;
    GetClassicAsRed()->ConfigQueue(q, prec, thMin, thMax, maxP);
}

void
QueueDisc::SetMredMode(MredMode mode, uint32_t q)
{
    m_classicUserConfigured = true;
    GetClassicAsRed()->SetMredMode(mode, q);
}

void
QueueDisc::SetNumPrec(uint32_t q, uint32_t n)
{
    GetClassicAsRed()->SetNumPrec(q, n);
}

void
QueueDisc::SetQueueLimit(uint32_t q, uint32_t n)
{
    if (q == m_l4sQueueIdxLegacy)
    {
        // L4S slot: set the FIFO child's MaxSize. FifoQueueDisc's default
        // is 1000 packets, which is too small for the coupling-invariant
        // tests that enqueue 4000+ ECT(1) packets.
        if (!m_l4sQueue)
        {
            EnsureDefaultChildren();
        }
        m_l4sQueue->SetAttribute("MaxSize", QueueSizeValue(QueueSize(QueueSizeUnit::PACKETS, n)));
        return;
    }
    m_classicUserConfigured = true;
    GetClassicAsRed()->SetQueueLimit(q, n);
}

void
QueueDisc::SetMeanPacketSize(int mps)
{
    GetClassicAsRed()->SetMeanPacketSize(mps);
}

void
QueueDisc::SetQueueBandwidth(uint32_t q, double bps)
{
    GetClassicAsRed()->SetQueueBandwidth(q, bps);
}

int
QueueDisc::GetVirtualQueueLen(uint32_t q, uint32_t prec) const
{
    if (q == m_l4sQueueIdxLegacy)
    {
        // L4S lane — FifoQueueDisc has no precedence, report current size.
        return m_l4sQueue ? static_cast<int>(m_l4sQueue->GetCurrentSize().GetValue()) : 0;
    }
    return GetClassicAsRed()->GetVirtualQueueLen(q, prec);
}

// --- Attribute setters/getters ---

void
QueueDisc::SetL4sQueueIdx(uint32_t idx)
{
    NS_LOG_FUNCTION(this << idx << " [scheduler-slot mapping; composer child slots stay fixed]");
    m_l4sQueueIdxLegacy = idx;
}

uint32_t
QueueDisc::GetL4sQueueIdx() const
{
    return m_l4sQueueIdxLegacy;
}

void
QueueDisc::SetL4sTargetSojournMs(double ms)
{
    m_l4sTargetSojournMs = ms;
}

double
QueueDisc::GetL4sTargetSojournMs() const
{
    return m_l4sTargetSojournMs;
}

void
QueueDisc::SetClassicTargetSojournMs(double ms)
{
    m_classicTargetSojournMs = ms;
}

double
QueueDisc::GetClassicTargetSojournMs() const
{
    return m_classicTargetSojournMs;
}

void
QueueDisc::SetCouplingFactor(double k)
{
    m_couplingFactor = k;
}

double
QueueDisc::GetCouplingFactor() const
{
    return m_couplingFactor;
}

void
QueueDisc::SetClassicAqm(ClassicAqm m)
{
    m_classicAqmMode = m;
}

QueueDisc::ClassicAqm
QueueDisc::GetClassicAqm() const
{
    return m_classicAqmMode;
}

void
QueueDisc::SetL4sBandwidthBps(double bps)
{
    m_l4sBandwidthBps = bps;
}

double
QueueDisc::GetL4sBandwidthBps() const
{
    return m_l4sBandwidthBps;
}

void
QueueDisc::SetControllerInterval(Time interval)
{
    m_controllerInterval = interval;
}

Time
QueueDisc::GetControllerInterval() const
{
    return m_controllerInterval;
}

double
QueueDisc::GetBaseProb() const
{
    return m_baseProb;
}

double
QueueDisc::GetLastClassicCoupledProb() const
{
    return m_lastCoupledProb;
}

double
QueueDisc::GetLastL4sMarkProb() const
{
    return m_lastL4sMarkProb;
}

void
QueueDisc::ForceBaseProbForTest(double p)
{
    m_baseProb = std::clamp(p, 0.0, 1.0);
    m_forceBaseProbForTest = true;
}

void
QueueDisc::ClearForcedBaseProbForTest()
{
    m_forceBaseProbForTest = false;
}

int64_t
QueueDisc::AssignStreams(int64_t stream)
{
    NS_LOG_FUNCTION(this << stream);
    // Walk children in fixed order so RNG-stream assignment is stable.
    // Order matters for byte-for-byte S1/S2 equivalence; do not
    // re-order without a matching re-baseline.
    m_rng->SetStream(stream);
    int64_t next = stream + 1;

    // m_l4sQueue (FifoQueueDisc) has no RNG: 0 streams consumed.

    // m_classicAqm as RedQueueDisc: walk inner RedSubQueue children.
    Ptr<RedQueueDisc> red = DynamicCast<RedQueueDisc>(m_classicAqm);
    if (red)
    {
        for (uint32_t i = 0; i < red->GetNQueueDiscClasses(); ++i)
        {
            Ptr<RedSubQueue> sub =
                DynamicCast<RedSubQueue>(red->GetQueueDiscClass(i)->GetQueueDisc());
            if (sub)
            {
                sub->AssignStreams(next++);
            }
        }
    }
    return next - stream;
}

// --- Classification + enqueue ---

bool
QueueDisc::IsL4sPacket(Ptr<const QueueDiscItem> item) const
{
    Ptr<const Ipv4QueueDiscItem> ipItem = DynamicCast<const Ipv4QueueDiscItem>(item);
    if (!ipItem)
    {
        return false;
    }
    Ipv4Header::EcnType ecn = ipItem->GetHeader().GetEcn();
    return ecn == Ipv4Header::ECN_ECT1 || ecn == Ipv4Header::ECN_CE;
}

double
QueueDisc::ComputeItemSojournMs(Ptr<const QueueDiscItem> item) const
{
    if (!item)
    {
        return 0.0;
    }
    // The item's own enqueue timestamp — true per-packet sojourn, read at
    // dequeue so each packet is judged against its own age.
    TimestampTag tag;
    if (item->GetPacket()->PeekPacketTag(tag))
    {
        Time sojourn = Simulator::Now() - tag.GetTimestamp();
        return sojourn.GetSeconds() * 1e3;
    }
    return 0.0;
}

uint32_t
QueueDisc::TotalQueueBytes() const
{
    uint32_t bytes = 0;
    if (m_l4sQueue)
    {
        bytes += m_l4sQueue->GetNBytes();
    }
    if (m_classicAqm)
    {
        bytes += m_classicAqm->GetNBytes();
    }
    return bytes;
}

double
QueueDisc::ComputeClassicSojournMs() const
{
    if (!m_classicAqm)
    {
        return 0.0;
    }

    // Head packet's enqueue timestamp — true sojourn. Classic packets are
    // tagged with the same TimestampTag in DoEnqueue so the same
    // measurement path applies on both sub-queues.
    Ptr<const QueueDiscItem> head = m_classicAqm->Peek();
    if (head)
    {
        TimestampTag tag;
        if (head->GetPacket()->PeekPacketTag(tag))
        {
            Time sojourn = Simulator::Now() - tag.GetTimestamp();
            return sojourn.GetSeconds() * 1e3;
        }
    }

    // Bandwidth proxy fallback (same bottleneck rate as the L4S lane).
    // GetNPackets() (not GetCurrentSize) so we don't require the inner AQM
    // to advertise a MaxSize policy — under ClassicAqm::CoupledOnly the
    // classic AQM may be NO_LIMITS by design.
    uint32_t qlen = m_classicAqm->GetNPackets();
    if (qlen == 0 || m_l4sBandwidthBps <= 0.0)
    {
        return 0.0;
    }
    constexpr double kMtuBytes = 1500.0;
    double sojournSec = static_cast<double>(qlen) * 8.0 * kMtuBytes / m_l4sBandwidthBps;
    return sojournSec * 1e3;
}

void
QueueDisc::UpdateBaseProb()
{
    if (m_forceBaseProbForTest)
    {
        return;
    }

    // RFC 9332 App. A.1 Figure 2 lines 13-14:
    //   alpha = 0.1 * Tupdate / RTT_max^2;  beta = 0.3 / RTT_max
    // evaluated at the RFC defaults Tupdate = 16 ms, RTT_max = 100 ms,
    // for which the RFC states "alpha = 0.16; beta = 3.2". alpha is
    // pre-multiplied by Tupdate, so both gains apply once per tick.
    constexpr double kAlphaHz = 0.16;
    constexpr double kBetaHz = 3.2;

    // RFC 9332 App. A.1 Figure 6 line 2 (curq = cq.time()): the P.I.²
    // controller integrates the *classic* queue's sojourn against the
    // classic target. (The App. A.2 overload variant samples
    // max(cq.time(), lq.time()) instead; this implementation uses the
    // A.1 form.) The L4S queue's sojourn drives a separate step-AQM
    // mechanism (immediate marking at L4sTargetSojournMs), not the PI
    // integrator.
    double sojournMs = ComputeClassicSojournMs();
    double sojournSec = sojournMs * 1e-3;
    double targetSec = m_classicTargetSojournMs * 1e-3;
    double prevSojournSec = m_lastSojournMs * 1e-3;

    double error = sojournSec - targetSec;
    // GPRT and the RFC 9332 App. A.1 pseudocode initialise prevq = 0
    // (m_lastSojournMs starts at 0), so the first tick applies the full
    // beta*(curq - 0) proportional term rather than suppressing it.
    double derivative = sojournSec - prevSojournSec;

    m_baseProb += kAlphaHz * error + kBetaHz * derivative;
    m_baseProb = std::clamp(m_baseProb, 0.0, 1.0);

    m_lastSojournMs = sojournMs;
}

void
QueueDisc::ControllerTick()
{
    NS_LOG_FUNCTION(this);
    UpdateBaseProb();
    m_controllerEvent = Simulator::Schedule(m_controllerInterval, &QueueDisc::ControllerTick, this);
}

double
QueueDisc::ComputeCoupledDropProb() const
{
    // RFC 9332 App. A.1 Figure 6 line 5: p_C = p'^2. The coupling
    // factor k scales the L4S-side probability p_CL = k * p' only
    // (§2.1 eq. (1): p_C = (p_CL / k)^2); it must not appear here.
    return std::clamp(m_baseProb * m_baseProb, 0.0, 1.0);
}

bool
QueueDisc::ApplyL4sCoupledMark(Ptr<QueueDiscItem> item)
{
    // Step branch (RFC 9332 App. A.1 StepAqm): mark with certainty once the
    // packet's own sojourn reaches the L4S target. Coupled branch:
    // p_CL = k * p' (App. A.1 Fig. 6 line 4), capped at 1 like the recur()
    // marking it feeds. The coupled probabilistic mark is suppressed while
    // the total queue is below two MTUs (App. A.1 MustDrop floor); the step
    // mark is not gated by the floor.
    double sojournMs = ComputeItemSojournMs(item);
    double pL;
    if (sojournMs >= m_l4sTargetSojournMs)
    {
        pL = 1.0;
    }
    else if (TotalQueueBytes() < 2 * m_mtu)
    {
        pL = 0.0;
    }
    else
    {
        pL = std::clamp(m_couplingFactor * m_baseProb, 0.0, 1.0);
    }
    m_lastL4sMarkProb = pL;

    bool drewMark = (pL > 0.0) && (m_rng->GetValue(0.0, 1.0) < pL);
    if (!drewMark)
    {
        return false;
    }

    // RFC 9331 §5 CE idempotence guard.
    Ptr<const Ipv4QueueDiscItem> ipItem = DynamicCast<const Ipv4QueueDiscItem>(item);
    if (ipItem && ipItem->GetHeader().GetEcn() == Ipv4Header::ECN_CE)
    {
        return true;
    }

    bool marked = Mark(item, "L4S_IMMEDIATE_MARK");
    if (marked)
    {
        m_l4sMarkTrace(item);
    }
    return true;
}

bool
QueueDisc::MaybeCoupledDrop(Ptr<QueueDiscItem> item)
{
    // RFC 9332 App. A.1 MustDrop floor (GPRT m_thLen = 2 * m_mtu): take no
    // coupled action while the total queue sits below two MTUs.
    if (TotalQueueBytes() < 2 * m_mtu)
    {
        return false;
    }
    double pC = ComputeCoupledDropProb();
    m_lastCoupledProb = pC;
    if (pC <= 0.0)
    {
        return false;
    }
    if (m_rng->GetValue(0.0, 1.0) < pC)
    {
        m_classicCoupledDropTrace(item, pC);
        // Composer-originated drop (before delegation to either child):
        // the manual DropBeforeEnqueue is correct here because no child
        // disc has been touched yet, so no ChildQueueDiscDropFunctor is
        // in play.
        DropBeforeEnqueue(item, "L4S_COUPLED_DROP");
        return true;
    }
    return false;
}

bool
QueueDisc::DoEnqueue(Ptr<QueueDiscItem> item)
{
    NS_LOG_FUNCTION(this << item);

    if (IsL4sPacket(item))
    {
        m_l4sClassifiedTrace(item, true);
        return EnqueueL4s(item);
    }

    m_l4sClassifiedTrace(item, false);

    if (MaybeCoupledDrop(item))
    {
        return false;
    }

    // Timestamp tag for true classic-queue sojourn measurement at the head.
    // Mirrors the L4S-lane tagging in EnqueueL4s(); ComputeClassicSojournMs()
    // reads this tag from the classic sub-queue's head packet.
    TimestampTag tag(Simulator::Now());
    item->GetPacket()->ReplacePacketTag(tag);

    bool ok = m_classicAqm->Enqueue(item);
    if (ok && m_scheduler)
    {
        const uint32_t classicSlot = (m_l4sQueueIdxLegacy == 0) ? 1 : 0;
        m_scheduler->OnEnqueueWithTime(classicSlot, item->GetSize(), Simulator::Now().GetSeconds());
    }
    return ok;
}

bool
QueueDisc::EnqueueL4s(Ptr<QueueDiscItem> item)
{
    NS_LOG_FUNCTION(this << item);

    // Timestamp tag for per-packet sojourn measurement at dequeue. The
    // coupled / step CE mark is applied in DoDequeue (RFC 9332 App. A.1
    // StepAqm), keyed off this stamp, not at enqueue.
    TimestampTag tag(Simulator::Now());
    item->GetPacket()->ReplacePacketTag(tag);

    bool ok = m_l4sQueue->Enqueue(item);
    if (ok && m_scheduler)
    {
        m_scheduler->OnEnqueueWithTime(m_l4sQueueIdxLegacy,
                                       item->GetSize(),
                                       Simulator::Now().GetSeconds());
    }
    return ok;
}

Ptr<QueueDiscItem>
QueueDisc::DoDequeue()
{
    if (!m_scheduler)
    {
        // Fallback dispatcher: L4S-first, classic-fallback. Preserves
        // the priority intent when no scheduler is configured.
        if (m_l4sQueue)
        {
            Ptr<QueueDiscItem> item = m_l4sQueue->Dequeue();
            if (item)
            {
                // RFC 9332 App. A.1: apply the L4S step / coupled CE mark
                // per packet, on the way out.
                ApplyL4sCoupledMark(item);
                return item;
            }
        }
        if (m_classicAqm)
        {
            return m_classicAqm->Dequeue();
        }
        return nullptr;
    }

    int32_t idx = m_scheduler->SelectNextQueue();
    if (idx < 0)
    {
        return nullptr;
    }
    if (static_cast<uint32_t>(idx) == m_l4sQueueIdxLegacy)
    {
        Ptr<QueueDiscItem> item = m_l4sQueue->Dequeue();
        if (item)
        {
            // RFC 9332 App. A.1: apply the L4S step / coupled CE mark per
            // packet, on the way out.
            ApplyL4sCoupledMark(item);
        }
        return item;
    }
    return m_classicAqm->Dequeue();
}

Ptr<const QueueDiscItem>
QueueDisc::DoPeek()
{
    // Match DoDequeue priority order when no scheduler is configured.
    if (m_l4sQueue)
    {
        Ptr<const QueueDiscItem> item = m_l4sQueue->Peek();
        if (item)
        {
            return item;
        }
    }
    if (m_classicAqm)
    {
        return m_classicAqm->Peek();
    }
    return nullptr;
}

bool
QueueDisc::CheckConfig()
{
    NS_LOG_FUNCTION(this);

    if (GetNInternalQueues() > 0)
    {
        NS_LOG_ERROR("QueueDisc must not have internal queues");
        return false;
    }

    EnsureDefaultChildren();

    // Briscoe draft-briscoe-tsvwg-l4s-diffserv-02 §4 deploys the DualQ
    // "as if it were an indivisible 'atomic' component" in every one of
    // its examples. The fixed 2-child shape (L4S
    // + classic) is the structural expression of that prescription; any
    // deviation breaks the atomicity guarantee documented in the class
    // header.
    NS_ASSERT_MSG(GetNQueueDiscClasses() == 2,
                  "DualQ atomicity violated: QueueDisc requires exactly "
                  "2 QueueDiscClass children (L4S at idx 0, classic at idx 1) "
                  "per Briscoe draft-briscoe-tsvwg-l4s-diffserv-02 §4. "
                  "Got: "
                      << GetNQueueDiscClasses());
    if (GetNQueueDiscClasses() != 2)
    {
        NS_LOG_ERROR("QueueDisc expects exactly 2 children "
                     "(0=L4S, 1=classic); got "
                     << GetNQueueDiscClasses());
        return false;
    }

    // Any Ptr<ns3::QueueDisc> is accepted as the inner classic AQM.
    // Type-specific forwarders (PHB, WRED config, etc.) assert on
    // their own when called against a foreign inner.
    return true;
}

void
QueueDisc::InitializeParams()
{
    NS_LOG_FUNCTION(this);
    // Composer-level param init runs before child initialization (see
    // ns3::QueueDisc::DoInitialize order). The CoupledOnly munging and the
    // controller arming both need children to already exist, so they
    // live in DoInitialize instead.
}

void
QueueDisc::DoInitialize()
{
    NS_LOG_FUNCTION(this);

    // Let the base orchestrate CheckConfig -> InitializeParams -> child
    // Initialize() cascades. After this returns, m_classicAqm's inner
    // RedSubQueue children have been auto-created and initialized.
    ns3::QueueDisc::DoInitialize();

    if (m_classicAqmMode == ClassicAqm::CoupledOnly)
    {
        // CoupledOnly only makes sense against the Red pipeline it was
        // designed for. With a foreign inner AQM (FqCoDel, PIE, ...)
        // there is no WRED early-drop to suppress, so the mode is a
        // silent no-op — the coupled p_C drop in MaybeCoupledDrop still
        // fires inner-agnostically.
        Ptr<RedQueueDisc> red = DynamicCast<RedQueueDisc>(m_classicAqm);
        if (red)
        {
            for (uint32_t q = 0; q < red->GetNQueueDiscClasses(); ++q)
            {
                Ptr<RedSubQueue> subQ =
                    DynamicCast<RedSubQueue>(red->GetQueueDiscClass(q)->GetQueueDisc());
                if (!subQ)
                {
                    continue;
                }
                subQ->SetMredMode(MredMode::DROP_TAIL);
                uint32_t qlim = subQ->GetQueueLimit();
                auto passThrough = static_cast<double>(qlim + 1);
                for (uint32_t prec = 0; prec < subQ->GetNumPrec(); ++prec)
                {
                    subQ->ConfigureVirtualQueue(prec, passThrough, passThrough, 0.0);
                }
            }
            NS_LOG_INFO("CoupledOnly: inner classic sub-queues configured as "
                        "pass-through FIFO");
        }
        else
        {
            NS_LOG_INFO("CoupledOnly + non-Red inner AQM: no pass-through munging applies");
        }
    }
    else if (m_classicAqmMode == ClassicAqm::Wred && !m_classicUserConfigured)
    {
        // Wred mode + no user config: inject sane defaults so a fresh
        // `Wred` enum picker gets a functional classic queue rather than
        // the trap-chain default (RIO_C with thMin=thMax=0, empty PHB,
        // qlim=0 → near-100% drop). When the caller has already touched
        // the classic config (SetQueueLimit / ConfigQueue / SetMredMode /
        // AddPhbEntry), this block is skipped — the user's config wins
        // and we don't stomp on it during Initialize.
        Ptr<RedQueueDisc> red = DynamicCast<RedQueueDisc>(m_classicAqm);
        if (red)
        {
            for (uint32_t q = 0; q < red->GetNQueueDiscClasses(); ++q)
            {
                Ptr<RedSubQueue> subQ =
                    DynamicCast<RedSubQueue>(red->GetQueueDiscClass(q)->GetQueueDisc());
                if (!subQ)
                {
                    continue;
                }
                subQ->SetMredMode(MredMode::WRED);
                subQ->SetQueueLimit(25);
                for (uint32_t prec = 0; prec < subQ->GetNumPrec(); ++prec)
                {
                    subQ->ConfigureVirtualQueue(prec, 5.0, 15.0, 0.1);
                }
            }
            // PHB table: BE (DSCP 0) and EF (DSCP 46) -> sub-queue 0,
            // precedence 0. Without these AddPhbEntry calls the
            // LookupPhb path drops every classified packet with
            // NO_PHB_MATCH.
            red->AddPhbEntry(0, 0, 0);
            red->AddPhbEntry(46, 0, 0);
            NS_LOG_INFO("Wred: inner classic sub-queues configured as WRED "
                        "(thMin=5, thMax=15, maxP=0.1, qlim=25)");
        }
        else
        {
            NS_LOG_INFO("Wred + non-Red inner AQM: low-band mitigation skipped");
        }
    }

    // Arm the periodic controller tick (children are ready).
    m_controllerEvent = Simulator::Schedule(m_controllerInterval, &QueueDisc::ControllerTick, this);
}

void
QueueDisc::DoDispose()
{
    NS_LOG_FUNCTION(this);
    if (m_controllerEvent.IsPending())
    {
        Simulator::Cancel(m_controllerEvent);
    }
    m_l4sQueue = nullptr;
    m_classicAqm = nullptr;
    m_scheduler = nullptr;
    ns3::QueueDisc::DoDispose();
}

} // namespace ns3::stratum::l4s
