/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "stratum-cake-linux-autorate-hook.h"

#include "ns3/log.h"
#include "ns3/simulator.h"

namespace ns3::stratum::cake
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::cake::LinuxAutorateHook");

namespace
{
//! One second in nanoseconds — the inter-arrival sample cap (sch_cake.c:1885).
constexpr uint64_t kNsecPerSec = 1'000'000'000ULL;
} // namespace

LinuxAutorateHook::LinuxAutorateHook() = default;

uint64_t
LinuxAutorateHook::CakeEwma(uint64_t avg, uint64_t sample, uint32_t shift)
{
    // sch_cake.c:1373 — avg -= avg >> shift; avg += sample >> shift.
    avg -= (avg >> shift);
    avg += (sample >> shift);
    return avg;
}

void
LinuxAutorateHook::SeedRate(uint64_t rateBps)
{
    NS_LOG_FUNCTION(this << rateBps);
    // cake_init: avg_peak_bandwidth = rate_bps (sch_cake.c:2893). The kernel's
    // rate_bps is bytes/sec; the argument arrives in the dispatcher's bits/sec
    // convention, so convert by dividing by eight.
    m_avgPeakBandwidth = rateBps / 8;
}

void
LinuxAutorateHook::OnEnqueue(uint32_t adjLenBytes, Time now)
{
    NS_LOG_FUNCTION(this << adjLenBytes << now);

    // Accumulate this packet's bytes into the open window first, mirroring the
    // stats block at sch_cake.c:1871 (avg_window_bytes += len) which runs
    // before the capacity-estimate block.
    m_windowBytes += adjLenBytes;

    // Incoming-bandwidth capacity estimate (sch_cake.c:1880-1916).
    uint64_t packetInterval = (now - m_lastEnqueue).GetNanoSeconds();
    if (packetInterval > kNsecPerSec)
    {
        packetInterval = kNsecPerSec; // sch_cake.c:1885
    }

    // Filter short-term bursts: attack shift 2 when the interval grows, decay
    // shift 8 otherwise (sch_cake.c:1889-1893). From a zero average the first
    // interval folds in as interval >> shift — no first-sample special-case.
    m_avgPacketInterval =
        CakeEwma(m_avgPacketInterval, packetInterval, packetInterval > m_avgPacketInterval ? 2 : 8);
    m_lastEnqueue = now;

    // An inter-arrival above the running average closes the window
    // (sch_cake.c:1897).
    if (packetInterval > m_avgPacketInterval)
    {
        const uint64_t windowNs = (now - m_windowStart).GetNanoSeconds();
        if (windowNs > 0)
        {
            // window bytes/sec = avg_window_bytes * NSEC_PER_SEC / window_interval
            // (sch_cake.c:1901-1903). The 128-bit intermediate makes the
            // multiply overflow-proof; it equals the kernel's div64_u64 result
            // for every window the kernel itself does not overflow.
            const uint64_t windowBytesPerSec = static_cast<uint64_t>(
                (static_cast<unsigned __int128>(m_windowBytes) * kNsecPerSec) / windowNs);
            m_avgPeakBandwidth = CakeEwma(m_avgPeakBandwidth,
                                          windowBytesPerSec,
                                          windowBytesPerSec > m_avgPeakBandwidth ? 2 : 8);
        }
        m_windowBytes = 0;
        m_windowStart = now;
    }
}

int64_t
LinuxAutorateHook::ComputeRateDelta(uint64_t currentRateBps) const
{
    const Time now = Simulator::Now();
    if (m_haveReconfigured && (now - m_lastReconfig) < MilliSeconds(250))
    {
        return 0;
    }
    m_haveReconfigured = true;
    m_lastReconfig = now;

    if (m_avgPeakBandwidth == 0)
    {
        return 0;
    }

    // Target rate = avg_peak_bandwidth x 15/16 (sch_cake.c:1913), computed in
    // the kernel's bytes/sec unit, then converted to the dispatcher's bits/sec.
    // The x8 follows the >>4 (kernel order); doing it earlier would change the
    // low bits the truncation drops.
    const uint64_t targetBytesPerSec = (m_avgPeakBandwidth * 15ULL) >> 4;
    const uint64_t targetBps = targetBytesPerSec * 8ULL;
    return static_cast<int64_t>(targetBps) - static_cast<int64_t>(currentRateBps);
}

} // namespace ns3::stratum::cake
