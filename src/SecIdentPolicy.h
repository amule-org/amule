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

#ifndef SECIDENTPOLICY_H
#define SECIDENTPOLICY_H

#include "NetworkAddress.h"

namespace SecIdent
{
// v2's ChallengeIP is exactly four nonzero IPv4 bytes, never an IPv6 projection.
inline bool PeerIPv4(const CNetworkAddress &address, uint32_t &ip)
{
	ip = 0;
	return address.ToIPv4NetworkOrder(ip) && ip != 0;
}

/**
 * SecIdent v2 signs an IPv4 ChallengeIP that both ends must derive identically, so it is usable
 * only with a peer that has one. PeerIPv4 supplies that prerequisite at the wire boundary.
 *
 * An obligation for whatever adds IPv6 sockets: CLibSocket::GetPeerInt() must keep yielding zero
 * for a peer with no IPv4 endpoint. True now on both paths, the transport one through
 * ToIPv4NetworkOrderOrZero() and the asio one because SetIp() takes an amuleIPV4Address, and
 * widening the second is what dual stack does. Narrow a native IPv6 address to anything non-zero
 * and v2 is selected for a peer that cannot agree on the value, which fails verification with no
 * log line -- the failure this policy exists to prevent.
 */
enum Version
{
	Unavailable = 0,
	V1 = 1,
	V2 = 2
};

inline unsigned SupportedVersions(bool cryptoAvailable, bool hasPeerIPv4)
{
	return cryptoAvailable ? (hasPeerIPv4 ? V1 | V2 : V1) : Unavailable;
}

inline Version SignatureVersion(unsigned peerVersions, bool hasPeerIPv4)
{
	// Preserve the historical v1 preference, even on IPv4 connections.
	if (peerVersions & V1) {
		return V1;
	}
	return (hasPeerIPv4 && (peerVersions & V2)) ? V2 : Unavailable;
}
} // namespace SecIdent

#endif // SECIDENTPOLICY_H
