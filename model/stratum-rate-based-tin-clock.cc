/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "stratum-rate-based-tin-clock.h"

#include "ns3/log.h"

namespace ns3::stratum::cake
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::cake::RateBasedTinClock");

bool
RateBasedTinClock::MaybeAllow(Time now) const
{
    NS_LOG_FUNCTION(this << now);
    return now >= tNext;
}

void
RateBasedTinClock::Charge(uint32_t adjLen, Time now)
{
    if (rateBps == 0)
    {
        // Unshaped — no virtual clock advance.
        return;
    }

    // tin_dur = adjLen * 8 * 1e9 / rateBps  (nanoseconds)
    // Use __int128 intermediate to avoid uint64_t overflow at GBps.
    __int128 numer = static_cast<__int128>(adjLen) * 8 * 1'000'000'000ULL;
    int64_t tinDurNs = static_cast<int64_t>(numer / rateBps);
    Time tinDur = NanoSeconds(tinDurNs);

    // Three-branch advance per cake_advance_shaper:
    if (tNext < now)
    {
        // Stale: advance from stale value (NOT snap-to-now).
        tNext = tNext + tinDur;
    }
    else if (tNext < now + tinDur)
    {
        // Behind-but-within-window: snap to now + tinDur.
        tNext = now + tinDur;
    }
    // Else (tNext >= now + tinDur): natural shaping, untouched.
}

void
RateBasedTinClock::OnEnqueueIdleReset(Time now)
{
    // Linux cake_enqueue (when !b->tin_backlog): hard snap-to-now if stale.
    // This is the FIRST of the two catchup sites; the SECOND lives in
    // Charge (cake_advance_shaper). Together they prevent banked idle
    // credit from bursting at the next dequeue while still allowing
    // residual credit to drain naturally under sustained backlog.
    if (tNext < now)
    {
        tNext = now;
    }
}

uint32_t
RateBasedTinClock::ComputeAdjLen(uint32_t netLen,
                                 int32_t overhead,
                                 FramingMode framing,
                                 uint32_t mpu)
{
    // Apply overhead: signed addition (Linux rate_overhead is s32).
    int64_t adj = static_cast<int64_t>(netLen) + overhead;
    if (adj < 0)
    {
        adj = 0;
    }

    // MPU floor (after overhead, NOT max() against the sum).
    if (static_cast<uint64_t>(adj) < mpu)
    {
        adj = mpu;
    }

    switch (framing)
    {
    case FramingMode::Atm:
        // len = ((adj + 47) / 48) * 53
        adj = ((adj + 47) / 48) * 53;
        break;
    case FramingMode::Ptm:
        // len += (len + 63) / 64
        adj = adj + (adj + 63) / 64;
        break;
    case FramingMode::NoAtm:
        // unchanged
        break;
    }

    return static_cast<uint32_t>(adj);
}

} // namespace ns3::stratum::cake
