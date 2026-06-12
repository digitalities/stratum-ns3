/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef NS3_STRATUM_CAKE_LINUX_AUTORATE_HOOK_H
#define NS3_STRATUM_CAKE_LINUX_AUTORATE_HOOK_H

#include "ns3/nstime.h"
#include "ns3/stratum-cake-helper.h"

#include <cstdint>

namespace ns3::stratum::cake
{

/**
 * @brief Linux-faithful peak-rate-EWMA autorate hook.
 *
 * Reproduces the incoming-capacity estimator embedded in
 * `sch_cake.c::cake_enqueue` (the `CAKE_FLAG_AUTORATE_INGRESS` branch).
 * Each accepted packet's wire length is accumulated into the open
 * measurement window; an inter-arrival EWMA (`cake_ewma`, `sch_cake.c:1373`)
 * filters short-term bursts, using shift 2 (alpha=1/4) when the new
 * inter-arrival exceeds the running average and shift 8 (alpha=1/256)
 * otherwise. When an inter-arrival exceeds that average the window is
 * closed: its bytes-per-second is folded into `avg_peak_bandwidth` with
 * the same asymmetric shift rule (fast attack up, slow decay down), and a
 * new window opens. The reconfigure target is `avg_peak_bandwidth x 15/16`
 * (`sch_cake.c:1913`), throttled by a 250 ms deadband.
 *
 * The internal `avg_peak_bandwidth` is held in the kernel's native units of
 * bytes per second; it is seeded from the configured aggregate rate
 * (`sch_cake.c:2893`) and the inter-arrival sample is capped at one second
 * (`sch_cake.c:1885`). The rate conversion to/from the dispatcher's
 * bits-per-second convention happens only at the seed input and the target
 * output, so the running state evolves byte-for-byte with the kernel given
 * the same arrival stream.
 *
 * @see provenance/linux-sch-cake-67dc6c56b871/sch_cake.c::cake_enqueue
 */
class LinuxAutorateHook : public AutorateIngressHook
{
  public:
    LinuxAutorateHook();

    /**
     * @brief Seed the peak-bandwidth estimate from the configured rate.
     *
     * Mirrors `cake_init` setting `avg_peak_bandwidth = rate_bps`
     * (`sch_cake.c:2893`). The argument is the configured aggregate rate in
     * bits per second (the dispatcher's convention); it is converted to the
     * kernel's internal bytes-per-second unit. Zero (an unlimited shaper)
     * leaves the estimate unseeded.
     *
     * @param rateBps configured aggregate rate in bits per second
     */
    void SeedRate(uint64_t rateBps);

    /**
     * @brief Observe a packet arrival and update EWMA state.
     *
     * @param adjLenBytes packet wire-length after overhead/MPU/framing
     * @param now         current simulation time
     */
    void OnEnqueue(uint32_t adjLenBytes, Time now);

    int64_t ComputeRateDelta(uint64_t currentRateBps) const override;

  private:
    /**
     * @brief Kernel `cake_ewma`: `avg - (avg >> shift) + (sample >> shift)`.
     *
     * Two-term unsigned form from `sch_cake.c:1373`. It cannot underflow
     * (`avg >> shift <= avg`) and, from `avg == 0`, the first sample yields
     * `sample >> shift` rather than the full sample.
     *
     * @param avg    current EWMA
     * @param sample new sample
     * @param shift  log2 of the filter weight (2 => alpha=1/4, 8 => alpha=1/256)
     * @return updated EWMA
     */
    static uint64_t CakeEwma(uint64_t avg, uint64_t sample, uint32_t shift);

    Time m_lastEnqueue{Time(0)};            //!< Simulation time of the previous arrival
    uint64_t m_avgPacketInterval{0};        //!< EWMA of inter-arrival time in nanoseconds
    Time m_windowStart{Time(0)};            //!< Start of the current measurement window
    uint64_t m_windowBytes{0};              //!< Bytes accumulated in the current window
    uint64_t m_avgPeakBandwidth{0};         //!< EWMA of per-window peak bandwidth in bytes/sec
    mutable bool m_haveReconfigured{false}; //!< True once ComputeRateDelta has run at least once
    mutable Time m_lastReconfig{Time(0)};   //!< Time of the last ComputeRateDelta update
};

} // namespace ns3::stratum::cake

#endif // NS3_STRATUM_CAKE_LINUX_AUTORATE_HOOK_H
