// This file is part of the aMule Project.
// Copyright (c) 2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU General Public License, version 2 or later.

#ifndef CANONICALPEERINDEX_H
#define CANONICALPEERINDEX_H

#include "PeerAddressing.h"

#include <map>

/**
 * Address index shared by production and standalone tests.
 *
 * Reference is stored by value: CClientList uses CClientRef, retaining its existing
 * ownership. Identity predicates select one client within a shared address bucket;
 * port/hash policy stays with the caller. No mutable map access can bypass address
 * normalization or insert an absent address. Iterators have std::multimap stability.
 */
template <typename Reference> class CCanonicalPeerIndex
{
	using Map = std::multimap<CNetworkAddress, Reference>;

public:
	using const_iterator = typename Map::const_iterator;
	using Range = std::pair<const_iterator, const_iterator>;

	void Insert(const CNetworkAddress &address, const Reference &reference)
	{
		if (PeerAddressing::IsIndexable(address)) {
			m_entries.emplace(PeerAddressing::IndexKey(address), reference);
		}
	}

	Range equal_range(const CNetworkAddress &address) const
	{
		return m_entries.equal_range(PeerAddressing::IndexKey(address));
	}

	template <typename Matches> bool Remove(const CNetworkAddress &address, Matches matches)
	{
		auto range = equal_range(address);
		for (auto it = range.first; it != range.second; ++it) {
			if (matches(it->second)) {
				m_entries.erase(it);
				return true;
			}
		}
		return false;
	}

	// Called before the owner changes its address. Keep a reference alive across
	// removal, even when this index is its only owner.
	template <typename Matches>
	void Update(const CNetworkAddress &oldAddress,
		const CNetworkAddress &newAddress,
		Reference reference,
		Matches matches)
	{
		if (PeerAddressing::IndexKey(oldAddress) == PeerAddressing::IndexKey(newAddress)) {
			return;
		}
		Remove(oldAddress, matches);
		Insert(newAddress, reference);
	}

	const_iterator begin() const { return m_entries.begin(); }
	const_iterator end() const { return m_entries.end(); }
	const_iterator lower_bound(const CNetworkAddress &address) const
	{
		return m_entries.lower_bound(PeerAddressing::IndexKey(address));
	}
	typename Map::size_type count(const CNetworkAddress &address) const
	{
		return m_entries.count(PeerAddressing::IndexKey(address));
	}
	typename Map::size_type size() const { return m_entries.size(); }
	void clear() { m_entries.clear(); }

private:
	Map m_entries;
};

#endif // CANONICALPEERINDEX_H
