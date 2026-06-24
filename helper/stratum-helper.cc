/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#include "stratum-helper.h"

#include "ns3/abort.h"
#include "ns3/double.h"
#include "ns3/object-factory.h"
#include "ns3/stratum-pq-scheduler.h"
#include "ns3/uinteger.h"

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
Helper::AddTokenBucketPolicy(Ptr<EdgeQueueDisc> edge, const TokenBucketPolicySpec& p)
{
    PolicyEntry pe;
    pe.codePoint = p.codePt;
    pe.meter = MeterType::TOKEN_BUCKET;
    pe.policer = PolicerType::TOKEN_BUCKET;
    pe.policyIndex = static_cast<uint32_t>(MeterType::TOKEN_BUCKET);
    pe.cir = p.cirBps / 8.0;
    pe.cbs = p.cbsBytes;
    pe.cBucket = p.cbsBytes;
    edge->GetPolicyClassifier()->AddPolicyEntry(pe);
}

void
Helper::AddSrTcmPolicy(Ptr<EdgeQueueDisc> edge, const SrTcmPolicySpec& p)
{
    PolicyEntry pe;
    pe.codePoint = p.codePt;
    pe.meter = MeterType::SRTCM;
    pe.policer = PolicerType::SRTCM;
    pe.policyIndex = static_cast<uint32_t>(MeterType::SRTCM);
    pe.cir = p.cirBps / 8.0;
    pe.cbs = p.cbsBytes;
    pe.ebs = p.ebsBytes;
    pe.cBucket = p.cbsBytes;
    pe.eBucket = p.ebsBytes;
    edge->GetPolicyClassifier()->AddPolicyEntry(pe);
}

void
Helper::AddSrTcmMeterRule(Ptr<EdgeQueueDisc> edge, const SrTcmMeterRuleSpec& rule)
{
    Ptr<PerFlowPolicyClassifier> pf = edge->GetPerFlowClassifier();
    if (!pf)
    {
        pf = CreateObject<PerFlowPolicyClassifier>();
        edge->SetPerFlowClassifier(pf);
    }

    FlowKey key{rule.srcIp, rule.srcPort, rule.dstIp, rule.dstPort, rule.proto};
    double cirBytesPerSec = rule.cirBps / 8.0;
    pf->AddSrTcmRule(key,
                     rule.greenDscp,
                     rule.yellowDscp,
                     rule.redDscp,
                     cirBytesPerSec,
                     rule.cbsBytes,
                     rule.ebsBytes);
}

void
Helper::AddTrTcmPolicy(Ptr<EdgeQueueDisc> edge, const TrTcmPolicySpec& p)
{
    PolicyEntry pe;
    pe.codePoint = p.codePt;
    pe.meter = MeterType::TRTCM;
    pe.policer = PolicerType::TRTCM;
    pe.policyIndex = static_cast<uint32_t>(MeterType::TRTCM);
    pe.cir = p.cirBps / 8.0;
    pe.pir = p.pirBps / 8.0;
    pe.cbs = p.cbsBytes;
    pe.pbs = p.pbsBytes;
    pe.cBucket = p.cbsBytes;
    pe.pBucket = p.pbsBytes;
    edge->GetPolicyClassifier()->AddPolicyEntry(pe);
}

void
Helper::AddTsw2cmPolicy(Ptr<EdgeQueueDisc> edge, const Tsw2cmPolicySpec& p)
{
    PolicyEntry pe;
    pe.codePoint = p.codePt;
    pe.meter = MeterType::TSW2CM;
    pe.policer = PolicerType::TSW2CM;
    pe.policyIndex = static_cast<uint32_t>(MeterType::TSW2CM);
    pe.cir = p.cirBps / 8.0;
    pe.winLen = p.winLenSeconds;
    edge->GetPolicyClassifier()->AddPolicyEntry(pe);
}

void
Helper::AddTsw3cmPolicy(Ptr<EdgeQueueDisc> edge, const Tsw3cmPolicySpec& p)
{
    PolicyEntry pe;
    pe.codePoint = p.codePt;
    pe.meter = MeterType::TSW3CM;
    pe.policer = PolicerType::TSW3CM;
    pe.policyIndex = static_cast<uint32_t>(MeterType::TSW3CM);
    pe.cir = p.cirBps / 8.0;
    pe.pir = p.pirBps / 8.0;
    pe.winLen = p.winLenSeconds;
    edge->GetPolicyClassifier()->AddPolicyEntry(pe);
}

void
Helper::AddPolicerEntry(Ptr<EdgeQueueDisc> edge, const PolicerEntry& entry)
{
    edge->GetPolicyClassifier()->AddPolicerEntry(entry);
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
Helper::ConfigQueue(Ptr<RedQueueDisc> disc, const RedQueueConfig& cfg)
{
    disc->ConfigQueue(cfg);
}

void
Helper::SetMredMode(Ptr<RedQueueDisc> disc, MredMode mode, uint32_t queue)
{
    disc->SetMredMode(mode, queue);
}

void
Helper::SetMredModeAllQueues(Ptr<RedQueueDisc> disc, MredMode mode)
{
    disc->SetMredModeAllQueues(mode);
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

void
Helper::SetAsDiffserv(Ptr<EdgeQueueDisc> edge, const DiffservSpec& spec)
{
    Helper helper;
    Ptr<RedQueueDisc> inner = helper.InstallRedInner(edge);

    const uint32_t numQueues = (spec.profile == Profile::ExpeditedForwarding) ? 2u : 1u;
    inner->SetNumQueues(numQueues);
    for (uint32_t q = 0; q < numQueues; ++q)
    {
        inner->SetNumPrec(q, 1);
    }

    switch (spec.profile)
    {
    case Profile::ExpeditedForwarding:
        inner->AddPhbEntry(46, 0, 0); // EF -> priority lane (queue 0)
        inner->AddPhbEntry(0, 1, 0);  // best-effort -> queue 1
        break;
    case Profile::BestEffort:
        inner->AddPhbEntry(0, 0, 0); // best-effort -> the single queue
        break;
    }

    Ptr<Scheduler> scheduler = spec.scheduler;
    if (!scheduler)
    {
        scheduler = CreateObjectWithAttributes<PriorityScheduler>("NumQueues",
                                                                  UintegerValue(numQueues),
                                                                  "WinLen",
                                                                  DoubleValue(1.0));
    }
    inner->SetScheduler(scheduler);
}

} // namespace ns3::stratum::diffserv
