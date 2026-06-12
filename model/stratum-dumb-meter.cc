/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * Copyright (C) 2000 Nortel Networks
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsPolicy.cc DumbPolicy (2001).
 */

#include "stratum-dumb-meter.h"

#include "ns3/log.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::DumbMeter");

} // namespace ns3

namespace ns3::stratum
{

NS_OBJECT_ENSURE_REGISTERED(DumbMeter);

TypeId
DumbMeter::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::DumbMeter")
                            .SetParent<Meter>()
                            .SetGroupName("Stratum")
                            .AddConstructor<DumbMeter>();
    return tid;
}

void
DumbMeter::ApplyMeter(PolicyEntry& entry, double nowSeconds, uint32_t /* packetSize */)
{
    entry.arrivalTime = nowSeconds;
}

Colour
DumbMeter::DoApplyPolicer(PolicyEntry& /* entry */, uint32_t /* packetSize */)
{
    return Colour::GREEN;
}

} // namespace ns3::stratum
