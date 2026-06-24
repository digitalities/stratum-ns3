/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * RFC-7928-aligned multi-AQM characterisation harness.  Sweeps the
 * full vanilla-AQM matrix and the DiffServ-aware composites across
 * nine congestion-level scenarios.  Demonstrates that the Stratum
 * substrate composes equivalently with mainline AQMs and our
 * DiffServ-aware queue discs through the same single binary.
 *
 * Conceptual lineage: Deepak, Shravya, Tahiliani, "Design and
 * Implementation of AQM Evaluation Suite for ns-3", WNS3 2017
 * (DOI 10.1145/3067665.3067674).  We re-implement the suite's
 * scenario shape from RFC 7928 directly; the ellipse-plot
 * visualisation is inherited from TCP Ex Machina (Winstein &
 * Balakrishnan, SIGCOMM 2013, DOI 10.1145/2486001.2486020).  No
 * source copied.
 *
 *   Topology: standard RFC-7928 dumbbell.
 *
 *     sender(s) -- 100 Mbps / 1 ms -- edge -- TOTAL_RATE / 5 ms -- sink
 *                                       |
 *                                     bottleneck egress queue disc
 *
 *   Scenarios (--scenario=):
 *     steady   RFC 7928 Test 1: 4 saturating UDP + 1 sparse UDP probe
 *     mixed    RFC 7928 Test 5: 2 bulk UDP + 2 short bursts + 1 CBR
 *     rt-bulk  RFC 7928 Test 7: 1 real-time CBR + 4 saturating UDP
 *     mild-congestion   RFC 7928 Section 8.2.2: 2 bulk + 1 probe
 *     medium-congestion RFC 7928 Section 8.2.3: 4 bulk + 1 probe
 *     heavy-congestion  RFC 7928 Section 8.2.4: 8 bulk + 1 probe
 *
 *   AQMs (--aqm=):
 *     12 cells: 9 mainline (PfifoFast, Red, AdaptiveRed, CoDel,
 *     FqCoDel, Pie, FqPie, Cobalt, FqCobalt) + 3 Stratum
 *     (StratumRed, StratumL4s, StratumCake-diffserv4).
 *
 *   Output (per cell):
 *     <outDir>/<scenario>-<aqm>-perflow.csv   per-flow stats
 *     <outDir>/<scenario>-<aqm>-summary.txt   aggregate stats
 *
 * @see paper/related-papers/aqm-eval-suite.pdf (Deepak et al. 2017)
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/flow-monitor-helper.h"
#include "ns3/internet-module.h"
#include "ns3/ipv4-flow-classifier.h"
#include "ns3/ipv4-header.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/stratum-aqm-registry.h"
#include "ns3/stratum-edge-queue-disc.h"
#include "ns3/stratum-helper.h"
#include "ns3/stratum-install-helper.h"
#include "ns3/stratum-scheduler-registry.h"
#include "ns3/tcp-header.h"
#include "ns3/tcp-retransmit-tag.h"
#include "ns3/traffic-control-module.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

using namespace ns3;
namespace aqm_eval = ns3::stratum::aqm_eval;
namespace diffserv = ns3::stratum::diffserv;
using ns3::stratum::EdgeQueueDisc;
using ns3::stratum::SchedulerRegistry;
using ns3::stratum::SerialiseSchedulerEntry;

NS_LOG_COMPONENT_DEFINE("AqmEvalRunner");

namespace
{

// ----- Scenario taxonomy --------------------------------------------------

enum class Scenario
{
    Steady,
    Mixed,
    RtBulk,
    MildCongestion,
    MediumCongestion,
    HeavyCongestion,
    TcpFriendly,    // RFC 7928 §5.1.1: 2 long-lived TCP NewReno
    TcpAggressive,  // RFC 7928 §5.2:   2 TCP NewReno + 1 HighSpeed-TCP
    TcpUnresponsive // RFC 7928 §5.3.1: 1 TCP NewReno + 1 UDP CBR (unresponsive)
};

const std::map<std::string, Scenario>&
ScenarioLut()
{
    static const std::map<std::string, Scenario> kLut = {
        {"steady", Scenario::Steady},
        {"mixed", Scenario::Mixed},
        {"rt-bulk", Scenario::RtBulk},
        {"mild-congestion", Scenario::MildCongestion},
        {"medium-congestion", Scenario::MediumCongestion},
        {"heavy-congestion", Scenario::HeavyCongestion},
        {"tcp-friendly", Scenario::TcpFriendly},
        {"tcp-aggressive", Scenario::TcpAggressive},
        {"tcp-unresponsive", Scenario::TcpUnresponsive},
    };
    return kLut;
}

Scenario
ParseScenario(const std::string& s)
{
    auto it = ScenarioLut().find(s);
    NS_ABORT_MSG_IF(it == ScenarioLut().end(), "unknown --scenario=" << s);
    return it->second;
}

const char*
ScenarioName(Scenario s)
{
    switch (s)
    {
    case Scenario::Steady:
        return "steady";
    case Scenario::Mixed:
        return "mixed";
    case Scenario::RtBulk:
        return "rt-bulk";
    case Scenario::MildCongestion:
        return "mild-congestion";
    case Scenario::MediumCongestion:
        return "medium-congestion";
    case Scenario::HeavyCongestion:
        return "heavy-congestion";
    case Scenario::TcpFriendly:
        return "tcp-friendly";
    case Scenario::TcpAggressive:
        return "tcp-aggressive";
    case Scenario::TcpUnresponsive:
        return "tcp-unresponsive";
    }
    return "?";
}

enum class FlowProto
{
    Udp,
    TcpNewReno,
    TcpHighSpeed,
};

struct FlowSpec
{
    std::string name;
    uint64_t rateBps; // for UDP CBR; for TCP this is BulkSend offered cap (use big value)
    double startSec;
    double stopSec;
    uint8_t tos; // ToS byte; DSCP = tos >> 2.  46 << 2 = EF.
    FlowProto proto = FlowProto::Udp;
};

std::vector<FlowSpec>
BuildFlowPlan(Scenario s, double simTime)
{
    std::vector<FlowSpec> flows;
    switch (s)
    {
    case Scenario::Steady:
        for (int i = 0; i < 4; ++i)
        {
            flows.push_back({"bulk", 3'000'000, 1.0, simTime, 0});
        }
        flows.push_back({"probe", 200'000, 1.0, simTime, 46u << 2});
        break;
    case Scenario::Mixed:
        flows.push_back({"bulk-long-1", 3'000'000, 1.0, simTime, 0});
        flows.push_back({"bulk-long-2", 3'000'000, 1.0, simTime, 0});
        flows.push_back({"burst-1", 3'000'000, 3.0, 5.0, 0});
        flows.push_back({"burst-2", 3'000'000, 6.0, 8.0, 0});
        flows.push_back({"cbr-audio", 96'000, 1.0, simTime, 46u << 2});
        break;
    case Scenario::RtBulk:
        flows.push_back({"rt-cbr", 500'000, 1.0, simTime, 46u << 2});
        for (int i = 0; i < 4; ++i)
        {
            flows.push_back({"bulk", 3'000'000, 1.0, simTime, 0});
        }
        break;
    case Scenario::MildCongestion:
        // Aggregate offered load ~70% of typical 10 Mbps bottleneck.
        for (int i = 0; i < 2; ++i)
        {
            flows.push_back({"bulk", 3'500'000, 1.0, simTime, 0});
        }
        flows.push_back({"probe", 200'000, 1.0, simTime, 46u << 2});
        break;
    case Scenario::MediumCongestion:
        // Aggregate offered load ~120% (saturating).
        for (int i = 0; i < 4; ++i)
        {
            flows.push_back({"bulk", 3'000'000, 1.0, simTime, 0});
        }
        flows.push_back({"probe", 200'000, 1.0, simTime, 46u << 2});
        break;
    case Scenario::HeavyCongestion:
        // Aggregate offered load ~200% (heavily oversubscribed).
        for (int i = 0; i < 8; ++i)
        {
            flows.push_back({"bulk", 2'500'000, 1.0, simTime, 0});
        }
        flows.push_back({"probe", 200'000, 1.0, simTime, 46u << 2});
        break;
    case Scenario::TcpFriendly:
        // RFC 7928 §5.1.1: 2 long-lived TCP NewReno flows, identical IW.
        flows.push_back({"tcp-1", 100'000'000, 1.0, simTime, 0, FlowProto::TcpNewReno});
        flows.push_back({"tcp-2", 100'000'000, 1.0, simTime, 0, FlowProto::TcpNewReno});
        break;
    case Scenario::TcpAggressive:
        // RFC 7928 §5.2: 2 NewReno + 1 HighSpeed-TCP (CUBIC absent in pinned ns-3).
        flows.push_back({"tcp-newreno-1", 100'000'000, 1.0, simTime, 0, FlowProto::TcpNewReno});
        flows.push_back({"tcp-newreno-2", 100'000'000, 1.0, simTime, 0, FlowProto::TcpNewReno});
        flows.push_back({"tcp-highspeed", 100'000'000, 1.0, simTime, 0, FlowProto::TcpHighSpeed});
        break;
    case Scenario::TcpUnresponsive:
        // RFC 7928 §5.3.1: 1 long TCP NewReno + 1 unresponsive UDP CBR.
        flows.push_back({"tcp", 100'000'000, 1.0, simTime, 0, FlowProto::TcpNewReno});
        // 9 Mbps UDP-CBR on a 10 Mbps link saturates the bottleneck and
        // forces TCP to back off under FIFO/single-queue AQMs.  Earlier 5
        // Mbps left enough headroom that TCP was never starved, so the
        // scenario was not actually testing unresponsive isolation.
        flows.push_back({"udp-cbr", 9'000'000, 1.0, simTime, 0, FlowProto::Udp});
        break;
    }
    return flows;
}

// ----- AQM dispatch via centralised registry -----------------------------
//
// All AQM construction is delegated to AqmRegistry (stratum-aqm-registry.{h,cc}).
// The registry holds dispatch name, file-tag, family label, ECN-support
// flag and a per-entry factory closure in one table.  Adding a new cell
// = adding one entry there; the runner needs no further edit.

std::string
JoinAqmNames()
{
    std::ostringstream oss;
    bool first = true;
    for (const auto& n : aqm_eval::AqmRegistry::Get().Names())
    {
        if (!first)
        {
            oss << ", ";
        }
        oss << n;
        first = false;
    }
    return oss.str();
}

void
PrintAqmCatalogue(std::ostream& os)
{
    os << "Registered AQMs (" << aqm_eval::AqmRegistry::Get().All().size() << "):\n";
    for (const auto& e : aqm_eval::AqmRegistry::Get().All())
    {
        os << "  " << std::left << std::setw(34) << e.name << "  family=" << std::setw(7)
           << aqm_eval::FamilyName(e.family) << "  ecn=" << (e.supportsEcn ? "yes" : "no ") << "  "
           << e.displayName << "\n";
    }
}

void
PrintScenarioCatalogue(std::ostream& os)
{
    os << "Registered scenarios (" << ScenarioLut().size() << "):\n";
    for (const auto& kv : ScenarioLut())
    {
        os << "  " << kv.first << "\n";
    }
}

std::string
JoinScenarioNames()
{
    std::ostringstream oss;
    bool first = true;
    for (const auto& kv : ScenarioLut())
    {
        if (!first)
        {
            oss << ", ";
        }
        oss << kv.first;
        first = false;
    }
    return oss.str();
}

// ----- Output-path management --------------------------------------------
//
// Owns the output directory's lifecycle and emits the per-cell file
// paths.  Replaces inline mkdir + string concat throughout main().

struct OutputPaths
{
    std::string root;
    std::string base; // root + "/" + scenario + "-" + aqmTag

    OutputPaths(const std::string& outDir,
                const std::string& scenarioName,
                const std::string& aqmFileTag)
        : root(outDir),
          base(outDir + "/" + scenarioName + "-" + aqmFileTag)
    {
        if (mkdir(root.c_str(), 0755) != 0 && errno != EEXIST)
        {
            NS_ABORT_MSG("mkdir(" << root << ") failed: " << std::strerror(errno));
        }
    }

    std::string PerflowCsv() const
    {
        return base + "-perflow.csv";
    }

    std::string SummaryTxt() const
    {
        return base + "-summary.txt";
    }
};

// ----- DSCP wiring (only for Stratum composites) -------------------------

void
WireDsCakeDscp(Ptr<EdgeQueueDisc> edge,
               const std::vector<FlowSpec>& flows,
               const std::vector<Ipv4InterfaceContainer>& senderIfs)
{
    diffserv::Helper helper;
    for (uint32_t i = 0; i < flows.size(); ++i)
    {
        const uint8_t dscp = flows[i].tos >> 2;
        edge->AddMarkRule({.dscp = dscp, .srcAddr = senderIfs[i].GetAddress(0)});
        helper.AddDumbPolicy(edge, dscp);
    }
}

// ----- Source/sink application lifecycle ----------------------------------
//
// Owns the per-flow source and sink applications, the per-flow sink and
// retransmission byte counters, and the receiver-side IP-layer trace hook
// that feeds them.  Replaces inline app construction in main(), mirroring
// OutputPaths: one struct owns one lifecycle concern end-to-end.

struct EvalApp
{
    ApplicationContainer sinkApps;
    ApplicationContainer sourceApps;
    std::vector<uint64_t> sinkBytesPerFlow;
    std::vector<uint64_t> retxBytesPerFlow;

    EvalApp(const std::vector<FlowSpec>& flows,
            const NodeContainer& senders,
            Ptr<Node> sinkNode,
            const Ipv4InterfaceContainer& sinkIfs,
            uint16_t basePort,
            double simTime)
        : sinkBytesPerFlow(flows.size(), 0),
          retxBytesPerFlow(flows.size(), 0)
    {
        // Hook the receiver's IP-layer LocalDeliver trace to count retransmitted
        // bytes per flow.  TcpRetransmitTag rides at TCP layer (stamped in
        // SendDataPacket, stripped in DoForwardUp), so it is visible here —
        // BEFORE the strip — for every retransmitted segment.  This is the
        // demonstration of MR !2830 in operation: per-flow retx accounting
        // works for ANY queue disc (vanilla or DiffServ-aware) because the tag
        // travels with the packet through the entire path.
        Ptr<Ipv4L3Protocol> sinkIpv4 = sinkNode->GetObject<Ipv4L3Protocol>();
        sinkIpv4->TraceConnectWithoutContext(
            "LocalDeliver",
            MakeBoundCallback(
                +[](std::vector<uint64_t>* retxBytes,
                    uint16_t basePort,
                    uint32_t numFlows,
                    const Ipv4Header& iph,
                    Ptr<const Packet> p,
                    uint32_t /*ifIdx*/) {
                    if (iph.GetProtocol() != 6 /* TCP */)
                    {
                        return; // UDP CBR doesn't carry TcpRetransmitTag
                    }
                    TcpRetransmitTag retxTag;
                    if (!p->PeekPacketTag(retxTag))
                    {
                        return; // not a retransmission
                    }
                    // Peek the TCP header to extract the destination port and
                    // map back to the flow index.  The ns-3 LocalDeliver trace
                    // delivers the L4 PDU (TCP segment), so the first 20 bytes
                    // are the TCP header.
                    TcpHeader tcph;
                    Ptr<Packet> copy = p->Copy();
                    copy->PeekHeader(tcph);
                    const uint16_t dport = tcph.GetDestinationPort();
                    if (dport < basePort || dport >= basePort + numFlows)
                    {
                        return;
                    }
                    const uint32_t flowIdx = dport - basePort;
                    // Count IP-layer payload bytes (= TCP segment size).
                    (*retxBytes)[flowIdx] += p->GetSize();
                },
                &retxBytesPerFlow,
                basePort,
                static_cast<uint32_t>(flows.size())));

        for (uint32_t i = 0; i < flows.size(); ++i)
        {
            const uint16_t port = basePort + i;
            const bool isTcp = (flows[i].proto != FlowProto::Udp);
            const std::string sockFactory =
                isTcp ? "ns3::TcpSocketFactory" : "ns3::UdpSocketFactory";

            PacketSinkHelper sinkHelper(sockFactory,
                                        InetSocketAddress(Ipv4Address::GetAny(), port));
            ApplicationContainer thisSink = sinkHelper.Install(sinkNode);
            Ptr<PacketSink> ps = DynamicCast<PacketSink>(thisSink.Get(0));
            ps->TraceConnectWithoutContext(
                "Rx",
                MakeBoundCallback(
                    +[](uint64_t* counter, Ptr<const Packet> pkt, const Address&) {
                        *counter += pkt->GetSize();
                    },
                    &sinkBytesPerFlow[i]));
            sinkApps.Add(thisSink);

            ApplicationContainer src;
            if (isTcp)
            {
                // Per-flow TCP socket type override.  Note: ns-3 ConfigPath of
                // TcpSocketBase::TypeId via Config::Set is global; per-flow
                // override requires creating the socket explicitly and
                // attaching it to a BulkSendApplication.
                const std::string tcpType = (flows[i].proto == FlowProto::TcpHighSpeed)
                                                ? "ns3::TcpHighSpeed"
                                                : "ns3::TcpNewReno";
                Config::Set("/NodeList/" + std::to_string(senders.Get(i)->GetId()) +
                                "/$ns3::TcpL4Protocol/SocketType",
                            TypeIdValue(TypeId::LookupByName(tcpType)));
                BulkSendHelper bulk(sockFactory, InetSocketAddress(sinkIfs.GetAddress(1), port));
                bulk.SetAttribute("MaxBytes",
                                  UintegerValue(0)); // unlimited; bounded by stop time
                bulk.SetAttribute("SendSize", UintegerValue(1448));
                src = bulk.Install(senders.Get(i));
            }
            else
            {
                OnOffHelper onOff(sockFactory, InetSocketAddress(sinkIfs.GetAddress(1), port));
                onOff.SetAttribute("OnTime",
                                   StringValue("ns3::ConstantRandomVariable[Constant=1]"));
                onOff.SetAttribute("OffTime",
                                   StringValue("ns3::ConstantRandomVariable[Constant=0]"));
                onOff.SetAttribute("DataRate", DataRateValue(DataRate(flows[i].rateBps)));
                const uint32_t pktSize = (flows[i].rateBps >= 1'000'000) ? 1000 : 200;
                onOff.SetAttribute("PacketSize", UintegerValue(pktSize));
                onOff.SetAttribute("Tos", UintegerValue(flows[i].tos));
                src = onOff.Install(senders.Get(i));
            }
            src.Start(Seconds(flows[i].startSec));
            src.Stop(Seconds(flows[i].stopSec));
            sourceApps.Add(src);
        }
        sinkApps.Start(Seconds(0.0));
        sinkApps.Stop(Seconds(simTime + 1.0));
    }
};

// ----- Per-flow accounting -----------------------------------------------

struct PerFlowResult
{
    std::string name;
    uint32_t flowIdx;     // index into the FlowSpec plan (dport - basePort)
    uint64_t fmRxBytes;   // FlowMonitor rxBytes (IP layer; includes retx)
    uint64_t fmRetxBytes; // tag-aware: bytes flagged with TcpRetransmitTag
    uint64_t sinkRxBytes; // PacketSink Rx (post-TCP; retx-free for TCP)
    uint64_t txPackets;   // FlowMonitor txPackets (sender IP layer)
    uint64_t lostPackets; // FlowMonitor lostPackets (after CheckForLostPackets)
    double rxRateBps;     // FlowMonitor-with-retx-subtracted rate (RFC-conformant goodput)
    double meanDelayMs;
};

} // namespace

int
main(int argc, char* argv[])
{
    std::string aqm = "ns3::FqCoDelQueueDisc";
    std::string scenarioStr = "steady";
    std::string outDir = "/tmp/aqm-eval-runner";
    std::string manifestPath;
    std::string schedulerManifestPath;
    std::string ecnMode = "default";
    double simTime = 4.0;
    uint64_t totalRateBps = 10'000'000;
    double rttCat1Ms = 0.0;
    double rttCat2Ms = 0.0;

    CommandLine cmd(__FILE__);
    cmd.AddValue("aqm", "Bottleneck queue disc (TypeId or StratumRed|StratumL4s|StratumCake)", aqm);
    cmd.AddValue(
        "scenario",
        "Scenario name: steady|mixed|rt-bulk|mild-congestion|medium-congestion|heavy-congestion",
        scenarioStr);
    cmd.AddValue("outDir", "Directory for CSV outputs", outDir);
    cmd.AddValue("simTime", "Simulation duration (seconds)", simTime);
    cmd.AddValue("totalRateBps", "Bottleneck bitrate (bps)", totalRateBps);
    cmd.AddValue("manifest",
                 "Write AQM registry as JSON to PATH and exit (no simulation runs)",
                 manifestPath);
    cmd.AddValue("scheduler-manifest",
                 "Write scheduler registry as JSON to PATH and exit (no simulation runs)",
                 schedulerManifestPath);
    cmd.AddValue("ecn",
                 "ECN override: on|off|default (default leaves the AQM's built-in setting)",
                 ecnMode);
    cmd.AddValue("rttCat1Ms",
                 "RTT-fairness mode (RFC 7928 Section 6): category-I flow RTT in ms "
                 "(0 disables the mode; set both flags; requires --scenario=tcp-friendly)",
                 rttCat1Ms);
    cmd.AddValue("rttCat2Ms",
                 "RTT-fairness mode (RFC 7928 Section 6): category-II flow RTT in ms "
                 "(0 disables the mode; set both flags; requires --scenario=tcp-friendly)",
                 rttCat2Ms);
    cmd.Parse(argc, argv);

    // RTT-fairness mode reshapes path delays so each of the two TCP flows
    // gets a prescribed end-to-end RTT (RFC 7928 Section 6.2: category I
    // fixed, category II swept across [5 ms, 560 ms]).  With the mode's
    // 1 ms bottleneck and 1 ms sink-link delays, the per-flow access delay
    // is RTT/2 - 2 ms, so the smallest expressible RTT is just above 4 ms.
    const bool rttFairnessMode = (rttCat1Ms != 0.0 || rttCat2Ms != 0.0);
    if (rttFairnessMode && (rttCat1Ms < 4.2 || rttCat2Ms < 4.2))
    {
        std::cerr << "[aqm-eval-runner] --rttCat1Ms and --rttCat2Ms must both be set and "
                     ">= 4.2 ms (got "
                  << rttCat1Ms << ", " << rttCat2Ms << ")\n";
        return 1;
    }
    if (rttFairnessMode)
    {
        // RFC 7928 Section 6.2 prescribes non-application-limited flows.
        // ns-3's default 128 KB socket buffers window-limit a flow to
        // ~1.9 Mbps at 560 ms RTT (BDP at 10 Mbps x 560 ms is ~700 KB),
        // which would measure the buffer, not the AQM.  4 MB clears the
        // largest swept bandwidth-delay product with margin.
        Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(4'194'304));
        Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(4'194'304));
    }

    if (ecnMode != "on" && ecnMode != "off" && ecnMode != "default")
    {
        std::cerr << "[aqm-eval-runner] --ecn must be 'on', 'off' or 'default' (got '" << ecnMode
                  << "')\n";
        return 1;
    }

    // ----- Manifest dump (Python plotters consume this) ------------------
    if (!manifestPath.empty())
    {
        std::ofstream js(manifestPath);
        if (!js)
        {
            std::cerr << "[aqm-eval-runner] cannot open --manifest=" << manifestPath << "\n";
            return 1;
        }
        const auto& entries = aqm_eval::AqmRegistry::Get().All();
        js << "{\n  \"aqms\": [\n";
        for (size_t i = 0; i < entries.size(); ++i)
        {
            js << "    ";
            aqm_eval::SerialiseAqmEntry(js, entries[i]);
            if (i + 1 < entries.size())
            {
                js << ",";
            }
            js << "\n";
        }
        js << "  ],\n  \"scenarios\": [\n";
        const auto& scen = ScenarioLut();
        size_t k = 0;
        for (const auto& kv : scen)
        {
            js << "    \"" << kv.first << "\"";
            if (++k < scen.size())
            {
                js << ",";
            }
            js << "\n";
        }
        js << "  ]\n}\n";
        std::cout << "[aqm-eval-runner] wrote " << manifestPath << "\n";
        return 0;
    }

    // ----- Scheduler-registry manifest dump --------------------------------
    if (!schedulerManifestPath.empty())
    {
        std::ofstream js(schedulerManifestPath);
        if (!js)
        {
            std::cerr << "[aqm-eval-runner] cannot open --scheduler-manifest="
                      << schedulerManifestPath << "\n";
            return 1;
        }
        SchedulerRegistry::Get().DumpManifest(js, "schedulers", &SerialiseSchedulerEntry);
        std::cout << "[aqm-eval-runner] wrote " << schedulerManifestPath << "\n";
        return 0;
    }

    // ----- Catalogue listings (--aqm=list / --scenario=list) ------------
    if (aqm == "list" || aqm == "help")
    {
        PrintAqmCatalogue(std::cout);
        return 0;
    }
    if (scenarioStr == "list" || scenarioStr == "help")
    {
        PrintScenarioCatalogue(std::cout);
        return 0;
    }

    // ----- Friendly validation against the registry ---------------------
    if (!aqm_eval::AqmRegistry::Get().FindByName(aqm))
    {
        std::cerr << "[aqm-eval-runner] unknown --aqm='" << aqm << "'\n"
                  << "  valid choices: " << JoinAqmNames() << "\n"
                  << "  (run with --aqm=list for full catalogue)\n";
        return 1;
    }
    if (ScenarioLut().find(scenarioStr) == ScenarioLut().end())
    {
        std::cerr << "[aqm-eval-runner] unknown --scenario='" << scenarioStr << "'\n"
                  << "  valid choices: " << JoinScenarioNames() << "\n"
                  << "  (run with --scenario=list for full catalogue)\n";
        return 1;
    }

    const Scenario scenario = ParseScenario(scenarioStr);
    if (rttFairnessMode && scenario != Scenario::TcpFriendly)
    {
        std::cerr << "[aqm-eval-runner] RTT-fairness mode requires --scenario=tcp-friendly "
                     "(two long-lived TCP NewReno flows, RFC 7928 Section 6.2)\n";
        return 1;
    }
    const aqm_eval::AqmEntry* aqmEntry = aqm_eval::AqmRegistry::Get().FindByName(aqm);
    OutputPaths paths(outDir, ScenarioName(scenario), aqmEntry->fileTag);

    const std::vector<FlowSpec> flows = BuildFlowPlan(scenario, simTime);

    // ----- Topology --------------------------------------------------------
    NodeContainer senders;
    senders.Create(flows.size());
    NodeContainer routers;
    routers.Create(2);
    NodeContainer sinks;
    sinks.Create(1);

    PointToPointHelper accessLink;
    accessLink.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    accessLink.SetChannelAttribute("Delay", StringValue("1ms"));
    PointToPointHelper bottleneck;
    bottleneck.SetDeviceAttribute("DataRate", DataRateValue(DataRate(totalRateBps)));
    bottleneck.SetChannelAttribute("Delay", StringValue(rttFairnessMode ? "1ms" : "5ms"));
    PointToPointHelper sinkLink;
    sinkLink.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    sinkLink.SetChannelAttribute("Delay", StringValue("1ms"));

    InternetStackHelper stack;
    stack.Install(senders);
    stack.Install(routers);
    stack.Install(sinks);

    Ipv4AddressHelper addr;
    std::vector<Ipv4InterfaceContainer> senderIfs(flows.size());
    for (uint32_t i = 0; i < flows.size(); ++i)
    {
        if (rttFairnessMode)
        {
            // Per-flow RTT = 2 * (access + bottleneck 1 ms + sink-link 1 ms).
            const double rttMs = (i == 0) ? rttCat1Ms : rttCat2Ms;
            std::ostringstream delay;
            delay << (rttMs / 2.0 - 2.0) << "ms";
            accessLink.SetChannelAttribute("Delay", StringValue(delay.str()));
        }
        NetDeviceContainer dev = accessLink.Install(senders.Get(i), routers.Get(0));
        std::ostringstream ssNet;
        ssNet << "10.1." << (i + 1) << ".0";
        addr.SetBase(ssNet.str().c_str(), "255.255.255.0");
        senderIfs[i] = addr.Assign(dev);
    }

    NetDeviceContainer bottleneckDev = bottleneck.Install(routers.Get(0), routers.Get(1));
    addr.SetBase("10.2.1.0", "255.255.255.0");
    addr.Assign(bottleneckDev);

    NetDeviceContainer sinkDev = sinkLink.Install(routers.Get(1), sinks.Get(0));
    addr.SetBase("10.3.1.0", "255.255.255.0");
    Ipv4InterfaceContainer sinkIfs = addr.Assign(sinkDev);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // ----- Bottleneck queue disc -------------------------------------------
    Ptr<QueueDisc> qdisc = aqm_eval::AqmRegistry::Get().Make(aqm, DataRate(totalRateBps));
    if (aqm == "StratumCake")
    {
        WireDsCakeDscp(DynamicCast<EdgeQueueDisc>(qdisc), flows, senderIfs);
    }

    // ECN override (best-effort).  Most mainline AQMs (RED, ARED, CoDel,
    // PIE, Cobalt, FQ-*) expose a `UseEcn` BooleanValue attribute; we
    // toggle it via SetAttributeFailSafe so qdiscs without that attribute
    // (PfifoFast, StratumRed, the Stratum composite wrappers) silently no-op
    // rather than abort.  Composites with internal ECN policy (StratumL4s,
    // StratumCake) keep their built-in behaviour: this flag is for vanilla A/B
    // characterisation, not for rewriting composite semantics.
    std::string ecnApplied = "default";
    if (ecnMode != "default")
    {
        const bool target = (ecnMode == "on");
        if (qdisc->SetAttributeFailSafe("UseEcn", BooleanValue(target)))
        {
            ecnApplied = ecnMode;
        }
        else if (!aqmEntry->supportsEcn)
        {
            std::cout << "[aqm-eval-runner] note: --ecn=" << ecnMode << " ignored; '" << aqm
                      << "' does not support ECN (registry).\n";
        }
        else
        {
            std::cout << "[aqm-eval-runner] note: --ecn=" << ecnMode
                      << " not directly settable on composite '" << aqm
                      << "'; using composite's built-in ECN behaviour.\n";
        }
    }

    Ptr<NetDevice> egress = bottleneckDev.Get(0);
    stratum::InstallRoot(egress, qdisc);

    // ----- Apps + side-channel sink-byte counter --------------------------
    const uint16_t basePort = 5000;
    EvalApp apps(flows, senders, sinks.Get(0), sinkIfs, basePort, simTime);

    FlowMonitorHelper fmh;
    Ptr<FlowMonitor> mon = fmh.InstallAll();

    Simulator::Stop(Seconds(simTime + 1.0));
    Simulator::Run();

    mon->CheckForLostPackets();
    auto classifier = DynamicCast<Ipv4FlowClassifier>(fmh.GetClassifier());

    std::vector<PerFlowResult> perFlow;
    // Goodput denominator = full source-active duration.  FlowMonitor counts
    // bytes from t=0; flows are active from t=0 to simTime.  Earlier code
    // used (simTime - 0.5) as a warm-up trim, but did not also trim bytes
    // from the numerator, so the rate was inflated by ~5%; fixed.
    const double measureSpan = simTime;
    for (const auto& kv : mon->GetFlowStats())
    {
        Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow(kv.first);
        const uint16_t dport = t.destinationPort;
        if (dport < basePort || dport >= basePort + flows.size())
        {
            continue;
        }
        const uint32_t flowIdx = dport - basePort;
        PerFlowResult r;
        r.name = flows[flowIdx].name;
        r.flowIdx = flowIdx;
        r.fmRxBytes = kv.second.rxBytes;
        r.fmRetxBytes = apps.retxBytesPerFlow[flowIdx];
        r.sinkRxBytes = apps.sinkBytesPerFlow[flowIdx];
        r.txPackets = kv.second.txPackets;
        r.lostPackets = kv.second.lostPackets;
        // Goodput per RFC 7928 §3.2: useful (= original, non-retransmitted)
        // bytes per second.  FlowMonitor counts IP-layer bytes (includes
        // retx); subtract retx to recover the goodput measurement.
        const uint64_t origBytes =
            (r.fmRxBytes >= r.fmRetxBytes) ? (r.fmRxBytes - r.fmRetxBytes) : r.fmRxBytes;
        r.rxRateBps = (origBytes * 8.0) / measureSpan;
        r.meanDelayMs = kv.second.rxPackets > 0
                            ? (kv.second.delaySum.GetMicroSeconds() / 1000.0) / kv.second.rxPackets
                            : 0.0;
        perFlow.push_back(r);
    }

    // ----- Aggregates ------------------------------------------------------
    double sumX = 0.0;
    double sumX2 = 0.0;
    uint64_t totalFmBytes = 0;
    uint64_t totalRetxBytes = 0;
    uint64_t totalSinkBytes = 0;
    for (const auto& r : perFlow)
    {
        sumX += r.rxRateBps;
        sumX2 += r.rxRateBps * r.rxRateBps;
        totalFmBytes += r.fmRxBytes;
        totalRetxBytes += r.fmRetxBytes;
        totalSinkBytes += r.sinkRxBytes;
    }
    const double jain = perFlow.empty() ? 0.0 : (sumX * sumX) / (perFlow.size() * sumX2);

    // Cross-plane gate: |FM-orig (= FM total minus retx) - sink_total| / sink_total.
    // Residual is the IP+TCP/UDP header overhead (constant per packet); the
    // gate ≤ ~3% confirms goodput-conformant accounting on both planes.
    const uint64_t fmOrigBytes =
        (totalFmBytes >= totalRetxBytes) ? (totalFmBytes - totalRetxBytes) : totalFmBytes;
    const double crossPlaneDelta =
        totalSinkBytes > 0
            ? std::abs(static_cast<double>(fmOrigBytes) - static_cast<double>(totalSinkBytes)) /
                  static_cast<double>(totalSinkBytes)
            : 0.0;

    // ----- CSV + summary ---------------------------------------------------
    const std::string scenStr = ScenarioName(scenario);

    {
        std::ofstream csv(paths.PerflowCsv());
        csv << "flow,fm_rx_bytes,fm_retx_bytes,sink_rx_bytes,rx_rate_bps,mean_delay_ms\n";
        for (const auto& r : perFlow)
        {
            csv << r.name << "," << r.fmRxBytes << "," << r.fmRetxBytes << "," << r.sinkRxBytes
                << "," << std::fixed << std::setprecision(0) << r.rxRateBps << "," << std::fixed
                << std::setprecision(3) << r.meanDelayMs << "\n";
        }
    }

    {
        std::ofstream sm(paths.SummaryTxt());
        sm << "scenario=" << scenStr << "\n"
           << "aqm=" << aqm << "\n"
           << "ecn_requested=" << ecnMode << "\n"
           << "ecn_applied=" << ecnApplied << "\n"
           << "totalRate=" << totalRateBps << "\n"
           << "simTime=" << simTime << "\n"
           << "numFlows=" << perFlow.size() << "\n"
           << "aggregate_Mbps=" << std::fixed << std::setprecision(3) << (sumX / 1.0e6) << "\n"
           << "jain_fairness=" << std::fixed << std::setprecision(4) << jain << "\n"
           << "fm_total_bytes=" << totalFmBytes << "\n"
           << "fm_retx_bytes=" << totalRetxBytes << "\n"
           << "fm_orig_bytes=" << fmOrigBytes << "\n"
           << "sink_total_bytes=" << totalSinkBytes << "\n"
           << "cross_plane_delta_ratio=" << std::fixed << std::setprecision(6) << crossPlaneDelta
           << "\n";
        for (const auto& r : perFlow)
        {
            if (r.name == "udp-probe" || r.name == "cbr-audio" || r.name == "rt-cbr" ||
                r.name == "probe")
            {
                sm << "ef_flow=" << r.name << "\n"
                   << "ef_meanDelayMs=" << std::fixed << std::setprecision(3) << r.meanDelayMs
                   << "\n"
                   << "ef_rxRateBps=" << std::fixed << std::setprecision(0) << r.rxRateBps << "\n";
                break;
            }
        }

        // RTT-fairness outputs per RFC 7928 Section 6.3: per-category
        // goodput, the category-II/category-I goodput ratio, and the
        // per-category packet drop rate.  Category I = flow 0 (fixed RTT),
        // category II = flow 1 (swept RTT).  Emitted only in RTT-fairness
        // mode so canonical matrix summaries are unchanged.
        if (rttFairnessMode)
        {
            const PerFlowResult* cat1 = nullptr;
            const PerFlowResult* cat2 = nullptr;
            for (const auto& r : perFlow)
            {
                if (r.flowIdx == 0)
                {
                    cat1 = &r;
                }
                if (r.flowIdx == 1)
                {
                    cat2 = &r;
                }
            }
            sm << "rtt_cat1_ms=" << rttCat1Ms << "\n"
               << "rtt_cat2_ms=" << rttCat2Ms << "\n";
            if (cat1 && cat2)
            {
                const double ratio =
                    (cat1->rxRateBps > 0.0) ? (cat2->rxRateBps / cat1->rxRateBps) : 0.0;
                const double drop1 = (cat1->txPackets > 0)
                                         ? static_cast<double>(cat1->lostPackets) / cat1->txPackets
                                         : 0.0;
                const double drop2 = (cat2->txPackets > 0)
                                         ? static_cast<double>(cat2->lostPackets) / cat2->txPackets
                                         : 0.0;
                sm << "goodput_cat1_bps=" << std::fixed << std::setprecision(0) << cat1->rxRateBps
                   << "\n"
                   << "goodput_cat2_bps=" << std::fixed << std::setprecision(0) << cat2->rxRateBps
                   << "\n"
                   << "goodput_ratio_cat2_over_cat1=" << std::fixed << std::setprecision(6) << ratio
                   << "\n"
                   << "drop_rate_cat1=" << std::fixed << std::setprecision(6) << drop1 << "\n"
                   << "drop_rate_cat2=" << std::fixed << std::setprecision(6) << drop2 << "\n";
            }
            else
            {
                sm << "goodput_ratio_cat2_over_cat1=0\n";
            }
        }
    }

    std::cout << "[aqm-eval-runner] " << scenStr << " / " << aqm << "  agg=" << std::fixed
              << std::setprecision(2) << (sumX / 1.0e6) << " Mbps"
              << "  Jain=" << std::fixed << std::setprecision(3) << jain
              << "  cross-plane-Δ=" << std::fixed << std::setprecision(4)
              << (crossPlaneDelta * 100.0) << "%"
              << "  -> " << paths.base << "-{perflow.csv,summary.txt}\n";

    Simulator::Destroy();
    return 0;
}
