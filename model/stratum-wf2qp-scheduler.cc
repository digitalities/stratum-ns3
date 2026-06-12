/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsscheduler.cc class dsWF2Qp (2001).
 * Worst-case Fair Weighted Fair Queueing+ (Bennett & Zhang, IEEE/ACM ToN 1997).
 */

#include "stratum-wf2qp-scheduler.h"

#include "ns3/assert.h"
#include "ns3/log.h"
#include "ns3/simulator.h"

#include <algorithm>
#include <limits>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::Wf2qPlusScheduler");

} // namespace ns3

namespace ns3::stratum
{

NS_OBJECT_ENSURE_REGISTERED(Wf2qPlusScheduler);

TypeId
Wf2qPlusScheduler::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::Wf2qPlusScheduler")
                            .SetParent<Scheduler>()
                            .SetGroupName("Stratum")
                            .AddConstructor<Wf2qPlusScheduler>();
    return tid;
}

Wf2qPlusScheduler::Wf2qPlusScheduler()
{
    for (uint32_t i = 0; i < kMaxQueues; ++i)
    {
        m_flow[i].weight = 1.0;
    }
    Reset();
}

Wf2qPlusScheduler::~Wf2qPlusScheduler() = default;

void
Wf2qPlusScheduler::Reset()
{
    for (uint32_t i = 0; i < kMaxQueues; ++i)
    {
        m_flow[i].qcrtSize = 0;
        m_flow[i].S = 0.0;
        m_flow[i].F = 0.0;
        // Clear the flow queue (swap with empty queue)
        std::queue<uint32_t>().swap(m_flow[i].flowQueue);
    }
    m_V = 0.0;
    m_lastTimeV = 0.0;
}

void
Wf2qPlusScheduler::OnEnqueue(uint32_t queueIndex, uint32_t packetSizeBytes)
{
    // Called by OnEnqueueWithTime for base-class tracking.
    // WF2Q+ does not use this path directly; the virtual-time update
    // is in OnEnqueueWithTime. This satisfies the pure-virtual requirement.
    NS_LOG_FUNCTION(this << queueIndex << packetSizeBytes);
}

void
Wf2qPlusScheduler::OnEnqueueWithTime(uint32_t queueIndex,
                                     uint32_t packetSizeBytes,
                                     double /*nowSeconds*/)
{
    NS_LOG_FUNCTION(this << queueIndex << packetSizeBytes);
    NS_ASSERT_MSG(queueIndex < m_numQueues, "Queue index out of range");

    // Base-class tracking (no-op for WF2Q+ but keeps the interface contract)
    OnEnqueue(queueIndex, packetSizeBytes);

    double bwBytes = GetLinkBandwidth() / 8.0;
    NS_ASSERT_MSG(bwBytes > 0.0, "Link bandwidth must be set before enqueue");
    NS_ASSERT_MSG(m_flow[queueIndex].weight > 0.0, "Weight must be positive");

    // Wire-byte basis: finish-time math charges IP bytes plus the L2
    // framing the netdev will add downstream.
    const uint32_t wireBytes = packetSizeBytes + GetL2OverheadBytes();

    if (m_flow[queueIndex].qcrtSize == 0)
    {
        // Flow was empty -- compute S and F from formulas (23b) and (24)
        m_flow[queueIndex].S = std::max(m_V, m_flow[queueIndex].F);
        m_flow[queueIndex].F = m_flow[queueIndex].S + static_cast<double>(wireBytes) /
                                                          (m_flow[queueIndex].weight * bwBytes);

        // Update V per formula (22): V = max(min_S_of_active_flows, V)
        double minS = m_flow[queueIndex].S;
        for (uint32_t j = 0; j < m_numQueues; ++j)
        {
            if (m_flow[j].qcrtSize > 0 && m_flow[j].S < minS)
            {
                minS = m_flow[j].S;
            }
        }
        m_V = std::max(minS, m_V);
    }

    m_flow[queueIndex].flowQueue.push(packetSizeBytes);
    m_flow[queueIndex].qcrtSize += packetSizeBytes;

    NS_LOG_DEBUG("WF2Q+ enqueue q=" << queueIndex << " S=" << m_flow[queueIndex].S
                                    << " F=" << m_flow[queueIndex].F << " V=" << m_V);
}

int
Wf2qPlusScheduler::SelectNextQueue()
{
    NS_LOG_FUNCTION(this);

    double now = Simulator::Now().GetSeconds();
    double minF = std::numeric_limits<double>::max();
    int flowId = -1;
    double W = 0.0; // sum of active weights

    // Find eligible flow with minimum F
    for (uint32_t i = 0; i < m_numQueues; ++i)
    {
        if (m_flow[i].qcrtSize > 0)
        {
            W += m_flow[i].weight;
            if (m_flow[i].S <= m_V && m_flow[i].F < minF)
            {
                flowId = static_cast<int>(i);
                minF = m_flow[i].F;
            }
        }
    }

    if (flowId >= 0)
    {
        uint32_t pktSize = m_flow[flowId].flowQueue.front();
        m_flow[flowId].qcrtSize -= pktSize;
        m_flow[flowId].flowQueue.pop();

        double bwBytes = GetLinkBandwidth() / 8.0;

        // Update S and F for remaining packets in this flow.
        // Wire-byte basis.
        if (!m_flow[flowId].flowQueue.empty())
        {
            const uint32_t nextWireBytes = m_flow[flowId].flowQueue.front() + GetL2OverheadBytes();
            m_flow[flowId].S = m_flow[flowId].F;
            m_flow[flowId].F = m_flow[flowId].S + static_cast<double>(nextWireBytes) /
                                                      (m_flow[flowId].weight * bwBytes);
        }

        // Update V: V = max(min_S_of_active, V + (now - lastTimeV) / W)
        double minS = std::numeric_limits<double>::max();
        for (uint32_t i = 0; i < m_numQueues; ++i)
        {
            if (m_flow[i].qcrtSize > 0 && m_flow[i].S < minS)
            {
                minS = m_flow[i].S;
            }
        }
        if (minS == std::numeric_limits<double>::max())
        {
            minS = 0.0;
        }

        double elapsed = (now - m_lastTimeV) / W;
        m_V = std::max(minS, m_V + elapsed);
        m_lastTimeV = now;

        NS_LOG_DEBUG("WF2Q+ dequeue q=" << flowId << " V=" << m_V << " now=" << now);
    }

    return flowId;
}

void
Wf2qPlusScheduler::SetParam(uint32_t queueIndex, double weight)
{
    NS_ASSERT_MSG(queueIndex < m_numQueues, "Queue index out of range");
    NS_ASSERT_MSG(weight > 0.0, "Weight must be positive");
    m_flow[queueIndex].weight = weight;
}

} // namespace ns3::stratum
