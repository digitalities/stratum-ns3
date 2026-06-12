/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "stratum-per-flow-policy-classifier.h"

#include "ns3/log.h"

#include <cstdio>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("PerFlowPolicyClassifier");

} // namespace ns3

namespace ns3::stratum::diffserv
{

NS_OBJECT_ENSURE_REGISTERED(PerFlowPolicyClassifier);

TypeId
PerFlowPolicyClassifier::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::diffserv::PerFlowPolicyClassifier")
                            .SetParent<Object>()
                            .SetGroupName("Stratum")
                            .AddConstructor<PerFlowPolicyClassifier>();
    return tid;
}

PerFlowPolicyClassifier::PerFlowPolicyClassifier()
    : m_meter(CreateObject<SrTcmMeter>())
{
}

PerFlowPolicyClassifier::~PerFlowPolicyClassifier() = default;

void
PerFlowPolicyClassifier::DoDispose()
{
    NS_LOG_FUNCTION(this);
    for (auto& kv : m_flows)
    {
        kv.second.meter = nullptr;
    }
    m_flows.clear();
    m_meter = nullptr;
    Object::DoDispose();
}

void
PerFlowPolicyClassifier::AddSrTcmRule(const FlowKey& key,
                                      uint8_t greenDscp,
                                      uint8_t yellowDscp,
                                      uint8_t redDscp,
                                      double cirBytesPerSec,
                                      double cbsBytes,
                                      double ebsBytes)
{
    Entry e;
    e.rule = SrTcmRule{greenDscp, yellowDscp, redDscp, cirBytesPerSec, cbsBytes, ebsBytes};
    e.state.meter = MeterType::SRTCM;
    e.state.cir = cirBytesPerSec;
    e.state.cbs = cbsBytes;
    e.state.ebs = ebsBytes;
    e.state.cBucket = cbsBytes;
    e.state.eBucket = ebsBytes;
    e.state.arrivalTime = 0.0;
    e.meter = m_meter;
    m_flows[key] = e;
}

uint8_t
PerFlowPolicyClassifier::ApplyPolicy(const FlowKey& key, uint32_t packetSize, double nowSeconds)
{
    auto it = m_flows.find(key);
    NS_ASSERT_MSG(it != m_flows.end(), "ApplyPolicy on unregistered flow");

    Entry& e = it->second;
    e.meter->ApplyMeter(e.state, nowSeconds, packetSize);
    Colour c = e.meter->ApplyPolicer(e.state, packetSize);
    switch (c)
    {
    case Colour::GREEN:
        return e.rule.greenDscp;
    case Colour::YELLOW:
        return e.rule.yellowDscp;
    case Colour::RED:
    default:
        return e.rule.redDscp;
    }
}

uint8_t
PerFlowPolicyClassifier::ApplyPolicyOrPassthrough(const FlowKey& key,
                                                  uint32_t packetSize,
                                                  double nowSeconds,
                                                  uint8_t incomingDscp)
{
    // Exact-match fast path: full 5-tuple (srcIp, srcPort, dstIp, dstPort,
    // proto).
    auto it = m_flows.find(key);
    if (it != m_flows.end())
    {
        return ApplyPolicy(key, packetSize, nowSeconds);
    }

    // Wildcard fallback: TCP client sockets bind ephemeral src ports that are
    // not known at rule-install time. Try the same tuple with srcPort = 0 —
    // the convention used by Helper::AddSrTcmMeterRule when the
    // caller cannot predict the source port. All TCP connections from the
    // same (srcIp, dstIp, dstPort, proto) then share one meter instance,
    // which matches the thesis's per-application-stream accounting where
    // each (server, client, port) pair is a single logical flow.
    FlowKey wildcard = key;
    wildcard.srcPort = 0;
    auto wIt = m_flows.find(wildcard);
    if (wIt != m_flows.end())
    {
        return ApplyPolicy(wildcard, packetSize, nowSeconds);
    }

    // Unknown flow -> preserve caller's DSCP (e.g. stamped by mark rules).
    return incomingDscp;
}

void
PerFlowPolicyClassifier::PrintRules() const
{
    std::printf("Per-flow policy rules: %zu flows\n", m_flows.size());
    for (const auto& [key, entry] : m_flows)
    {
        std::printf("  %s:%u -> %s:%u proto=%u  cir=%.0f cbs=%.0f ebs=%.0f\n",
                    "<ip>",
                    key.srcPort,
                    "<ip>",
                    key.dstPort,
                    key.proto,
                    entry.rule.cirBytesPerSec,
                    entry.rule.cbsBytes,
                    entry.rule.ebsBytes);
    }
}

} // namespace ns3::stratum::diffserv
