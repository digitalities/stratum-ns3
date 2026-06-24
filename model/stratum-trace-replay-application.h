/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Trace-replay application: feeds a QueueDisc with synthesized
 * QueueDiscItem instances (IPv4 or IPv6) reconstructed from packet headers
 * recorded in one or more pcap files.
 */

#ifndef NS3_STRATUM_TRACE_REPLAY_APPLICATION_H
#define NS3_STRATUM_TRACE_REPLAY_APPLICATION_H

#include "ns3/application.h"
#include "ns3/event-id.h"
#include "ns3/ipv4-queue-disc-item.h"
#include "ns3/ipv6-queue-disc-item.h"
#include "ns3/nstime.h"
#include "ns3/ptr.h"
#include "ns3/queue-disc.h"
#include "ns3/queue-item.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ns3::stratum
{

/**
 * @ingroup stratum
 *
 * @brief Application that replays a captured packet stream into a QueueDisc.
 *
 * Reads one or more pcap files, sorts records by capture timestamp, and
 * synthesizes QueueDiscItem instances that are enqueued at their original
 * relative inter-arrival timing. The application bypasses the TCP/UDP/IP
 * socket layers and holds a Ptr<QueueDisc> directly, invoking Enqueue()
 * via scheduled events.
 *
 * Pcap link-layer type DLT_EN10MB (Ethernet, value 1) is supported; the
 * 14-byte Ethernet header is skipped. IPv4 (`0x0800`) frames are parsed
 * as Ipv4Header; IPv6 (`0x86DD`) frames are parsed as Ipv6Header (fixed
 * 40-byte header, no extension-header walk). Other EtherTypes (e.g. ARP)
 * are silently dropped with a warning log entry. Both microsecond-precision
 * and nanosecond-precision pcap magic numbers are accepted; the file's
 * internal nanosecond flag is consulted via PcapFile::IsNanoSecMode().
 *
 * Synthesized items reproduce the layer-3 header (addresses, DS field, and
 * length) and the original inter-arrival timing, but carry a zero-filled
 * payload with no transport header. Replayed flows therefore preserve
 * address- and DSCP-level distinctness (host isolation, tin/PHB selection)
 * but not transport ports: any consumer that hashes on the full 5-tuple
 * (per-flow fairness, bulk-flow counting) reads a zero source/destination
 * port for every replayed packet, so flows that differ only in port collapse
 * to one hash bucket. Use captured traffic to drive aggregate DS-field
 * behaviour, not per-flow port distinctness.
 */
class TraceReplayApplication : public Application
{
  public:
    /**
     * @brief Get the TypeId for this class.
     * @return the TypeId.
     */
    static TypeId GetTypeId();

    /** @brief Construct a TraceReplayApplication with default attributes. */
    TraceReplayApplication();

    ~TraceReplayApplication() override;

    /**
     * @brief Append a pcap file to read at startup.
     * @param path filesystem path to a pcap file with DLT_EN10MB link layer
     */
    void AddInputPcap(const std::string& path);

    /**
     * @brief Set the target queue disc that receives the replayed enqueues.
     * @param qdisc target queue disc (must already be Initialize()d)
     */
    void SetTargetQdisc(Ptr<QueueDisc> qdisc);

    /// One replay record materialized from the pcap stream.
    struct Record
    {
        Time relativeOffset;     //!< offset from the earliest packet across all pcaps
        Ptr<QueueDiscItem> item; //!< synthesized item (IPv4 or IPv6) ready to Enqueue
    };

    /**
     * @brief Parse all configured pcaps and return the merged record vector.
     *
     * The returned vector is sorted by relative offset. This method does
     * not require an active Simulator and is exposed for unit-testing the
     * pcap-to-item synthesis independently of scheduling.
     *
     * @return merged, time-sorted record vector
     */
    std::vector<Record> LoadTrace() const;

    /**
     * @brief Record count successfully enqueued by the most recent run.
     * @return enqueued packet count since the last StartApplication
     */
    uint64_t GetReplayedCount() const;

  protected:
    void DoDispose() override;

  private:
    void StartApplication() override;
    void StopApplication() override;

    /**
     * @brief Enqueue the loaded record at the given index into the target qdisc.
     * @param index position in m_loaded
     */
    void EnqueueOne(uint32_t index);

    std::vector<std::string> m_pcapPaths; //!< pcap files to merge-replay
    Ptr<QueueDisc> m_qdisc;               //!< target queue disc
    std::vector<Record> m_loaded;         //!< sorted records from the most recent LoadTrace
    std::vector<EventId> m_events;        //!< pending scheduled enqueues
    uint64_t m_replayed;                  //!< enqueues completed since last start
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_TRACE_REPLAY_APPLICATION_H
