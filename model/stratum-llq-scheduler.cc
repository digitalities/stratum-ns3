/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsscheduler.cc class dsLLQ (2001).
 * Low Latency Queueing (Cisco): strict priority for queue 0,
 * configurable fair-queueing scheduler for queues 1..N-1.
 */

#include "stratum-llq-scheduler.h"

#include "stratum-scfq-scheduler.h"
#include "stratum-sfq-scheduler.h"
#include "stratum-wf2qp-scheduler.h"
#include "stratum-wfq-scheduler.h"

#include "ns3/assert.h"
#include "ns3/double.h"
#include "ns3/enum.h"
#include "ns3/log.h"
#include "ns3/object-factory.h"
#include "ns3/uinteger.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::LlqScheduler");

} // namespace ns3

namespace ns3::stratum
{

NS_OBJECT_ENSURE_REGISTERED(LlqScheduler);

TypeId
LlqScheduler::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::LlqScheduler")
                            .SetParent<Scheduler>()
                            .SetGroupName("Stratum")
                            .AddConstructor<LlqScheduler>()
                            .AddAttribute("FqVariant",
                                          "Inner fair-queueing variant served on queues "
                                          "1..N-1. Queue 0 is always priority. "
                                          "Construct-only because the inner-scheduler "
                                          "object is materialised in DoInitialize; changing "
                                          "this after Initialize has no effect on "
                                          "already-constructed sub-schedulers.",
                                          TypeId::ATTR_GET | TypeId::ATTR_CONSTRUCT,
                                          EnumValue(FqVariant::WFQ),
                                          MakeEnumAccessor<FqVariant>(&LlqScheduler::SetFqVariant,
                                                                      &LlqScheduler::GetFqVariant),
                                          MakeEnumChecker(FqVariant::WFQ,
                                                          "WFQ",
                                                          FqVariant::WF2Qp,
                                                          "WF2Qp",
                                                          FqVariant::SCFQ,
                                                          "SCFQ",
                                                          FqVariant::SFQ,
                                                          "SFQ"));
    return tid;
}

LlqScheduler::LlqScheduler() = default;
LlqScheduler::~LlqScheduler() = default;

void
LlqScheduler::SetFqVariant(FqVariant v)
{
    m_fqVariant = v;
}

LlqScheduler::FqVariant
LlqScheduler::GetFqVariant() const
{
    return m_fqVariant;
}

void
LlqScheduler::NotifyConstructionCompleted()
{
    NS_ASSERT_MSG(m_numQueues >= 2, "LLQ requires at least 2 queues");

    const double linkBw = GetLinkBandwidth();
    // Forward the wire-byte basis to the inner FQ sub-scheduler so a
    // single L2OverheadBytes set on the LLQ propagates to the PFQ that
    // computes finish times. PQ sub-scheduler does not consume it.
    const uint32_t l2 = GetL2OverheadBytes();

    // PQ sub-scheduler for queue 0 (1 queue, winLen=1).
    m_pq = CreateObjectWithAttributes<PriorityScheduler>("NumQueues",
                                                         UintegerValue(1),
                                                         "WinLen",
                                                         DoubleValue(1.0));

    // FQ sub-scheduler for queues 1..N-1.
    const uint32_t fqQueues = m_numQueues - 1;
    switch (m_fqVariant)
    {
    case FqVariant::WFQ:
        m_pfq = CreateObjectWithAttributes<WfqScheduler>("NumQueues",
                                                         UintegerValue(fqQueues),
                                                         "LinkBandwidth",
                                                         DoubleValue(linkBw),
                                                         "L2OverheadBytes",
                                                         UintegerValue(l2));
        break;
    case FqVariant::WF2Qp:
        m_pfq = CreateObjectWithAttributes<Wf2qPlusScheduler>("NumQueues",
                                                              UintegerValue(fqQueues),
                                                              "LinkBandwidth",
                                                              DoubleValue(linkBw),
                                                              "L2OverheadBytes",
                                                              UintegerValue(l2));
        break;
    case FqVariant::SCFQ:
        m_pfq = CreateObjectWithAttributes<ScfqScheduler>("NumQueues",
                                                          UintegerValue(fqQueues),
                                                          "LinkBandwidth",
                                                          DoubleValue(linkBw),
                                                          "L2OverheadBytes",
                                                          UintegerValue(l2));
        break;
    case FqVariant::SFQ:
        m_pfq = CreateObjectWithAttributes<SfqScheduler>("NumQueues",
                                                         UintegerValue(fqQueues),
                                                         "LinkBandwidth",
                                                         DoubleValue(linkBw),
                                                         "L2OverheadBytes",
                                                         UintegerValue(l2));
        break;
    }

    Scheduler::NotifyConstructionCompleted();
}

void
LlqScheduler::Reset()
{
    m_pq->Reset();
    m_pfq->Reset();
}

void
LlqScheduler::OnEnqueue(uint32_t queueIndex, uint32_t packetSizeBytes)
{
    // Delegate to OnEnqueueWithTime with time=0 for backward compat.
    // Callers should prefer OnEnqueueWithTime for FQ sub-schedulers.
    if (queueIndex == 0)
    {
        m_pq->OnEnqueue(0, packetSizeBytes);
    }
    else
    {
        m_pfq->OnEnqueue(queueIndex - 1, packetSizeBytes);
    }
}

void
LlqScheduler::OnEnqueueWithTime(uint32_t queueIndex, uint32_t packetSizeBytes, double nowSeconds)
{
    NS_LOG_FUNCTION(this << queueIndex << packetSizeBytes << nowSeconds);
    NS_ASSERT_MSG(queueIndex < m_numQueues, "Queue index out of range");

    if (queueIndex == 0)
    {
        m_pq->OnEnqueueWithTime(0, packetSizeBytes, nowSeconds);
    }
    else
    {
        m_pfq->OnEnqueueWithTime(queueIndex - 1, packetSizeBytes, nowSeconds);
    }
}

int
LlqScheduler::SelectNextQueue()
{
    NS_LOG_FUNCTION(this);

    // Priority queue is served strictly first
    int q = m_pq->SelectNextQueue();
    if (q >= 0)
    {
        return 0;
    }

    // Fall back to fair-queueing sub-scheduler
    q = m_pfq->SelectNextQueue();
    if (q >= 0)
    {
        return q + 1; // Offset back to original numbering
    }

    return -1;
}

void
LlqScheduler::SetParam(uint32_t queueIndex, double weight)
{
    NS_LOG_FUNCTION(this << queueIndex << weight);

    if (queueIndex == 0)
    {
        // PQ doesn't use weights; silently ignore (matches ns-2 AddParam).
        return;
    }
    m_pfq->SetParam(queueIndex - 1, weight);
}

void
LlqScheduler::SetPqRateCap(double rateBps)
{
    NS_LOG_FUNCTION(this << rateBps);
    m_pq->SetParam(0, rateBps);
}

} // namespace ns3::stratum
