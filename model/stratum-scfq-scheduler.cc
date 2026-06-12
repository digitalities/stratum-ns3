/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsscheduler.cc class dsSCFQ (2001).
 * Self-Clocked Fair Queueing (Golestani 1994).
 */

#include "stratum-scfq-scheduler.h"

#include "ns3/assert.h"
#include "ns3/log.h"
#include "ns3/simulator.h"

#include <algorithm>
#include <iomanip>
#include <limits>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::ScfqScheduler");

} // namespace ns3

namespace ns3::stratum
{

NS_OBJECT_ENSURE_REGISTERED(ScfqScheduler);

TypeId
ScfqScheduler::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::ScfqScheduler")
                            .SetParent<Scheduler>()
                            .SetGroupName("Stratum")
                            .AddConstructor<ScfqScheduler>();
    return tid;
}

ScfqScheduler::ScfqScheduler()
{
    for (uint32_t i = 0; i < kMaxQueues; ++i)
    {
        m_session[i].weight = 1.0;
    }
    Reset();
}

ScfqScheduler::~ScfqScheduler() = default;

void
ScfqScheduler::Reset()
{
    for (uint32_t i = 0; i < kMaxQueues; ++i)
    {
        m_session[i].label = 0.0;
        // Clear the session queue (swap with empty queue)
        std::queue<double>().swap(m_session[i].sessionQueue);
    }
    m_tlabel = 0.0;
}

void
ScfqScheduler::OnEnqueue(uint32_t queueIndex, uint32_t packetSizeBytes)
{
    // Called by OnEnqueueWithTime for base-class tracking.
    // SCFQ does not use this path directly; the virtual-time update
    // is in OnEnqueueWithTime. This satisfies the pure-virtual requirement.
    NS_LOG_FUNCTION(this << queueIndex << packetSizeBytes);
}

void
ScfqScheduler::OnEnqueueWithTime(uint32_t queueIndex,
                                 uint32_t packetSizeBytes,
                                 double /*nowSeconds*/)
{
    NS_LOG_FUNCTION(this << queueIndex << packetSizeBytes);
    NS_ASSERT_MSG(queueIndex < m_numQueues, "Queue index out of range");

    // Base-class tracking (no-op for SCFQ but keeps the interface contract)
    OnEnqueue(queueIndex, packetSizeBytes);

    double bwBytes = GetLinkBandwidth() / 8.0;
    NS_ASSERT_MSG(bwBytes > 0.0, "Link bandwidth must be set before enqueue");
    NS_ASSERT_MSG(m_session[queueIndex].weight > 0.0, "Weight must be positive");

    // Wire-byte basis: finish-time math charges IP bytes plus the L2
    // framing the netdev will add downstream, so the FQ promise matches
    // what the link physically delivers.
    const uint32_t wireBytes = packetSizeBytes + GetL2OverheadBytes();

    double prevLabel = m_session[queueIndex].label;
    double maxBase = std::max(prevLabel, m_tlabel);
    double increment = static_cast<double>(wireBytes) / (m_session[queueIndex].weight * bwBytes);

    m_session[queueIndex].label = maxBase + increment;
    m_session[queueIndex].sessionQueue.push(m_session[queueIndex].label);

    NS_LOG_DEBUG("SCFQ enqueue q=" << queueIndex << " label=" << m_session[queueIndex].label);

    if (m_logStream)
    {
        double now = Simulator::Now().GetSeconds();
        *m_logStream << std::fixed << std::setprecision(9) << now << ",ENQ," << queueIndex << ","
                     << packetSizeBytes << "," << std::setprecision(12) << prevLabel << ","
                     << m_tlabel << "," << maxBase << "," << increment << ","
                     << m_session[queueIndex].label << "," << m_session[0].sessionQueue.size()
                     << "," << m_session[1].sessionQueue.size() << "\n";
    }
}

int
ScfqScheduler::SelectNextQueue()
{
    NS_LOG_FUNCTION(this);

    int qToDq = -1;
    double minFinishTime = std::numeric_limits<double>::max();

    for (uint32_t i = 0; i < m_numQueues; ++i)
    {
        if (!m_session[i].sessionQueue.empty() && m_session[i].sessionQueue.front() < minFinishTime)
        {
            qToDq = static_cast<int>(i);
            minFinishTime = m_session[i].sessionQueue.front();
        }
    }

    if (qToDq != -1)
    {
        double prevTlabel = m_tlabel;
        m_tlabel = minFinishTime;
        m_session[qToDq].sessionQueue.pop();
        NS_LOG_DEBUG("SCFQ dequeue q=" << qToDq << " tlabel=" << m_tlabel);

        m_deqCount[qToDq]++;

        if (m_logStream)
        {
            double now = Simulator::Now().GetSeconds();
            // Front labels of each queue (what would be selected next)
            double efFront =
                m_session[0].sessionQueue.empty() ? -1.0 : m_session[0].sessionQueue.front();
            double beFront =
                m_session[1].sessionQueue.empty() ? -1.0 : m_session[1].sessionQueue.front();
            uint64_t totalDeq = m_deqCount[0] + m_deqCount[1];
            double efPct = (totalDeq > 0) ? 100.0 * m_deqCount[0] / totalDeq : 0.0;
            double bePerEf =
                (m_deqCount[0] > 0) ? static_cast<double>(m_deqCount[1]) / m_deqCount[0] : 0.0;
            *m_logStream << std::fixed << std::setprecision(9) << now << ",DEQ," << qToDq << ","
                         << 0 << "," // pktSize not available at dequeue
                         << std::setprecision(12) << efFront << "," << beFront << "," << prevTlabel
                         << "," << 0.0 << "," << m_tlabel << "," << m_session[0].sessionQueue.size()
                         << "," << m_session[1].sessionQueue.size() << "," << m_deqCount[0] << ","
                         << m_deqCount[1] << "," << std::setprecision(4) << efPct << ","
                         << std::setprecision(2) << bePerEf << "\n";
        }
    }

    return qToDq;
}

void
ScfqScheduler::SetParam(uint32_t queueIndex, double weight)
{
    NS_ASSERT_MSG(queueIndex < m_numQueues, "Queue index out of range");
    NS_ASSERT_MSG(weight > 0.0, "Weight must be positive");
    m_session[queueIndex].weight = weight;
}

void
ScfqScheduler::SetLogStream(std::ostream* os)
{
    m_logStream = os;
    if (m_logStream)
    {
        // Write CSV header
        *m_logStream << "time,event,queue,pktSize,efLabelOrFront,beLabelOrFront,"
                     << "tlabelOrMaxBase,increment,newLabelOrTlabel,efQdepth,beQdepth,"
                     << "efDeqCum,beDeqCum,efDeqPct,bePerEf\n";
    }
}

} // namespace ns3::stratum
