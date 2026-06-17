/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef NS3_STRATUM_RATE_BASED_SHAPER_DISPATCHER_H
#define NS3_STRATUM_RATE_BASED_SHAPER_DISPATCHER_H

#include "stratum-rate-based-global-clock.h"
#include "stratum-rate-based-tin-clock.h"

#include "ns3/event-id.h"
#include "ns3/ptr.h"
#include "ns3/queue-disc.h"

#include <array>
#include <memory>
#include <vector>

namespace ns3::stratum::cake
{

// Forward declaration — avoids pulling stratum-cake-helper.h into the model layer.
class LinuxAutorateHook;

/**
 * @brief CAKE rate-based virtual-clock shaper dispatcher.
 *
 * Parallels `TinShaperDispatcher` but consumes a per-tin
 * `RateBasedTinClock` instead of `TinTokenBucket`. Each tin holds an
 * independent virtual clock; an additional aggregate-rate clock pair
 * binds total egress per the Linux sch_cake dual-clock model
 * (provenance/linux-sch-cake-67dc6c56b871/sch_cake.c @ 67dc6c56b871).
 *
 * Across-tin selection follows Linux cake's shaped mode: the global
 * clock pair (primary + failsafe) is the only hard gate; per-tin
 * clocks rank, never block. Among backlogged tins, the highest-
 * priority tin whose clock meets its schedule is served; when none
 * meets it, the earliest-scheduled backlogged tin is served anyway
 * (work-conserving). Tin priority is the helper-supplied permutation
 * (ascending; default slot order). On dequeue the per-tin clock
 * advances via the three-branch logic and the global pair advances by
 * adj_len/global_rate (failsafe at 1.5x, skipped on drop charging).
 * When the global pair gates, the dispatcher self-wakes at its
 * effective gate time.
 */
class RateBasedShaperDispatcher : public QueueDisc
{
  public:
    /** @brief Get the TypeId for this class. */
    static TypeId GetTypeId();

    RateBasedShaperDispatcher();
    ~RateBasedShaperDispatcher() override;

    /**
     * @brief Configure a tin slot's per-tin clock parameters.
     *
     * Must be called once per tin before the dispatcher starts.
     *
     * @param slot     zero-based tin index
     * @param rateBps  per-tin shaping rate in bits per second
     * @param overhead signed per-packet wire-byte overhead
     * @param mpu      minimum packet unit in bytes
     * @param framing  ATM/PTM/none cell-rounding mode
     */
    void ConfigureTin(uint32_t slot,
                      uint64_t rateBps,
                      int32_t overhead,
                      uint32_t mpu,
                      RateBasedTinClock::FramingMode framing);

    /**
     * @brief Configure the aggregate global clock rate.
     *
     * @param rateBps aggregate shaping rate in bits per second
     */
    void ConfigureGlobal(uint64_t rateBps);

    /**
     * @brief LLQ-first toggle: when true, the LLQ slot is examined ahead
     *        of the shaped-mode selection.
     *
     * @param enabled true to enable LLQ-first dequeue priority
     */
    void SetEnableLlq(bool enabled);

    /**
     * @brief Set the across-tin priority permutation (ascending priority).
     *
     * Linux cake's shaped-mode selection scans tins by raw index with
     * last-qualifying-wins, so the scan order IS the priority order
     * (diffserv4: BE < Bulk < Video < Voice). This dispatcher's slot
     * layout is helper-defined, so the helper supplies the equivalent
     * permutation; default is ascending slot order.
     *
     * @param order slot indices in ascending priority order. Entries
     *              beyond the internal-queue count are skipped at
     *              dequeue; positions past the end of @p order fall
     *              back to their own slot index; an empty vector keeps
     *              plain ascending slot order.
     */
    void SetTinPriorityOrder(std::vector<uint32_t> order);

    /**
     * @brief Set the DSCP-codepoint -> tin-slot mapping.
     *
     * Drives DSCP-based classification when this dispatcher is installed
     * as a standalone root qdisc (via @c Helper::BuildAndInstall).
     * @p dscp must be a valid 6-bit code point; @p slot must be a
     * configured tin index.
     *
     * @param dscp 6-bit DSCP code point (0..63)
     * @param slot zero-based tin index
     */
    void SetDscpToSlot(uint8_t dscp, uint32_t slot);

    /**
     * @brief Read the configured tin slot for a DSCP code point.
     * @param dscp 6-bit DSCP code point (0..63)
     * @return zero-based tin index the code point maps to
     */
    uint32_t GetDscpToSlot(uint8_t dscp) const;

    /**
     * @brief Toggle Linux `tc-cake(8)` `ingress` mode.
     *
     * When enabled, the per-tin and global clocks advance on packet drops
     * (overflow at the dispatcher boundary) as well as on forwarded
     * packets. Matches `sch_cake.c::cake_enqueue` calling
     * `cake_advance_shaper(..., true)` when CAKE_FLAG_INGRESS is set.
     *
     * Default: false (egress shaping — clocks advance only on dequeue).
     *
     * Note: AQM-decided drops inside the inner FqCobaltQueueDisc are not
     * visible to this dispatcher in v1; ingress accounting covers
     * overflow drops at the dispatcher boundary only.
     *
     * @param enabled true to enable ingress-mode clock charging on drop
     * @see provenance/linux-sch-cake-67dc6c56b871/sch_cake.c::cake_enqueue
     */
    void SetIngressMode(bool enabled);

    /** @brief Return the current ingress-mode setting. */
    bool GetIngressMode() const;

    /**
     * @brief Attach a Linux-faithful autorate hook.
     *
     * When non-null, the hook's `OnEnqueue` is called on every accepted
     * packet, allowing it to maintain EWMA state across the arrival
     * stream. The hook must outlive this dispatcher.
     *
     * @param hook pointer to an instantiated `LinuxAutorateHook`
     *             (nullptr disables autorate observation)
     */
    void SetAutorateHook(std::shared_ptr<LinuxAutorateHook> hook);

    /**
     * @brief Return the cumulative bytes charged to a per-tin clock.
     *
     * In egress mode, counts forwarded bytes only (charged on dequeue).
     * In ingress mode, also counts dropped bytes at the dispatcher
     * boundary (overflow path in DoEnqueue).
     *
     * @param slot tin index
     * @return total bytes charged to the per-tin clock for @p slot
     */
    uint64_t GetTinBytesCharged(uint32_t slot) const;

    /**
     * @brief Return the current aggregate (global-clock) shaping rate.
     *
     * Constant unless an autorate hook is installed, in which case it
     * tracks the autorate-inferred bottleneck. Exposed for test
     * observability and autorate read-back.
     *
     * @return aggregate shaping rate in bits per second
     */
    uint64_t GetGlobalRateBps() const;

  protected:
    bool DoEnqueue(Ptr<QueueDiscItem> item) override;
    Ptr<QueueDiscItem> DoDequeue() override;
    Ptr<const QueueDiscItem> DoPeek() override;
    bool CheckConfig() override;
    void InitializeParams() override;

  private:
    /** @brief Deferred-wakeup callback: drives the dequeue loop. */
    void OnSelfWake();

    /**
     * @brief Schedule a SelfWake at the global effective gate if the disc
     *        is backlogged and no wake is already pending.
     */
    void MaybeArmSelfWake();

    /**
     * @brief Dequeue one packet from @p slot, charge per-tin and global
     *        clocks, and return the item.
     *
     * @param slot tin index; must have a non-empty internal queue
     * @param now  current simulation time
     * @return the dequeued item, or nullptr if the internal queue is empty
     */
    Ptr<QueueDiscItem> ServeSlot(uint32_t slot, Time now);

    /**
     * @brief Look up a tin slot from the item's IPv4 DSCP, or 0 if the
     *        item is non-IPv4 / out of range.
     *
     * @param item the QueueDiscItem being classified
     * @return tin slot index (0 by default, never negative)
     */
    int32_t ClassifyByDscp(Ptr<QueueDiscItem> item) const;

    std::vector<RateBasedTinClock> m_tinClocks;        //!< Per-tin virtual-clock state
    RateBasedGlobalClock m_globalClock;                //!< Aggregate-rate virtual clock pair
    EventId m_selfWakeEvent;                           //!< Outstanding deferred-wakeup event
    bool m_enableLlq{false};                           //!< LLQ-first dequeue priority
    bool m_ingressMode{false};                         //!< Ingress-mode flag (charge on drop)
    std::vector<uint32_t> m_priorityOrder;             //!< Ascending-priority slot scan order
    std::array<uint8_t, 64> m_dscpToSlot{};            //!< DSCP -> tin slot (zero-init)
    std::vector<uint64_t> m_bytesCharged;              //!< Cumulative bytes charged per tin
    std::shared_ptr<LinuxAutorateHook> m_autorateHook; //!< Shared autorate hook (Linux variant)
};

} // namespace ns3::stratum::cake

#endif // NS3_STRATUM_RATE_BASED_SHAPER_DISPATCHER_H
