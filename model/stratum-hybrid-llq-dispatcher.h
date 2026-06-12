/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Hybrid LLQ-across-tins dispatcher — third concrete SlotDispatcher.
 * Combines a strict-priority fast path over a configurable subset of
 * slots with deficit-round-robin over the remaining slots; the SP set
 * is empty by default, in which case behaviour matches the pure-DRR
 * `cake::TinShaperDispatcher` byte-for-byte.
 *
 */

#ifndef NS3_STRATUM_HYBRID_LLQ_DISPATCHER_H
#define NS3_STRATUM_HYBRID_LLQ_DISPATCHER_H

#include "stratum-edge-queue-disc.h"
#include "stratum-slot-dispatcher.h"
#include "stratum-tin-token-bucket.h"

#include <array>
#include <cstdint>

namespace ns3::stratum
{

/**
 * @ingroup stratum
 *
 * @brief Hybrid LLQ-across-tins dispatcher for `EdgeQueueDisc`.
 *
 * Each slot is configured as either *strict-priority* or *DRR*. On
 * `SelectDequeueSlot`:
 * 1. SP slots are walked in index order; the first non-empty SP slot
 * drains immediately, with no deficit accounting.
 * 2. If no SP slot is ready, the dispatcher falls through to DRR over
 * the non-SP slots, identical in semantics to
 * `cake::TinShaperDispatcher` (round-start credit when deficit ≤ 0;
 * idle slots skipped without credit; cursor advances on round
 * completion or stays in place on drain-to-empty).
 *
 * The default-constructed dispatcher has no SP slots configured. In
 * that degenerate state it walks DRR-only and is byte-equivalent to
 * `cake::TinShaperDispatcher` configured with the same quanta. The opt-in
 * shape mirrors the project's other dual-track features ( RNG
 * isolation, TCP persist) — every existing scenario that does
 * not opt in observes byte-identical output.
 *
 * **SP saturation contract.** A saturating SP-marked slot starves DRR
 * slots — strict priority is what delivers sub-30 ms latency and is
 * by design. Production deployments running the SP class as a
 * high-bandwidth bulk class should configure all slots as DRR
 * (equivalent to using `cake::TinShaperDispatcher` directly).
 *
 * **Configuration discipline.** A slot is either SP or DRR, never
 * both. `SetSlotStrictPriority(slot)` asserts `m_quantum[slot] == 0`;
 * `SetQuantum(slot, ...)` asserts `IsStrictPriority(slot) == false`.
 * Both setters are pre-Initialize only.
 *
 * **Byte accounting.** Uses `QueueDiscItem::GetSize()` (wire bytes
 * including the IPv4 header) to match Linux `sch_cake`
 * (provenance/linux-sch-cake-67dc6c56b871/sch_cake.c) and
 * `cake::TinShaperDispatcher` DRR semantics. SP slots have no deficit.
 *
 */
class HybridLlqDispatcher : public SlotDispatcher
{
  public:
    /** @brief Get the TypeId for this class. */
    static TypeId GetTypeId();

    HybridLlqDispatcher();
    ~HybridLlqDispatcher() override = default;

    /**
     * @brief Mark a slot as strict-priority.
     *
     * SP slots are served first on every `SelectDequeueSlot`, in slot-
     * index order, with no deficit accounting. Asserts the slot has no
     * DRR quantum configured. Must be called before the owning edge is
     * initialised.
     *
     * @param slot the slot index to mark; must be < `kMaxInnerSlots`
     */
    void SetSlotStrictPriority(uint32_t slot);

    /**
     * @brief Query whether a slot is configured for strict priority.
     *
     * @param slot the slot index to query; must be < `kMaxInnerSlots`
     * @return true if the slot was marked SP, false if DRR or
     * unconfigured
     */
    bool IsStrictPriority(uint32_t slot) const;

    /**
     * @brief Set the DRR quantum for a slot, in bytes.
     *
     * Larger quanta yield a slot proportionally more byte-service per
     * round. Asserts the slot is not SP. Must be called before the
     * owning edge is initialised.
     *
     * @param slot the slot index to configure; must be < `kMaxInnerSlots`
     * @param bytes the per-round byte quantum; must be > 0
     */
    void SetQuantum(uint32_t slot, uint32_t bytes);

    /**
     * @brief Get the DRR quantum, in bytes, for a slot.
     *
     * @param slot the slot index to query; must be < `kMaxInnerSlots`
     * @return the quantum previously installed via `SetQuantum`, or 0
     * if the slot was never configured or is SP
     */
    uint32_t GetQuantum(uint32_t slot) const;

    /**
     * @brief Enable per-slot hard rate cap.
     *
     * Applies UNIFORMLY to strict-priority and DRR slots. When @p
     * rateBps > 0, the dispatcher skips the slot (on both the SP fast
     * path and the DRR fall-through) if its token bucket lacks tokens
     * for the head packet's wire size, even if the slot is otherwise
     * eligible to serve. The token bucket charges on every successful
     * dequeue regardless of slot type; idle time refills the bucket up
     * to @p burstBytes.
     *
     * Composing `SetSlotStrictPriority(slot)` with `SetRateCap(slot, ...)`
     * on the same slot produces the **Cisco MQC LLQ pattern** — a
     * priority class served ahead of all DRR slots but held to a hard
     * bandwidth ceiling. The two setters are orthogonal: SP/DRR mode
     * controls scheduling order; the cap controls served bytes.
     *
     * @p rateBps == 0 (the default) disables the cap; the
     * dispatcher then runs work-conserving (DRR + SP, no rate gate).
     *
     * Must be called before the owning edge is initialised.
     *
     * @param slot the slot index to cap; must be < `kMaxInnerSlots`
     * @param rateBps cap in bits per second; 0 disables
     * @param burstBytes bucket ceiling in bytes; ignored when
     * rateBps == 0; otherwise must be > 0
     */
    void SetRateCap(uint32_t slot, uint64_t rateBps, uint64_t burstBytes);

    /**
     * @brief Get the rate cap, in bits per second, for a slot.
     *
     * @param slot the slot index to query; must be < `kMaxInnerSlots`
     * @return the cap previously installed via `SetRateCap`, or 0 if
     * the slot has no cap configured
     */
    uint64_t GetRateCapBps(uint32_t slot) const;

    int32_t SelectDequeueSlot(EdgeQueueDisc* edge) override;
    int32_t PeekSlot(EdgeQueueDisc* edge) override;
    void OnEnqueue(uint32_t slot, Ptr<QueueDiscItem> item, EdgeQueueDisc* edge) override;
    void OnDequeue(uint32_t slot, Ptr<QueueDiscItem> item, EdgeQueueDisc* edge) override;
    cake::TinStats GetTinStats(uint32_t tinIdx, const EdgeQueueDisc* edge) const override;

  private:
    /**
     * @brief Walk SP slots in index order; return the first non-empty
     * SP slot's index, or -1 if none are ready.
     *
     * Stateless — same code path serves both `SelectDequeueSlot` and
     * `PeekSlot`.
     */
    int32_t FirstReadyStrictPrioritySlot(EdgeQueueDisc* edge) const;

    std::array<bool, EdgeQueueDisc::kMaxInnerSlots>
        m_isStrictPriority{}; //!< SP-mode bitmap, false = DRR
    uint32_t m_drrCursor{0};  //!< Next non-SP slot to attempt on DRR fall-through
    std::array<int32_t, EdgeQueueDisc::kMaxInnerSlots>
        m_deficit{}; //!< Per-slot DRR deficit in bytes; SP slots stay 0
    std::array<uint32_t, EdgeQueueDisc::kMaxInnerSlots>
        m_quantum{}; //!< Per-slot DRR quantum in bytes; SP slots stay 0
    std::array<cake::TinTokenBucket, EdgeQueueDisc::kMaxInnerSlots>
        m_tokenBuckets{}; //!< Per-slot token bucket; applies uniformly to SP and DRR slots.
                          //!< Default rateBps=0 disables the cap (gate is a no-op).
    std::array<uint64_t, EdgeQueueDisc::kMaxInnerSlots>
        m_bytesEnqueued{}; //!< S-17.32 — wire-bytes admitted per tin
    std::array<uint64_t, EdgeQueueDisc::kMaxInnerSlots>
        m_bytesDequeued{}; //!< S-17.32 — wire-bytes drained per tin
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_HYBRID_LLQ_DISPATCHER_H
