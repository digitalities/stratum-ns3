/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsscheduler.cc class dsWF2Qp (2001).
 * Worst-case Fair Weighted Fair Queueing+ (Bennett & Zhang, IEEE/ACM ToN 1997).
 */

#ifndef NS3_STRATUM_WF2QP_SCHEDULER_H
#define NS3_STRATUM_WF2QP_SCHEDULER_H

#include "stratum-scheduler.h"

#include <queue>

namespace ns3::stratum
{

/**
 * @ingroup stratum
 *
 * @brief WF2Q+ (Worst-case Fair Weighted Fair Queueing+) scheduler.
 *
 * Algorithm: Bennett & Zhang, "Hierarchical Packet Fair Queueing
 * Algorithms", IEEE/ACM Trans. Networking 5(5):675-689, October 1997.
 *
 * Key difference from SCFQ/SFQ: at any dequeue instant, only packets
 * whose virtual start time S <= V (system virtual time) are eligible
 * candidates. This prevents a flow from being served ahead of its
 * GPS-computed schedule, guaranteeing worst-case fairness.
 *
 * Per-flow state: a FIFO queue of packet sizes (bytes), current queue
 * size, weight, virtual start time S, and virtual finish time F.
 * S and F are recomputed when a flow transitions from empty to
 * non-empty (on enqueue) or when the head-of-line packet is dequeued.
 *
 * The system virtual time V advances using both the minimum S of
 * active flows and the real-time elapsed since the last dequeue,
 * weighted by the sum of active weights.
 *
 * @note Unlike SCFQ/SFQ, WF2Q+ reads Simulator::Now() inside
 * SelectNextQueue() to advance the virtual clock. Tests must
 * therefore use Simulator::Schedule() / Simulator::Run().
 *
 */
class Wf2qPlusScheduler : public Scheduler
{
  public:
    /**
     * @brief Get the TypeId for this class.
     * @return the TypeId.
     */
    static TypeId GetTypeId();

    /** @brief Construct a Wf2qPlusScheduler with default per-flow weights. */
    Wf2qPlusScheduler();

    ~Wf2qPlusScheduler() override;

    void Reset() override;
    void OnEnqueue(uint32_t queueIndex, uint32_t packetSizeBytes) override;
    void OnEnqueueWithTime(uint32_t queueIndex,
                           uint32_t packetSizeBytes,
                           double nowSeconds) override;
    int SelectNextQueue() override;
    void SetParam(uint32_t queueIndex, double weight) override;

  private:
    /** @brief Per-flow state for WF2Q+. */
    struct FlowState
    {
        std::queue<uint32_t> flowQueue; //!< FIFO of packet sizes (bytes)
        uint32_t qcrtSize{0};           //!< Current queue size in bytes
        double weight{1.0};             //!< Flow weight (> 0)
        double S{0.0};                  //!< Virtual start time
        double F{0.0};                  //!< Virtual finish time
    };

    FlowState m_flow[kMaxQueues]; //!< Per-flow state
    double m_V{0.0};              //!< System virtual time
    double m_lastTimeV{0.0};      //!< Real time of last V update (seconds)
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_WF2QP_SCHEDULER_H
