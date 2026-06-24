/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Unit tests for TraceReplayApplication: pcap parsing, time-aligned
 * scheduling, multi-pcap merge, and Ipv4QueueDiscItem 5-tuple synthesis.
 */

#include "ns3/enum.h"
#include "ns3/fq-cobalt-queue-disc.h"
#include "ns3/ipv4-header.h"
#include "ns3/ipv4-queue-disc-item.h"
#include "ns3/ipv6-address.h"
#include "ns3/ipv6-header.h"
#include "ns3/ipv6-queue-disc-item.h"
#include "ns3/log.h"
#include "ns3/node.h"
#include "ns3/nstime.h"
#include "ns3/pcap-file.h"
#include "ns3/ptr.h"
#include "ns3/simulator.h"
#include "ns3/stratum-trace-replay-application.h"
#include "ns3/test.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ns3::stratum
{

namespace
{

constexpr uint32_t k_dltEn10mb = 1;
constexpr uint16_t k_ethertypeIpv4 = 0x0800;
constexpr uint16_t k_ethertypeIpv6 = 0x86DD;

/// Builder for one pcap record used by the tests.
struct RecordSpec
{
    uint32_t tsSec;         //!< capture timestamp seconds
    uint32_t tsSubSec;      //!< sub-second component (usec by default)
    std::string srcIp;      //!< IPv4 source address (dotted-quad)
    std::string dstIp;      //!< IPv4 destination address (dotted-quad)
    uint8_t proto;          //!< IP protocol number (e.g. 6=TCP, 17=UDP)
    uint32_t totalFrameLen; //!< full on-wire frame length (L2+L3+payload)
};

/**
 * @brief Write one pcap file with the given record specs.
 *
 * Each record is encoded as a 14-byte Ethernet header (zero MAC addresses,
 * IPv4 EtherType) followed by a fixed-length IPv4 header (20 bytes, no
 * options) and zero-filled payload. Total written length matches the
 * record's totalFrameLen.
 */
void
WritePcap(const std::string& path, const std::vector<RecordSpec>& records, uint32_t snaplen = 2000)
{
    PcapFile out;
    out.Open(path, std::ios::out);
    out.Init(k_dltEn10mb, snaplen);

    for (const RecordSpec& r : records)
    {
        std::vector<uint8_t> frame(r.totalFrameLen, 0);

        // Ethernet header: dst MAC (0..5) + src MAC (6..11) + EtherType (12..13).
        frame[12] = static_cast<uint8_t>(k_ethertypeIpv4 >> 8);
        frame[13] = static_cast<uint8_t>(k_ethertypeIpv4 & 0xff);

        // IPv4 header at offset 14.
        Ipv4Header hdr;
        hdr.SetSource(Ipv4Address(r.srcIp.c_str()));
        hdr.SetDestination(Ipv4Address(r.dstIp.c_str()));
        hdr.SetProtocol(r.proto);
        const uint32_t payloadSize = r.totalFrameLen - 14 - hdr.GetSerializedSize();
        hdr.SetPayloadSize(payloadSize);

        Buffer buf;
        buf.AddAtStart(hdr.GetSerializedSize());
        hdr.Serialize(buf.Begin());
        Buffer::Iterator it = buf.Begin();
        std::vector<uint8_t> ipBytes(hdr.GetSerializedSize());
        it.Read(ipBytes.data(), static_cast<uint32_t>(ipBytes.size()));
        std::memcpy(frame.data() + 14, ipBytes.data(), ipBytes.size());

        out.Write(r.tsSec, r.tsSubSec, frame.data(), r.totalFrameLen);
    }

    out.Close();
}

/// Write one pcap file of IPv6 frames: 14-byte Ethernet (EtherType 0x86DD)
/// + fixed 40-byte Ipv6Header + zero-filled payload. RecordSpec.srcIp/dstIp
/// carry IPv6 literals; RecordSpec.proto becomes the Next Header byte.
void
WritePcapV6(const std::string& path,
            const std::vector<RecordSpec>& records,
            uint32_t snaplen = 2000)
{
    PcapFile out;
    out.Open(path, std::ios::out);
    out.Init(k_dltEn10mb, snaplen);

    for (const RecordSpec& r : records)
    {
        std::vector<uint8_t> frame(r.totalFrameLen, 0);
        frame[12] = static_cast<uint8_t>(k_ethertypeIpv6 >> 8);
        frame[13] = static_cast<uint8_t>(k_ethertypeIpv6 & 0xff);

        Ipv6Header hdr;
        hdr.SetSource(Ipv6Address(r.srcIp.c_str()));
        hdr.SetDestination(Ipv6Address(r.dstIp.c_str()));
        hdr.SetNextHeader(r.proto);
        const uint32_t payloadSize = r.totalFrameLen - 14 - hdr.GetSerializedSize();
        hdr.SetPayloadLength(static_cast<uint16_t>(payloadSize));

        Buffer buf;
        buf.AddAtStart(hdr.GetSerializedSize());
        hdr.Serialize(buf.Begin());
        Buffer::Iterator it = buf.Begin();
        std::vector<uint8_t> ipBytes(hdr.GetSerializedSize());
        it.Read(ipBytes.data(), static_cast<uint32_t>(ipBytes.size()));
        std::memcpy(frame.data() + 14, ipBytes.data(), ipBytes.size());

        out.Write(r.tsSec, r.tsSubSec, frame.data(), r.totalFrameLen);
    }
    out.Close();
}

/// Construct an initialized FqCobaltQueueDisc target with host-isolation enabled.
Ptr<FqCobaltQueueDisc>
MakeTargetQdisc()
{
    Ptr<FqCobaltQueueDisc> q = CreateObject<FqCobaltQueueDisc>();
    q->SetQuantum(1500); // MTU-equivalent; required when no NetDevice is attached
    q->SetEnableHostIsolation(true);
    q->SetHostIsolationMode(FqCobaltQueueDisc::HostIsolationMode::Triple);
    q->Initialize();
    return q;
}

/// Remove a test pcap file from /tmp. Errors are ignored.
void
RemoveFile(const std::string& path)
{
    std::remove(path.c_str());
}

/// The IPv4 header of a replayed record. All pre-IPv6 fixtures are IPv4, so the
/// item is always an Ipv4QueueDiscItem here; downcast to read its header.
const Ipv4Header&
V4Header(const TraceReplayApplication::Record& r)
{
    return DynamicCast<Ipv4QueueDiscItem>(r.item)->GetHeader();
}

} // unnamed namespace

/**
 * @ingroup stratum-tests
 * @brief Configuring no pcap files yields an empty trace and a no-op replay.
 */
class TestTraceReplay_EmptyTraceNoOp : public TestCase
{
  public:
    TestTraceReplay_EmptyTraceNoOp()
        : TestCase("TraceReplay_EmptyTraceNoOp")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<Node> node = CreateObject<Node>();
        Ptr<FqCobaltQueueDisc> q = MakeTargetQdisc();
        Ptr<TraceReplayApplication> app = CreateObject<TraceReplayApplication>();
        app->SetTargetQdisc(q);
        node->AddApplication(app);

        NS_TEST_ASSERT_MSG_EQ(app->LoadTrace().size(),
                              0u,
                              "LoadTrace with no pcaps must return empty");

        app->SetStartTime(Seconds(0));
        Simulator::Stop(Seconds(0.1));
        Simulator::Run();
        Simulator::Destroy();

        NS_TEST_ASSERT_MSG_EQ(app->GetReplayedCount(),
                              0u,
                              "no pcaps configured — replayed count must stay 0");
    }
};

/**
 * @ingroup stratum-tests
 * @brief Single-packet pcap drives one Enqueue with correct 5-tuple.
 */
class TestTraceReplay_SinglePacket : public TestCase
{
  public:
    TestTraceReplay_SinglePacket()
        : TestCase("TraceReplay_SinglePacket")
    {
    }

  private:
    void DoRun() override
    {
        const std::string path = "/tmp/ds-trace-replay-test-single.pcap";
        const uint32_t frameLen = 1500;
        WritePcap(path, {{/*sec*/ 0, /*usec*/ 0, "10.1.1.1", "10.3.1.1", /*proto*/ 17, frameLen}});

        Ptr<Node> node = CreateObject<Node>();
        Ptr<FqCobaltQueueDisc> q = MakeTargetQdisc();
        Ptr<TraceReplayApplication> app = CreateObject<TraceReplayApplication>();
        app->AddInputPcap(path);
        app->SetTargetQdisc(q);
        node->AddApplication(app);
        app->SetStartTime(Seconds(0));

        Simulator::Stop(Seconds(1));
        Simulator::Run();

        NS_TEST_ASSERT_MSG_EQ(app->GetReplayedCount(), 1u, "one record must replay");

        Ptr<QueueDiscItem> head = q->Dequeue();
        NS_TEST_ASSERT_MSG_NE(head, nullptr, "queue must hold the replayed packet");
        Ptr<Ipv4QueueDiscItem> ipItem = DynamicCast<Ipv4QueueDiscItem>(head);
        NS_TEST_ASSERT_MSG_NE(ipItem, nullptr, "replayed item must be Ipv4QueueDiscItem");
        NS_TEST_ASSERT_MSG_EQ(ipItem->GetHeader().GetSource(),
                              Ipv4Address("10.1.1.1"),
                              "source IP must match pcap record");
        NS_TEST_ASSERT_MSG_EQ(ipItem->GetHeader().GetDestination(),
                              Ipv4Address("10.3.1.1"),
                              "destination IP must match pcap record");

        Simulator::Destroy();
        RemoveFile(path);
    }
};

/**
 * @ingroup stratum-tests
 * @brief Records out-of-order in the file emerge sorted by timestamp.
 */
class TestTraceReplay_OrderingPreserved : public TestCase
{
  public:
    TestTraceReplay_OrderingPreserved()
        : TestCase("TraceReplay_OrderingPreserved")
    {
    }

  private:
    void DoRun() override
    {
        const std::string path = "/tmp/ds-trace-replay-test-ordering.pcap";
        // Wire order is t=0, t=10ms, t=5ms — middle and last are
        // intentionally swapped so sort-by-offset is observable.
        WritePcap(path,
                  {{0, 0, "10.0.0.1", "10.0.1.1", 17, 1200},
                   {0, 10000, "10.0.0.2", "10.0.1.2", 17, 1200},
                   {0, 5000, "10.0.0.3", "10.0.1.3", 17, 1200}});

        Ptr<Node> node = CreateObject<Node>();
        Ptr<FqCobaltQueueDisc> q = MakeTargetQdisc();
        Ptr<TraceReplayApplication> app = CreateObject<TraceReplayApplication>();
        app->AddInputPcap(path);
        app->SetTargetQdisc(q);

        std::vector<TraceReplayApplication::Record> rs = app->LoadTrace();
        NS_TEST_ASSERT_MSG_EQ(rs.size(), 3u, "three records expected");
        NS_TEST_ASSERT_MSG_LT(rs[0].relativeOffset, rs[1].relativeOffset, "ordered ascending");
        NS_TEST_ASSERT_MSG_LT(rs[1].relativeOffset, rs[2].relativeOffset, "ordered ascending");
        NS_TEST_ASSERT_MSG_EQ(rs[0].relativeOffset, MicroSeconds(0), "first at offset 0");
        NS_TEST_ASSERT_MSG_EQ(rs[1].relativeOffset, MicroSeconds(5000), "second at +5 ms");
        NS_TEST_ASSERT_MSG_EQ(rs[2].relativeOffset, MicroSeconds(10000), "third at +10 ms");

        // Source IPs at the head of each record reflect timestamp order:
        // 10.0.0.1 (t=0), 10.0.0.3 (t=5ms), 10.0.0.2 (t=10ms).
        NS_TEST_ASSERT_MSG_EQ(V4Header(rs[0]).GetSource(),
                              Ipv4Address("10.0.0.1"),
                              "earliest record by timestamp");
        NS_TEST_ASSERT_MSG_EQ(V4Header(rs[1]).GetSource(),
                              Ipv4Address("10.0.0.3"),
                              "5ms record sorts to middle");
        NS_TEST_ASSERT_MSG_EQ(V4Header(rs[2]).GetSource(),
                              Ipv4Address("10.0.0.2"),
                              "10ms record sorts to tail");

        node->AddApplication(app);
        app->SetStartTime(Seconds(0));
        Simulator::Stop(Seconds(1));
        Simulator::Run();

        NS_TEST_ASSERT_MSG_EQ(app->GetReplayedCount(), 3u, "all three records replay");

        Ptr<QueueDiscItem> a = q->Dequeue();
        Ptr<QueueDiscItem> b = q->Dequeue();
        Ptr<QueueDiscItem> c = q->Dequeue();
        NS_TEST_ASSERT_MSG_NE(a, nullptr, "first dequeue not null");
        NS_TEST_ASSERT_MSG_NE(b, nullptr, "second dequeue not null");
        NS_TEST_ASSERT_MSG_NE(c, nullptr, "third dequeue not null");
        Ptr<Ipv4QueueDiscItem> ipA = DynamicCast<Ipv4QueueDiscItem>(a);
        Ptr<Ipv4QueueDiscItem> ipB = DynamicCast<Ipv4QueueDiscItem>(b);
        Ptr<Ipv4QueueDiscItem> ipC = DynamicCast<Ipv4QueueDiscItem>(c);
        NS_TEST_ASSERT_MSG_EQ(ipA->GetHeader().GetSource(),
                              Ipv4Address("10.0.0.1"),
                              "first dequeue is earliest record");
        NS_TEST_ASSERT_MSG_EQ(ipB->GetHeader().GetSource(),
                              Ipv4Address("10.0.0.3"),
                              "second dequeue is 5ms record");
        NS_TEST_ASSERT_MSG_EQ(ipC->GetHeader().GetSource(),
                              Ipv4Address("10.0.0.2"),
                              "third dequeue is 10ms record");

        Simulator::Destroy();
        RemoveFile(path);
    }
};

/**
 * @ingroup stratum-tests
 * @brief Absolute pcap timestamps are converted to offsets relative to the earliest.
 */
class TestTraceReplay_TimeAlignmentRelative : public TestCase
{
  public:
    TestTraceReplay_TimeAlignmentRelative()
        : TestCase("TraceReplay_TimeAlignmentRelative")
    {
    }

  private:
    void DoRun() override
    {
        const std::string path = "/tmp/ds-trace-replay-test-time.pcap";
        WritePcap(path,
                  {{12345, 0, "10.0.0.1", "10.0.1.1", 17, 1000},
                   {12345, 10000, "10.0.0.2", "10.0.1.2", 17, 1000}});

        Ptr<Node> node = CreateObject<Node>();
        Ptr<FqCobaltQueueDisc> q = MakeTargetQdisc();
        Ptr<TraceReplayApplication> app = CreateObject<TraceReplayApplication>();
        app->AddInputPcap(path);
        app->SetTargetQdisc(q);

        std::vector<TraceReplayApplication::Record> rs = app->LoadTrace();
        NS_TEST_ASSERT_MSG_EQ(rs.size(), 2u, "two records");
        NS_TEST_ASSERT_MSG_EQ(rs[0].relativeOffset, MicroSeconds(0), "first record rebased to t=0");
        NS_TEST_ASSERT_MSG_EQ(rs[1].relativeOffset, MicroSeconds(10000), "second record at +10 ms");

        Simulator::Destroy();
        RemoveFile(path);
    }
};

/**
 * @ingroup stratum-tests
 * @brief Five-tuple plus payload size of the synthesized item matches the pcap record.
 */
class TestTraceReplay_FiveTupleSynthesis : public TestCase
{
  public:
    TestTraceReplay_FiveTupleSynthesis()
        : TestCase("TraceReplay_FiveTupleSynthesis")
    {
    }

  private:
    void DoRun() override
    {
        const std::string path = "/tmp/ds-trace-replay-test-5tuple.pcap";
        const uint32_t frameLen = 1400;
        WritePcap(path, {{0, 0, "192.168.1.5", "10.0.0.42", /*proto TCP*/ 6, frameLen}});

        Ptr<Node> node = CreateObject<Node>();
        Ptr<FqCobaltQueueDisc> q = MakeTargetQdisc();
        Ptr<TraceReplayApplication> app = CreateObject<TraceReplayApplication>();
        app->AddInputPcap(path);
        app->SetTargetQdisc(q);

        std::vector<TraceReplayApplication::Record> rs = app->LoadTrace();
        NS_TEST_ASSERT_MSG_EQ(rs.size(), 1u, "one record");
        const Ipv4Header& hdr = V4Header(rs[0]);
        NS_TEST_ASSERT_MSG_EQ(hdr.GetSource(), Ipv4Address("192.168.1.5"), "source IP preserved");
        NS_TEST_ASSERT_MSG_EQ(hdr.GetDestination(),
                              Ipv4Address("10.0.0.42"),
                              "destination IP preserved");
        NS_TEST_ASSERT_MSG_EQ(hdr.GetProtocol(), 6u, "protocol preserved (TCP)");

        const uint32_t expectedPayload = frameLen - 14u - hdr.GetSerializedSize();
        NS_TEST_ASSERT_MSG_EQ(rs[0].item->GetPacket()->GetSize(),
                              expectedPayload,
                              "payload size = frame - L2 - L3");

        Simulator::Destroy();
        RemoveFile(path);
    }
};

/**
 * @ingroup stratum-tests
 * @brief A single IPv6 frame is replayed as one Ipv6QueueDiscItem whose header
 *        preserves the captured source and destination addresses.
 */
class TestTraceReplay_Ipv6SinglePacket : public TestCase
{
  public:
    TestTraceReplay_Ipv6SinglePacket()
        : TestCase("TraceReplay_Ipv6SinglePacket")
    {
    }

  private:
    void DoRun() override
    {
        const std::string path = "/tmp/stratum-trace-replay-v6-single.pcap";
        const uint32_t frameLen = 14 + 40 + 100; // L2 + fixed IPv6 header + payload
        WritePcapV6(path, {{0, 0, "2001:db8::1", "2001:db8::2", /*Next Header UDP*/ 17, frameLen}});

        Ptr<TraceReplayApplication> app = CreateObject<TraceReplayApplication>();
        app->AddInputPcap(path);
        std::vector<TraceReplayApplication::Record> rs = app->LoadTrace();

        NS_TEST_ASSERT_MSG_EQ(rs.size(), 1, "exactly one IPv6 record must be replayed");
        Ptr<Ipv6QueueDiscItem> v6 = DynamicCast<Ipv6QueueDiscItem>(rs[0].item);
        NS_TEST_ASSERT_MSG_NE(v6, nullptr, "replayed item must be an Ipv6QueueDiscItem");
        NS_TEST_ASSERT_MSG_EQ(v6->GetHeader().GetSource(),
                              Ipv6Address("2001:db8::1"),
                              "IPv6 source address preserved");
        NS_TEST_ASSERT_MSG_EQ(v6->GetHeader().GetDestination(),
                              Ipv6Address("2001:db8::2"),
                              "IPv6 destination address preserved");
        RemoveFile(path);
    }
};

/**
 * @ingroup stratum-tests
 * @brief Two pcap inputs are merged and sorted by global offset.
 */
class TestTraceReplay_TwoPcapsMerged : public TestCase
{
  public:
    TestTraceReplay_TwoPcapsMerged()
        : TestCase("TraceReplay_TwoPcapsMerged")
    {
    }

  private:
    void DoRun() override
    {
        const std::string pathA = "/tmp/ds-trace-replay-test-merge-a.pcap";
        const std::string pathB = "/tmp/ds-trace-replay-test-merge-b.pcap";

        // A holds packets at absolute t=100s+0us and 100s+15ms.
        WritePcap(pathA,
                  {{100, 0, "10.0.0.1", "10.0.1.1", 17, 1000},
                   {100, 15000, "10.0.0.2", "10.0.1.2", 17, 1000}});
        // B holds packets at absolute t=100s+5ms and 100s+20ms.
        WritePcap(pathB,
                  {{100, 5000, "10.0.0.3", "10.0.1.3", 17, 1000},
                   {100, 20000, "10.0.0.4", "10.0.1.4", 17, 1000}});

        Ptr<TraceReplayApplication> app = CreateObject<TraceReplayApplication>();
        app->AddInputPcap(pathA);
        app->AddInputPcap(pathB);

        std::vector<TraceReplayApplication::Record> rs = app->LoadTrace();
        NS_TEST_ASSERT_MSG_EQ(rs.size(), 4u, "four merged records");
        NS_TEST_ASSERT_MSG_EQ(rs[0].relativeOffset, MicroSeconds(0), "earliest from A");
        NS_TEST_ASSERT_MSG_EQ(rs[1].relativeOffset, MicroSeconds(5000), "next from B");
        NS_TEST_ASSERT_MSG_EQ(rs[2].relativeOffset, MicroSeconds(15000), "third from A");
        NS_TEST_ASSERT_MSG_EQ(rs[3].relativeOffset, MicroSeconds(20000), "last from B");

        NS_TEST_ASSERT_MSG_EQ(V4Header(rs[0]).GetSource(),
                              Ipv4Address("10.0.0.1"),
                              "merge order place 0");
        NS_TEST_ASSERT_MSG_EQ(V4Header(rs[1]).GetSource(),
                              Ipv4Address("10.0.0.3"),
                              "merge order place 1");
        NS_TEST_ASSERT_MSG_EQ(V4Header(rs[2]).GetSource(),
                              Ipv4Address("10.0.0.2"),
                              "merge order place 2");
        NS_TEST_ASSERT_MSG_EQ(V4Header(rs[3]).GetSource(),
                              Ipv4Address("10.0.0.4"),
                              "merge order place 3");

        RemoveFile(pathA);
        RemoveFile(pathB);
    }
};

class DsTraceReplayApplicationTestSuite : public TestSuite
{
  public:
    DsTraceReplayApplicationTestSuite()
        : TestSuite("stratum-trace-replay-application", Type::UNIT)
    {
        AddTestCase(new TestTraceReplay_EmptyTraceNoOp, Duration::QUICK);
        AddTestCase(new TestTraceReplay_SinglePacket, Duration::QUICK);
        AddTestCase(new TestTraceReplay_OrderingPreserved, Duration::QUICK);
        AddTestCase(new TestTraceReplay_TimeAlignmentRelative, Duration::QUICK);
        AddTestCase(new TestTraceReplay_FiveTupleSynthesis, Duration::QUICK);
        AddTestCase(new TestTraceReplay_Ipv6SinglePacket, Duration::QUICK);
        AddTestCase(new TestTraceReplay_TwoPcapsMerged, Duration::QUICK);
    }
};

static DsTraceReplayApplicationTestSuite g_dsTraceReplayApplicationTestSuite;

} // namespace ns3::stratum
