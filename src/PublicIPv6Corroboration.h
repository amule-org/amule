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
 * What a peer claims our own public IPv6 address is, believed only when it is
 * an address we actually hold and several unrelated peers agree on it.
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
 *    hash form, then filters and corroborates it before believing it.
 *
 * aMule follows emule-qt. BaseClient.cpp reads only the hash form and routes
 * it here.
 *
 * Two independent gates stand between a peer's word and an adopted address,
 * and they do different jobs:
 *
 *  - The locally-assigned filter is the security control. Peers are
 *    unauthenticated, so corroboration on its own must never be able to make
 *    us advertise a foreign address. Requiring the claimed value to be one we
 *    actually hold bounds the damage to "picks the wrong one of our own
 *    addresses", which is the only question left.
 *  - Corroboration is the tie-breaker for that remaining question. A
 *    multi-homed host holds several global addresses and only some of them are
 *    the one the outside world sees; asking several peers which one they see
 *    is how the right one is picked.
 *
 * The quorum is keyed on the address the packet was observed arriving from,
 * never the sender's self-declared user hash: a hash costs nothing to invent,
 * so a single host could otherwise manufacture as many "distinct"
 * corroborating peers as the threshold demands. A routable source address
 * cannot be invented for free, because a reply has to come back through it.
 *
 * Nothing consumes the result yet. This is recognition only, like the rest of
 * this change: aMule has no IPv6 stack, so the corroborated address is
 * recorded for the dual-stack change and read by nothing else.
 *
 * Not thread-safe, and does not need to be: hellos and the interface refresh
 * both run on the main thread.
 */

//! How many distinct observed source addresses have to agree on the same
//! value before it is believed.
//!
//! Two would be wrong: two source addresses is one dual-homed host, one host
//! that reconnected from a new lease, or one attacker holding a second
//! socket -- none of which is a second opinion. Three is the smallest count
//! that forces a claimant to hold addresses it does not control alone, and it
//! is still reachable in an ordinary session, where a handful of vendor peers
//! connect over its lifetime.
//!
//! It is a floor, not a proof: three addresses under one operator still agree
//! with each other. That is what the locally-assigned filter is for -- three
//! colluding peers can still only steer us between addresses we hold.
constexpr std::size_t PUBLIC_IPV6_CORROBORATION_THRESHOLD = 3;

//! How long one observer's claim keeps counting.
//!
//! Without a window, "three distinct peers agree" means "three peers said so
//! at some point since the process started", so a laptop that moved to another
//! network hours ago still carries the votes that elected the address it had
//! there, and no amount of fresh disagreement can unseat them. Votes that age
//! out are what makes re-election possible at all.
//!
//! Thirty minutes because hellos from distinct peers arrive sporadically --
//! minutes apart on a quiet client -- so a window of a few minutes would
//! expire the first vote before the third arrived and the quorum would never
//! form. It is an upper bound on how long a stale address can survive
//! unchallenged, and the interface refresh already covers the case where the
//! address simply went away.
constexpr std::uint64_t PUBLIC_IPV6_CORROBORATION_WINDOW_MS = 30ull * 60ull * 1000ull;

class CPublicIPv6Corroboration
{
public:
	//! A 128-bit address, big-endian, as it travels in the tag.
	typedef std::array<std::uint8_t, 16> Address;

	CPublicIPv6Corroboration() = default;

	/**
	 * Publish the addresses currently assigned to a local interface.
	 *
	 * This set gates every claim, so it has to be published before any claim
	 * can be believed -- with an empty set nothing is ever adopted, which is
	 * the safe direction to fail in.
	 *
	 * It must be a refreshed cache rather than a startup snapshot: a prefix
	 * renumber or a privacy-address rotation would otherwise go unnoticed for
	 * as long as the session lasts, and we would keep an address we no longer
	 * hold. It must equally not be re-scanned per claim -- that would put an
	 * interface walk on a path any peer can drive.
	 *
	 * The second half of the refresh is what makes the cache safe: an already
	 * adopted address is re-checked here and dropped when it is no longer
	 * ours. Without that, a corroborated address outlives the prefix it came
	 * from.
	 *
	 * @param local   addresses assigned to a local interface. Anything that is
	 *                not global unicast is discarded, so callers can pass a
	 *                whole interface enumeration.
	 * @param nowMs   a millisecond tick count.
	 */
	void SetLocalAddresses(const std::vector<Address> &local, std::uint64_t nowMs)
	{
		m_local.clear();
		for (const auto &address : local) {
			// IsLocallyAssigned() reads the set being built, so this also
			// collapses the duplicates an interface enumeration produces when
			// the same address is reported on more than one node.
			if (IsGlobalUnicast(address) && !IsLocallyAssigned(address)) {
				m_local.push_back(address);
			}
		}

		// A tally for an address we no longer hold is not evidence about the
		// address we hold now, and keeping it would let the old value stay
		// elected. Dropping it here is how a renumber gets noticed.
		std::vector<Candidate> kept;
		for (const auto &candidate : m_candidates) {
			if (IsLocallyAssigned(candidate.value)) {
				kept.push_back(candidate);
			}
		}
		m_candidates.swap(kept);

		Expire(nowMs);
		Elect();
	}

	/**
	 * Record one peer's claim.
	 *
	 * @param observedFrom  the IPv4 address the hello was actually seen
	 *                      arriving from, host order. Zero is ignored: with
	 *                      no observed address there is nothing to key on,
	 *                      and an unkeyed claim would let one peer supply
	 *                      the whole quorum by itself.
	 * @param claimed  16 bytes, big-endian. May be NULL, which is ignored.
	 * @param nowMs    a millisecond tick count. Passed in rather than read
	 *                 here so the window is testable at all.
	 * @return whether some value is corroborated now. A repeat from an
	 *         address already counted for that value refreshes that vote and
	 *         changes nothing else.
	 */
	bool AddClaim(std::uint32_t observedFrom, const std::uint8_t *claimed, std::uint64_t nowMs)
	{
		// Aging runs before the claim is judged, so a claim arriving after a
		// long quiet spell sees an empty tally rather than votes that should
		// already have expired.
		Expire(nowMs);
		Elect();

		if (observedFrom == 0 || claimed == nullptr) {
			return IsCorroborated();
		}

		const Address value = ToAddress(claimed);
		// Both checks, not just the second: the locally-assigned set is
		// already filtered to global unicast, but the address family test is
		// the invariant this class promises and it must not depend on how a
		// caller happened to populate that set.
		if (!IsGlobalUnicast(value) || !IsLocallyAssigned(value)) {
			// Deliberately no state whatsoever for a rejected value. A
			// rejection that allocated something would let a peer spend our
			// memory on values it invented, which is exactly what the filter
			// exists to prevent.
			return IsCorroborated();
		}

		Candidate *candidate = Find(value);
		if (candidate == nullptr) {
			// emplace_back() rather than push_back(Candidate()), which
			// clang-tidy flags. Its return value is not used: it only
			// returns a reference from C++17, and the unit-test targets
			// take clang's default of C++14.
			m_candidates.emplace_back();
			candidate = &m_candidates.back();
			candidate->value = value;
		}

		Observer *observer = candidate->Find(observedFrom);
		if (observer != nullptr) {
			// A peer that is still saying it keeps its vote alive. Without
			// this the window would expire long-lived peers that never
			// stopped agreeing.
			observer->lastSeenMs = nowMs;
		} else if (candidate->observers.size() < PUBLIC_IPV6_CORROBORATION_THRESHOLD) {
			// Stops growing at the threshold: past it the count answers
			// the only question asked of it, and the extra addresses
			// would just be memory a peer can ask us to spend.
			Observer fresh;
			fresh.address = observedFrom;
			fresh.lastSeenMs = nowMs;
			candidate->observers.push_back(fresh);
		}

		Elect();
		return IsCorroborated();
	}

	//! True once some value reached the threshold and still holds it.
	bool IsCorroborated() const { return m_adopted; }

	//! The corroborated address, or NULL while none is. 16 bytes.
	const std::uint8_t *CorroboratedAddress() const
	{
		return m_adopted ? m_adoptedValue.data() : nullptr;
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

	//! How many local addresses the filter is currently gating on.
	std::size_t LocalAddressCount() const { return m_local.size(); }

	//! Forget every claim. The local address set survives: it comes from this
	//! machine rather than from peers, and clearing it would silently disable
	//! the filter until the next refresh.
	void Reset()
	{
		m_candidates.clear();
		m_adopted = false;
	}

	//! Global unicast, 2000::/3. Everything a peer could otherwise reflect at
	//! us -- the unspecified address, loopback, link-local, unique-local,
	//! multicast, IPv4-mapped -- falls outside it, and none of those is an
	//! address the outside world could have seen us arrive from.
	static bool IsGlobalUnicast(const Address &value) { return (value[0] & 0xE0) == 0x20; }

private:
	//! The 16 bytes at @a claimed, which is never NULL here.
	static Address ToAddress(const std::uint8_t *claimed)
	{
		Address value = {};
		std::memcpy(value.data(), claimed, value.size());
		return value;
	}

	struct Observer
	{
		std::uint32_t address = 0;
		std::uint64_t lastSeenMs = 0;
	};

	struct Candidate
	{
		Address value = {};
		std::vector<Observer> observers;

		Observer *Find(std::uint32_t address)
		{
			for (auto &observer : observers) {
				if (observer.address == address) {
					return &observer;
				}
			}
			return nullptr;
		}
	};

	bool IsLocallyAssigned(const Address &value) const
	{
		for (const auto &address : m_local) {
			if (address == value) {
				return true;
			}
		}
		return false;
	}

	//! Drop votes older than the window, and candidates left with none.
	void Expire(std::uint64_t nowMs)
	{
		std::vector<Candidate> kept;
		for (auto &candidate : m_candidates) {
			Candidate fresh;
			fresh.value = candidate.value;
			for (const auto &observer : candidate.observers) {
				// A tick count that appears to move backwards expires the
				// vote rather than keeping it. The tick source is uptime and
				// does not go backwards, so this is a defence against a
				// caller mixing clocks -- and holding a vote we cannot date
				// is the one outcome worth avoiding, while re-gathering one
				// costs nothing but time.
				if (nowMs >= observer.lastSeenMs &&
					nowMs - observer.lastSeenMs <= PUBLIC_IPV6_CORROBORATION_WINDOW_MS) {
					fresh.observers.push_back(observer);
				}
			}
			if (!fresh.observers.empty()) {
				kept.push_back(fresh);
			}
		}
		m_candidates.swap(kept);
	}

	/**
	 * Re-run the election.
	 *
	 * An adopted value keeps its seat while it still holds a quorum, so a
	 * second value reaching the threshold does not flip us back and forth
	 * between two addresses we equally hold. It loses the seat the moment its
	 * quorum lapses -- votes aged out, or the address no longer assigned --
	 * and only then may another value take it.
	 */
	void Elect()
	{
		if (m_adopted) {
			const Candidate *candidate = Find(m_adoptedValue);
			if (candidate != nullptr &&
				candidate->observers.size() >= PUBLIC_IPV6_CORROBORATION_THRESHOLD) {
				return;
			}
			m_adopted = false;
		}

		for (const auto &candidate : m_candidates) {
			if (candidate.observers.size() >= PUBLIC_IPV6_CORROBORATION_THRESHOLD) {
				m_adoptedValue = candidate.value;
				m_adopted = true;
				return;
			}
		}
	}

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

	//! The addresses assigned to a local interface, global unicast only.
	//!
	//! There is no cap on this or on the candidate list, and there deliberately
	//! is none. An earlier revision capped the number of tracked values because
	//! the input was attacker-chosen; with the filter in front, a candidate can
	//! only ever be one of the addresses in this set, which this machine's own
	//! interfaces bound -- a handful, even with privacy addresses rotating. A
	//! cap would now buy nothing and cost something real: occupying a slot
	//! costs one claim while corroborating a value costs three, so refusing
	//! entries past a limit would make permanent denial the cheaper attack.
	std::vector<Address> m_local;

	std::vector<Candidate> m_candidates;

	Address m_adoptedValue = {};
	bool m_adopted = false;
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

/**
 * Re-read this machine's interfaces and publish the result to
 * ObservedPublicIPv6().
 *
 * Called at startup, on every server connect, and on a periodic tick. The
 * periodic one is not redundant: without it the only triggers are startup and
 * a server connect, so a prefix renumber or a privacy-address rotation goes
 * unnoticed for as long as the session stays connected.
 *
 * Declared here, beside what it refreshes, but defined in amule.cpp: the
 * interface enumeration pulls in wx and the platform socket headers, and this
 * header stays clear of both so the tracker can be unit-tested on its own.
 */
void RefreshLocalPublicIPv6Addresses();

#endif // PUBLICIPV6CORROBORATION_H
