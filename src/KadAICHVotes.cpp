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

#include "KadAICHVotes.h"
#include "CryptoPP_Inc.h"
#include "WarningsPush_CryptoPP.h"
#include CRYPTO_HEADER(hmac.h)
#include "WarningsPop.h"
#include "Logger.h"
#include <algorithm>
#include <tuple>
#include <utility>

CKadAICHVotes::Key CKadAICHVotes::GenerateKey()
{
	CryptoPP::AutoSeededRandomPool random;
	Key key;
	random.GenerateBlock(key.data(), key.size());
	return key;
}

CKadAICHVotes::Key CKadAICHVotes::Rank(uint32_t subnet) const
{
	// Fixed encoding makes sampling independent of the host's byte order.
	const uint8_t bytes[] = { static_cast<uint8_t>(subnet),
		static_cast<uint8_t>(subnet >> 8),
		static_cast<uint8_t>(subnet >> 16),
		static_cast<uint8_t>(subnet >> 24) };
	CryptoPP::HMAC<CryptoPP::SHA256> hash(m_key.data(), m_key.size());
	Key rank;
	hash.CalculateDigest(rank.data(), bytes, sizeof(bytes));
	return rank;
}

void CKadAICHVotes::Admit(uint32_t subnet, const Entry &entry)
{
	auto existing = m_entries.find(subnet);
	if (existing != m_entries.end()) {
		if (entry.conflicting || existing->second.root != entry.root) {
			existing->second.conflicting = true;
			// Canonical tombstone: neither root wins through arrival order.
			existing->second.root = CAICHHash();
		}
		return;
	}
	if (m_entries.size() >= kMaxWitnesses) {
		auto lowerRank = [](const auto &a, const auto &b) {
			return std::tie(a.second.rank, a.first) < std::tie(b.second.rank, b.first);
		};
		auto worst = std::max_element(m_entries.begin(), m_entries.end(), lowerRank);
		if (std::tie(entry.rank, subnet) >= std::tie(worst->second.rank, worst->first)) {
			return;
		}
		auto replacement = m_entries.extract(worst);
		replacement.key() = subnet;
		replacement.mapped() = entry;
		m_entries.insert(std::move(replacement));
		return;
	}
	m_entries.emplace(subnet, entry);
}

void CKadAICHVotes::Add(uint32_t responder, const CAICHHash &root)
{
	if (responder != 0) {
		const uint32_t subnet = CAICHUntrustedHash::SigningSubnet(responder);
		Admit(subnet, Entry{ Rank(subnet), root, false });
	}
}

void CKadAICHVotes::Merge(const CKadAICHVotes &other)
{
	// All rows merged by the current search-result path share one search ID
	// and its key, so a mismatch is not expected in normal operation. Keep
	// this defensive check in case that invariant changes: re-ranking a
	// truncated sample under another key could lose eligible witnesses.
	// Skip the merge and log it rather than aborting the UDP callback.
	if (m_key != other.m_key) {
		AddDebugLogLineC(
			logKadSearch, "Kad AICH evidence belongs to a different search key; skipping merge");
		return;
	}
	for (const auto &entry : other.m_entries) {
		Admit(entry.first, entry.second);
	}
}

std::map<uint32_t, CAICHHash> CKadAICHVotes::Get() const
{
	std::map<uint32_t, CAICHHash> votes;
	for (const auto &entry : m_entries) {
		if (!entry.second.conflicting) {
			votes.emplace(entry.first, entry.second.root);
		}
	}
	return votes;
}
