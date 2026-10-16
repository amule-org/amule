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

#include "NetworkAddress.h" // Needed for CNetworkAddress

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

/**
 * What peers claim our own public IPv6 address is: a vote when it is an address we hold, evidence
 * against our address when it is in a /64 we do not hold.
 *
 * The claim arrives in the CT_MOD_YOUR_IP (0xAD) hello tag. The two reference implementations
 * disagree about it, and the disagreement is the whole reason this class exists:
 *
 *  - eMuleAI accepts the tag's integer form and sets its own public IPv4 from it. One peer,
 *    unverified, decides what that client believes its own address to be -- and a client wrong
 *    about its own address is wrong about whether it is firewalled, which address it publishes
 *    to Kad and where it asks to be called back.
 *  - emule-qt refuses the integer form outright and takes only the 128-bit hash form, then
 *    filters and corroborates it before believing it.
 *
 * aMule follows emule-qt. BaseClient.cpp reads only the hash form and routes it here.
 *
 * Two independent gates stand between a peer's word and a corroborated prefix, and they do
 * different jobs:
 *
 *  - The locally-assigned filter is the security control. Peers are unauthenticated, so
 *    corroboration on its own must never be able to make us advertise a foreign address.
 *    Requiring the claimed value to be one we actually hold bounds the damage to "confirms the
 *    wrong one of our own prefixes".
 *  - Corroboration answers whether the outside world sees that prefix at all. Behind IPv6 NAT
 *    (Docker's default) a host holds a global address nobody can reach; peers then see another
 *    prefix, and ours never gathers votes.
 *
 * Claims are grouped by the /64 of the claimed address: outbound connections leave from a
 * temporary address (RFC 6724 rule 7) while the stable one is what we would advertise, and both
 * sit in one delegated /64.
 *
 * A claim in a /64 we do not hold is never adopted, but it is not noise either: behind NAT66 or
 * NPTv6 that is exactly what every peer reports. Enough unrelated observers agreeing on one such
 * prefix, while nobody currently confirms ours, vetoes advertising ours -- see IsPrefixVetoed().
 *
 * The quorum is keyed on the address the packet was observed arriving from, never the sender's
 * self-declared user hash: a hash costs nothing to invent, so a single host could otherwise
 * manufacture as many "distinct" corroborating peers as the threshold demands. A routable source
 * address cannot be invented for free, because a reply has to come back through it -- but a home
 * subscriber is usually delegated a /56 or a /48, so each observer counts once per /48.
 *
 * Not thread-safe, and does not need to be: hellos and the interface refresh both run on the main
 * thread.
 */

//! How many distinct observer /48s have to agree on a prefix before it is believed.
//!
//! Two would be wrong: two observers is one dual-homed host, one host that reconnected from a new
//! lease, or one attacker holding a second socket -- none of which is a second opinion. Three is
//! the smallest count that forces a claimant to hold addresses it does not control alone, and is
//! still reachable in an ordinary session.
//!
//! It is a floor, not a proof: three prefixes under one operator still agree with each other. That
//! is what the locally-assigned filter is for -- colluding peers can still only confirm a prefix
//! we hold.
constexpr std::size_t PUBLIC_IPV6_CORROBORATION_THRESHOLD = 3;

/**
 * How many distinct observers are remembered per claimed /64.
 *
 * Deliberately above the threshold. Recording only as many votes as the quorum
 * needs leaves no margin: when one observer falls out of the window the count
 * drops below the quorum and the prefix is no longer corroborated, even though
 * other peers had been agreeing the whole time and were turned away at the cap.
 * The surplus is the redundancy that makes an expiry survivable, and it is what
 * a peer cannot spend, since the bound still holds.
 */
constexpr std::size_t PUBLIC_IPV6_CORROBORATION_MAX_OBSERVERS = 8;

//! How long one observer's claim keeps counting.
//!
//! Without a window, "three distinct peers agree" means "three peers said so at some point since
//! the process started", so a laptop that moved networks hours ago still carries the votes that
//! corroborated the prefix it had there, and no amount of fresh disagreement can unseat them.
//!
//! Thirty minutes because hellos from distinct peers arrive sporadically, minutes apart on a quiet
//! client, so a window of a few minutes would expire the first vote before the third arrived and
//! the quorum would never form. It bounds how long a stale address can survive unchallenged; the
//! interface refresh covers the case where the address simply went away.
constexpr std::uint64_t PUBLIC_IPV6_CORROBORATION_WINDOW_MS = 30ull * 60ull * 1000ull;

//! How many observers' contradicting claims are remembered, one claim per observer /48.
//!
//! Unlike a vote, a contradiction names a prefix the peer chose, so it cannot be bounded by what
//! this machine holds. Keeping only each observer's latest claim stops one peer from spending our
//! memory on prefixes it invents; the cap bounds the observers. When full, the observer heard from
//! least recently makes room, so fresh evidence is never refused because stale evidence came first.
//! Evicting the evidence behind a real veto then takes more distinct /48s than raising a veto does.
constexpr std::size_t PUBLIC_IPV6_CONTRADICTION_MAX_OBSERVERS = 16;

class CPublicIPv6Corroboration
{
public:
	//! A 128-bit address, big-endian, as it travels in the tag.
	typedef std::array<std::uint8_t, 16> Address;

	CPublicIPv6Corroboration() = default;

	/**
	 * Publish the addresses currently assigned to a local interface.
	 *
	 * This set gates every claim, so it has to be published before any claim can be believed --
	 * with an empty set nothing is ever corroborated, which is the safe direction to fail in.
	 *
	 * It must be a refreshed cache rather than a startup snapshot: a prefix renumber or a
	 * privacy-address rotation would otherwise go unnoticed for as long as the session lasts,
	 * and we would keep an address we no longer hold. It must equally not be re-scanned per
	 * claim -- that would put an interface walk on a path any peer can drive.
	 *
	 * The second half of the refresh is what makes the cache safe: a tallied /64 is re-checked
	 * here and dropped once none of our addresses is in it. Without that, a corroborated prefix
	 * outlives the renumber.
	 *
	 * @param local addresses assigned to a local interface. Anything not global unicast is
	 * discarded, so callers can pass a whole interface enumeration.
	 * @param nowMs a millisecond tick count.
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

		// A rotated temporary address leaves its /64 in place, so its votes stay; a
		// prefix we no longer hold anything in is not evidence about the one we hold now.
		std::vector<Candidate> kept;
		for (const auto &candidate : m_candidates) {
			if (HoldsAddressIn(candidate.prefix)) {
				kept.push_back(candidate);
			}
		}
		m_candidates.swap(kept);

		// A prefix we now hold is ours, not evidence against ours.
		m_contradictions.erase(std::remove_if(m_contradictions.begin(),
					       m_contradictions.end(),
					       [this](const Contradiction &contradiction) {
						       return HoldsAddressIn(contradiction.prefix);
					       }),
			m_contradictions.end());

		Expire(nowMs);
	}

	/**
	 * Record one peer's claim.
	 *
	 * @param observedFrom the address the hello was actually seen arriving from. Only a globally
	 * routable IPv6 address counts, as one vote for its whole /48.
	 * @param claimed 16 bytes, big-endian. May be nullptr, which is ignored.
	 * @param nowMs a millisecond tick count. Passed in rather than read here so the window is
	 * testable at all.
	 * @return true exactly when this claim brings the claimed /64 to the threshold; false
	 * otherwise, including repeats once it is reached and every contradiction. A repeat from an
	 * observer already counted refreshes that vote and changes nothing else.
	 */
	bool AddClaim(const CNetworkAddress &observedFrom, const std::uint8_t *claimed, std::uint64_t nowMs)
	{
		// Aging runs before the claim is judged, so a claim arriving after a long quiet
		// spell sees an empty tally rather than votes that should already have expired.
		Expire(nowMs);

		// An IPv4 peer cannot see our IPv6 address, and once we advertise one, a peer echoing
		// our own advertisement back must not count as a witness. Only a peer that reached us
		// over global IPv6 saw where we arrive from.
		if (claimed == nullptr || !observedFrom.IsGloballyRoutableIPv6()) {
			return false;
		}

		const Address value = ToAddress(claimed);
		// Tested on the value itself, not left to the locally-assigned set: that set is
		// already filtered to global unicast, but the address family test is the invariant
		// this class promises and must not depend on how a caller populated that set.
		if (!IsGlobalUnicast(value)) {
			return false;
		}

		// A host on our own LAN prefix, a container on the same bridge, or a neighbouring
		// subnet of our own delegation sees our address without telling us how the outside
		// world sees it.
		const CNetworkAddress scope = ObserverScope(observedFrom);
		if (scope == ObserverScope(CNetworkAddress::IPv6FromOctets(value))) {
			return false;
		}

		const Address prefix = PrefixOf(value);
		if (!IsLocallyAssigned(value)) {
			// Another address in a /64 we hold is a rotated or invented suffix, not a
			// different view of where we arrive from. It leaves no state at all: a
			// rejection that allocated something would let a peer spend our memory on
			// values it invented.
			if (!HoldsAddressIn(prefix)) {
				AddContradiction(scope, prefix, nowMs);
			}
			return false;
		}
		Candidate *candidate = Find(prefix);
		if (candidate == nullptr) {
			// emplace_back() rather than push_back(Candidate()), which
			// clang-tidy flags. It returns a reference from C++17.
			candidate = &m_candidates.emplace_back();
			candidate->prefix = prefix;
		}

		const std::size_t before = candidate->observers.size();
		Observer *observer = candidate->Find(scope);
		if (observer != nullptr) {
			// A peer that is still saying it keeps its vote alive. Without this the
			// window would expire long-lived peers that never stopped agreeing.
			observer->lastSeenMs = nowMs;
		} else if (candidate->observers.size() < PUBLIC_IPV6_CORROBORATION_MAX_OBSERVERS) {
			// Bounded, but above the quorum. Stopping at the threshold would mean
			// the first observer to age out un-corroborates a prefix that other
			// live peers still agree on, because their votes were refused at the
			// cap and there is nothing left to fall back on.
			Observer fresh;
			fresh.scope = scope;
			fresh.lastSeenMs = nowMs;
			candidate->observers.push_back(fresh);
		}
		const std::size_t after = candidate->observers.size();
		if (after >= PUBLIC_IPV6_CORROBORATION_THRESHOLD) {
			candidate->verdict = true;
		}
		return before < PUBLIC_IPV6_CORROBORATION_THRESHOLD &&
		       after >= PUBLIC_IPV6_CORROBORATION_THRESHOLD;
	}

	/**
	 * Whether @a address is one of ours and its /64 is corroborated: a quorum of observers is
	 * inside the window at @a nowMs, or one was once and nothing has ended that verdict since.
	 *
	 * The verdict outlives the votes because nodes meet IPv6 peers only now and then, and the
	 * address did not become less reachable when they left. It ends with a veto, when we stop
	 * holding the /64, and on ForgetVerdicts().
	 */
	bool IsPrefixCorroborated(const CNetworkAddress &address, std::uint64_t nowMs) const
	{
		if (!address.IsIPv6() || !IsLocallyAssigned(address.GetOctets())) {
			return false;
		}
		const Candidate *candidate = Find(PrefixOf(address.GetOctets()));
		if (candidate == nullptr) {
			return false;
		}
		if (LiveVotes(*candidate, nowMs) >= PUBLIC_IPV6_CORROBORATION_THRESHOLD) {
			return true;
		}
		// Checked here as well as on expiry: a vote can lapse under a live veto between two
		// mutations, and the verdict must not read as standing in that gap.
		return candidate->verdict && !IsPrefixVetoed(address, nowMs);
	}

	/**
	 * Whether peers contradict @a address: a /64 we do not hold has a quorum of observers inside
	 * the window at @a nowMs, and nobody inside the window confirms the /64 of @a address.
	 *
	 * Both halves are needed. The quorum keeps one peer from switching our advertising off; the
	 * live-vote condition means a set of peers cannot do it either while another peer is reaching
	 * us at the address we advertise.
	 */
	bool IsPrefixVetoed(const CNetworkAddress &address, std::uint64_t nowMs) const
	{
		if (!address.IsIPv6()) {
			return false;
		}
		const Candidate *candidate = Find(PrefixOf(address.GetOctets()));
		if (candidate != nullptr && LiveVotes(*candidate, nowMs) > 0) {
			return false;
		}
		return HasContradictingQuorum(nowMs);
	}

	//! End every recorded verdict, keeping the votes. For when the address we advertise changes:
	//! a verdict formed for one address must not carry over to the next.
	void ForgetVerdicts()
	{
		for (auto &candidate : m_candidates) {
			candidate.verdict = false;
		}
		ExpireVoteless();
	}

	//! How many distinct observers are recorded for the /64 of @a claimed. For tests and
	//! diagnostics; nothing should gate on it instead of IsPrefixCorroborated(), which owns the
	//! threshold and the window.
	std::size_t DistinctObserversFor(const std::uint8_t *claimed) const
	{
		if (claimed == nullptr) {
			return 0;
		}
		const Candidate *candidate = Find(PrefixOf(ToAddress(claimed)));
		return candidate == nullptr ? 0 : candidate->observers.size();
	}

	//! How many /64s are being tracked.
	std::size_t CandidateCount() const { return m_candidates.size(); }

	//! How many observers' contradicting claims are being tracked.
	std::size_t ContradictionCount() const { return m_contradictions.size(); }

	//! How many local addresses the filter is currently gating on.
	std::size_t LocalAddressCount() const { return m_local.size(); }

	//! Forget every claim. The local address set survives: it comes from this machine, not from
	//! peers, and clearing it would disable the filter until the next refresh.
	void Reset()
	{
		m_candidates.clear();
		m_contradictions.clear();
	}

	//! Global unicast, 2000::/3. Everything a peer could otherwise reflect at us -- the
	//! unspecified address, loopback, link-local, unique-local, multicast, IPv4-mapped -- falls
	//! outside it, and none of those is an address the outside world could have seen us arrive
	//! from.
	static bool IsGlobalUnicast(const Address &value) { return (value[0] & 0xE0) == 0x20; }

private:
	//! The 16 bytes at @a claimed, which is never nullptr here.
	static Address ToAddress(const std::uint8_t *claimed)
	{
		Address value = {};
		std::memcpy(value.data(), claimed, value.size());
		return value;
	}

	//! The /48 that @a observer votes for. IsGloballyRoutableIPv6() has already excluded every
	//! prefix that many unrelated hosts share, so no exemption is needed here.
	static CNetworkAddress ObserverScope(const CNetworkAddress &observer)
	{
		return observer.TruncatedToPrefix(48);
	}

	//! @a value with its interface identifier cleared: the /64 it belongs to.
	static Address PrefixOf(const Address &value)
	{
		Address prefix = value;
		std::fill(prefix.begin() + 8, prefix.end(), 0);
		return prefix;
	}

	struct Observer
	{
		CNetworkAddress scope;
		std::uint64_t lastSeenMs = 0;
	};

	struct Candidate
	{
		Address prefix = {};
		std::vector<Observer> observers;
		bool verdict = false; // A quorum formed, and nothing has ended it since

		Observer *Find(const CNetworkAddress &scope)
		{
			for (auto &observer : observers) {
				if (observer.scope == scope) {
					return &observer;
				}
			}
			return nullptr;
		}
	};

	//! One observer's latest claim for a /64 we do not hold.
	struct Contradiction
	{
		CNetworkAddress scope;
		Address prefix = {};
		std::uint64_t lastSeenMs = 0;
	};

	static bool IsLive(std::uint64_t lastSeenMs, std::uint64_t nowMs)
	{
		// A tick that appears to move backwards expires the vote rather than keeping
		// it -- uptime does not go backwards, so this defends against a caller mixing
		// clocks.
		return nowMs >= lastSeenMs && nowMs - lastSeenMs <= PUBLIC_IPV6_CORROBORATION_WINDOW_MS;
	}

	// Counted at read time rather than trusted: nothing expires the tally between claims, so
	// evidence that went quiet would otherwise still read as current.
	static std::size_t LiveVotes(const Candidate &candidate, std::uint64_t nowMs)
	{
		return static_cast<std::size_t>(std::count_if(candidate.observers.begin(),
			candidate.observers.end(),
			[nowMs](const Observer &observer) { return IsLive(observer.lastSeenMs, nowMs); }));
	}

	bool HasContradictingQuorum(std::uint64_t nowMs) const
	{
		for (const auto &contradiction : m_contradictions) {
			const auto agreeing = std::count_if(m_contradictions.begin(),
				m_contradictions.end(),
				[&contradiction, nowMs](const Contradiction &other) {
					return other.prefix == contradiction.prefix &&
					       IsLive(other.lastSeenMs, nowMs);
				});
			if (static_cast<std::size_t>(agreeing) >= PUBLIC_IPV6_CORROBORATION_THRESHOLD) {
				return true;
			}
		}
		return false;
	}

	void AddContradiction(const CNetworkAddress &scope, const Address &prefix, std::uint64_t nowMs)
	{
		auto existing = std::find_if(m_contradictions.begin(),
			m_contradictions.end(),
			[&scope](
				const Contradiction &contradiction) { return contradiction.scope == scope; });
		if (existing == m_contradictions.end()) {
			if (m_contradictions.size() >= PUBLIC_IPV6_CONTRADICTION_MAX_OBSERVERS) {
				existing = std::min_element(m_contradictions.begin(),
					m_contradictions.end(),
					[](const Contradiction &a, const Contradiction &b) {
						return a.lastSeenMs < b.lastSeenMs;
					});
			} else {
				existing = m_contradictions.emplace(m_contradictions.end());
			}
		}
		existing->scope = scope;
		existing->prefix = prefix;
		existing->lastSeenMs = nowMs;

		EndContradictedVerdicts(nowMs);
		ExpireVoteless();
	}

	//! "Until it is contradicted": a verdict standing on no live vote ends under a veto and does
	//! not return when the veto lapses. A live vote blocks the veto, so its verdict stays -- until
	//! that vote lapses, which is why this runs on expiry too and not only when a claim arrives.
	void EndContradictedVerdicts(std::uint64_t nowMs)
	{
		if (!HasContradictingQuorum(nowMs)) {
			return;
		}
		for (auto &candidate : m_candidates) {
			if (LiveVotes(candidate, nowMs) == 0) {
				candidate.verdict = false;
			}
		}
	}

	bool IsLocallyAssigned(const Address &value) const
	{
		for (const auto &address : m_local) {
			if (address == value) {
				return true;
			}
		}
		return false;
	}

	bool HoldsAddressIn(const Address &prefix) const
	{
		for (const auto &address : m_local) {
			if (PrefixOf(address) == prefix) {
				return true;
			}
		}
		return false;
	}

	//! Drop votes and contradictions older than the window, and candidates left with neither a
	//! vote nor a verdict.
	void Expire(std::uint64_t nowMs)
	{
		for (auto &candidate : m_candidates) {
			candidate.observers.erase(std::remove_if(candidate.observers.begin(),
							  candidate.observers.end(),
							  [nowMs](const Observer &observer) {
								  return !IsLive(observer.lastSeenMs, nowMs);
							  }),
				candidate.observers.end());
		}

		m_contradictions.erase(std::remove_if(m_contradictions.begin(),
					       m_contradictions.end(),
					       [nowMs](const Contradiction &contradiction) {
						       return !IsLive(contradiction.lastSeenMs, nowMs);
					       }),
			m_contradictions.end());

		EndContradictedVerdicts(nowMs);
		ExpireVoteless();
	}

	void ExpireVoteless()
	{
		m_candidates.erase(std::remove_if(m_candidates.begin(),
					   m_candidates.end(),
					   [](const Candidate &candidate) {
						   return candidate.observers.empty() && !candidate.verdict;
					   }),
			m_candidates.end());
	}

	Candidate *Find(const Address &prefix)
	{
		for (auto &candidate : m_candidates) {
			if (candidate.prefix == prefix) {
				return &candidate;
			}
		}
		return nullptr;
	}

	const Candidate *Find(const Address &prefix) const
	{
		for (const auto &candidate : m_candidates) {
			if (candidate.prefix == prefix) {
				return &candidate;
			}
		}
		return nullptr;
	}

	//! The addresses assigned to a local interface, global unicast only.
	//!
	//! Deliberately no cap on this or on the candidate list. An earlier revision capped the
	//! tracked values because the input was attacker-chosen; with the filter in front, a
	//! candidate can only be a /64 holding one of the addresses this machine's own interfaces
	//! bound. A cap would now cost something real: occupying a slot costs one claim while
	//! corroborating a prefix costs three, so refusing entries past a limit would make permanent
	//! denial the cheaper attack.
	std::vector<Address> m_local;

	std::vector<Candidate> m_candidates;

	std::vector<Contradiction> m_contradictions;
};

/// The one tracker the hello path feeds and AdvertisedIPv6ForPeers() reads. A function-local static
/// rather than a member of the app class, so this header and its tests stay clear of the app.
inline CPublicIPv6Corroboration &ObservedPublicIPv6()
{
	static CPublicIPv6Corroboration instance;
	return instance;
}

/**
 * Re-read this machine's interfaces and publish the result to ObservedPublicIPv6().
 *
 * Called at startup, on every server connect, and on a periodic tick. The periodic one is not
 * redundant: without it the only triggers are startup and a server connect, so a prefix renumber or
 * a privacy-address rotation goes unnoticed for as long as the session stays connected.
 *
 * Declared here, beside what it refreshes, but defined in amule.cpp: the interface enumeration
 * pulls in the platform socket headers, and this header stays clear of them so the tracker can be
 * unit-tested on its own.
 */
void RefreshLocalPublicIPv6Addresses();

#endif // PUBLICIPV6CORROBORATION_H
