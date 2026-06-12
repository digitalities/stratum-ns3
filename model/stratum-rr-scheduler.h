/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsscheduler.h class dsRR (2001).
 */

#ifndef NS3_STRATUM_RR_SCHEDULER_H
#define NS3_STRATUM_RR_SCHEDULER_H

#include "stratum-scheduler.h"

namespace ns3::stratum
{

/**
 * @ingroup stratum
 *
 * @brief Round-Robin scheduler.
 *
 * Cycles through the physical queues in fixed index order, dequeueing
 * one packet from each non-empty queue per visit. Empty queues are
 * skipped. Work-conserving: returns -1 only when all queues are empty.
 *
 * Ported from DiffServ4NS `dsRR` (2001). The lowest-overhead
 * discipline in the suite; fairness is per-packet, not per-byte, so it
 * mis-serves flows whose packet-size distributions differ.
 *
 */
class RoundRobinScheduler : public Scheduler
{
  public:
    static TypeId GetTypeId();
    RoundRobinScheduler();
    ~RoundRobinScheduler() override;

    void Reset() override;
    void OnEnqueue(uint32_t queueIndex, uint32_t packetSizeBytes) override;
    int SelectNextQueue() override;

  private:
    int m_queueLen[kMaxQueues];
    int m_qToDq{-1};
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_RR_SCHEDULER_H
