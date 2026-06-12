/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * Copyright (C) 2000 Nortel Networks
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsPolicy.cc TSW3CMPolicy (2001).
 * Line-for-line translation of applyMeter (line 558) and
 * applyPolicer (line 589).
 */

#include "stratum-tsw3cm-meter.h"

#include "ns3/log.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::Tsw3cmMeter");

} // namespace ns3

namespace ns3::stratum
{

NS_OBJECT_ENSURE_REGISTERED(Tsw3cmMeter);

TypeId
Tsw3cmMeter::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::Tsw3cmMeter")
                            .SetParent<Meter>()
                            .SetGroupName("Stratum")
                            .AddConstructor<Tsw3cmMeter>();
    return tid;
}

Tsw3cmMeter::Tsw3cmMeter()
    : m_uv(CreateObject<UniformRandomVariable>())
{
}

void
Tsw3cmMeter::ApplyMeter(PolicyEntry& entry, double nowSeconds, uint32_t packetSize)
{
    // dsPolicy.cc line 562-566: same EWMA as TSW2CM.
    // Wire-byte basis.
    double bytesInTsw = entry.avgRate * entry.winLen;
    double newBytes = bytesInTsw + static_cast<double>(packetSize + m_l2OverheadBytes);
    entry.avgRate = newBytes / (nowSeconds - entry.arrivalTime + entry.winLen);
    entry.arrivalTime = nowSeconds;
}

Colour
Tsw3cmMeter::DoApplyPolicer(PolicyEntry& entry, uint32_t /* packetSize */)
{
    // dsPolicy.cc line 590: rand = avgRate * (1 - uniform(0,1))
    // Produces a value in (0, avgRate].
    double rand = entry.avgRate * (1.0 - m_uv->GetValue());

    // dsPolicy.cc line 592-597: threshold comparison
    if (rand > entry.pir)
    {
        return Colour::RED;
    }
    if (rand > entry.cir)
    {
        return Colour::YELLOW;
    }
    return Colour::GREEN;
}

int64_t
Tsw3cmMeter::AssignStreams(int64_t stream)
{
    m_uv->SetStream(stream);
    return 1;
}

} // namespace ns3::stratum
