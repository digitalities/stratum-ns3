/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsscheduler.cc class dsLLQ (2001).
 * Low Latency Queueing (Cisco): strict priority for queue 0,
 * configurable fair-queueing scheduler for queues 1..N-1.
 */

#ifndef NS3_STRATUM_LLQ_SCHEDULER_H
#define NS3_STRATUM_LLQ_SCHEDULER_H

#include "stratum-pq-scheduler.h"
#include "stratum-scheduler.h"

namespace ns3::stratum
{

/**
 * @ingroup stratum
 *
 * @brief Low Latency Queueing (LLQ) scheduler.
 *
 * Composes strict priority for queue 0 with a configurable
 * fair-queueing (PFQ) sub-scheduler for queues 1..N-1.
 *
 * Queue 0 is served by a PriorityScheduler (1 queue, winLen=1).
 * Queues 1..N-1 are served by one of WFQ, WF2Qp, SCFQ, or SFQ,
 * with indices offset by -1.
 *
 * SelectNextQueue() returns queue 0 whenever PQ has packets;
 * otherwise it polls the FQ sub-scheduler and offsets the result
 * back to the original numbering.
 *
 */
class LlqScheduler : public Scheduler
{
  public:
    /** @brief Inner fair-queueing variant served on queues 1..N-1. */
    enum class FqVariant
    {
        WFQ,
        WF2Qp,
        SCFQ,
        SFQ
    };

    /**
     * @brief Get the TypeId for this class.
     * @return the TypeId.
     */
    static TypeId GetTypeId();

    /** @brief Construct a LlqScheduler with default sub-schedulers. */
    LlqScheduler();

    ~LlqScheduler() override;

    void Reset() override;
    void OnEnqueue(uint32_t queueIndex, uint32_t packetSizeBytes) override;
    void OnEnqueueWithTime(uint32_t queueIndex,
                           uint32_t packetSizeBytes,
                           double nowSeconds) override;
    int SelectNextQueue() override;
    void SetParam(uint32_t queueIndex, double weight) override;

    /**
     * @brief Set the rate cap on the priority queue (queue 0).
     * @param rateBps maximum departure rate in bits per second
     */
    void SetPqRateCap(double rateBps);

    /**
     * @brief Attribute accessor: select the inner FQ variant.
     * @param v inner fair-queueing algorithm to use on queues 1..N-1
     */
    void SetFqVariant(FqVariant v);

    /**
     * @brief Attribute accessor: read the inner FQ variant.
     * @return currently-configured inner fair-queueing algorithm
     */
    FqVariant GetFqVariant() const;

  protected:
    void NotifyConstructionCompleted() override;

  private:
    FqVariant m_fqVariant{FqVariant::WFQ}; //!< Inner FQ algorithm for queues 1..N-1
    Ptr<PriorityScheduler> m_pq;           //!< PQ sub-scheduler for queue 0
    Ptr<Scheduler> m_pfq;                  //!< Fair-queueing sub-scheduler for queues 1..N-1
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_LLQ_SCHEDULER_H
