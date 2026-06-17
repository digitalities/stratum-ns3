/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsscheduler.h class dsScheduler (2001).
 */

#ifndef NS3_STRATUM_SCHEDULER_H
#define NS3_STRATUM_SCHEDULER_H

#include "stratum-constants.h"

#include "ns3/object.h"

namespace ns3::stratum
{

/**
 * @ingroup stratum
 *
 * @brief Abstract base class for DiffServ schedulers.
 *
 * Schedulers decide which physical queue to dequeue from next.
 * They receive notifications on enqueue (to update internal bookkeeping)
 * and are polled on dequeue to select the next queue.
 *
 * The base class provides a TSW-based departure rate estimator used by
 * PriorityScheduler for rate caps and available to all schedulers
 * for statistics.
 *
 */
class Scheduler : public Object
{
  public:
    /**
     * @brief Get the TypeId for this class.
     * @return the TypeId.
     */
    static TypeId GetTypeId();

    /** @brief Construct a Scheduler with default state. */
    Scheduler();

    ~Scheduler() override;

    /** @brief Reset per-queue bookkeeping to the zero state. */
    virtual void Reset();

    /**
     * @brief Notification that a packet has been enqueued.
     * @param queueIndex the queue that received the packet
     * @param packetSizeBytes packet size in bytes
     */
    virtual void OnEnqueue(uint32_t queueIndex, uint32_t packetSizeBytes) = 0;

    /**
     * @brief Select the next queue index to dequeue from.
     * @return the queue index, or -1 if no queue is eligible
     */
    virtual int SelectNextQueue() = 0;

    /**
     * @brief Set a scheduler-specific numeric parameter for one queue.
     * @param queueIndex queue index
     * @param value implementation-specific value (weight, priority, ...)
     */
    virtual void SetParam(uint32_t queueIndex, double value);

    /**
     * @brief Notification of an enqueue event with simulation time.
     *
     * Fair-queueing schedulers override this to update virtual-time
     * bookkeeping. The default implementation delegates to OnEnqueue().
     *
     * @param queueIndex the queue that received the packet
     * @param packetSizeBytes packet size in bytes
     * @param nowSeconds current simulation time in seconds
     */
    virtual void OnEnqueueWithTime(uint32_t queueIndex,
                                   uint32_t packetSizeBytes,
                                   double nowSeconds);

    /**
     * @brief Set the link bandwidth for finish-time computation.
     * @param bandwidthBps link bandwidth in bits per second
     */
    void SetLinkBandwidth(double bandwidthBps);

    /**
     * @brief Get the configured link bandwidth.
     * @return link bandwidth in bits per second
     */
    double GetLinkBandwidth() const;

    /**
     * @brief Set the TSW window length.
     * @param winLenSeconds window length in seconds (> 0)
     *
     * Exposed so derived classes (PriorityScheduler) can register
     * it as an attribute without breaking the protected-access rule
     * on pointer-to-member from a static context.
     */
    void SetWinLen(double winLenSeconds);

    /**
     * @brief Get the TSW window length in seconds.
     * @return window length in seconds
     */
    double GetWinLen() const;

    /**
     * @brief Set the L2 framing overhead bytes per packet.
     * @param bytes per-packet L2 overhead (PPP=2, Ethernet=14,
     * SimpleLink=0). Default 0.
     *
     * FQ subclasses (SCFQ/SFQ/WFQ/WF2Q+/LLQ-inner) add this to the
     * IP-layer packet size before computing virtual-time / finish-time
     * increments, so the scheduler reasons in WIRE bytes — the byte
     * basis the link physically consumes. Strict-priority does not
     * use it. Mirrors Linux tc-stab, Cisco MQC `bandwidth` (L2 by
     * default), Juniper `account-layer-overhead`. The companion
     * meter/policer attribute must be set to the same value so meter
     * and scheduler agree on byte basis.
     *
     */
    void SetL2OverheadBytes(uint32_t bytes);

    /**
     * @brief Get the L2 framing overhead bytes per packet.
     * @return per-packet L2 overhead in bytes
     */
    uint32_t GetL2OverheadBytes() const;

    /**
     * @brief Update the TSW departure-rate estimator after a dequeue.
     * @param queueIndex queue that was dequeued
     * @param prec drop-precedence index
     * @param packetSizeBytes packet size in bytes
     * @param nowSeconds current simulation time in seconds
     */
    virtual void UpdateDepartureRate(uint32_t queueIndex,
                                     uint32_t prec,
                                     uint32_t packetSizeBytes,
                                     double nowSeconds);

    /**
     * @brief Get the estimated departure rate for (queue, prec).
     * @param queueIndex queue index
     * @param prec drop-precedence index
     * @return estimated departure rate in bytes per second
     */
    double GetDepartureRate(uint32_t queueIndex, int prec) const;

  protected:
    uint32_t m_numQueues{1};       //!< Number of queues the scheduler manages
    double m_winLen{1.0};          //!< TSW window length (seconds)
    double m_linkBandwidth{0.0};   //!< Link bandwidth in bits/s (0 = not set)
    uint32_t m_l2OverheadBytes{0}; //!< L2 framing overhead per packet

    double m_queueAvgRate[kMaxQueues]; //!< Per-queue EWMA departure rate
    double m_queueArrTime[kMaxQueues]; //!< Per-queue last-departure time

    double m_qpAvgRate[kMaxQueues][kMaxPrec]; //!< Per-(queue,prec) EWMA departure rate
    double m_qpArrTime[kMaxQueues][kMaxPrec]; //!< Per-(queue,prec) last-departure time

  private:
    /**
     * @brief TSW update step shared by per-queue and per-(queue,prec) rate
     * estimators.
     *
     * @param packetSizeBytes packet size in bytes
     * @param avgRate in/out EWMA rate estimate pointer
     * @param arrTime in/out last-event time pointer
     * @param nowSeconds current simulation time in seconds
     */
    void ApplyTswMeter(uint32_t packetSizeBytes,
                       double* avgRate,
                       double* arrTime,
                       double nowSeconds) const;
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_SCHEDULER_H
