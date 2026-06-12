/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * Copyright (C) 2000 Nortel Networks
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsPolicy.cc TRTCMPolicy (2001).
 * Line-for-line translation of applyMeter (line 729) and
 * applyPolicer (line 761).
 */

#include "stratum-tr-tcm-meter.h"

#include "ns3/log.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::TrTcmMeter");

} // namespace ns3

namespace ns3::stratum
{

NS_OBJECT_ENSURE_REGISTERED(TrTcmMeter);

TypeId
TrTcmMeter::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::TrTcmMeter")
                            .SetParent<Meter>()
                            .SetGroupName("Stratum")
                            .AddConstructor<TrTcmMeter>();
    return tid;
}

void
TrTcmMeter::ApplyMeter(PolicyEntry& entry, double nowSeconds, uint32_t /* packetSize */)
{
    double dt = nowSeconds - entry.arrivalTime;

    // dsPolicy.cc line 732-736: cBucket refills at CIR, capped at CBS
    double cTokens = entry.cir * dt;
    if (entry.cBucket + cTokens <= entry.cbs)
    {
        entry.cBucket += cTokens;
    }
    else
    {
        entry.cBucket = entry.cbs;
    }

    // dsPolicy.cc line 738-742: pBucket refills at PIR, capped at PBS
    double pTokens = entry.pir * dt;
    if (entry.pBucket + pTokens <= entry.pbs)
    {
        entry.pBucket += pTokens;
    }
    else
    {
        entry.pBucket = entry.pbs;
    }

    entry.arrivalTime = nowSeconds;
}

Colour
TrTcmMeter::DoApplyPolicer(PolicyEntry& entry, uint32_t packetSize)
{
    // dsPolicy.cc line 765-776: RED / YELLOW / GREEN.
    // Wire-byte basis.
    auto size = static_cast<double>(packetSize + m_l2OverheadBytes);

    if ((entry.pBucket - size) < 0)
    {
        // RED: pBucket too small. Neither bucket is decremented.
        return Colour::RED;
    }
    if ((entry.cBucket - size) < 0)
    {
        // YELLOW: pBucket sufficient but cBucket insufficient.
        // Only pBucket is decremented; cBucket is untouched.
        entry.pBucket -= size;
        return Colour::YELLOW;
    }
    // GREEN: both buckets sufficient. Both are decremented.
    entry.cBucket -= size;
    entry.pBucket -= size;
    return Colour::GREEN;
}

} // namespace ns3::stratum
