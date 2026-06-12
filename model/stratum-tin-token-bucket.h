/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Per-tin token bucket — bookkeeping helper shared by TinShaperDispatcher
 * and HybridLlqDispatcher to enforce per-slot hard rate caps without
 * duplicating token-bucket math across the two dispatcher classes.
 *
 */

#ifndef NS3_STRATUM_TIN_TOKEN_BUCKET_H
#define NS3_STRATUM_TIN_TOKEN_BUCKET_H

#include "ns3/nstime.h"

#include <cstdint>

namespace ns3::stratum::cake
{

/**
 * @ingroup stratum
 *
 * @brief Per-tin token bucket — drains on serve, refills with elapsed time.
 *
 * A plain bookkeeping struct (no `Object` lifecycle, no virtuals) used by
 * `TinShaperDispatcher` and `HybridLlqDispatcher` to enforce a hard
 * per-slot rate cap. The default-constructed state has `rateBps == 0`,
 * which the API treats as **disabled** — `HasTokensFor` always returns
 * true and `Charge` is a no-op. This is the load-bearing byte-identity
 * invariant: every existing scenario that does not call
 * `TinShaperDispatcher::SetRateCap` (or the LLQ analogue) observes
 * the dispatcher's work-conserving DRR behaviour.
 *
 * **Time math.** Refill computed in integer arithmetic via
 * `Time::GetNanoSeconds()`; no floating-point. Tokens may overdraw
 * (`tokensBytes` may go negative after `Charge`) — `HasTokensFor` accounts
 * for that on the next call by computing the projected balance after
 * refill, identical to the Linux `sch_cake`
 * (provenance/linux-sch-cake-67dc6c56b871/sch_cake.c) and `sch_htb`
 * convention.
 */
struct TinTokenBucket
{
    uint64_t rateBps{0};         //!< 0 = disabled (no rate cap)
    uint64_t burstBytes{0};      //!< Bucket ceiling
    int64_t tokensBytes{0};      //!< Current tokens; may go negative after Charge
    Time lastUpdate{Seconds(0)}; //!< Time of last refill

    /**
     * @brief True if `bytes` could be served at @p now without taking
     * the bucket below 0.
     *
     * Pure: reads dispatcher state and `Simulator::Now()` (passed in
     * via @p now); does not mutate. Safe to call from `PeekSlot` paths
     * which must not have side effects.
     *
     * Returns true unconditionally when `rateBps == 0`.
     */
    bool HasTokensFor(uint32_t bytes, Time now) const;

    /**
     * @brief Refill to @p now and deduct @p bytes from the bucket.
     *
     * Always called immediately after a serve decision (in the
     * dispatcher's `OnDequeue` hook). Updates `tokensBytes` and
     * `lastUpdate`. If `rateBps == 0`, this is a no-op.
     */
    void Charge(uint32_t bytes, Time now);

    /**
     * @brief Reset the bucket to a freshly-configured state.
     *
     * Sets `rateBps`, `burstBytes`, `tokensBytes = burstBytes`
     * (full bucket), and `lastUpdate = now`. Called by
     * `TinShaperDispatcher::SetRateCap` and the LLQ analogue.
     */
    void Configure(uint64_t rateBps, uint64_t burstBytes, Time now);
};

} // namespace ns3::stratum::cake

#endif // NS3_STRATUM_TIN_TOKEN_BUCKET_H
