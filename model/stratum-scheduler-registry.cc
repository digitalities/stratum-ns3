/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "stratum-scheduler-registry.h"

#include "stratum-llq-scheduler.h"
#include "stratum-pq-scheduler.h"
#include "stratum-rr-scheduler.h"
#include "stratum-scfq-scheduler.h"
#include "stratum-sfq-scheduler.h"
#include "stratum-wf2qp-scheduler.h"
#include "stratum-wfq-scheduler.h"
#include "stratum-wirr-scheduler.h"
#include "stratum-wrr-scheduler.h"

#include "ns3/abort.h"
#include "ns3/double.h"
#include "ns3/object-factory.h"
#include "ns3/uinteger.h"

#include <algorithm>

namespace ns3::stratum
{

namespace
{

void
ApplyWeights(Ptr<Scheduler> sched, const std::vector<double>& weights)
{
    for (uint32_t i = 0; i < weights.size(); ++i)
    {
        sched->SetParam(i, weights[i]);
    }
}

} // namespace

const char*
FamilyName(SchedulerEntry::Family f)
{
    switch (f)
    {
    case SchedulerEntry::Family::Priority:
        return "priority";
    case SchedulerEntry::Family::RoundRobin:
        return "round-robin";
    case SchedulerEntry::Family::FairQueue:
        return "fair-queue";
    case SchedulerEntry::Family::Hybrid:
        return "hybrid";
    }
    return "?";
}

const char*
ParameterShapeName(SchedulerEntry::ParameterShape s)
{
    switch (s)
    {
    case SchedulerEntry::ParameterShape::None:
        return "none";
    case SchedulerEntry::ParameterShape::PriorityWinLen:
        return "priority-winlen";
    case SchedulerEntry::ParameterShape::RoundRobinWeights:
        return "rr-weights";
    case SchedulerEntry::ParameterShape::FairQueueShares:
        return "fq-shares";
    case SchedulerEntry::ParameterShape::HybridLlq:
        return "hybrid-llq";
    }
    return "?";
}

void
SerialiseSchedulerEntry(std::ostream& os, const SchedulerEntry& e)
{
    os << "{"
       << "\"fileTag\": \"" << e.fileTag << "\", "
       << "\"displayName\": \"" << e.displayName << "\", "
       << "\"family\": \"" << FamilyName(e.family) << "\", "
       << "\"parameterShape\": \"" << ParameterShapeName(e.parameterShape) << "\", "
       << "\"needsLinkBandwidth\": " << (e.needsLinkBandwidth ? "true" : "false") << ", "
       << "\"description\": \"" << e.description << "\"}";
}

SchedulerRegistry::SchedulerRegistry()
{
    using F = SchedulerEntry::Family;
    using P = SchedulerEntry::ParameterShape;

    Register({"pq",
              "PQ",
              F::Priority,
              P::PriorityWinLen,
              false,
              "Strict priority — queue 0 served first; WinLen is the "
              "rate-estimator window for the optional per-queue rate cap",
              [](const SchedulerArgs& a) -> Ptr<Scheduler> {
                  return CreateObjectWithAttributes<PriorityScheduler>("NumQueues",
                                                                       UintegerValue(a.numQueues),
                                                                       "WinLen",
                                                                       DoubleValue(a.winLen));
              }});

    Register({"rr",
              "RR",
              F::RoundRobin,
              P::None,
              false,
              "Plain round-robin across all queues; no weights",
              [](const SchedulerArgs& a) -> Ptr<Scheduler> {
                  return CreateObjectWithAttributes<RoundRobinScheduler>(
                      "NumQueues",
                      UintegerValue(a.numQueues));
              }});

    Register({"wrr",
              "WRR",
              F::RoundRobin,
              P::RoundRobinWeights,
              false,
              "Weighted round-robin; per-queue integer-style weights via SetParam",
              [](const SchedulerArgs& a) -> Ptr<Scheduler> {
                  auto s = CreateObjectWithAttributes<WeightedRoundRobinScheduler>(
                      "NumQueues",
                      UintegerValue(a.numQueues));
                  ApplyWeights(s, a.weights);
                  return s;
              }});

    Register({"wirr",
              "WIRR",
              F::RoundRobin,
              P::RoundRobinWeights,
              false,
              "Weighted interleaved round-robin; per-queue integer weights",
              [](const SchedulerArgs& a) -> Ptr<Scheduler> {
                  auto s = CreateObjectWithAttributes<WeightedInterleavedRoundRobinScheduler>(
                      "NumQueues",
                      UintegerValue(a.numQueues));
                  ApplyWeights(s, a.weights);
                  return s;
              }});

    Register({"scfq",
              "SCFQ",
              F::FairQueue,
              P::FairQueueShares,
              true,
              "Self-clocked fair queueing; per-queue fractional weights summing to 1",
              [](const SchedulerArgs& a) -> Ptr<Scheduler> {
                  auto s = CreateObjectWithAttributes<ScfqScheduler>("NumQueues",
                                                                     UintegerValue(a.numQueues),
                                                                     "LinkBandwidth",
                                                                     DoubleValue(a.linkBps));
                  ApplyWeights(s, a.weights);
                  return s;
              }});

    Register({"sfq",
              "SFQ",
              F::FairQueue,
              P::FairQueueShares,
              true,
              "Start-time fair queueing; per-queue fractional weights summing to 1",
              [](const SchedulerArgs& a) -> Ptr<Scheduler> {
                  auto s = CreateObjectWithAttributes<SfqScheduler>("NumQueues",
                                                                    UintegerValue(a.numQueues),
                                                                    "LinkBandwidth",
                                                                    DoubleValue(a.linkBps));
                  ApplyWeights(s, a.weights);
                  return s;
              }});

    Register({"wfq",
              "WFQ",
              F::FairQueue,
              P::FairQueueShares,
              true,
              "Parekh-Gallager PGPS — true V(t) snapshot; per-queue fractional weights",
              [](const SchedulerArgs& a) -> Ptr<Scheduler> {
                  auto s = CreateObjectWithAttributes<WfqScheduler>("NumQueues",
                                                                    UintegerValue(a.numQueues),
                                                                    "LinkBandwidth",
                                                                    DoubleValue(a.linkBps));
                  ApplyWeights(s, a.weights);
                  return s;
              }});

    Register({"wf2qp",
              "WF2Q+",
              F::FairQueue,
              P::FairQueueShares,
              true,
              "Worst-case fair WFQ+ (Bennett-Zhang 1997, time-discrete); per-queue weights",
              [](const SchedulerArgs& a) -> Ptr<Scheduler> {
                  auto s = CreateObjectWithAttributes<Wf2qPlusScheduler>("NumQueues",
                                                                         UintegerValue(a.numQueues),
                                                                         "LinkBandwidth",
                                                                         DoubleValue(a.linkBps));
                  ApplyWeights(s, a.weights);
                  return s;
              }});

    Register({"llq",
              "LLQ",
              F::Hybrid,
              P::HybridLlq,
              true,
              "Cisco LLQ: queue 0 is strict-priority slot (weight=0 sentinel); "
              "queues 1..N share residual via WFQ-style weights summing to 1",
              [](const SchedulerArgs& a) -> Ptr<Scheduler> {
                  auto s = CreateObjectWithAttributes<LlqScheduler>("NumQueues",
                                                                    UintegerValue(a.numQueues),
                                                                    "LinkBandwidth",
                                                                    DoubleValue(a.linkBps));
                  ApplyWeights(s, a.weights);
                  return s;
              }});
}

const SchedulerRegistry&
SchedulerRegistry::Get()
{
    static const SchedulerRegistry kInstance;
    return kInstance;
}

Ptr<Scheduler>
SchedulerRegistry::Construct(const std::string& fileTag, const SchedulerArgs& args) const
{
    const SchedulerEntry* e = Find(fileTag);
    NS_ABORT_MSG_IF(!e, "SchedulerRegistry: unknown scheduler '" << fileTag << "'");
    return e->construct(args);
}

std::vector<std::string>
SchedulerRegistry::FileTags() const
{
    std::vector<std::string> out;
    out.reserve(m_entries.size());
    for (const auto& e : m_entries)
    {
        out.push_back(e.fileTag);
    }
    return out;
}

} // namespace ns3::stratum
