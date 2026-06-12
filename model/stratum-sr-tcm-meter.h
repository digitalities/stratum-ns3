/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * Copyright (C) 2000 Nortel Networks
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsPolicy.cc SRTCMPolicy (2001).
 */

#ifndef NS3_STRATUM_SR_TCM_METER_H
#define NS3_STRATUM_SR_TCM_METER_H

#include "stratum-meter.h"

namespace ns3::stratum
{

/**
 * @ingroup stratum
 *
 * @brief Single Rate Three Colour Marker (RFC 2697).
 *
 * One rate (CIR) fills cBucket; overflow spills into eBucket.
 * Colour-blind mode: GREEN if cBucket >= size, YELLOW if eBucket >= size,
 * RED otherwise. Only the bucket that satisfies the test is decremented.
 *
 * Reference: dsPolicy.cc SRTCMPolicy::applyMeter (line 664) and
 * SRTCMPolicy::applyPolicer (line 697).
 *
 */
class SrTcmMeter : public Meter
{
  public:
    /**
     * @brief Get the TypeId for this class.
     * @return the TypeId.
     */
    static TypeId GetTypeId();

    void ApplyMeter(PolicyEntry& entry, double nowSeconds, uint32_t packetSize) override;

  protected:
    Colour DoApplyPolicer(PolicyEntry& entry, uint32_t packetSize) override;
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_SR_TCM_METER_H
