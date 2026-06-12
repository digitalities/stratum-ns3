/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef NS3_STRATUM_RATE_BASED_TIN_CLOCK_H
#define NS3_STRATUM_RATE_BASED_TIN_CLOCK_H

#include "ns3/data-rate.h"
#include "ns3/nstime.h"

#include <cstdint>

namespace ns3::stratum::cake
{

/**
 * @brief Per-tin rate-based virtual-clock state for the CAKE rate-based
 *        shaper path.
 *
 * Mirrors the per-tin shaper state in the Linux sch_cake.c qdisc
 * (67dc6c56b871, cake_advance_shaper @ line 1533;
 * provenance/linux-sch-cake-67dc6c56b871/sch_cake.c):
 * each tin owns an independent virtual clock (tNext) advanced on dequeue
 * by adj_len/rate. Eligibility is gated on Now() >= tNext.
 *
 * Trivially copyable: this is a value-type POD held in a vector inside
 * the dispatcher.
 */
struct RateBasedTinClock
{
    /// Wire framing for adj_len cell-rounding.
    enum class FramingMode
    {
        NoAtm,
        Atm,
        Ptm,
    };

    /// Next eligible egress time for this tin's virtual clock.
    Time tNext{Time(0)};

    /// Per-tin rate (bits per second). Zero means unshaped (rate-gate disabled).
    uint64_t rateBps{0};

    /// Per-packet wire-byte overhead (signed: matches Linux s32 rate_overhead).
    int32_t overhead{0};

    /// Minimum packet unit (bytes); adj_len floored at this after overhead add.
    uint32_t mpu{0};

    /// Cell-rounding framing.
    FramingMode framing{FramingMode::NoAtm};

    /**
     * @brief Test whether the tin is eligible for dequeue at @p now.
     */
    bool MaybeAllow(Time now) const;

    /**
     * @brief Advance the per-tin virtual clock for a packet of size
     *        @p adjLen (already overhead/MPU/framing-adjusted).
     *
     * Implements the three-branch advance from cake_advance_shaper.
     */
    void Charge(uint32_t adjLen, Time now);

    /**
     * @brief Hard snap-to-now when the tin transitions empty -> non-empty
     *        with stale tNext (cake_enqueue site).
     */
    void OnEnqueueIdleReset(Time now);

    /**
     * @brief Compute the per-packet adj_len matching cake_calc_overhead:
     *        net_len + overhead, MPU floor, then ATM/PTM cell-rounding.
     */
    static uint32_t ComputeAdjLen(uint32_t netLen,
                                  int32_t overhead,
                                  FramingMode framing,
                                  uint32_t mpu);
};

} // namespace ns3::stratum::cake

#endif // NS3_STRATUM_RATE_BASED_TIN_CLOCK_H
