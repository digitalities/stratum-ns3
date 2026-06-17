/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * Copyright (C) 2000 Nortel Networks
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsredq.cc class redQueue (2001).
 */

#include "stratum-red-sub-queue.h"

#include "ns3/drop-tail-queue.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"

#include <algorithm>
#include <cmath>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::RedSubQueue");

} // namespace ns3

namespace ns3::stratum
{

NS_OBJECT_ENSURE_REGISTERED(RedSubQueue);

// --- VirtualQueueParams ---

VirtualQueueParams::VirtualQueueParams()
    : thMin(0.0),
      thMax(0.0),
      maxP(0.0),
      qW(0.002),
      ptc(0.0),
      meanPktSize(1000),
      vAve(0.0),
      vProb(0.0),
      count(0),
      qlen(0),
      idletime(0.0),
      idle(true)
{
}

// --- RedSubQueue ---

TypeId
RedSubQueue::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::RedSubQueue")
                            .SetParent<QueueDisc>()
                            .SetGroupName("Stratum")
                            .AddConstructor<RedSubQueue>();
    return tid;
}

RedSubQueue::RedSubQueue()
    : m_numPrec(kMaxPrec),
      m_qlim(50),
      m_qlen(0),
      m_qMaxBur(0),
      m_mredMode(MredMode::RIO_C),
      m_currentPrec(0),
      m_currentEcn(false),
      m_lastResult(PktResult::PKT_ENQUEUED)
{
    m_rng = CreateObject<UniformRandomVariable>();
}

RedSubQueue::~RedSubQueue()
{
}

int64_t
RedSubQueue::AssignStreams(int64_t stream)
{
    NS_LOG_FUNCTION(this << stream);
    m_rng->SetStream(stream);
    return 1;
}

void
RedSubQueue::ConfigureVirtualQueue(uint32_t prec, double thMin, double thMax, double maxP)
{
    prec = std::min(prec, static_cast<uint32_t>(kMaxPrec) - 1);
    m_qParam[prec].thMin = thMin;
    m_qParam[prec].thMax = thMax;
    m_qParam[prec].maxP = maxP;
}

void
RedSubQueue::SetNumPrec(uint32_t numPrec)
{
    // Clamp to the fixed per-precedence array bound so m_qParam[] is never
    // iterated out of range by a misconfigured precedence count.
    m_numPrec = std::min(numPrec, static_cast<uint32_t>(kMaxPrec));
}

uint32_t
RedSubQueue::GetNumPrec() const
{
    return m_numPrec;
}

void
RedSubQueue::SetMredMode(MredMode mode)
{
    m_mredMode = mode;
}

MredMode
RedSubQueue::GetMredMode() const
{
    return m_mredMode;
}

void
RedSubQueue::SetQueueLimit(uint32_t limit)
{
    m_qlim = limit;
}

uint32_t
RedSubQueue::GetQueueLimit() const
{
    return m_qlim;
}

void
RedSubQueue::SetPtc(double linkBandwidthBps)
{
    for (uint32_t i = 0; i < kMaxPrec; ++i)
    {
        m_qParam[i].ptc = linkBandwidthBps / (8.0 * m_qParam[i].meanPktSize);
    }
}

void
RedSubQueue::SetMeanPacketSize(int mps)
{
    for (uint32_t i = 0; i < kMaxPrec; ++i)
    {
        m_qParam[i].meanPktSize = mps;
    }
}

PktResult
RedSubQueue::EnqueueWithPrec(Ptr<QueueDiscItem> item, uint32_t prec, bool ecn)
{
    m_currentPrec = std::min(prec, static_cast<uint32_t>(kMaxPrec) - 1);
    m_currentEcn = ecn;
    m_lastResult = PktResult::PKT_ENQUEUED;

    QueueDisc::Enqueue(item);

    return m_lastResult;
}

void
RedSubQueue::InitRedStateVars(double nowSeconds)
{
    for (uint32_t i = 0; i < m_numPrec; ++i)
    {
        m_qParam[i].idle = true;
        m_qParam[i].idletime = nowSeconds;
    }
}

void
RedSubQueue::UpdateRedStateVar(uint32_t prec, double nowSeconds)
{
    prec = std::min(prec, static_cast<uint32_t>(kMaxPrec) - 1);
    // Port of redQueue::updateREDStateVar from dsredq.cc
    m_qParam[prec].qlen--;

    if (m_qParam[prec].qlen == 0)
    {
        if (m_mredMode == MredMode::RIO_C)
        {
            bool idle = true;
            for (uint32_t i = 0; i < prec; ++i)
            {
                if (m_qParam[i].qlen != 0)
                {
                    idle = false;
                }
            }
            if (idle)
            {
                for (uint32_t i = prec; i < m_numPrec; ++i)
                {
                    if (m_qParam[i].qlen == 0)
                    {
                        m_qParam[i].idle = true;
                        m_qParam[i].idletime = nowSeconds;
                    }
                    else
                    {
                        break;
                    }
                }
            }
        }
        else if (m_mredMode == MredMode::RIO_D)
        {
            m_qParam[prec].idle = true;
            m_qParam[prec].idletime = nowSeconds;
        }
        else if (m_mredMode == MredMode::WRED)
        {
            m_qParam[0].idle = true;
            m_qParam[0].idletime = nowSeconds;
        }
    }
}

double
RedSubQueue::GetWeightedLength() const
{
    if (m_mredMode == MredMode::RIO_C)
    {
        return m_qParam[m_numPrec - 1].vAve;
    }
    double sum = 0.0;
    for (uint32_t prec = 0; prec < m_numPrec; ++prec)
    {
        sum += m_qParam[prec].vAve;
    }
    return sum;
}

int
RedSubQueue::GetRealLength() const
{
    return m_qlen;
}

int
RedSubQueue::GetVirtualQueueLen(uint32_t prec) const
{
    return m_qParam[std::min(prec, static_cast<uint32_t>(kMaxPrec) - 1)].qlen;
}

void
RedSubQueue::CalcAvg(uint32_t prec, int m)
{
    // Port of redQueue::calcAvg from dsredq.cc
    double f = m_qParam[prec].vAve;

    while (--m >= 1)
    {
        f *= 1.0 - m_qParam[prec].qW;
    }
    f *= 1.0 - m_qParam[prec].qW;

    if (m_mredMode == MredMode::RIO_C)
    {
        for (uint32_t i = 0; i <= prec; ++i)
        {
            f += m_qParam[i].qW * m_qParam[i].qlen;
        }
    }
    else if (m_mredMode == MredMode::RIO_D)
    {
        f += m_qParam[prec].qW * m_qParam[prec].qlen;
    }
    else // WRED
    {
        f += m_qParam[prec].qW * m_qlen; // use physical queue length
    }

    if (m_mredMode == MredMode::WRED)
    {
        for (uint32_t i = 0; i < m_numPrec; ++i)
        {
            m_qParam[i].vAve = f;
        }
    }
    else
    {
        m_qParam[prec].vAve = f;
    }
}

bool
RedSubQueue::DoEnqueue(Ptr<QueueDiscItem> item)
{
    uint32_t prec = m_currentPrec;
    bool ecn = m_currentEcn;

    // DropTail special case: if th_min == -1 for this prec, early-drop everything
    if (m_mredMode == MredMode::DROP_TAIL && m_qParam[prec].thMin < 0)
    {
        m_lastResult = PktResult::PKT_EDROPPED;
        DropBeforeEnqueue(item, "EDROP_DISABLED_PREC");
        return false;
    }

    // Physical buffer overflow -> tail drop
    if (static_cast<uint32_t>(m_qlen) >= m_qlim)
    {
        m_lastResult = PktResult::PKT_DROPPED;
        DropBeforeEnqueue(item, "QUEUE_FULL");
        return false;
    }

    double now = Simulator::Now().GetSeconds();

    // DropTail mode
    if (m_mredMode == MredMode::DROP_TAIL)
    {
        if (m_qParam[0].thMin >= 0 && m_qlen >= static_cast<int>(m_qParam[0].thMin))
        {
            m_lastResult = PktResult::PKT_EDROPPED;
            DropBeforeEnqueue(item, "DROPTAIL_THRESHOLD");
            return false;
        }
        // Enqueue to internal queue
        bool ok = GetInternalQueue(0)->Enqueue(item);
        if (ok)
        {
            m_qlen++;
            m_qParam[prec].qlen++;
            m_qMaxBur = std::max(m_qMaxBur, m_qlen);
        }
        m_lastResult = ok ? PktResult::PKT_ENQUEUED : PktResult::PKT_DROPPED;
        return ok;
    }

    // Update EWMA averages per MRED mode
    int m = 0;
    if (m_mredMode == MredMode::RIO_C)
    {
        for (uint32_t i = prec; i < m_numPrec; ++i)
        {
            m = 0;
            if (m_qParam[i].idle)
            {
                m_qParam[i].idle = false;
                m = static_cast<int>(m_qParam[i].ptc * (now - m_qParam[i].idletime));
            }
            CalcAvg(i, m + 1);
        }
    }
    else if (m_mredMode == MredMode::RIO_D)
    {
        if (m_qParam[prec].idle)
        {
            m_qParam[prec].idle = false;
            m = static_cast<int>(m_qParam[prec].ptc * (now - m_qParam[prec].idletime));
        }
        CalcAvg(prec, m + 1);
    }
    else // WRED
    {
        if (m_qParam[0].idle)
        {
            m_qParam[0].idle = false;
            m = static_cast<int>(m_qParam[0].ptc * (now - m_qParam[0].idletime));
        }
        CalcAvg(0, m + 1);
    }

    // If using ECN, enqueue before drop decision
    if (ecn)
    {
        bool ok = GetInternalQueue(0)->Enqueue(item);
        if (ok)
        {
            m_qlen++;
            m_qMaxBur = std::max(m_qMaxBur, m_qlen);
            m_qParam[prec].qlen++;
        }
    }

    // RED drop decision
    if (m_qParam[prec].vAve > m_qParam[prec].thMin)
    {
        if (m_qParam[prec].vAve <= m_qParam[prec].thMax)
        {
            // Between thMin and thMax: probabilistic drop
            m_qParam[prec].count++;
            m_qParam[prec].vProb = m_qParam[prec].maxP *
                                   (m_qParam[prec].vAve - m_qParam[prec].thMin) /
                                   (m_qParam[prec].thMax - m_qParam[prec].thMin);

            double pb = m_qParam[prec].vProb;
            double pa = pb / (1.0 - m_qParam[prec].count * pb);

            // Random draw
            double u = m_rng->GetValue(0.0, 1.0);

            if (u <= pa)
            {
                if (ecn)
                {
                    m_lastResult = PktResult::PKT_MARKED;
                    return true; // already enqueued
                }
                m_lastResult = PktResult::PKT_EDROPPED;
                DropBeforeEnqueue(item, "RED_EARLY_DROP");
                return false;
            }
        }
        else
        {
            // Above thMax: forced drop
            m_qParam[prec].count = 0;
            if (ecn)
            {
                m_lastResult = PktResult::PKT_MARKED;
                return true; // already enqueued
            }
            m_lastResult = PktResult::PKT_EDROPPED;
            DropBeforeEnqueue(item, "RED_FORCED_DROP");
            return false;
        }
    }
    m_qParam[prec].count = -1;

    // If ECN, packet already enqueued
    if (ecn)
    {
        m_lastResult = PktResult::PKT_ENQUEUED;
        return true;
    }

    // Enqueue the packet
    bool ok = GetInternalQueue(0)->Enqueue(item);
    if (ok)
    {
        m_qParam[prec].qlen++;
        m_qlen++;
        m_qMaxBur = std::max(m_qMaxBur, m_qlen);
    }
    m_lastResult = ok ? PktResult::PKT_ENQUEUED : PktResult::PKT_DROPPED;
    return ok;
}

Ptr<QueueDiscItem>
RedSubQueue::DoDequeue()
{
    if (GetInternalQueue(0)->IsEmpty())
    {
        return nullptr;
    }
    Ptr<QueueDiscItem> item = GetInternalQueue(0)->Dequeue();
    m_qlen--;
    return item;
}

Ptr<const QueueDiscItem>
RedSubQueue::DoPeek()
{
    if (GetInternalQueue(0)->IsEmpty())
    {
        return nullptr;
    }
    return GetInternalQueue(0)->Peek();
}

bool
RedSubQueue::CheckConfig()
{
    if (GetNInternalQueues() == 0)
    {
        AddInternalQueue(CreateObjectWithAttributes<DropTailQueue<QueueDiscItem>>(
            "MaxSize",
            QueueSizeValue(QueueSize(QueueSizeUnit::PACKETS, m_qlim))));
    }
    return (GetNInternalQueues() == 1);
}

void
RedSubQueue::InitializeParams()
{
    double now = Simulator::Now().GetSeconds();
    InitRedStateVars(now);
}

} // namespace ns3::stratum
