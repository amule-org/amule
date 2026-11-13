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

#include <muleunit/test.h>

#include "ClientMatchPolicy.h"

using namespace muleunit;

DECLARE_SIMPLE(ClientMatchPolicy)

namespace
{
// A NAT neighbour: both hashes from a hello, and they differ.
SClientMatchFacts Neighbour()
{
	SClientMatchFacts facts;
	facts.incomingHasHash = true;
	facts.candidateHasHash = true;
	facts.incomingHashFromHello = true;
	facts.candidateHashFromHello = true;
	facts.sameHash = false;
	return facts;
}
} // namespace

TEST(ClientMatchPolicy, AnotherHelloHashIsAnotherPeer)
{
	ASSERT_TRUE(IsOtherPeerAtSameEndpoint(Neighbour()));
}

TEST(ClientMatchPolicy, TheSameHashIsTheSamePeer)
{
	SClientMatchFacts facts = Neighbour();
	facts.sameHash = true;
	ASSERT_FALSE(IsOtherPeerAtSameEndpoint(facts));
}

TEST(ClientMatchPolicy, AHashFromASourceListMayBeStale)
{
	SClientMatchFacts facts = Neighbour();
	facts.candidateHashFromHello = false;
	ASSERT_FALSE(IsOtherPeerAtSameEndpoint(facts));
}

TEST(ClientMatchPolicy, ASourceFromAListIsMergedAsBefore)
{
	// The hash of a source added from a source list or Kad may be stale.
	SClientMatchFacts facts = Neighbour();
	facts.incomingHashFromHello = false;
	ASSERT_FALSE(IsOtherPeerAtSameEndpoint(facts));
}

TEST(ClientMatchPolicy, OnlyACallbackLookupSkipsAnAnsweredRoute)
{
	ASSERT_TRUE(SkipsClientAtEndpoint(true, true));
	// The buddy, firewall-check and dial-back lookups still see that client.
	ASSERT_FALSE(SkipsClientAtEndpoint(false, true));
	ASSERT_FALSE(SkipsClientAtEndpoint(true, false));
	ASSERT_FALSE(SkipsClientAtEndpoint(false, false));
}

TEST(ClientMatchPolicy, WithoutBothHashesTheEndpointDecides)
{
	SClientMatchFacts facts = Neighbour();
	facts.incomingHasHash = false;
	ASSERT_FALSE(IsOtherPeerAtSameEndpoint(facts));
	facts = Neighbour();
	facts.candidateHasHash = false;
	ASSERT_FALSE(IsOtherPeerAtSameEndpoint(facts));
}
