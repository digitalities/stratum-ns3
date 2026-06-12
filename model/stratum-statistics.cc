/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * Copyright (C) 2000 Nortel Networks
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsred.{h,cc} statType and dsFD (2001).
 */

#include "stratum-statistics.h"

#include "ns3/log.h"
#include "ns3/type-id.h"

#include <iomanip>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::Statistics");

} // namespace ns3

namespace ns3::stratum
{

NS_OBJECT_ENSURE_REGISTERED(Statistics);

TypeId
Statistics::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::Statistics")
                            .SetParent<Object>()
                            .SetGroupName("Stratum")
                            .AddConstructor<Statistics>();
    return tid;
}

Statistics::Statistics()
    : Object(),
      m_counters()
{
}

// --- Recording methods -------------------------------------------------------

void
Statistics::RecordEnqueue(uint8_t dscp, uint32_t packetSizeBytes)
{
    NS_LOG_FUNCTION(this << static_cast<uint32_t>(dscp) << packetSizeBytes);
    NS_ASSERT_MSG(dscp < kMaxCodePoints, "DSCP out of range: " << static_cast<uint32_t>(dscp));
    m_counters[dscp].enqueued++;
}

void
Statistics::RecordDequeue(uint8_t dscp, uint32_t packetSizeBytes)
{
    NS_LOG_FUNCTION(this << static_cast<uint32_t>(dscp) << packetSizeBytes);
    NS_ASSERT_MSG(dscp < kMaxCodePoints, "DSCP out of range: " << static_cast<uint32_t>(dscp));
    m_counters[dscp].dequeued++;
}

void
Statistics::RecordRedDrop(uint8_t dscp, uint32_t packetSizeBytes)
{
    NS_LOG_FUNCTION(this << static_cast<uint32_t>(dscp) << packetSizeBytes);
    NS_ASSERT_MSG(dscp < kMaxCodePoints, "DSCP out of range: " << static_cast<uint32_t>(dscp));
    m_counters[dscp].redDrops++;
}

void
Statistics::RecordTailDrop(uint8_t dscp, uint32_t packetSizeBytes)
{
    NS_LOG_FUNCTION(this << static_cast<uint32_t>(dscp) << packetSizeBytes);
    NS_ASSERT_MSG(dscp < kMaxCodePoints, "DSCP out of range: " << static_cast<uint32_t>(dscp));
    m_counters[dscp].tailDrops++;
}

void
Statistics::RecordOwd(uint8_t dscp, double owdSeconds)
{
    NS_LOG_FUNCTION(this << static_cast<uint32_t>(dscp) << owdSeconds);
    NS_ASSERT_MSG(dscp < kMaxCodePoints, "DSCP out of range: " << static_cast<uint32_t>(dscp));
    m_counters[dscp].sumOwd += owdSeconds;
    m_counters[dscp].owdCount++;
}

void
Statistics::RecordIpdv(uint8_t dscp, double ipdvSeconds)
{
    NS_LOG_FUNCTION(this << static_cast<uint32_t>(dscp) << ipdvSeconds);
    NS_ASSERT_MSG(dscp < kMaxCodePoints, "DSCP out of range: " << static_cast<uint32_t>(dscp));
    m_counters[dscp].sumIpdv += ipdvSeconds;
    m_counters[dscp].ipdvCount++;
}

void
Statistics::RecordOrigBytes(uint8_t dscp, uint32_t bytes)
{
    NS_LOG_FUNCTION(this << static_cast<uint32_t>(dscp) << bytes);
    NS_ASSERT_MSG(dscp < kMaxCodePoints, "DSCP out of range: " << static_cast<uint32_t>(dscp));
    m_counters[dscp].origBytes += bytes;
}

void
Statistics::RecordRetxBytes(uint8_t dscp, uint32_t bytes)
{
    NS_LOG_FUNCTION(this << static_cast<uint32_t>(dscp) << bytes);
    NS_ASSERT_MSG(dscp < kMaxCodePoints, "DSCP out of range: " << static_cast<uint32_t>(dscp));
    m_counters[dscp].retxBytes += bytes;
}

// --- Query methods -----------------------------------------------------------

uint64_t
Statistics::GetEnqueued(uint8_t dscp) const
{
    NS_ASSERT_MSG(dscp < kMaxCodePoints, "DSCP out of range: " << static_cast<uint32_t>(dscp));
    return m_counters[dscp].enqueued;
}

uint64_t
Statistics::GetDequeued(uint8_t dscp) const
{
    NS_ASSERT_MSG(dscp < kMaxCodePoints, "DSCP out of range: " << static_cast<uint32_t>(dscp));
    return m_counters[dscp].dequeued;
}

uint64_t
Statistics::GetRedDrops(uint8_t dscp) const
{
    NS_ASSERT_MSG(dscp < kMaxCodePoints, "DSCP out of range: " << static_cast<uint32_t>(dscp));
    return m_counters[dscp].redDrops;
}

uint64_t
Statistics::GetTailDrops(uint8_t dscp) const
{
    NS_ASSERT_MSG(dscp < kMaxCodePoints, "DSCP out of range: " << static_cast<uint32_t>(dscp));
    return m_counters[dscp].tailDrops;
}

uint64_t
Statistics::GetTotalDrops(uint8_t dscp) const
{
    NS_ASSERT_MSG(dscp < kMaxCodePoints, "DSCP out of range: " << static_cast<uint32_t>(dscp));
    return m_counters[dscp].redDrops + m_counters[dscp].tailDrops;
}

double
Statistics::GetMeanOwd(uint8_t dscp) const
{
    NS_ASSERT_MSG(dscp < kMaxCodePoints, "DSCP out of range: " << static_cast<uint32_t>(dscp));
    if (m_counters[dscp].owdCount == 0)
    {
        return 0.0;
    }
    return m_counters[dscp].sumOwd / static_cast<double>(m_counters[dscp].owdCount);
}

double
Statistics::GetMeanIpdv(uint8_t dscp) const
{
    NS_ASSERT_MSG(dscp < kMaxCodePoints, "DSCP out of range: " << static_cast<uint32_t>(dscp));
    if (m_counters[dscp].ipdvCount == 0)
    {
        return 0.0;
    }
    return m_counters[dscp].sumIpdv / static_cast<double>(m_counters[dscp].ipdvCount);
}

uint64_t
Statistics::GetOrigBytes(uint8_t dscp) const
{
    NS_ASSERT_MSG(dscp < kMaxCodePoints, "DSCP out of range: " << static_cast<uint32_t>(dscp));
    return m_counters[dscp].origBytes;
}

uint64_t
Statistics::GetRetxBytes(uint8_t dscp) const
{
    NS_ASSERT_MSG(dscp < kMaxCodePoints, "DSCP out of range: " << static_cast<uint32_t>(dscp));
    return m_counters[dscp].retxBytes;
}

// --- Output ------------------------------------------------------------------

void
Statistics::PrintStats(std::ostream& os) const
{
    // Match ns-2 dsREDQueue::printStats() format (dsred.cc line 465)
    os << "\nPackets Statistics\n";
    os << "=======================================\n";
    os << " CP  TotPkts   TxPkts   ldrops   edrops\n";
    os << " --  -------   ------   ------   ------\n";

    uint64_t totalPkts = 0;
    uint64_t totalTailDrops = 0;
    uint64_t totalRedDrops = 0;

    for (int i = 0; i < kMaxCodePoints; ++i)
    {
        const auto& c = m_counters[i];
        if (c.enqueued == 0)
        {
            continue;
        }

        auto pktCount = static_cast<double>(c.enqueued);
        double txPct = (c.enqueued - c.tailDrops - c.redDrops) * 100.0 / pktCount;
        double lDropPct = c.tailDrops * 100.0 / pktCount;
        double eDropPct = c.redDrops * 100.0 / pktCount;

        os << std::setw(3) << i << " " << std::setw(8) << c.enqueued << "  " << std::fixed
           << std::setprecision(2) << std::setw(6) << txPct << "%  " << std::setw(6) << lDropPct
           << "%   " << std::setw(6) << eDropPct << "%\n";

        totalPkts += c.enqueued;
        totalTailDrops += c.tailDrops;
        totalRedDrops += c.redDrops;
    }

    os << "----------------------------------------\n";
    if (totalPkts != 0)
    {
        auto pktCount = static_cast<double>(totalPkts);
        double txPct = (totalPkts - totalTailDrops - totalRedDrops) * 100.0 / pktCount;
        double lDropPct = totalTailDrops * 100.0 / pktCount;
        double eDropPct = totalRedDrops * 100.0 / pktCount;

        os << "All " << std::setw(8) << totalPkts << "  " << std::fixed << std::setprecision(2)
           << std::setw(6) << txPct << "%  " << std::setw(6) << lDropPct << "%   " << std::setw(6)
           << eDropPct << "%\n";
    }

    // Retx accounting block — only emitted when at least one DSCP has data.
    // Goodput here is the thesis definition: origBytes / (origBytes + retxBytes).
    bool anyRetxData = false;
    for (int i = 0; i < kMaxCodePoints; ++i)
    {
        if (m_counters[i].origBytes != 0 || m_counters[i].retxBytes != 0)
        {
            anyRetxData = true;
            break;
        }
    }

    if (anyRetxData)
    {
        os << "\nRetx Statistics\n";
        os << "=======================================\n";
        os << " CP   origBytes   retxBytes  goodput\n";
        os << " --   ---------   ---------  -------\n";

        for (int i = 0; i < kMaxCodePoints; ++i)
        {
            const auto& c = m_counters[i];
            if (c.origBytes == 0 && c.retxBytes == 0)
            {
                continue;
            }
            auto denom = static_cast<double>(c.origBytes + c.retxBytes);
            double gp = (denom > 0.0) ? (static_cast<double>(c.origBytes) / denom) : 1.0;
            os << std::setw(3) << i << "  " << std::setw(10) << c.origBytes << "  " << std::setw(10)
               << c.retxBytes << "  " << std::fixed << std::setprecision(3) << std::setw(7) << gp
               << "\n";
        }
        os << "----------------------------------------\n";
    }
}

} // namespace ns3::stratum
