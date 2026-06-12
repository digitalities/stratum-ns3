/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#ifndef NS3_STRATUM_MONITOR_HELPER_H
#define NS3_STRATUM_MONITOR_HELPER_H

#include "ns3/event-id.h"
#include "ns3/nstime.h"
#include "ns3/ptr.h"
#include "ns3/stratum-red-queue-disc.h"
#include "ns3/stratum-statistics.h"

#include <fstream>
#include <string>
#include <vector>

namespace ns3::stratum
{

/**
 * @ingroup stratum
 *
 * @brief Declarative monitoring helper that connects RedQueueDisc trace
 * sources to Statistics and writes periodic metric traces.
 *
 * Usage:
 * @code
 * MonitorHelper monitor;
 * monitor.SetOutputDirectory("/tmp/diffserv-run1");
 * monitor.SetDepartureRateInterval(Seconds(1.0));
 * monitor.SetQueueLengthInterval(Seconds(0.5));
 * monitor.Install(edgeDisc);
 * // ... run simulation ...
 * monitor.PrintStats();
 * monitor.Close();
 * @endcode
 *
 * Install() connects to the StratumEnqueue, StratumDequeue, and StratumDrop trace sources
 * on the given RedQueueDisc, creates (or reuses) a Statistics
 * object, opens trace files, and schedules periodic sampling callbacks.
 *
 * This is a plain C++ class (not an ns-3 Object), following the same
 * pattern as diffserv::Helper.
 *
 */
class MonitorHelper
{
  public:
    /** @brief Construct a MonitorHelper with default intervals. */
    MonitorHelper();

    ~MonitorHelper();

    // -- Configuration (call before Install) --

    /**
     * @brief Set the output directory for trace files.
     *
     * The directory is created if it does not exist.
     *
     * @param dir path to output directory
     */
    void SetOutputDirectory(const std::string& dir);

    /**
     * @brief Set the interval for departure-rate sampling.
     * @param interval sampling interval (default: 1.0 s)
     */
    void SetDepartureRateInterval(Time interval);

    /**
     * @brief Set the interval for queue-length sampling.
     * @param interval sampling interval (default: 0.5 s)
     */
    void SetQueueLengthInterval(Time interval);

    /**
     * @brief Set the simulation time at which periodic sampling begins.
     * @param startTime start time (default: 0.0 s)
     */
    void SetSamplingStartTime(Time startTime);

    // -- Installation --

    /**
     * @brief Connect traces, open files, and schedule sampling.
     *
     * Connects to the StratumEnqueue, StratumDequeue, and StratumDrop trace sources on
     * the underlying RedQueueDisc, creates a Statistics object,
     * opens trace files in the configured output directory, and schedules
     * periodic departure-rate and queue-length sampling callbacks.
     *
     * Accepts either a bare `RedQueueDisc` or one of the composers
     * (`EdgeQueueDisc`, `CoreQueueDisc`) whose inner
     * RedQueueDisc carries the trace sources and sub-queue fan-out.
     *
     * @param disc the queue disc to monitor
     */
    void Install(Ptr<QueueDisc> disc);

    // -- Post-simulation --

    /**
     * @brief Print per-DSCP statistics to std::cout.
     *
     * Delegates to Statistics::PrintStats().
     */
    void PrintStats() const;

    /**
     * @brief Close all open trace files.
     *
     * Safe to call multiple times; also called by the destructor.
     */
    void Close();

    /**
     * @brief Get the Statistics object.
     * @return the statistics collector, or nullptr if not installed
     */
    Ptr<Statistics> GetStats() const;

  private:
    // -- Trace sink callbacks --

    /**
     * @brief Trace sink for StratumEnqueue.
     * @param item the enqueued item
     * @param dscp the DSCP code point
     */
    void OnEnqueue(Ptr<const QueueDiscItem> item, uint8_t dscp);

    /**
     * @brief Trace sink for StratumDequeue.
     * @param item the dequeued item
     * @param dscp the DSCP code point
     * @param queueIndex the physical queue index
     */
    void OnDequeue(Ptr<const QueueDiscItem> item, uint8_t dscp, uint32_t queueIndex);

    /**
     * @brief Trace sink for StratumDrop.
     * @param item the dropped item
     * @param dscp the DSCP code point
     * @param dropReason 0 = RED early drop, 1 = tail drop
     */
    void OnDrop(Ptr<const QueueDiscItem> item, uint8_t dscp, uint32_t dropReason);

    // -- Periodic sampling callbacks --

    /**
     * @brief Sample departure rates and write to ServiceRate.tr.
     */
    void SampleDepartureRate();

    /**
     * @brief Sample queue lengths and write to QueueLen-Q{N}.tr files.
     */
    void SampleQueueLength();

    // -- Members --

    /**
     * @brief The monitored queue disc.
     *
     * Typed as `Ptr<QueueDisc>` so the helper accepts any inner
     * queue-disc type. Red-specific operations — scheduler sampling
     * and StratumEnqueue / StratumDequeue / StratumDrop trace connections — guard
     * via `DynamicCast<RedQueueDisc>`; if the inner is non-Red
     * those paths degrade gracefully (no service-rate sampling, no
     * per-packet DSCP counters) while per-physical-queue length
     * sampling continues to work via the generic
     * `GetNQueueDiscClasses()` +
     * `GetQueueDiscClass(i)->GetQueueDisc()->GetNPackets()` path.
     */
    Ptr<QueueDisc> m_disc;
    Ptr<RedQueueDisc> m_redDisc; //!< Non-null iff m_disc is a RedQueueDisc
    Ptr<Statistics> m_stats;     //!< Statistics collector

    std::string m_outputDir;      //!< Output directory for trace files
    Time m_departureRateInterval; //!< Departure-rate sampling interval
    Time m_queueLengthInterval;   //!< Queue-length sampling interval
    Time m_samplingStartTime;     //!< Simulation time to start sampling

    std::ofstream m_serviceRateFile;            //!< ServiceRate.tr file
    std::vector<std::ofstream> m_queueLenFiles; //!< Per-queue QueueLen-Q{N}.tr files

    /**
     * @brief Pending departure-rate sampling event.
     *
     * Cancelled on destruction so that a helper going out of scope
     * before Simulator::Destroy() does not leave a callback holding a
     * raw this pointer in the event queue.
     */
    EventId m_depRateEvent;

    /**
     * @brief Pending queue-length sampling event.
     *
     * Same lifetime contract as m_depRateEvent.
     */
    EventId m_qLenEvent;

    bool m_installed{false}; //!< True after Install() has been called
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_MONITOR_HELPER_H
