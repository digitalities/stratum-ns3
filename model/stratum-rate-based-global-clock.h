/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef NS3_STRATUM_RATE_BASED_GLOBAL_CLOCK_H
#define NS3_STRATUM_RATE_BASED_GLOBAL_CLOCK_H

#include "ns3/nstime.h"

#include <algorithm>
#include <cstdint>

namespace ns3::stratum::cake
{

/**
 * @brief Aggregate-rate virtual clock for the CAKE rate-based shaper.
 *
 * Mirrors `q->time_next_packet` and `q->failsafe_next_packet` on
 * `struct cake_sched_data`. The global clock pair advances on every
 * dequeue (regardless of which tin) and binds the aggregate egress
 * rate independently of the per-tin clocks.
 *
 * Per Linux `cake_configure_rates`, the global rate is set to the
 * fastest tin's rate; this implementation accepts any rate set by
 * the dispatcher at construction.
 */
struct RateBasedGlobalClock
{
    /// Next eligible egress time for the aggregate (q->time_next_packet).
    Time tNext{Time(0)};

    /// Failsafe companion clock (q->failsafe_next_packet): advances at
    /// 1.5x the primary duration per packet and is not charged on
    /// ingress-mode drop advances; the aggregate is blocked only while
    /// BOTH clocks are in the future (sch_cake.c:1543-1561, 2060-2063).
    Time tNextFailsafe{Time(0)};

    /// Aggregate rate (bits per second). Zero means unshaped.
    uint64_t rateBps{0};

    /// Blocking gate: min(tNext, tNextFailsafe). The aggregate is gated
    /// only while BOTH clocks are in the future (gate > now) — service
    /// resumes as soon as either clock is satisfied. Time(0) when
    /// unshaped (always open).
    Time EffectiveGate() const;

    /// now >= EffectiveGate().
    bool MaybeAllow(Time now) const;

    /**
     * @brief Advance the primary clock by adjLen/rate; advance the failsafe by
     *        1.5x that duration unless @p drop (ingress-mode drop charging).
     */
    void Charge(uint32_t adjLen, Time now, bool drop);

    /**
     * @brief Hard snap of BOTH clocks to now when the whole disc was empty
     *        (cake_enqueue stale-state site, sch_cake.c:1789-1792).
     */
    void OnEnqueueIdleReset(Time now);
};

} // namespace ns3::stratum::cake

#endif // NS3_STRATUM_RATE_BASED_GLOBAL_CLOCK_H
