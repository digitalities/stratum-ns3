/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef NS3_STRATUM_WFQ_SCHEDULER_H
#define NS3_STRATUM_WFQ_SCHEDULER_H

#include "stratum-scheduler.h"

#include <queue>

namespace ns3::stratum
{

/**
 * @ingroup stratum
 *
 * @brief Weighted Fair Queueing (WFQ): packet-by-packet GPS approximation.
 *
 * Implements Packet-by-Packet Generalized Processor Sharing (PGPS) with
 * finish-tag service order, per Parekh & Gallager, "A Generalized
 * Processor Sharing Approach to Flow Control in Integrated Services
 * Networks: The Single-Node Case," IEEE/ACM Trans. Networking
 * 1(3):344-357, June 1993.
 *
 * **Virtual time `V(t)` (Eq. 10).** GPS virtual time advances at rate
 * `1 / Sum_{i in B(t)} phi_i` over busy-set epochs, where `B(t)` is the
 * set of sessions with non-empty backlog at time `t`. Implemented as a
 * piecewise-linear function recomputed on demand from a single snapshot
 * triple `(t_epoch, V_epoch, sumPhiBusy)`:
 *
 *     V(t) = V_epoch + (t - t_epoch) / sumPhiBusy
 *
 * The snapshot is refreshed every time the busy set changes (an arrival
 * into an empty session queue, or a dequeue that empties a session
 * queue). One subtraction and one division per query, no accumulation
 * across events.
 *
 * **Per-flow finish tag (Eq. 3).** For the k-th arrival of flow `i`:
 *
 *     F_i^k = max( F_i^{k-1}, V(t_arrival) ) + L_i^k / (phi_i * r)
 *
 * where `phi_i` is the flow weight, `r` is the link rate, and `L_i^k`
 * is the wire size (IP bytes + L2 framing). Tags are immutable from
 * enqueue to dequeue; a `std::queue<double>` per flow is sufficient
 * because the per-flow tag sequence is monotone non-decreasing by
 * construction.
 *
 * **Service order.** `SelectNextQueue` picks the backlogged flow with
 * minimum head-tag (argmin scan over `m_numQueues`) and pops it. `V(t)`
 * is not updated at dequeue; it advances continuously with real time
 * according to the busy-set epoch above.
 *
 * **Theorem 1 (Parekh-Gallager 1993, p. 347).** For all packets `p`,
 * `F_hat_p - F_p <= L_max / r`, where `F_p` is the GPS finish time and
 * `F_hat_p` is the PGPS finish time. Pure PGPS satisfies the bound
 * asymptotically; finite-window throughput shares may deviate by up to
 * `L_max` per flow during the busy-set startup transient. The
 * tighter per-packet variant that closes that startup gap is
 * `Wf2qPlusScheduler` (Bennett-Zhang, ToN 1997).
 */
class WfqScheduler : public Scheduler
{
  public:
    /**
     * @brief Get the TypeId for this class.
     * @return the TypeId.
     */
    static TypeId GetTypeId();

    /** @brief Construct a WfqScheduler with default per-flow weights. */
    WfqScheduler();

    ~WfqScheduler() override;

    void Reset() override;
    void OnEnqueue(uint32_t queueIndex, uint32_t packetSizeBytes) override;
    void OnEnqueueWithTime(uint32_t queueIndex,
                           uint32_t packetSizeBytes,
                           double nowSeconds) override;
    int SelectNextQueue() override;
    void SetParam(uint32_t queueIndex, double weight) override;

    /**
     * @brief Read the current virtual time `V(t)`.
     *
     * Computed at `Simulator::Now()` from the current busy-set epoch
     * snapshot. Monotone non-decreasing across calls.
     */
    double GetVirtualTime() const;

  protected:
    void DoDispose() override;

  private:
    /** @brief Per-flow state. */
    struct FlowState
    {
        std::queue<double> finishTags; //!< F_i^k tags, monotone non-decreasing per flow
        double weight{1.0};            //!< Flow weight phi_i (> 0)
        double finishT{0.0};           //!< Last computed F_i^k (max-clamp memory)
    };

    /**
     * @brief Compute V(t) at `nowSeconds` from the current busy-set
     * epoch snapshot.
     *
     * Returns `V_epoch + (nowSeconds - t_epoch) / sumPhiBusy`, or
     * `V_epoch` if `sumPhiBusy <= 0` (no flows backlogged - `V` is
     * frozen until the next arrival starts a new busy epoch).
     */
    double ComputeVirtualTime(double nowSeconds) const;

    /**
     * @brief Snapshot the current `(t_epoch, V_epoch)` at
     * `nowSeconds`, before mutating `sumPhiBusy`. Caller is
     * responsible for adjusting `sumPhiBusy` after the snapshot.
     */
    void SnapshotBusyEpoch(double nowSeconds);

    FlowState m_fs[kMaxQueues]; //!< Per-flow state

    double m_sumPhiBusy{0.0};      //!< Sum of phi_i over backlogged sessions
    double m_busyEpochStartT{0.0}; //!< Real time at start of current busy epoch
    double m_busyEpochStartV{0.0}; //!< V at start of current busy epoch
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_WFQ_SCHEDULER_H
