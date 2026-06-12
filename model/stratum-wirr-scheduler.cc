/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsscheduler.cc class dsWIRR (2001).
 */

#include "stratum-wirr-scheduler.h"

#include "ns3/log.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::WeightedInterleavedRoundRobinScheduler");

} // namespace ns3

namespace ns3::stratum
{

NS_OBJECT_ENSURE_REGISTERED(WeightedInterleavedRoundRobinScheduler);

TypeId
WeightedInterleavedRoundRobinScheduler::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::WeightedInterleavedRoundRobinScheduler")
                            .SetParent<Scheduler>()
                            .SetGroupName("Stratum")
                            .AddConstructor<WeightedInterleavedRoundRobinScheduler>();
    return tid;
}

WeightedInterleavedRoundRobinScheduler::WeightedInterleavedRoundRobinScheduler()
{
    for (uint32_t i = 0; i < kMaxQueues; ++i)
    {
        m_queueLen[i] = 0;
        m_queueWeight[i] = 1;
        m_wirrTemp[i] = 0;
        m_slicecount[i] = 0;
        m_wirrqDone[i] = false;
    }
    Reset();
}

WeightedInterleavedRoundRobinScheduler::~WeightedInterleavedRoundRobinScheduler() = default;

void
WeightedInterleavedRoundRobinScheduler::Reset()
{
    for (uint32_t i = 0; i < kMaxQueues; ++i)
    {
        m_slicecount[i] = 0;
        m_wirrTemp[i] = 0;
        m_wirrqDone[i] = false;
    }
}

void
WeightedInterleavedRoundRobinScheduler::OnEnqueue(uint32_t queueIndex, uint32_t /*packetSizeBytes*/)
{
    m_queueLen[queueIndex]++;
}

void
WeightedInterleavedRoundRobinScheduler::SetParam(uint32_t queueIndex, double weight)
{
    m_queueWeight[queueIndex] = static_cast<int>(std::ceil(weight));
}

int
WeightedInterleavedRoundRobinScheduler::SelectNextQueue()
{
    uint32_t i = 0;
    m_qToDq = (m_qToDq + 1) % static_cast<int>(m_numQueues);
    while (i < m_numQueues && (m_queueLen[m_qToDq] == 0 || m_wirrqDone[m_qToDq]))
    {
        if (!m_wirrqDone[m_qToDq] && m_queueLen[m_qToDq] == 0)
        {
            m_queuesDone++;
            m_wirrqDone[m_qToDq] = true;
        }
        m_qToDq = (m_qToDq + 1) % static_cast<int>(m_numQueues);
        ++i;
    }
    if (m_wirrTemp[m_qToDq] == 1)
    {
        m_queuesDone++;
        m_wirrqDone[m_qToDq] = true;
    }
    m_wirrTemp[m_qToDq]--;
    if (m_queuesDone >= m_numQueues)
    {
        m_queuesDone = 0;
        for (uint32_t j = 0; j < m_numQueues; ++j)
        {
            m_wirrTemp[j] = m_queueWeight[j];
            m_wirrqDone[j] = false;
        }
    }
    if (i == m_numQueues)
    {
        return -1;
    }
    m_queueLen[m_qToDq]--;
    return m_qToDq;
}

} // namespace ns3::stratum
