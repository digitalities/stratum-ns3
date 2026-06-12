/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef NS3_STRATUM_REGISTRY_H
#define NS3_STRATUM_REGISTRY_H

#include <algorithm>
#include <functional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace ns3::stratum
{

/**
 * @brief Type-erased substrate registry: stores a vector of domain entries,
 *        provides lookup-by-tag and grouping-by-family, and a JSON dump
 *        driven by a domain-supplied serialiser.
 *
 * @tparam EntryT The domain-specific entry struct. The contract:
 *
 *   - Public field `std::string fileTag` — unique within the registry,
 *     used as the lookup key by Find().
 *   - Public field `displayName` (any string-convertible type) — human-
 *     readable name; not used by the template directly.
 *   - Public field `family` (any equality-comparable type) — coarse group
 *     label used by ByFamily(). May be a string or an enum.
 *
 *   The contract is not enforced at compile time (no static_assert,
 *   no concepts) so that domain entries can carry whatever additional
 *   fields they need (factory closures, ECN-support flags, parameter-
 *   shape enums) without the template prescribing their shape.
 *
 * Storage is append-only. Entries keep their registration order (used
 * by JSON dump and by examples binaries that print catalogues).
 *
 * Lifecycle: each domain registry uses the Meyers-singleton pattern
 * (`static const Foo kInstance` inside `Foo::Get()`) and seeds itself
 * via `Register()` calls in its constructor. The template does not
 * impose a singleton — that is a domain decision.
 *
 * Thread-safety: not thread-safe. ns-3 simulations are single-threaded.
 */
template <typename EntryT>
class Registry
{
  public:
    /// Append a new entry. Caller owns uniqueness of `fileTag`.
    void Register(EntryT entry)
    {
        m_entries.push_back(std::move(entry));
    }

    /// All registered entries, in registration order.
    const std::vector<EntryT>& All() const
    {
        return m_entries;
    }

    /// Number of registered entries.
    std::size_t Size() const
    {
        return m_entries.size();
    }

    /// Find an entry by its `fileTag`. Returns nullptr if not found.
    const EntryT* Find(const std::string& fileTag) const
    {
        auto it = std::find_if(m_entries.begin(), m_entries.end(), [&](const EntryT& e) {
            return e.fileTag == fileTag;
        });
        return (it == m_entries.end()) ? nullptr : &(*it);
    }

    /// All entries whose `family` equals `f`. Templated on the family
    /// type so the same primitive serves enum-valued families (e.g. AQM)
    /// and string-valued families (e.g. future SchedulerRegistry).
    template <typename FamilyT>
    std::vector<const EntryT*> ByFamily(const FamilyT& f) const
    {
        std::vector<const EntryT*> out;
        for (const auto& e : m_entries)
        {
            if (e.family == f)
            {
                out.push_back(&e);
            }
        }
        return out;
    }

    /// Signature of a domain-specific serialiser: writes one entry to
    /// `os` as a JSON object (no trailing comma — DumpManifest manages
    /// commas, brackets, and the surrounding object).
    using SerializeFn = std::function<void(std::ostream& os, const EntryT& e)>;

    /// Dump the registry to `os` as a JSON object of the form
    /// `{ "<arrayKey>": [ {entry}, {entry}, ... ] }`. The domain owns
    /// per-entry serialisation since the fields it cares about are
    /// domain-specific.
    void DumpManifest(std::ostream& os, const std::string& arrayKey, const SerializeFn& fn) const
    {
        os << "{\n  \"" << arrayKey << "\": [\n";
        for (std::size_t i = 0; i < m_entries.size(); ++i)
        {
            os << "    ";
            fn(os, m_entries[i]);
            if (i + 1 < m_entries.size())
            {
                os << ",";
            }
            os << "\n";
        }
        os << "  ]\n}\n";
    }

  protected:
    std::vector<EntryT> m_entries;
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_REGISTRY_H
