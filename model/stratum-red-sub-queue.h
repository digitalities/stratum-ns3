/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * Copyright (C) 2000 Nortel Networks
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsredq.h class redQueue (2001).
 */

#ifndef NS3_STRATUM_RED_SUB_QUEUE_H
#define NS3_STRATUM_RED_SUB_QUEUE_H

#include "stratum-constants.h"

#include "ns3/queue-disc.h"
#include "ns3/random-variable-stream.h"

namespace ns3::stratum
{

/**
 * @ingroup stratum
 *
 * @brief RED parameters for one virtual queue (one drop-precedence level).
 * Direct port of ns-2 qParam struct from dsredq.h.
 */
struct VirtualQueueParams
{
    double thMin;    //!< RED minimum threshold (packets)
    double thMax;    //!< RED maximum threshold (packets)
    double maxP;     //!< Maximum drop probability
    double qW;       //!< EWMA weight (default 0.002)
    double ptc;      //!< Packet time constant (pkts/s)
    int meanPktSize; //!< Mean packet size in bytes

    double vAve;     //!< Weighted average queue length
    double vProb;    //!< Current drop probability
    int count;       //!< Packets since last drop
    int qlen;        //!< Virtual queue length (packets)
    double idletime; //!< Time when virtual queue became idle (seconds)
    bool idle;       //!< Whether virtual queue is idle

    /** @brief Construct with ns-2 default RED parameters. */
    VirtualQueueParams();
};

/**
 * @ingroup stratum
 *
 * @brief One physical queue with per-precedence RED (RIO).
 *
 * Models a single physical queue containing up to kMaxPrec virtual queues.
 * All precedence levels share the physical buffer; each has independent
 * RED parameters and EWMA average queue length.
 *
 */
class RedSubQueue : public QueueDisc
{
  public:
    /**
     * @brief Get the TypeId for this class.
     * @return the TypeId.
     */
    static TypeId GetTypeId();

    /** @brief Construct a RedSubQueue with ns-2 default RED parameters. */
    RedSubQueue();

    ~RedSubQueue() override;

    /**
     * @brief Configure RED thresholds for one virtual queue.
     * @param prec precedence level [0, kMaxPrec)
     * @param thMin RED minimum threshold (packets)
     * @param thMax RED maximum threshold (packets)
     * @param maxP maximum drop probability
     */
    void ConfigureVirtualQueue(uint32_t prec, double thMin, double thMax, double maxP);

    /**
     * @brief Set the number of active precedence levels.
     * @param numPrec number of precedence levels [1, kMaxPrec]
     */
    void SetNumPrec(uint32_t numPrec);

    /**
     * @brief Get the number of active precedence levels.
     * @return the number of active precedence levels
     */
    uint32_t GetNumPrec() const;

    /**
     * @brief Set the MRED operating mode.
     * @param mode the MRED mode to install
     */
    void SetMredMode(MredMode mode);

    /**
     * @brief Get the MRED operating mode.
     * @return the current MRED mode
     */
    MredMode GetMredMode() const;

    /**
     * @brief Set the physical queue limit (packets).
     * @param limit buffer limit in packets
     */
    void SetQueueLimit(uint32_t limit);

    /**
     * @brief Get the physical queue limit.
     * @return the buffer limit in packets
     */
    uint32_t GetQueueLimit() const;

    /**
     * @brief Set the packet time constant from link bandwidth.
     * @param linkBandwidthBps link bandwidth in bits per second
     */
    void SetPtc(double linkBandwidthBps);

    /**
     * @brief Set mean packet size for all virtual queues.
     * @param mps mean packet size in bytes
     */
    void SetMeanPacketSize(int mps);

    /**
     * @brief Enqueue a packet with a given precedence level.
     *
     * Called by the outer RedQueueDisc. Stashes prec/ecn into member
     * variables, then delegates to QueueDisc::Enqueue() which calls DoEnqueue().
     *
     * @param item the queue disc item to enqueue
     * @param prec drop-precedence level [0, kMaxPrec)
     * @param ecn whether ECN marking is enabled for this packet
     * @return the result of the enqueue attempt
     */
    PktResult EnqueueWithPrec(Ptr<QueueDiscItem> item, uint32_t prec, bool ecn);

    /**
     * @brief Initialize RED state variables for all virtual queues.
     * @param nowSeconds current simulation time in seconds
     */
    void InitRedStateVars(double nowSeconds);

    /**
     * @brief Update RED state variables after dequeuing a packet.
     * @param prec the precedence level of the dequeued packet
     * @param nowSeconds current simulation time in seconds
     */
    void UpdateRedStateVar(uint32_t prec, double nowSeconds);

    /**
     * @brief Get the weighted average queue length across all precedence levels.
     * @return the weighted average queue length (packets)
     */
    double GetWeightedLength() const;

    /**
     * @brief Get the current physical queue length.
     * @return the current queue length in packets
     */
    int GetRealLength() const;

    /**
     * @brief Get the virtual (per-precedence) queue length.
     * @param prec precedence level
     * @return the virtual queue length in packets
     */
    int GetVirtualQueueLen(uint32_t prec) const;

    /**
     * @brief Assign a dedicated RNG stream to this sub-queue.
     *
     * Required by @c l4s::QueueDisc's @c AssignStreams cascade: the ns-3
     * @c QueueDisc base does not cascade streams, and the RNG that drives
     * RED drop decisions lives here in the leaf. Composers walk their
     * children and call this one-shot.
     *
     * @param stream stream index to assign (same convention as ns-3
     * @c AssignStreams)
     * @return number of streams consumed (always 1)
     */
    int64_t AssignStreams(int64_t stream);

  protected:
    bool DoEnqueue(Ptr<QueueDiscItem> item) override;
    Ptr<QueueDiscItem> DoDequeue() override;
    Ptr<const QueueDiscItem> DoPeek() override;
    bool CheckConfig() override;
    void InitializeParams() override;

  private:
    /**
     * @brief Calculate the EWMA average queue length for a virtual queue.
     * @param prec precedence level
     * @param m number of idle-time decay steps + 1
     */
    void CalcAvg(uint32_t prec, int m);

    uint32_t m_numPrec;  //!< Number of active precedence levels
    uint32_t m_qlim;     //!< Physical queue limit (packets)
    int m_qlen;          //!< Current physical queue length (packets)
    int m_qMaxBur;       //!< Maximum observed burstiness (packets)
    MredMode m_mredMode; //!< MRED operating mode

    VirtualQueueParams m_qParam[kMaxPrec]; //!< Per-precedence RED parameters

    uint32_t m_currentPrec; //!< Precedence stashed by EnqueueWithPrec
    bool m_currentEcn;      //!< ECN flag stashed by EnqueueWithPrec
    PktResult m_lastResult; //!< Result of last DoEnqueue (for EnqueueWithPrec)

    Ptr<UniformRandomVariable> m_rng; //!< RNG stream for RED drop decisions
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_RED_SUB_QUEUE_H
