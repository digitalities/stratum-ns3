/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "stratum-rate-based-shaper-dispatcher.h"

#include "stratum-cake-linux-autorate-hook.h"
#include "stratum-ds-field.h"

#include "ns3/drop-tail-queue.h"
#include "ns3/ipv4-header.h"
#include "ns3/ipv4-queue-disc-item.h"
#include "ns3/log.h"
#include "ns3/object-factory.h"
#include "ns3/simulator.h"

#include <algorithm>

namespace ns3::stratum::cake
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::cake::RateBasedShaperDispatcher");
NS_OBJECT_ENSURE_REGISTERED(RateBasedShaperDispatcher);

TypeId
RateBasedShaperDispatcher::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::stratum::cake::RateBasedShaperDispatcher")
            .SetParent<QueueDisc>()
            .SetGroupName("Stratum")
            .AddConstructor<RateBasedShaperDispatcher>()
            .AddAttribute("MaxSize",
                          "The maximum number of packets accepted by this queue disc.",
                          QueueSizeValue(QueueSize("1000p")),
                          MakeQueueSizeAccessor(&QueueDisc::SetMaxSize, &QueueDisc::GetMaxSize),
                          MakeQueueSizeChecker());
    return tid;
}

RateBasedShaperDispatcher::RateBasedShaperDispatcher()
    : QueueDisc(QueueDiscSizePolicy::MULTIPLE_QUEUES, QueueSizeUnit::PACKETS)
{
    NS_LOG_FUNCTION(this);
}

RateBasedShaperDispatcher::~RateBasedShaperDispatcher()
{
    NS_LOG_FUNCTION(this);
}

void
RateBasedShaperDispatcher::ConfigureTin(uint32_t slot,
                                        uint64_t rateBps,
                                        int32_t overhead,
                                        uint32_t mpu,
                                        RateBasedTinClock::FramingMode framing)
{
    NS_LOG_FUNCTION(this << slot << rateBps);
    if (m_tinClocks.size() <= slot)
    {
        m_tinClocks.resize(slot + 1);
    }
    m_tinClocks[slot].rateBps = rateBps;
    m_tinClocks[slot].overhead = overhead;
    m_tinClocks[slot].mpu = mpu;
    m_tinClocks[slot].framing = framing;
}

void
RateBasedShaperDispatcher::ConfigureGlobal(uint64_t rateBps)
{
    NS_LOG_FUNCTION(this << rateBps);
    m_globalClock.rateBps = rateBps;
}

void
RateBasedShaperDispatcher::SetEnableLlq(bool enabled)
{
    NS_LOG_FUNCTION(this << enabled);
    m_enableLlq = enabled;
}

void
RateBasedShaperDispatcher::SetTinPriorityOrder(std::vector<uint32_t> order)
{
    NS_LOG_FUNCTION(this);
    m_priorityOrder = std::move(order);
}

void
RateBasedShaperDispatcher::SetDscpToSlot(uint8_t dscp, uint32_t slot)
{
    NS_LOG_FUNCTION(this << static_cast<uint32_t>(dscp) << slot);
    NS_ASSERT_MSG(dscp < 64, "DSCP codepoint out of 6-bit range");
    m_dscpToSlot[dscp] = static_cast<uint8_t>(slot);
}

uint32_t
RateBasedShaperDispatcher::GetDscpToSlot(uint8_t dscp) const
{
    NS_ASSERT_MSG(dscp < 64, "DSCP codepoint out of 6-bit range");
    return m_dscpToSlot[dscp];
}

void
RateBasedShaperDispatcher::SetIngressMode(bool enabled)
{
    NS_LOG_FUNCTION(this << enabled);
    m_ingressMode = enabled;
}

bool
RateBasedShaperDispatcher::GetIngressMode() const
{
    return m_ingressMode;
}

uint64_t
RateBasedShaperDispatcher::GetTinBytesCharged(uint32_t slot) const
{
    if (slot >= m_bytesCharged.size())
    {
        return 0;
    }
    return m_bytesCharged[slot];
}

uint64_t
RateBasedShaperDispatcher::GetGlobalRateBps() const
{
    return m_globalClock.rateBps;
}

void
RateBasedShaperDispatcher::SetAutorateHook(std::shared_ptr<LinuxAutorateHook> hook)
{
    NS_LOG_FUNCTION(this);
    m_autorateHook = std::move(hook);
}

int32_t
RateBasedShaperDispatcher::ClassifyByDscp(Ptr<QueueDiscItem> item) const
{
    uint8_t dscp;
    if (!stratum::GetDscp(item, dscp))
    {
        return 0; // non-IP item: dispatch to tin 0
    }
    if (dscp >= 64)
    {
        return 0;
    }
    return static_cast<int32_t>(m_dscpToSlot[dscp]);
}

bool
RateBasedShaperDispatcher::DoEnqueue(Ptr<QueueDiscItem> item)
{
    NS_LOG_FUNCTION(this << item);

    // Classify the item to a tin slot. When this dispatcher is installed
    // standalone (Helper::BuildAndInstall), classification reads
    // the IPv4 DSCP and indexes a 64-entry slot table populated by the
    // helper for the configured tc-cake profile.
    int32_t slot = ClassifyByDscp(item);
    if (slot < 0 || static_cast<uint32_t>(slot) >= GetNInternalQueues())
    {
        DropBeforeEnqueue(item, "no-classifier-match");
        return false;
    }

    bool tinWasEmpty = (GetInternalQueue(slot)->GetNPackets() == 0);
    bool globalWasEmpty = (GetNPackets() == 0);
    Time now = Simulator::Now();

    bool ok = GetInternalQueue(slot)->Enqueue(item);
    if (!ok)
    {
        // Inner queue overflow. In ingress mode, advance the per-tin and
        // global clocks for the dropped packet, matching Linux
        // sch_cake.c::cake_enqueue calling cake_advance_shaper(..., true)
        // when CAKE_FLAG_INGRESS is set.
        if (m_ingressMode && static_cast<uint32_t>(slot) < m_tinClocks.size())
        {
            const uint32_t adjLen = RateBasedTinClock::ComputeAdjLen(item->GetSize(),
                                                                     m_tinClocks[slot].overhead,
                                                                     m_tinClocks[slot].framing,
                                                                     m_tinClocks[slot].mpu);
            m_tinClocks[slot].Charge(adjLen, now);
            m_globalClock.Charge(adjLen, now, true);
            if (static_cast<uint32_t>(slot) >= m_bytesCharged.size())
            {
                m_bytesCharged.resize(static_cast<uint32_t>(slot) + 1, 0);
            }
            m_bytesCharged[slot] += adjLen;
        }
        return false;
    }

    if (tinWasEmpty && static_cast<uint32_t>(slot) < m_tinClocks.size())
    {
        m_tinClocks[slot].OnEnqueueIdleReset(now);
    }
    if (globalWasEmpty)
    {
        m_globalClock.OnEnqueueIdleReset(now);
    }

    // Notify the Linux-faithful autorate hook so it can maintain its
    // EWMA state across the arrival stream. The DynamicCast is only
    // needed when the caller passes a base-class pointer; SetAutorateHook
    // already stores a typed pointer, so this is a direct call.
    if (m_autorateHook != nullptr)
    {
        // The autorate capacity estimate accumulates the RAW wire length:
        // sch_cake.c:1871 folds qdisc_pkt_len into avg_window_bytes, keeping the
        // overhead/MPU/framing adjustment confined to the shaper clocks above.
        m_autorateHook->OnEnqueue(item->GetSize(), now);

        // Close the autorate loop: apply the inferred rate to the aggregate
        // clock and rescale the per-tin clocks proportionally so their relative
        // demotion order is preserved (the ranking is scale-invariant).
        // ComputeRateDelta self-throttles via the 250 ms reconfigure deadband,
        // matching sch_cake.c gating cake_reconfigure on q->last_reconfig_time.
        // Only a shaped aggregate (non-zero rate) is adjusted.
        const int64_t rateDelta = m_autorateHook->ComputeRateDelta(m_globalClock.rateBps);
        if (rateDelta != 0 && m_globalClock.rateBps != 0)
        {
            const uint64_t oldRate = m_globalClock.rateBps;
            const int64_t newSigned = static_cast<int64_t>(oldRate) + rateDelta;
            if (newSigned > 0)
            {
                const uint64_t newRate = static_cast<uint64_t>(newSigned);
                for (auto& tin : m_tinClocks)
                {
                    if (tin.rateBps != 0)
                    {
                        tin.rateBps = static_cast<uint64_t>(
                            (static_cast<unsigned __int128>(tin.rateBps) * newRate) / oldRate);
                    }
                }
                m_globalClock.rateBps = newRate;
            }
        }
    }

    MaybeArmSelfWake();
    return true;
}

Ptr<QueueDiscItem>
RateBasedShaperDispatcher::ServeSlot(uint32_t slot, Time now)
{
    Ptr<QueueDiscItem> item = GetInternalQueue(slot)->Dequeue();
    if (item == nullptr)
    {
        return nullptr;
    }
    uint32_t adjLen = RateBasedTinClock::ComputeAdjLen(
        item->GetSize(),
        slot < m_tinClocks.size() ? m_tinClocks[slot].overhead : 0,
        slot < m_tinClocks.size() ? m_tinClocks[slot].framing
                                  : RateBasedTinClock::FramingMode::NoAtm,
        slot < m_tinClocks.size() ? m_tinClocks[slot].mpu : 0);
    if (slot < m_tinClocks.size())
    {
        m_tinClocks[slot].Charge(adjLen, now);
        if (slot >= m_bytesCharged.size())
        {
            m_bytesCharged.resize(slot + 1, 0);
        }
        m_bytesCharged[slot] += adjLen;
    }
    m_globalClock.Charge(adjLen, now, false);
    return item;
}

Ptr<QueueDiscItem>
RateBasedShaperDispatcher::DoDequeue()
{
    NS_LOG_FUNCTION(this);
    Time now = Simulator::Now();

    bool anyBacklog = false;
    for (uint32_t slot = 0; slot < GetNInternalQueues(); ++slot)
    {
        if (GetInternalQueue(slot)->GetNPackets() > 0)
        {
            anyBacklog = true;
            break;
        }
    }
    if (!anyBacklog)
    {
        return nullptr;
    }

    // Global hard shaper — the only gate. Blocked only while BOTH the
    // primary and the failsafe clock are in the future; self-wake at
    // their minimum.
    Time gate = m_globalClock.EffectiveGate();
    if (gate > now)
    {
        m_selfWakeEvent.Cancel();
        m_selfWakeEvent =
            Simulator::Schedule(gate - now, &RateBasedShaperDispatcher::OnSelfWake, this);
        return nullptr;
    }

    if (m_enableLlq && GetNInternalQueues() > 0 && GetInternalQueue(0)->GetNPackets() > 0)
    {
        return ServeSlot(0, now);
    }

    // Choose a tin: scan backlogged tins in ascending priority order;
    // any schedule-meeting tin steals the choice (so the LAST meeting
    // tin = highest priority wins); when none meets its schedule, the
    // earliest-scheduled tin wins with ties to higher priority. A tin
    // behind its schedule is still served — its clock demotes, it does
    // not block.
    int32_t bestSlot = -1;
    Time bestTime = Time::Max();
    const uint32_t n = GetNInternalQueues();
    for (uint32_t k = 0; k < n; ++k)
    {
        const uint32_t slot = (k < m_priorityOrder.size()) ? m_priorityOrder[k] : k;
        if (slot >= n || GetInternalQueue(slot)->GetNPackets() == 0)
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
    if (bestSlot < 0)
    {
        return nullptr;
    }
    return ServeSlot(static_cast<uint32_t>(bestSlot), now);
}

Ptr<const QueueDiscItem>
RateBasedShaperDispatcher::DoPeek()
{
    if (GetNInternalQueues() == 0)
    {
        return nullptr;
    }
    return GetInternalQueue(0)->Peek();
}

bool
RateBasedShaperDispatcher::CheckConfig()
{
    NS_LOG_FUNCTION(this);
    if (GetNInternalQueues() == 0)
    {
        // Mirror pfifo-fast-queue-disc.cc: install one DropTailQueue per
        // configured tin (or one queue when no tin clocks are defined,
        // for the besteffort layout).
        const uint32_t numTins = std::max<uint32_t>(static_cast<uint32_t>(m_tinClocks.size()), 1u);
        ObjectFactory factory;
        factory.SetTypeId("ns3::DropTailQueue<QueueDiscItem>");
        factory.Set("MaxSize", QueueSizeValue(GetMaxSize()));
        for (uint32_t i = 0; i < numTins; ++i)
        {
            AddInternalQueue(factory.Create<InternalQueue>());
        }
    }
    return true;
}

void
RateBasedShaperDispatcher::InitializeParams()
{
    // Seed the autorate estimate from the configured aggregate rate, mirroring
    // cake_init setting avg_peak_bandwidth = rate_bps (sch_cake.c:2893). Runs
    // after ConfigureGlobal and SetAutorateHook, so the global clock carries
    // the bootstrap rate the estimate starts from.
    if (m_autorateHook != nullptr)
    {
        m_autorateHook->SeedRate(m_globalClock.rateBps);
    }
}

void
RateBasedShaperDispatcher::OnSelfWake()
{
    NS_LOG_FUNCTION(this);
    Run();
}

void
RateBasedShaperDispatcher::MaybeArmSelfWake()
{
    if (m_selfWakeEvent.IsPending())
    {
        return;
    }
    Time now = Simulator::Now();
    bool anyBacklog = false;
    for (uint32_t slot = 0; slot < GetNInternalQueues(); ++slot)
    {
        if (GetInternalQueue(slot)->GetNPackets() > 0)
        {
            anyBacklog = true;
            break;
        }
    }
    if (!anyBacklog)
    {
        return;
    }
    Time gate = m_globalClock.EffectiveGate();
    if (gate > now)
    {
        m_selfWakeEvent =
            Simulator::Schedule(gate - now, &RateBasedShaperDispatcher::OnSelfWake, this);
    }
}

} // namespace ns3::stratum::cake
