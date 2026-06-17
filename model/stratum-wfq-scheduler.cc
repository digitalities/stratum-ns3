/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "stratum-wfq-scheduler.h"

#include "ns3/assert.h"
#include "ns3/log.h"
#include "ns3/simulator.h"

#include <algorithm>
#include <limits>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::WfqScheduler");

} // namespace ns3

namespace ns3::stratum
{

NS_OBJECT_ENSURE_REGISTERED(WfqScheduler);

TypeId
WfqScheduler::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::WfqScheduler")
                            .SetParent<Scheduler>()
                            .SetGroupName("Stratum")
                            .AddConstructor<WfqScheduler>();
    return tid;
}

WfqScheduler::WfqScheduler() = default;

WfqScheduler::~WfqScheduler() = default;

void
WfqScheduler::DoDispose()
{
    Scheduler::DoDispose();
}

void
WfqScheduler::Reset()
{
    for (uint32_t i = 0; i < kMaxQueues; ++i)
    {
        std::queue<double> empty;
        std::swap(m_fs[i].finishTags, empty);
        m_fs[i].finishT = 0.0;
    }
    m_sumPhiBusy = 0.0;
    m_busyEpochStartT = 0.0;
    m_busyEpochStartV = 0.0;
}

double
WfqScheduler::ComputeVirtualTime(double nowSeconds) const
{
    if (m_sumPhiBusy <= 0.0)
    {
        return m_busyEpochStartV;
    }
    return m_busyEpochStartV + (nowSeconds - m_busyEpochStartT) / m_sumPhiBusy;
}

void
WfqScheduler::SnapshotBusyEpoch(double nowSeconds)
{
    m_busyEpochStartV = ComputeVirtualTime(nowSeconds);
    m_busyEpochStartT = nowSeconds;
}

void
WfqScheduler::OnEnqueue(uint32_t queueIndex, uint32_t packetSizeBytes)
{
    NS_LOG_FUNCTION(this << queueIndex << packetSizeBytes);
}

void
WfqScheduler::OnEnqueueWithTime(uint32_t queueIndex, uint32_t packetSizeBytes, double nowSeconds)
{
    NS_LOG_FUNCTION(this << queueIndex << packetSizeBytes << nowSeconds);
    NS_ASSERT_MSG(queueIndex < m_numQueues, "Queue index out of range");

    OnEnqueue(queueIndex, packetSizeBytes);

    const double bw = GetLinkBandwidth();
    NS_ASSERT_MSG(bw > 0.0, "Link bandwidth must be set before enqueue");
    NS_ASSERT_MSG(m_fs[queueIndex].weight > 0.0, "Weight must be positive");

    // Busy-set transition: this session was empty before this arrival
    // and is becoming backlogged. Snapshot V(t) under the OLD
    // sumPhiBusy, then add this session's weight.
    const bool wasBusy = !m_fs[queueIndex].finishTags.empty();
    if (!wasBusy)
    {
        SnapshotBusyEpoch(nowSeconds);
        m_sumPhiBusy += m_fs[queueIndex].weight;
    }

    const double vNow = ComputeVirtualTime(nowSeconds);

    // Parekh-Gallager 1993 Eq. (3): finish tag of the k-th arrival of flow i.
    // Wire-byte basis: charge IP bytes plus the L2 framing the netdev
    // will add downstream.
    const uint32_t wireBytes = packetSizeBytes + GetL2OverheadBytes();
    m_fs[queueIndex].finishT =
        std::max(m_fs[queueIndex].finishT, vNow) +
        static_cast<double>(wireBytes) * 8.0 / (m_fs[queueIndex].weight * bw);
    m_fs[queueIndex].finishTags.push(m_fs[queueIndex].finishT);

    NS_LOG_DEBUG("WFQ enqueue q=" << queueIndex << " finishT=" << m_fs[queueIndex].finishT
                                  << " V=" << vNow << " sumPhi=" << m_sumPhiBusy);
}

int
WfqScheduler::SelectNextQueue()
{
    NS_LOG_FUNCTION(this);

    int qStar = -1;
    double minTag = std::numeric_limits<double>::infinity();

    for (uint32_t i = 0; i < m_numQueues; ++i)
    {
        if (!m_fs[i].finishTags.empty() && m_fs[i].finishTags.front() < minTag)
        {
            qStar = static_cast<int>(i);
            minTag = m_fs[i].finishTags.front();
        }
    }

    if (qStar >= 0)
    {
        m_fs[qStar].finishTags.pop();
        // Busy-set transition: if this dequeue empties the session
        // queue, snapshot V(t) under the OLD sumPhiBusy and then drop
        // this session's weight from it.
        if (m_fs[qStar].finishTags.empty())
        {
            const double now = Simulator::Now().GetSeconds();
            SnapshotBusyEpoch(now);
            m_sumPhiBusy -= m_fs[qStar].weight;
            // When the busy set empties, the backlogged-weight sum is exactly
            // zero by definition. Pin it to zero: the running +=/-= sequence
            // accumulates a tiny IEEE-754 residue with non-dyadic weights, and
            // a positive residue would otherwise survive the `< 0` clamp and
            // the `<= 0` freeze in ComputeVirtualTime, driving V(t) to ~1e16 on
            // the next post-idle arrival and collapsing service to lowest index.
            bool anyBacklogged = false;
            for (uint32_t i = 0; i < m_numQueues; ++i)
            {
                if (!m_fs[i].finishTags.empty())
                {
                    anyBacklogged = true;
                    break;
                }
            }
            if (!anyBacklogged || m_sumPhiBusy < 0.0)
            {
                m_sumPhiBusy = 0.0;
            }
        }
        NS_LOG_DEBUG("WFQ dequeue q=" << qStar << " finishTag=" << minTag);
    }

    return qStar;
}

void
WfqScheduler::SetParam(uint32_t queueIndex, double weight)
{
    NS_ASSERT_MSG(queueIndex < m_numQueues, "Queue index out of range");
    NS_ASSERT_MSG(weight > 0.0, "Weight must be positive");
    m_fs[queueIndex].weight = weight;
}

double
WfqScheduler::GetVirtualTime() const
{
    return ComputeVirtualTime(Simulator::Now().GetSeconds());
}

} // namespace ns3::stratum
