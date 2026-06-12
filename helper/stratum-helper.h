/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#ifndef NS3_STRATUM_HELPER_H
#define NS3_STRATUM_HELPER_H

#include "ns3/net-device.h"
#include "ns3/ptr.h"
#include "ns3/stratum-app-type-tag.h"
#include "ns3/stratum-constants.h"
#include "ns3/stratum-core-queue-disc.h"
#include "ns3/stratum-edge-queue-disc.h"
#include "ns3/stratum-mark-rule.h"
#include "ns3/stratum-per-flow-policy-classifier.h"
#include "ns3/stratum-policy-entry.h"
#include "ns3/stratum-red-queue-disc.h"
#include "ns3/stratum-scheduler.h"

#include <cstdint>
#include <string>

/**
 * @defgroup stratum Stratum QoS substrate (DiffServ, L4S, CAKE)
 *
 * Port of the 2001 ns-2 DiffServ4NS module (Sergio Andreozzi) to ns-3
 * mainline. Provides edge/core composite queue disciplines, a full
 * meter hierarchy (token-bucket, srTCM, trTCM, TSW2CM, TSW3CM, FW),
 * scheduler variants (PQ, LLQ, RR, WRR, WIRR, WFQ, WF2Q+, SFQ, SCFQ,
 * L4S-coupled), and helpers for DSCP-based classification and marking.
 */

namespace ns3::stratum::diffserv
{

/**
 * @brief Create @p path (mode 0755) if it does not already exist.
 *
 * Aborts via `NS_ABORT_MSG` on any `mkdir(2)` failure other than
 * `EEXIST`. Parent directories must already exist — there is no
 * recursive `mkdir -p` behaviour (broken paths should fail early,
 * not be silently materialised; see plan §6 non-goals).
 *
 * Replaces the 2001-era pattern of bare `mkdir(path.c_str(), 0755)`
 * calls with no error handling that was flagged across the examples
 * + `MonitorHelper` in the 2026-04-19 `/cpp-review` sweep.
 */
void EnsureDir(const std::string& path);

/**
 * @brief Convenience helper for constructing DiffServ edge and core router
 * configurations.
 *
 * Provides fluent methods for adding mark rules, policy entries, policer
 * entries, PHB table entries, and scheduler assignments without requiring
 * callers to manually construct PolicyEntry / PolicerEntry / MarkRule structs.
 *
 * Rates are accepted in bits/s and converted to bytes/s internally before being
 * stored in the PolicyEntry fields (which use bytes/s, matching the ns-2
 * original).
 *
 * This is a plain C++ class, not an ns-3 Object — no TypeId is needed.
 *
 */
class Helper
{
  public:
    Helper();

    /**
     * @brief Auto-detect the per-packet L2 framing overhead a NetDevice
     * will add to each outgoing packet.
     *
     * Returns the number of bytes the device's `AddHeader` step will
     * prepend to each IP-layer packet before serialisation:
     *
     * - `PointToPointNetDevice` (PPP): 2 bytes
     * - `CsmaNetDevice` (Ethernet): 14 bytes (header only)
     * - any other / unrecognised type: 0 (caller may set explicitly)
     *
     * Use this when configuring DiffServ schedulers and meters to
     * reason in WIRE bytes rather than IP-layer bytes, so FQ
     * allocations and token-bucket charges match the byte budget the
     * link physically consumes.
     *
     * @param dev the netdev the qdisc is (or will be) attached to
     * @return per-packet L2 overhead in bytes, or 0 if not detectable
     */
    static uint32_t DetectL2OverheadBytes(Ptr<NetDevice> dev);

    // -------------------------------------------------------------------------
    // Inner-disc installation sugar
    // -------------------------------------------------------------------------

    /**
     * @brief Create a fresh `RedQueueDisc`, install it as the edge's
     * inner via `SetInnerDisc`, and return a typed handle.
     *
     * Terse idiom for the common case where the edge wraps a
     * RedQueueDisc configured via its own API:
     *
     * @code
     * auto inner = helper.InstallRedInner(edge);
     * inner->SetNumQueues(5);
     * inner->AddPhbEntry(46, 0, 0);
     * // ... configure inner fully ...
     * edge->Initialize();
     * @endcode
     *
     * Must be called before Initialize on the edge. Returns the same
     * `Ptr<RedQueueDisc>` the caller could obtain via
     * `DynamicCast<RedQueueDisc>(edge->GetInnerDisc())` after the fact.
     */
    Ptr<RedQueueDisc> InstallRedInner(Ptr<EdgeQueueDisc> edge);

    /** @brief Same as above for `CoreQueueDisc`. */
    Ptr<RedQueueDisc> InstallRedInner(Ptr<CoreQueueDisc> core);

    // -------------------------------------------------------------------------
    // Mark rules (edge only)
    // -------------------------------------------------------------------------

    /**
     * @brief Add a multi-field classification rule to an edge queue disc.
     *
     * @param edge the edge queue disc to configure
     * @param dscp the DSCP to assign on match
     * @param srcAddr source address (-1 = kAnyHost = any)
     * @param dstAddr destination address (-1 = kAnyHost = any)
     * @param protocol IP protocol (0 = kAnyProtocol = any)
     * @param appType application type (0 = kAnyAppType = any)
     */
    void AddMarkRule(Ptr<EdgeQueueDisc> edge,
                     uint8_t dscp,
                     int32_t srcAddr,
                     int32_t dstAddr,
                     uint8_t protocol,
                     uint32_t appType);

    /**
     * @brief Add a mark rule with transport-layer port matching (RFC 2475
     * §2.3.1).
     *
     * @param edge the edge queue disc to configure
     * @param dscp initial DSCP to mark matching packets with
     * @param srcAddr source IPv4 address (kAnyHost = -1 for wildcard)
     * @param dstAddr destination IPv4 address (kAnyHost = -1 for wildcard)
     * @param protocol IP protocol (kAnyProtocol = 0 for wildcard, 6=TCP, 17=UDP)
     * @param appType application type (0 = kAnyAppType = any)
     * @param srcPort source port (kAnyPort = 0 for wildcard)
     * @param dstPort destination port (kAnyPort = 0 for wildcard)
     */
    void AddMarkRuleWithPorts(Ptr<EdgeQueueDisc> edge,
                              uint8_t dscp,
                              int32_t srcAddr,
                              int32_t dstAddr,
                              uint8_t protocol,
                              uint32_t appType,
                              uint16_t srcPort,
                              uint16_t dstPort);

    // -------------------------------------------------------------------------
    // Policy entries — rates in bits/s, converted to bytes/s internally
    // -------------------------------------------------------------------------

    /**
     * @brief Add a Dumb (pass-through) policy entry.
     *
     * @param edge the edge queue disc to configure
     * @param codePt DSCP code point this policy applies to
     */
    void AddDumbPolicy(Ptr<EdgeQueueDisc> edge, uint8_t codePt);

    /**
     * @brief Add a Token-Bucket policy entry.
     *
     * @param edge the edge queue disc to configure
     * @param codePt DSCP code point this policy applies to
     * @param cirBps committed information rate in bits/s
     * @param cbsBytes committed burst size in bytes
     */
    void AddTokenBucketPolicy(Ptr<EdgeQueueDisc> edge,
                              uint8_t codePt,
                              double cirBps,
                              double cbsBytes);

    /**
     * @brief Add an srTCM (RFC 2697) policy entry.
     *
     * @param edge the edge queue disc to configure
     * @param codePt DSCP code point this policy applies to
     * @param cirBps committed information rate in bits/s
     * @param cbsBytes committed burst size in bytes
     * @param ebsBytes excess burst size in bytes
     */
    void AddSrTcmPolicy(Ptr<EdgeQueueDisc> edge,
                        uint8_t codePt,
                        double cirBps,
                        double cbsBytes,
                        double ebsBytes);

    /**
     * @brief Register a per-flow srTCM metering rule on an edge queue disc.
     *
     * Installs the edge disc's PerFlowPolicyClassifier (creating it
     * on the first call) and adds a 5-tuple rule. Rates are in
     * bits/s; bucket sizes in bytes.
     *
     * @param edge the edge queue disc
     * @param srcIp source IPv4 address (concrete — no wildcards)
     * @param srcPort source port
     * @param dstIp destination IPv4 address
     * @param dstPort destination port
     * @param proto IP protocol (6 = TCP, 17 = UDP)
     * @param greenDscp DSCP stamped on GREEN-coloured packets
     * @param yellowDscp DSCP stamped on YELLOW
     * @param redDscp DSCP stamped on RED
     * @param cirBps committed information rate in bits/s
     * @param cbsBytes committed burst size in bytes
     * @param ebsBytes excess burst size in bytes
     */
    void AddSrTcmMeterRule(Ptr<EdgeQueueDisc> edge,
                           Ipv4Address srcIp,
                           uint16_t srcPort,
                           Ipv4Address dstIp,
                           uint16_t dstPort,
                           uint8_t proto,
                           uint8_t greenDscp,
                           uint8_t yellowDscp,
                           uint8_t redDscp,
                           double cirBps,
                           double cbsBytes,
                           double ebsBytes);

    /**
     * @brief Add a trTCM (RFC 2698) policy entry.
     *
     * @param edge the edge queue disc to configure
     * @param codePt DSCP code point this policy applies to
     * @param cirBps committed information rate in bits/s
     * @param cbsBytes committed burst size in bytes
     * @param pirBps peak information rate in bits/s
     * @param pbsBytes peak burst size in bytes
     */
    void AddTrTcmPolicy(Ptr<EdgeQueueDisc> edge,
                        uint8_t codePt,
                        double cirBps,
                        double cbsBytes,
                        double pirBps,
                        double pbsBytes);

    /**
     * @brief Add a TSW2CM policy entry.
     *
     * @param edge the edge queue disc to configure
     * @param codePt DSCP code point this policy applies to
     * @param cirBps committed information rate in bits/s
     */
    void AddTsw2cmPolicy(Ptr<EdgeQueueDisc> edge, uint8_t codePt, double cirBps);

    /**
     * @brief Add a TSW3CM policy entry.
     *
     * @param edge the edge queue disc to configure
     * @param codePt DSCP code point this policy applies to
     * @param cirBps committed information rate in bits/s
     * @param pirBps peak information rate in bits/s
     */
    void AddTsw3cmPolicy(Ptr<EdgeQueueDisc> edge, uint8_t codePt, double cirBps, double pirBps);

    // -------------------------------------------------------------------------
    // Policer entries
    // -------------------------------------------------------------------------

    /**
     * @brief Add a policer table entry to an edge queue disc.
     *
     * @param edge the edge queue disc to configure
     * @param policer the policer type
     * @param initialCodePt GREEN outcome DSCP
     * @param downgrade1 YELLOW (or out-of-profile) outcome DSCP
     * @param downgrade2 RED outcome DSCP (0 if not applicable)
     */
    void AddPolicerEntry(Ptr<EdgeQueueDisc> edge,
                         PolicerType policer,
                         int initialCodePt,
                         int downgrade1,
                         int downgrade2);

    // -------------------------------------------------------------------------
    // Shared configuration (any DS-RED-family queue disc)
    //
    // The edge and core composers do not inherit from RedQueueDisc, so
    // these helpers take the generic `Ptr<QueueDisc>` and dispatch at
    // runtime to the matching forwarder on edge/core or the method on
    // RedQueueDisc itself. All three concrete targets expose the same
    // method surface, so the dispatch is trivial.
    // -------------------------------------------------------------------------

    /**
     * @brief Add a PHB table entry on a DS-RED, edge, or core queue disc.
     *
     * @param disc the queue disc (RedQueueDisc, edge, or core)
     * @param codePt DSCP code point
     * @param queue physical queue index
     * @param prec drop precedence level
     */
    void AddPhbEntry(Ptr<RedQueueDisc> disc, uint8_t codePt, uint8_t queue, uint8_t prec);

    /** @brief Set the scheduling discipline on a RedQueueDisc. */
    void SetScheduler(Ptr<RedQueueDisc> disc, Ptr<Scheduler> scheduler);

    /** @brief Configure RED thresholds for a (queue, prec) virtual queue. */
    void ConfigQueue(Ptr<RedQueueDisc> disc,
                     uint32_t queue,
                     uint32_t prec,
                     double thMin,
                     double thMax,
                     double maxP);

    /** @brief Set the MRED mode for one or all queues. */
    void SetMredMode(Ptr<RedQueueDisc> disc, MredMode mode, uint32_t queue = kMaxQueues);
};

} // namespace ns3::stratum::diffserv

#endif // NS3_STRATUM_HELPER_H
