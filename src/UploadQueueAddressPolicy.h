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
} // namespace UploadQueueAddressPolicy

#endif // UPLOADQUEUEADDRESSPOLICY_H
