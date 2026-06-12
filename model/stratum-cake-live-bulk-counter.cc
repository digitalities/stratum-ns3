/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "stratum-cake-live-bulk-counter.h"

#include "ns3/ipv4-header.h"
#include "ns3/ipv4-queue-disc-item.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/string.h"
#include "ns3/tcp-header.h"
#include "ns3/udp-header.h"

namespace ns3::stratum::cake
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::cake::LiveBulkCounter");

NS_OBJECT_ENSURE_REGISTERED(LiveBulkCounter);

TypeId
LiveBulkCounter::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::cake::LiveBulkCounter")
                            .SetParent<Object>()
                            .SetGroupName("Stratum")
                            .AddConstructor<LiveBulkCounter>();
    return tid;
}

LiveBulkCounter::LiveBulkCounter() = default;

void
LiveBulkCounter::Attach(Ptr<QueueDisc> inner, Time idleWindow)
{
    NS_LOG_FUNCTION(this << inner << idleWindow);
    NS_ASSERT_MSG(inner, "Attach requires non-null inner disc");

    if (idleWindow.IsZero())
    {
        // Default: 8 x inner's Interval (matches Linux bulk_flow_threshold).
        // FqCobaltQueueDisc stores Interval as a StringValue (e.g. "100ms").
        StringValue intervalStr;
        if (inner->GetAttributeFailSafe("Interval", intervalStr))
        {
            m_idleWindow = Time(intervalStr.Get()) * 8;
        }
        else
        {
            m_idleWindow = MilliSeconds(800); // fallback: 8 x 100 ms default
        }
    }
    else
    {
        m_idleWindow = idleWindow;
    }

    inner->TraceConnectWithoutContext("Enqueue",
                                      MakeCallback(&LiveBulkCounter::OnEnqueueTrace, this));
}

void
LiveBulkCounter::OnEnqueueTrace(Ptr<const QueueDiscItem> item)
{
    const uint64_t hash = FlowHashFromItem(item);
    m_lastSeen[hash] = Simulator::Now();
}

uint32_t
LiveBulkCounter::GetLiveCount(Time now)
{
    uint32_t live = 0;
    for (auto it = m_lastSeen.begin(); it != m_lastSeen.end();)
    {
        if (it->second + m_idleWindow <= now)
        {
            it = m_lastSeen.erase(it);
        }
        else
        {
            ++live;
            ++it;
        }
    }
    return live;
}

uint64_t
LiveBulkCounter::FlowHashFromItem(Ptr<const QueueDiscItem> item)
{
    Ptr<const Ipv4QueueDiscItem> ipv4Item = DynamicCast<const Ipv4QueueDiscItem>(item);
    if (!ipv4Item)
    {
        // Non-IPv4: treat all such packets as a single bulk flow so they
        // do not inflate the per-packet count by one unique hash each.
        return 0;
    }

    const Ipv4Header& ip = ipv4Item->GetHeader();
    uint64_t h = (static_cast<uint64_t>(ip.GetSource().Get()) << 32) |
                 static_cast<uint64_t>(ip.GetDestination().Get());

    // Copy the packet so header removal does not mutate the original.
    Ptr<Packet> pkt = ipv4Item->GetPacket()->Copy();
    if (ip.GetProtocol() == 17)
    {
        UdpHeader udp;
        pkt->RemoveHeader(udp);
        h ^= (static_cast<uint64_t>(udp.GetSourcePort()) << 16) |
             static_cast<uint64_t>(udp.GetDestinationPort());
    }
    else if (ip.GetProtocol() == 6)
    {
        TcpHeader tcp;
        pkt->RemoveHeader(tcp);
        h ^= (static_cast<uint64_t>(tcp.GetSourcePort()) << 16) |
             static_cast<uint64_t>(tcp.GetDestinationPort());
    }
    h ^= static_cast<uint64_t>(ip.GetProtocol());
    return h;
}

} // namespace ns3::stratum::cake
