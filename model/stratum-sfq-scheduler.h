/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsscheduler.cc class dsSFQ (2001).
 * Start-time Fair Queueing (Goyal, Vin, Cheng, 1997).
 */

#ifndef NS3_STRATUM_SFQ_SCHEDULER_H
#define NS3_STRATUM_SFQ_SCHEDULER_H

#include "stratum-scheduler.h"

#include <queue>

namespace ns3::stratum
{

/**
 * @ingroup stratum
 *
 * @brief Start-time Fair Queueing (SFQ) scheduler.
 *
 * Algorithm: Goyal, Vin, Cheng, "Start-time Fair Queueing", 1997.
 *
 * Key difference from SCFQ: dequeue selects by minimum *start* tag
 * (not finish tag), and the system virtual time V advances to the
 * start tag of the served packet.
 *
 * Per-flow state: lastFinishTag (monotonically non-decreasing),
 * a flow queue (FIFO of {startTag, finishTag} pairs), and a weight.
 *
 * On enqueue: startTag = max(V, lastFinishTag[i])
 * finishTag = startTag + pktBytes / (weight[i] * bwBytes)
 * On dequeue: select the non-empty queue with the smallest front startTag
 * (lower index wins ties). V = selected startTag.
 *
 */
class SfqScheduler : public Scheduler
{
  public:
    /**
     * @brief Get the TypeId for this class.
     * @return the TypeId.
     */
    static TypeId GetTypeId();

    /** @brief Construct a SfqScheduler with default per-flow weights. */
    SfqScheduler();

    ~SfqScheduler() override;

    void Reset() override;
    void OnEnqueue(uint32_t queueIndex, uint32_t packetSizeBytes) override;
    void OnEnqueueWithTime(uint32_t queueIndex,
                           uint32_t packetSizeBytes,
                           double nowSeconds) override;
    int SelectNextQueue() override;
    void SetParam(uint32_t queueIndex, double weight) override;

  private:
    /** @brief Per-packet tag pair stored in the flow queue. */
    struct PacketTags
    {
        double startTag;  //!< Start tag (virtual start time)
        double finishTag; //!< Finish tag (virtual finish time)
    };

    /** @brief Per-flow state for SFQ. */
    struct FlowState
    {
        double lastFinishTag{0.0};        //!< Last computed finish tag
        std::queue<PacketTags> flowQueue; //!< FIFO of {startTag, finishTag}
        double weight{1.0};               //!< Flow weight (> 0)
    };

    FlowState m_flow[kMaxQueues]; //!< Per-flow state
    double m_V{0.0};              //!< System virtual time
    double m_maxFinishTag{0.0};   //!< Maximum finish tag seen
    bool m_idle{true};            //!< True when all queues are empty
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_SFQ_SCHEDULER_H
