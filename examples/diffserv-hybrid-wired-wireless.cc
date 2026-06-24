/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Hybrid wired/wireless DiffServ4NS example: a wired backbone feeds an
 * 802.11ax AP that acts as the DS edge router. The AP classifies
 * traffic by destination STA on its downlink, marks DSCP, and runs an
 * LLQ scheduler (PQ+WFQ hybrid) on the qdisc. The classification +
 * marking happen at the AP — NOT at the wired servers — which is the
 * realistic deployment shape for residential/SOHO QoS-on-Wi-Fi.
 *
 * Topology:
 *
 *   server-voip ──┐
 *                 ├──P2P 1Gb,1ms──▶ core ──P2P 100Mb,1ms──▶ AP ══Wi-Fi══▶ sta0 (EF, voip)
 *   server-bulk ──┘                                                   ╠══▶ sta1 (AF41, video)
 *                                                                     ╠══▶ sta2 (BE, bulk)
 *                                                                     ╚═══▶ sta3 (BK, bulk)
 *
 * The downstream side is the bottleneck (Wi-Fi link < 100 Mbps backhaul
 * < 1 Gbps server-to-core), so the DiffServ edge on the AP downlink is
 * the only place the classifier needs to live for downlink QoS.
 *
 * A/B flag:
 *
 *   --diffserv=true  : LLQ scheduler on AP downlink, classes differentiated
 *   --diffserv=false : default FIFO, no DiffServ
 *
 * Expected qualitative result with diffserv=true: under saturating
 * BE+BK load, EF p99 OWD stays close to its unloaded baseline; with
 * diffserv=false it grows with the BE backlog.
 *
 * Caveats (carry-over from diffserv-wifi-demo.cc):
 *
 *  - LinkBandwidth on the LLQ scheduler is set to a representative
 *    airtime budget (PHY rate * airtime fraction). A Wi-Fi link is
 *    not a constant-rate pipe.
 *  - L2OverheadBytes is set explicitly; diffserv::Helper::DetectL2Overhead
 *    returns 0 for WifiNetDevice on purpose.
 *  - This is a demo, not a Q-tier validated scenario.
 *  - QoS on the Wi-Fi MAC is enabled (QosSupported=true), so the
 *    DSCP→WMM AC mapping kicks in automatically inside WifiNetDevice.
 *    The qdisc-level LLQ then does an ADDITIONAL layer of differentiation
 *    on top of EDCA, which is the realistic deployment pattern.
 *
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/double.h"
#include "ns3/flow-monitor-helper.h"
#include "ns3/internet-module.h"
#include "ns3/ipv4-flow-classifier.h"
#include "ns3/ipv4-header.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/stratum-constants.h"
#include "ns3/stratum-edge-queue-disc.h"
#include "ns3/stratum-helper.h"
#include "ns3/stratum-install-helper.h"
#include "ns3/stratum-llq-scheduler.h"
#include "ns3/stratum-red-queue-disc.h"
#include "ns3/traffic-control-module.h"
#include "ns3/uinteger.h"
#include "ns3/wifi-module.h"

#include <iomanip>
#include <iostream>
#include <map>

using namespace ns3;
namespace diffserv = ns3::stratum::diffserv;
using ns3::stratum::EdgeQueueDisc;
using ns3::stratum::LlqScheduler;
using ns3::stratum::Meter;
using ns3::stratum::MeterType;
using ns3::stratum::MredMode;
using ns3::stratum::PolicerType;

NS_LOG_COMPONENT_DEFINE("DiffServHybridWiredWireless");

namespace
{

constexpr uint32_t kNumQueues = 4;

double
HistogramP99Ms(const Histogram& h)
{
    uint32_t total = 0;
    for (uint32_t i = 0; i < h.GetNBins(); ++i)
    {
        total += h.GetBinCount(i);
    }
    if (total == 0)
    {
        return 0.0;
    }
    auto target = static_cast<uint32_t>(0.99 * total);
    uint32_t cum = 0;
    for (uint32_t i = 0; i < h.GetNBins(); ++i)
    {
        cum += h.GetBinCount(i);
        if (cum >= target)
        {
            return (h.GetBinStart(i) + h.GetBinEnd(i)) * 0.5 * 1000.0;
        }
    }
    return 0.0;
}

} // namespace

int
main(int argc, char* argv[])
{
    bool diffserv = true;
    double simTime = 10.0;
    double airtimeFraction = 0.65;
    double phyRateMbps = 6.0;
    uint32_t l2OverheadBytes = 36;

    CommandLine cmd;
    cmd.AddValue("diffserv", "Install LLQ DS edge on AP downlink", diffserv);
    cmd.AddValue("simTime", "Simulation duration (s)", simTime);
    cmd.AddValue("airtimeFraction", "Effective share of Wi-Fi PHY rate", airtimeFraction);
    cmd.AddValue("phyRateMbps", "Representative sustained PHY rate (Mb/s)", phyRateMbps);
    cmd.AddValue("l2OverheadBytes", "Per-packet 802.11 framing overhead (B)", l2OverheadBytes);
    cmd.Parse(argc, argv);

    const double schedulerLinkBps = phyRateMbps * 1e6 * airtimeFraction;

    // ---- Nodes ----
    NodeContainer servers;
    servers.Create(2); // 0 = voip server, 1 = bulk server
    NodeContainer core;
    core.Create(1);
    NodeContainer ap;
    ap.Create(1);
    NodeContainer stas;
    stas.Create(kNumQueues);

    // ---- Wired backbone ----
    PointToPointHelper p2pCore;
    p2pCore.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    p2pCore.SetChannelAttribute("Delay", StringValue("1ms"));
    NetDeviceContainer voipCoreDev = p2pCore.Install(servers.Get(0), core.Get(0));
    NetDeviceContainer bulkCoreDev = p2pCore.Install(servers.Get(1), core.Get(0));

    PointToPointHelper p2pBackhaul;
    p2pBackhaul.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    p2pBackhaul.SetChannelAttribute("Delay", StringValue("1ms"));
    NetDeviceContainer coreApDev = p2pBackhaul.Install(core.Get(0), ap.Get(0));

    // ---- Wi-Fi ----
    YansWifiChannelHelper channelHelper = YansWifiChannelHelper::Default();
    YansWifiPhyHelper phy;
    phy.SetChannel(channelHelper.Create());

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211a);
    wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                 "DataMode",
                                 StringValue("OfdmRate6Mbps"),
                                 "ControlMode",
                                 StringValue("OfdmRate6Mbps"));

    WifiMacHelper mac;
    Ssid ssid("ds4-hybrid");
    mac.SetType("ns3::ApWifiMac", "Ssid", SsidValue(ssid), "QosSupported", BooleanValue(true));
    NetDeviceContainer apDev = wifi.Install(phy, mac, ap.Get(0));
    mac.SetType("ns3::StaWifiMac",
                "Ssid",
                SsidValue(ssid),
                "ActiveProbing",
                BooleanValue(false),
                "QosSupported",
                BooleanValue(true));
    NetDeviceContainer staDevs = wifi.Install(phy, mac, stas);

    // ---- Mobility ----
    MobilityHelper mobility;
    Ptr<ListPositionAllocator> pos = CreateObject<ListPositionAllocator>();
    pos->Add(Vector(0.0, 0.0, 0.0));
    for (uint32_t i = 0; i < kNumQueues; ++i)
    {
        pos->Add(Vector(2.0, static_cast<double>(i) * 1.5, 0.0));
    }
    mobility.SetPositionAllocator(pos);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(ap);
    mobility.Install(stas);

    // ---- IP stacks ----
    InternetStackHelper internet;
    internet.Install(servers);
    internet.Install(core);
    internet.Install(ap);
    internet.Install(stas);

    Ipv4AddressHelper addr;
    addr.SetBase("10.0.0.0", "255.255.255.0");
    Ipv4InterfaceContainer voipIf = addr.Assign(voipCoreDev);
    addr.SetBase("10.0.1.0", "255.255.255.0");
    Ipv4InterfaceContainer bulkIf = addr.Assign(bulkCoreDev);
    addr.SetBase("10.0.2.0", "255.255.255.0");
    Ipv4InterfaceContainer coreApIf = addr.Assign(coreApDev);
    addr.SetBase("10.1.0.0", "255.255.255.0");
    Ipv4InterfaceContainer apIf = addr.Assign(apDev);
    Ipv4InterfaceContainer staIf = addr.Assign(staDevs);
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // ---- DiffServ edge on AP downlink ----
    Ptr<NetDevice> apWifi = apDev.Get(0);
    if (diffserv)
    {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        diffserv::Helper helper;
        auto inner = helper.InstallRedInner(edge);
        inner->SetNumQueues(kNumQueues);
        for (uint32_t q = 0; q < kNumQueues; ++q)
        {
            inner->SetNumPrec(q, 1);
            inner->SetQueueLimit(q, 100);
        }

        Ptr<LlqScheduler> sched =
            CreateObjectWithAttributes<LlqScheduler>("NumQueues",
                                                     UintegerValue(kNumQueues),
                                                     "LinkBandwidth",
                                                     DoubleValue(schedulerLinkBps));
        sched->SetParam(0, 0.0);  // EF -> SP
        sched->SetParam(1, 0.50); // AF41
        sched->SetParam(2, 0.33); // BE
        sched->SetParam(3, 0.17); // BK
        sched->SetL2OverheadBytes(l2OverheadBytes);
        inner->SetScheduler(sched);

        constexpr uint8_t kDscpEf = 46;
        constexpr uint8_t kDscpAf41 = 34;
        constexpr uint8_t kDscpBe = 0;
        constexpr uint8_t kDscpCs1 = 8;
        const uint8_t dscps[kNumQueues] = {kDscpEf, kDscpAf41, kDscpBe, kDscpCs1};
        for (uint32_t i = 0; i < kNumQueues; ++i)
        {
            Ipv4Address d = staIf.GetAddress(i);
            edge->AddMarkRule({.dscp = dscps[i], .dstAddr = d});
        }

        for (uint8_t d : dscps)
        {
            helper.AddDumbPolicy(edge, d);
            helper.AddPolicerEntry(edge,
                                   {.policer = PolicerType::DUMB,
                                    .initialCodePt = d,
                                    .downgrade1 = d,
                                    .downgrade2 = d,
                                    .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});
        }
        if (Ptr<Meter> dm = edge->GetMeter(MeterType::DUMB))
        {
            dm->SetL2OverheadBytes(l2OverheadBytes);
        }

        helper.AddPhbEntry(inner, kDscpEf, 0, 0);
        helper.AddPhbEntry(inner, kDscpAf41, 1, 0);
        helper.AddPhbEntry(inner, kDscpBe, 2, 0);
        helper.AddPhbEntry(inner, kDscpCs1, 3, 0);

        stratum::InstallRoot(apWifi, edge);
        edge->Initialize();
        inner->SetMredModeAllQueues(MredMode::DROP_TAIL);
        for (uint32_t q = 0; q < kNumQueues; ++q)
        {
            helper.ConfigQueue(
                inner,
                {.queue = q, .prec = 0, .thMin = 100.0, .thMax = 100.0, .maxP = 1.0});
        }
    }

    // DSCP-mark survival across the wired-to-wireless transition is
    // demonstrated indirectly via per-class p99 OWD: with diffserv=true
    // the EF and AF classes track their unloaded latency while BE/BK
    // degrade under saturation. If the marks were not surviving the AP
    // ingress -> AP qdisc -> Wi-Fi egress path, all four classes would
    // mix in the LLQ scheduler and the differentiation would vanish.

    // ---- Traffic ----
    // Per-class flows: EF (voip server -> sta0), AF41/BE/BK (bulk server -> sta1/2/3).
    // Saturating BE+BK to make the AP qdisc the bottleneck.
    const uint16_t basePort = 9;
    const std::pair<Ipv4Address, std::pair<double, uint32_t>> flows[kNumQueues] = {
        {staIf.GetAddress(0), {300e3, 160}}, // EF
        {staIf.GetAddress(1), {3e6, 1200}},  // AF
        {staIf.GetAddress(2), {30e6, 1400}}, // BE
        {staIf.GetAddress(3), {30e6, 1400}}, // BK
    };
    const Ptr<Node> srcNodes[kNumQueues] = {
        servers.Get(0), // EF -> from voip server
        servers.Get(1), // AF -> from bulk server
        servers.Get(1),
        servers.Get(1),
    };

    ApplicationContainer sinks;
    for (uint32_t i = 0; i < kNumQueues; ++i)
    {
        PacketSinkHelper sh(
            "ns3::UdpSocketFactory",
            InetSocketAddress(Ipv4Address::GetAny(), static_cast<uint16_t>(basePort + i)));
        sinks.Add(sh.Install(stas.Get(i)));
    }
    sinks.Start(Seconds(0.0));

    for (uint32_t i = 0; i < kNumQueues; ++i)
    {
        OnOffHelper src("ns3::UdpSocketFactory",
                        InetSocketAddress(flows[i].first, static_cast<uint16_t>(basePort + i)));
        src.SetAttribute("DataRate", DataRateValue(DataRate(flows[i].second.first)));
        src.SetAttribute("PacketSize", UintegerValue(flows[i].second.second));
        src.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
        src.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
        ApplicationContainer app = src.Install(srcNodes[i]);
        app.Start(Seconds(1.0 + 0.05 * i));
        app.Stop(Seconds(simTime));
    }

    FlowMonitorHelper fmHelper;
    Ptr<FlowMonitor> monitor = fmHelper.InstallAll();
    monitor->SetAttribute("DelayBinWidth", DoubleValue(0.0005));

    Simulator::Stop(Seconds(simTime + 0.5));
    Simulator::Run();

    monitor->CheckForLostPackets();
    Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier>(fmHelper.GetClassifier());
    auto stats = monitor->GetFlowStats();

    // Per-class summary
    const std::map<uint16_t, std::string> portToClass = {
        {basePort + 0, "EF"},
        {basePort + 1, "AF41"},
        {basePort + 2, "BE"},
        {basePort + 3, "BK"},
    };
    std::map<std::string, std::pair<double, double>> perClass; // {kbps, p99 ms}
    for (const auto& [flowId, fs] : stats)
    {
        Ipv4FlowClassifier::FiveTuple ft = classifier->FindFlow(flowId);
        auto it = portToClass.find(ft.destinationPort);
        if (it == portToClass.end())
        {
            continue;
        }
        const double rxBits = static_cast<double>(fs.rxBytes) * 8.0;
        const double durSec = simTime - 1.0;
        perClass[it->second].first += rxBits / durSec / 1e3;
        perClass[it->second].second =
            std::max(perClass[it->second].second, HistogramP99Ms(fs.delayHistogram));
    }

    std::cout << "diffserv=" << (diffserv ? "on" : "off") << std::endl;
    std::cout << std::left << std::setw(8) << "class" << std::right << std::setw(12) << "rx_kbps"
              << std::setw(12) << "p99_ms" << std::endl;
    for (const std::string& c : {"EF", "AF41", "BE", "BK"})
    {
        std::cout << std::left << std::setw(8) << c << std::right << std::fixed
                  << std::setprecision(1) << std::setw(12) << perClass[c].first << std::setw(12)
                  << perClass[c].second << std::endl;
    }

    Simulator::Destroy();
    return 0;
}
