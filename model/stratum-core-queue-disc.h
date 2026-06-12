/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * Copyright (C) 2000 Nortel Networks
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsCore.{h,cc} class coreQueue (2001).
 */

#ifndef NS3_STRATUM_CORE_QUEUE_DISC_H
#define NS3_STRATUM_CORE_QUEUE_DISC_H

#include "stratum-constants.h"
#include "stratum-red-queue-disc.h"
#include "stratum-scheduler.h"

#include "ns3/queue-disc.h"

namespace ns3::stratum
{

/**
 * @ingroup stratum
 *
 * @brief DiffServ core router queue disc — composition over RedQueueDisc.
 *
 * The core router queue disc descends from `QueueDisc` and composes
 * a single `Ptr<RedQueueDisc> m_inner`. Unlike the edge composer
 * the core adds no classification or metering — it is a pure BA
 * (DSCP-keyed) delegator that relies on the upstream edge router to
 * have set the DSCP.
 *
 * The thesis §3.3.1 split between edge (MF+meter) and core (BA only)
 * is preserved: the typed distinction is meaningful for topology
 * helpers and for future extension points (e.g. a core variant that
 * composes a l4s::QueueDisc instead of a RedQueueDisc).
 *
 * Port of ns-2 coreQueue from dsCore.{h,cc}.
 *
 */
class CoreQueueDisc : public QueueDisc
{
  public:
    /**
     * @brief Get the TypeId for this class.
     * @return the TypeId.
     */
    static TypeId GetTypeId();

    /** @brief Construct a CoreQueueDisc with a default inner disc. */
    CoreQueueDisc();

    ~CoreQueueDisc() override;

    // --- Inner disc accessor ---

    /**
     * @brief Install a pre-built inner queueing pipeline (replaces the default).
     *
     * Must be called before Initialize. The parameter is
     * `Ptr<QueueDisc>` (not Red-specific) for symmetry with the edge
     * disc; Red-specific forwarders assert via `DynamicCast` on
     * non-Red inners.
     *
     * @param inner pre-built inner queueing pipeline
     */
    void SetInnerDisc(Ptr<QueueDisc> inner);

    /**
     * @brief Get the composed inner queueing pipeline.
     * @return the inner queueing pipeline (never nullptr post-Initialize)
     */
    Ptr<QueueDisc> GetInnerDisc() const;

    // --- Inner configuration ---
    //
    // Red-specific configuration is not exposed on the core composer.
    // Callers configure the inner via its own API before
    // `SetInnerDisc`, or use `helper.InstallRedInner(core)` to create
    // + wire an inner in one shot.

    // --- Runtime probes: queue-stats accessors via the
    // `QueueStatsProvider` interface. ---

    /**
     * @brief Get the inner scheduler.
     * @return the inner scheduler, or nullptr if no scheduler is installed
     */
    Ptr<Scheduler> GetScheduler() const;

    /**
     * @brief Get the inner physical-queue count.
     * @return the number of physical queues in the inner pipeline
     */
    uint32_t GetNumQueues() const;

    /**
     * @brief Get the inner virtual-queue length at (queue, prec).
     * @param queue physical-queue index
     * @param prec drop-precedence index
     * @return current virtual-queue length in packets (or 0 if out of range)
     */
    int GetVirtualQueueLen(uint32_t queue, uint32_t prec) const;

    /** @brief Print a stats summary for the inner pipeline to stdout. */
    void PrintStats() const;

    /** @brief Print the PHB (DSCP -> queue, prec) table to stdout. */
    void PrintPhbTable() const;

    /**
     * @brief Cascade RNG streams into m_inner's RedSubQueue children.
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
    /** @brief Ensure m_inner exists and is registered as the sole QueueDiscClass. */
    void EnsureDefaultInner();

    //!< Composed inner queueing pipeline (typed as the generic
    //!< `Ptr<QueueDisc>`, not the Red-specific subtype)
    Ptr<QueueDisc> m_inner;

    static constexpr uint32_t kInnerChildIdx = 0; //!< Child-slot index of m_inner
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_CORE_QUEUE_DISC_H
