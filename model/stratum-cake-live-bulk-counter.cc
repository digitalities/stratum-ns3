/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "stratum-cake-live-bulk-counter.h"

#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/string.h"

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
        // Default idle window: 8 x the inner's Interval. This is a Stratum
        // heuristic for deciding when a host's flow has gone quiet; sch_cake
        // has no time-window bulk threshold (its SPARSE/BULK classification is
        // the per-flow DRR-rotation transition), so this is not a Linux port.
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
    // Non-IP packets (e.g. ARP) have no 5-tuple to hash; collapse them to a
    // single bucket so they do not inflate the per-host live-flow count with
    // one unique hash each. GetUint8Value(IP_DSFIELD) is true for both IPv4 and
    // IPv6 items and false for non-IP items — the family-agnostic IP guard.
    uint8_t dsField;
    if (!item->GetUint8Value(QueueItem::IP_DSFIELD, dsField))
    {
        return 0;
    }
    // Ipv4QueueDiscItem / Ipv6QueueDiscItem both override Hash() with a Murmur3
    // hash over their respective 5-tuple (source, destination, protocol, source
    // port, destination port). One call, both families.
    return item->Hash();
}

} // namespace ns3::stratum::cake
