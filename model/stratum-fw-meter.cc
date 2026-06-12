/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * Copyright (C) 2000 Nortel Networks
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Line-for-line translation of DiffServ4NS dsPolicy.cc FWPolicy (2001).
 * applyMeter (line 808) and applyPolicer (line 876).
 */

#include "stratum-fw-meter.h"

#include "ns3/double.h"
#include "ns3/log.h"

#include <vector>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::FWMeter");

} // namespace ns3

namespace ns3::stratum
{

NS_OBJECT_ENSURE_REGISTERED(FWMeter);

TypeId
FWMeter::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::FWMeter")
                            .SetParent<Meter>()
                            .SetGroupName("Stratum")
                            .AddConstructor<FWMeter>();
    return tid;
}

FWMeter::FWMeter()
    : m_rng(CreateObject<UniformRandomVariable>())
{
    m_rng->SetAttribute("Min", DoubleValue(0.0));
    m_rng->SetAttribute("Max", DoubleValue(1.0));
}

// ---------------------------------------------------------------------------
//  Base class overrides (interface compatibility)
// ---------------------------------------------------------------------------

void
FWMeter::ApplyMeter(PolicyEntry& entry, double nowSeconds, uint32_t packetSize)
{
    // Default single-flow mode: all traffic is flow 0.
    ApplyMeterWithFlowId(entry, nowSeconds, packetSize, 0);
}

Colour
FWMeter::DoApplyPolicer(PolicyEntry& entry, uint32_t packetSize)
{
    // Simple threshold check for interface compatibility.
    // Flow 0 is the default single-flow bucket.
    auto it = m_flowTable.find(0);
    if (it != m_flowTable.end() && it->second.bytesSent > entry.cir)
    {
        return Colour::YELLOW;
    }
    return Colour::GREEN;
}

// ---------------------------------------------------------------------------
//  Extended API with explicit flow ID
// ---------------------------------------------------------------------------

void
FWMeter::ApplyMeterWithFlowId(PolicyEntry& entry,
                              double nowSeconds,
                              uint32_t packetSize,
                              uint32_t flowId)
{
    // dsPolicy.cc line 808-869: maintain per-flow byte accounting.
    // Purge expired flows first (matches ns-2 inline purge during traversal).
    PurgeExpiredFlows(nowSeconds);

    auto it = m_flowTable.find(flowId);
    if (it != m_flowTable.end())
    {
        // Flow already recorded: update timestamp and byte count.
        it->second.lastUpdate = nowSeconds;
        it->second.bytesSent += packetSize;
    }
    else
    {
        // First time this flow appears: create new entry.
        FlowEntry fe;
        fe.flowId = flowId;
        fe.lastUpdate = nowSeconds;
        fe.bytesSent = packetSize;
        fe.count = 0;
        m_flowTable[flowId] = fe;
    }
}

int
FWMeter::ApplyPolicerFw(PolicyEntry& entry,
                        PolicerEntry& policer,
                        uint32_t /* packetSize */,
                        uint32_t flowId)
{
    // dsPolicy.cc line 876-931: policing decision per flow.
    auto it = m_flowTable.find(flowId);
    if (it == m_flowTable.end())
    {
        // Flow not found (should not happen if ApplyMeterWithFlowId was called).
        NS_LOG_WARN("FW: no flow " << flowId << " in the table");
        return policer.initialCodePt;
    }

    FlowEntry& fe = it->second;

    if (fe.bytesSent > static_cast<uint64_t>(entry.cir))
    {
        // Excess flow: apply penalty based on downgrade2 mode.
        if (policer.downgrade2 == 0)
        {
            // Mode 0 — deterministic: every excess packet is downgraded.
            return policer.downgrade1;
        }
        else if (policer.downgrade2 == 1)
        {
            // Mode 1 — probabilistic: P(GREEN) = CIR / bytesSent.
            // dsPolicy.cc line 895: Random::uniform(0,1) > (1 - cir/bytes_sent)
            double prob = 1.0 - (entry.cir / static_cast<double>(fe.bytesSent));
            if (m_rng->GetValue() > prob)
            {
                return policer.initialCodePt;
            }
            else
            {
                return policer.downgrade1;
            }
        }
        else
        {
            // Mode 2 — periodic: 1-in-6 cycle (5 downgrades, then 1 green).
            // dsPolicy.cc line 904: count == 5 → reset, return initial.
            if (fe.count == 5)
            {
                fe.count = 0;
                return policer.initialCodePt;
            }
            else
            {
                fe.count++;
                return policer.downgrade1;
            }
        }
    }
    else
    {
        // In-profile: return initial code point.
        return policer.initialCodePt;
    }
}

int64_t
FWMeter::AssignStreams(int64_t stream)
{
    m_rng->SetStream(stream);
    return 1;
}

// ---------------------------------------------------------------------------
//  Private helpers
// ---------------------------------------------------------------------------

void
FWMeter::PurgeExpiredFlows(double nowSeconds)
{
    // dsPolicy.cc line 826: remove flows with last_update + FLOW_TIME_OUT < now.
    // Collect keys to erase (avoid modifying map during iteration).
    std::vector<uint32_t> expired;
    for (const auto& kv : m_flowTable)
    {
        if (kv.second.lastUpdate + kFlowTimeoutSeconds < nowSeconds)
        {
            expired.push_back(kv.first);
        }
    }
    for (uint32_t key : expired)
    {
        m_flowTable.erase(key);
    }
}

} // namespace ns3::stratum
