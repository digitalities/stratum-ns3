/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsscheduler.h class dsWRR (2001).
 */

#ifndef NS3_STRATUM_WRR_SCHEDULER_H
#define NS3_STRATUM_WRR_SCHEDULER_H

#include "stratum-scheduler.h"

#include <cmath>

namespace ns3::stratum
{

/**
 * @ingroup stratum
 *
 * @brief Weighted Round-Robin (WRR) scheduler.
 *
 * Assigns each physical queue an integer weight; in one RR cycle queue
 * `i` receives up to `weight_i` consecutive dequeues before the
 * scheduler advances to queue `i + 1`. Weights are per-packet, not
 * per-byte — WRR is fair only when all flows have equal packet sizes.
 *
 * The consecutive-dequeue pattern produces a bursty output schedule
 * (e.g. weights `{3, 1}` yield `A A A B | A A A B | ...`); for the
 * smoother interleaved variant see `WeightedInterleavedRoundRobinScheduler`.
 *
 * Ported from DiffServ4NS `dsWRR` (2001). @see
 * and .
 */
class WeightedRoundRobinScheduler : public Scheduler
{
  public:
    static TypeId GetTypeId();
    WeightedRoundRobinScheduler();
    ~WeightedRoundRobinScheduler() override;

    void Reset() override;
    void OnEnqueue(uint32_t queueIndex, uint32_t packetSizeBytes) override;
    int SelectNextQueue() override;
    void SetParam(uint32_t queueIndex, double weight) override;

  private:
    int m_queueLen[kMaxQueues];
    int m_queueWeight[kMaxQueues];
    int m_wirrTemp[kMaxQueues];
    int m_qToDq{0};
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_WRR_SCHEDULER_H
