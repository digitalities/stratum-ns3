/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Trace-replay application implementation.
 */

#include "stratum-trace-replay-application.h"

#include "ns3/buffer.h"
#include "ns3/ipv4-header.h"
#include "ns3/log.h"
#include "ns3/mac48-address.h"
#include "ns3/packet.h"
#include "ns3/pcap-file.h"
#include "ns3/simulator.h"

#include <algorithm>
#include <vector>

namespace ns3::stratum
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::TraceReplayApplication");
NS_OBJECT_ENSURE_REGISTERED(TraceReplayApplication);

namespace
{

/// Ethernet (DLT_EN10MB) link-layer header size in bytes.
constexpr uint32_t k_ethernetHeaderBytes = 14;

/// Minimum bytes required to carry a fixed-length IPv4 header (no options).
constexpr uint32_t k_minIpv4HeaderBytes = 20;

/// Ethernet type code for IPv4 frames.
constexpr uint16_t k_ethertypeIpv4 = 0x0800;

/// Snapshot length used when reading pcap records (matches default snaplen).
constexpr uint32_t k_maxReadBytes = 65535;

} // unnamed namespace

TypeId
TraceReplayApplication::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::TraceReplayApplication")
                            .SetParent<Application>()
                            .SetGroupName("Stratum")
                            .AddConstructor<TraceReplayApplication>();
    return tid;
}

TraceReplayApplication::TraceReplayApplication()
    : m_qdisc(nullptr),
      m_replayed(0)
{
    NS_LOG_FUNCTION(this);
}

TraceReplayApplication::~TraceReplayApplication()
{
    NS_LOG_FUNCTION(this);
}

void
TraceReplayApplication::DoDispose()
{
    m_qdisc = nullptr;
    m_loaded.clear();
    m_events.clear();
    Application::DoDispose();
}

void
TraceReplayApplication::AddInputPcap(const std::string& path)
{
    m_pcapPaths.push_back(path);
}

void
TraceReplayApplication::SetTargetQdisc(Ptr<QueueDisc> qdisc)
{
    m_qdisc = qdisc;
}

uint64_t
TraceReplayApplication::GetReplayedCount() const
{
    return m_replayed;
}

std::vector<TraceReplayApplication::Record>
TraceReplayApplication::LoadTrace() const
{
    NS_LOG_FUNCTION(this);

    // Two-pass merge: per-pcap absolute timestamps are first collected as
    // (seconds, sub-second nanos, item). After all files are scanned the
    // earliest absolute time becomes the zero of relativeOffset.
    struct AbsRecord
    {
        int64_t absNs;
        Ptr<Ipv4QueueDiscItem> item;
    };

    std::vector<AbsRecord> absoluteRecords;
    bool haveEarliest = false;
    int64_t earliestNs = 0;

    for (const std::string& path : m_pcapPaths)
    {
        PcapFile pcap;
        pcap.Open(path, std::ios::in);
        if (pcap.Fail())
        {
            NS_LOG_WARN("TraceReplayApplication: cannot open pcap " << path);
            continue;
        }

        const bool nanosec = pcap.IsNanoSecMode();

        std::vector<uint8_t> buffer(k_maxReadBytes);

        while (true)
        {
            uint32_t tsSec = 0;
            uint32_t tsSubSec = 0;
            uint32_t inclLen = 0;
            uint32_t origLen = 0;
            uint32_t readLen = 0;
            pcap.Read(buffer.data(),
                      static_cast<uint32_t>(buffer.size()),
                      tsSec,
                      tsSubSec,
                      inclLen,
                      origLen,
                      readLen);
            if (readLen == 0 || pcap.Eof())
            {
                break;
            }

            // Skip the Ethernet header and locate the IP version nibble.
            if (readLen < k_ethernetHeaderBytes + k_minIpv4HeaderBytes)
            {
                NS_LOG_WARN("TraceReplayApplication: short frame in " << path << " (skipping)");
                continue;
            }

            // Verify the EtherType is IPv4 (offset 12-13 in the Ethernet header).
            const uint16_t etherType = static_cast<uint16_t>((buffer[12] << 8) | buffer[13]);
            if (etherType != k_ethertypeIpv4)
            {
                NS_LOG_WARN("TraceReplayApplication: non-IPv4 EtherType 0x"
                            << std::hex << etherType << std::dec << " in " << path
                            << " (skipping)");
                continue;
            }

            const uint8_t* ipStart = buffer.data() + k_ethernetHeaderBytes;
            const uint8_t ihl = ipStart[0] & 0x0f;
            const uint32_t ipHeaderBytes = static_cast<uint32_t>(ihl) * 4;
            if (ipHeaderBytes < k_minIpv4HeaderBytes ||
                k_ethernetHeaderBytes + ipHeaderBytes > readLen)
            {
                NS_LOG_WARN("TraceReplayApplication: malformed IPv4 header in " << path
                                                                                << " (skipping)");
                continue;
            }

            // Deserialize the IPv4 header from the raw bytes.
            Buffer tmp;
            tmp.AddAtStart(ipHeaderBytes);
            Buffer::Iterator it = tmp.Begin();
            it.Write(ipStart, ipHeaderBytes);
            Ipv4Header hdr;
            it = tmp.Begin();
            hdr.Deserialize(it);

            // Synthesize the payload sized to the original on-wire frame
            // minus the L2 and L3 header bytes.
            uint32_t payloadSize = 0;
            if (origLen > k_ethernetHeaderBytes + ipHeaderBytes)
            {
                payloadSize = origLen - k_ethernetHeaderBytes - ipHeaderBytes;
            }
            Ptr<Packet> packet = Create<Packet>(payloadSize);

            Ptr<Ipv4QueueDiscItem> item =
                Create<Ipv4QueueDiscItem>(packet, Mac48Address(), k_ethertypeIpv4, hdr);

            const int64_t subNs =
                nanosec ? static_cast<int64_t>(tsSubSec) : static_cast<int64_t>(tsSubSec) * 1000;
            const int64_t absNs = static_cast<int64_t>(tsSec) * 1000000000LL + subNs;

            if (!haveEarliest || absNs < earliestNs)
            {
                earliestNs = absNs;
                haveEarliest = true;
            }
            absoluteRecords.push_back({absNs, item});
        }

        pcap.Close();
    }

    std::vector<Record> records;
    records.reserve(absoluteRecords.size());
    for (const AbsRecord& a : absoluteRecords)
    {
        Record r;
        r.relativeOffset = NanoSeconds(a.absNs - earliestNs);
        r.item = a.item;
        records.push_back(r);
    }

    std::sort(records.begin(), records.end(), [](const Record& a, const Record& b) {
        return a.relativeOffset < b.relativeOffset;
    });

    return records;
}

void
TraceReplayApplication::StartApplication()
{
    NS_LOG_FUNCTION(this);
    m_replayed = 0;
    m_loaded = LoadTrace();
    m_events.clear();
    m_events.reserve(m_loaded.size());

    for (uint32_t idx = 0; idx < m_loaded.size(); ++idx)
    {
        EventId e = Simulator::Schedule(m_loaded[idx].relativeOffset,
                                        &TraceReplayApplication::EnqueueOne,
                                        this,
                                        idx);
        m_events.push_back(e);
    }
}

void
TraceReplayApplication::StopApplication()
{
    NS_LOG_FUNCTION(this);
    for (EventId& e : m_events)
    {
        if (!e.IsExpired())
        {
            Simulator::Cancel(e);
        }
    }
    m_events.clear();
}

void
TraceReplayApplication::EnqueueOne(uint32_t index)
{
    if (!m_qdisc || index >= m_loaded.size())
    {
        return;
    }
    m_qdisc->Enqueue(m_loaded[index].item);
    ++m_replayed;
}

} // namespace ns3::stratum
