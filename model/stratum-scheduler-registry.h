/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Centralised registry for the nine scheduler cells the Stratum substrate
 * exposes (PQ / RR / WRR / WIRR / SCFQ / SFQ / WFQ / WF2Q+ / LLQ).  Each
 * entry carries its canonical name, family classification, parameter
 * shape, and a typed factory closure that builds a fully-attribute-set
 * Scheduler from a SchedulerArgs struct.
 *
 * The registry is the second instantiation of Registry<EntryT>; it
 * collapses the if/else chains previously duplicated in
 * diffserv-wifi-scheduler-comparison.cc and chang-comparison.cc into
 * one Register() call per scheduler.
 */
#ifndef NS3_STRATUM_SCHEDULER_REGISTRY_H
#define NS3_STRATUM_SCHEDULER_REGISTRY_H

#include "stratum-registry.h"
#include "stratum-scheduler.h"

#include "ns3/ptr.h"

#include <cstdint>
#include <functional>
#include <ostream>
#include <string>
#include <vector>

namespace ns3::stratum
{

/// Construction-time arguments common to every scheduler entry. Each
/// per-entry closure pulls the subset its scheduler actually uses and
/// ignores the rest. Defaults match the "no-op" interpretation: zero
/// linkBps, empty weights, winLen=1.
struct SchedulerArgs
{
    uint32_t numQueues = 0;
    double linkBps = 0.0;
    std::vector<double> weights;
    double winLen = 1.0;
};

struct SchedulerEntry
{
    enum class Family
    {
        Priority,   // PQ
        RoundRobin, // RR / WRR / WIRR
        FairQueue,  // SCFQ / SFQ / WFQ / WF2Q+
        Hybrid      // LLQ (PQ + WFQ)
    };

    /// The metadata axis that downstream consumers (CLI help, manifest
    /// dump, handbook tables) read to decide what arguments matter for
    /// each scheduler. Mutually exclusive: each entry has exactly one
    /// shape.
    enum class ParameterShape
    {
        None,              // RR
        PriorityWinLen,    // PQ (winLen, no per-queue weights)
        RoundRobinWeights, // WRR / WIRR (per-queue weights, no linkBps)
        FairQueueShares,   // SCFQ / SFQ / WFQ / WF2Q+ (linkBps + weights)
        HybridLlq          // LLQ (linkBps + weights with [0]=sentinel)
    };

    std::string fileTag;     // canonical lowercase name (pq, rr, ...)
    std::string displayName; // pretty form for catalogues (PQ, WFQ, WF2Q+)
    Family family;
    ParameterShape parameterShape;
    bool needsLinkBandwidth;
    std::string description;

    std::function<Ptr<Scheduler>(const SchedulerArgs&)> construct;
};

const char* FamilyName(SchedulerEntry::Family f);
const char* ParameterShapeName(SchedulerEntry::ParameterShape s);

/// Domain-specific JSON serialiser for one scheduler row. Caller-managed
/// array delimiters and outer object structure (matches the
/// SerialiseAqmEntry contract).
void SerialiseSchedulerEntry(std::ostream& os, const SchedulerEntry& e);

class SchedulerRegistry : public Registry<SchedulerEntry>
{
  public:
    static const SchedulerRegistry& Get();

    /// Build a scheduler by canonical fileTag. Aborts on miss. The
    /// closure populates NumQueues + (optionally) LinkBandwidth +
    /// WinLen via attribute construction and applies per-queue weights
    /// via SetParam.
    Ptr<Scheduler> Construct(const std::string& fileTag, const SchedulerArgs& args) const;

    /// All canonical fileTags, in registration order.
    std::vector<std::string> FileTags() const;

  private:
    SchedulerRegistry();
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_SCHEDULER_REGISTRY_H
