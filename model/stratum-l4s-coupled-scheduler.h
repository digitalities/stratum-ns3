/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * L4S-coupled scheduler — RFC 9332 §2.5.1 bounded conditional priority.
 */

#ifndef NS3_STRATUM_L4S_COUPLED_SCHEDULER_H
#define NS3_STRATUM_L4S_COUPLED_SCHEDULER_H

#include "stratum-scheduler.h"

namespace ns3::stratum::l4s
{

/**
 * @ingroup stratum
 * @brief L4S priority scheduler with classic-queue starvation safeguard.
 *
 * Acts as a strict-L4S-priority scheduler under normal load: as long
 * as the L4S sub-queue has packets, it is served first. RFC 9332
 * §2.5.1 requires the L4S priority to be bounded so that sustained
 * heavy L4S backlog cannot starve the classic queue (see also §4.2.2),
 * so this scheduler enforces a packet-based
 * burst cap: after @c L4sBurstCap consecutive L4S dequeues, a non-empty
 * classic queue is served once, then the L4S burst counter resets.
 *
 * Defaults: @c L4sBurstCap = 8 packets (RFC 9332 §4.2.2 advises that
 * the classic scheduling weight "should be small (e.g., 1/16)"; a cap
 * of 8 guarantees classic flows ~11 % of dequeue opportunities under
 * sustained L4S backlog).
 *
 * **Composition note:** This scheduler is plugged into a
 * @link QueueDisc @endlink (or any classful disc) via
 * @c SetScheduler. It does not subclass @link QueueDisc @endlink
 * and does not require changes to it; the starvation safeguard
 * lives entirely at the scheduler layer.
 */
class CoupledScheduler : public Scheduler
{
  public:
    /**
     * @brief Get the TypeId for this class.
     * @return the TypeId.
     */
    static TypeId GetTypeId();

    /** @brief Construct a CoupledScheduler with default burst cap. */
    CoupledScheduler();

    ~CoupledScheduler() override;

    void Reset() override;
    void OnEnqueue(uint32_t queueIndex, uint32_t packetSizeBytes) override;
    int SelectNextQueue() override;

    /**
     * @brief Get the L4S burst cap (max consecutive L4S dequeues).
     * @return the burst cap in packets
     */
    uint32_t GetL4sBurstCap() const;

    /**
     * @brief Set the L4S burst cap.
     *
     * Tests use this to make starvation behaviour reachable in a small
     * number of packets.
     *
     * @param cap maximum consecutive L4S dequeues before a classic is
     * forced
     */
    void SetL4sBurstCap(uint32_t cap);

    /**
     * @brief Inspection: current L4S burst counter.
     * @return consecutive L4S dequeues since the last classic dequeue
     */
    uint32_t GetL4sBurstCount() const;

    /**
     * @brief Inspection: total times the burst cap forced a classic dequeue.
     * @return count of forced classic dequeues since construction
     */
    uint64_t GetForcedClassicCount() const;

  private:
    uint32_t m_l4sQueueIdx{0};        //!< Scheduler-slot index assigned to the L4S lane
    uint32_t m_burstCap{8};           //!< Max consecutive L4S dequeues before classic forced
    uint32_t m_l4sBurstCount{0};      //!< Current L4S burst counter
    uint64_t m_forcedClassicCount{0}; //!< Times the burst cap forced a classic dequeue
    int m_queueLen[kMaxQueues];       //!< Per-queue occupancy tracked via OnEnqueue
    uint32_t m_classicRrCursor{0};    //!< Round-robin cursor across non-L4S queues
};

} // namespace ns3::stratum::l4s

#endif // NS3_STRATUM_L4S_COUPLED_SCHEDULER_H
