/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Host-isolation characterisation in pure ns-3 (mainline B path).
 *
 * Parameterised over (groupAHosts, groupAFlowsPerHost,
 * groupBHosts, groupBFlowsPerHost). Each test case runs once and emits
 * a single PHASE1-CELL line to stdout for post-processing by
 * scripts/host_iso_phase_1_pivot.py.
 *
 * Asserts only bytesA > 0 and bytesB > 0 (simulator ran). The ratio is
 * recorded but not gated; a fixed ratio gate is inappropriate.
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/flow-monitor-helper.h"
#include "ns3/fq-cobalt-queue-disc.h"
#include "ns3/internet-module.h"
#include "ns3/ipv4-flow-classifier.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/test.h"
#include "ns3/traffic-control-module.h"

#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace ns3;

namespace
{

enum class TcpVariant
{
    NewReno,
    Cubic,
    Bbr
};

const char*
TcpVariantLabel(TcpVariant v)
{
    switch (v)
    {
    case TcpVariant::NewReno:
        return "NewReno";
    case TcpVariant::Cubic:
        return "Cubic";
    case TcpVariant::Bbr:
        return "Bbr";
    }
    return "Unknown";
}

const char*
TcpVariantTypeId(TcpVariant v)
{
    // Maps to the TypeIds registered by ns-3 TcpL4Protocol::SocketType.
    // See mem:reference_ns3_socket_type_default_global_late_bound for why
    // SetDefault must be called per-test-case (Config defaults persist across
    // Simulator::Run within the same process; the last SetDefault wins).
    switch (v)
    {
    case TcpVariant::NewReno:
        return "ns3::TcpNewReno";
    case TcpVariant::Cubic:
        return "ns3::TcpCubic";
    case TcpVariant::Bbr:
        return "ns3::TcpBbr";
    }
    return "ns3::TcpNewReno";
}

/**
 * @brief Host-isolation characterisation cell (mainline B path).
 *
 * One cell = one (gA_hosts, gA_flowsPerHost, gB_hosts,
 * gB_flowsPerHost) point. Mirrors the topology and traffic-generator
 * structure of the Q-15.9 characterisation fixture
 * (diffserv-cake-q15-test-suite.cc, RrulMultiHostFairnessTest) but
 * generalises Group B from a single source-host to m_gB_h source-hosts.
 */
class HostIsoPhase1CharacterisationCase : public TestCase
{
  public:
    HostIsoPhase1CharacterisationCase(uint32_t groupAHosts,
                                      uint32_t groupAFlowsPerHost,
                                      uint32_t groupBHosts,
                                      uint32_t groupBFlowsPerHost,
                                      uint32_t seed,
                                      TcpVariant tcpVariant);

  private:
    void DoRun() override;
    std::pair<uint64_t, uint64_t> RunScenario();

    uint32_t m_gA_h;
    uint32_t m_gA_f;
    uint32_t m_gB_h;
    uint32_t m_gB_f;
    uint32_t m_seed;
    TcpVariant m_tcpVariant;
};

HostIsoPhase1CharacterisationCase::HostIsoPhase1CharacterisationCase(uint32_t groupAHosts,
                                                                     uint32_t groupAFlowsPerHost,
                                                                     uint32_t groupBHosts,
                                                                     uint32_t groupBFlowsPerHost,
                                                                     uint32_t seed,
                                                                     TcpVariant tcpVariant)
    : TestCase("host-iso characterisation cell"
               " gA=" +
               std::to_string(groupAHosts) + "h*" + std::to_string(groupAFlowsPerHost) +
               "f gB=" + std::to_string(groupBHosts) + "h*" + std::to_string(groupBFlowsPerHost) +
               "f seed=" + std::to_string(seed) +
               " tcp=" + std::string(TcpVariantLabel(tcpVariant))),
      m_gA_h(groupAHosts),
      m_gA_f(groupAFlowsPerHost),
      m_gB_h(groupBHosts),
      m_gB_f(groupBFlowsPerHost),
      m_seed(seed),
      m_tcpVariant(tcpVariant)
{
}

std::pair<uint64_t, uint64_t>
HostIsoPhase1CharacterisationCase::RunScenario()
{
    // Mirror Q-15.9 anchor numbers (bottleneck rate, simTime, link
    // attrs). See diffserv-cake-q15-test-suite.cc (RrulMultiHostFairnessTest)
    // for the Q-15.9 fixture this generalises from.
    const double bottleneckBps = 10e6;
    const double simTime = 30.0;
    const double measureStart = 5.0;
    (void)measureStart; // ratio cancels uniform-window approximation.

    NodeContainer groupA;
    groupA.Create(m_gA_h);
    NodeContainer groupB;
    groupB.Create(m_gB_h);
    NodeContainer routers;
    routers.Create(2);
    NodeContainer sinkNode;
    sinkNode.Create(1);

    PointToPointHelper access;
    access.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    access.SetChannelAttribute("Delay", StringValue("1ms"));
    PointToPointHelper bottleneck;
    bottleneck.SetDeviceAttribute("DataRate", DataRateValue(DataRate(bottleneckBps)));
    bottleneck.SetChannelAttribute("Delay", StringValue("10ms"));
    // 1p NetDevice queue forces qdisc-LLQ buffering at the qdisc layer.
    bottleneck.SetQueue("ns3::DropTailQueue", "MaxSize", StringValue("1p"));

    InternetStackHelper stack;
    stack.Install(groupA);
    stack.Install(groupB);
    stack.Install(routers);
    stack.Install(sinkNode);

    Ipv4AddressHelper addr;
    // Group A source hosts: 10.1.h.0/24 (one /24 per host).
    std::vector<Ipv4InterfaceContainer> groupAIfs(m_gA_h);
    for (uint32_t i = 0; i < m_gA_h; ++i)
    {
        NetDeviceContainer dev = access.Install(groupA.Get(i), routers.Get(0));
        std::ostringstream net;
        net << "10.1." << (i + 1) << ".0";
        addr.SetBase(net.str().c_str(), "255.255.255.0");
        groupAIfs[i] = addr.Assign(dev);
    }
    // Group B source hosts: 10.2.h.0/24 (one /24 per host). Each host gets
    // its own P2P access link to routers[0] mirroring Group A's pattern
    // so source-side fairness is honest.
    std::vector<Ipv4InterfaceContainer> groupBIfs(m_gB_h);
    for (uint32_t i = 0; i < m_gB_h; ++i)
    {
        NetDeviceContainer dev = access.Install(groupB.Get(i), routers.Get(0));
        std::ostringstream net;
        net << "10.2." << (i + 1) << ".0";
        addr.SetBase(net.str().c_str(), "255.255.255.0");
        groupBIfs[i] = addr.Assign(dev);
    }

    NetDeviceContainer bnDev = bottleneck.Install(routers.Get(0), routers.Get(1));
    addr.SetBase("10.3.1.0", "255.255.255.0");
    addr.Assign(bnDev);

    NetDeviceContainer sinkDev = access.Install(routers.Get(1), sinkNode.Get(0));
    addr.SetBase("10.4.1.0", "255.255.255.0");
    Ipv4InterfaceContainer sinkIf = addr.Assign(sinkDev);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // Install root qdisc at the bottleneck egress (routers[0] side).
    // Mainline FqCobaltQueueDisc with patch 0016 host-isolation attribute surface.
    Ptr<NetDevice> bnEgress = bnDev.Get(0);
    Ptr<TrafficControlLayer> tcl = bnEgress->GetNode()->GetObject<TrafficControlLayer>();
    if (tcl->GetRootQueueDiscOnDevice(bnEgress))
    {
        tcl->DeleteRootQueueDiscOnDevice(bnEgress);
    }
    TrafficControlHelper tch;
    tch.SetRootQueueDisc("ns3::FqCobaltQueueDisc",
                         "EnableSetAssociativeHash",
                         BooleanValue(true),
                         "SetWays",
                         UintegerValue(8),
                         "EnableHostIsolation",
                         BooleanValue(true),
                         "HostIsolationMode",
                         EnumValue(FqCobaltQueueDisc::HostIsolationMode::Triple));
    tch.Install(NetDeviceContainer(bnEgress));

    // Sink applications: one PacketSink per flow port.
    constexpr uint16_t kBasePort = 9100;
    ApplicationContainer sinkApps;
    const uint32_t totalAFlows = m_gA_h * m_gA_f;
    const uint32_t totalBFlows = m_gB_h * m_gB_f;
    const uint32_t totalFlows = totalAFlows + totalBFlows;
    for (uint32_t f = 0; f < totalFlows; ++f)
    {
        const uint16_t port = static_cast<uint16_t>(kBasePort + f);
        PacketSinkHelper sinkHelper("ns3::TcpSocketFactory",
                                    InetSocketAddress(Ipv4Address::GetAny(), port));
        sinkApps.Add(sinkHelper.Install(sinkNode.Get(0)));
    }
    sinkApps.Start(Seconds(0.0));
    sinkApps.Stop(Seconds(simTime + 1.0));

    // Group A traffic: m_gA_h source hosts x m_gA_f flows each.
    uint32_t flowIdx = 0;
    for (uint32_t h = 0; h < m_gA_h; ++h)
    {
        for (uint32_t k = 0; k < m_gA_f; ++k)
        {
            const uint16_t port = static_cast<uint16_t>(kBasePort + flowIdx);
            BulkSendHelper src("ns3::TcpSocketFactory",
                               InetSocketAddress(sinkIf.GetAddress(1), port));
            src.SetAttribute("MaxBytes", UintegerValue(0));
            ApplicationContainer app = src.Install(groupA.Get(h));
            app.Start(Seconds(0.5 + 0.001 * flowIdx));
            app.Stop(Seconds(simTime));
            ++flowIdx;
        }
    }
    // Group B traffic: m_gB_h source hosts x m_gB_f flows each.
    for (uint32_t h = 0; h < m_gB_h; ++h)
    {
        for (uint32_t k = 0; k < m_gB_f; ++k)
        {
            const uint16_t port = static_cast<uint16_t>(kBasePort + flowIdx);
            BulkSendHelper src("ns3::TcpSocketFactory",
                               InetSocketAddress(sinkIf.GetAddress(1), port));
            src.SetAttribute("MaxBytes", UintegerValue(0));
            ApplicationContainer app = src.Install(groupB.Get(h));
            app.Start(Seconds(0.5 + 0.001 * flowIdx));
            app.Stop(Seconds(simTime));
            ++flowIdx;
        }
    }

    FlowMonitorHelper fmHelper;
    Ptr<FlowMonitor> fm = fmHelper.InstallAll();

    Simulator::Stop(Seconds(simTime + 1.0));
    Simulator::Run();

    fm->CheckForLostPackets();
    Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier>(fmHelper.GetClassifier());

    uint64_t bytesA = 0;
    uint64_t bytesB = 0;
    const auto& stats = fm->GetFlowStats();
    for (const auto& p : stats)
    {
        FlowMonitor::FlowStats fs = p.second;
        Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow(p.first);
        if (t.destinationPort < kBasePort || t.destinationPort >= kBasePort + totalFlows)
        {
            continue;
        }
        // Source IP differentiates Group A (10.1.x.x) from Group B (10.2.x.x).
        const uint32_t srcRaw = t.sourceAddress.Get();
        const uint32_t group = (srcRaw >> 16) & 0xff; // second octet of 10.X.y.z
        if (group == 1)
        {
            bytesA += fs.rxBytes;
        }
        else if (group == 2)
        {
            bytesB += fs.rxBytes;
        }
    }

    Simulator::Destroy();
    return {bytesA, bytesB};
}

void
HostIsoPhase1CharacterisationCase::DoRun()
{
    // Per-case SetDefault: Config defaults persist across Simulator::Run within
    // the same process (mem:reference_ns3_socket_type_default_global_late_bound).
    // Must set BEFORE RunScenario builds the TCP sockets.
    Config::SetDefault("ns3::TcpL4Protocol::SocketType",
                       TypeIdValue(TypeId::LookupByName(TcpVariantTypeId(m_tcpVariant))));
    RngSeedManager::SetRun(m_seed);
    auto [bytesA, bytesB] = RunScenario();

    NS_TEST_ASSERT_MSG_GT(bytesA, 0, "Group A received zero bytes -- simulator did not run");
    NS_TEST_ASSERT_MSG_GT(bytesB, 0, "Group B received zero bytes -- simulator did not run");

    const double ratio = static_cast<double>(bytesA) / static_cast<double>(bytesB);
    const double total = static_cast<double>(bytesA) + static_cast<double>(bytesB);
    const double shareA = total > 0 ? static_cast<double>(bytesA) / total : 0.0;

    std::ostringstream phase1Cell;
    phase1Cell << "PHASE1-CELL"
              << " gA_h=" << m_gA_h << " gA_f=" << m_gA_f << " gB_h=" << m_gB_h
              << " gB_f=" << m_gB_f << " seed=" << m_seed
              << " tcp=" << TcpVariantLabel(m_tcpVariant) << " bytes_A=" << bytesA
              << " bytes_B=" << bytesB << " ratio=" << ratio << " share_A=" << shareA;
    std::cout << phase1Cell.str() << std::endl;
}

// ===========================================================================
// Test suite registration
// ===========================================================================

class DiffservCakeHostIsoPhase1TestSuite : public TestSuite
{
  public:
    DiffservCakeHostIsoPhase1TestSuite()
        : TestSuite("stratum-cake-host-iso-phase-1", Type::EXAMPLE)
    {
        // Protocol sweep: 3 TCPs x 3 seeds at the Stratum-bridge F2 anchor
        // (16h*1f vs 1h*16f, shared sink) = 9 cells. Mainline B path only;
        // the prior wrapper impl was retired after characterisation confirmed
        // B is Linux-faithful at F2 across CUBIC/NewReno/BBR.
        for (auto tcp : {TcpVariant::NewReno, TcpVariant::Cubic, TcpVariant::Bbr})
        {
            for (uint32_t seed = 1; seed <= 3; ++seed)
            {
                AddTestCase(new HostIsoPhase1CharacterisationCase(16, 1, 1, 16, seed, tcp),
                            Duration::EXTENSIVE);
            }
        }
    }
};

static DiffservCakeHostIsoPhase1TestSuite g_diffservCakeHostIsoPhase1TestSuite;

} // namespace
