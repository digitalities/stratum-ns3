// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright (C) 2026 Sergio Andreozzi
//
// Closed-loop bridging example. Single ns-3 Node with two EmuFdNetDevices
// (raw-socket-bound to host veth ends) acts as an L3 router between two
// Linux network namespaces. Stratum CAKE (DiffServ4 with host-isolation
// enabled — same wiring as cake-host-fairness-sweep) sits on the forward
// egress; a FifoQueueDisc no-op on the reverse egress.

#include "ns3/core-module.h"
#include "ns3/fd-net-device-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/stratum-module.h"
#include "ns3/traffic-control-module.h"

#include <fstream>

using namespace ns3;
namespace cake = ns3::stratum::cake;
using ns3::stratum::EdgeQueueDisc;

NS_LOG_COMPONENT_DEFINE("CakeStratumBridgeRouter");

namespace
{

PcapFile g_enqueuePcap;
PcapFile g_dequeuePcap;
bool g_enqueueInited = false;
bool g_dequeueInited = false;

void
EnqueueCallback(Ptr<const QueueDiscItem> item)
{
    if (!g_enqueueInited || !item || !item->GetPacket())
    {
        return;
    }
    Ptr<const Ipv4QueueDiscItem> ipv4Item = DynamicCast<const Ipv4QueueDiscItem>(item);
    if (!ipv4Item)
    {
        return;
    }
    Ptr<Packet> p = ipv4Item->GetPacket()->Copy();
    p->AddHeader(ipv4Item->GetHeader());
    const int64_t ns = Simulator::Now().GetNanoSeconds();
    g_enqueuePcap.Write(static_cast<uint32_t>(ns / 1000000000LL),
                        static_cast<uint32_t>(ns % 1000000000LL),
                        p);
}

void
DequeueCallback(Ptr<const QueueDiscItem> item)
{
    if (!g_dequeueInited || !item || !item->GetPacket())
    {
        return;
    }
    Ptr<const Ipv4QueueDiscItem> ipv4Item = DynamicCast<const Ipv4QueueDiscItem>(item);
    if (!ipv4Item)
    {
        return;
    }
    Ptr<Packet> p = ipv4Item->GetPacket()->Copy();
    p->AddHeader(ipv4Item->GetHeader());
    const int64_t ns = Simulator::Now().GetNanoSeconds();
    g_dequeuePcap.Write(static_cast<uint32_t>(ns / 1000000000LL),
                        static_cast<uint32_t>(ns % 1000000000LL),
                        p);
}

std::string
ReadEnv(const char* k, const std::string& dflt)
{
    const char* v = std::getenv(k);
    return v ? std::string(v) : dflt;
}

void
EmitProvenance(const std::string& path,
               uint32_t rngRun,
               double simTime,
               const std::string& bandwidth)
{
    if (path.empty())
    {
        return;
    }
    std::ofstream f(path);
    f << "{\n"
      << "  \"rng_run\": " << rngRun << ",\n"
      << "  \"sim_time_s\": " << simTime << ",\n"
      << "  \"bandwidth\": \"" << bandwidth << "\",\n"
      << "  \"ns3_commit\": \"" << ReadEnv("NS3_COMMIT", "unknown") << "\",\n"
      << "  \"diffserv_sha\": \"" << ReadEnv("DIFFSERV_SHA", "unknown") << "\",\n"
      << "  \"example\": \"cake-stratum-bridge-router\"\n"
      << "}\n";
}

} // namespace

int
main(int argc, char* argv[])
{
    std::string rxIface = "ns3-rx";
    std::string txIface = "ns3-tx";
    std::string rxAddr = "10.2.0.2";
    std::string txAddr = "10.4.0.1";
    std::string rxPeerAddr = "10.2.0.1";
    std::string txPeerAddr = "10.4.0.2";
    std::string srcSubnet = "10.1.0.0";
    std::string sinkSubnet = "10.3.0.0";
    std::string bandwidth = "100Mbps";
    bool enableHostIsolation = true; // false = control: CAKE without host-isolation DRR
    bool useDualPi2Inner = false;    // true = per-tin DualPI2 (CAKE+L4S composition);
                                     // mutually exclusive with enableHostIsolation
    double simTime = 80.0;
    std::string enqueuePcapPath;
    std::string dequeuePcapPath;
    std::string provenanceOut;
    uint32_t rngRun = 1;

    CommandLine cmd(__FILE__);
    cmd.AddValue("rxIface", "veth name on the receive (router-a) side", rxIface);
    cmd.AddValue("txIface", "veth name on the transmit (router-b) side", txIface);
    cmd.AddValue("rxAddr", "ns-3 IP on rxIface", rxAddr);
    cmd.AddValue("txAddr", "ns-3 IP on txIface", txAddr);
    cmd.AddValue("rxPeerAddr", "next-hop IP toward src netns", rxPeerAddr);
    cmd.AddValue("txPeerAddr", "next-hop IP toward sink netns", txPeerAddr);
    cmd.AddValue("srcSubnet", "src netns /16", srcSubnet);
    cmd.AddValue("sinkSubnet", "sink netns /16", sinkSubnet);
    cmd.AddValue("bandwidth", "CAKE bandwidth (e.g. 100Mbps)", bandwidth);
    cmd.AddValue("enableHostIsolation",
                 "Enable CAKE host-isolation DRR (default true; set false for control)",
                 enableHostIsolation);
    cmd.AddValue("useDualPi2Inner",
                 "Use L4S DualPI2 as the per-tin inner instead of FqCobalt; "
                 "mutually exclusive with enableHostIsolation",
                 useDualPi2Inner);
    cmd.AddValue("simTime", "wall-clock seconds to run", simTime);
    cmd.AddValue("enqueuePcap", "pcap path for CAKE enqueue trace", enqueuePcapPath);
    cmd.AddValue("dequeuePcap", "pcap path for CAKE dequeue trace", dequeuePcapPath);
    cmd.AddValue("provenanceOut", "path to write provenance.json", provenanceOut);
    cmd.AddValue("RngRun", "RngSeedManager run number", rngRun);
    cmd.Parse(argc, argv);

    RngSeedManager::SetRun(rngRun);

    // Real-time scheduler MUST be configured before any node creation.
    // BestEffort policy: don't abort when real-time falls behind simulation
    // time. Hard-limit policy (the default) calls NS_FATAL_ERROR if the
    // simulator can't keep up, which crashes the process under 16-flow CAKE
    // load on a 4-CPU Lima. BestEffort just keeps going as fast as possible;
    // the resulting timing drift is logged via the cross-validation check
    // for `hard limit exceeded` warnings in bands.yaml (set to 0 — drift is
    // detected as ci95-width inflation, not as a fatal abort).
    GlobalValue::Bind("SimulatorImplementationType", StringValue("ns3::RealtimeSimulatorImpl"));
    Config::SetDefault("ns3::RealtimeSimulatorImpl::SynchronizationMode",
                       StringValue("BestEffort"));
    GlobalValue::Bind("ChecksumEnabled", BooleanValue(true));

    NodeContainer nodes;
    nodes.Create(1);
    Ptr<Node> nodeA = nodes.Get(0);

    // Two EmuFdNetDevices.
    EmuFdNetDeviceHelper rxHelper;
    rxHelper.SetDeviceName(rxIface);
    NetDeviceContainer rxDevs = rxHelper.Install(nodeA);

    EmuFdNetDeviceHelper txHelper;
    txHelper.SetDeviceName(txIface);
    NetDeviceContainer txDevs = txHelper.Install(nodeA);

    // Internet stack + addresses + forwarding.
    InternetStackHelper internet;
    internet.SetIpv6StackInstall(false);
    internet.Install(nodeA);

    Ptr<Ipv4> ipv4 = nodeA->GetObject<Ipv4>();
    NS_ABORT_MSG_IF(!ipv4, "Ipv4 not installed on nodeA");

    auto addIface = [&](Ptr<NetDevice> dev, const std::string& addr) -> uint32_t {
        uint32_t idx = ipv4->AddInterface(dev);
        Ipv4InterfaceAddress ifAddr(Ipv4Address(addr.c_str()), Ipv4Mask("/24"));
        ipv4->AddAddress(idx, ifAddr);
        ipv4->SetMetric(idx, 1);
        ipv4->SetUp(idx);
        return idx;
    };
    uint32_t rxIfIdx = addIface(rxDevs.Get(0), rxAddr);
    uint32_t txIfIdx = addIface(txDevs.Get(0), txAddr);
    ipv4->SetAttribute("IpForward", BooleanValue(true));

    Ipv4StaticRoutingHelper srHelper;
    Ptr<Ipv4StaticRouting> sr = srHelper.GetStaticRouting(ipv4);
    sr->AddNetworkRouteTo(Ipv4Address(srcSubnet.c_str()),
                          Ipv4Mask("/16"),
                          Ipv4Address(rxPeerAddr.c_str()),
                          rxIfIdx);
    sr->AddNetworkRouteTo(Ipv4Address(sinkSubnet.c_str()),
                          Ipv4Mask("/16"),
                          Ipv4Address(txPeerAddr.c_str()),
                          txIfIdx);

    // Stratum CAKE on TX egress (same wiring as cake-host-fairness-sweep:
    // DiffServ4 with host-isolation enabled).
    Ptr<TrafficControlLayer> tc = nodeA->GetObject<TrafficControlLayer>();
    NS_ABORT_MSG_IF(!tc, "TrafficControlLayer missing");
    if (tc->GetRootQueueDiscOnDevice(txDevs.Get(0)))
    {
        tc->DeleteRootQueueDiscOnDevice(txDevs.Get(0));
    }
    DataRate bw(bandwidth);
    Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
    cake::Helper::SetAsCakeDiffserv4(edge,
                                     bw,
                                     /*enableAckFilter=*/false,
                                     /*enableLlq=*/false,
                                     /*enableTinShaping=*/true,
                                     /*enableHostIsolation=*/enableHostIsolation,
                                     /*useInnerTbfShaping=*/false,
                                     /*enableAckFilterAggressive=*/false,
                                     /*useDualPi2Inner=*/useDualPi2Inner);
    tc->SetRootQueueDiscOnDevice(txDevs.Get(0), edge);

    // No-op FifoQueueDisc on RX egress (reverse-direction ACKs path).
    if (tc->GetRootQueueDiscOnDevice(rxDevs.Get(0)))
    {
        tc->DeleteRootQueueDiscOnDevice(rxDevs.Get(0));
    }
    TrafficControlHelper tchNoOp;
    tchNoOp.SetRootQueueDisc("ns3::FifoQueueDisc");
    tchNoOp.Install(rxDevs);

    // Enqueue/dequeue pcap traces: capture LINKTYPE_RAW with nanosecond
    // timestamps so external tooling can diff the qdisc input vs output.
    if (!enqueuePcapPath.empty())
    {
        g_enqueuePcap.Open(enqueuePcapPath, std::ios::out);
        g_enqueuePcap.Init(/*linkType=*/101 /*RAW*/, 65535, 0, 0, true /*nano*/);
        g_enqueueInited = true;
        edge->TraceConnectWithoutContext("Enqueue", MakeCallback(&EnqueueCallback));
    }
    if (!dequeuePcapPath.empty())
    {
        g_dequeuePcap.Open(dequeuePcapPath, std::ios::out);
        g_dequeuePcap.Init(/*linkType=*/101 /*RAW*/, 65535, 0, 0, true /*nano*/);
        g_dequeueInited = true;
        edge->TraceConnectWithoutContext("Dequeue", MakeCallback(&DequeueCallback));
    }

    EmitProvenance(provenanceOut, rngRun, simTime, bandwidth);

    NS_LOG_INFO("Starting closed-loop bridge router; rngRun=" << rngRun << " simTime=" << simTime
                                                              << "s bandwidth=" << bandwidth);
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();
    Simulator::Destroy();
    return 0;
}
