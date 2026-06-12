/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Per-packet enqueue-time tag attached by QueueDisc on both
 * sub-queues. Read at enqueue (head packet) to compute true sojourn
 * time for the RFC 9332 App. A.1 P.I controller and the immediate-mark
 * step.
 */

#ifndef NS3_STRATUM_L4S_TIMESTAMP_TAG_H
#define NS3_STRATUM_L4S_TIMESTAMP_TAG_H

#include "ns3/nstime.h"
#include "ns3/tag-buffer.h"
#include "ns3/tag.h"
#include "ns3/type-id.h"

namespace ns3::stratum::l4s
{

/**
 * @ingroup stratum
 * @brief L4S-specific enqueue timestamp.
 *
 * A dedicated tag class (rather than reusing ns-3's generic
 * @link ns3::TimestampTag @endlink) avoids collisions with other modules
 * that may already attach a `ns3::TimestampTag` for unrelated purposes.
 */
class TimestampTag : public Tag
{
  public:
    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;

    TimestampTag();
    explicit TimestampTag(Time t);

    void Serialize(TagBuffer i) const override;
    void Deserialize(TagBuffer i) override;
    uint32_t GetSerializedSize() const override;
    void Print(std::ostream& os) const override;

    Time GetTimestamp() const;
    void SetTimestamp(Time t);

  private:
    Time m_timestamp{0};
};

} // namespace ns3::stratum::l4s

#endif // NS3_STRATUM_L4S_TIMESTAMP_TAG_H
