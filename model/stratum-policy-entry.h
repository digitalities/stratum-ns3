/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * Copyright (C) 2000 Nortel Networks
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsPolicy.h policyTableEntry / policerTableEntry
 * (2001).
 */

#ifndef NS3_STRATUM_POLICY_ENTRY_H
#define NS3_STRATUM_POLICY_ENTRY_H

#include "stratum-constants.h"

#include <cstdint>

namespace ns3::stratum
{

/**
 * @ingroup stratum
 *
 * @brief Per-flow metering state.
 *
 * Holds all runtime state for a single policy table entry. The meter
 * algorithms operate on this struct; the struct itself is passive data.
 * Field-for-field translation of the ns-2 policyTableEntry.
 *
 */
struct PolicyEntry
{
    // ---- Flow identification ------------------------------------------------
    int32_t sourceNode{kAnyHost};           //!< Source node id (kAnyHost = wildcard)
    int32_t destNode{kAnyHost};             //!< Destination node id (kAnyHost = wildcard)
    uint32_t policyIndex{0};                //!< Policy table index for joining with PolicerEntry
    PolicerType policer{PolicerType::DUMB}; //!< Policer algorithm selector
    MeterType meter{MeterType::DUMB};       //!< Meter algorithm selector
    uint8_t codePoint{0};                   //!< In-profile DSCP

    // ---- Rate parameters (bytes per second) ---------------------------------
    double cir{0.0}; //!< Committed information rate
    double pir{0.0}; //!< Peak information rate (trTCM only)

    // ---- Bucket sizes (bytes) -----------------------------------------------
    double cbs{0.0}; //!< Committed burst size
    double ebs{0.0}; //!< Excess burst size (srTCM only)
    double pbs{0.0}; //!< Peak burst size (trTCM only)

    // ---- Bucket levels (bytes, runtime state) -------------------------------
    double cBucket{0.0}; //!< Current committed bucket level
    double eBucket{0.0}; //!< Current excess bucket level (srTCM)
    double pBucket{0.0}; //!< Current peak bucket level (trTCM)

    // ---- Timing -------------------------------------------------------------
    double arrivalTime{0.0}; //!< Last packet arrival (seconds)

    // ---- TSW state ----------------------------------------------------------
    double avgRate{0.0}; //!< EWMA rate estimate
    double winLen{1.0};  //!< TSW window length (seconds)
};

/**
 * @ingroup stratum
 *
 * @brief Policer table entry -- maps colour decisions to DSCP code points.
 *
 */
struct PolicerEntry
{
    PolicerType policer{PolicerType::DUMB}; //!< Policer algorithm selector
    int initialCodePt{0};                   //!< GREEN -> this DSCP
    int downgrade1{0};                      //!< YELLOW (or out-of-profile) -> this DSCP
    int downgrade2{0};                      //!< RED -> this DSCP
    uint32_t policyIndex{0};                //!< Policy table index for joining with PolicyEntry
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_POLICY_ENTRY_H
