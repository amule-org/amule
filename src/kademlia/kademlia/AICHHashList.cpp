//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002-2011 Merkur ( devs@emule-project.net / http://www.emule-project.net )
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

#include "AICHHashList.h"

#include <protocol/kad2/Constants.h> // Needed for KADEMLIA_VERSION9_50a

#include <algorithm>

namespace Kademlia
{

uint16_t CKadAICHHashList::AddReference(const CKadAICHHash &hash)
{
	for (size_t i = 0; i < m_hashes.size(); ++i) {
		if (m_hashes[i] == hash) {
			if (m_popularity[i] < 0xFF) {
				m_popularity[i]++;
			}
			return (uint16_t)i;
		}
	}

	m_hashes.push_back(hash);
	m_popularity.push_back(1);
	return (uint16_t)(m_hashes.size() - 1);
}

void CKadAICHHashList::DropReferenceAt(uint16_t index)
{
	if (index >= m_hashes.size()) {
		return;
	}
	if (m_popularity[index] > 0) {
		m_popularity[index]--;
	}
}

uint16_t CKadAICHHashList::GetReferencedCount() const
{
	uint16_t count = 0;
	for (const uint16_t popularity : m_popularity) {
		if (popularity > 0) {
			count++;
		}
	}
	return count;
}

uint8_t CKadAICHHashList::GetPopularityAt(uint16_t index) const
{
	return (index < m_popularity.size()) ? m_popularity[index] : 0;
}

const CKadAICHHash &CKadAICHHashList::GetHashAt(uint16_t index) const
{
	// A zeroed hash for an out-of-range index keeps this a total function:
	// the callers are packet and file parsers, where an index that failed
	// validation must not turn into undefined behaviour.
	static const CKadAICHHash s_empty = CKadAICHHash();
	return (index < m_hashes.size()) ? m_hashes[index] : s_empty;
}

std::vector<uint16_t> CKadAICHHashList::BuildCompactionMap() const
{
	// uint16_t(...) rather than INVALID_INDEX: the fill constructor takes a
	// const reference, which would ODR-use the member and need an
	// out-of-line definition -- ill-formed for a constexpr member in C++17.
	std::vector<uint16_t> map(m_hashes.size(), uint16_t(INVALID_INDEX));
	uint16_t next = 0;
	for (size_t i = 0; i < m_hashes.size(); ++i) {
		if (m_popularity[i] > 0) {
			map[i] = next++;
		}
	}
	return map;
}

std::vector<uint8_t> CKadAICHHashList::EncodeResultTag() const
{
	std::vector<uint8_t> encoded;

	uint8_t count = 0;
	for (size_t i = 0; i < m_hashes.size() && count < MAX_RESULT_HASHES; ++i) {
		if (m_popularity[i] > 0) {
			count++;
		}
	}
	if (count == 0) {
		return encoded;
	}

	encoded.reserve(1 + (1 + KAD_AICH_HASH_SIZE) * count);
	encoded.push_back(count);
	uint8_t written = 0;
	for (size_t i = 0; i < m_hashes.size() && written < count; ++i) {
		if (m_popularity[i] == 0) {
			continue;
		}
		encoded.push_back(m_popularity[i]);
		encoded.insert(encoded.end(), m_hashes[i].begin(), m_hashes[i].end());
		written++;
	}
	return encoded;
}

bool CKadAICHHashList::DecodeResultTag(const uint8_t *data, size_t length, std::vector<SResultHash> &out)
{
	out.clear();
	if (data == nullptr || length < 1) {
		return false;
	}

	const uint8_t count = data[0];
	if (count > MAX_RESULT_HASHES) {
		return false;
	}
	if (length < 1 + (size_t)count * (1 + KAD_AICH_HASH_SIZE)) {
		return false;
	}

	size_t pos = 1;
	for (uint8_t i = 0; i < count; ++i) {
		SResultHash entry;
		entry.m_popularity = data[pos++];
		std::copy(data + pos, data + pos + KAD_AICH_HASH_SIZE, entry.m_hash.begin());
		pos += KAD_AICH_HASH_SIZE;
		// A hash nobody publishes carries no information, and is what a
		// peer that has just evicted its publishers would emit.
		if (entry.m_popularity > 0) {
			out.push_back(entry);
		}
	}
	return true;
}

bool CKadAICHHashList::PeerSupportsAICHKeywordStorage(uint8_t peerKadVersion)
{
	return peerKadVersion >= KADEMLIA_VERSION9_50a;
}

const CKadAICHHashList::SResultHash *CKadAICHHashList::GetMostPopular(const std::vector<SResultHash> &hashes)
{
	const SResultHash *best = nullptr;
	for (const SResultHash &hash : hashes) {
		if (best == nullptr || hash.m_popularity > best->m_popularity) {
			best = &hash;
		}
	}
	return best;
}

} // namespace Kademlia
