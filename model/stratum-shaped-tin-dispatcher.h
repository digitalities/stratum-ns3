/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Across-slot shaped-mode dispatcher — aggregate virtual-clock pair as
 * the only hard gate, per-tin clocks as priority-demotion signals.
 *
 */

#ifndef NS3_STRATUM_SHAPED_TIN_DISPATCHER_H
#define NS3_STRATUM_SHAPED_TIN_DISPATCHER_H

#include "stratum-edge-queue-disc.h"
#include "stratum-rate-based-global-clock.h"
#include "stratum-rate-based-tin-clock.h"
#include "stratum-slot-dispatcher.h"

#include "ns3/event-id.h"
#include "ns3/nstime.h"

#include <array>
#include <cstdint>
#include <vector>

namespace ns3::stratum::cake
{

/**
 * @ingroup stratum
 *
 * @brief Shaped-mode across-slot dispatcher for `EdgeQueueDisc`.
 *
 * Mirrors Linux `cake_dequeue` shaped-mode semantics
 * (provenance/linux-sch-cake-67dc6c56b871/sch_cake.c): the aggregate
 * virtual-clock pair (primary plus the 1.5x failsafe companion,
 * sch_cake.c:1533-1558) is the only hard gate (sch_cake.c:2060-2065);
 * among backlogged tins scanned in ascending priority order, any
 * schedule-meeting tin steals the choice so the last meeting tin —
 * highest priority — wins, and when none meets its schedule the
 * earliest-scheduled backlogged tin is served anyway
 * (sch_cake.c:2106-2129). Per-tin clocks demote selection priority;
 * they never block.
 *
 * In-tin scheduling is delegated to each slot's inner queue disc —
 * with per-tin `FqCobaltQueueDisc` inners this composes the
 * `tc-cake bandwidth N diffserv4` stack (aggregate shaper + tin
 * priority + per-flow DRR + AQM).
 *
 * When the gate is closed the dispatcher returns -1 from
 * `SelectDequeueSlot` and self-wakes the edge at the gate time, the
 * analog of `qdisc_watchdog_schedule_ns` (sch_cake.c:2064).
 *
 * Egress-mode semantics: packets dropped inside an inner disc never
 * reach `OnDequeue` and so never charge the shaper, matching Linux
 * egress mode (drop charging there is ingress-flag-gated).
 */
class ShapedTinDispatcher : public SlotDispatcher
{
  public:
    /** @brief Get the TypeId for this class. */
    static TypeId GetTypeId();

    ShapedTinDispatcher() = default;

    /**
     * @brief Set the aggregate shaper rate.
     * @param rateBps aggregate egress rate in bits per second
     */
    void ConfigureGlobal(uint64_t rateBps);

    /**
     * @brief Configure one tin's demotion clock.
     *
     * @param slot tin slot index
     * @param rateBps tin rate in bits per second (demotion threshold)
     * @param overhead per-packet wire-byte overhead (signed)
     * @param mpu minimum packet unit in bytes
     * @param framing cell-rounding framing mode
     */
    void ConfigureTin(uint32_t slot,
                      uint64_t rateBps,
                      int32_t overhead,
                      uint32_t mpu,
                      RateBasedTinClock::FramingMode framing);

    /**
     * @brief Install the ascending-priority scan permutation.
     * @param order slot indices in ascending selection priority; must be a permutation covering
     * every populated slot (a partial or duplicated order can starve a slot)
     */
    void SetTinPriorityOrder(std::vector<uint32_t> order);

    int32_t SelectDequeueSlot(EdgeQueueDisc* edge) override;
    int32_t PeekSlot(EdgeQueueDisc* edge) override;
    void OnEnqueue(uint32_t slot, Ptr<QueueDiscItem> item, EdgeQueueDisc* edge) override;
    void OnDequeue(uint32_t slot, Ptr<QueueDiscItem> item, EdgeQueueDisc* edge) override;
    cake::TinStats GetTinStats(uint32_t tinIdx, const EdgeQueueDisc* edge) const override;

  protected:
    void DoDispose() override;

  private:
    /**
     * @brief Shaped scan over backlogged slots; -1 when none populated.
     */
    int32_t ScanBestSlot(const EdgeQueueDisc* edge, Time now) const;

    /** @brief True when any populated inner slot holds a packet. */
    static bool AnyBacklog(const EdgeQueueDisc* edge);

    RateBasedGlobalClock m_globalClock;         //!< Aggregate clock pair
    std::vector<RateBasedTinClock> m_tinClocks; //!< Per-tin demotion clocks
    std::vector<uint32_t> m_priorityOrder;      //!< Ascending-priority scan order
    EventId m_selfWakeEvent;                    //!< Pending gate wake, if any

    /// Wire bytes admitted per tin (tc -s parity counters).
    std::array<uint64_t, EdgeQueueDisc::kMaxInnerSlots> m_bytesEnqueued{};
    /// Wire bytes drained per tin.
    std::array<uint64_t, EdgeQueueDisc::kMaxInnerSlots> m_bytesDequeued{};
};

} // namespace ns3::stratum::cake

#endif // NS3_STRATUM_SHAPED_TIN_DISPATCHER_H
