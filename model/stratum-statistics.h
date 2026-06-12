/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * Copyright (C) 2000 Nortel Networks
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsred.{h,cc} statType and dsFD (2001).
 */

#ifndef NS3_STRATUM_STATISTICS_H
#define NS3_STRATUM_STATISTICS_H

#include "stratum-constants.h"

#include "ns3/object.h"

#include <array>
#include <cstdint>
#include <ostream>

namespace ns3::stratum
{

/**
 * @ingroup stratum
 *
 * @brief Per-DSCP packet statistics collector.
 *
 * Replaces the ns-2 `statType` struct and `dsFD` frequency-distribution
 * class from dsred.{h,cc}. Provides per-code-point counters for enqueued,
 * dequeued, RED (early) drops, and tail (late) drops, plus OWD/IPDV
 * accumulators that are fed externally by user Rx callbacks.
 *
 * The PrintStats() method produces output matching the ns-2 format from
 * dsREDQueue::printStats() (dsred.cc line 465).
 *
 */
class Statistics : public Object
{
  public:
    /**
     * @brief Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId();

    /** @brief Default constructor; all counters zero-initialized. */
    Statistics();

    // --- Recording methods ---------------------------------------------------

    /**
     * @brief Record an enqueue event for a given DSCP.
     * @param dscp DSCP code point [0, 63]
     * @param packetSizeBytes size of the packet in bytes
     */
    void RecordEnqueue(uint8_t dscp, uint32_t packetSizeBytes);

    /**
     * @brief Record a dequeue event for a given DSCP.
     * @param dscp DSCP code point [0, 63]
     * @param packetSizeBytes size of the packet in bytes
     */
    void RecordDequeue(uint8_t dscp, uint32_t packetSizeBytes);

    /**
     * @brief Record a RED (early) drop for a given DSCP.
     * @param dscp DSCP code point [0, 63]
     * @param packetSizeBytes size of the packet in bytes
     */
    void RecordRedDrop(uint8_t dscp, uint32_t packetSizeBytes);

    /**
     * @brief Record a tail (late/buffer overflow) drop for a given DSCP.
     * @param dscp DSCP code point [0, 63]
     * @param packetSizeBytes size of the packet in bytes
     */
    void RecordTailDrop(uint8_t dscp, uint32_t packetSizeBytes);

    /**
     * @brief Record a one-way delay sample for a given DSCP.
     *
     * Called externally by user Rx callbacks.
     *
     * @param dscp DSCP code point [0, 63]
     * @param owdSeconds one-way delay in seconds
     */
    void RecordOwd(uint8_t dscp, double owdSeconds);

    /**
     * @brief Record an IP delay variation sample for a given DSCP.
     *
     * Called externally by user Rx callbacks.
     *
     * @param dscp DSCP code point [0, 63]
     * @param ipdvSeconds IP delay variation in seconds
     */
    void RecordIpdv(uint8_t dscp, double ipdvSeconds);

    /**
     * @brief Record a first-transmission byte count for a DSCP.
     *
     * Used to derive thesis-style goodput, which is defined as
     * `goodput(x) = origBytes(x) / (origBytes(x) + retxBytes(x))`.
     * Increment on every enqueued packet *not* carrying TcpRetransmitTag.
     *
     * @param dscp DSCP code point [0, 63]
     * @param bytes packet size on the wire
     */
    void RecordOrigBytes(uint8_t dscp, uint32_t bytes);

    /**
     * @brief Record a retransmission byte count for a DSCP.
     *
     * Increment on every enqueued packet that carries TcpRetransmitTag
     * (added by ns-3 mainline patch 0002 in TcpSocketBase::SendDataPacket).
     *
     * @param dscp DSCP code point [0, 63]
     * @param bytes packet size on the wire
     */
    void RecordRetxBytes(uint8_t dscp, uint32_t bytes);

    // --- Query methods -------------------------------------------------------

    /**
     * @brief Get the total number of packets enqueued for a DSCP.
     * @param dscp DSCP code point [0, 63]
     * @return number of enqueued packets
     */
    uint64_t GetEnqueued(uint8_t dscp) const;

    /**
     * @brief Get the total number of packets dequeued for a DSCP.
     * @param dscp DSCP code point [0, 63]
     * @return number of dequeued packets
     */
    uint64_t GetDequeued(uint8_t dscp) const;

    /**
     * @brief Get the number of RED (early) drops for a DSCP.
     * @param dscp DSCP code point [0, 63]
     * @return number of RED drops
     */
    uint64_t GetRedDrops(uint8_t dscp) const;

    /**
     * @brief Get the number of tail (late) drops for a DSCP.
     * @param dscp DSCP code point [0, 63]
     * @return number of tail drops
     */
    uint64_t GetTailDrops(uint8_t dscp) const;

    /**
     * @brief Get the total number of drops (RED + tail) for a DSCP.
     * @param dscp DSCP code point [0, 63]
     * @return number of total drops
     */
    uint64_t GetTotalDrops(uint8_t dscp) const;

    /**
     * @brief Get the mean one-way delay for a DSCP.
     * @param dscp DSCP code point [0, 63]
     * @return mean OWD in seconds, or 0.0 if no samples recorded
     */
    double GetMeanOwd(uint8_t dscp) const;

    /**
     * @brief Get the mean IP delay variation for a DSCP.
     * @param dscp DSCP code point [0, 63]
     * @return mean IPDV in seconds, or 0.0 if no samples recorded
     */
    double GetMeanIpdv(uint8_t dscp) const;

    /**
     * @brief Get total first-transmission bytes recorded at this DSCP.
     * @param dscp DSCP code point [0, 63]
     * @return total first-transmission bytes recorded at this DSCP
     */
    uint64_t GetOrigBytes(uint8_t dscp) const;

    /**
     * @brief Get total retransmission bytes recorded at this DSCP.
     * @param dscp DSCP code point [0, 63]
     * @return total retransmission bytes recorded at this DSCP
     */
    uint64_t GetRetxBytes(uint8_t dscp) const;

    // --- Output --------------------------------------------------------------

    /**
     * @brief Print per-DSCP statistics in the ns-2 format.
     *
     * Output matches dsREDQueue::printStats() (dsred.cc line 465):
     * @code
     * Packets Statistics
     * =======================================
     * CP TotPkts TxPkts ldrops edrops
     * -- ------- ------ ------ ------
     * 46 100 90.00% 3.00% 7.00%
     * ----------------------------------------
     * All 100 90.00% 3.00% 7.00%
     * @endcode
     *
     * "ldrops" = late drops (tail/buffer overflow),
     * "edrops" = early drops (RED).
     *
     * @param os output stream
     */
    void PrintStats(std::ostream& os) const;

  private:
    /** @brief Per-DSCP counter block. */
    struct PerDscpCounters
    {
        uint64_t enqueued{0};  //!< Total packets arriving (enqueue attempts that succeed + drops)
        uint64_t dequeued{0};  //!< Packets successfully transmitted
        uint64_t redDrops{0};  //!< Early (RED probabilistic) drops
        uint64_t tailDrops{0}; //!< Late (buffer overflow) drops
        double sumOwd{0.0};    //!< Sum of OWD samples (seconds)
        uint64_t owdCount{0};  //!< Number of OWD samples
        double sumIpdv{0.0};   //!< Sum of IPDV samples (seconds)
        uint64_t ipdvCount{0}; //!< Number of IPDV samples
        uint64_t origBytes{0}; //!< First-transmission bytes (for thesis goodput)
        uint64_t retxBytes{0}; //!< Retransmission bytes (for thesis goodput)
    };

    std::array<PerDscpCounters, kMaxCodePoints> m_counters; //!< Per-DSCP counters
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_STATISTICS_H
