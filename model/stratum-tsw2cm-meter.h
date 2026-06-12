/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * Copyright (C) 2000 Nortel Networks
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsPolicy.cc TSW2CMPolicy (2001).
 */

#ifndef NS3_STRATUM_TSW2CM_METER_H
#define NS3_STRATUM_TSW2CM_METER_H

#include "stratum-meter.h"

#include "ns3/random-variable-stream.h"

namespace ns3::stratum
{

/**
 * @ingroup stratum
 *
 * @brief Time Sliding Window Two Colour Marker (RFC 2859, two-colour variant).
 *
 * Uses an EWMA rate estimator. When the estimated rate exceeds CIR,
 * packets are marked RED with probability (avgRate - CIR) / avgRate.
 * Below CIR, all packets are GREEN.
 *
 * Reference: dsPolicy.cc TSW2CMPolicy::applyMeter (line 515) and
 * TSW2CMPolicy::applyPolicer (line 536).
 *
 */
class Tsw2cmMeter : public Meter
{
  public:
    /**
     * @brief Get the TypeId for this class.
     * @return the TypeId.
     */
    static TypeId GetTypeId();

    /** @brief Construct a Tsw2cmMeter. */
    Tsw2cmMeter();

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

#endif // NS3_STRATUM_TSW2CM_METER_H
