/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * Copyright (C) 2000 Nortel Networks
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsPolicy.h class Policy (2001).
 */

#include "stratum-meter.h"

#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::Meter");

} // namespace ns3

namespace ns3::stratum
{

NS_OBJECT_ENSURE_REGISTERED(Meter);

TypeId
Meter::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::stratum::Meter")
            .SetParent<Object>()
            .SetGroupName("Stratum")
            .AddAttribute(
                "L2OverheadBytes",
                "L2 framing overhead bytes per packet, added to "
                "the IP-layer packet size by metering subclasses "
                "(token-bucket / srTCM / trTCM / tsw2cm / tsw3cm) "
                "when charging tokens or measuring byte rate. "
                "PPP=2, Ethernet=14, SimpleLink=0. Default 0 "
                "preserves ns-2.35 SimpleLink parity. The "
                "companion Scheduler attribute MUST be set to "
                "the same value so meter and scheduler agree on "
                "byte basis.",
                UintegerValue(0),
                MakeUintegerAccessor(&Meter::SetL2OverheadBytes, &Meter::GetL2OverheadBytes),
                MakeUintegerChecker<uint32_t>(0, 64))
            .AddTraceSource("MeterColour",
                            "Fires on every colour decision: (colour, policyIndex, time). "
                            "All concrete meters in the registry emit this trace via the "
                            "base-class ApplyPolicer wrapper.",
                            MakeTraceSourceAccessor(&Meter::m_colourTrace),
                            "ns3::stratum::Meter::MeterColourTracedCallback");
    return tid;
}

Colour
Meter::ApplyPolicer(PolicyEntry& entry, uint32_t packetSize)
{
    const Colour c = DoApplyPolicer(entry, packetSize);
    NotifyColour(c, entry.policyIndex);
    return c;
}

void
Meter::NotifyColour(Colour colour, uint32_t classId)
{
    m_colourTrace(colour, classId, Simulator::Now());
}

} // namespace ns3::stratum
