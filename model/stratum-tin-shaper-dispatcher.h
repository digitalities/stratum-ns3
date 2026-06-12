/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Across-slot deficit-round-robin dispatcher — second concrete
 * SlotDispatcher. Clients include the CAKE multi-tin composition.
 *
 */

#ifndef NS3_STRATUM_TIN_SHAPER_DISPATCHER_H
#define NS3_STRATUM_TIN_SHAPER_DISPATCHER_H

#include "stratum-edge-queue-disc.h"
#include "stratum-slot-dispatcher.h"
#include "stratum-tin-token-bucket.h"

#include <array>
#include <cstdint>

namespace ns3::stratum::cake
{

/**
 * @ingroup stratum
 *
 * @brief Per-flow counter snapshot inside one tin's inner queue disc.
 *
 * Mirrors the per-flow row of Linux `tc -s qdisc show cake`'s per-tin
 * block. Wire-byte accumulators use the same `GetSize()` semantics as
 * `TinStats` (IPv4 header included). Drop and mark counters are
 * read-side from the inner per-flow `QueueDisc::Stats` at call time.
 *
 * `bytesRemaining` is the live backlog at the moment of the snapshot.
 * The other counters are monotonic since the queue disc was
 * initialized.
 */
struct PerFlowStats
{
    uint64_t bytesEnqueued{0};  //!< Total wire bytes admitted on this flow
    uint64_t pktsEnqueued{0};   //!< Total packets admitted on this flow
    uint64_t pktsDropped{0};    //!< Cumulative drops attributed to this flow
    uint64_t pktsMarked{0};     //!< Cumulative ECN marks on this flow
    uint64_t bytesRemaining{0}; //!< Current backlog of this flow in bytes
};

/**
 * @ingroup stratum
 *
 * @brief Deficit-round-robin across-slot dispatcher for `EdgeQueueDisc`.
 *
 * Each populated slot carries a per-round byte quantum and an
 * accumulating deficit counter. `SelectDequeueSlot` credits the
 * active slot's deficit by its quantum, then checks whether the head
 * packet fits; if it does not, the cursor advances to the next slot.
 * Empty slots are skipped without crediting so idle tins do not
 * accrue save-up credit that would burst unfairly on refill. The
 * deficit is zeroed when a slot drains to empty (see `OnDequeue`)
 * for the same reason.
 *
 * Per-slot rate-capping is deliberately outside this dispatcher's
 * scope. Linux `sch_cake`'s
 * (provenance/linux-sch-cake-67dc6c56b871/sch_cake.c) tin-shaper
 * role splits into two
 * primitives in this port: this dispatcher performs DRR across
 * slots; per-slot rate-caps are achieved by wrapping each slot's
 * inner with a `TbfQueueDisc` (inner-qdisc composition).
 *
 * Byte accounting uses `QueueDiscItem::GetSize()` (wire bytes
 * including the IPv4 header) to match Linux `sch_cake`
 * (provenance/linux-sch-cake-67dc6c56b871/sch_cake.c @ 67dc6c56b871)
 * and ns-3 `FqCoDelQueueDisc` DRR semantics. `GetPacket()->GetSize()`
 * excludes the IP header and is only appropriate for self-clocked FQ
 * virtual-time computations.
 *
 */
class TinShaperDispatcher : public SlotDispatcher
{
  public:
    /** @brief Get the TypeId for this class. */
    static TypeId GetTypeId();

    TinShaperDispatcher();
    ~TinShaperDispatcher() override = default;

    /**
     * @brief Set the DRR quantum for a slot, in bytes.
     *
     * Larger quanta yield a slot proportionally more byte-service per
     * round. Must be called before the owning edge is initialised.
     *
     * @param slot the slot index to configure
     * @param bytes the per-round byte quantum; must be > 0
     */
    void SetQuantum(uint32_t slot, uint32_t bytes);

    /**
     * @brief Get the DRR quantum, in bytes, for a slot.
     *
     * @param slot the slot index to query
     * @return the quantum previously installed via `SetQuantum`, or 0
     *         if the slot was never configured
     */
    uint32_t GetQuantum(uint32_t slot) const;

    /**
     * @brief Enable per-slot hard rate cap.
     *
     * When @p rateBps > 0, `SelectDequeueSlot` skips the slot if its
     * token bucket lacks tokens for the head packet's wire size, even
     * if the DRR deficit would otherwise permit a serve. The token
     * bucket charges on every successful dequeue; idle time refills
     * the bucket up to @p burstBytes.
     *
     * @p rateBps == 0 (the default) disables the cap; the
     * dispatcher then runs purely work-conserving DRR.
     *
     * Must be called before the owning edge is initialised.
     *
     * @param slot the slot index to cap; must be < `kMaxInnerSlots`
     * @param rateBps cap in bits per second; 0 disables
     * @param burstBytes bucket ceiling in bytes; ignored when
     *        rateBps == 0; otherwise must be > 0
     */
    void SetRateCap(uint32_t slot, uint64_t rateBps, uint64_t burstBytes);

    /**
     * @brief Get the rate cap, in bits per second, for a slot.
     *
     * @param slot the slot index to query; must be < `kMaxInnerSlots`
     * @return the cap previously installed via `SetRateCap`, or 0 if
     *         the slot has no cap configured
     */
    uint64_t GetRateCapBps(uint32_t slot) const;

    int32_t SelectDequeueSlot(EdgeQueueDisc* edge) override;
    int32_t PeekSlot(EdgeQueueDisc* edge) override;
    void OnEnqueue(uint32_t slot, Ptr<QueueDiscItem> item, EdgeQueueDisc* edge) override;
    void OnDequeue(uint32_t slot, Ptr<QueueDiscItem> item, EdgeQueueDisc* edge) override;
    TinStats GetTinStats(uint32_t tinIdx, const EdgeQueueDisc* edge) const override;

    /**
     * @brief Per-flow counters within one tin's inner queue disc.
     *
     * The dispatcher reads the inner queue disc at @p slot via the
     * supplied @p edge, walks down to its `flowId`-th `QueueDiscClass`
     * (mirroring the FqCoDel/FqCobalt class list addressing), and
     * returns the per-flow `QueueDisc::Stats` projected onto
     * `PerFlowStats`.
     *
     * Mirrors the Linux `tc -s qdisc show cake` per-tin per-flow row.
     *
     * Out-of-range slot, slot whose inner exposes no flow classes
     * (e.g. flat DropTail or non-FQ inner), or out-of-range flow id
     * return a zero-initialized snapshot rather than aborting.
     *
     * @param slot the tin index to query; must be < `kMaxInnerSlots`.
     * @param flowId zero-based index into the inner's
     *               `QueueDiscClass` list as exposed by
     *               `inner->GetQueueDiscClass(flowId)`.
     * @param edge the owning edge disc (nullable; null yields a zero
     *        snapshot).
     * @return per-flow counters per `PerFlowStats`; zeroed on any
     *         out-of-range / non-FQ-inner condition.
     */
    PerFlowStats GetPerFlowStats(uint32_t slot, uint32_t flowId, const EdgeQueueDisc* edge) const;

  private:
    uint32_t m_activeSlot{0}; //!< Next slot to attempt on SelectDequeueSlot entry
    std::array<int32_t, EdgeQueueDisc::kMaxInnerSlots>
        m_deficit{}; //!< Per-slot deficit counter in bytes, reset on drain-to-empty
    std::array<uint32_t, EdgeQueueDisc::kMaxInnerSlots>
        m_quantum{}; //!< Per-slot DRR quantum in bytes, zero for unconfigured slots
    std::array<TinTokenBucket, EdgeQueueDisc::kMaxInnerSlots>
        m_tokenBuckets{}; //!< Per-slot token bucket; default rateBps=0 disables cap
    std::array<uint64_t, EdgeQueueDisc::kMaxInnerSlots>
        m_bytesEnqueued{}; //!< S-17.32 — wire-bytes admitted per tin
    std::array<uint64_t, EdgeQueueDisc::kMaxInnerSlots>
        m_bytesDequeued{}; //!< S-17.32 — wire-bytes drained per tin
};

} // namespace ns3::stratum::cake

#endif // NS3_STRATUM_TIN_SHAPER_DISPATCHER_H
