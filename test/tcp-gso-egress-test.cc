/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/test.h"
#include "ns3/traffic-control-module.h"

#include <algorithm>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("TcpGsoEgressTest");

namespace
{

void
CaptureIpSize(std::vector<uint32_t>* out,
              Ptr<const Packet> p,
              Ptr<Ipv4> /*ipv4*/,
              uint32_t /*iface*/)
{
    out->push_back(p->GetSize());
}

void
CaptureMacTx(std::vector<uint32_t>* out, Ptr<const Packet> p)
{
    out->push_back(p->GetSize());
}

void
BuildP2pPair(NodeContainer& nodes, NetDeviceContainer& devs, Ipv4InterfaceContainer& iface)
{
    nodes.Create(2);
    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("1ms"));
    p2p.SetDeviceAttribute("Mtu", UintegerValue(1500));
    devs = p2p.Install(nodes);

    InternetStackHelper stack;
    stack.Install(nodes);

    Ipv4AddressHelper addr;
    addr.SetBase("10.0.0.0", "255.255.255.0");
    iface = addr.Assign(devs);
}

} // namespace

/**
 * @ingroup stratum-tests
 *
 * @brief Verify GSO disabled (default): every IP-layer emission is at or
 * below MTU, every device-layer transmission is at or below MTU+overhead,
 * and the receiver delivers all bytes. Regression discipline: this case
 * exercises only the EnableGso=false code path, which is byte-identical
 * to the pre-patch implementation.
 */
class TcpGsoEgressBaselineTestCase : public TestCase
{
  public:
    TcpGsoEgressBaselineTestCase()
        : TestCase("GSO disabled (default): all emissions <= MTU; full byte delivery")
    {
    }

  private:
    void DoRun() override
    {
        NodeContainer nodes;
        NetDeviceContainer devs;
        Ipv4InterfaceContainer iface;
        BuildP2pPair(nodes, devs, iface);

        const uint32_t bytesToSend = 100 * 1024;
        BulkSendHelper src("ns3::TcpSocketFactory", InetSocketAddress(iface.GetAddress(1), 9));
        src.SetAttribute("MaxBytes", UintegerValue(bytesToSend));
        ApplicationContainer srcApp = src.Install(nodes.Get(0));
        srcApp.Start(Seconds(0.1));
        srcApp.Stop(Seconds(10.0));

        PacketSinkHelper sink("ns3::TcpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), 9));
        ApplicationContainer sinkApp = sink.Install(nodes.Get(1));
        sinkApp.Start(Seconds(0.0));
        sinkApp.Stop(Seconds(10.0));

        std::vector<uint32_t> ipSizes;
        std::vector<uint32_t> macSizes;
        Config::ConnectWithoutContext("/NodeList/0/$ns3::Ipv4L3Protocol/Tx",
                                      MakeBoundCallback(&CaptureIpSize, &ipSizes));
        Config::ConnectWithoutContext("/NodeList/0/DeviceList/0/$ns3::PointToPointNetDevice/MacTx",
                                      MakeBoundCallback(&CaptureMacTx, &macSizes));

        Simulator::Stop(Seconds(11.0));
        Simulator::Run();

        for (uint32_t sz : ipSizes)
        {
            NS_TEST_ASSERT_MSG_LT_OR_EQ(sz,
                                        1500u,
                                        "GSO=off: IP-layer emission " + std::to_string(sz) +
                                            " > MTU");
        }
        for (uint32_t sz : macSizes)
        {
            NS_TEST_ASSERT_MSG_LT_OR_EQ(sz,
                                        1600u,
                                        "GSO=off: MAC-layer transmission " + std::to_string(sz) +
                                            " > MTU+overhead");
        }

        Ptr<PacketSink> sinkPtr = DynamicCast<PacketSink>(sinkApp.Get(0));
        NS_TEST_ASSERT_MSG_EQ(sinkPtr->GetTotalRx(),
                              bytesToSend,
                              "Receiver should get all 100 KB at GSO=off");

        Simulator::Destroy();
    }
};

/**
 * @ingroup stratum-tests
 *
 * @brief Verify GSO enabled: super-segments are observable at the IP layer
 * (size > MTU), every device-layer transmission is at or below MTU+overhead
 * (TrafficControlLayer re-segments), and the receiver delivers all bytes
 * via valid per-fragment TCP headers.
 *
 * Until TrafficControlLayer GSO-aware send wrapper is wired, this test
 * fails on the MAC-layer assertion (super-segments reach the device
 * un-fragmented). It passes once the wrapper segments per-fragment TCP
 * headers.
 */
class TcpGsoEgressEnabledTestCase : public TestCase
{
  public:
    TcpGsoEgressEnabledTestCase()
        : TestCase("GSO enabled: super-segments at IP; MTU-sized at MAC; full byte delivery")
    {
    }

  private:
    void DoRun() override
    {
        Config::SetDefault("ns3::TcpSocketBase::EnableGso", BooleanValue(true));
        Config::SetDefault("ns3::TcpSocketBase::MaxGsoSize", UintegerValue(8000));

        NodeContainer nodes;
        NetDeviceContainer devs;
        Ipv4InterfaceContainer iface;
        BuildP2pPair(nodes, devs, iface);

        const uint32_t bytesToSend = 100 * 1024;
        BulkSendHelper src("ns3::TcpSocketFactory", InetSocketAddress(iface.GetAddress(1), 9));
        src.SetAttribute("MaxBytes", UintegerValue(bytesToSend));
        ApplicationContainer srcApp = src.Install(nodes.Get(0));
        srcApp.Start(Seconds(0.1));
        srcApp.Stop(Seconds(10.0));

        PacketSinkHelper sink("ns3::TcpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), 9));
        ApplicationContainer sinkApp = sink.Install(nodes.Get(1));
        sinkApp.Start(Seconds(0.0));
        sinkApp.Stop(Seconds(10.0));

        std::vector<uint32_t> ipSizes;
        std::vector<uint32_t> macSizes;
        Config::ConnectWithoutContext("/NodeList/0/$ns3::Ipv4L3Protocol/Tx",
                                      MakeBoundCallback(&CaptureIpSize, &ipSizes));
        Config::ConnectWithoutContext("/NodeList/0/DeviceList/0/$ns3::PointToPointNetDevice/MacTx",
                                      MakeBoundCallback(&CaptureMacTx, &macSizes));

        Simulator::Stop(Seconds(11.0));
        Simulator::Run();

        // 1) IP layer should see super-segments (evidence GSO emission is wired).
        bool sawSuperSegment =
            std::any_of(ipSizes.begin(), ipSizes.end(), [](uint32_t sz) { return sz > 1500; });
        NS_TEST_ASSERT_MSG_EQ(sawSuperSegment,
                              true,
                              "GSO=on: expected at least one super-segment at IP layer");
        for (uint32_t sz : ipSizes)
        {
            NS_TEST_ASSERT_MSG_LT_OR_EQ(sz,
                                        8000u + 100u,
                                        "GSO=on: IP-layer emission " + std::to_string(sz) +
                                            " exceeded MaxGsoSize+overhead");
        }

        // 2) MAC layer must NEVER see > MTU+overhead: TrafficControlLayer must segment.
        for (uint32_t sz : macSizes)
        {
            NS_TEST_ASSERT_MSG_LT_OR_EQ(
                sz,
                1600u,
                "GSO=on: MAC-layer transmission " + std::to_string(sz) +
                    " > MTU+overhead — TrafficControlLayer did NOT segment");
        }

        // 3) Receiver delivery — TCP correctness across per-segment headers.
        Ptr<PacketSink> sinkPtr = DynamicCast<PacketSink>(sinkApp.Get(0));
        NS_TEST_ASSERT_MSG_EQ(sinkPtr->GetTotalRx(),
                              bytesToSend,
                              "Receiver should get all 100 KB even with GSO=on");

        Config::Reset();
        Simulator::Destroy();
    }
};

/**
 * @ingroup stratum-tests
 */
class TcpGsoEgressTestSuite : public TestSuite
{
  public:
    TcpGsoEgressTestSuite()
        : TestSuite("tcp-gso-egress", Type::UNIT)
    {
        AddTestCase(new TcpGsoEgressBaselineTestCase, Duration::QUICK);
        AddTestCase(new TcpGsoEgressEnabledTestCase, Duration::QUICK);
    }
};

static TcpGsoEgressTestSuite g_tcpGsoEgressTestSuite; //!< Static instance.
