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

#ifndef CLIENT_MATCH_POLICY_H
#define CLIENT_MATCH_POLICY_H

struct SClientMatchFacts
{
	bool incomingHasHash = false;
	bool candidateHasHash = false;
	// Each hash came from that client's own hello, not from a source list or Kad.
	bool incomingHashFromHello = false;
	bool candidateHashFromHello = false;
	bool sameHash = false;
};

/**
 * Whether a client found by address and port, or by ID, is another peer and must not be merged.
 *
 * Users behind one NAT share the address and often the default port. Merging the second user's
 * hello into the first user's client drops the first one's connection and bans the address. A hash
 * is only a claim, so this only refuses a merge: a peer with a new hash is treated as a new peer.
 * Both hashes must come from a hello. One from a source list or Kad may be stale, so a source
 * added from a list, or a candidate known only from one, is merged as before.
 */
inline bool IsOtherPeerAtSameEndpoint(const SClientMatchFacts &facts)
{
	return facts.incomingHasHash && facts.candidateHasHash && facts.incomingHashFromHello &&
	       facts.candidateHashFromHello && !facts.sameHash;
}

/**
 * Whether a lookup by address and port passes over a client whose route another host answered.
 * Only a callback we are asked to dial does: that client is a NAT neighbour's, not the one at the
 * endpoint now. Every other lookup wants the client at that endpoint whatever its route.
 */
inline bool SkipsClientAtEndpoint(bool forCallback, bool routeAnsweredByOtherHost)
{
	return forCallback && routeAnsweredByOtherHost;
}

#endif // CLIENT_MATCH_POLICY_H
