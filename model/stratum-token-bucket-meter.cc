/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * Copyright (C) 2000 Nortel Networks
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsPolicy.cc TBPolicy (2001).
 * Line-for-line translation of applyMeter (line 610) and
 * applyPolicer (line 634).
 */

#include "stratum-token-bucket-meter.h"

#include "ns3/log.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::TokenBucketMeter");

} // namespace ns3

namespace ns3::stratum
{

NS_OBJECT_ENSURE_REGISTERED(TokenBucketMeter);

TypeId
TokenBucketMeter::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::TokenBucketMeter")
                            .SetParent<Meter>()
                            .SetGroupName("Stratum")
                            .AddConstructor<TokenBucketMeter>();
    return tid;
}

void
TokenBucketMeter::ApplyMeter(PolicyEntry& entry, double nowSeconds, uint32_t /* packetSize */)
{
    // dsPolicy.cc line 614: tokenBytes = cir * (now - arrivalTime)
    double tokenBytes = entry.cir * (nowSeconds - entry.arrivalTime);

    // dsPolicy.cc line 615-618: fill cBucket, cap at CBS
    if (entry.cBucket + tokenBytes <= entry.cbs)
    {
        entry.cBucket += tokenBytes;
    }
    else
    {
        entry.cBucket = entry.cbs;
    }
    entry.arrivalTime = nowSeconds;
}

Colour
TokenBucketMeter::DoApplyPolicer(PolicyEntry& entry, uint32_t packetSize)
{
    // dsPolicy.cc line 639: if (cBucket - size >= 0).
    // Wire-byte basis: charge tokens against the wire bytes the link
    // will transmit (IP + L2 framing) so meter and FQ scheduler share
    // one byte basis.
    auto size = static_cast<double>(packetSize + m_l2OverheadBytes);
    if ((entry.cBucket - size) >= 0)
    {
        entry.cBucket -= size;
        return Colour::GREEN;
    }
    return Colour::RED;
}

} // namespace ns3::stratum
