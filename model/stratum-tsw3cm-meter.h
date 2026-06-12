/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * Copyright (C) 2000 Nortel Networks
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsPolicy.cc TSW3CMPolicy (2001).
 */

#ifndef NS3_STRATUM_TSW3CM_METER_H
#define NS3_STRATUM_TSW3CM_METER_H

#include "stratum-meter.h"

#include "ns3/random-variable-stream.h"

namespace ns3::stratum
{

/**
 * @ingroup stratum
 *
 * @brief Time Sliding Window Three Colour Marker (RFC 2859, three-colour variant).
 *
 * Uses the same EWMA rate estimator as Tsw2cmMeter. The policer
 * generates a random value in (0, avgRate] and compares against CIR
 * and PIR thresholds:
 * - rand > PIR -> RED
 * - rand > CIR -> YELLOW
 * - rand <= CIR -> GREEN
 *
 * Reference: dsPolicy.cc TSW3CMPolicy::applyMeter (line 558) and
 * TSW3CMPolicy::applyPolicer (line 589).
 *
 */
class Tsw3cmMeter : public Meter
{
  public:
    /**
     * @brief Get the TypeId for this class.
     * @return the TypeId.
     */
    static TypeId GetTypeId();

    /** @brief Construct a Tsw3cmMeter. */
    Tsw3cmMeter();

    void ApplyMeter(PolicyEntry& entry, double nowSeconds, uint32_t packetSize) override;

    /**
     * @brief Assign a fixed RNG stream for reproducible testing.
     * @param stream the stream index to assign
     * @return the number of streams consumed (always 1)
     */
    int64_t AssignStreams(int64_t stream);

  protected:
    Colour DoApplyPolicer(PolicyEntry& entry, uint32_t packetSize) override;

  private:
    Ptr<UniformRandomVariable> m_uv; //!< Probabilistic mark RNG
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_TSW3CM_METER_H
