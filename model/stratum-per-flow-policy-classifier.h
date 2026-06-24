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
#include "ns3/ipv6-address.h"
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
    // --- existing five fields (kept first, in the same order) ---
    // All positional-init sites ({Ipv4Address, port, Ipv4Address, port, proto})
    // leave the appended members at their defaults and build V4 keys unchanged.
    Ipv4Address srcIp; //!< Source IPv4 address (V4 path)
    uint16_t srcPort;  //!< Source transport port
    Ipv4Address dstIp; //!< Destination IPv4 address (V4 path)
    uint16_t dstPort;  //!< Destination transport port
    uint8_t proto;     //!< IP protocol number (6 = TCP, 17 = UDP)

    // --- extended fields (appended; defaults preserve V4 behaviour) ---
    //
    // Reserved groundwork: the equality and hash machinery below is fully
    // V6-capable, but no production path builds a V6 key today. The edge disc
    // gates per-flow metering on an IPv4 item, so these fields are exercised
    // only by unit tests. Enabling V6 per-flow metering means wiring the edge
    // to build V6 keys and adding a V6 rule API in lockstep.

    /** Address family tag. */
    enum Family
    {
        V4, //!< IPv4 — uses srcIp / dstIp
        V6  //!< IPv6 — uses srcIp6 / dstIp6
    };

    Family family{V4};  //!< Defaults to V4; positional-init sites unchanged
    Ipv6Address srcIp6; //!< Source IPv6 address (V6 path; unused when family == V4)
    Ipv6Address dstIp6; //!< Destination IPv6 address (V6 path; unused when family == V4)

    /**
     * @brief Family-aware equality comparison.
     *
     * The V4 path (family == V4) is value-equivalent to the original
     * five-field comparison, preserving byte-identical metering for all
     * existing V4 flows.
     *
     * @param o the other FlowKey
     * @return true iff all discriminating fields match
     */
    bool operator==(const FlowKey& o) const
    {
        if (family != o.family)
        {
            return false;
        }
        if (srcPort != o.srcPort || dstPort != o.dstPort || proto != o.proto)
        {
            return false;
        }
        if (family == V4)
        {
            return srcIp == o.srcIp && dstIp == o.dstIp;
        }
        // V6
        return srcIp6 == o.srcIp6 && dstIp6 == o.dstIp6;
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
        if (k.family == ns3::stratum::FlowKey::V4)
        {
            // V4 branch: BYTE-IDENTICAL to the original formula — do NOT
            // fold family / srcIp6 / dstIp6 into this path.  Any change
            // here reshuffles unordered_map buckets and breaks the
            // byte-replay regression oracle.
            size_t h = std::hash<uint32_t>()(k.srcIp.Get());
            h ^= std::hash<uint16_t>()(k.srcPort) << 1;
            h ^= std::hash<uint32_t>()(k.dstIp.Get()) << 2;
            h ^= std::hash<uint16_t>()(k.dstPort) << 3;
            h ^= std::hash<uint8_t>()(k.proto) << 4;
            return h;
        }
        // V6 branch: new behaviour — mix 16-byte src + 16-byte dst + ports + proto.
        // Collisions are acceptable (equality check governs correctness).
        uint8_t srcBuf[16];
        uint8_t dstBuf[16];
        k.srcIp6.GetBytes(srcBuf);
        k.dstIp6.GetBytes(dstBuf);
        size_t h = 0;
        for (int i = 0; i < 16; ++i)
        {
            h ^= std::hash<uint8_t>()(srcBuf[i]) << (i & 0xF);
            h ^= std::hash<uint8_t>()(dstBuf[i]) << ((i + 8) & 0xF);
        }
        h ^= std::hash<uint16_t>()(k.srcPort) << 1;
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
