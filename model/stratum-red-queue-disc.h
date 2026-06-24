/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * Copyright (C) 2000 Nortel Networks
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsred.h class dsREDQueue (2001).
 */

#ifndef NS3_STRATUM_RED_QUEUE_DISC_H
#define NS3_STRATUM_RED_QUEUE_DISC_H

#include "stratum-constants.h"
#include "stratum-phb-table.h"
#include "stratum-queue-stats-provider.h"
#include "stratum-red-sub-queue.h"
#include "stratum-scheduler.h"

#include "ns3/boolean.h"
#include "ns3/queue-disc.h"
#include "ns3/traced-callback.h"

namespace ns3::stratum
{

/**
 * @ingroup stratum
 * @brief RED threshold configuration for one (queue, prec) virtual queue.
 */
struct RedQueueConfig
{
    uint32_t queue; //!< physical queue index
    uint32_t prec;  //!< drop-precedence level
    double thMin;   //!< RED minimum threshold (packets)
    double thMax;   //!< RED maximum threshold (packets)
    double maxP;    //!< maximum drop probability
};

/**
 * @ingroup stratum
 *
 * @brief Classful DiffServ RED queue disc with per-queue MRED and pluggable
 * schedulers.
 *
 * This is the outer multi-queue disc that holds N RedSubQueue children
 * (each wrapped in a QueueDiscClass), a PHB table mapping DSCP code points
 * to (queue, precedence) pairs, and a pluggable Scheduler.
 *
 * Port of ns-2 dsREDQueue from dsred.{h,cc}.
 *
 */
class RedQueueDisc : public QueueDisc, public QueueStatsProvider
{
  public:
    /**
     * @brief Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId();

    /** @brief Construct a RedQueueDisc with default configuration. */
    RedQueueDisc();

    ~RedQueueDisc() override;

    // --- PHB Table ---

    /**
     * @brief Add a PHB table entry mapping a code point to a (queue, prec) pair.
     * @param codePt DSCP code point [0, kMaxCodePoints)
     * @param queue physical queue index [0, kMaxQueues)
     * @param prec drop precedence / virtual queue index [0, kMaxPrec)
     */
    void AddPhbEntry(uint8_t codePt, uint8_t queue, uint8_t prec);

    /**
     * @brief Look up the PHB table for a given code point.
     * @param codePt DSCP code point to look up
     * @param[out] queue physical queue index
     * @param[out] prec drop precedence
     * @return true if found, false otherwise
     */
    bool LookupPhb(uint8_t codePt, uint8_t& queue, uint8_t& prec) const;

    // --- Scheduler ---

    /**
     * @brief Set the scheduling discipline.
     * @param scheduler the scheduler to use
     */
    void SetScheduler(Ptr<Scheduler> scheduler);

    /**
     * @brief Get the current scheduler.
     * @return the current scheduler
     */
    Ptr<Scheduler> GetScheduler() const;

    // --- Configuration ---

    /**
     * @brief Set the number of physical queues.
     * @param numQueues number of queues [1, kMaxQueues]
     */
    void SetNumQueues(uint32_t numQueues);

    /**
     * @brief Get the number of physical queues.
     * @return the number of queues
     */
    uint32_t GetNumQueues() const override;

    /**
     * @brief Configure RED thresholds for a (queue, prec) virtual queue.
     * @param cfg RED threshold configuration
     */
    void ConfigQueue(const RedQueueConfig& cfg);

    /**
     * @brief Set the MRED mode for one physical queue.
     * @param mode the MRED mode
     * @param queue physical queue index
     */
    void SetMredMode(MredMode mode, uint32_t queue);

    /**
     * @brief Set the MRED mode for every physical queue.
     * @param mode the MRED mode
     */
    void SetMredModeAllQueues(MredMode mode);

    /**
     * @brief Set the number of precedence levels for a given queue.
     * @param queue physical queue index
     * @param numPrec number of precedence levels
     */
    void SetNumPrec(uint32_t queue, uint32_t numPrec);

    /**
     * @brief Set the buffer limit (packets) for a given queue.
     * @param queue physical queue index
     * @param limit maximum number of packets
     */
    void SetQueueLimit(uint32_t queue, uint32_t limit);

    /**
     * @brief Set the mean packet size for all sub-queues.
     * @param mps mean packet size in bytes
     */
    void SetMeanPacketSize(int mps);

    /**
     * @brief Set the link bandwidth used by RED for a specific queue.
     *
     * Computes the packet time constant (ptc) from the given bandwidth
     * and mean packet size. Equivalent to ns-2's setQueueBW command.
     *
     * @param queue physical queue index
     * @param bandwidthBps link bandwidth in bits per second
     */
    void SetQueueBandwidth(uint32_t queue, double bandwidthBps);

    /**
     * @brief Get the virtual (per-precedence) queue length within a physical
     * queue.
     *
     * Returns the number of packets attributed to a given precedence level
     * within a physical queue, as tracked by the MRED algorithm.
     *
     * @param queue physical queue index
     * @param prec precedence level
     * @return number of packets in the virtual queue
     */
    int GetVirtualQueueLen(uint32_t queue, uint32_t prec) const;

    /**
     * @brief QueueStatsProvider override — forwards to GetVirtualQueueLen.
     * @param queue physical queue index
     * @param prec drop-precedence index
     * @return current virtual-queue length in packets
     */
    int GetQueueLen(uint32_t queue, uint32_t prec) const override
    {
        return GetVirtualQueueLen(queue, prec);
    }

    // --- Statistics ---

    /**
     * @brief Print per-CP and aggregate packet statistics.
     */
    void PrintStats() const override;

    /**
     * @brief Print the PHB table.
     */
    void PrintPhbTable() const;

  protected:
    bool DoEnqueue(Ptr<QueueDiscItem> item) override;
    Ptr<QueueDiscItem> DoDequeue() override;
    Ptr<const QueueDiscItem> DoPeek() override;
    bool CheckConfig() override;
    void InitializeParams() override;

    /**
     * @brief Release the scheduler before destruction.
     *
     * Clears `m_scheduler` so the `Ptr<>` cycle (if any future scheduler
     * back-refs are added) is broken before `QueueDisc` base teardown.
     */
    void DoDispose() override;

    /**
     * @brief Extract the DSCP code point from a queue disc item.
     * @param item the queue disc item
     * @return the DSCP value as a uint8_t, or 0 if not IPv4
     */
    uint8_t GetCodePoint(Ptr<const QueueDiscItem> item) const;

    /**
     * @brief Enqueue a packet using an explicit code point (bypasses header
     * read).
     *
     * Used by EdgeQueueDisc which computes the code point from
     * classification + metering rather than reading the IPv4 header.
     *
     * @param item the queue disc item
     * @param codePt the DSCP code point to use for PHB lookup
     * @return true if enqueued, false if dropped
     */
    bool EnqueueWithCodePoint(Ptr<QueueDiscItem> item, uint8_t codePt);

    /**
     * @brief Get the RedSubQueue child at the given index.
     * @param index the child index
     * @return a pointer to the sub-queue
     */
    Ptr<RedSubQueue> GetSubQueue(uint32_t index) const;

    Ptr<Scheduler> m_scheduler; //!< The scheduling discipline
    uint32_t m_numQueues;       //!< Number of physical queues

  public:
    /**
     * @brief TracedCallback signature for StratumEnqueue.
     * @param item the enqueued queue disc item
     * @param dscp the DSCP code point used for PHB lookup
     */
    typedef void (*EnqueueTracedCallback)(Ptr<const QueueDiscItem> item, uint8_t dscp);

    /**
     * @brief TracedCallback signature for StratumDequeue.
     * @param item the dequeued queue disc item
     * @param dscp the DSCP code point
     * @param queueIndex the physical queue the packet was dequeued from
     */
    typedef void (*DequeueTracedCallback)(Ptr<const QueueDiscItem> item,
                                          uint8_t dscp,
                                          uint32_t queueIndex);

    /**
     * @brief TracedCallback signature for StratumDrop.
     * @param item the dropped queue disc item
     * @param dscp the DSCP code point
     * @param dropReason 0 = RED early drop, 1 = tail drop
     */
    typedef void (*DropTracedCallback)(Ptr<const QueueDiscItem> item,
                                       uint8_t dscp,
                                       uint32_t dropReason);

  private:
    PhbTable m_phb;                       //!< PHB classification table (extracted helper)
    bool m_ecn;                           //!< ECN enabled flag
    uint32_t m_perQueueLimit[kMaxQueues]; //!< Per-queue buffer limits (packets)

    /** @brief Packet statistics counters. */
    struct Stats
    {
        int drops;                     //!< Total tail drops
        int edrops;                    //!< Total early drops
        int pkts;                      //!< Total packets received
        int drops_cp[kMaxCodePoints];  //!< Tail drops per code point
        int edrops_cp[kMaxCodePoints]; //!< Early drops per code point
        int pkts_cp[kMaxCodePoints];   //!< Packets received per code point

        /** @brief Construct a zeroed Stats counter. */
        Stats();
    } m_stats; //!< Statistics counters

    //!< Traced callback fired after a packet is successfully enqueued
    TracedCallback<Ptr<const QueueDiscItem>, uint8_t> m_dsEnqueueTrace;
    //!< Traced callback fired after a packet is dequeued
    TracedCallback<Ptr<const QueueDiscItem>, uint8_t, uint32_t> m_dsDequeueTrace;
    //!< Traced callback fired when a packet is dropped (early or tail)
    TracedCallback<Ptr<const QueueDiscItem>, uint8_t, uint32_t> m_dsDropTrace;
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_RED_QUEUE_DISC_H
