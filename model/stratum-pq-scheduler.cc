/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsscheduler.cc class dsPQ (2001).
 */

#include "stratum-pq-scheduler.h"

#include "ns3/double.h"
#include "ns3/log.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::PriorityScheduler");

} // namespace ns3

namespace ns3::stratum
{

NS_OBJECT_ENSURE_REGISTERED(PriorityScheduler);

TypeId
PriorityScheduler::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::stratum::PriorityScheduler")
            .SetParent<Scheduler>()
            .SetGroupName("Stratum")
            .AddConstructor<PriorityScheduler>()
            .AddAttribute("WinLen",
                          "Time-Sliding-Window length in seconds "
                          "for the base-class departure-rate "
                          "estimator. Construct-only because "
                          "mid-stream change would corrupt "
                          "accumulated rate state.",
                          TypeId::ATTR_GET | TypeId::ATTR_CONSTRUCT,
                          DoubleValue(1.0),
                          MakeDoubleAccessor(&Scheduler::SetWinLen, &Scheduler::GetWinLen),
                          MakeDoubleChecker<double>(0.0));
    return tid;
}

PriorityScheduler::PriorityScheduler()
{
    for (uint32_t i = 0; i < kMaxQueues; ++i)
    {
        m_queueMaxRate[i] = 0.0;
        m_queueLen[i] = 0;
    }
}

PriorityScheduler::~PriorityScheduler() = default;

void
PriorityScheduler::Reset()
{
    for (uint32_t i = 0; i < kMaxQueues; ++i)
    {
        m_queueArrTime[i] = 0.0;
    }
}

void
PriorityScheduler::OnEnqueue(uint32_t queueIndex, uint32_t /*packetSizeBytes*/)
{
    m_queueLen[queueIndex]++;
}

void
PriorityScheduler::SetParam(uint32_t queueIndex, double maxRateBps)
{
    // Store rate cap in bytes/s (matching ns-2: queueMaxRate[Queue]=MaxRate/8)
    m_queueMaxRate[queueIndex] = maxRateBps / 8.0;
}

int
PriorityScheduler::SelectNextQueue()
{
    // First pass: highest-priority non-empty queue under its rate cap
    int qToDq = 0;
    uint32_t i = 0;
    while (i < m_numQueues &&
           (m_queueLen[qToDq] == 0 ||
            (m_queueMaxRate[qToDq] > 0.0 && m_queueAvgRate[qToDq] > m_queueMaxRate[qToDq])))
    {
        ++i;
        qToDq = static_cast<int>(i);
    }

    // Second pass: if all rate-capped, fall back to any non-empty queue
    if (i == m_numQueues)
    {
        i = 0;
        qToDq = 0;
        while (i < m_numQueues && m_queueLen[qToDq] == 0)
        {
            qToDq = (qToDq + 1) % static_cast<int>(m_numQueues);
            ++i;
        }
    }

    if (i < m_numQueues)
    {
        m_queueLen[qToDq]--;
        return qToDq;
    }
    return -1;
}

} // namespace ns3::stratum
