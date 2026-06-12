/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsscheduler.h class dsSCFQ (2001).
 * Self-Clocked Fair Queueing (Golestani 1994).
 */

#ifndef NS3_STRATUM_SCFQ_SCHEDULER_H
#define NS3_STRATUM_SCFQ_SCHEDULER_H

#include "stratum-scheduler.h"

#include <ostream>
#include <queue>

namespace ns3::stratum
{

/**
 * @ingroup stratum
 *
 * @brief Self-Clocked Fair Queueing (SCFQ) scheduler.
 *
 * Uses the finish time of the currently-in-service packet as system
 * virtual time. No GPS reference clock, no event scheduling, no
 * real-time dependency.
 *
 * Per-queue state: a finish label (monotonically non-decreasing), a
 * session queue (FIFO of finish labels), and a weight. The system
 * virtual time (tlabel) is the finish label of the last dequeued packet.
 *
 * On enqueue: label[qi] = max(label[qi], tlabel) + pktBytes / (weight[qi] *
 * bwBytes) On dequeue: select the non-empty queue with the smallest front label
 * (lower index wins ties).
 *
 */
class ScfqScheduler : public Scheduler
{
  public:
    /**
     * @brief Get the TypeId for this class.
     * @return the TypeId.
     */
    static TypeId GetTypeId();

    /** @brief Construct a ScfqScheduler with default per-queue weights. */
    ScfqScheduler();

    ~ScfqScheduler() override;

    void Reset() override;
    void OnEnqueue(uint32_t queueIndex, uint32_t packetSizeBytes) override;
    void OnEnqueueWithTime(uint32_t queueIndex,
                           uint32_t packetSizeBytes,
                           double nowSeconds) override;
    int SelectNextQueue() override;
    void SetParam(uint32_t queueIndex, double weight) override;

    /**
     * @brief Enable per-packet decision log to a stream.
     * @param os output stream (caller manages lifetime)
     *
     * Writes CSV:
     * time,event,queue,pktSize,efLabel,beLabel,tlabel,efQdepth,beQdepth,newLabel
     */
    void SetLogStream(std::ostream* os);

  private:
    /** @brief Per-queue session state for SCFQ. */
    struct Session
    {
        double label{0.0};               //!< Last computed finish label
        std::queue<double> sessionQueue; //!< FIFO of finish labels
        double weight{1.0};              //!< Queue weight (> 0)
    };

    Session m_session[kMaxQueues];      //!< Per-queue session state
    double m_tlabel{0.0};               //!< System virtual time
    std::ostream* m_logStream{nullptr}; //!< Per-packet decision log (optional)
    uint64_t m_deqCount[kMaxQueues]{};  //!< Cumulative dequeue count per queue
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_SCFQ_SCHEDULER_H
