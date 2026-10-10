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

#ifndef AMULE_CONTACTDISTRIBUTION_H
#define AMULE_CONTACTDISTRIBUTION_H
#include <array>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <vector>
namespace Kademlia
{
enum class ContactDistributionState
{
	Loading,
	Available,
	Unsupported,
	Invalid
};
struct ContactDistribution
{
	static constexpr size_t BinCount = 4096; // twelve most significant KadID bits
	std::array<uint32_t, BinCount> contacts{};
	std::array<uint32_t, BinCount> verified{};
	uint32_t subnets = 0;
	uint32_t localID = 0; // most significant word of the local KadID
	bool hasLocalID = false;
	uint32_t Total() const
	{
		uint32_t sum = 0;
		for (auto n : contacts) {
			sum += n;
		}
		return sum;
	}
	uint32_t Verified() const
	{
		uint32_t sum = 0;
		for (auto n : verified) {
			sum += n;
		}
		return sum;
	}
	// EC v2: flags, local ID word, subnet count, entry count, then sparse
	// (uint16 bin, uint32 contacts, uint32 verified) entries, all big endian.
	using Wire = std::vector<uint8_t>;
	Wire Encode() const
	{
		Wire wire{ 2, static_cast<uint8_t>(hasLocalID) };
		auto put = [&](uint32_t n, int bytes) {
			for (int shift = (bytes - 1) * 8; shift >= 0; shift -= 8) {
				wire.push_back(static_cast<uint8_t>(n >> shift));
			}
		};
		put(localID, 4);
		put(subnets, 4);
		size_t entries = 0;
		for (size_t i = 0; i < BinCount; ++i) {
			if (contacts[i] || verified[i]) {
				++entries;
			}
		}
		wire.reserve(12 + entries * 10);
		put(entries, 2);
		for (size_t i = 0; i < BinCount; ++i) {
			if (contacts[i] || verified[i]) {
				put(i, 2);
				put(contacts[i], 4);
				put(verified[i], 4);
			}
		}
		return wire;
	}
	static bool Decode(const void *data, size_t size, ContactDistribution &out)
	{
		if (!data || size < 12) {
			return false;
		}
		const auto *wire = static_cast<const uint8_t *>(data);
		if (wire[0] != 2 || wire[1] > 1) {
			return false;
		}
		ContactDistribution decoded;
		decoded.hasLocalID = wire[1] != 0;
		size_t offset = 2;
		auto get = [&](int bytes) {
			uint32_t n = 0;
			for (int i = 0; i < bytes; ++i) {
				n = (n << 8) | wire[offset++];
			}
			return n;
		};
		decoded.localID = get(4);
		decoded.subnets = get(4);
		const size_t entries = get(2);
		if (entries > BinCount || size != 12 + entries * 10) {
			return false;
		}
		uint64_t total = 0;
		size_t previous = 0;
		for (size_t i = 0; i < entries; ++i) {
			const size_t bin = get(2);
			const uint32_t contacts = get(4), verified = get(4);
			if (bin >= BinCount || (i && bin <= previous) || !contacts || verified > contacts) {
				return false;
			}
			previous = bin;
			decoded.contacts[bin] = contacts;
			decoded.verified[bin] = verified;
			total += contacts;
		}
		if (total > std::numeric_limits<uint32_t>::max() || decoded.subnets > total) {
			return false;
		}
		out = decoded;
		return true;
	}
};
// Remote snapshot state follows the request, not the mere absence of a reply tag.
class ContactDistributionCache
{
public:
	void Update(bool requested, bool tagPresent, const void *data, size_t size)
	{
		if (!requested) {
			return;
		}
		m_data = {};
		if (!tagPresent) {
			m_state = ContactDistributionState::Unsupported;
		} else {
			m_state = ContactDistribution::Decode(data, size, m_data)
					  ? ContactDistributionState::Available
					  : ContactDistributionState::Invalid;
		}
	}
	void Reset()
	{
		m_data = {};
		m_state = ContactDistributionState::Loading;
	}
	ContactDistributionState Get(ContactDistribution &out) const
	{
		out = m_data;
		return m_state;
	}

private:
	ContactDistribution m_data;
	ContactDistributionState m_state = ContactDistributionState::Loading;
};

class ContactDistributionBuilder
{
public:
	void Add(uint32_t mostSignificantIDWord, bool verified)
	{
		const size_t bin = mostSignificantIDWord >> 20;
		++m_data.contacts[bin];
		if (verified) {
			++m_data.verified[bin];
		}
	}
	ContactDistribution Get(uint32_t subnets, uint32_t localID) const
	{
		auto data = m_data;
		data.subnets = subnets;
		data.localID = localID;
		data.hasLocalID = true;
		return data;
	}

private:
	ContactDistribution m_data;
};
} // namespace Kademlia
#endif
