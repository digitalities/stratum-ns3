/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef NS3_STRATUM_ONOFF_APPLICATION_H
#define NS3_STRATUM_ONOFF_APPLICATION_H

#include "ns3/address.h"
#include "ns3/application.h"
#include "ns3/event-id.h"
#include "ns3/ptr.h"
#include "ns3/random-variable-stream.h"

#include <cstdint>

namespace ns3
{
class Socket;

} // namespace ns3

namespace ns3::stratum
{

/**
 * @ingroup stratum
 *
 * @brief ON/OFF UDP traffic generator that stamps every outgoing packet with
 * a SendTimeTag at send time.
 *
 * Stock ns3::OnOffApplication fires its Tx TraceSource *after* calling
 * socket->Send, so a TraceSource-based approach cannot attach a PacketTag
 * before transmission. This class replicates the ON/OFF + fixed-rate
 * behaviour and stamps every outgoing packet with SendTimeTag
 * (simulation time at send), which receiving sinks read to compute OWD
 * and IPDV.
 *
 * The model is intentionally minimal: a single UDP socket, exponential
 * ON and OFF periods (configurable means), fixed packet size, fixed
 * data rate during ON periods. For richer traffic models use stock
 * ns3::OnOffApplication without tag stamping.
 *
 * Typical usage via OnOffHelper:
 * @code
 * OnOffHelper voip(InetSocketAddress(sinkAddr, 5060));
 * voip.SetAttribute("PacketSize", UintegerValue(48));
 * voip.SetAttribute("DataRate",   DataRateValue(DataRate("6.4kb/s")));
 * voip.SetAttribute("OnMean",     DoubleValue(0.340));
 * voip.SetAttribute("OffMean",    DoubleValue(0.427));
 * ApplicationContainer apps = voip.Install(senderNode);
 * @endcode
 *
 * @see SendTimeTag
 * @see OnOffHelper
 */
class OnOffApplication : public Application
{
  public:
    /**
     * @brief Get the TypeId for this class.
     * @return the TypeId.
     */
    static TypeId GetTypeId();

    /** @brief Construct a OnOffApplication with default attributes. */
    OnOffApplication();

    ~OnOffApplication() override = default;

    /**
     * @brief Configure the application.
     *
     * Equivalent to the SetAttribute-based configuration but accepts all
     * parameters at once — convenient when building senders
     * programmatically (no helper).
     *
     * @param remote remote address (InetSocketAddress)
     * @param pktSize bytes per packet
     * @param dataRateBps data rate during ON periods, bits/second
     * @param onMean mean of exponential ON period, seconds
     * @param offMean mean of exponential OFF period, seconds
     */
    void Setup(Address remote,
               uint32_t pktSize,
               uint64_t dataRateBps,
               double onMean,
               double offMean);

  private:
    void StartApplication() override;
    void StopApplication() override;

    /** @brief Begin a new ON period (exponential duration) and start sending. */
    void StartOnPeriod();

    /** @brief Begin a new OFF period (exponential duration) and suspend sending. */
    void StartOffPeriod();

    /** @brief Send one packet and schedule the next send event. */
    void SendPacket();

    Ptr<Socket> m_socket;                    //!< UDP socket used for transmission
    Address m_remote;                        //!< Remote address (InetSocketAddress)
    uint32_t m_pktSize;                      //!< Bytes per packet
    uint64_t m_dataRate;                     //!< Data rate during ON periods (bits/second)
    double m_onMean;                         //!< Mean of exponential ON period (seconds)
    double m_offMean;                        //!< Mean of exponential OFF period (seconds)
    Ptr<ExponentialRandomVariable> m_onRng;  //!< Exponential RNG for ON period lengths
    Ptr<ExponentialRandomVariable> m_offRng; //!< Exponential RNG for OFF period lengths
    bool m_running;                          //!< True between StartApplication and StopApplication
    bool m_onNow;                            //!< True during ON periods

    /**
     * @brief Pending on/off toggle event (StartOnPeriod <-> StartOffPeriod).
     */
    EventId m_nextEvent;

    /**
     * @brief Pending next SendPacket recursion.
     *
     * Distinct from m_nextEvent because the two schedules run on
     * independent cadences during an ON period. Captured so
     * StopApplication can cancel the pending send instead of relying on
     * SendPacket's !m_running early-return to harmlessly drop the
     * late-firing callback.
     */
    EventId m_sendEvent;
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_ONOFF_APPLICATION_H
