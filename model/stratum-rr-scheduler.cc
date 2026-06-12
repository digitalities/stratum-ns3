/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsscheduler.cc class dsRR (2001).
 */

#include "stratum-rr-scheduler.h"

#include "ns3/log.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::RoundRobinScheduler");

} // namespace ns3

namespace ns3::stratum
{

NS_OBJECT_ENSURE_REGISTERED(RoundRobinScheduler);

TypeId
RoundRobinScheduler::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::RoundRobinScheduler")
                            .SetParent<Scheduler>()
                            .SetGroupName("Stratum")
                            .AddConstructor<RoundRobinScheduler>();
    return tid;
}

RoundRobinScheduler::RoundRobinScheduler()
{
    for (uint32_t i = 0; i < kMaxQueues; ++i)
    {
        m_queueLen[i] = 0;
    }
}

RoundRobinScheduler::~RoundRobinScheduler() = default;

void
RoundRobinScheduler::Reset()
{
    m_qToDq = -1;
    for (uint32_t i = 0; i < kMaxQueues; ++i)
    {
        m_queueLen[i] = 0;
    }
}

void
RoundRobinScheduler::OnEnqueue(uint32_t queueIndex, uint32_t /*packetSizeBytes*/)
{
    m_queueLen[queueIndex]++;
}

int
RoundRobinScheduler::SelectNextQueue()
{
    uint32_t i = 0;
    m_qToDq = (m_qToDq + 1) % static_cast<int>(m_numQueues);
    while (i < m_numQueues && m_queueLen[m_qToDq] == 0)
    {
        m_qToDq = (m_qToDq + 1) % static_cast<int>(m_numQueues);
        ++i;
    }
    if (i == m_numQueues)
    {
        return -1;
    }
    m_queueLen[m_qToDq]--;
    return m_qToDq;
}

} // namespace ns3::stratum
