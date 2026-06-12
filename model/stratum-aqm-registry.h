/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Single source of truth for the AQM cells exposed by aqm-eval-runner
 * (and any future evaluation harness in this directory).  Each entry
 * carries its dispatch name, a file-tag for output filenames, a family
 * label that matches the Python plotting scripts, and a factory closure
 * that knows how to construct the queue disc with mode-specific
 * configuration.
 *
 * Pattern adapted from the GSoC 2025 ns-3 AQM Evaluation Suite work
 * (David Lin, mentored by M.P. Tahiliani / A. Singh / T. Henderson):
 * a centralised registry replaces hard-coded enumeration in the runner,
 * the helper, and the plotting code.
 *
 * This registry is the first instantiation of the Registry<EntryT>
 * template (ds-registry.h). Future instantiations cover schedulers and
 * — opportunistically — meters.
 */
#ifndef NS3_STRATUM_AQM_REGISTRY_H
#define NS3_STRATUM_AQM_REGISTRY_H

#include "stratum-registry.h"

#include "ns3/data-rate.h"
#include "ns3/ptr.h"
#include "ns3/queue-disc.h"

#include <functional>
#include <ostream>
#include <string>

namespace ns3::stratum
{
namespace aqm_eval
{

struct AqmEntry
{
    enum class Family
    {
        Single, // PfifoFast / Red / AdaptiveRed / CoDel / Pie / Cobalt
        Fq,     // FqCoDel / FqPie / FqCobalt
        Stratum // StratumRed / StratumL4sWred / StratumL4sCoupledOnly / StratumCake
    };

    std::string name;        // dispatch key passed via --aqm=
    std::string fileTag;     // sanitised name for output filenames
    std::string displayName; // pretty name for --aqm=list
    Family family;
    bool supportsEcn;
    std::function<Ptr<QueueDisc>(DataRate)> factory;
};

const char* FamilyName(AqmEntry::Family f);

/// Domain-specific JSON serialiser for one AQM row. Writes a
/// brace-delimited JSON object with no trailing newline or comma; the
/// caller manages array delimiters and outer structure.
void SerialiseAqmEntry(std::ostream& os, const AqmEntry& e);

class AqmRegistry : public Registry<AqmEntry>
{
  public:
    static const AqmRegistry& Get();

    /// Look up an entry by its dispatch `name` (the value passed via
    /// `--aqm=`). Returns nullptr if not found. Distinct from the
    /// template's Find(), which matches against `fileTag`.
    const AqmEntry* FindByName(const std::string& name) const;

    /// All dispatch names, in registration order.
    std::vector<std::string> Names() const;

    /// Construct a queue disc by dispatch `name`. Aborts if not found.
    Ptr<QueueDisc> Make(const std::string& name, DataRate totalRate) const;

  private:
    AqmRegistry();
};

} // namespace aqm_eval
} // namespace ns3::stratum

#endif // NS3_STRATUM_AQM_REGISTRY_H
