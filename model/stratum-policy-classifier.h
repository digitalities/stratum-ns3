/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * Copyright (C) 2000 Nortel Networks
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsPolicy.{h,cc} class PolicyClassifier (2001).
 */

#ifndef NS3_STRATUM_POLICY_CLASSIFIER_H
#define NS3_STRATUM_POLICY_CLASSIFIER_H

#include "stratum-constants.h"
#include "stratum-policy-entry.h"

#include "ns3/object.h"
#include "ns3/ptr.h"

#include <set>

namespace ns3::stratum
{
class EdgeMeterProvider;
} // namespace ns3::stratum

namespace ns3::stratum::diffserv
{

/**
 * @ingroup stratum
 *
 * @brief DiffServ policy classifier: maps DSCP code points to metered/policed DSCPs.
 *
 * Maintains a policy table (per-DSCP metering parameters + bucket state)
 * and a policer table (colour-to-DSCP mappings). On each packet, it:
 * 1. Looks up the policy entry for the incoming DSCP.
 * 2. Looks up the policer entry for the policy.
 * 3. Resolves the Meter algorithm via the injected EdgeMeterProvider.
 * 4. Runs the meter algorithm to update bucket state in the entry.
 * 5. Queries the policer for a colour decision.
 * 6. Returns the re-marked DSCP.
 *
 * The classifier consults an EdgeMeterProvider (typically the
 * owning EdgeQueueDisc) for its Meter algorithm rather than
 * owning meters directly. Bucket state remains per-PolicyEntry
 * (per-DSCP).
 *
 * Ported from dsPolicy.{h,cc} class PolicyClassifier (DiffServ4NS 2001).
 *
 */
class PolicyClassifier : public Object
{
  public:
    /**
     * @brief Get the TypeId for PolicyClassifier.
     * @return the TypeId.
     */
    static TypeId GetTypeId();

    /** @brief Construct a PolicyClassifier with empty tables. */
    PolicyClassifier();

    ~PolicyClassifier() override;

    /**
     * @brief Install the meter-strategy lookup hook.
     *
     * Must be set before ApplyPolicy is called. The owning edge disc
     * wires itself as the provider at construction. The classifier does
     * not take ownership; the provider must outlive the classifier
     * (guaranteed in practice because the edge disc owns the classifier).
     *
     * @param provider meter-strategy lookup hook (raw pointer; non-owning)
     */
    void SetMeterProvider(EdgeMeterProvider* provider);

    /**
     * @brief Add a policy table entry (per-DSCP metering parameters + state).
     * @param entry policy entry to append
     */
    void AddPolicyEntry(const PolicyEntry& entry);

    /**
     * @brief Add a policer table entry (colour-to-DSCP mapping).
     * @param entry policer entry to append
     */
    void AddPolicerEntry(const PolicerEntry& entry);

    /**
     * @brief Apply metering and policing to a packet, returning the re-marked DSCP.
     *
     * @param codePt incoming DSCP code point
     * @param packetSize packet size in bytes
     * @param nowSeconds current simulation time in seconds
     * @return re-marked DSCP (or unchanged @p codePt if no policy found)
     */
    uint8_t ApplyPolicy(uint8_t codePt, uint32_t packetSize, double nowSeconds);

    /** @brief Print the policy table to stdout. */
    void PrintPolicyTable() const;

    /** @brief Print the policer table to stdout. */
    void PrintPolicerTable() const;

    /**
     * @brief Return the set of MeterType values referenced by any entry
     * in the policy table.
     *
     * Used by the owning edge disc at CheckConfig time to eagerly
     * materialise the corresponding meter slots, so that a later
     * edge->AssignStreams(N) call reaches them before lazy creation
     * silently binds them to the global default stream.
     *
     * @return set of MeterType values currently in use
     */
    std::set<MeterType> GetUsedMeterTypes() const;

  private:
    /**
     * @brief Look up the policy entry matching the given DSCP.
     * @param codePt DSCP code point to match
     * @return pointer to the matching policy entry, or nullptr
     */
    PolicyEntry* GetPolicyEntry(uint8_t codePt);

    /**
     * @brief Look up the policer entry matching policyIndex + initialCodePt.
     * @param policyIndex policy table index
     * @param initialCodePt initial (GREEN) DSCP stamped by the policy
     * @return pointer to the matching policer entry, or nullptr
     */
    PolicerEntry* GetPolicerEntry(uint32_t policyIndex, int initialCodePt);

    PolicyEntry m_policyTable[kMaxPolicies]; //!< Policy table entries
    int m_policyTableSize;                   //!< Number of valid policy entries

    PolicerEntry m_policerTable[kMaxCodePoints]; //!< Policer table entries
    int m_policerTableSize;                      //!< Number of valid policer entries

    EdgeMeterProvider* m_meterProvider; //!< Injected hook (non-owning); see SetMeterProvider
};

} // namespace ns3::stratum::diffserv

#endif // NS3_STRATUM_POLICY_CLASSIFIER_H
