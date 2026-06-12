/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * Copyright (C) 2000 Nortel Networks
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Standalone PHB classification table — first-class helper extracted
 * from RedQueueDisc, matching the thesis Figure 3.11 analogy.
 */

#ifndef NS3_STRATUM_PHB_TABLE_H
#define NS3_STRATUM_PHB_TABLE_H

#include "stratum-constants.h"

#include <cstdint>

namespace ns3::stratum
{

/**
 * @ingroup stratum
 *
 * @brief PHB table: DSCP code point -> (physical queue, drop precedence).
 *
 * Value type. Fixed-size storage of `kMaxCodePoints` entries (one per DSCP
 * value). Used by `RedQueueDisc` and, via forwarder, by `l4s::QueueDisc`.
 *
 */
class PhbTable
{
  public:
    /** @brief Construct an empty PhbTable. */
    PhbTable();

    /**
     * @brief Append a PHB table entry.
     *
     * No-op if the table is full. Duplicate code points are accepted
     * and will shadow earlier entries (first-match-wins in @c Lookup).
     *
     * @param codePt DSCP code point [0, kMaxCodePoints)
     * @param queue physical queue index [0, kMaxQueues)
     * @param prec drop precedence / virtual queue index [0, kMaxPrec)
     */
    void AddEntry(uint8_t codePt, uint8_t queue, uint8_t prec);

    /**
     * @brief First-match lookup by code point.
     *
     * @param codePt DSCP code point to look up
     * @param [out] queue matched physical queue index (unchanged on miss)
     * @param [out] prec matched drop precedence (unchanged on miss)
     * @return true on match, false otherwise
     */
    bool Lookup(uint8_t codePt, uint8_t& queue, uint8_t& prec) const;

    /**
     * @brief Number of entries currently stored.
     * @return the number of entries in the table
     */
    uint32_t Size() const;

    /**
     * @brief Remove all entries (capacity unchanged).
     */
    void Clear();

    /**
     * @brief Print the table to stdout (diagnostic only).
     */
    void Print() const;

  private:
    PhbEntry m_entries[kMaxCodePoints]; //!< Entry storage
    uint32_t m_numEntries;              //!< Active count
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_PHB_TABLE_H
