/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * CSV-bundle sink for Flent-style time-series export. Wires trace
 * callbacks on TCP PacketSinks, the Ping application, and a small
 * UDP echo probe pair, and emits one CSV file per logical flow plus
 * a metadata.json sidecar. The schema is documented in
 * scripts/flent-export/SCHEMA.md and is the contract between this
 * emitter and the Python converter that turns a bundle into a
 * Flent v1 JSON file.
 *
 */

#ifndef NS3_STRATUM_FLENT_CSV_SINK_H
#define NS3_STRATUM_FLENT_CSV_SINK_H

#include "ns3/application.h"
#include "ns3/event-id.h"
#include "ns3/nstime.h"
#include "ns3/ptr.h"
#include "ns3/traced-callback.h"

#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ns3
{

class Ping;
class PacketSink;
class Socket;
class Packet;
class Address;

} // namespace ns3

namespace ns3::stratum
{

class FlentUdpProbeClient;

/**
 * @ingroup stratum
 *
 * @brief Per-flow CSV time-series emitter for Flent-style bundles.
 *
 * The sink is a plain C++ helper (not a TypeId Object). The owner
 * configures the test name, sampling step size, simulation length, and
 * output directory; then registers each TCP, ICMP, and UDP flow before
 * the simulator runs. TCP flows are sampled at @c stepSize cadence by
 * polling @c PacketSink::GetTotalRx; ICMP and UDP RTTs are written
 * event-by-event from trace-source callbacks.
 *
 * The metadata.json sidecar carries simulation parameters as JSON
 * strings. The Python converter coerces numeric fields per-key. The
 * @c dscp_map value must already be a JSON-encoded object string
 * (the v1 contract; an empty map is "{}").
 *
 * Call order: SetTestName / SetStepSize / SetLength / SetOutputDir,
 * then AddTcpDown / AddTcpUp / AddIcmpProbe / AddUdpProbe in any
 * order, then StampMetadata, then Simulator::Run, then Finalize.
 * Finalize is idempotent and safe from a Simulator::Schedule hook.
 *
 */
class FlentCsvSink
{
  public:
    FlentCsvSink();
    ~FlentCsvSink();

    // Non-copyable: holds open ofstreams.
    FlentCsvSink(const FlentCsvSink&) = delete;
    FlentCsvSink& operator=(const FlentCsvSink&) = delete;

    /**
     * @brief Set the test name (written to metadata.json).
     * @param name test identifier, e.g. "rrul"
     */
    void SetTestName(const std::string& name);

    /**
     * @brief Set the sampling step size for TCP goodput.
     * @param stepSize interval between SampleAll firings
     */
    void SetStepSize(Time stepSize);

    /**
     * @brief Set the simulation length (sampler stops after this).
     * @param length total simulation duration
     */
    void SetLength(Time length);

    /**
     * @brief Set (and create if missing) the output directory.
     * @param dir absolute or relative path; created with mode 0755
     */
    void SetOutputDir(const std::string& dir);

    /**
     * @brief Wire a TCP-download flow: poll bytes_rx every step_size.
     * @param index flow slot (controls filename suffix)
     * @param sink the PacketSink whose GetTotalRx is sampled
     */
    void AddTcpDownFlow(uint32_t index, Ptr<PacketSink> sink);

    /**
     * @brief Wire a TCP-upload flow.
     * @param index flow slot (controls filename suffix)
     * @param sink the PacketSink whose GetTotalRx is sampled
     * @param hostId optional host label written to the `host` column; empty string if omitted
     */
    void AddTcpUpFlow(uint32_t index, Ptr<PacketSink> sink, const std::string& hostId = "");

    /**
     * @brief Wire ICMP RTT capture from a Ping application.
     * @param pingApp the Ping app; its "Rtt" trace source is connected
     */
    void AddIcmpProbe(Ptr<Ping> pingApp);

    /**
     * @brief Wire UDP RTT capture from a FlentUdpProbeClient.
     * @param index probe slot (controls filename suffix)
     * @param client the client app; its "Rtt" trace source is connected
     */
    void AddUdpProbe(uint32_t index, Ptr<FlentUdpProbeClient> client);

    /**
     * @brief Write metadata.json. Call after all flows have been
     *        registered and before Simulator::Run.
     * @param params key/value pairs; all values are written as JSON
     *        strings except the "dscp_map" key, whose value must
     *        already be a JSON-encoded object literal
     */
    void StampMetadata(const std::map<std::string, std::string>& params);

    /**
     * @brief Flush and close all CSV files; cancel any pending sampler.
     *
     * Idempotent. Safe to call from a Simulator::Schedule hook fired
     * just before Stop, or from main() after Simulator::Run returns.
     */
    void Finalize();

    /**
     * @brief UDP probe RTT callback dispatch (public so the
     *        per-index trampoline in the .cc can route into the sink).
     * @param index UDP probe slot
     * @param seq UDP echo sequence number
     * @param rtt round-trip time
     */
    void OnUdpRtt(uint32_t index, uint16_t seq, Time rtt);

  private:
    /**
     * @brief Reschedules itself every step_size until length is reached.
     *
     * For each registered TCP-down/up flow, samples GetTotalRx, writes
     * a delta + goodput row, and advances the per-flow last-bytes
     * cursor. Also appends the current time to x_values.csv.
     */
    void SampleAll();

    /**
     * @brief Trace callback for Ping "Rtt".
     * @param seq ICMP echo sequence number
     * @param rtt round-trip time
     */
    void OnPingRtt(uint16_t seq, Time rtt);

    /**
     * @brief Open (lazily) the per-tcp-down CSV file for @p index.
     * @param index flow slot
     * @return reference to the opened ofstream
     */
    std::ofstream& OpenTcpDown(uint32_t index);

    /**
     * @brief Open (lazily) the per-tcp-up CSV file for @p index.
     * @param index flow slot
     * @return reference to the opened ofstream
     */
    std::ofstream& OpenTcpUp(uint32_t index);

    /**
     * @brief Open (lazily) the per-udp-probe CSV file for @p index.
     * @param index probe slot
     * @return reference to the opened ofstream
     */
    std::ofstream& OpenUdpProbe(uint32_t index);

    /**
     * @brief Open (lazily) the ping_icmp.csv file.
     * @return reference to the opened ofstream
     */
    std::ofstream& OpenPingIcmp();

    /**
     * @brief Open (lazily) the x_values.csv file.
     * @return reference to the opened ofstream
     */
    std::ofstream& OpenXValues();

    /**
     * @brief Arm the sampler if not already armed.
     */
    void ArmSamplerIfNeeded();

    std::string m_testName{"unspecified"};
    std::string m_outputDir{"."};
    Time m_stepSize{MilliSeconds(200)};
    Time m_length{Seconds(60)};

    // Registered flows (parallel arrays indexed by flow slot).
    std::vector<Ptr<PacketSink>> m_tcpDownSinks;
    std::vector<uint64_t> m_tcpDownLastBytes;
    std::vector<Ptr<PacketSink>> m_tcpUpSinks;
    std::vector<uint64_t> m_tcpUpLastBytes;

    // Per-flow host labels (parallel to m_tcpUpSinks; empty string for unattributed flows).
    std::vector<std::string> m_tcpUpHostIds;

    // Lazily opened ofstreams.
    std::vector<std::unique_ptr<std::ofstream>> m_tcpDownStreams;
    std::vector<std::unique_ptr<std::ofstream>> m_tcpUpStreams;
    std::vector<std::unique_ptr<std::ofstream>> m_udpProbeStreams;
    std::unique_ptr<std::ofstream> m_pingIcmpStream;
    std::unique_ptr<std::ofstream> m_xValuesStream;

    EventId m_samplerEvent;
    bool m_samplerArmed{false};
    bool m_finalized{false};
};

/**
 * @ingroup stratum
 *
 * @brief Tiny UDP echo client that timestamps each packet and emits
 * an "Rtt" trace on each reply.
 *
 * Built on @c SeqTsEchoHeader (already in ns-3 mainline). On
 * StartApplication the client opens an ephemeral UDP socket, schedules
 * the first Send, and continues sending one packet every @c Interval
 * until StopApplication. On Recv it extracts the SeqTsEchoHeader,
 * computes RTT as @c Now() - hdr.GetTsValue(), and fires the trace.
 *
 */
class FlentUdpProbeClient : public Application
{
  public:
    /**
     * @brief Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId();

    FlentUdpProbeClient();
    ~FlentUdpProbeClient() override;

    /**
     * @brief Trace-source signature for the "Rtt" trace.
     * @param [in] seq UDP echo sequence number (cast to uint16_t)
     * @param [in] rtt round-trip time
     */
    typedef void (*RttTrace)(uint16_t seq, Time rtt);

  protected:
    void DoDispose() override;

  private:
    void StartApplication() override;
    void StopApplication() override;

    /**
     * @brief Send one timestamped UDP probe and reschedule.
     */
    void Send();

    /**
     * @brief Receive callback for the UDP socket.
     * @param socket the receiving socket
     */
    void Receive(Ptr<Socket> socket);

    Address m_remoteAddress;
    uint16_t m_remotePort{0};
    Time m_interval{MilliSeconds(16)};
    uint32_t m_packetSize{100};
    uint8_t m_tos{0};

    Ptr<Socket> m_socket;
    EventId m_sendEvent;
    uint32_t m_nextSeq{0};

    TracedCallback<uint16_t, Time> m_rttTrace;
};

/**
 * @ingroup stratum
 *
 * @brief Tiny UDP echo server that mirrors back each
 * @c FlentUdpProbeClient packet with the receive timestamp filled in.
 *
 * The TsEchoReply field is set to @c Now() before transmit; it is
 * informational (the client RTT computation only uses the round-trip of
 * TsValue), but populating it keeps the wire format consistent with the
 * existing SeqTsEchoHeader contract.
 *
 */
class FlentUdpProbeServer : public Application
{
  public:
    /**
     * @brief Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId();

    FlentUdpProbeServer();
    ~FlentUdpProbeServer() override;

  protected:
    void DoDispose() override;

  private:
    void StartApplication() override;
    void StopApplication() override;

    /**
     * @brief Receive callback for the UDP socket.
     * @param socket the receiving socket
     */
    void Receive(Ptr<Socket> socket);

    uint16_t m_port{0};
    Ptr<Socket> m_socket;
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_FLENT_CSV_SINK_H
