/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef NS3_STRATUM_APP_TYPE_TAG_H
#define NS3_STRATUM_APP_TYPE_TAG_H

#include "ns3/tag.h"

#include <ostream>

namespace ns3::stratum
{

/**
 * Sentinel value meaning "match any application type".
 * @see AppTypeTag
 */
static constexpr uint32_t kAnyAppType = 0;

/**
 * @ingroup stratum
 *
 * @brief Packet tag carrying an application-type identifier for policy
 * classification.
 *
 * AppTypeTag allows traffic generators to stamp packets with an
 * opaque application-type identifier so that a diffserv::PolicyClassifier can
 * match flows by application type without inspecting port numbers or payload.
 * The sentinel value kAnyAppType (0) means "match any application type" when
 * used in a classifier filter entry.
 *
 */
class AppTypeTag : public Tag
{
  public:
    /**
     * @brief Get the TypeId for this class.
     * @return the object TypeId
     */
    static TypeId GetTypeId();

    TypeId GetInstanceTypeId() const override;

    /** @brief Default constructor; sets app type to kAnyAppType (0). */
    AppTypeTag();

    /**
     * @brief Construct a AppTypeTag with the given application-type
     * identifier.
     * @param appType opaque application-type identifier
     */
    explicit AppTypeTag(uint32_t appType);

    /**
     * @brief Set the application-type identifier.
     * @param appType opaque application-type identifier
     */
    void SetAppType(uint32_t appType);

    /**
     * @brief Get the application-type identifier.
     * @return the application-type identifier
     */
    uint32_t GetAppType() const;

    // Tag serialization overrides
    uint32_t GetSerializedSize() const override;
    void Serialize(TagBuffer i) const override;
    void Deserialize(TagBuffer i) override;
    void Print(std::ostream& os) const override;

  private:
    uint32_t m_appType; //!< Opaque application-type identifier
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_APP_TYPE_TAG_H
