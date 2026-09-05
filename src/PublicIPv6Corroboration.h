//
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

#ifndef PUBLICIPV6CORROBORATION_H
#define PUBLICIPV6CORROBORATION_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

/**
 * What a peer claims our own public IPv6 address is, believed only once
 * several unrelated peers say the same thing.
 *
 * The claim arrives in the CT_MOD_YOUR_IP (0xAD) hello tag. The two reference
 * implementations disagree about it, and the disagreement is the whole reason
 * this class exists:
 *
 *  - eMuleAI accepts the tag's integer form and sets its own public IPv4 from
 *    it. One peer, unverified, decides what that client believes its own
 *    address to be -- and a client that is wrong about its own address is
 *    wrong about whether it is firewalled, which address it publishes to Kad
 *    and where it asks to be called back.
 *  - emule-qt refuses the integer form outright and takes only the 128-bit
 *    hash form, then corroborates it across peers before believing it.
 *
 * aMule follows emule-qt. BaseClient.cpp reads only the hash form and routes
 * it here.
 *
 * The key is the address the packet was observed arriving from, never the
 * sender's self-declared user hash: a hash costs nothing to invent, so a
 * single host could otherwise manufacture as many "distinct" corroborating
 * peers as the threshold demands. A routable source address cannot be
 * invented for free, because a reply has to come back through it.
 *
 * Nothing consumes the result yet. This is recognition only, like the rest of
 * this change: aMule has no IPv6 stack, so the corroborated address is
 * recorded for the dual-stack change and read by nothing else.
 */

//! How many distinct observed source addresses have to agree on the same
//! value before it is believed.
//!
//! Two would be wrong: two source addresses is one dual-homed host, one host
//! that reconnected from a new lease, or one attacker holding a second
//! socket -- none of which is a second opinion. Three is the smallest count
//! that forces a claimant to hold addresses it does not control alone, and it
//! is still reachable in an ordinary session, where a handful of vendor peers
//! connect over its lifetime and the tracker never expires an entry.
//!
//! It is a floor, not a proof: three addresses under one operator still agree
//! with each other. That is acceptable while nothing acts on the result. A
//! change that does act on it -- publishing this address, or deciding
//! firewalled state from it -- needs more than a count here, because the
//! addresses would also have to be shown to be unrelated.
constexpr std::size_t PUBLIC_IPV6_CORROBORATION_THRESHOLD = 3;

//! How many differing claimed values are tracked at once.
//!
//! Bounded on purpose: the input is attacker-controlled, and an unbounded map
//! keyed on a value a peer chooses is a peer-driven allocation. Beyond this
//! many distinct claims nothing is being corroborated anyway -- that is a
//! peer population disagreeing, not a quorum forming -- so further new values
//! are dropped rather than making room by evicting a candidate that may be
//! the honest one.
constexpr std::size_t PUBLIC_IPV6_CORROBORATION_MAX_CANDIDATES = 8;

class CPublicIPv6Corroboration
{
public:
	//! A 128-bit address, big-endian, as it travels in the tag.
	typedef std::array<std::uint8_t, 16> Address;

	CPublicIPv6Corroboration() = default;

	/**
	 * Record one peer's claim.
	 *
	 * @param observedFrom  the IPv4 address the hello was actually seen
	 *                      arriving from, host order. Zero is ignored: with
	 *                      no observed address there is nothing to key on,
	 *                      and an unkeyed claim would let one peer supply
	 *                      the whole quorum by itself.
	 * @param claimed  16 bytes, big-endian. May be NULL, which is ignored.
	 * @return whether some value is corroborated now. A repeat from an
	 *         address already counted for that value changes nothing.
	 */
	bool AddClaim(std::uint32_t observedFrom, const std::uint8_t *claimed)
	{
		if (observedFrom == 0 || claimed == nullptr) {
			return IsCorroborated();
		}

		const Address value = ToAddress(claimed);
		Candidate *candidate = Find(value);
		if (candidate == nullptr) {
			if (m_candidates.size() >= PUBLIC_IPV6_CORROBORATION_MAX_CANDIDATES) {
				return IsCorroborated();
			}
			// emplace_back() rather than push_back(Candidate()), which
			// clang-tidy flags. Its return value is not used: it only
			// returns a reference from C++17, and the unit-test targets
			// take clang's default of C++14.
			m_candidates.emplace_back();
			candidate = &m_candidates.back();
			candidate->value = value;
		}

		if (!candidate->HasObserver(observedFrom)) {
			// Stops growing at the threshold: past it the count answers
			// the only question asked of it, and the extra addresses
			// would just be memory a peer can ask us to spend.
			if (candidate->observers.size() < PUBLIC_IPV6_CORROBORATION_THRESHOLD) {
				candidate->observers.push_back(observedFrom);
			}
		}

		return IsCorroborated();
	}

	//! True once some value reached the threshold.
	bool IsCorroborated() const { return Corroborated() != nullptr; }

	//! The corroborated address, or NULL while none is. 16 bytes.
	const std::uint8_t *CorroboratedAddress() const
	{
		const Candidate *candidate = Corroborated();
		return candidate == nullptr ? nullptr : candidate->value.data();
	}

	//! How many distinct observed addresses have claimed this value. For
	//! tests and diagnostics; nothing should gate on it instead of
	//! IsCorroborated(), which owns the threshold.
	std::size_t DistinctObserversFor(const std::uint8_t *claimed) const
	{
		if (claimed == nullptr) {
			return 0;
		}
		const Candidate *candidate = Find(ToAddress(claimed));
		return candidate == nullptr ? 0 : candidate->observers.size();
	}

	//! How many differing values are being tracked.
	std::size_t CandidateCount() const { return m_candidates.size(); }

	void Reset() { m_candidates.clear(); }

private:
	//! The 16 bytes at @a claimed, which is never NULL here.
	static Address ToAddress(const std::uint8_t *claimed)
	{
		Address value = {};
		std::memcpy(value.data(), claimed, value.size());
		return value;
	}

	struct Candidate
	{
		Address value = {};
		std::vector<std::uint32_t> observers;

		bool HasObserver(std::uint32_t address) const
		{
			for (const auto &observer : observers) {
				if (observer == address) {
					return true;
				}
			}
			return false;
		}
	};

	Candidate *Find(const Address &value)
	{
		for (auto &candidate : m_candidates) {
			if (candidate.value == value) {
				return &candidate;
			}
		}
		return nullptr;
	}

	const Candidate *Find(const Address &value) const
	{
		for (const auto &candidate : m_candidates) {
			if (candidate.value == value) {
				return &candidate;
			}
		}
		return nullptr;
	}

	const Candidate *Corroborated() const
	{
		for (const auto &candidate : m_candidates) {
			if (candidate.observers.size() >= PUBLIC_IPV6_CORROBORATION_THRESHOLD) {
				return &candidate;
			}
		}
		return nullptr;
	}

	std::vector<Candidate> m_candidates;
};

/**
 * The one tracker the hello path feeds.
 *
 * A function-local static rather than a member of the app class: nothing
 * consumes the corroborated address yet, and a recognition-only change should
 * not reach into the app hierarchy to park state no caller reads. It moves to
 * wherever its consumer lives when one exists.
 */
inline CPublicIPv6Corroboration &ObservedPublicIPv6()
{
	static CPublicIPv6Corroboration instance;
	return instance;
}

#endif // PUBLICIPV6CORROBORATION_H
