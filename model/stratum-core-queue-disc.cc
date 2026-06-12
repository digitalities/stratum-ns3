/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * Copyright (C) 2000 Nortel Networks
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsCore.{h,cc} class coreQueue (2001).
 */

#include "stratum-core-queue-disc.h"

#include "stratum-l4s-queue-disc.h"
#include "stratum-queue-stats-provider.h"
#include "stratum-red-sub-queue.h"

#include "ns3/log.h"
#include "ns3/queue-disc.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::CoreQueueDisc");

} // namespace ns3

namespace ns3::stratum
{

NS_OBJECT_ENSURE_REGISTERED(CoreQueueDisc);

TypeId
CoreQueueDisc::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::CoreQueueDisc")
                            .SetParent<QueueDisc>()
                            .SetGroupName("Stratum")
                            .AddConstructor<CoreQueueDisc>();
    return tid;
}

CoreQueueDisc::CoreQueueDisc()
    : QueueDisc(QueueDiscSizePolicy::NO_LIMITS)
{
    NS_LOG_FUNCTION(this);
}

CoreQueueDisc::~CoreQueueDisc()
{
    NS_LOG_FUNCTION(this);
}

void
CoreQueueDisc::SetInnerDisc(Ptr<QueueDisc> inner)
{
    NS_LOG_FUNCTION(this << inner);
    NS_ASSERT_MSG(GetNQueueDiscClasses() == 0, "SetInnerDisc must be called before Initialize");
    m_inner = inner;
}

Ptr<QueueDisc>
CoreQueueDisc::GetInnerDisc() const
{
    return m_inner;
}

void
CoreQueueDisc::EnsureDefaultInner()
{
    if (!m_inner)
    {
        m_inner = CreateObject<RedQueueDisc>();
    }
    if (GetNQueueDiscClasses() == 0)
    {
        Ptr<QueueDiscClass> cls = CreateObject<QueueDiscClass>();
        cls->SetQueueDisc(m_inner);
        AddQueueDiscClass(cls);
    }
}

// --- Inner configuration ---
//
// Callers configure the inner via its own API before `SetInnerDisc`.

// --- Runtime probes via the QueueStatsProvider interface ---

Ptr<Scheduler>
CoreQueueDisc::GetScheduler() const
{
    if (!m_inner)
    {
        return nullptr;
    }
    Ptr<RedQueueDisc> red = DynamicCast<RedQueueDisc>(m_inner);
    return red ? red->GetScheduler() : nullptr;
}

uint32_t
CoreQueueDisc::GetNumQueues() const
{
    auto* stats = dynamic_cast<QueueStatsProvider*>(PeekPointer(m_inner));
    return stats ? stats->GetNumQueues() : 0;
}

int
CoreQueueDisc::GetVirtualQueueLen(uint32_t queue, uint32_t prec) const
{
    auto* stats = dynamic_cast<QueueStatsProvider*>(PeekPointer(m_inner));
    return stats ? stats->GetQueueLen(queue, prec) : 0;
}

void
CoreQueueDisc::PrintStats() const
{
    if (!m_inner)
    {
        return;
    }
    Ptr<RedQueueDisc> red = DynamicCast<RedQueueDisc>(m_inner);
    if (red)
    {
        red->PrintStats();
    }
}

void
CoreQueueDisc::PrintPhbTable() const
{
    if (!m_inner)
    {
        return;
    }
    Ptr<RedQueueDisc> red = DynamicCast<RedQueueDisc>(m_inner);
    if (red)
    {
        red->PrintPhbTable();
    }
}

int64_t
CoreQueueDisc::AssignStreams(int64_t stream)
{
    NS_LOG_FUNCTION(this << stream);
    int64_t consumed = 0;
    if (m_inner)
    {
        // Branch on inner type; see edge's AssignStreams for the
        // rationale. The core cascade is the same shape minus the
        // meter-slot cascade (core has no meters).
        Ptr<RedQueueDisc> red = DynamicCast<RedQueueDisc>(m_inner);
        Ptr<l4s::QueueDisc> l4s = DynamicCast<l4s::QueueDisc>(m_inner);
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
    return consumed;
}

// --- QueueDisc overrides ---

bool
CoreQueueDisc::DoEnqueue(Ptr<QueueDiscItem> item)
{
    NS_LOG_FUNCTION(this << item);
    // Pure delegation: core is BA-only, no classification or metering.
    // Inner reads the IPv4 header DSCP (no DscpTag in play) via
    // its own DoEnqueue path. Drop aggregation cascades via the
    // AddQueueDiscClass automatic drop-functor hookup.
    return m_inner->Enqueue(item);
}

Ptr<QueueDiscItem>
CoreQueueDisc::DoDequeue()
{
    NS_LOG_FUNCTION(this);
    return m_inner ? m_inner->Dequeue() : nullptr;
}

Ptr<const QueueDiscItem>
CoreQueueDisc::DoPeek()
{
    NS_LOG_FUNCTION(this);
    return m_inner ? m_inner->Peek() : nullptr;
}

bool
CoreQueueDisc::CheckConfig()
{
    NS_LOG_FUNCTION(this);

    if (GetNInternalQueues() > 0)
    {
        NS_LOG_ERROR("CoreQueueDisc must not have internal queues");
        return false;
    }

    EnsureDefaultInner();

    NS_ASSERT_MSG(GetNQueueDiscClasses() == 1,
                  "CoreQueueDisc requires exactly one QueueDiscClass "
                  "child (the inner disc at idx 0); got "
                      << GetNQueueDiscClasses());
    return GetNQueueDiscClasses() == 1;
}

void
CoreQueueDisc::InitializeParams()
{
    NS_LOG_FUNCTION(this);
}

void
CoreQueueDisc::DoDispose()
{
    NS_LOG_FUNCTION(this);
    m_inner = nullptr;
    QueueDisc::DoDispose();
}

} // namespace ns3::stratum
