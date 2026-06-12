/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Maps to ns-2 hdr_cmn::sendtime_ field (see docs/NS2_PATCHES.md §1).
 */

#ifndef NS3_STRATUM_SEND_TIME_TAG_H
#define NS3_STRATUM_SEND_TIME_TAG_H

#include "ns3/tag.h"

#include <ostream>

namespace ns3::stratum
{

/**
 * @ingroup stratum
 *
 * @brief Packet tag carrying the simulation time at which the packet was
 * created.
 *
 * Attached by the sending application (or a helper callback) to record
 * Simulator::Now() at packet creation time. Read by the receiving side to
 * compute one-way delay (OWD = recvTime - sendTime) and IP delay variation
 * (IPDV = |OWD_i - OWD_{i-1}|).
 *
 * This is the ns-3 equivalent of the sendtime_ field that DiffServ4NS added
 * to ns-2's hdr_cmn (documented in docs/NS2_PATCHES.md §1).
 *
 */
class SendTimeTag : public Tag
{
  public:
    /**
     * @brief Get the TypeId for this class.
     * @return the TypeId.
     */
    static TypeId GetTypeId();

    TypeId GetInstanceTypeId() const override;

    /** @brief Default constructor; sets send time to 0. */
    SendTimeTag();

    /**
     * @brief Construct with a specific send time.
     * @param sendTimeSeconds simulation time in seconds
     */
    explicit SendTimeTag(double sendTimeSeconds);

    /**
     * @brief Set the send time.
     * @param sendTimeSeconds simulation time in seconds
     */
    void SetSendTime(double sendTimeSeconds);

    /**
     * @brief Get the send time.
     * @return simulation time in seconds at which the packet was created
     */
    double GetSendTime() const;

    // Tag serialization overrides
    uint32_t GetSerializedSize() const override;
    void Serialize(TagBuffer i) const override;
    void Deserialize(TagBuffer i) override;
    void Print(std::ostream& os) const override;

  private:
    double m_sendTime; //!< Simulation time in seconds at packet creation
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_SEND_TIME_TAG_H
