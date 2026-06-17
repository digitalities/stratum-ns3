/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * Copyright (C) 2000 Nortel Networks
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ported from DiffServ4NS dsPolicy.{h,cc} class PolicyClassifier (2001).
 */

#include "stratum-policy-classifier.h"

#include "stratum-edge-meter-provider.h"
#include "stratum-fw-meter.h"
#include "stratum-meter.h"

#include "ns3/log.h"

#include <cstdio>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::diffserv::PolicyClassifier");

} // namespace ns3

namespace ns3::stratum::diffserv
{

NS_OBJECT_ENSURE_REGISTERED(PolicyClassifier);

TypeId
PolicyClassifier::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::diffserv::PolicyClassifier")
                            .SetParent<Object>()
                            .SetGroupName("Stratum")
                            .AddConstructor<PolicyClassifier>();
    return tid;
}

PolicyClassifier::PolicyClassifier()
    : m_policyTableSize(0),
      m_policerTableSize(0),
      m_meterProvider(nullptr)
{
}

PolicyClassifier::~PolicyClassifier()
{
}

void
PolicyClassifier::SetMeterProvider(EdgeMeterProvider* provider)
{
    m_meterProvider = provider;
}

void
PolicyClassifier::AddPolicyEntry(const PolicyEntry& entry)
{
    NS_ASSERT_MSG(m_policyTableSize < kMaxPolicies, "Policy table full");
    m_policyTable[m_policyTableSize++] = entry;
}

void
PolicyClassifier::AddPolicerEntry(const PolicerEntry& entry)
{
    NS_ASSERT_MSG(m_policerTableSize < kMaxCodePoints, "Policer table full");
    m_policerTable[m_policerTableSize++] = entry;
}

uint8_t
PolicyClassifier::ApplyPolicy(uint8_t codePt, uint32_t packetSize, double nowSeconds)
{
    // Step 1: find the policy entry for this code point
    PolicyEntry* policy = GetPolicyEntry(codePt);
    if (!policy)
    {
        NS_LOG_DEBUG("No policy entry for code point " << static_cast<int>(codePt)
                                                       << "; passing through");
        return codePt;
    }

    // Step 2: find the policer entry for this policy
    PolicerEntry* policer = GetPolicerEntry(policy->policyIndex, static_cast<int>(codePt));
    if (!policer)
    {
        NS_LOG_DEBUG("No policer entry for policyIndex=" << policy->policyIndex
                                                         << " codePt=" << static_cast<int>(codePt)
                                                         << "; passing through");
        return codePt;
    }

    // Step 3: resolve the meter algorithm via the injected provider
    if (!m_meterProvider)
    {
        NS_LOG_ERROR("No EdgeMeterProvider installed on classifier; "
                     "passing through. SetMeterProvider(...) must be called "
                     "before ApplyPolicy (the owning edge disc does this "
                     "automatically).");
        return codePt;
    }
    Ptr<Meter> meter = m_meterProvider->GetMeter(policy->meter);
    if (!meter)
    {
        NS_LOG_ERROR("Meter type " << static_cast<int>(static_cast<uint8_t>(policy->meter))
                                   << " not supported by provider; passing through");
        return codePt;
    }

    // Step 4: update bucket state
    meter->ApplyMeter(*policy, nowSeconds, packetSize);

    // FWMeter encodes its remark decision — including the deterministic /
    // probabilistic / periodic penalty modes selected by
    // PolicerEntry::downgrade2 — as a DSCP code point directly. The colour
    // interface cannot carry the mode, so route it through the faithful
    // per-flow path (ns-2 FWPolicy::applyPolicer) instead of the colour map.
    if (Ptr<FWMeter> fw = DynamicCast<FWMeter>(meter))
    {
        return static_cast<uint8_t>(fw->ApplyPolicerFw(*policy, *policer, packetSize, 0));
    }

    // Step 5: decide colour
    Colour colour = meter->ApplyPolicer(*policy, packetSize);

    // Step 6: map colour to DSCP
    int remarkedCp;
    switch (colour)
    {
    case Colour::GREEN:
        remarkedCp = policer->initialCodePt;
        break;
    case Colour::YELLOW:
        remarkedCp = policer->downgrade1;
        break;
    case Colour::RED:
    default:
        remarkedCp = policer->downgrade2;
        break;
    }

    // Step 7: return re-marked DSCP
    return static_cast<uint8_t>(remarkedCp);
}

void
PolicyClassifier::PrintPolicyTable() const
{
    std::printf("Policy Table (%d entries):\n", m_policyTableSize);
    std::printf(" Idx  CodePt  PolicyIdx  Meter  Policer      CIR        CBS\n");
    std::printf(" ---  ------  ---------  -----  -------  ---------  ---------\n");
    for (int i = 0; i < m_policyTableSize; ++i)
    {
        const PolicyEntry& e = m_policyTable[i];
        std::printf("%4d  %6d  %9u  %5d  %7d  %9.1f  %9.1f\n",
                    i,
                    static_cast<int>(e.codePoint),
                    e.policyIndex,
                    static_cast<int>(static_cast<uint8_t>(e.meter)),
                    static_cast<int>(static_cast<uint8_t>(e.policer)),
                    e.cir,
                    e.cbs);
    }
    std::printf("\n");
}

std::set<MeterType>
PolicyClassifier::GetUsedMeterTypes() const
{
    std::set<MeterType> types;
    for (int i = 0; i < m_policyTableSize; ++i)
    {
        types.insert(m_policyTable[i].meter);
    }
    return types;
}

void
PolicyClassifier::PrintPolicerTable() const
{
    std::printf("Policer Table (%d entries):\n", m_policerTableSize);
    std::printf(" Idx  PolicyIdx  InitCP  Down1  Down2\n");
    std::printf(" ---  ---------  ------  -----  -----\n");
    for (int i = 0; i < m_policerTableSize; ++i)
    {
        const PolicerEntry& e = m_policerTable[i];
        std::printf("%4d  %9u  %6d  %5d  %5d\n",
                    i,
                    e.policyIndex,
                    e.initialCodePt,
                    e.downgrade1,
                    e.downgrade2);
    }
    std::printf("\n");
}

PolicyEntry*
PolicyClassifier::GetPolicyEntry(uint8_t codePt)
{
    for (int i = 0; i < m_policyTableSize; ++i)
    {
        if (m_policyTable[i].codePoint == codePt)
        {
            return &m_policyTable[i];
        }
    }
    return nullptr;
}

PolicerEntry*
PolicyClassifier::GetPolicerEntry(uint32_t policyIndex, int initialCodePt)
{
    for (int i = 0; i < m_policerTableSize; ++i)
    {
        if (m_policerTable[i].policyIndex == policyIndex &&
            m_policerTable[i].initialCodePt == initialCodePt)
        {
            return &m_policerTable[i];
        }
    }
    return nullptr;
}

} // namespace ns3::stratum::diffserv
