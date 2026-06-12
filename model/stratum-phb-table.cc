/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * Copyright (C) 2000 Nortel Networks
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "stratum-phb-table.h"

#include "ns3/log.h"

#include <cstdio>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("PhbTable");

} // namespace ns3

namespace ns3::stratum
{

PhbTable::PhbTable()
    : m_numEntries(0)
{
    for (int i = 0; i < kMaxCodePoints; ++i)
    {
        m_entries[i].codePt = 0;
        m_entries[i].queue = 0;
        m_entries[i].prec = 0;
    }
}

void
PhbTable::AddEntry(uint8_t codePt, uint8_t queue, uint8_t prec)
{
    NS_LOG_FUNCTION(this << static_cast<uint32_t>(codePt) << static_cast<uint32_t>(queue)
                         << static_cast<uint32_t>(prec));

    if (m_numEntries >= kMaxCodePoints)
    {
        NS_LOG_ERROR("PHB Table size limit exceeded.");
        return;
    }
    m_entries[m_numEntries].codePt = codePt;
    m_entries[m_numEntries].queue = queue;
    m_entries[m_numEntries].prec = prec;
    ++m_numEntries;
}

bool
PhbTable::Lookup(uint8_t codePt, uint8_t& queue, uint8_t& prec) const
{
    for (uint32_t i = 0; i < m_numEntries; ++i)
    {
        if (m_entries[i].codePt == codePt)
        {
            queue = m_entries[i].queue;
            prec = m_entries[i].prec;
            return true;
        }
    }
    NS_LOG_WARN("No match found for code point " << static_cast<uint32_t>(codePt)
                                                 << " in PHB Table.");
    return false;
}

uint32_t
PhbTable::Size() const
{
    return m_numEntries;
}

void
PhbTable::Clear()
{
    m_numEntries = 0;
}

void
PhbTable::Print() const
{
    std::printf("PHB Table:\n");
    for (uint32_t i = 0; i < m_numEntries; ++i)
    {
        std::printf("Code Point %d is associated with Queue %d, Precedence %d\n",
                    m_entries[i].codePt,
                    m_entries[i].queue,
                    m_entries[i].prec);
    }
    std::printf("\n");
}

} // namespace ns3::stratum
