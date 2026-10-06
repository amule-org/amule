//								-*- C++ -*-
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// Any parts of this program derived from the xMule, lMule or eMule project,
// or contributed by third-party developers are copyrighted by their
// respective authors.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
//

#ifndef KADAICHVOTES_H
#define KADAICHVOTES_H

#include <array>
#include <map>
#include "SHAHashSet.h"

// Transient, reputation-free evidence. The kMaxWitnesses (64) slots represent
// consensus /20s. Rows that admit evidence use their search's secret key; a row
// without evidence takes the key of the first evidence merged into it.
// Retain the lowest (HMAC-SHA256(key, subnet), subnet) ranks, including conflict
// tombstones. Priorities never depend on roots, reply counts, or reputation.
// A discarded rank can never qualify later as the cutoff only decreases. Thus
// top-K(top-K(A) union top-K(B)) equals top-K(A union B), including tombstones.
// This bounds memory without losing conflict information for a final retained
// subnet. Merging evidence requires equal keys; copies preserve the key.
// Get() is consumed once at download construction, before any vote is counted.
// No key/evidence is persisted, and neither changes locally verified hashes.
// Sampling prevents arrival preference; it cannot defeat unlimited /20 Sybils.
class CKadAICHVotes
{
public:
	using Key = std::array<uint8_t, 32>;
	// Bounded number of retained consensus /20 witnesses. Admit and the bound
	// tests read this single source so the contract cannot drift.
	static constexpr size_t kMaxWitnesses = 64;
	static Key GenerateKey();
	// Empty/restored models use the default; network rows supply a generated key.
	explicit CKadAICHVotes(const Key &key = Key{})
	: m_key(key)
	{
	}

	void Add(uint32_t responder, const CAICHHash &root);
	void Merge(const CKadAICHVotes &other);

	// Contradictory subnets retain a slot but contribute no correctness vote.
	std::map<uint32_t, CAICHHash> Get() const;
	size_t GetSlotCount() const { return m_entries.size(); }

private:
	struct Entry
	{
		Key rank;
		CAICHHash root;
		bool conflicting;
	};
	Key Rank(uint32_t subnet) const;
	void Admit(uint32_t subnet, const Entry &entry);

	Key m_key;
	std::map<uint32_t, Entry> m_entries;
};

#endif
