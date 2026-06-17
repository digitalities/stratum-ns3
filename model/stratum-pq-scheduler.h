/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsscheduler.h class dsPQ (2001).
 */

#ifndef NS3_STRATUM_PQ_SCHEDULER_H
#define NS3_STRATUM_PQ_SCHEDULER_H

#include "stratum-scheduler.h"

namespace ns3::stratum
{

/**
 * @ingroup stratum
 *
 * @brief Strict Priority Queueing (PQ) scheduler with optional rate caps.
 *
 * Always services the lowest-indexed non-empty queue — queue 0 has the
 * highest priority. Lower-priority queues are starved whenever higher-
 * priority queues have packets to send.
 *
 * Each queue may carry an optional rate cap (`SetParam(i, maxRateBps)`
 * with maxRateBps > 0). When a cap is set, the base-class TSW
 * departure-rate estimator monitors the queue's measured rate; the
 * scheduler skips queue `i` for one selection whenever its TSW estimate
 * exceeds the cap. A cap of 0 means uncapped (the default).
 *
 * The rate-cap mechanism makes PQ safe as the inner scheduler of an EF
 * class (RFC 3246) — expedited-forwarding traffic is guaranteed
 * precedence but cannot starve lower-priority queues beyond its
 * configured CIR.
 *
 * Ported from DiffServ4NS `dsPQ` (2001). @see
 * and .
 */
class PriorityScheduler : public Scheduler
{
  public:
    /**
     * @brief Get the TypeId for this class.
     * @return the TypeId.
     */
    static TypeId GetTypeId();

    /** @brief Construct a PriorityScheduler with no rate caps set. */
    PriorityScheduler();

    ~PriorityScheduler() override;

    void Reset() override;
    void OnEnqueue(uint32_t queueIndex, uint32_t packetSizeBytes) override;
    int SelectNextQueue() override;
    void SetParam(uint32_t queueIndex, double maxRateBps) override;

    /**
     * @brief Decline (return -1) instead of serving an over-cap queue when no
     * queue is under its cap.
     *
     * The standalone scheduler is work-conserving: if every non-empty queue is
     * over its cap it still serves the highest-priority one. When this priority
     * scheduler is the inner EF lane of an LLQ, that fallback would defeat the
     * cap — the composing LLQ needs the inner lane to decline so it can serve
     * the fair lanes instead (RFC 3246 EF policing). Enable this mode only in
     * that composition.
     *
     * @param yield true to decline when all queues are empty-or-over-cap
     */
    void SetYieldWhenRateCapped(bool yield);

  private:
    double m_queueMaxRate[kMaxQueues]; //!< Rate cap in bytes/s (0 = no cap)
    int m_queueLen[kMaxQueues];        //!< Per-queue occupancy tracked via OnEnqueue
    bool m_yieldWhenRateCapped{false}; //!< Inner-LLQ mode: decline when over cap
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_PQ_SCHEDULER_H
