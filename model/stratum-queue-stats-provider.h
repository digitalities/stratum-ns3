/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef NS3_STRATUM_QUEUE_STATS_PROVIDER_H
#define NS3_STRATUM_QUEUE_STATS_PROVIDER_H

#include <cstdint>

namespace ns3::stratum
{

/**
 * @ingroup stratum
 *
 * @brief Inner-agnostic runtime-probe interface for queue statistics.
 *
 * The edge / core composers' runtime probes (GetVirtualQueueLen,
 * GetNumQueues, PrintStats, PrintPhbTable) query any inner that
 * implements this interface polymorphically — callers do not need
 * to know the concrete inner type to sample queue state.
 *
 * Implementations:
 * - RedQueueDisc — maps (queue, prec) to the corresponding
 *   RedSubQueue's virtual-queue length (current length minus tail-
 *   drop / RED-early-drop losses).
 * - l4s::QueueDisc — semantic mapping: (0, _) = L-queue occupancy,
 *   (1, _) = classic-queue occupancy. Precedence is ignored (L4S has
 *   no drop-precedence axis in this sense).
 *
 * Callers that need stats for a composed edge / core obtain the
 * interface via DynamicCast<QueueStatsProvider>(edge->GetInnerDisc()).
 * The interface is DynamicCast-friendly (no Object inheritance);
 * ns-3's Ptr<> template accepts pure-abstract interfaces once the
 * concrete implementation is an Object.
 */
class QueueStatsProvider
{
  public:
    virtual ~QueueStatsProvider() = default;

    /**
     * @brief Get the number of top-level queues.
     * @return the number of top-level queues
     */
    virtual uint32_t GetNumQueues() const = 0;

    /**
     * @brief Get the current queue length at (queue, prec).
     *
     * Returns 0 for out-of-range indices. Semantics are
     * implementation-specific: Red maps to RED sub-queue virtual-queue
     * length; L4S maps queue 0 to the L-queue and queue 1 to the
     * classic queue.
     *
     * @param queue queue index
     * @param prec drop-precedence index (ignored by some implementations)
     * @return current queue length in packets (or 0 for out-of-range)
     */
    virtual int GetQueueLen(uint32_t queue, uint32_t prec) const = 0;

    /**
     * @brief Print a stats summary to stdout.
     *
     * Format is implementation-specific.
     */
    virtual void PrintStats() const = 0;
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_QUEUE_STATS_PROVIDER_H
