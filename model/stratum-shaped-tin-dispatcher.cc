/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "stratum-shaped-tin-dispatcher.h"

#include "stratum-edge-queue-disc.h"

#include "ns3/log.h"
#include "ns3/queue-disc.h"
#include "ns3/simulator.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::cake::ShapedTinDispatcher");

} // namespace ns3

namespace ns3::stratum::cake
{

NS_OBJECT_ENSURE_REGISTERED(ShapedTinDispatcher);

TypeId
ShapedTinDispatcher::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::cake::ShapedTinDispatcher")
                            .SetParent<SlotDispatcher>()
                            .SetGroupName("Stratum")
                            .AddConstructor<ShapedTinDispatcher>();
    return tid;
}

void
ShapedTinDispatcher::ConfigureGlobal(uint64_t rateBps)
{
    m_globalClock.rateBps = rateBps;
}

void
ShapedTinDispatcher::ConfigureTin(uint32_t slot,
                                  uint64_t rateBps,
                                  int32_t overhead,
                                  uint32_t mpu,
                                  RateBasedTinClock::FramingMode framing)
{
    if (slot >= m_tinClocks.size())
    {
        m_tinClocks.resize(slot + 1);
    }
    m_tinClocks[slot].rateBps = rateBps;
    m_tinClocks[slot].overhead = overhead;
    m_tinClocks[slot].mpu = mpu;
    m_tinClocks[slot].framing = framing;
}

void
ShapedTinDispatcher::SetTinPriorityOrder(std::vector<uint32_t> order)
{
    m_priorityOrder = std::move(order);
}

bool
ShapedTinDispatcher::AnyBacklog(const EdgeQueueDisc* edge)
{
    for (uint32_t s = 0; s < EdgeQueueDisc::kMaxInnerSlots; ++s)
    {
        Ptr<QueueDisc> inner = edge->GetInnerDiscAt(s);
        if (inner && inner->GetNPackets() > 0)
        {
            return true;
        }
    }
    return false;
}

int32_t
ShapedTinDispatcher::ScanBestSlot(const EdgeQueueDisc* edge, Time now) const
{
    // Scan backlogged tins in ascending priority order; any
    // schedule-meeting tin steals the choice (so the LAST meeting tin =
    // highest priority wins); when none meets its schedule, the
    // earliest-scheduled tin wins with ties to higher priority. A tin
    // behind its schedule is still served — its clock demotes, it does
    // not block.
    int32_t bestSlot = -1;
    Time bestTime = Time::Max();
    const uint32_t n = EdgeQueueDisc::kMaxInnerSlots;
    for (uint32_t k = 0; k < n; ++k)
    {
        const uint32_t slot = (k < m_priorityOrder.size()) ? m_priorityOrder[k] : k;
        Ptr<QueueDisc> inner = (slot < n) ? edge->GetInnerDiscAt(slot) : nullptr;
        if (!inner || inner->GetNPackets() == 0)
        {
            continue;
        }
        const Time tinNext = (slot < m_tinClocks.size()) ? m_tinClocks[slot].tNext : Time(0);
        const Time timeToSchedule = tinNext - now;
        if (timeToSchedule <= Time(0) || timeToSchedule <= bestTime)
        {
            bestTime = timeToSchedule;
            bestSlot = static_cast<int32_t>(slot);
        }
    }
    return bestSlot;
}

int32_t
ShapedTinDispatcher::SelectDequeueSlot(EdgeQueueDisc* edge)
{
    NS_LOG_FUNCTION(this);
    const Time now = Simulator::Now();
    if (!AnyBacklog(edge))
    {
        return -1;
    }
    // Aggregate hard shaper — the only gate. Blocked only while BOTH
    // the primary and the failsafe clock are in the future; self-wake
    // at their minimum.
    const Time gate = m_globalClock.EffectiveGate();
    if (gate > now)
    {
        m_selfWakeEvent.Cancel();
        m_selfWakeEvent = Simulator::Schedule(gate - now, &QueueDisc::Run, Ptr<QueueDisc>(edge));
        return -1;
    }
    return ScanBestSlot(edge, now);
}

int32_t
ShapedTinDispatcher::PeekSlot(EdgeQueueDisc* edge)
{
    NS_LOG_FUNCTION(this << edge);
    const Time now = Simulator::Now();
    if (!AnyBacklog(edge) || m_globalClock.EffectiveGate() > now)
    {
        return -1;
    }
    return ScanBestSlot(edge, now);
}

void
ShapedTinDispatcher::OnEnqueue(uint32_t slot, Ptr<QueueDiscItem> item, EdgeQueueDisc* edge)
{
    NS_LOG_FUNCTION(this << slot);
    const Time now = Simulator::Now();
    // Fires after a successful inner enqueue: a count of one on the
    // target slot means the tin just turned non-empty; a total of one
    // across all slots means the whole disc did.
    // Packets requeued at the base QueueDisc level are not visible here, so an enqueue in that
    // window can snap the clocks one packet early.
    uint32_t totalPkts = 0;
    for (uint32_t s = 0; s < EdgeQueueDisc::kMaxInnerSlots; ++s)
    {
        Ptr<QueueDisc> inner = edge->GetInnerDiscAt(s);
        totalPkts += inner ? inner->GetNPackets() : 0;
    }
    Ptr<QueueDisc> target = edge->GetInnerDiscAt(slot);
    if (target && target->GetNPackets() == 1 && slot < m_tinClocks.size())
    {
        m_tinClocks[slot].OnEnqueueIdleReset(now);
    }
    if (totalPkts == 1)
    {
        m_globalClock.OnEnqueueIdleReset(now);
    }
    if (slot < m_bytesEnqueued.size())
    {
        m_bytesEnqueued[slot] += item->GetSize();
    }
    // Belt-and-braces wake: the edge's own Run() follows the enqueue,
    // but if the gate is pending make sure a wake is armed.
    const Time gate = m_globalClock.EffectiveGate();
    if (!m_selfWakeEvent.IsPending() && gate > now)
    {
        m_selfWakeEvent = Simulator::Schedule(gate - now, &QueueDisc::Run, Ptr<QueueDisc>(edge));
    }
}

void
ShapedTinDispatcher::OnDequeue(uint32_t slot, Ptr<QueueDiscItem> item, EdgeQueueDisc* /*edge*/)
{
    NS_LOG_FUNCTION(this << slot);
    const Time now = Simulator::Now();
    const bool hasClock = slot < m_tinClocks.size();
    const uint32_t adjLen = RateBasedTinClock::ComputeAdjLen(
        item->GetSize(),
        hasClock ? m_tinClocks[slot].overhead : 0,
        hasClock ? m_tinClocks[slot].framing : RateBasedTinClock::FramingMode::NoAtm,
        hasClock ? m_tinClocks[slot].mpu : 0);
    if (hasClock)
    {
        m_tinClocks[slot].Charge(adjLen, now);
    }
    m_globalClock.Charge(adjLen, now, false);
    if (slot < m_bytesDequeued.size())
    {
        m_bytesDequeued[slot] += item->GetSize();
    }
}

cake::TinStats
ShapedTinDispatcher::GetTinStats(uint32_t tinIdx, const EdgeQueueDisc* edge) const
{
    TinStats out{};
    if (tinIdx >= EdgeQueueDisc::kMaxInnerSlots)
    {
        return out;
    }
    out.bytesEnqueued = m_bytesEnqueued[tinIdx];
    out.bytesDequeued = m_bytesDequeued[tinIdx];
    if (edge)
    {
        Ptr<QueueDisc> inner = edge->GetInnerDiscAt(tinIdx);
        if (inner)
        {
            const auto& s = inner->GetStats();
            out.drops = s.nTotalDroppedPackets;
            out.marks = s.nTotalMarkedPackets;
        }
    }
    return out;
}

void
ShapedTinDispatcher::DoDispose()
{
    m_selfWakeEvent.Cancel();
    SlotDispatcher::DoDispose();
}

} // namespace ns3::stratum::cake
