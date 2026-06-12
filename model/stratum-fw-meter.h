/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * Copyright (C) 2000 Nortel Networks
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * FW Meter — per-flow byte-accounting policer.
 *
 * ALGORITHM PROVENANCE:
 * - Originally "Short Flow Differentiating (SFD)" by Chen and
 * Heidemann (USC/ISI): Computer Networks 41(6):779-794, 2003.
 * - Implemented in ns-2.29 DiffServ module as SFDPolicy (~2000)
 * - Renamed to FWPolicy in DiffServ4NS (Andreozzi, 2001) before
 * the paper was published; algorithm preserved verbatim.
 * The expansion of "FW" is not recorded; the class name
 * FWMeter was assigned during the ns-3 port (originally
 * FWMeter, shortened for consistency with the FW prefix).
 * - Ported to ns-3 in this module (2026)
 *
 * NOT an RFC-based meter. Maintains per-flow byte accounting and
 * penalises flows exceeding CIR via three configurable modes.
 *
 * RELATIONSHIP TO PER-FLOW CLASSIFIER:
 * FWMeter is a single fused (classify + meter + downgrade)
 * algorithm with its own internal FlowEntry table. The per-flow
 * classification substrate is `diffserv::PerFlowPolicyClassifier`, which
 * any Meter (including RFC-2697 `SrTcmMeter`) can drive per-flow.
 * FWMeter is one instance of a broader family of per-flow
 * policies, distinguished by its fused design and pre-standards
 * (Chen/Heidemann 2003) origin.
 *
 * @see provenance/FW_THESIS_REFERENCES.md (provenance analysis)
 * @see diffserv::PerFlowPolicyClassifier (generalisation of the per-flow pattern)
 */

#ifndef NS3_STRATUM_FW_METER_H
#define NS3_STRATUM_FW_METER_H

#include "stratum-meter.h"

#include "ns3/random-variable-stream.h"

#include <cstdint>
#include <unordered_map>

namespace ns3::stratum
{

/**
 * @ingroup stratum
 *
 * @brief FW per-flow byte-accounting meter.
 *
 * Unlike all other meters in this module, FWMeter maintains
 * internal per-flow state (an unordered map of FlowEntry structs keyed
 * by flow ID). Flows not seen for > kFlowTimeoutSeconds are purged.
 *
 * Three penalty modes are supported for excess flows (selected via
 * PolicerEntry::downgrade2):
 * 0 = deterministic: every excess packet is downgraded
 * 1 = probabilistic: P(GREEN) = CIR / bytesSent
 * 2 = periodic: 1-in-6 cycle (5 downgrades, then 1 green)
 *
 * Exception to the base interface: ApplyPolicerFw() returns DSCP code
 * points directly instead of Colour enum. The base-class ApplyPolicer()
 * override returns Colour for interface compatibility.
 *
 */
class FWMeter : public Meter
{
  public:
    /**
     * @brief Get the TypeId for this class.
     * @return the TypeId.
     */
    static TypeId GetTypeId();

    /** @brief Construct an FWMeter. */
    FWMeter();

    // --- Base class overrides (interface compatibility) ----------------------

    /**
     * @brief Delegates to ApplyMeterWithFlowId with flowId=0 (single-flow default).
     *
     * @param entry policy entry (uses cir as byte threshold)
     * @param nowSeconds current simulation time in seconds
     * @param packetSize packet size in bytes
     */
    void ApplyMeter(PolicyEntry& entry, double nowSeconds, uint32_t packetSize) override;

    // --- Extended API with explicit flow ID ---------------------------------

    /**
     * @brief Update per-flow byte accounting for the given flow.
     *
     * @param entry policy entry (uses cir as byte threshold)
     * @param nowSeconds current simulation time in seconds
     * @param packetSize packet size in bytes
     * @param flowId explicit flow identifier
     */
    void ApplyMeterWithFlowId(PolicyEntry& entry,
                              double nowSeconds,
                              uint32_t packetSize,
                              uint32_t flowId);

    /**
     * @brief Policing decision returning DSCP code point directly.
     *
     * @param entry policy entry (uses cir as byte threshold)
     * @param policer policer entry (initialCodePt, downgrade1, downgrade2)
     * @param packetSize packet size in bytes (unused, state already in FlowEntry)
     * @param flowId explicit flow identifier
     * @return DSCP code point (initialCodePt or downgrade1)
     */
    int ApplyPolicerFw(PolicyEntry& entry,
                       PolicerEntry& policer,
                       uint32_t packetSize,
                       uint32_t flowId);

    /**
     * @brief Seat this meter's UniformRandomVariable on an explicit stream
     * so simulations can control reproducibility independently of the
     * global default-stream sequence.
     *
     * documents the silent global-stream usage inherited from
     * the 2001 ns-2 code; this hook lets the edge disc's
     * AssignStreams cascade isolate FW probabilistic marks.
     *
     * @param stream RNG stream index
     * @return number of streams consumed (always 1)
     */
    int64_t AssignStreams(int64_t stream);

  protected:
    /**
     * @brief Simple threshold check: GREEN if bytesSent <= CIR, YELLOW otherwise.
     *
     * @param entry policy entry (uses cir as byte threshold)
     * @param packetSize packet size in bytes
     * @return GREEN or YELLOW colour decision
     */
    Colour DoApplyPolicer(PolicyEntry& entry, uint32_t packetSize) override;

  private:
    /** Flows not seen for longer than this are purged from the table. */
    static constexpr double kFlowTimeoutSeconds = 5.0;

    /** @brief Per-flow byte-accounting state. */
    struct FlowEntry
    {
        uint32_t flowId{0};     //!< Flow identifier (from per-flow classifier)
        double lastUpdate{0.0}; //!< Simulator time (seconds) of last packet on this flow
        uint64_t bytesSent{0};  //!< Running byte count for the flow
        uint32_t count{0};      //!< Periodic mode counter (downgrade2=2)
    };

    std::unordered_map<uint32_t, FlowEntry> m_flowTable; //!< flowId -> FlowEntry
    Ptr<UniformRandomVariable> m_rng;                    //!< Probabilistic-mode RNG

    /**
     * @brief Remove flows whose lastUpdate is older than
     * nowSeconds - kFlowTimeoutSeconds.
     *
     * @param nowSeconds current simulation time in seconds
     */
    void PurgeExpiredFlows(double nowSeconds);
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_FW_METER_H
