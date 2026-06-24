/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#include "stratum-flent-csv-sink.h"

#include "ns3/inet-socket-address.h"
#include "ns3/inet6-socket-address.h"
#include "ns3/ipv4-address.h"
#include "ns3/ipv6-address.h"
#include "ns3/log.h"
#include "ns3/packet-sink.h"
#include "ns3/packet.h"
#include "ns3/ping.h"
#include "ns3/seq-ts-echo-header.h"
#include "ns3/simulator.h"
#include "ns3/socket-factory.h"
#include "ns3/socket.h"
#include "ns3/uinteger.h"

#include <cerrno>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>

namespace ns3::stratum
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::FlentCsvSink");

namespace
{

void
EnsureDir(const std::string& path)
{
    if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST)
    {
        NS_ABORT_MSG("FlentCsvSink: mkdir(" << path << ") failed: " << std::strerror(errno));
    }
}

std::string
JsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s)
    {
        switch (c)
        {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += c;
        }
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// FlentCsvSink
// ---------------------------------------------------------------------------

FlentCsvSink::FlentCsvSink() = default;

FlentCsvSink::~FlentCsvSink()
{
    Finalize();
}

void
FlentCsvSink::SetTestName(const std::string& name)
{
    m_testName = name;
}

void
FlentCsvSink::SetStepSize(Time stepSize)
{
    m_stepSize = stepSize;
}

void
FlentCsvSink::SetLength(Time length)
{
    m_length = length;
}

void
FlentCsvSink::SetOutputDir(const std::string& dir)
{
    m_outputDir = dir;
    EnsureDir(m_outputDir);
}

void
FlentCsvSink::AddTcpDownFlow(uint32_t index, Ptr<PacketSink> sink)
{
    if (m_tcpDownSinks.size() <= index)
    {
        m_tcpDownSinks.resize(index + 1);
        m_tcpDownLastBytes.resize(index + 1, 0);
        m_tcpDownStreams.resize(index + 1);
    }
    m_tcpDownSinks[index] = sink;
    m_tcpDownLastBytes[index] = 0;
    ArmSamplerIfNeeded();
}

void
FlentCsvSink::AddTcpUpFlow(uint32_t index, Ptr<PacketSink> sink, const std::string& hostId)
{
    if (m_tcpUpSinks.size() <= index)
    {
        m_tcpUpSinks.resize(index + 1);
        m_tcpUpLastBytes.resize(index + 1, 0);
        m_tcpUpStreams.resize(index + 1);
        m_tcpUpHostIds.resize(index + 1);
    }
    m_tcpUpSinks[index] = sink;
    m_tcpUpLastBytes[index] = 0;
    m_tcpUpHostIds[index] = hostId;
    ArmSamplerIfNeeded();
}

void
FlentCsvSink::AddIcmpProbe(Ptr<Ping> pingApp)
{
    pingApp->TraceConnectWithoutContext("Rtt", MakeCallback(&FlentCsvSink::OnPingRtt, this));
}

namespace
{

void
UdpRttTrampoline(FlentCsvSink* self, uint32_t index, uint16_t seq, Time rtt)
{
    self->OnUdpRtt(index, seq, rtt);
}

} // namespace

void
FlentCsvSink::AddUdpProbe(uint32_t index, Ptr<FlentUdpProbeClient> client)
{
    if (m_udpProbeStreams.size() <= index)
    {
        m_udpProbeStreams.resize(index + 1);
    }
    client->TraceConnectWithoutContext("Rtt", MakeBoundCallback(&UdpRttTrampoline, this, index));
}

void
FlentCsvSink::ArmSamplerIfNeeded()
{
    if (m_samplerArmed)
    {
        return;
    }
    m_samplerArmed = true;
    m_samplerEvent = Simulator::Schedule(m_stepSize, &FlentCsvSink::SampleAll, this);
}

void
FlentCsvSink::SampleAll()
{
    const double now = Simulator::Now().GetSeconds();
    const double dt = m_stepSize.GetSeconds();

    OpenXValues() << std::fixed << std::setprecision(6) << now << "\n";

    for (uint32_t i = 0; i < m_tcpDownSinks.size(); ++i)
    {
        if (!m_tcpDownSinks[i])
        {
            continue;
        }
        const uint64_t total = m_tcpDownSinks[i]->GetTotalRx();
        const uint64_t delta = total - m_tcpDownLastBytes[i];
        m_tcpDownLastBytes[i] = total;
        const double goodputMbps = (dt > 0.0) ? (delta * 8.0 / dt / 1.0e6) : 0.0;
        OpenTcpDown(i) << std::fixed << std::setprecision(6) << now << "," << delta << ","
                       << std::setprecision(6) << goodputMbps << "\n";
    }

    for (uint32_t i = 0; i < m_tcpUpSinks.size(); ++i)
    {
        if (!m_tcpUpSinks[i])
        {
            continue;
        }
        const uint64_t total = m_tcpUpSinks[i]->GetTotalRx();
        const uint64_t delta = total - m_tcpUpLastBytes[i];
        m_tcpUpLastBytes[i] = total;
        const double goodputMbps = (dt > 0.0) ? (delta * 8.0 / dt / 1.0e6) : 0.0;
        OpenTcpUp(i) << std::fixed << std::setprecision(6) << now << "," << delta << ","
                     << std::setprecision(6) << goodputMbps << ","
                     << (i < m_tcpUpHostIds.size() ? m_tcpUpHostIds[i] : std::string()) << "\n";
    }

    if (Simulator::Now() + m_stepSize <= m_length)
    {
        m_samplerEvent = Simulator::Schedule(m_stepSize, &FlentCsvSink::SampleAll, this);
    }
}

void
FlentCsvSink::OnPingRtt(uint16_t seq, Time rtt)
{
    const double now = Simulator::Now().GetSeconds();
    OpenPingIcmp() << std::fixed << std::setprecision(6) << now << "," << seq << ","
                   << std::setprecision(6) << rtt.GetSeconds() * 1.0e3 << "\n";
}

void
FlentCsvSink::OnUdpRtt(uint32_t index, uint16_t seq, Time rtt)
{
    const double now = Simulator::Now().GetSeconds();
    OpenUdpProbe(index) << std::fixed << std::setprecision(6) << now << "," << seq << ","
                        << std::setprecision(6) << rtt.GetSeconds() * 1.0e3 << "\n";
}

std::ofstream&
FlentCsvSink::OpenTcpDown(uint32_t index)
{
    if (m_tcpDownStreams.size() <= index)
    {
        m_tcpDownStreams.resize(index + 1);
    }
    if (!m_tcpDownStreams[index])
    {
        std::ostringstream path;
        path << m_outputDir << "/tcp_down_flow" << index << ".csv";
        m_tcpDownStreams[index] = std::make_unique<std::ofstream>(path.str());
        NS_ABORT_MSG_UNLESS(m_tcpDownStreams[index]->is_open(),
                            "FlentCsvSink: cannot open " << path.str());
        (*m_tcpDownStreams[index]) << "t,bytes_delta,goodput_mbps\n";
    }
    return *m_tcpDownStreams[index];
}

std::ofstream&
FlentCsvSink::OpenTcpUp(uint32_t index)
{
    if (m_tcpUpStreams.size() <= index)
    {
        m_tcpUpStreams.resize(index + 1);
    }
    if (!m_tcpUpStreams[index])
    {
        std::ostringstream path;
        path << m_outputDir << "/tcp_up_flow" << index << ".csv";
        m_tcpUpStreams[index] = std::make_unique<std::ofstream>(path.str());
        NS_ABORT_MSG_UNLESS(m_tcpUpStreams[index]->is_open(),
                            "FlentCsvSink: cannot open " << path.str());
        (*m_tcpUpStreams[index]) << "t,bytes_delta,goodput_mbps,host\n";
    }
    return *m_tcpUpStreams[index];
}

std::ofstream&
FlentCsvSink::OpenUdpProbe(uint32_t index)
{
    if (m_udpProbeStreams.size() <= index)
    {
        m_udpProbeStreams.resize(index + 1);
    }
    if (!m_udpProbeStreams[index])
    {
        std::ostringstream path;
        path << m_outputDir << "/udp_probe_flow" << index << ".csv";
        m_udpProbeStreams[index] = std::make_unique<std::ofstream>(path.str());
        NS_ABORT_MSG_UNLESS(m_udpProbeStreams[index]->is_open(),
                            "FlentCsvSink: cannot open " << path.str());
        (*m_udpProbeStreams[index]) << "t,seq,rtt_ms\n";
    }
    return *m_udpProbeStreams[index];
}

std::ofstream&
FlentCsvSink::OpenPingIcmp()
{
    if (!m_pingIcmpStream)
    {
        const std::string path = m_outputDir + "/ping_icmp.csv";
        m_pingIcmpStream = std::make_unique<std::ofstream>(path);
        NS_ABORT_MSG_UNLESS(m_pingIcmpStream->is_open(), "FlentCsvSink: cannot open " << path);
        (*m_pingIcmpStream) << "t,seq,rtt_ms\n";
    }
    return *m_pingIcmpStream;
}

std::ofstream&
FlentCsvSink::OpenXValues()
{
    if (!m_xValuesStream)
    {
        const std::string path = m_outputDir + "/x_values.csv";
        m_xValuesStream = std::make_unique<std::ofstream>(path);
        NS_ABORT_MSG_UNLESS(m_xValuesStream->is_open(), "FlentCsvSink: cannot open " << path);
        (*m_xValuesStream) << "t\n";
    }
    return *m_xValuesStream;
}

void
FlentCsvSink::StampMetadata(const std::map<std::string, std::string>& params)
{
    const std::string path = m_outputDir + "/metadata.json";
    std::ofstream out(path);
    NS_ABORT_MSG_UNLESS(out.is_open(), "FlentCsvSink: cannot open " << path);
    out << "{\n";
    bool first = true;
    for (const auto& kv : params)
    {
        if (!first)
        {
            out << ",\n";
        }
        first = false;
        out << "  \"" << JsonEscape(kv.first) << "\": ";
        if (kv.first == "dscp_map")
        {
            // Caller-supplied JSON object literal; written verbatim.
            out << kv.second;
        }
        else
        {
            out << "\"" << JsonEscape(kv.second) << "\"";
        }
    }
    out << "\n}\n";
}

void
FlentCsvSink::Finalize()
{
    if (m_finalized)
    {
        return;
    }
    m_finalized = true;

    if (m_samplerEvent.IsPending())
    {
        Simulator::Cancel(m_samplerEvent);
    }

    for (auto& s : m_tcpDownStreams)
    {
        if (s)
        {
            s->flush();
            s->close();
        }
    }
    for (auto& s : m_tcpUpStreams)
    {
        if (s)
        {
            s->flush();
            s->close();
        }
    }
    for (auto& s : m_udpProbeStreams)
    {
        if (s)
        {
            s->flush();
            s->close();
        }
    }
    if (m_pingIcmpStream)
    {
        m_pingIcmpStream->flush();
        m_pingIcmpStream->close();
    }
    if (m_xValuesStream)
    {
        m_xValuesStream->flush();
        m_xValuesStream->close();
    }
}

// ---------------------------------------------------------------------------
// FlentUdpProbeClient
// ---------------------------------------------------------------------------

NS_OBJECT_ENSURE_REGISTERED(FlentUdpProbeClient);

TypeId
FlentUdpProbeClient::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::stratum::FlentUdpProbeClient")
            .SetParent<Application>()
            .SetGroupName("Stratum")
            .AddConstructor<FlentUdpProbeClient>()
            .AddAttribute("RemoteAddress",
                          "Destination address of the echo server.",
                          AddressValue(),
                          MakeAddressAccessor(&FlentUdpProbeClient::m_remoteAddress),
                          MakeAddressChecker())
            .AddAttribute("RemotePort",
                          "Destination UDP port of the echo server.",
                          UintegerValue(0),
                          MakeUintegerAccessor(&FlentUdpProbeClient::m_remotePort),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("Interval",
                          "Time between two consecutive probes.",
                          TimeValue(MilliSeconds(16)),
                          MakeTimeAccessor(&FlentUdpProbeClient::m_interval),
                          MakeTimeChecker())
            .AddAttribute("PacketSize",
                          "Size in bytes of the probe payload (header excluded).",
                          UintegerValue(100),
                          MakeUintegerAccessor(&FlentUdpProbeClient::m_packetSize),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("Tos",
                          "IPv4 ToS byte applied to outgoing probes.",
                          UintegerValue(0),
                          MakeUintegerAccessor(&FlentUdpProbeClient::m_tos),
                          MakeUintegerChecker<uint8_t>())
            .AddTraceSource("Rtt",
                            "Round-trip time for each successful echo.",
                            MakeTraceSourceAccessor(&FlentUdpProbeClient::m_rttTrace),
                            "ns3::stratum::FlentUdpProbeClient::RttTrace");
    return tid;
}

FlentUdpProbeClient::FlentUdpProbeClient() = default;

FlentUdpProbeClient::~FlentUdpProbeClient() = default;

void
FlentUdpProbeClient::DoDispose()
{
    m_socket = nullptr;
    Application::DoDispose();
}

void
FlentUdpProbeClient::StartApplication()
{
    if (!m_socket)
    {
        TypeId tid = TypeId::LookupByName("ns3::UdpSocketFactory");
        m_socket = Socket::CreateSocket(GetNode(), tid);
        Address remote;
        if (Ipv4Address::IsMatchingType(m_remoteAddress))
        {
            if (m_socket->Bind() == -1)
            {
                NS_FATAL_ERROR("FlentUdpProbeClient: bind failed");
            }
            m_socket->SetIpTos(m_tos);
            remote = InetSocketAddress(Ipv4Address::ConvertFrom(m_remoteAddress), m_remotePort);
        }
        else
        {
            if (m_socket->Bind6() == -1)
            {
                NS_FATAL_ERROR("FlentUdpProbeClient: bind6 failed");
            }
            m_socket->SetIpv6Tclass(m_tos);
            remote = Inet6SocketAddress(Ipv6Address::ConvertFrom(m_remoteAddress), m_remotePort);
        }
        m_socket->Connect(remote);
        m_socket->SetRecvCallback(MakeCallback(&FlentUdpProbeClient::Receive, this));
    }
    m_sendEvent = Simulator::ScheduleNow(&FlentUdpProbeClient::Send, this);
}

void
FlentUdpProbeClient::StopApplication()
{
    if (m_sendEvent.IsPending())
    {
        Simulator::Cancel(m_sendEvent);
    }
    if (m_socket)
    {
        m_socket->Close();
    }
}

void
FlentUdpProbeClient::Send()
{
    SeqTsEchoHeader hdr;
    hdr.SetSeq(m_nextSeq++);
    hdr.SetTsValue(Simulator::Now());
    Ptr<Packet> p = Create<Packet>(m_packetSize);
    p->AddHeader(hdr);
    m_socket->Send(p);
    m_sendEvent = Simulator::Schedule(m_interval, &FlentUdpProbeClient::Send, this);
}

void
FlentUdpProbeClient::Receive(Ptr<Socket> socket)
{
    Ptr<Packet> p;
    Address from;
    while ((p = socket->RecvFrom(from)))
    {
        SeqTsEchoHeader hdr;
        if (p->GetSize() < hdr.GetSerializedSize())
        {
            continue;
        }
        p->RemoveHeader(hdr);
        const Time rtt = Simulator::Now() - hdr.GetTsValue();
        m_rttTrace(static_cast<uint16_t>(hdr.GetSeq() & 0xFFFF), rtt);
    }
}

// ---------------------------------------------------------------------------
// FlentUdpProbeServer
// ---------------------------------------------------------------------------

NS_OBJECT_ENSURE_REGISTERED(FlentUdpProbeServer);

TypeId
FlentUdpProbeServer::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::FlentUdpProbeServer")
                            .SetParent<Application>()
                            .SetGroupName("Stratum")
                            .AddConstructor<FlentUdpProbeServer>()
                            .AddAttribute("Port",
                                          "UDP port to bind on.",
                                          UintegerValue(0),
                                          MakeUintegerAccessor(&FlentUdpProbeServer::m_port),
                                          MakeUintegerChecker<uint16_t>())
                            .AddAttribute("Local",
                                          "Local address to bind; its family selects the bind "
                                          "family (default: IPv4 any).",
                                          AddressValue(Ipv4Address::GetAny()),
                                          MakeAddressAccessor(&FlentUdpProbeServer::m_local),
                                          MakeAddressChecker());
    return tid;
}

FlentUdpProbeServer::FlentUdpProbeServer() = default;

FlentUdpProbeServer::~FlentUdpProbeServer() = default;

void
FlentUdpProbeServer::DoDispose()
{
    m_socket = nullptr;
    Application::DoDispose();
}

void
FlentUdpProbeServer::StartApplication()
{
    if (!m_socket)
    {
        TypeId tid = TypeId::LookupByName("ns3::UdpSocketFactory");
        m_socket = Socket::CreateSocket(GetNode(), tid);
        Address local;
        if (Ipv4Address::IsMatchingType(m_local))
        {
            local = InetSocketAddress(Ipv4Address::ConvertFrom(m_local), m_port);
        }
        else
        {
            local = Inet6SocketAddress(Ipv6Address::ConvertFrom(m_local), m_port);
        }
        if (m_socket->Bind(local) == -1)
        {
            NS_FATAL_ERROR("FlentUdpProbeServer: bind failed");
        }
        m_socket->SetRecvCallback(MakeCallback(&FlentUdpProbeServer::Receive, this));
    }
}

void
FlentUdpProbeServer::StopApplication()
{
    if (m_socket)
    {
        m_socket->Close();
    }
}

void
FlentUdpProbeServer::Receive(Ptr<Socket> socket)
{
    Ptr<Packet> p;
    Address from;
    while ((p = socket->RecvFrom(from)))
    {
        SeqTsEchoHeader hdr;
        if (p->GetSize() < hdr.GetSerializedSize())
        {
            continue;
        }
        p->RemoveHeader(hdr);
        hdr.SetTsEchoReply(Simulator::Now());
        Ptr<Packet> reply = Create<Packet>(p->GetSize());
        reply->AddHeader(hdr);
        socket->SendTo(reply, 0, from);
    }
}

} // namespace ns3::stratum
