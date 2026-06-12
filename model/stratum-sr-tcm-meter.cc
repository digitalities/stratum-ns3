/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * Copyright (C) 2000 Nortel Networks
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsPolicy.cc SRTCMPolicy (2001).
 * Line-for-line translation of applyMeter (line 664) and
 * applyPolicer (line 697).
 */

#include "stratum-sr-tcm-meter.h"

#include "ns3/log.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::SrTcmMeter");

} // namespace ns3

namespace ns3::stratum
{

NS_OBJECT_ENSURE_REGISTERED(SrTcmMeter);

TypeId
SrTcmMeter::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::SrTcmMeter")
                            .SetParent<Meter>()
                            .SetGroupName("Stratum")
                            .AddConstructor<SrTcmMeter>();
    return tid;
}

void
SrTcmMeter::ApplyMeter(PolicyEntry& entry, double nowSeconds, uint32_t /* packetSize */)
{
    // dsPolicy.cc line 668: tokenBytes = cir * (now - arrivalTime)
    double tokenBytes = entry.cir * (nowSeconds - entry.arrivalTime);

    // dsPolicy.cc line 669-678: fill cBucket, overflow to eBucket
    if (entry.cBucket + tokenBytes <= entry.cbs)
    {
        // All tokens fit in cBucket; eBucket is unchanged.
        entry.cBucket += tokenBytes;
    }
    else
    {
        // cBucket overflows: compute excess and spill to eBucket.
        // dsPolicy.cc line 672: tokenBytes = tokenBytes - (cbs - cBucket)
        tokenBytes = tokenBytes - (entry.cbs - entry.cBucket);
        entry.cBucket = entry.cbs;

        if (entry.eBucket + tokenBytes <= entry.ebs)
        {
            entry.eBucket += tokenBytes;
        }
        else
        {
            entry.eBucket = entry.ebs;
        }
    }
    entry.arrivalTime = nowSeconds;
}

Colour
SrTcmMeter::DoApplyPolicer(PolicyEntry& entry, uint32_t packetSize)
{
    // dsPolicy.cc line 702-711: GREEN/YELLOW/RED cascade.
    // Wire-byte basis.
    auto size = static_cast<double>(packetSize + m_l2OverheadBytes);

    if ((entry.cBucket - size) >= 0)
    {
        // GREEN: cBucket absorbs the packet.
        entry.cBucket -= size;
        return Colour::GREEN;
    }
    if ((entry.eBucket - size) >= 0)
    {
        // YELLOW: eBucket absorbs; cBucket is NOT decremented.
        entry.eBucket -= size;
        return Colour::YELLOW;
    }
    // RED: neither bucket is decremented.
    return Colour::RED;
}

} // namespace ns3::stratum
