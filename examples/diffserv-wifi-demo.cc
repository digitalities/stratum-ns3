/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Wireless demo: DiffServ4NS edge disc attached to a Wi-Fi 802.11ax AP's
 * downlink NetDevice. Demonstrates that the substrate is wireless-agnostic
 * (the queue disc plugs into any TrafficControlLayer) and exercises the
 * DSCP -> 802.11e UP mapping that ns-3 mainline already provides via
 * `qos-utils.cc::QosUtilsMapTidToAc` (RFC 8325-shaped).
 *
 * Topology:
 *
 * server --P2P 100Mb,1ms--> AP --Wi-Fi 802.11ax 5GHz--> sta0 (EF)
 * \-> sta1 (BE)
 *
 * The DiffServ edge disc sits on the AP's Wi-Fi NetDevice (downlink). The
 * AP DSCP-marks two flows on the way out: 300 kbps EF (DSCP 46) to sta0
 * and a saturating BE flow (DSCP 0) to sta1. With DiffServ enabled, EF
 * latency under BE saturation should track the unloaded baseline; without
 * it (--diffserv=false) the BE flow shares the queue with EF.
 *
 * Caveats (documented, intentional):
 *
 * - LinkBandwidth on the scheduler is set to a representative airtime
 * budget (PHY rate * airtime fraction). A Wi-Fi link is not a
 * constant-rate pipe; this is the same approximation Linux `tc-cake`
 * users make on a Wi-Fi AP.
 * - L2OverheadBytes is set explicitly. diffserv::Helper::DetectL2OverheadBytes
 * returns 0 for WifiNetDevice on purpose (per-packet, variable);
 * here we pass a representative LLC/SNAP+QoS+MAC header size.
 * - This is a demo, not a Q-tier validated scenario. There is no
 * external calibration target. The artefact is the topology +
 * attachment pattern, not a numeric claim.
 *
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/double.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/stratum-constants.h"
#include "ns3/stratum-edge-queue-disc.h"
#include "ns3/stratum-helper.h"
#include "ns3/stratum-install-helper.h"
#include "ns3/stratum-pq-scheduler.h"
#include "ns3/stratum-red-queue-disc.h"
#include "ns3/traffic-control-module.h"
#include "ns3/uinteger.h"
#include "ns3/wifi-module.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <system_error>

using namespace ns3;
namespace diffserv = ns3::stratum::diffserv;
using ns3::stratum::EdgeQueueDisc;
using ns3::stratum::Meter;
using ns3::stratum::MeterType;
using ns3::stratum::MredMode;
using ns3::stratum::PolicerType;
using ns3::stratum::PriorityScheduler;

NS_LOG_COMPONENT_DEFINE("DiffServWifiDemo");

int
main(int argc, char* argv[])
{
    // ---- CLI ----
    bool diffserv = true;
    double simTime = 10.0;
    double cirEfBps = 300e3;       // EF rate (matches example-1)
    double cbsEfBytes = 4687.0;    // Cisco MQC default Bc (CIR * 125 ms)
    double airtimeFraction = 0.45; // representative effective share of PHY rate
    double phyRateMbps = 60.0;     // representative HE-MCS sustained rate
    uint32_t l2OverheadBytes = 36; // 802.11 QoS-data + LLC/SNAP, no AMPDU
    uint32_t beRateMbps = 50;      // saturating BE flow

    CommandLine cmd;
    cmd.AddValue("diffserv", "Attach DiffServ edge on AP downlink", diffserv);
    cmd.AddValue("simTime", "Simulation duration (s)", simTime);
    cmd.AddValue("airtimeFraction", "Effective share of Wi-Fi PHY rate", airtimeFraction);
    cmd.AddValue("phyRateMbps", "Representative sustained PHY rate (Mb/s)", phyRateMbps);
    cmd.AddValue("l2OverheadBytes", "Per-packet 802.11 framing overhead (B)", l2OverheadBytes);
    cmd.AddValue("beRateMbps", "Saturating BE flow rate (Mb/s)", beRateMbps);
    cmd.Parse(argc, argv);

    double schedulerLinkBps = phyRateMbps * 1e6 * airtimeFraction;

    // ---- Nodes ----
    NodeContainer server;
    server.Create(1);
    NodeContainer ap;
    ap.Create(1);
    NodeContainer stas;
    stas.Create(2);

    // ---- Backhaul P2P (server <-> AP) ----
    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("1ms"));
    NetDeviceContainer p2pDevs = p2p.Install(server.Get(0), ap.Get(0));

    // ---- Wi-Fi 802.11ax 5GHz ----
    YansWifiChannelHelper channelHelper = YansWifiChannelHelper::Default();
    YansWifiPhyHelper phy;
    phy.SetChannel(channelHelper.Create());

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211ax);
    wifi.SetRemoteStationManager("ns3::IdealWifiManager");

    WifiMacHelper mac;
    Ssid ssid("ds4-wifi-demo");

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

    // ---- Mobility (static placement) ----
    MobilityHelper mobility;
    Ptr<ListPositionAllocator> pos = CreateObject<ListPositionAllocator>();
    pos->Add(Vector(0.0, 0.0, 0.0)); // AP
    pos->Add(Vector(2.0, 0.0, 0.0)); // sta0 (EF)
    pos->Add(Vector(2.0, 2.0, 0.0)); // sta1 (BE)
    mobility.SetPositionAllocator(pos);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(ap);
    mobility.Install(stas);

    // ---- IP stacks ----
    InternetStackHelper internet;
    internet.Install(server);
    internet.Install(ap);
    internet.Install(stas);

    Ipv4AddressHelper addr;
    addr.SetBase("10.0.0.0", "255.255.255.0");
    Ipv4InterfaceContainer p2pIf = addr.Assign(p2pDevs);
    addr.SetBase("10.1.0.0", "255.255.255.0");
    Ipv4InterfaceContainer apIf = addr.Assign(apDev);
    Ipv4InterfaceContainer staIf = addr.Assign(staDevs);
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    Ipv4Address efDst = staIf.GetAddress(0);
    Ipv4Address beDst = staIf.GetAddress(1);

    // ---- Optional: DiffServ edge on AP's Wi-Fi NetDevice (downlink) ----
    if (diffserv)
    {
        Ptr<NetDevice> apWifi = apDev.Get(0);

        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        diffserv::Helper helper;
        auto inner = helper.InstallRedInner(edge);

        inner->SetNumQueues(2);
        inner->SetNumPrec(0, 2);
        inner->SetNumPrec(1, 1);
        inner->SetQueueLimit(0, 30);
        inner->SetQueueLimit(1, 100);

        Ptr<PriorityScheduler> pq = CreateObjectWithAttributes<PriorityScheduler>("NumQueues",
                                                                                  UintegerValue(2),
                                                                                  "WinLen",
                                                                                  DoubleValue(1.0));
        // Wireless caveat: this is a representative airtime budget, not a
        // physical line rate. Document via the scheduler attribute.
        pq->SetLinkBandwidth(schedulerLinkBps);
        // Wi-Fi L2 overhead: variable per-packet; pass a representative value.
        pq->SetL2OverheadBytes(l2OverheadBytes);
        inner->SetScheduler(pq);

        edge->AddMarkRule({.dscp = 46, .dstAddr = efDst});
        edge->AddMarkRule({.dscp = 0, .dstAddr = beDst});

        helper.AddTokenBucketPolicy(edge,
                                    {.codePt = 46, .cirBps = cirEfBps, .cbsBytes = cbsEfBytes});
        helper.AddDumbPolicy(edge, 48);
        helper.AddDumbPolicy(edge, 0);
        helper.AddPolicerEntry(edge,
                               {.policer = PolicerType::TOKEN_BUCKET,
                                .initialCodePt = 46,
                                .downgrade1 = 48,
                                .downgrade2 = 48,
                                .policyIndex = static_cast<uint32_t>(PolicerType::TOKEN_BUCKET)});
        helper.AddPolicerEntry(edge,
                               {.policer = PolicerType::DUMB,
                                .initialCodePt = 0,
                                .downgrade1 = 0,
                                .downgrade2 = 0,
                                .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});
        if (Ptr<Meter> tbm = edge->GetMeter(MeterType::TOKEN_BUCKET))
        {
            tbm->SetL2OverheadBytes(l2OverheadBytes);
        }
        if (Ptr<Meter> dm = edge->GetMeter(MeterType::DUMB))
        {
            dm->SetL2OverheadBytes(l2OverheadBytes);
        }

        helper.AddPhbEntry(inner, 46, 0, 0);
        helper.AddPhbEntry(inner, 48, 0, 1);
        helper.AddPhbEntry(inner, 0, 1, 0);

        stratum::InstallRoot(apWifi, edge);
        edge->Initialize();
        inner->SetMredModeAllQueues(MredMode::DROP_TAIL);
        helper.ConfigQueue(inner,
                           {.queue = 0, .prec = 0, .thMin = 30.0, .thMax = 30.0, .maxP = 1.0});
        helper.ConfigQueue(inner,
                           {.queue = 0, .prec = 1, .thMin = -1.0, .thMax = -1.0, .maxP = 0.0});
        helper.ConfigQueue(inner,
                           {.queue = 1, .prec = 0, .thMin = 100.0, .thMax = 100.0, .maxP = 1.0});

        // Once the DSCP is stamped by the edge mark rule, the standard
        // ns-3 path forwards the packet to WifiNetDevice; the WMM mapper
        // (`qos-utils.cc::QosUtilsMapTidToAc`, RFC 8325-shaped) routes
        // DSCP 46 -> AC_VO and DSCP 0 -> AC_BE without further wiring.
    }

    // ---- Traffic ----
    uint16_t efPort = 9;
    uint16_t bePort = 10;

    PacketSinkHelper efSinkH("ns3::UdpSocketFactory",
                             InetSocketAddress(Ipv4Address::GetAny(), efPort));
    PacketSinkHelper beSinkH("ns3::UdpSocketFactory",
                             InetSocketAddress(Ipv4Address::GetAny(), bePort));
    ApplicationContainer efSinkApps = efSinkH.Install(stas.Get(0));
    ApplicationContainer beSinkApps = beSinkH.Install(stas.Get(1));
    efSinkApps.Start(Seconds(0.0));
    beSinkApps.Start(Seconds(0.0));

    OnOffHelper efSrc("ns3::UdpSocketFactory", InetSocketAddress(efDst, efPort));
    efSrc.SetAttribute("DataRate", DataRateValue(DataRate(cirEfBps)));
    efSrc.SetAttribute("PacketSize", UintegerValue(160));
    efSrc.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
    efSrc.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
    ApplicationContainer efApp = efSrc.Install(server.Get(0));
    efApp.Start(Seconds(1.0));
    efApp.Stop(Seconds(simTime));

    OnOffHelper beSrc("ns3::UdpSocketFactory", InetSocketAddress(beDst, bePort));
    beSrc.SetAttribute("DataRate", DataRateValue(DataRate(beRateMbps * 1000000)));
    beSrc.SetAttribute("PacketSize", UintegerValue(1400));
    beSrc.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
    beSrc.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
    ApplicationContainer beApp = beSrc.Install(server.Get(0));
    beApp.Start(Seconds(2.0));
    beApp.Stop(Seconds(simTime));

    Simulator::Stop(Seconds(simTime + 0.5));
    Simulator::Run();

    Ptr<PacketSink> efSinkApp = efSinkApps.Get(0)->GetObject<PacketSink>();
    Ptr<PacketSink> beSinkApp = beSinkApps.Get(0)->GetObject<PacketSink>();
    std::cout << "[ds4-wifi-demo] diffserv=" << (diffserv ? "on" : "off")
              << " EF rx=" << (efSinkApp ? efSinkApp->GetTotalRx() : 0)
              << " BE rx=" << (beSinkApp ? beSinkApp->GetTotalRx() : 0) << std::endl;

    // ---- CSV write: flow-summary.csv (space-sep, no header; matches
    //      plot-recipe wireless-aqm-evaluation csv_opts schema). The
    //      p99_ms column is a placeholder (0.0); the per-flow-bar plot
    //      only reads rx_kbps. Adding p99 measurement would require
    //      new instrumentation, out of scope.
    {
        const uint64_t efBytes = efSinkApp ? efSinkApp->GetTotalRx() : 0;
        const uint64_t beBytes = beSinkApp ? beSinkApp->GetTotalRx() : 0;
        const double efKbps = static_cast<double>(efBytes) * 8.0 / simTime / 1e3;
        const double beKbps = static_cast<double>(beBytes) * 8.0 / simTime / 1e3;
        const std::string armDir =
            "output/ns3/wifi-aqm/" + std::string(diffserv ? "on" : "off") + "/";
        std::error_code ec;
        std::filesystem::create_directories(armDir, ec);
        if (ec)
        {
            NS_ABORT_MSG("create_directories(" << armDir << ") failed: " << ec.message());
        }
        std::ofstream csv(armDir + "flow-summary.csv");
        csv << "EF " << std::fixed << std::setprecision(1) << efKbps << " 0.0\n";
        csv << "BE " << beKbps << " 0.0\n";
    }

    Simulator::Destroy();
    return 0;
}
