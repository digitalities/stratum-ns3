/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * Copyright (C) 2000 Nortel Networks
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsPolicy.cc TSW2CMPolicy (2001).
 * Line-for-line translation of applyMeter (line 515) and
 * applyPolicer (line 536).
 */

#include "stratum-tsw2cm-meter.h"

#include "ns3/log.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::Tsw2cmMeter");

} // namespace ns3

namespace ns3::stratum
{

NS_OBJECT_ENSURE_REGISTERED(Tsw2cmMeter);

TypeId
Tsw2cmMeter::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::Tsw2cmMeter")
                            .SetParent<Meter>()
                            .SetGroupName("Stratum")
                            .AddConstructor<Tsw2cmMeter>();
    return tid;
}

Tsw2cmMeter::Tsw2cmMeter()
    : m_uv(CreateObject<UniformRandomVariable>())
{
}

void
Tsw2cmMeter::ApplyMeter(PolicyEntry& entry, double nowSeconds, uint32_t packetSize)
{
    // dsPolicy.cc line 519-523: EWMA rate estimation.
    // Wire-byte basis.
    double bytesInTsw = entry.avgRate * entry.winLen;
    double newBytes = bytesInTsw + static_cast<double>(packetSize + m_l2OverheadBytes);
    entry.avgRate = newBytes / (nowSeconds - entry.arrivalTime + entry.winLen);
    entry.arrivalTime = nowSeconds;
}

Colour
Tsw2cmMeter::DoApplyPolicer(PolicyEntry& entry, uint32_t /* packetSize */)
{
    // dsPolicy.cc line 537-543: probabilistic marking
    if (entry.avgRate > entry.cir)
    {
        double pDrop = 1.0 - (entry.cir / entry.avgRate);
        if (m_uv->GetValue() <= pDrop)
        {
            return Colour::RED;
        }
    }
    return Colour::GREEN;
}

int64_t
Tsw2cmMeter::AssignStreams(int64_t stream)
{
    m_uv->SetStream(stream);
    return 1;
}

} // namespace ns3::stratum
