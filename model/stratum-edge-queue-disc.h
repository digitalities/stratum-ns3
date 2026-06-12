/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * Copyright (C) 2000 Nortel Networks
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsEdge.{h,cc} class edgeQueue (2001).
 */

#ifndef NS3_STRATUM_EDGE_QUEUE_DISC_H
#define NS3_STRATUM_EDGE_QUEUE_DISC_H

#include "stratum-constants.h"
#include "stratum-edge-meter-provider.h"
#include "stratum-mark-rule.h"
#include "stratum-meter.h"
#include "stratum-per-flow-policy-classifier.h"
#include "stratum-policy-classifier.h"
#include "stratum-red-queue-disc.h"
#include "stratum-scheduler.h"
#include "stratum-slot-dispatcher.h"

#include "ns3/queue-disc.h"

#include <array>
#include <cstddef>

namespace ns3::stratum
{

/**
 * @ingroup stratum
 *
 * @brief DiffServ edge router queue disc — composition over RedQueueDisc.
 *
 * `EdgeQueueDisc` descends directly from `QueueDisc` and
 * composes a single `Ptr<RedQueueDisc> m_inner` child (idx 0) that
 * handles the PHB table, sub-queues, WRED / RIO state, and scheduler.
 * The outer composer owns the edge-specific responsibilities only:
 *
 * * multi-field classification via the `MarkRule` table,
 * * DSCP-keyed metering and policing via `diffserv::PolicyClassifier`,
 * * optional per-flow metering via `diffserv::PerFlowPolicyClassifier`.
 *
 * The computed final DSCP is stamped as a `DscpTag` on enqueue;
 * the inner `RedQueueDisc::DoEnqueue` reads the tag for its PHB
 * lookup (symmetric with its tag-first `DoDequeue`). On dequeue the
 * composer reads the tag back, rewrites the IPv4 header TOS field,
 * and removes the tag.
 *
 * Port of ns-2 edgeQueue from dsEdge.{h,cc} (DiffServ4NS 2001).
 *
 */
class EdgeQueueDisc : public QueueDisc, public EdgeMeterProvider
{
  public:
    /**
     * @brief Get the TypeId for this class.
     * @return the TypeId.
     */
    static TypeId GetTypeId();

    /** @brief Construct a EdgeQueueDisc with default tables. */
    EdgeQueueDisc();

    ~EdgeQueueDisc() override;

    // --- Meter strategy slots ---

    /**
     * @brief Install a custom Meter algorithm for the given MeterType.
     *
     * Must be called before Initialize. Once installed, classifiers that
     * consult this edge as their EdgeMeterProvider (the DSCP-keyed
     * classifier automatically; per-flow classifiers opt in) will route
     * all policy->meter == type dispatches through the injected
     * instance. Passing nullptr clears the slot; GetMeter will then
     * create the default implementation on the next request.
     *
     * Enables experiments that swap a stateful meter for a stock one
     * without subclassing the classifier (e.g. an ECN-aware meter).
     *
     * @param type meter algorithm selector
     * @param meter meter instance to install (nullptr clears the slot)
     */
    void SetMeter(MeterType type, Ptr<Meter> meter);

    /**
     * @brief Lookup hook implementation (EdgeMeterProvider).
     *
     * Returns the installed meter for @p type, lazy-creating the
     * default implementation if the slot is empty.
     *
     * @param type meter algorithm selector
     * @return the Meter instance
     */
    Ptr<Meter> GetMeter(MeterType type) override;

    // --- Edge-specific: classification and metering ---

    /**
     * @brief Add a multi-field classification rule.
     * @param rule the mark rule to append
     */
    void AddMarkRule(const MarkRule& rule);

    /**
     * @brief Get the policy classifier for external configuration.
     * @return the policy classifier
     */
    Ptr<diffserv::PolicyClassifier> GetPolicyClassifier() const;

    /**
     * @brief Install a per-flow policy classifier. When set, DoEnqueue extracts
     * the 5-tuple from the packet and routes through the per-flow
     * classifier instead of the DSCP-keyed one. Unregistered flows fall
     * through to the DSCP-keyed path.
     *
     * @param classifier the per-flow classifier to install (may be null to
     * disable)
     */
    void SetPerFlowClassifier(Ptr<diffserv::PerFlowPolicyClassifier> classifier);

    /**
     * @brief Get the installed per-flow classifier (or null if none).
     * @return the per-flow classifier (may be nullptr)
     */
    Ptr<diffserv::PerFlowPolicyClassifier> GetPerFlowClassifier() const;

    // --- Inner disc accessors (composition; multi-slot) ---

    /**
     * @brief Maximum number of parallel inner queueing pipelines the
     * edge can compose.
     *
     * Slot 0 carries the default inner; additional slots host
     * heterogeneous inners (e.g. L4S alongside Red) dispatched by
     * DSCP. See SetInnerDiscAt / SetDscpToSlot.
     */
    static constexpr uint32_t kMaxInnerSlots = 8;

    /**
     * @brief Install a pre-built inner queueing pipeline at slot 0.
     *
     * Convenience overload that anchors to `SetInnerDiscAt(0, inner)`.
     * With the default DSCP-to-slot map routing every code point to
     * slot 0, single-inner scenarios reach the inner exclusively
     * through this slot.
     *
     * Must be called before Initialize. Accepts any `Ptr<QueueDisc>`,
     * not Red-specifically, so callers can install `l4s::QueueDisc`
     * (or another composed disc) as the edge's queueing pipeline.
     */
    void SetInnerDisc(Ptr<QueueDisc> inner);

    /**
     * @brief Get the inner queueing pipeline at slot 0.
     *
     * Never nullptr once `EnsureDefaultInner` has run (via
     * `CheckConfig`). Returns `Ptr<QueueDisc>`; callers that need a
     * concrete subclass (`RedQueueDisc` / `l4s::QueueDisc` / etc.)
     * `DynamicCast` as needed. Convenience accessor for
     * `GetInnerDiscAt(0)`.
     */
    Ptr<QueueDisc> GetInnerDisc() const;

    /**
     * @brief Install an inner at the given slot.
     *
     * Must be called before Initialize. Slots fill monotonically: the
     * caller must populate slot `k` only after slot `k-1` has an inner.
     * Strict priority at dequeue time is indexed by slot (slot 0 highest,
     * slot N-1 lowest), so place the latency-sensitive inner (typically
     * L4S) at slot 0 and the background inner (Red / FqCoDel) at a
     * higher index.
     *
     * Asserts: `slot < kMaxInnerSlots`, `inner != nullptr`, slots fill
     * monotonically (no gaps).
     */
    void SetInnerDiscAt(uint32_t slot, Ptr<QueueDisc> inner);

    /**
     * @brief Slot accessor. Returns nullptr for unpopulated slots.
     */
    Ptr<QueueDisc> GetInnerDiscAt(uint32_t slot) const;

    /**
     * @brief Route a DSCP code point to an inner slot.
     *
     * Must be called before Initialize, after the target slot is
     * populated via `SetInnerDiscAt`. The default map sends every DSCP
     * to slot 0, which makes single-inner scenarios route through the
     * single populated slot.
     *
     * Asserts: `dscp < kMaxCodePoints`, `slot < kMaxInnerSlots`, and
     * `GetInnerDiscAt(slot) != nullptr`.
     */
    void SetDscpToSlot(uint8_t dscp, uint32_t slot);

    /**
     * @brief Get the slot index currently mapped for @p dscp.
     *
     * Default mapping is slot 0 for every code point until
     * `SetDscpToSlot` overrides it. Asserts `dscp < kMaxCodePoints`.
     *
     * @param dscp DSCP code point to query
     * @return slot index in `[0, kMaxInnerSlots)`
     */
    uint32_t GetDscpToSlot(uint8_t dscp) const;

    /**
     * @brief Count of populated inner slots.
     *
     * Slot 0 is always populated by `EnsureDefaultInner`, so this
     * returns `>= 1` post-Initialize. Pre-Initialize scenarios that
     * haven't installed any inner return 0.
     */
    uint32_t GetNumInnerSlots() const;

    /**
     * @brief Per-tin diagnostic counter snapshot — `tc -s qdisc show`
     * equivalent for a CAKE-composed edge.
     *
     * Delegates to the active `SlotDispatcher` (`cake::TinShaperDispatcher`
     * or `HybridLlqDispatcher` track per-tin counters; the default
     * `StrictPriorityDispatcher` returns zeroed counters). v1
     * surfaces wire-byte-enqueued / dequeued / drops / marks; sparse-
     * flow count is v1.1.
     *
     * @param slot the slot index (CAKE tin) to report; out-of-range
     *        values yield a zeroed snapshot
     * @return per-tin counters per `cake::TinStats`
     * @see specs/02-structural.md S-17.32
     */
    cake::TinStats GetTinStats(uint32_t slot) const;

    // --- Across-slot dispatch strategy ---

    /**
     * @brief Install a custom across-slot dispatch strategy.
     *
     * The default installed by the constructor is
     * `StrictPriorityDispatcher` — byte-identical strict-priority
     * `DoDequeue` loop, so existing scenarios observe no behaviour
     * change. Scenarios that want non-strict policy (CAKE's
     * DRR-across-tins, future WFQ / HTB) supply a subclass.
     *
     * Must be called before Initialize. Passing nullptr is invalid.
     *
     * @param dispatcher the dispatcher strategy to install
     */
    void SetSlotDispatcher(Ptr<SlotDispatcher> dispatcher);

    /**
     * @brief Read access to the active across-slot dispatch strategy.
     * @return the installed `SlotDispatcher`
     */
    Ptr<SlotDispatcher> GetSlotDispatcher() const;

    // --- Inner configuration ---
    //
    // Red-specific configuration (AddPhbEntry / ConfigQueue /
    // SetMredMode / SetNumQueues / SetNumPrec / SetQueueLimit /
    // SetMeanPacketSize / SetQueueBandwidth / SetScheduler /
    // LookupPhb) is not exposed on the edge composer. Callers
    // configure the inner via its own API before `SetInnerDisc`, or
    // unwrap via `GetInnerDisc()` with an explicit DynamicCast to the
    // concrete type. The edge forwarders are deliberately omitted to
    // keep the edge inner-agnostic.

    // --- Runtime probes (slot 0) ---
    //
    // GetScheduler and PrintPhbTable remain Red-specific (the scheduler
    // abstraction and PHB table are Red concepts). The other zero-arg
    // probes are convenience overloads that delegate to the per-slot
    // form at slot 0 and work with any inner that implements
    // `QueueStatsProvider`.

    /**
     * @brief Get the slot-0 scheduler (Red inner only).
     * @return the inner scheduler, or nullptr if inner is not Red
     */
    Ptr<Scheduler> GetScheduler() const;

    /**
     * @brief Convenience overload for `GetNumQueues(0)`.
     * @return number of top-level queues at slot 0
     */
    uint32_t GetNumQueues() const;

    /**
     * @brief Convenience overload for `GetVirtualQueueLen(0, queue, prec)`.
     * @param queue physical queue index
     * @param prec drop-precedence index
     * @return current virtual-queue length in packets
     */
    int GetVirtualQueueLen(uint32_t queue, uint32_t prec) const;

    /** @brief Convenience overload for `PrintStats(0)`. */
    void PrintStats() const;

    /** @brief Print the PHB (DSCP -> queue, prec) table to stdout. */
    void PrintPhbTable() const;

    // --- Per-slot overloads ---
    //
    // Multi-inner scenarios may want to probe slots other than 0.
    // These per-slot overloads carry the substantive bodies; they
    // delegate to the inner at the requested slot via
    // `QueueStatsProvider` (Red, L4S, and any future inner type that
    // implements the mixin). The zero-argument convenience overloads
    // above route here with slot=0. Unpopulated slots or inners that
    // do not implement `QueueStatsProvider` return conservative
    // defaults (0 / no-op).

    /**
     * @brief Queue count for the inner at the given slot.
     * @param slot slot index in `[0, kMaxInnerSlots)`.
     * @return number of per-queue indices exposed by that slot's inner
     * (0 if slot is unpopulated or inner lacks QueueStatsProvider).
     */
    uint32_t GetNumQueues(uint32_t slot) const;

    /**
     * @brief Per-slot virtual queue length.
     * @param slot slot index in `[0, kMaxInnerSlots)`.
     * @param queue physical queue index within that slot's inner.
     * @param prec drop precedence within that queue.
     * @return packet count on the (queue, prec) virtual queue
     * (0 if slot is unpopulated or inner lacks QueueStatsProvider).
     */
    int GetVirtualQueueLen(uint32_t slot, uint32_t queue, uint32_t prec) const;

    /**
     * @brief Per-slot stats print (no-op on unpopulated or foreign slot).
     * @param slot slot index in `[0, kMaxInnerSlots)`.
     */
    void PrintStats(uint32_t slot) const;

    /**
     * @brief Cascade RNG streams into m_inner's RedSubQueue children.
     *
     * The edge composer has no RNG of its own — the classifier,
     * meters, and mark-rule table are all deterministic — so the
     * cascade simply forwards verbatim into the inner's sub-queue
     * leaves where the RED RNGs live. Mirrors the
     * `l4s::QueueDisc::AssignStreams` discipline.
     *
     * @param stream starting RNG stream index
     * @return number of streams consumed (one per inner RED sub-queue)
     */
    int64_t AssignStreams(int64_t stream);

  protected:
    bool DoEnqueue(Ptr<QueueDiscItem> item) override;
    Ptr<QueueDiscItem> DoDequeue() override;
    Ptr<const QueueDiscItem> DoPeek() override;
    bool CheckConfig() override;
    void InitializeParams() override;
    void DoDispose() override;

  private:
    /**
     * @brief Classify a packet by matching against the mark rule table.
     *
     * Returns the initial DSCP for the first matching rule, or the
     * packet's existing DSCP if no rule matches ( passthrough).
     *
     * @param item queue disc item to classify
     * @return initial DSCP for the first matching rule (or the existing DSCP)
     */
    uint8_t Classify(Ptr<const QueueDiscItem> item) const;

    /** @brief Ensure m_inner exists and is registered as the sole QueueDiscClass. */
    void EnsureDefaultInner();

    MarkRule m_markRules[kMaxMarkRules];                        //!< Mark rule table
    int m_numMarkRules;                                         //!< Number of valid mark rules
    Ptr<diffserv::PolicyClassifier> m_policyClassifier;         //!< DSCP-keyed policier
    Ptr<diffserv::PerFlowPolicyClassifier> m_perFlowClassifier; //!< Optional per-flow path

    /**
     * @brief Inner queueing pipelines indexed by slot.
     *
     * m_inners[0] is the default slot; additional slots host
     * heterogeneous inners dispatched by DSCP. Populated slots fill
     * monotonically (no gaps). Queried via GetInnerDiscAt.
     */
    std::array<Ptr<QueueDisc>, kMaxInnerSlots> m_inners;

    /**
     * @brief DSCP-to-slot dispatch map.
     *
     * Defaults every code point to slot 0; override via SetDscpToSlot
     * to steer specific code points to other populated slots.
     */
    std::array<uint32_t, kMaxCodePoints> m_dscpToSlot;

    /**
     * @brief Across-slot dispatch strategy.
     *
     * Default-initialised to `StrictPriorityDispatcher` in the
     * constructor. Overridden via `SetSlotDispatcher` for CAKE
     * (DRR-across-tins) and future policies.
     */
    Ptr<SlotDispatcher> m_slotDispatcher;

    /**
     * @brief Egress DSCP wash flag (mirrors Linux `tc-cake wash`).
     *
     * When `true`, `DoDequeue` zeros the DSCP bits of the IPv4 TOS byte
     * before returning the dequeued item, leaving the low two ECN bits
     * untouched. Classification still drives inner-slot routing — only
     * the egress packet's DSCP byte is cleared, so downstream
     * forwarders see CS0/Default. Default `false`.
     */
    bool m_wash{false};

    /**
     * @brief Meter strategy pool indexed by MeterType.
     *
     * Lazy-created by GetMeter or pre-installed via SetMeter.
     */
    static constexpr std::size_t kMeterPoolSize = 7;
    Ptr<Meter> m_meters[kMeterPoolSize]; //!< Meter strategy pool (see kMeterPoolSize)
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_EDGE_QUEUE_DISC_H
