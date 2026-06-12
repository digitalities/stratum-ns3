/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * Copyright (C) 2000 Nortel Networks
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsconsts.h (2001).
 */

#ifndef NS3_STRATUM_CONSTANTS_H
#define NS3_STRATUM_CONSTANTS_H

#include <cstdint>

namespace ns3::stratum
{

/** Maximum physical queues per device (I-4.1). */
static constexpr int kMaxQueues = 9;

/** Maximum drop-precedence levels per physical queue (I-4.2). */
static constexpr int kMaxPrec = 3;

/** Maximum DSCP code points (6-bit field -> 64 values). */
static constexpr int kMaxCodePoints = 64;

/** Default mean packet size for RED calculations (bytes). */
static constexpr int kMeanPacketSize = 1000;

/**
 * @ingroup stratum
 * @brief Packet colour as decided by a meter/policer.
 */
enum class Colour : uint8_t
{
    GREEN = 0,
    YELLOW = 1,
    RED = 2
};

/**
 * @ingroup stratum
 * @brief Meter algorithm type.
 */
enum class MeterType : uint8_t
{
    DUMB,
    TOKEN_BUCKET,
    SRTCM,
    TRTCM,
    TSW2CM,
    TSW3CM,
    FAIR_WEIGHTED
};

/**
 * @ingroup stratum
 * @brief Policer type (determines how colour maps to DSCP).
 */
enum class PolicerType : uint8_t
{
    DUMB,
    TOKEN_BUCKET,
    SRTCM,
    TRTCM,
    TSW2CM,
    TSW3CM,
    FAIR_WEIGHTED
};

/**
 * @ingroup stratum
 * @brief Multi-RED mode (determines how virtual queue averages are computed).
 *
 * @see src/ns-2.29/diffserv/dsredq.h
 */
enum class MredMode : uint8_t
{
    RIO_C,    //!< Coupled: virtual queue avg includes all lower-precedence queues
    RIO_D,    //!< Decoupled: each virtual queue maintains its own avg independently
    WRED,     //!< Weighted RED: single avg over entire physical queue, shared by all prec levels
    DROP_TAIL //!< Simple drop-tail (no RED)
};

/**
 * @ingroup stratum
 * @brief Result of a sub-queue enqueue attempt.
 */
enum class PktResult : uint8_t
{
    PKT_DROPPED = 0,  //!< Buffer overflow (tail drop)
    PKT_ENQUEUED = 1, //!< Successfully enqueued
    PKT_EDROPPED = 2, //!< Early drop (RED probabilistic drop)
    PKT_MARKED = 3    //!< ECN marked (enqueued with CE bit set)
};

/**
 * @ingroup stratum
 * @brief PHB table entry: maps a DSCP code point to a (queue, precedence) pair.
 */
struct PhbEntry
{
    uint8_t codePt; //!< DSCP code point
    uint8_t queue;  //!< Physical queue index [0, kMaxQueues)
    uint8_t prec;   //!< Drop precedence / virtual queue index [0, kMaxPrec)
};

/** Maximum mark rules per edge router. */
static constexpr int kMaxMarkRules = 20;

/** Maximum policy table entries. */
static constexpr int kMaxPolicies = 20;

/** Sentinel: match any source/destination address. */
static constexpr int32_t kAnyHost = -1;

/** Sentinel: match any IP protocol (0 = wildcard). */
static constexpr uint8_t kAnyProtocol = 0;

// ---- Application-type identifiers for AppTypeTag ----
// These are opaque uint32_t values used with AppTypeTag
// and MarkRule::appType for application-based classification.
// Port-based classification (AddMarkRuleWithPorts) is preferred
// in ns-3 scenarios; these constants exist for scenarios that
// require application-layer tagging.

/** Application type: Telnet (interactive TCP). */
static constexpr uint32_t kAppTypeTelnet = 1;

/** Application type: FTP (bulk TCP transfer). */
static constexpr uint32_t kAppTypeFtp = 2;

} // namespace ns3::stratum

#endif // NS3_STRATUM_CONSTANTS_H
