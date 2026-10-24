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

#ifndef UPLOADQUEUEADDRESSPOLICY_H
#define UPLOADQUEUEADDRESSPOLICY_H

#include "PeerAddressing.h"

namespace UploadQueueAddressPolicy
{
/**
 * Whether an address can take part in a match at all.
 *
 * Absent and unspecified addresses name nobody, so two of them are not the same peer. Hoisting this
 * out of the loop is also what keeps a requester that can never match from walking the queue.
 */
inline bool IsMatchable(const CNetworkAddress &address) noexcept
{
	return PeerAddressing::IsIndexable(address);
}

/**
 * Match a UDP requester to a queued peer without narrowing native IPv6 to zero.
 *
 * Address only. The caller that needs the port dimension as well goes through
 * PeerAddressing::MatchesUdpSource(), whose zero-port refusal this must not silently duplicate.
 */
inline bool Matches(const CNetworkAddress &requester, const CNetworkAddress &peer) noexcept
{
	return IsMatchable(requester) && IsMatchable(peer) &&
	       PeerAddressing::IndexKey(requester) == PeerAddressing::IndexKey(peer);
}

/** Match upload-capacity accounting scopes without widening peer identity. */
inline bool MatchesRateLimitScope(const CNetworkAddress &first, const CNetworkAddress &second) noexcept
{
	return IsMatchable(first) && IsMatchable(second) &&
	       PeerAddressing::RateLimitScope(first) == PeerAddressing::RateLimitScope(second);
}

/** Upload slots one accounting scope may hold while other clients can use the slot. */
inline constexpr unsigned MaxUploadSlotsPerScope = 2;

/**
 * Upload slots one accounting scope may hold even when nobody else can use them.
 *
 * Above MaxUploadSlotsPerScope a scope gets spare capacity only, but with unlimited upload a fast
 * line nearly always has spare slots (up to 250), and kicking runs only once all of them are taken.
 * Generous, so that many users behind one CGNAT IPv4 are still served at once.
 */
inline constexpr unsigned HardMaxUploadSlotsPerScope = 10;

/**
 * Whether a peer at `candidate` may take another upload slot, given the peers already uploading.
 *
 * The waiting-queue cap alone does not bound slots: each queued client that gets a slot leaves the
 * queue, so one host rotating user hashes over several ports can refill it and collect slot after
 * slot. Like that cap, an address without a host identity is outside this one.
 */
template <typename Range, typename AddressOf>
bool HasUploadSlotRoom(const CNetworkAddress &candidate,
	const Range &uploading,
	AddressOf addressOf,
	unsigned limit = MaxUploadSlotsPerScope)
{
	if (!IsMatchable(candidate)) {
		return true;
	}
	// One value for the whole walk, as in the waiting-queue cap.
	const CNetworkAddress scope = PeerAddressing::RateLimitScope(candidate);
	unsigned held = 0;
	for (const auto &peer : uploading) {
		const CNetworkAddress &address = addressOf(peer);
		if (IsMatchable(address) && PeerAddressing::RateLimitScope(address) == scope &&
			++held >= limit) {
			return false;
		}
	}
	return true;
}
} // namespace UploadQueueAddressPolicy

#endif // UPLOADQUEUEADDRESSPOLICY_H
