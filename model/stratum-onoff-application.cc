/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "stratum-onoff-application.h"

#include "stratum-send-time-tag.h"

#include "ns3/double.h"
#include "ns3/log.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/socket.h"
#include "ns3/udp-socket-factory.h"
#include "ns3/uinteger.h"

namespace ns3::stratum
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::OnOffApplication");

NS_OBJECT_ENSURE_REGISTERED(OnOffApplication);

TypeId
OnOffApplication::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::OnOffApplication")
                            .SetParent<Application>()
                            .SetGroupName("Stratum")
                            .AddConstructor<OnOffApplication>()
                            .AddAttribute("Remote",
                                          "Remote (sink) address.",
                                          AddressValue(),
                                          MakeAddressAccessor(&OnOffApplication::m_remote),
                                          MakeAddressChecker())
                            .AddAttribute("PacketSize",
                                          "Bytes per packet during ON periods.",
                                          UintegerValue(48),
                                          MakeUintegerAccessor(&OnOffApplication::m_pktSize),
                                          MakeUintegerChecker<uint32_t>(1))
                            .AddAttribute("DataRateBps",
                                          "Data rate during ON periods (bits/s).",
                                          UintegerValue(6400),
                                          MakeUintegerAccessor(&OnOffApplication::m_dataRate),
                                          MakeUintegerChecker<uint64_t>(1))
                            .AddAttribute("OnMean",
                                          "Mean of exponential ON period (seconds).",
                                          DoubleValue(0.340),
                                          MakeDoubleAccessor(&OnOffApplication::m_onMean),
                                          MakeDoubleChecker<double>(0.0))
                            .AddAttribute("OffMean",
                                          "Mean of exponential OFF period (seconds).",
                                          DoubleValue(0.427),
                                          MakeDoubleAccessor(&OnOffApplication::m_offMean),
                                          MakeDoubleChecker<double>(0.0));
    return tid;
}

OnOffApplication::OnOffApplication()
    : m_socket(nullptr),
      m_pktSize(48),
      m_dataRate(6400),
      m_onMean(0.340),
      m_offMean(0.427),
      m_onRng(CreateObject<ExponentialRandomVariable>()),
      m_offRng(CreateObject<ExponentialRandomVariable>()),
      m_running(false),
      m_onNow(false)
{
}

void
OnOffApplication::Setup(Address remote,
                        uint32_t pktSize,
                        uint64_t dataRateBps,
                        double onMean,
                        double offMean)
{
    m_remote = remote;
    m_pktSize = pktSize;
    m_dataRate = dataRateBps;
    m_onMean = onMean;
    m_offMean = offMean;
}

void
OnOffApplication::StartApplication()
{
    m_onRng->SetAttribute("Mean", DoubleValue(m_onMean));
    m_offRng->SetAttribute("Mean", DoubleValue(m_offMean));

    m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
    m_socket->Connect(m_remote);
    m_running = true;
    StartOnPeriod();
}

void
OnOffApplication::StopApplication()
{
    m_running = false;
    if (m_nextEvent.IsPending())
    {
        Simulator::Cancel(m_nextEvent);
    }
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
OnOffApplication::StartOnPeriod()
{
    if (!m_running)
    {
        return;
    }
    m_onNow = true;
    double onDur = m_onRng->GetValue();
    m_nextEvent = Simulator::Schedule(Seconds(onDur), &OnOffApplication::StartOffPeriod, this);
    SendPacket();
}

void
OnOffApplication::StartOffPeriod()
{
    if (!m_running)
    {
        return;
    }
    m_onNow = false;
    double offDur = m_offRng->GetValue();
    m_nextEvent = Simulator::Schedule(Seconds(offDur), &OnOffApplication::StartOnPeriod, this);
}

void
OnOffApplication::SendPacket()
{
    if (!m_running || !m_onNow)
    {
        return;
    }
    Ptr<Packet> packet = Create<Packet>(m_pktSize);
    SendTimeTag tag(Simulator::Now().GetSeconds());
    packet->AddPacketTag(tag);
    m_socket->Send(packet);

    double interval = static_cast<double>(m_pktSize * 8) / static_cast<double>(m_dataRate);
    m_sendEvent = Simulator::Schedule(Seconds(interval), &OnOffApplication::SendPacket, this);
}

} // namespace ns3::stratum
