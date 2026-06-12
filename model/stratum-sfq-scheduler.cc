/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsscheduler.cc class dsSFQ (2001).
 * Start-time Fair Queueing (Goyal, Vin, Cheng, 1997).
 */

#include "stratum-sfq-scheduler.h"

#include "ns3/assert.h"
#include "ns3/log.h"

#include <algorithm>
#include <limits>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::SfqScheduler");

} // namespace ns3

namespace ns3::stratum
{

NS_OBJECT_ENSURE_REGISTERED(SfqScheduler);

TypeId
SfqScheduler::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::SfqScheduler")
                            .SetParent<Scheduler>()
                            .SetGroupName("Stratum")
                            .AddConstructor<SfqScheduler>();
    return tid;
}

SfqScheduler::SfqScheduler()
{
    for (uint32_t i = 0; i < kMaxQueues; ++i)
    {
        m_flow[i].weight = 1.0;
    }
    Reset();
}

SfqScheduler::~SfqScheduler() = default;

void
SfqScheduler::Reset()
{
    for (uint32_t i = 0; i < kMaxQueues; ++i)
    {
        m_flow[i].lastFinishTag = 0.0;
        // Clear the flow queue (swap with empty queue)
        std::queue<PacketTags>().swap(m_flow[i].flowQueue);
    }
    m_V = 0.0;
}

void
SfqScheduler::OnEnqueue(uint32_t queueIndex, uint32_t packetSizeBytes)
{
    // Called by OnEnqueueWithTime for base-class tracking.
    // SFQ does not use this path directly; the virtual-time update
    // is in OnEnqueueWithTime. This satisfies the pure-virtual requirement.
    NS_LOG_FUNCTION(this << queueIndex << packetSizeBytes);
}

void
SfqScheduler::OnEnqueueWithTime(uint32_t queueIndex,
                                uint32_t packetSizeBytes,
                                double /*nowSeconds*/)
{
    NS_LOG_FUNCTION(this << queueIndex << packetSizeBytes);
    NS_ASSERT_MSG(queueIndex < m_numQueues, "Queue index out of range");

    // Base-class tracking (no-op for SFQ but keeps the interface contract)
    OnEnqueue(queueIndex, packetSizeBytes);

    double bwBytes = GetLinkBandwidth() / 8.0;
    NS_ASSERT_MSG(bwBytes > 0.0, "Link bandwidth must be set before enqueue");
    NS_ASSERT_MSG(m_flow[queueIndex].weight > 0.0, "Weight must be positive");

    // Wire-byte basis.
    const uint32_t wireBytes = packetSizeBytes + GetL2OverheadBytes();

    double startTag = std::max(m_V, m_flow[queueIndex].lastFinishTag);
    double finishTag =
        startTag + static_cast<double>(wireBytes) / (m_flow[queueIndex].weight * bwBytes);

    m_flow[queueIndex].lastFinishTag = finishTag;
    m_flow[queueIndex].flowQueue.push({startTag, finishTag});
    m_idle = false;

    NS_LOG_DEBUG("SFQ enqueue q=" << queueIndex << " startTag=" << startTag
                                  << " finishTag=" << finishTag);
}

int
SfqScheduler::SelectNextQueue()
{
    NS_LOG_FUNCTION(this);

    int qToDq = -1;
    double minStartTag = std::numeric_limits<double>::max();

    for (uint32_t i = 0; i < m_numQueues; ++i)
    {
        // Fix ns-2 bug: check empty() BEFORE accessing front()
        if (!m_flow[i].flowQueue.empty() && m_flow[i].flowQueue.front().startTag < minStartTag)
        {
            qToDq = static_cast<int>(i);
            minStartTag = m_flow[i].flowQueue.front().startTag;
        }
    }

    if (qToDq != -1)
    {
        m_V = minStartTag;
        m_maxFinishTag = std::max(m_maxFinishTag, m_flow[qToDq].flowQueue.front().finishTag);
        m_flow[qToDq].flowQueue.pop();

        NS_LOG_DEBUG("SFQ dequeue q=" << qToDq << " V=" << m_V);

        // Check if all queues are now empty
        bool allEmpty = true;
        for (uint32_t i = 0; i < m_numQueues; ++i)
        {
            if (!m_flow[i].flowQueue.empty())
            {
                allEmpty = false;
                break;
            }
        }
        if (allEmpty)
        {
            m_maxFinishTag = 0.0;
            Reset();
            m_idle = true;
        }
    }
    else if (!m_idle)
    {
        m_maxFinishTag = 0.0;
        Reset();
        m_idle = true;
    }

    return qToDq;
}

void
SfqScheduler::SetParam(uint32_t queueIndex, double weight)
{
    NS_ASSERT_MSG(queueIndex < m_numQueues, "Queue index out of range");
    NS_ASSERT_MSG(weight > 0.0, "Weight must be positive");
    m_flow[queueIndex].weight = weight;
}

} // namespace ns3::stratum
