/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Wireless scheduler comparison demo: 8 DiffServ4NS schedulers (PQ, RR, WRR,
 * WIRR, SCFQ, WFQ, WF2Q+, LLQ) on the AP downlink over 802.11a 6 Mb/s, plus
 * a single-AC saturation mode for Bianchi 2000 / Magrin et al. WNS3 2021
 * Figure 3 calibration. Demonstrates that the scheduler choice is independent
 * of the substrate — every scheduler in the module composes with
 * WifiNetDevice the same way it composes with PointToPointNetDevice.
 *
 * Topology (qdisc-comparison mode):
 *
 *                          +-- sta0 (EF, DSCP 46)
 *   server --P2P 1Gb,1ms-- AP --802.11a 6 Mb/s-- sta1 (AF41, DSCP 34)
 *                          +-- sta2 (BE, DSCP 0)
 *                          +-- sta3 (BK, DSCP 8)
 *
 * CLI flags (run as `./ns3 run "diffserv-wifi-scheduler-comparison [flags]"`):
 *
 *   --scheduler={pq|rr|wrr|wirr|scfq|wfq|wf2qp|llq}    Stratum qdisc scheduler
 *                                                      (default pq).
 *   --wmmMode={off|hybrid|qdisc-only|edca-only}        L2 differentiation
 *                                                      mode (default off).
 *                                                      See handbook §12.6.1.
 *   --lowLoad={true|false}                             ~7 Mb/s vs ~128 Mb/s
 *                                                      offered (default
 *                                                      false).
 *   --singleAcSaturation={true|false}                  Switch to single-AC
 *                                                      saturation mode
 *                                                      (Bianchi / Magrin).
 *   --standard={80211a|80211ax}                        Wi-Fi standard
 *                                                      (default 80211a).
 *   --heMcs=N                                          HE MCS 0-11 when
 *                                                      standard=80211ax.
 *   --numStas=N                                        STA count in
 *                                                      saturation mode.
 *   --simTime=SECS                                     Simulation duration.
 *   --airtimeFraction=FRAC                             Scheduler airtime
 *                                                      budget fraction.
 *   --phyRateMbps=MBPS                                 Scheduler LinkBandwidth.
 *   --l2OverheadBytes=BYTES                            Per-packet 802.11
 *                                                      framing overhead.
 *
 * Output (qdisc-comparison): single CSV row per run, format
 *   scheduler,ef_kbps,ef_p99_ms,af_kbps,af_p99_ms,be_kbps,be_p99_ms,bk_kbps,bk_p99_ms
 *
 * Output (saturation):
 *   single_ac_saturation,standard=...,heMcs=...,numStas=...,phy_mbps=...,
 *     dl_mbps=...,ul_mbps=...,aggregate_mbps=...
 *
 * Caveats (carry-over from diffserv-wifi-demo.cc):
 *
 *  - LinkBandwidth on each scheduler is set to a representative airtime
 *    budget (PHY rate * airtime fraction). A Wi-Fi link is not a
 *    constant-rate pipe; this is the same approximation Linux `tc-cake`
 *    users make on a Wi-Fi AP.
 *  - L2OverheadBytes is set explicitly. diffserv::Helper::DetectL2OverheadBytes
 *    returns 0 for WifiNetDevice on purpose (per-packet, variable);
 *    here we pass a representative LLC/SNAP+QoS+MAC header size.
 *  - This is a comparison demo, not a Q-tier validated scenario. The
 *    artefact is the topology + scheduler-attachment pattern. Numeric
 *    differences between schedulers are illustrative, not certified.
 *
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/double.h"
#include "ns3/flow-monitor-helper.h"
#include "ns3/internet-module.h"
#include "ns3/ipv4-flow-classifier.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/stratum-constants.h"
#include "ns3/stratum-edge-queue-disc.h"
#include "ns3/stratum-helper.h"
#include "ns3/stratum-llq-scheduler.h"
#include "ns3/stratum-pq-scheduler.h"
#include "ns3/stratum-red-queue-disc.h"
#include "ns3/stratum-rr-scheduler.h"
#include "ns3/stratum-scfq-scheduler.h"
#include "ns3/stratum-scheduler-registry.h"
#include "ns3/stratum-scheduler.h"
#include "ns3/stratum-wf2qp-scheduler.h"
#include "ns3/stratum-wfq-scheduler.h"
#include "ns3/stratum-wirr-scheduler.h"
#include "ns3/stratum-wrr-scheduler.h"
#include "ns3/traffic-control-module.h"
#include "ns3/uinteger.h"
#include "ns3/wifi-module.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <system_error>

using namespace ns3;
namespace diffserv = ns3::stratum::diffserv;
using ns3::stratum::EdgeQueueDisc;
using ns3::stratum::kAnyHost;
using ns3::stratum::kAnyProtocol;
using ns3::stratum::Meter;
using ns3::stratum::MeterType;
using ns3::stratum::MredMode;
using ns3::stratum::PolicerType;
using ns3::stratum::RoundRobinScheduler;
using ns3::stratum::SchedulerArgs;
using ns3::stratum::SchedulerEntry;
using ns3::stratum::SchedulerRegistry;

NS_LOG_COMPONENT_DEFINE("DiffServWifiSchedulerComparison");

namespace
{

constexpr uint32_t kNumQueues = 4; // EF=0, AF=1, BE=2, BK=3

Ptr<stratum::Scheduler>
MakeScheduler(const std::string& name, double linkBps, uint32_t l2Overhead)
{
    using PS = SchedulerEntry::ParameterShape;

    const SchedulerEntry* entry = SchedulerRegistry::Get().Find(name);
    NS_ABORT_MSG_IF(
        !entry,
        "Unknown scheduler '" << name << "'. Valid: " << [] {
            std::string s;
            for (const auto& t : SchedulerRegistry::Get().FileTags())
            {
                if (!s.empty())
                {
                    s += "|";
                }
                s += t;
            }
            return s;
        }());

    SchedulerArgs args;
    args.numQueues = kNumQueues;
    args.linkBps = linkBps;
    // Per-queue weight policy for the EF/AF/BE/BK 4-queue setup. The
    // registry tells us which shape the scheduler expects; we supply
    // the matching default vector. PQ + RR carry no per-queue weights.
    switch (entry->parameterShape)
    {
    case PS::None:
    case PS::PriorityWinLen:
        break;
    case PS::RoundRobinWeights:
        args.weights = {4, 3, 2, 1}; // EF, AF, BE, BK
        break;
    case PS::FairQueueShares:
        args.weights = {0.40, 0.30, 0.20, 0.10}; // EF, AF, BE, BK
        break;
    case PS::HybridLlq:
        args.weights = {0.0, 0.50, 0.33, 0.17}; // EF=SP slot; 1..3 share residual
        break;
    }

    Ptr<stratum::Scheduler> sched = SchedulerRegistry::Get().Construct(name, args);
    sched->SetLinkBandwidth(linkBps);
    sched->SetL2OverheadBytes(l2Overhead);
    return sched;
}

struct ClassStats
{
    double rxKbps{0.0};
    double p99Ms{0.0};
};

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
    uint32_t target = static_cast<uint32_t>(0.99 * total);
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
    // ---- CLI ----
    std::string scheduler = "pq";
    bool singleAcSaturation = false;
    double simTime = 10.0;
    double airtimeFraction = 0.65;
    double phyRateMbps = 6.0; // 802.11a OfdmRate6Mbps
    uint32_t l2OverheadBytes = 36;
    std::string standard = "80211a";
    uint32_t heMcs = 5;
    uint32_t numStas = 1;
    std::string wmmMode = "off";
    bool lowLoad = false;

    CommandLine cmd;
    cmd.AddValue("scheduler",
                 "DiffServ4NS scheduler: pq|rr|wrr|wirr|scfq|wfq|wf2qp|llq",
                 scheduler);
    cmd.AddValue("singleAcSaturation",
                 "Single-AC saturation mode (Bianchi 2000 sanity check)",
                 singleAcSaturation);
    cmd.AddValue("simTime", "Simulation duration (s)", simTime);
    cmd.AddValue("airtimeFraction", "Effective share of Wi-Fi PHY rate", airtimeFraction);
    cmd.AddValue("phyRateMbps", "Representative sustained PHY rate (Mb/s)", phyRateMbps);
    cmd.AddValue("l2OverheadBytes", "Per-packet 802.11 framing overhead (B)", l2OverheadBytes);
    cmd.AddValue("standard", "Wi-Fi standard: 80211a or 80211ax", standard);
    cmd.AddValue("heMcs", "HE MCS index (0-11) when standard=80211ax", heMcs);
    cmd.AddValue("numStas", "Number of STAs in --singleAcSaturation mode (default 1)", numStas);
    cmd.AddValue("wmmMode",
                 "WMM mode: off (QoS off, qdisc only); hybrid (QoS on, qdisc above + EDCA below); "
                 "qdisc-only (QoS on for infrastructure, but SelectQueueCallback forces all "
                 "packets to AC_BE so EDCA does not differentiate); edca-only (QoS on, qdisc "
                 "collapsed to single queue so only EDCA differentiates)",
                 wmmMode);
    cmd.AddValue("lowLoad",
                 "Use ~1.2x link rate offered load (vs default ~25x). Surfaces EDCA effects "
                 "by keeping qdisc backlog brief. Total offered ~7 Mb/s.",
                 lowLoad);
    cmd.Parse(argc, argv);

    if (wmmMode != "off" && wmmMode != "hybrid" && wmmMode != "qdisc-only" &&
        wmmMode != "edca-only")
    {
        NS_FATAL_ERROR("Unknown wmmMode '" << wmmMode << "'. Use off|hybrid|qdisc-only|edca-only.");
    }

    const double schedulerLinkBps = phyRateMbps * 1e6 * airtimeFraction;

    // ---- Nodes ----
    NodeContainer server;
    server.Create(1);
    NodeContainer ap;
    ap.Create(1);
    NodeContainer stas;
    stas.Create(singleAcSaturation ? numStas : kNumQueues);

    // ---- Backhaul P2P (server <-> AP), generous so the bottleneck is the AP ----
    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    p2p.SetChannelAttribute("Delay", StringValue("1ms"));
    NetDeviceContainer p2pDevs = p2p.Install(server.Get(0), ap.Get(0));

    // ---- Wi-Fi 802.11ax 5GHz ----
    YansWifiChannelHelper channelHelper = YansWifiChannelHelper::Default();
    YansWifiPhyHelper phy;
    phy.SetChannel(channelHelper.Create());

    WifiHelper wifi;
    if (standard == "80211ax")
    {
        wifi.SetStandard(WIFI_STANDARD_80211ax);
        const std::string heDataMode = "HeMcs" + std::to_string(heMcs);
        const std::string heCtlMode = "HeMcs0";
        wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                     "DataMode",
                                     StringValue(heDataMode),
                                     "ControlMode",
                                     StringValue(heCtlMode));
        // Pin to 20 MHz on 5 GHz to match Magrin et al. WNS3 2021 setup.
        // Without this, ns-3's default channel can be wider and the link
        // is no longer the bottleneck at 100 Mb/s app-layer offered load.
        phy.Set("ChannelSettings", StringValue("{0, 20, BAND_5GHZ, 0}"));
    }
    else
    {
        wifi.SetStandard(WIFI_STANDARD_80211a);
        // ConstantRateWifiManager at OfdmRate6Mbps gives ~5 Mb/s usable
        // downlink — well below the aggregate offered load (~128 Mb/s) so
        // the AP qdisc is the bottleneck and the scheduler choice is
        // observable.
        wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                     "DataMode",
                                     StringValue("OfdmRate6Mbps"),
                                     "ControlMode",
                                     StringValue("OfdmRate6Mbps"));
    }

    WifiMacHelper mac;
    Ssid ssid("ds4-wifi-cmp");

    // QoS support follows wmmMode. The singleAcSaturation branch ignores
    // wmmMode (it has its own purpose, runs QoS off as before).
    //   off          -> QoS off; qdisc is the sole differentiator (default).
    //   hybrid       -> QoS on; qdisc above + EDCA below both differentiate.
    //   qdisc-only   -> QoS on (infrastructure available) but SelectQueueCallback
    //                   forces all packets to AC_BE; only the qdisc differentiates.
    //   edca-only    -> QoS on; the inner qdisc is collapsed to a single queue
    //                   (NumQueues=1, all DSCPs map to queue 0); only EDCA at L2
    //                   differentiates. Useful as a pure-WMM baseline column.
    const bool qosSupported = (wmmMode != "off") && !singleAcSaturation;
    const bool edcaOnly = (wmmMode == "edca-only") && !singleAcSaturation;
    const bool forceAcBe = (wmmMode == "qdisc-only") && !singleAcSaturation;

    if (forceAcBe)
    {
        // SelectQueueCallback overrides ns-3's default DSCP-to-AC mapping.
        // We return AC_BE (=0 in qos-utils.h) for every packet so all four
        // classes share AC_BE's EDCA parameters at L2 and EDCA does not
        // differentiate. The qdisc above is the sole differentiator.
        wifi.SetSelectQueueCallback([](Ptr<QueueItem> /*item*/) -> std::size_t { return AC_BE; });
    }

    mac.SetType("ns3::ApWifiMac",
                "Ssid",
                SsidValue(ssid),
                "QosSupported",
                BooleanValue(qosSupported));
    NetDeviceContainer apDev = wifi.Install(phy, mac, ap.Get(0));

    mac.SetType("ns3::StaWifiMac",
                "Ssid",
                SsidValue(ssid),
                "ActiveProbing",
                BooleanValue(false),
                "QosSupported",
                BooleanValue(qosSupported));
    NetDeviceContainer staDevs = wifi.Install(phy, mac, stas);

    // ---- Mobility ----
    MobilityHelper mobility;
    Ptr<ListPositionAllocator> pos = CreateObject<ListPositionAllocator>();
    pos->Add(Vector(0.0, 0.0, 0.0));
    for (uint32_t i = 0; i < stas.GetN(); ++i)
    {
        pos->Add(Vector(2.0, static_cast<double>(i) * 1.5, 0.0));
    }
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

    // ---- Single-AC saturation mode (Bianchi sanity check; Magrin
    //      WNS3 2021 calibration when --standard=80211ax + --numStas=N) ----
    if (singleAcSaturation)
    {
        const uint16_t basePortSat = 9;
        // 100 Mb/s per-flow app-layer offered + 1000 B payload match Magrin
        // et al. WNS3 2021 Table 2 / Table 3 exactly. For 802.11a
        // OfdmRate6Mbps this is far above link capacity; for 802.11ax
        // HE-MCS the link is fast enough that 100 Mb/s per flow is a
        // meaningful saturation target.
        const double offeredPerFlow = 100e6;

        // DL sinks (on each STA) + UL sinks (on the server). Magrin et al.
        // Figure 3 / §6 specify bidirectional UDP traffic and report
        // aggregate throughput Su + Sd (UL + DL). STA UL is what creates
        // the channel contention that drops aggregate throughput at small
        // CWmin as N grows.
        ApplicationContainer dlSinks;
        ApplicationContainer ulSinks;
        for (uint32_t i = 0; i < numStas; ++i)
        {
            PacketSinkHelper dlSinkH(
                "ns3::UdpSocketFactory",
                InetSocketAddress(Ipv4Address::GetAny(), static_cast<uint16_t>(basePortSat + i)));
            dlSinks.Add(dlSinkH.Install(stas.Get(i)));
        }
        const uint16_t baseUlPort = static_cast<uint16_t>(basePortSat + numStas);
        for (uint32_t i = 0; i < numStas; ++i)
        {
            PacketSinkHelper ulSinkH(
                "ns3::UdpSocketFactory",
                InetSocketAddress(Ipv4Address::GetAny(), static_cast<uint16_t>(baseUlPort + i)));
            ulSinks.Add(ulSinkH.Install(server.Get(0)));
        }
        dlSinks.Start(Seconds(0.0));
        ulSinks.Start(Seconds(0.0));

        const Ipv4Address serverAddr = p2pIf.GetAddress(0);
        for (uint32_t i = 0; i < numStas; ++i)
        {
            // DL: server -> STA i
            OnOffHelper dlSrc(
                "ns3::UdpSocketFactory",
                InetSocketAddress(staIf.GetAddress(i), static_cast<uint16_t>(basePortSat + i)));
            dlSrc.SetAttribute("DataRate", DataRateValue(DataRate(offeredPerFlow)));
            dlSrc.SetAttribute("PacketSize", UintegerValue(1000));
            dlSrc.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
            dlSrc.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
            ApplicationContainer dlApp = dlSrc.Install(server.Get(0));
            dlApp.Start(Seconds(1.0));
            dlApp.Stop(Seconds(simTime));

            // UL: STA i -> server
            OnOffHelper ulSrc("ns3::UdpSocketFactory",
                              InetSocketAddress(serverAddr, static_cast<uint16_t>(baseUlPort + i)));
            ulSrc.SetAttribute("DataRate", DataRateValue(DataRate(offeredPerFlow)));
            ulSrc.SetAttribute("PacketSize", UintegerValue(1000));
            ulSrc.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
            ulSrc.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
            ApplicationContainer ulApp = ulSrc.Install(stas.Get(i));
            ulApp.Start(Seconds(1.0));
            ulApp.Stop(Seconds(simTime));
        }

        Simulator::Stop(Seconds(simTime + 0.5));
        Simulator::Run();

        uint64_t dlBytes = 0;
        uint64_t ulBytes = 0;
        for (uint32_t i = 0; i < numStas; ++i)
        {
            if (Ptr<PacketSink> s = dlSinks.Get(i)->GetObject<PacketSink>())
            {
                dlBytes += s->GetTotalRx();
            }
            if (Ptr<PacketSink> s = ulSinks.Get(i)->GetObject<PacketSink>())
            {
                ulBytes += s->GetTotalRx();
            }
        }
        const double durSec = simTime - 1.0;
        const double dlMbps = static_cast<double>(dlBytes) * 8.0 / durSec / 1e6;
        const double ulMbps = static_cast<double>(ulBytes) * 8.0 / durSec / 1e6;
        const double aggMbps = dlMbps + ulMbps;
        std::cout << "single_ac_saturation,standard=" << standard << ",heMcs=" << heMcs
                  << ",numStas=" << numStas << ",phy_mbps=" << phyRateMbps
                  << ",dl_mbps=" << std::fixed << std::setprecision(2) << dlMbps
                  << ",ul_mbps=" << ulMbps << ",aggregate_mbps=" << aggMbps << std::endl;

        // ---- CSV write: saturation.csv (bookkeeping only; no plot-recipe
        //      consumer). Same per-arm dir as scheduler-summary.csv but a
        //      distinct filename so plot-recipe --validate ignores it.
        {
            const std::string armDir = "output/ns3/wifi-sched/" + scheduler + "/";
            std::error_code ec;
            std::filesystem::create_directories(armDir, ec);
            if (ec)
            {
                NS_ABORT_MSG("create_directories(" << armDir << ") failed: " << ec.message());
            }
            std::ofstream csv(armDir + "saturation.csv");
            csv << "single_ac_saturation " << standard << " " << heMcs << " " << numStas << " "
                << std::fixed << std::setprecision(2) << phyRateMbps << " " << dlMbps << " "
                << ulMbps << " " << aggMbps << "\n";
        }

        Simulator::Destroy();
        return 0;
    }

    // ---- DiffServ edge on AP downlink ----
    Ptr<NetDevice> apWifi = apDev.Get(0);
    TrafficControlHelper tchUninstall;
    tchUninstall.Uninstall(apWifi);

    Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
    diffserv::Helper helper;
    auto inner = helper.InstallRedInner(edge);

    // edca-only collapses the inner qdisc to a single shared queue (NumQueues=1)
    // so the qdisc does not differentiate classes; only L2 EDCA does. The mark
    // rules below still classify by destination IP and stamp the right DSCP, so
    // ns-3's WMM mapper at L2 routes packets to the correct AC.
    const uint32_t innerQueues = edcaOnly ? 1 : kNumQueues;
    inner->SetNumQueues(innerQueues);
    for (uint32_t q = 0; q < innerQueues; ++q)
    {
        inner->SetNumPrec(q, 1);
        inner->SetQueueLimit(q, edcaOnly ? 400 : 100);
    }

    if (!edcaOnly)
    {
        Ptr<stratum::Scheduler> sched = MakeScheduler(scheduler, schedulerLinkBps, l2OverheadBytes);
        inner->SetScheduler(sched);
    }
    else
    {
        // Single-queue inner: an RR scheduler with one queue is a passthrough
        // FIFO. We still pass linkBps + l2Overhead so the rate accounting is
        // consistent with the other modes.
        Ptr<stratum::Scheduler> sched =
            CreateObjectWithAttributes<RoundRobinScheduler>("NumQueues", UintegerValue(1));
        sched->SetLinkBandwidth(schedulerLinkBps);
        sched->SetL2OverheadBytes(l2OverheadBytes);
        inner->SetScheduler(sched);
    }

    // Mark rules: stamp DSCP per destination STA
    constexpr uint8_t kDscpEf = 46;
    constexpr uint8_t kDscpAf41 = 34;
    constexpr uint8_t kDscpBe = 0;
    constexpr uint8_t kDscpCs1 = 8; // BK
    const uint8_t dscps[kNumQueues] = {kDscpEf, kDscpAf41, kDscpBe, kDscpCs1};
    for (uint32_t i = 0; i < kNumQueues; ++i)
    {
        Ipv4Address d = staIf.GetAddress(i);
        helper
            .AddMarkRule(edge, dscps[i], kAnyHost, static_cast<int32_t>(d.Get()), kAnyProtocol, 0);
    }

    helper.AddDumbPolicy(edge, kDscpEf);
    helper.AddDumbPolicy(edge, kDscpAf41);
    helper.AddDumbPolicy(edge, kDscpBe);
    helper.AddDumbPolicy(edge, kDscpCs1);
    helper.AddPolicerEntry(edge, PolicerType::DUMB, kDscpEf, kDscpEf, kDscpEf);
    helper.AddPolicerEntry(edge, PolicerType::DUMB, kDscpAf41, kDscpAf41, kDscpAf41);
    helper.AddPolicerEntry(edge, PolicerType::DUMB, kDscpBe, kDscpBe, kDscpBe);
    helper.AddPolicerEntry(edge, PolicerType::DUMB, kDscpCs1, kDscpCs1, kDscpCs1);
    if (Ptr<Meter> dm = edge->GetMeter(MeterType::DUMB))
    {
        dm->SetL2OverheadBytes(l2OverheadBytes);
    }

    if (edcaOnly)
    {
        // All four DSCPs map to the single inner queue (index 0). DSCP itself
        // survives marking and is read by ns-3's WMM mapper at L2.
        helper.AddPhbEntry(inner, kDscpEf, 0, 0);
        helper.AddPhbEntry(inner, kDscpAf41, 0, 0);
        helper.AddPhbEntry(inner, kDscpBe, 0, 0);
        helper.AddPhbEntry(inner, kDscpCs1, 0, 0);
    }
    else
    {
        helper.AddPhbEntry(inner, kDscpEf, 0, 0);
        helper.AddPhbEntry(inner, kDscpAf41, 1, 0);
        helper.AddPhbEntry(inner, kDscpBe, 2, 0);
        helper.AddPhbEntry(inner, kDscpCs1, 3, 0);
    }

    Ptr<TrafficControlLayer> tc = ap.Get(0)->GetObject<TrafficControlLayer>();
    tc->SetRootQueueDiscOnDevice(apWifi, edge);
    edge->Initialize();
    inner->SetMredMode(MredMode::DROP_TAIL);
    for (uint32_t q = 0; q < innerQueues; ++q)
    {
        helper.ConfigQueue(inner, q, 0, 100.0, 100.0, 1.0);
    }

    // ---- Traffic per class ----
    // Default offered load is ~128 Mb/s aggregate into a 6 Mb/s link
    // (~25x over-saturation): the AP qdisc is permanently backlogged and
    // qdisc-level decisions dominate p99. With --lowLoad the rates drop to
    // ~7 Mb/s aggregate (~1.2x link rate): qdisc only briefly queues and
    // L2 EDCA's short-timescale ordering becomes visible in p99.
    //
    // High load (default): EF 300 kbps, AF 8 Mbps, BE/BK 60 Mbps each.
    // Low load:           EF 300 kbps, AF 800 kbps, BE/BK 3 Mbps each.
    const uint16_t basePort = 9;
    const std::map<uint32_t, std::pair<double, uint32_t>> rateMap =
        lowLoad
            ? std::map<uint32_t, std::pair<double, uint32_t>>{
                  {0, {300e3, 160}},
                  {1, {800e3, 1200}},
                  {2, {3e6, 1400}},
                  {3, {3e6, 1400}},
              }
            : std::map<uint32_t, std::pair<double, uint32_t>>{
                  {0, {300e3, 160}},
                  {1, {8e6, 1200}},
                  {2, {60e6, 1400}},
                  {3, {60e6, 1400}},
              };

    ApplicationContainer sinkApps;
    for (uint32_t i = 0; i < kNumQueues; ++i)
    {
        PacketSinkHelper sh(
            "ns3::UdpSocketFactory",
            InetSocketAddress(Ipv4Address::GetAny(), static_cast<uint16_t>(basePort + i)));
        sinkApps.Add(sh.Install(stas.Get(i)));
    }
    sinkApps.Start(Seconds(0.0));

    for (uint32_t i = 0; i < kNumQueues; ++i)
    {
        const auto& [rate, size] = rateMap.at(i);
        OnOffHelper src(
            "ns3::UdpSocketFactory",
            InetSocketAddress(staIf.GetAddress(i), static_cast<uint16_t>(basePort + i)));
        src.SetAttribute("DataRate", DataRateValue(DataRate(rate)));
        src.SetAttribute("PacketSize", UintegerValue(size));
        src.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
        src.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
        ApplicationContainer app = src.Install(server.Get(0));
        app.Start(Seconds(1.0 + 0.05 * i));
        app.Stop(Seconds(simTime));
    }

    // ---- FlowMonitor ----
    FlowMonitorHelper fmHelper;
    Ptr<FlowMonitor> monitor = fmHelper.InstallAll();
    monitor->SetAttribute("DelayBinWidth", DoubleValue(0.0005)); // 0.5 ms bins

    Simulator::Stop(Seconds(simTime + 0.5));
    Simulator::Run();

    // ---- Per-class stats from FlowMonitor ----
    monitor->CheckForLostPackets();
    Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier>(fmHelper.GetClassifier());
    auto stats = monitor->GetFlowStats();

    std::map<uint32_t, ClassStats> perClass;
    for (uint32_t i = 0; i < kNumQueues; ++i)
    {
        perClass[i] = {};
    }
    const std::map<uint16_t, uint32_t> portToClass = {
        {basePort + 0, 0},
        {basePort + 1, 1},
        {basePort + 2, 2},
        {basePort + 3, 3},
    };

    for (const auto& [flowId, fs] : stats)
    {
        Ipv4FlowClassifier::FiveTuple ft = classifier->FindFlow(flowId);
        auto it = portToClass.find(ft.destinationPort);
        if (it == portToClass.end())
        {
            continue;
        }
        const uint32_t cls = it->second;
        const double rxBits = static_cast<double>(fs.rxBytes) * 8.0;
        const double durSec = simTime - 1.0;
        perClass[cls].rxKbps += rxBits / durSec / 1e3;
        perClass[cls].p99Ms = std::max(perClass[cls].p99Ms, HistogramP99Ms(fs.delayHistogram));
    }

    std::cout << scheduler << "," << std::fixed << std::setprecision(1) << perClass[0].rxKbps << ","
              << perClass[0].p99Ms << "," << perClass[1].rxKbps << "," << perClass[1].p99Ms << ","
              << perClass[2].rxKbps << "," << perClass[2].p99Ms << "," << perClass[3].rxKbps << ","
              << perClass[3].p99Ms << std::endl;

    // ---- CSV write: scheduler-summary.csv (RFC 4180 with header and class names).
    //      All three columns are real measurements from perClass[].
    {
        const std::string armDir = "output/ns3/wifi-sched/" + scheduler + "/";
        std::error_code ec;
        std::filesystem::create_directories(armDir, ec);
        if (ec)
        {
            NS_ABORT_MSG("create_directories(" << armDir << ") failed: " << ec.message());
        }
        static constexpr const char* kClassNames[4] = {"EF", "AF", "BE", "BK"};
        std::ofstream csv(armDir + "scheduler-summary.csv");
        csv << "flow_class,rx_kbps,p99_ms\n";
        csv << std::fixed << std::setprecision(1);
        for (int cls = 0; cls < 4; ++cls)
        {
            csv << kClassNames[cls] << "," << perClass[cls].rxKbps << "," << perClass[cls].p99Ms
                << "\n";
        }
    }

    Simulator::Destroy();
    return 0;
}
