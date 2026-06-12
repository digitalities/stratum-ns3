/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "stratum-rate-based-global-clock.h"

#include "ns3/log.h"

namespace ns3::stratum::cake
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::cake::RateBasedGlobalClock");

Time
RateBasedGlobalClock::EffectiveGate() const
{
    if (rateBps == 0)
    {
        return Time(0);
    }
    return std::min(tNext, tNextFailsafe);
}

bool
RateBasedGlobalClock::MaybeAllow(Time now) const
{
    return now >= EffectiveGate();
}

void
RateBasedGlobalClock::Charge(uint32_t adjLen, Time /*now*/, bool drop)
{
    if (rateBps == 0)
    {
        return;
    }

    // Primary global clock advances unconditionally per cake_advance_shaper:
    //   q->time_next_packet = ktime_add_ns(q->time_next_packet, global_dur);
    __int128 numer = static_cast<__int128>(adjLen) * 8 * 1'000'000'000ULL;
    int64_t globalDurNs = static_cast<int64_t>(numer / rateBps);
    tNext = tNext + NanoSeconds(globalDurNs);

    // Failsafe advances at 1.5x; not charged on ingress-mode drops
    // (sch_cake.c:1555-1561: if (!drop) failsafe += global_dur + global_dur/2).
    if (!drop)
    {
        tNextFailsafe = tNextFailsafe + NanoSeconds(globalDurNs + globalDurNs / 2);
    }
}

void
RateBasedGlobalClock::OnEnqueueIdleReset(Time now)
{
    // Linux cake_enqueue (when !sch->q.qlen): hard snap-to-now if the
    // aggregate clock fell behind real time during the all-tins-empty
    // idle period (sch_cake.c:1789-1791).
    // When the primary is stale, snap BOTH clocks together: ingress-mode
    // overflow charging advances the primary only (the drop=true Charge
    // call in the dispatcher's enqueue path), and a fresh burst must not
    // inherit a half-stale pair.
    if (tNext < now)
    {
        tNextFailsafe = now;
        tNext = now;
    }
    else if (tNextFailsafe < now)
    {
        tNextFailsafe = now;
    }
}

} // namespace ns3::stratum::cake
