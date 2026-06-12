/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Per-flow DiffServ policy classifier keyed on 5-tuple.
 *
 * Unlike PolicyClassifier (DSCP-keyed, shared meter state across
 * all flows at a DSCP), this classifier maintains one PolicyEntry and one
 * meter instance per (srcIp, srcPort, dstIp, dstPort, proto) tuple. Used
 * for thesis-compatible srTCM rate-metered classification where each TCP
 * connection's bucket state evolves independently (RFC 2697 + Andreozzi
 * 2001 §3.3.4).
 *
 */

#ifndef NS3_STRATUM_PER_FLOW_POLICY_CLASSIFIER_H
#define NS3_STRATUM_PER_FLOW_POLICY_CLASSIFIER_H

#include "stratum-meter.h"
#include "stratum-policy-entry.h"
#include "stratum-sr-tcm-meter.h"

#include "ns3/ipv4-address.h"
#include "ns3/object.h"
#include "ns3/ptr.h"

#include <cstdint>
#include <unordered_map>

namespace ns3::stratum
{

/**
 * @ingroup stratum
 *
 * @brief 5-tuple flow key.
 *
 * Hashable via the std::hash specialisation defined below.
 */
struct FlowKey
{
    Ipv4Address srcIp; //!< Source IPv4 address
    uint16_t srcPort;  //!< Source transport port
    Ipv4Address dstIp; //!< Destination IPv4 address
    uint16_t dstPort;  //!< Destination transport port
    uint8_t proto;     //!< IP protocol number (6 = TCP, 17 = UDP)

    /**
     * @brief Equality comparison across all five tuple fields.
     * @param o the other FlowKey
     * @return true iff all five fields match
     */
    bool operator==(const FlowKey& o) const
    {
        return srcIp == o.srcIp && srcPort == o.srcPort && dstIp == o.dstIp &&
               dstPort == o.dstPort && proto == o.proto;
    }
};

} // namespace ns3::stratum

namespace std
{
template <>
struct hash<ns3::stratum::FlowKey>
{
    size_t operator()(const ns3::stratum::FlowKey& k) const noexcept
    {
        // Cheap mix: XOR+shift. Collisions are acceptable (we compare on equality).
        size_t h = std::hash<uint32_t>()(k.srcIp.Get());
        h ^= std::hash<uint16_t>()(k.srcPort) << 1;
        h ^= std::hash<uint32_t>()(k.dstIp.Get()) << 2;
        h ^= std::hash<uint16_t>()(k.dstPort) << 3;
        h ^= std::hash<uint8_t>()(k.proto) << 4;
        return h;
    }
};
} // namespace std

namespace ns3::stratum
{

/**
 * @ingroup stratum
 *
 * @brief Per-flow srTCM rule template.
 *
 * Describes the (DSCP, CIR, CBS, EBS) parameters to apply to every
 * flow registered against this rule.
 */
struct SrTcmRule
{
    uint8_t greenDscp;     //!< DSCP to stamp when srTCM decision is GREEN
    uint8_t yellowDscp;    //!< DSCP to stamp when srTCM decision is YELLOW
    uint8_t redDscp;       //!< DSCP to stamp when srTCM decision is RED
    double cirBytesPerSec; //!< Committed information rate (bytes/second)
    double cbsBytes;       //!< Committed burst size (bytes)
    double ebsBytes;       //!< Excess burst size (bytes)
};

} // namespace ns3::stratum

namespace ns3::stratum::diffserv
{

/**
 * @ingroup stratum
 *
 * @brief Per-flow DiffServ policy classifier keyed on 5-tuple.
 *
 * Unlike PolicyClassifier (DSCP-keyed, shared meter state
 * across all flows at a DSCP), this classifier maintains one
 * PolicyEntry per (srcIp, srcPort, dstIp, dstPort, proto) tuple so
 * that each TCP connection's bucket state evolves independently.
 *
 */
class PerFlowPolicyClassifier : public Object
{
  public:
    /**
     * @brief Get the TypeId for this class.
     * @return the TypeId.
     */
    static TypeId GetTypeId();

    /** @brief Construct a PerFlowPolicyClassifier with no rules. */
    PerFlowPolicyClassifier();

    ~PerFlowPolicyClassifier() override;

    /**
     * @brief Register a 5-tuple for srTCM metering.
     *
     * Lazy-creates the PolicyEntry and meter on first packet match
     * (not here).
     *
     * @param key 5-tuple identifying the flow
     * @param greenDscp DSCP to stamp when srTCM decision is GREEN
     * @param yellowDscp DSCP to stamp when srTCM decision is YELLOW
     * @param redDscp DSCP to stamp when srTCM decision is RED
     * @param cirBytesPerSec committed information rate (bytes/second)
     * @param cbsBytes committed burst size (bytes)
     * @param ebsBytes excess burst size (bytes)
     */
    void AddSrTcmRule(const FlowKey& key,
                      uint8_t greenDscp,
                      uint8_t yellowDscp,
                      uint8_t redDscp,
                      double cirBytesPerSec,
                      double cbsBytes,
                      double ebsBytes);

    /**
     * @brief Apply srTCM for a registered flow.
     *
     * Asserts that the flow is registered.
     *
     * @param key 5-tuple identifying the flow
     * @param packetSize packet size in bytes
     * @param nowSeconds current simulation time in seconds
     * @return the DSCP code point after srTCM re-marking
     */
    uint8_t ApplyPolicy(const FlowKey& key, uint32_t packetSize, double nowSeconds);

    /**
     * @brief Apply srTCM, or pass through if the flow is unknown.
     *
     * Used by the edge-disc fast path so that packets from unknown
     * flows retain their incoming DSCP instead of tripping the
     * assertion in ApplyPolicy.
     *
     * @param key 5-tuple identifying the flow
     * @param packetSize packet size in bytes
     * @param nowSeconds current simulation time in seconds
     * @param incomingDscp DSCP to return unchanged if the flow is unknown
     * @return the DSCP code point after srTCM re-marking, or @p incomingDscp
     */
    uint8_t ApplyPolicyOrPassthrough(const FlowKey& key,
                                     uint32_t packetSize,
                                     double nowSeconds,
                                     uint8_t incomingDscp);

    /** @brief Diagnostic: print the rule table to stdout. */
    void PrintRules() const;

  protected:
    /** @brief Release meter instances and the flow map before destruction. */
    void DoDispose() override;

  private:
    /** @brief Per-flow state entry. */
    struct Entry
    {
        SrTcmRule rule;        //!< Rule parameters cached from AddSrTcmRule
        PolicyEntry state;     //!< cBucket/eBucket/arrivalTime mutated per packet
        Ptr<SrTcmMeter> meter; //!< Singleton instance shared across Entries (stateless)
    };

    std::unordered_map<FlowKey, Entry> m_flows; //!< Per-flow state keyed by 5-tuple
    Ptr<SrTcmMeter> m_meter;                    //!< Shared stateless meter
};

} // namespace ns3::stratum::diffserv

#endif // NS3_STRATUM_PER_FLOW_POLICY_CLASSIFIER_H
