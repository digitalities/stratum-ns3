/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef NS3_STRATUM_DSCP_TAG_H
#define NS3_STRATUM_DSCP_TAG_H

#include "ns3/tag.h"

#include <ostream>

namespace ns3::stratum
{

/**
 * @ingroup stratum
 *
 * @brief Packet tag carrying a 6-bit DSCP codepoint.
 *
 * DscpTag is attached to a packet by the edge classifier or by a
 * meter to record the DSCP value that should be written into the IPv4 ToS
 * field before forwarding. Using a tag rather than modifying the header
 * directly defers header writes until the packet leaves the DiffServ
 * domain, keeping the inner simulation pristine for analysis.
 *
 * The 6-bit DSCP value is constrained to [0, 63].
 *
 */
class DscpTag : public Tag
{
  public:
    /**
     * @brief Get the TypeId for this class.
     * @return the object TypeId
     */
    static TypeId GetTypeId();

    TypeId GetInstanceTypeId() const override;

    /** @brief Default constructor; sets DSCP to 0. */
    DscpTag();

    /**
     * @brief Construct a DscpTag with the given DSCP value.
     * @param dscp 6-bit DSCP codepoint [0, 63]
     */
    explicit DscpTag(uint8_t dscp);

    /**
     * @brief Set the DSCP codepoint.
     * @param dscp 6-bit DSCP value [0, 63]
     */
    void SetDscp(uint8_t dscp);

    /**
     * @brief Get the DSCP codepoint.
     * @return the 6-bit DSCP value
     */
    uint8_t GetDscp() const;

    // Tag serialization overrides
    uint32_t GetSerializedSize() const override;
    void Serialize(TagBuffer i) const override;
    void Deserialize(TagBuffer i) override;
    void Print(std::ostream& os) const override;

  private:
    uint8_t m_dscp; //!< 6-bit DSCP codepoint [0, 63]
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_DSCP_TAG_H
