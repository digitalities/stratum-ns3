/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#include "stratum-tin-token-bucket.h"

#include <algorithm>
#include <cstdint>

namespace ns3::stratum::cake
{

namespace
{
/**
 * Refill amount, in bytes, accumulated over @p elapsed at @p rateBps.
 *
 * Uses 128-bit intermediate math to stay overflow-safe across the full
 * realistic envelope: at 100 Gbps over 1 hour the product is
 * `1e11 × 3.6e12 = 3.6e23`, well past uint64's 1.8e19 ceiling. Pure
 * integer math; no floating-point.
 *
 * The downstream `std::min(burstBytes, ...)` clamp in the caller bounds
 * the meaningful refill, so once `refill ≥ burstBytes` any further
 * accumulation is discarded — the saturation cap of int64 here exists
 * only as a defensive ceiling against pathological elapsed values.
 *
 * Cap: at rateBps == 0, returns 0 (caller should have early-returned).
 */
int64_t
RefillBytes(uint64_t rateBps, Time elapsed)
{
    if (rateBps == 0)
    {
        return 0;
    }
    int64_t elapsedNs = elapsed.GetNanoSeconds();
    if (elapsedNs <= 0)
    {
        return 0;
    }
    // refill_bytes = rateBps × elapsedNs / (8 × 1e9), in 128-bit math.
    __uint128_t product = static_cast<__uint128_t>(rateBps) * static_cast<__uint128_t>(elapsedNs);
    __uint128_t refill = product / 8000000000ull;
    if (refill > static_cast<__uint128_t>(INT64_MAX))
    {
        return INT64_MAX;
    }
    return static_cast<int64_t>(refill);
}
} // anonymous namespace

bool
TinTokenBucket::HasTokensFor(uint32_t bytes, Time now) const
{
    if (rateBps == 0)
    {
        return true;
    }
    int64_t refill = RefillBytes(rateBps, now - lastUpdate);
    int64_t projected = std::min(static_cast<int64_t>(burstBytes), tokensBytes + refill);
    return projected >= static_cast<int64_t>(bytes);
}

void
TinTokenBucket::Charge(uint32_t bytes, Time now)
{
    if (rateBps == 0)
    {
        return;
    }
    int64_t refill = RefillBytes(rateBps, now - lastUpdate);
    tokensBytes = std::min(static_cast<int64_t>(burstBytes), tokensBytes + refill);
    lastUpdate = now;
    tokensBytes -= static_cast<int64_t>(bytes);
}

void
TinTokenBucket::Configure(uint64_t rateBpsIn, uint64_t burstBytesIn, Time now)
{
    rateBps = rateBpsIn;
    burstBytes = burstBytesIn;
    tokensBytes = static_cast<int64_t>(burstBytesIn);
    lastUpdate = now;
}

} // namespace ns3::stratum::cake
