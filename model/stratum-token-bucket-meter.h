/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * Copyright (C) 2000 Nortel Networks
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsPolicy.cc TBPolicy (2001).
 */

#ifndef NS3_STRATUM_TOKEN_BUCKET_METER_H
#define NS3_STRATUM_TOKEN_BUCKET_METER_H

#include "stratum-meter.h"

namespace ns3::stratum
{

/**
 * @ingroup stratum
 *
 * @brief Single-rate token bucket meter.
 *
 * Refills cBucket at CIR, capped at CBS. Packets that fit in cBucket
 * are GREEN; packets that don't fit are RED. The bucket is NOT
 * decremented on a RED decision.
 *
 * Reference: dsPolicy.cc TBPolicy::applyMeter (line 610) and
 * TBPolicy::applyPolicer (line 634).
 *
 */
class TokenBucketMeter : public Meter
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

#endif // NS3_STRATUM_TOKEN_BUCKET_METER_H
