/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#include "stratum-helper.h"

#include "ns3/abort.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace ns3::stratum::diffserv
{

void
EnsureDir(const std::string& path)
{
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec)
    {
        NS_ABORT_MSG("EnsureDir: failed to create '" << path << "': " << ec.message());
    }
}

uint32_t
Helper::DetectL2OverheadBytes(Ptr<NetDevice> dev)
{
    // Charge wire bytes (IP + L2) so the FQ scheduler and policer-meter
    // reason in the byte basis the link physically consumes.
    // Default 0 covers any netdev whose L2 framing is either
    // zero (SimpleNetDevice) or variable per-packet and not summarisable
    // as a single scalar (Wifi/LTE/etc. — explicit attribute override).
    //
    // TypeId-name match avoids forcing a link dependency on the csma /
    // point-to-point modules, which could pull in transitive symbols
    // that the diffserv module otherwise does not need. When ns-3
    // mainline grows a `virtual NetDevice::GetL2OverheadBytes()`
    // accessor (see ),
    // this becomes a one-line dev->GetL2OverheadBytes().
    if (!dev)
    {
        return 0;
    }
    const std::string n = dev->GetInstanceTypeId().GetName();
    if (n == "ns3::PointToPointNetDevice")
    {
        return 2; // PppHeader: 2-byte protocol field per packet.
    }
    if (n == "ns3::CsmaNetDevice")
    {
        return 14; // Ethernet header: DA + SA + EtherType.
    }
    return 0;
}

// The shared-configuration helpers dispatch by concrete type: direct
// RedQueueDisc, or one of the composers (edge/core) that expose the
// same forwarder surface. Calling the forwarder rather than
// unwrapping preserves the composer's deferred inner-disc
// materialisation.
#define STRATUM_DISPATCH_OR_ABORT(method, ...)                                                     \
    do                                                                                             \
    {                                                                                              \
        if (auto ds = DynamicCast<RedQueueDisc>(disc))                                             \
        {                                                                                          \
            ds->method(__VA_ARGS__);                                                               \
            return;                                                                                \
        }                                                                                          \
        if (auto e = DynamicCast<EdgeQueueDisc>(disc))                                             \
        {                                                                                          \
            e->method(__VA_ARGS__);                                                                \
            return;                                                                                \
        }                                                                                          \
        if (auto c = DynamicCast<CoreQueueDisc>(disc))                                             \
        {                                                                                          \
            c->method(__VA_ARGS__);                                                                \
            return;                                                                                \
        }                                                                                          \
        NS_ABORT_MSG("Helper::" #method ": unsupported queue disc type");                          \
    } while (false)

Helper::Helper()
{
}

void
Helper::AddMarkRule(Ptr<EdgeQueueDisc> edge,
                    uint8_t dscp,
                    int32_t srcAddr,
                    int32_t dstAddr,
                    uint8_t protocol,
                    uint32_t appType)
{
    MarkRule rule;
    rule.dscp = dscp;
    rule.srcAddr = srcAddr;
    rule.dstAddr = dstAddr;
    rule.protocol = protocol;
    rule.appType = appType;
    edge->AddMarkRule(rule);
}

void
Helper::AddMarkRuleWithPorts(Ptr<EdgeQueueDisc> edge,
                             uint8_t dscp,
                             int32_t srcAddr,
                             int32_t dstAddr,
                             uint8_t protocol,
                             uint32_t appType,
                             uint16_t srcPort,
                             uint16_t dstPort)
{
    MarkRule rule;
    rule.dscp = dscp;
    rule.srcAddr = srcAddr;
    rule.dstAddr = dstAddr;
    rule.protocol = protocol;
    rule.appType = appType;
    rule.srcPort = srcPort;
    rule.dstPort = dstPort;
    edge->AddMarkRule(rule);
}

void
Helper::AddDumbPolicy(Ptr<EdgeQueueDisc> edge, uint8_t codePt)
{
    PolicyEntry pe;
    pe.codePoint = codePt;
    pe.meter = MeterType::DUMB;
    pe.policer = PolicerType::DUMB;
    pe.policyIndex = static_cast<uint32_t>(MeterType::DUMB);
    edge->GetPolicyClassifier()->AddPolicyEntry(pe);
}

void
Helper::AddTokenBucketPolicy(Ptr<EdgeQueueDisc> edge,
                             uint8_t codePt,
                             double cirBps,
                             double cbsBytes)
{
    PolicyEntry pe;
    pe.codePoint = codePt;
    pe.meter = MeterType::TOKEN_BUCKET;
    pe.policer = PolicerType::TOKEN_BUCKET;
    pe.policyIndex = static_cast<uint32_t>(MeterType::TOKEN_BUCKET);
    pe.cir = cirBps / 8.0;
    pe.cbs = cbsBytes;
    pe.cBucket = cbsBytes;
    edge->GetPolicyClassifier()->AddPolicyEntry(pe);
}

void
Helper::AddSrTcmPolicy(Ptr<EdgeQueueDisc> edge,
                       uint8_t codePt,
                       double cirBps,
                       double cbsBytes,
                       double ebsBytes)
{
    PolicyEntry pe;
    pe.codePoint = codePt;
    pe.meter = MeterType::SRTCM;
    pe.policer = PolicerType::SRTCM;
    pe.policyIndex = static_cast<uint32_t>(MeterType::SRTCM);
    pe.cir = cirBps / 8.0;
    pe.cbs = cbsBytes;
    pe.ebs = ebsBytes;
    pe.cBucket = cbsBytes;
    pe.eBucket = ebsBytes;
    edge->GetPolicyClassifier()->AddPolicyEntry(pe);
}

void
Helper::AddSrTcmMeterRule(Ptr<EdgeQueueDisc> edge,
                          Ipv4Address srcIp,
                          uint16_t srcPort,
                          Ipv4Address dstIp,
                          uint16_t dstPort,
                          uint8_t proto,
                          uint8_t greenDscp,
                          uint8_t yellowDscp,
                          uint8_t redDscp,
                          double cirBps,
                          double cbsBytes,
                          double ebsBytes)
{
    Ptr<PerFlowPolicyClassifier> pf = edge->GetPerFlowClassifier();
    if (!pf)
    {
        pf = CreateObject<PerFlowPolicyClassifier>();
        edge->SetPerFlowClassifier(pf);
    }

    FlowKey key{srcIp, srcPort, dstIp, dstPort, proto};
    double cirBytesPerSec = cirBps / 8.0;
    pf->AddSrTcmRule(key, greenDscp, yellowDscp, redDscp, cirBytesPerSec, cbsBytes, ebsBytes);
}

void
Helper::AddTrTcmPolicy(Ptr<EdgeQueueDisc> edge,
                       uint8_t codePt,
                       double cirBps,
                       double cbsBytes,
                       double pirBps,
                       double pbsBytes)
{
    PolicyEntry pe;
    pe.codePoint = codePt;
    pe.meter = MeterType::TRTCM;
    pe.policer = PolicerType::TRTCM;
    pe.policyIndex = static_cast<uint32_t>(MeterType::TRTCM);
    pe.cir = cirBps / 8.0;
    pe.pir = pirBps / 8.0;
    pe.cbs = cbsBytes;
    pe.pbs = pbsBytes;
    pe.cBucket = cbsBytes;
    pe.pBucket = pbsBytes;
    edge->GetPolicyClassifier()->AddPolicyEntry(pe);
}

void
Helper::AddTsw2cmPolicy(Ptr<EdgeQueueDisc> edge, uint8_t codePt, double cirBps)
{
    PolicyEntry pe;
    pe.codePoint = codePt;
    pe.meter = MeterType::TSW2CM;
    pe.policer = PolicerType::TSW2CM;
    pe.policyIndex = static_cast<uint32_t>(MeterType::TSW2CM);
    pe.cir = cirBps / 8.0;
    edge->GetPolicyClassifier()->AddPolicyEntry(pe);
}

void
Helper::AddTsw3cmPolicy(Ptr<EdgeQueueDisc> edge, uint8_t codePt, double cirBps, double pirBps)
{
    PolicyEntry pe;
    pe.codePoint = codePt;
    pe.meter = MeterType::TSW3CM;
    pe.policer = PolicerType::TSW3CM;
    pe.policyIndex = static_cast<uint32_t>(MeterType::TSW3CM);
    pe.cir = cirBps / 8.0;
    pe.pir = pirBps / 8.0;
    edge->GetPolicyClassifier()->AddPolicyEntry(pe);
}

void
Helper::AddPolicerEntry(Ptr<EdgeQueueDisc> edge,
                        PolicerType policer,
                        int initialCodePt,
                        int downgrade1,
                        int downgrade2)
{
    PolicerEntry pe;
    pe.policer = policer;
    pe.policyIndex = static_cast<uint32_t>(policer);
    pe.initialCodePt = initialCodePt;
    pe.downgrade1 = downgrade1;
    pe.downgrade2 = downgrade2;
    edge->GetPolicyClassifier()->AddPolicerEntry(pe);
}

// Shape-A helpers take `Ptr<RedQueueDisc>` directly. Callers that
// hold `Ptr<EdgeQueueDisc>` / `Ptr<CoreQueueDisc>`
// obtain the inner via `helper.InstallRedInner(...)` (terse) or
// `DynamicCast<RedQueueDisc>(edge->GetInnerDisc())` (explicit).
// `STRATUM_DISPATCH_OR_ABORT` is intentionally not used here so the
// type-narrowing happens at the call site rather than as a runtime
// branch inside the helper.

void
Helper::AddPhbEntry(Ptr<RedQueueDisc> disc, uint8_t codePt, uint8_t queue, uint8_t prec)
{
    disc->AddPhbEntry(codePt, queue, prec);
}

void
Helper::SetScheduler(Ptr<RedQueueDisc> disc, Ptr<Scheduler> scheduler)
{
    disc->SetScheduler(scheduler);
}

void
Helper::ConfigQueue(Ptr<RedQueueDisc> disc,
                    uint32_t queue,
                    uint32_t prec,
                    double thMin,
                    double thMax,
                    double maxP)
{
    disc->ConfigQueue(queue, prec, thMin, thMax, maxP);
}

void
Helper::SetMredMode(Ptr<RedQueueDisc> disc, MredMode mode, uint32_t queue)
{
    disc->SetMredMode(mode, queue);
}

Ptr<RedQueueDisc>
Helper::InstallRedInner(Ptr<EdgeQueueDisc> edge)
{
    Ptr<RedQueueDisc> inner = CreateObject<RedQueueDisc>();
    edge->SetInnerDisc(inner);
    return inner;
}

Ptr<RedQueueDisc>
Helper::InstallRedInner(Ptr<CoreQueueDisc> core)
{
    Ptr<RedQueueDisc> inner = CreateObject<RedQueueDisc>();
    core->SetInnerDisc(inner);
    return inner;
}

} // namespace ns3::stratum::diffserv
