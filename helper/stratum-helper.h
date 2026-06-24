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

/// Canonical DiffServ PHB layout selected by SetAsDiffserv.
enum class Profile
{
    ExpeditedForwarding, //!< EF (DSCP 46) on a priority lane + best-effort (DSCP 0).
    BestEffort,          //!< A single best-effort queue.
};

/// Declarative specification for SetAsDiffserv.
struct DiffservSpec
{
    Profile profile = Profile::ExpeditedForwarding; //!< PHB layout to compose.
    Ptr<Scheduler> scheduler = nullptr; //!< Across-queue scheduler; nullptr -> PriorityScheduler.
};

/**
 * @brief Designated-initialiser spec for a per-flow srTCM metering rule.
 *
 * Pass to Helper::AddSrTcmMeterRule(). Fields listed in declaration order
 * so C++20 designated-initialiser syntax compiles. Rates in bits/s;
 * bucket sizes in bytes. srcPort and dstPort default to kAnyPort (wildcard).
 */
struct SrTcmMeterRuleSpec
{
    Ipv4Address srcIp;          //!< Source IPv4 address (concrete — no wildcard)
    uint16_t srcPort{kAnyPort}; //!< Source port (kAnyPort = wildcard)
    Ipv4Address dstIp;          //!< Destination IPv4 address
    uint16_t dstPort{kAnyPort}; //!< Destination port
    uint8_t proto{6};           //!< IP protocol (6 = TCP, 17 = UDP)
    uint8_t greenDscp{0};       //!< DSCP stamped on GREEN-coloured packets
    uint8_t yellowDscp{0};      //!< DSCP stamped on YELLOW
    uint8_t redDscp{0};         //!< DSCP stamped on RED
    double cirBps{0.0};         //!< Committed information rate (bits/s)
    double cbsBytes{0.0};       //!< Committed burst size (bytes)
    double ebsBytes{0.0};       //!< Excess burst size (bytes)
};

/** @brief Designated-init spec for a token-bucket meter policy (codepoint-keyed). */
struct TokenBucketPolicySpec
{
    uint8_t codePt;  //!< DSCP code point this policy applies to
    double cirBps;   //!< committed information rate in bits/s
    double cbsBytes; //!< committed burst size in bytes
};

/** @brief Designated-init spec for an srTCM (RFC 2697) meter policy (codepoint-keyed). */
struct SrTcmPolicySpec
{
    uint8_t codePt;  //!< DSCP code point this policy applies to
    double cirBps;   //!< committed information rate in bits/s
    double cbsBytes; //!< committed burst size in bytes
    double ebsBytes; //!< excess burst size in bytes
};

/** @brief Designated-init spec for a trTCM (RFC 2698) meter policy (codepoint-keyed). */
struct TrTcmPolicySpec
{
    uint8_t codePt;  //!< DSCP code point this policy applies to
    double cirBps;   //!< committed information rate in bits/s
    double cbsBytes; //!< committed burst size in bytes
    double pirBps;   //!< peak information rate in bits/s
    double pbsBytes; //!< peak burst size in bytes
};

/** @brief Designated-init spec for a TSW2CM meter policy (codepoint-keyed). */
struct Tsw2cmPolicySpec
{
    uint8_t codePt;            //!< DSCP code point this policy applies to
    double cirBps;             //!< committed information rate in bits/s
    double winLenSeconds{1.0}; //!< TSW averaging-window length in seconds
};

/** @brief Designated-init spec for a TSW3CM meter policy (codepoint-keyed). */
struct Tsw3cmPolicySpec
{
    uint8_t codePt;            //!< DSCP code point this policy applies to
    double cirBps;             //!< committed information rate in bits/s
    double pirBps;             //!< peak information rate in bits/s
    double winLenSeconds{1.0}; //!< TSW averaging-window length in seconds
};

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
     * @brief Add a token-bucket policy entry (codepoint-keyed).
     *
     * @param edge the edge queue disc to configure
     * @param p designated-initialiser spec (see TokenBucketPolicySpec)
     */
    void AddTokenBucketPolicy(Ptr<EdgeQueueDisc> edge, const TokenBucketPolicySpec& p);

    /**
     * @brief Add an srTCM (RFC 2697) policy entry (codepoint-keyed).
     *
     * @param edge the edge queue disc to configure
     * @param p designated-initialiser spec (see SrTcmPolicySpec)
     */
    void AddSrTcmPolicy(Ptr<EdgeQueueDisc> edge, const SrTcmPolicySpec& p);

    /**
     * @brief Register a per-flow srTCM metering rule on an edge queue disc.
     *
     * Installs the edge disc's PerFlowPolicyClassifier (creating it on the
     * first call) and adds a 5-tuple rule. The CIR in @p rule is in bits/s
     * and is converted to bytes/s internally; bucket sizes are in bytes.
     *
     * @param edge the edge queue disc
     * @param rule designated-initialiser spec (see SrTcmMeterRuleSpec)
     */
    void AddSrTcmMeterRule(Ptr<EdgeQueueDisc> edge, const SrTcmMeterRuleSpec& rule);

    /**
     * @brief Add a trTCM (RFC 2698) policy entry (codepoint-keyed).
     *
     * @param edge the edge queue disc to configure
     * @param p designated-initialiser spec (see TrTcmPolicySpec)
     */
    void AddTrTcmPolicy(Ptr<EdgeQueueDisc> edge, const TrTcmPolicySpec& p);

    /**
     * @brief Add a TSW2CM policy entry (codepoint-keyed).
     *
     * @param edge the edge queue disc to configure
     * @param p designated-initialiser spec (see Tsw2cmPolicySpec)
     */
    void AddTsw2cmPolicy(Ptr<EdgeQueueDisc> edge, const Tsw2cmPolicySpec& p);

    /**
     * @brief Add a TSW3CM policy entry (codepoint-keyed).
     *
     * @param edge the edge queue disc to configure
     * @param p designated-initialiser spec (see Tsw3cmPolicySpec)
     */
    void AddTsw3cmPolicy(Ptr<EdgeQueueDisc> edge, const Tsw3cmPolicySpec& p);

    // -------------------------------------------------------------------------
    // Policer entries
    // -------------------------------------------------------------------------

    /**
     * @brief Add a policer table entry to an edge queue disc.
     *
     * @param edge the edge queue disc to configure
     * @param entry the policer entry (designated-init: .policer, .initialCodePt,
     *              .downgrade1, .downgrade2, .policyIndex)
     */
    void AddPolicerEntry(Ptr<EdgeQueueDisc> edge, const PolicerEntry& entry);

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
    void ConfigQueue(Ptr<RedQueueDisc> disc, const RedQueueConfig& cfg);

    /** @brief Set the MRED mode for one physical queue. */
    void SetMredMode(Ptr<RedQueueDisc> disc, MredMode mode, uint32_t queue);

    /** @brief Set the MRED mode for every physical queue. */
    void SetMredModeAllQueues(Ptr<RedQueueDisc> disc, MredMode mode);

    /**
     * @brief Compose @p edge as a canonical DiffServ edge in one call.
     *
     * Installs the inner RED disc, sizes the queue/precedence topology for
     * @p spec.profile, writes the PHB table, and attaches the scheduler
     * (@p spec.scheduler, or a PriorityScheduler when null). Mutates @p edge
     * in place; the caller initialises and installs it.
     *
     * @param edge edge queue disc to compose.
     * @param spec PHB profile and optional scheduler.
     */
    static void SetAsDiffserv(Ptr<EdgeQueueDisc> edge, const DiffservSpec& spec = {});
};

} // namespace ns3::stratum::diffserv

#endif // NS3_STRATUM_HELPER_H
