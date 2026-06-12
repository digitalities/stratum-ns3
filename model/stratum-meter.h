/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * Copyright (C) 2000 Nortel Networks
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsPolicy.h class Policy (2001).
 */

#ifndef NS3_STRATUM_METER_H
#define NS3_STRATUM_METER_H

#include "stratum-constants.h"
#include "stratum-policy-entry.h"

#include "ns3/nstime.h"
#include "ns3/object.h"
#include "ns3/traced-callback.h"

namespace ns3::stratum
{

/**
 * @ingroup stratum
 *
 * @brief Abstract base class for DiffServ meters.
 *
 * Each subclass implements a specific metering algorithm (token bucket,
 * srTCM, trTCM, etc.). State is held externally in a PolicyEntry;
 * the meter is a stateless algorithm operating on that state.
 *
 * The MeterColour trace source fires on every colour decision via the
 * non-virtual ApplyPolicer forwarding function that delegates to DoApplyPolicer.
 * Concrete meters override DoApplyPolicer only.
 *
 */
class Meter : public Object
{
  public:
    /**
     * @brief Callback signature for the MeterColour trace source.
     *
     * @param colour The colour decision (GREEN, YELLOW, or RED).
     * @param classId The policy table index from the PolicyEntry.
     * @param when The simulator time of the decision.
     */
    typedef void (*MeterColourTracedCallback)(Colour colour, uint32_t classId, ns3::Time when);

    /**
     * @brief Get the TypeId for this class.
     * @return the TypeId.
     */
    static TypeId GetTypeId();

    /**
     * @brief Update bucket state based on elapsed time since last arrival.
     *
     * @param entry policy entry with bucket state (modified in place)
     * @param nowSeconds current simulation time in seconds
     * @param packetSize packet size in bytes (used by TSW meters)
     */
    virtual void ApplyMeter(PolicyEntry& entry, double nowSeconds, uint32_t packetSize) = 0;

    /**
     * @brief Decide packet colour and fire the MeterColour trace.
     *
     * Calls DoApplyPolicer, then fires m_colourTrace
     * with (colour, entry.policyIndex, Simulator::Now()).
     *
     * @param entry policy entry (may be modified)
     * @param packetSize packet size in bytes
     * @return colour decision
     */
    Colour ApplyPolicer(PolicyEntry& entry, uint32_t packetSize);

    /**
     * @brief Set the L2 framing overhead bytes per packet.
     * @param bytes per-packet L2 overhead (PPP=2, Ethernet=14,
     * SimpleLink=0). Default 0.
     *
     * Subclasses that consume byte counts (token-bucket, srTCM, trTCM,
     * tsw2cm, tsw3cm) add this to the IP-layer packet size before
     * charging tokens / measuring rate, so the meter reasons in WIRE
     * bytes — the same byte basis the FQ scheduler must use to keep
     * meter and scheduler aligned. Companion attribute on
     * `Scheduler` MUST be set to the same value.
     *
     */
    void SetL2OverheadBytes(uint32_t bytes)
    {
        m_l2OverheadBytes = bytes;
    }

    /**
     * @brief Get the L2 framing overhead bytes per packet.
     * @return per-packet L2 overhead in bytes
     */
    uint32_t GetL2OverheadBytes() const
    {
        return m_l2OverheadBytes;
    }

  protected:
    /**
     * @brief Concrete meter implementation of the colour decision.
     *
     * Subclasses override this instead of ApplyPolicer.
     * Called by ApplyPolicer, which fires the trace after this returns.
     *
     * @param entry policy entry (may be modified)
     * @param packetSize packet size in bytes
     * @return colour decision
     */
    virtual Colour DoApplyPolicer(PolicyEntry& entry, uint32_t packetSize) = 0;

    /**
     * @brief Fire the MeterColour trace source.
     *
     * Called automatically by ApplyPolicer after DoApplyPolicer returns.
     *
     * @param colour The colour returned by DoApplyPolicer.
     * @param classId The policyIndex from the PolicyEntry.
     */
    void NotifyColour(Colour colour, uint32_t classId);

    uint32_t m_l2OverheadBytes{0}; //!< L2 framing overhead per packet

  private:
    ns3::TracedCallback<Colour, uint32_t, ns3::Time> m_colourTrace; //!< MeterColour trace source
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_METER_H
